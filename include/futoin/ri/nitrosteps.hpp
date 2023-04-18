//-----------------------------------------------------------------------------
//   Copyright 2018-2023 FutoIn Project
//   Copyright 2018-2023 Andrey Galkin
//
//   Licensed under the Apache License, Version 2.0 (the "License");
//   you may not use this file except in compliance with the License.
//   You may obtain a copy of the License at
//
//       http://www.apache.org/licenses/LICENSE-2.0
//
//   Unless required by applicable law or agreed to in writing, software
//   distributed under the License is distributed on an "AS IS" BASIS,
//   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//   See the License for the specific language governing permissions and
//   limitations under the License.
//-----------------------------------------------------------------------------
//! @file
//! @brief High Performing Implementation of AsyncSteps (FTN12) for C++
//! @sa https://specs.futoin.org/final/preview/ftn12_async_api.html
//-----------------------------------------------------------------------------

#ifndef FUTOIN_RI_NITROSTEPS_HPP
#define FUTOIN_RI_NITROSTEPS_HPP
//---
#include <futoin/fatalmsg.hpp>
#include <futoin/iasyncsteps.hpp>
#include <futoin/iasynctool.hpp>
//---
#include <futoin/ri/binaryapi.hpp>
//---
#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <tuple>
#include <type_traits>

namespace futoin {
    namespace ri {
        namespace nitro_details {
            using namespace asyncsteps;
            using StepIndex = std::uint8_t;

            // Impl details
            //-------------------------
            struct NitroStepData : StepData
            {
                using FlagBase = std::uint8_t;
                enum Flags : FlagBase
                {
                    HaveCancel = (1 << 0),
                    HaveTimeout = (1 << 1),
                    HaveWait = (1 << 2),
                    HaveExtended = (1 << 3),
                    RepeatStep = (1 << 4),
                    SuccessBlock = (HaveCancel | HaveTimeout | HaveWait),
                };

                bool is_auto_success() const noexcept
                {
                    return (flags & SuccessBlock) == 0;
                }

                void reset() noexcept
                {
                    flags = 0;
                }

                bool is_step_repeat() const noexcept
                {
                    return is_set(RepeatStep);
                }

                bool has_time_limit() const noexcept
                {
                    return is_set(HaveTimeout);
                }

                bool has_cancel() const noexcept
                {
                    return is_set(HaveCancel);
                }

                bool has_extended() const noexcept
                {
                    return is_set(HaveExtended);
                }

                void clear_resource_flags() noexcept
                {
                    clear_flags(SuccessBlock | HaveExtended);
                }

                bool is_set(FlagBase on_flags) const
                {
                    return (flags & on_flags) == on_flags;
                }

                void clear_flags(FlagBase off_flags)
                {
                    flags &= ~off_flags;
                }

                NitroStepData* parent{nullptr};
                FlagBase flags{0};
                StepIndex sub_queue_start{0};
                StepIndex sub_queue_front{0};
                StepIndex ext_state{0};
                StepIndex stack_allocs_count{0};
            };

            template<typename NS>
            struct HandleExecuteBase
            {
                void operator()()
                {
                    static_cast<NS*>(this)->handle_execute();
                }
            };

            template<typename NS>
            struct HandleTimeoutBase
            {
                void operator()()
                {
                    static_cast<NS*>(this)->handle_timeout();
                }
            };

            template<typename NS>
            struct HandleLoopBase
            {
                // Actual add() -> func
                void operator()(IAsyncSteps& asi)
                {
                    auto& ns = static_cast<NS&>(asi);
                    auto step = ns.last_step_;
                    auto& ext_state = ns.current_ext_state();
                    auto& cond = ext_state.cond;

                    if (step->stack_allocs_count != 0) {
                        ns.stack_dealloc(step->stack_allocs_count);
                        step->stack_allocs_count = 0;
                    }

                    if (!cond || cond(ext_state)) {
                        step->flags |= NitroStepData::RepeatStep;
                        step->on_error_ = std::ref(*this);
                        ext_state.handler(ext_state, asi);
                    } else {
                        step->clear_flags(NitroStepData::RepeatStep);
                    }
                }

                // Actual add() -> on_error
                void operator()(IAsyncSteps& asi, ErrorCode err)
                {
                    auto& ns = static_cast<NS&>(asi);
                    auto step = ns.last_step_;
                    auto& ext_state = ns.current_ext_state();

                    if (err == errors::LoopCont) {
                        auto error_label = asi.state().error_loop_label;

                        if ((error_label == nullptr)
                            || (strcmp(error_label, ext_state.label) == 0)) {
                            asi.success();
                        }
                    } else if (err == errors::LoopBreak) {
                        auto error_label = asi.state().error_loop_label;

                        if ((error_label == nullptr)
                            || (strcmp(error_label, ext_state.label) == 0)) {
                            step->clear_flags(NitroStepData::RepeatStep);
                            asi.success();
                        }
                    } else {
                        step->clear_flags(NitroStepData::RepeatStep);
                    }
                }
            };

            template<typename NS>
            struct HandleSyncBase
            {
                void operator()(IAsyncSteps& asi)
                {
                    auto& ns = static_cast<NS&>(asi);
                    auto& ext_state = ns.current_ext_state();

                    asi.setCancel(&HandleSyncBase::sync_unlock_handler);

                    asi.add(&HandleSyncBase::sync_lock_handler);

                    auto& data = ns.add_step();
                    auto& orig_step_data = ext_state.orig_step_data;
                    data.func_ = std::move(orig_step_data.func_);
                    data.on_error_ = std::move(orig_step_data.on_error_);

                    asi.add(&HandleSyncBase::sync_unlock_handler);
                }

                static void sync_unlock_handler(IAsyncSteps& asi)
                {
                    auto& ns = static_cast<NS&>(asi);
                    auto& ext_state = ns.current_ext_state();

                    ext_state.sync_object->unlock(asi);
                }

                static void sync_lock_handler(IAsyncSteps& asi)
                {
                    auto& ns = static_cast<NS&>(asi);
                    auto& ext_state = ns.current_ext_state();

                    ext_state.sync_object->lock(asi);
                }
            };

            struct IParallelRoot
            {
                virtual BaseState& state() noexcept = 0;
                virtual void sub_completion() noexcept = 0;
                virtual void sub_onerror(
                        IAsyncSteps& sub, ErrorCode code) noexcept = 0;
            };

            template<typename NS>
            struct HandleParallelBase : IParallelRoot
            {
                static void launch_parallel(IAsyncSteps& asi) noexcept
                {
                    auto& ns = static_cast<NS&>(asi);
                    auto& ext_state = ns.current_ext_state();

                    asi.setCancel(&HandleParallelBase::cancel_parallel);
                    ns.error_code_cache[0] = 0;
                    ext_state.parallel_completed = 0;

                    for (auto& p : ext_state.parallel_items) {
                        p.execute();
                    }
                }

                static void cancel_parallel(IAsyncSteps& asi)
                {
                    auto& ns = static_cast<NS&>(asi);
                    auto& ext_state = ns.current_ext_state();

                    if (ns.error_code_cache[0] == 0) {
                        ext_state.parallel_items.clear();
                    }
                }

                void sub_completion() noexcept override
                {
                    auto& ns = static_cast<NS&>(*this);
                    auto& ext_state = ns.current_ext_state();
                    auto& parallel_completed = ext_state.parallel_completed;

                    ++parallel_completed;

                    if (parallel_completed == ext_state.parallel_items.size()) {
                        ns.exec_handle_ =
                                ns.async_tool_.immediate(std::ref(*this));
                    }
                }

                void sub_onerror(
                        IAsyncSteps& sub, ErrorCode code) noexcept override
                {
                    auto& ns = static_cast<NS&>(*this);
                    auto& ext_state = ns.current_ext_state();

                    for (auto& v : ext_state.parallel_items) {
                        if (&v != &sub) {
                            v.cancel();
                        }
                    }

                    ns.cache_error_code(code);
                    ns.exec_handle_ = ns.async_tool_.immediate(std::ref(*this));
                }

                // Dirty hack: final completion
                void operator()() noexcept
                {
                    auto& ns = static_cast<NS&>(*this);

                    auto& ext_state = ns.current_ext_state();
                    ext_state.parallel_items.clear();

                    if (ns.error_code_cache[0] == 0) {
                        ns.handle_success();
                    } else {
                        ns.handle_error(ns.error_code_cache);
                    }
                }
            };

            template<typename NS, typename PS>
            struct ParallelProtector final : IAsyncSteps
            {
                using ParallelItems = typename NS::ExtendedState::ParallelItems;

                ParallelProtector(NS& root, ParallelItems& items) noexcept :
                    root(root), parallel_items(items)
                {}

                ~ParallelProtector() noexcept override = default;

                PS& new_parallel_item() noexcept
                {
                    parallel_items.emplace_back(root.async_tool_, root);
                    return parallel_items.back();
                }

                StepData& add_step() noexcept override
                {
                    return new_parallel_item().add_step();
                }

                asyncsteps::LoopState& add_loop(
                        asyncsteps::LoopLabel label) noexcept override
                {
                    return new_parallel_item().add_loop(label);
                }

                StepData& add_sync(ISync& obj) noexcept override
                {
                    return new_parallel_item().add_sync(obj);
                }

                void await_impl(AwaitPass cb) noexcept override
                {
                    new_parallel_item().await_impl(cb);
                }

                [[noreturn]] BaseState& state() noexcept final
                {
                    FatalMsg() << "parallel().state() misuse";
                }

                [[noreturn]] IAsyncSteps& parallel(
                        ErrorPass /*on_error*/) noexcept final
                {
                    FatalMsg() << "parallel().parallel() misuse";
                };

                [[noreturn]] void handle_success() noexcept final
                {
                    FatalMsg() << "parallel().handle_success() misuse";
                }
                [[noreturn]] void handle_error(ErrorCode /*code*/) final
                {
                    FatalMsg() << "parallel().handle_error() misuse";
                }

                [[noreturn]] asyncsteps::NextArgs& nextargs() noexcept final
                {
                    FatalMsg() << "parallel().nextargs() misuse";
                };

                [[noreturn]] IAsyncSteps& copyFrom(
                        IAsyncSteps& /*asi*/) noexcept final
                {
                    FatalMsg() << "parallel().copyFrom() misuse";
                };

                [[noreturn]] void setTimeout(
                        std::chrono::milliseconds /*to*/) noexcept final
                {
                    FatalMsg() << "parallel().setTimeout() misuse";
                }
                [[noreturn]] void setCancel(
                        CancelPass /*on_cancel*/) noexcept final
                {
                    FatalMsg() << "parallel().setCancel() misuse";
                }
                [[noreturn]] void waitExternal() noexcept final
                {
                    FatalMsg() << "parallel().waitExternal() misuse";
                }
                [[noreturn]] void execute() noexcept final
                {
                    FatalMsg() << "parallel().execute() misuse";
                }
                [[noreturn]] void cancel() noexcept final
                {
                    FatalMsg() << "parallel().cancel() misuse";
                }
                [[noreturn]] std::unique_ptr<IAsyncSteps> newInstance() noexcept
                        final
                {
                    FatalMsg() << "parallel().newInstance() misuse";
                };

                [[noreturn]] operator bool() const noexcept final
                {
                    FatalMsg() << "parallel().operator bool() misuse";
                };

                [[noreturn]] SyncRootID sync_root_id() const final
                {
                    FatalMsg() << "parallel().sync_root_id() misuse";
                }

                [[noreturn]] void* stack(
                        std::size_t /*object_size*/,
                        StackDestroyHandler /*destroy_cb*/) noexcept final
                {
                    FatalMsg() << "parallel().stack() misuse";
                }

                [[noreturn]] FutoInAsyncSteps& binary() noexcept override
                {
                    FatalMsg() << "parallel().binary() misuse";
                }

                [[noreturn]] std::unique_ptr<IAsyncSteps> wrap(
                        FutoInAsyncSteps& /*binary_steps*/) noexcept override
                {
                    FatalMsg() << "parallel().wrap() misuse";
                }

                [[noreturn]] IAsyncTool& tool() noexcept override
                {
                    FatalMsg() << "parallel().tool() misuse";
                }

                NS& root;
                ParallelItems& parallel_items;
            };

            template<typename NS>
            struct HandleAwaitBase
            {
                // Actual add() -> func
                void operator()(IAsyncSteps& asi)
                {
                    using std::chrono::milliseconds;

                    auto& ns = static_cast<NS&>(asi);
                    auto step = ns.last_step_;
                    auto& ext_state = ns.current_ext_state();

                    // NOTE: reset to shift queue in success()
                    step->clear_flags(NitroStepData::RepeatStep);

                    // NOTE: Yes, it's resource intensive
                    if (!ext_state.await_func_(asi, milliseconds{0}, true)) {
                        step->flags |= NitroStepData::RepeatStep;
                    }
                }
            };

            template<typename NS>
            struct HandleBases : HandleExecuteBase<NS>,
                                 HandleTimeoutBase<NS>,
                                 HandleLoopBase<NS>,
                                 HandleSyncBase<NS>,
                                 HandleParallelBase<NS>,
                                 HandleAwaitBase<NS>
            {
                using HandleExecute = HandleExecuteBase<NS>;
                using HandleTimeout = HandleTimeoutBase<NS>;
                using HandleLoop = HandleLoopBase<NS>;
                using HandleSync = HandleSyncBase<NS>;
                using HandleParallel = HandleParallelBase<NS>;
                using HandleAwait = HandleAwaitBase<NS>;
            };

            // Impl details
            //-------------------------
            template<bool is_root = true>
            struct StateBase
            {
                struct Impl
                {
                    Impl(IParallelRoot& /*ns*/, IAsyncTool& async_tool) noexcept
                        :
                        state_(async_tool.mem_pool())
                    {}

                    State& get_state() noexcept
                    {
                        return state_;
                    }

                    void sub_completion() const noexcept {}
                    bool sub_onerror(IAsyncSteps& /*sub*/, ErrorCode /*code*/)
                            const noexcept
                    {
                        return false;
                    }

                    State state_;
                };
            };

            template<>
            struct StateBase<false>
            {
                struct Impl
                {
                    Impl(IParallelRoot& ns, IAsyncTool& /*async_tool*/) noexcept
                        :
                        root_(&ns)
                    {}

                    // NOLINTNEXTLINE(readability-make-member-function-const)
                    BaseState& get_state() noexcept
                    {
                        return root_->state();
                    }

                    // NOLINTNEXTLINE(readability-make-member-function-const)
                    void sub_completion() noexcept
                    {
                        root_->sub_completion();
                    }

                    // NOLINTNEXTLINE(readability-make-member-function-const)
                    bool sub_onerror(IAsyncSteps& sub, ErrorCode code) noexcept
                    {
                        root_->sub_onerror(sub, code);
                        return true;
                    }

                    IParallelRoot* root_;
                };
            };

            // Parameters
            //-------------------------
            template<bool is_root>
            struct IsRoot
            {
                template<typename Base>
                struct Override : Base
                {
                    static constexpr bool IS_ROOT = is_root;

                    using Impl = typename StateBase<is_root>::Impl;
                };
            };

            template<StepIndex max_steps>
            struct MaxSteps
            {
                template<typename Base>
                struct Override : Base
                {
                    using Queue = std::array<NitroStepData, max_steps>;
                    static constexpr StepIndex MAX_STEPS = max_steps;
                };
            };

            template<StepIndex max_timeouts>
            struct MaxTimeouts
            {
                template<typename Base>
                struct Override : Base
                {
                    using TimeoutList =
                            std::array<IAsyncTool::Handle, max_timeouts>;
                    static constexpr auto MAX_TIMEOUTS = max_timeouts;
                };
            };

            template<StepIndex max_cancels>
            struct MaxCancels
            {
                template<typename Base>
                struct Override : Base
                {
                    struct CancelCallbackHolder
                    {
                        CancelPass::Storage storage;
                        CancelCallback func;
                    };

                    using CancelList =
                            std::array<CancelCallbackHolder, max_cancels>;
                    static constexpr auto MAX_TIMEOUTS = max_cancels;
                };
            };

            template<StepIndex max_extended>
            struct MaxExtended
            {
                template<typename Base>
                struct Override : Base
                {
                    template<typename NS>
                    struct ExtendedState : LoopState
                    {
                        bool is_used{false};
                        StepData orig_step_data;

                        using ParallelItem = typename NS::ParallelSteps;
                        using ParallelItems = std::deque<
                                ParallelItem,
                                IMemPool::Allocator<ParallelItem>>;
                        ParallelItems parallel_items;
                        std::aligned_storage<
                                sizeof(void*) * 3,
                                std::alignment_of<void*>::value>::type
                                protector_storage;

                        union
                        {
                            std::size_t parallel_completed{0};
                            ISync* sync_object;
                        };

                        asyncsteps::AwaitCallback await_func_;
                    };

                    template<typename NS>
                    using ExtendedList =
                            std::array<ExtendedState<NS>, max_extended>;
                    static constexpr auto MAX_EXTENDED = max_extended;
                };
            };

            template<StepIndex max_allocs>
            struct MaxStackAllocs
            {
                template<typename Base>
                struct Override : Base
                {
                    using StackAlloc = std::tuple<
                            void*,
                            IAsyncSteps::StackDestroyHandler,
                            size_t>;
                    using StackAllocList = std::array<StackAlloc, max_allocs>;
                    static constexpr auto MAX_STACK_ALLOCS = max_allocs;
                };
            };

            template<StepIndex max_size>
            struct ErrorCodeMaxSize
            {
                template<typename Base>
                struct Override : Base
                {
                    // NOLINTNEXTLINE(modernize-avoid-c-arrays)
                    using ErrorCodeCache = char[max_size + 1];
                };
            };

            // Yes, there is Boost.Parameter...
            //-------------------------

            struct DefaultNoop
            {};

            struct DefaultValues : IsRoot<true>::Override<DefaultNoop>,
                                   MaxSteps<16>::Override<DefaultNoop>,
                                   MaxTimeouts<4>::Override<DefaultNoop>,
                                   MaxCancels<4>::Override<DefaultNoop>,
                                   MaxExtended<4>::Override<DefaultNoop>,
                                   MaxStackAllocs<8>::Override<DefaultNoop>,
                                   ErrorCodeMaxSize<32>::Override<DefaultNoop>
            {};

            template<typename T, typename... Params>
            struct DefaultTraits
                : T::template Override<DefaultTraits<Params...>>
            {};

            template<>
            struct DefaultTraits<void> : DefaultValues
            {};

            template<typename... Params>
            struct Defaults : DefaultTraits<Params..., void>
            {};
        } // namespace nitro_details

        namespace nitro {
            using nitro_details::Defaults;
            using nitro_details::StepIndex;

            template<bool is_root>
            using IsRoot = nitro_details::IsRoot<is_root>;

            /**
             * @brief Configure maximum numbers of actively set steps.
             */
            template<StepIndex max_steps>
            using MaxSteps = nitro_details::MaxSteps<max_steps>;

            /**
             * @brief Configure maximum numbers of active setTimeout() calls.
             */
            template<StepIndex max_timeouts>
            using MaxTimeouts = nitro_details::MaxTimeouts<max_timeouts>;

            /**
             * @brief Configure maximum numbers of active setCancel() calls.
             */
            template<StepIndex max_cancels>
            using MaxCancels = nitro_details::MaxCancels<max_cancels>;

            /**
             * @brief Configure maximum numbers of active loop(), await() and/or
             * sync() calls.
             */
            template<StepIndex max_extended>
            using MaxExtended = nitro_details::MaxExtended<max_extended>;

            /**
             * @brief Configure maximum numbers of active stack() calls.
             */
            template<StepIndex max_allocs>
            using MaxStackAllocs = nitro_details::MaxStackAllocs<max_allocs>;

            /**
             * @brief Configure maximum length of error code.
             */
            template<StepIndex max_size>
            using ErrorCodeMaxSize = nitro_details::ErrorCodeMaxSize<max_size>;
        } // namespace nitro

        /**
         * @brief Nitro-implementation of AsyncSteps
         *
         * It's pure template based with almost all internals allocated
         * statically in root instance. Exception is for parallel sub-steps.
         */
        template<typename... Params>
        class NitroSteps final
            : public IAsyncSteps,
              private nitro_details::HandleBases<NitroSteps<Params...>>
        {
            using Parameters = nitro_details::Defaults<Params...>;
            using StepIndex = nitro_details::StepIndex;
            using NitroStepData = nitro_details::NitroStepData;
            using ExtendedState =
                    typename Parameters::template ExtendedState<NitroSteps>;

            using HandleBases = nitro_details::HandleBases<NitroSteps>;
            using typename HandleBases::HandleAwait;
            using typename HandleBases::HandleExecute;
            using typename HandleBases::HandleLoop;
            using typename HandleBases::HandleParallel;
            using typename HandleBases::HandleSync;
            using typename HandleBases::HandleTimeout;

            friend ExtendedState;
            friend HandleExecute;
            friend HandleTimeout;
            friend HandleLoop;
            friend HandleSync;
            friend HandleParallel;
            friend HandleAwait;

            // Special stuff for parallel
            //---
            using ParallelSteps = typename std::conditional<
                    Parameters::IS_ROOT,
                    NitroSteps<nitro::IsRoot<false>, Params...>,
                    NitroSteps>::type;

            using ParallelProtector =
                    nitro_details::ParallelProtector<NitroSteps, ParallelSteps>;

            template<typename, typename>
            friend struct nitro_details::ParallelProtector;
            template<typename...>
            friend class NitroSteps;
            template<typename>
            friend class futoin::IMemPool::Allocator;

            NitroSteps(
                    IAsyncTool& async_tool,
                    nitro_details::IParallelRoot& root) noexcept :
                async_tool_(async_tool), impl_(root, async_tool)
            {}
            //---

        public:
            NitroSteps(IAsyncTool& async_tool) noexcept :
                async_tool_(async_tool), impl_(*this, async_tool)
            {}

            NitroSteps(const NitroSteps&) = delete;
            NitroSteps& operator=(const NitroSteps&) = delete;
            NitroSteps(NitroSteps&&) = delete;
            NitroSteps& operator=(NitroSteps&&) = delete;
            ~NitroSteps() noexcept final
            {
                cancel();
            }

            IAsyncSteps& parallel(ErrorPass on_error = {}) noexcept final
            {
                auto& step = alloc_step(last_step_);
                auto& ext_state = alloc_extended(step);
                auto p = new (&ext_state.protector_storage)
                        ParallelProtector(*this, ext_state.parallel_items);

                step.func_ = &HandleParallel::launch_parallel;
                on_error.move(step.on_error_, step.on_error_storage_);

                return *p;
            }

            asyncsteps::NextArgs& nextargs() noexcept final
            {
                return next_args_;
            }

            [[noreturn]] IAsyncSteps& copyFrom(
                    IAsyncSteps& /*asi*/) noexcept final
            {
                FatalMsg() << "copyFrom() is not supported";
            }

            void setTimeout(std::chrono::milliseconds to) noexcept final
            {
                if (timeout_size_ == timeout_list_.size()) {
                    FatalMsg() << "Reached maximum number of setCancel() per "
                                  "NitroSteps";
                }

                auto& handle = timeout_list_[timeout_size_];
                ++timeout_size_;

                HandleTimeout& ht = *this;
                handle = async_tool_.deferred(to, std::ref(ht));

                last_step_->flags |= NitroStepData::HaveTimeout;
            }

            void setCancel(CancelPass cb) noexcept final
            {
                if (cancel_size_ == cancel_list_.size()) {
                    FatalMsg() << "Reached maximum number of setCancel() per "
                                  "NitroSteps";
                }

                auto& handler = cancel_list_[cancel_size_];
                ++cancel_size_;

                cb.move(handler.func, handler.storage);

                last_step_->flags |= NitroStepData::HaveCancel;
            }

            void waitExternal() noexcept final
            {
                last_step_->flags |= NitroStepData::HaveWait;
            }

            void execute() noexcept final
            {
                HandleExecute& he = *this;
                exec_handle_ = async_tool_.immediate(std::ref(he));
            }

            void cancel() noexcept final
            {
                if (!is_queue_empty() && !async_tool_.is_same_thread()) {
                    std::promise<void> done;
                    auto task = [this, &done]() {
                        this->cancel();
                        done.set_value();
                    };
                    async_tool_.immediate(std::ref(task));
                    done.get_future().wait();
                    return;
                }

                exec_handle_.cancel();

                while (last_step_ != nullptr) {
                    auto current = last_step_;

                    if (current->has_cancel()) {
                        cancel_list_[cancel_size_ - 1].func(*this);
                        --cancel_size_;
                        current->clear_flags(NitroStepData::HaveCancel);
                    }

                    free_step(current);
                    last_step_ = current->parent;
                }

                reset_queue();
            }

            operator bool() const noexcept final
            {
                return true;
            }

            std::unique_ptr<IAsyncSteps> newInstance() noexcept final
            {
                return std::unique_ptr<IAsyncSteps>(
                        new NitroSteps(async_tool_));
            }

            SyncRootID sync_root_id() const final
            {
                return reinterpret_cast<SyncRootID>(this);
            }

            BaseState& state() noexcept final
            {
                return impl_.get_state();
            }

            void* stack(
                    std::size_t object_size,
                    StackDestroyHandler destroy_cb) noexcept final
            {
                if (stack_alloc_size_ == stack_alloc_list_.size()) {
                    FatalMsg() << "Reached maximum number of stack() per "
                                  "NitroSteps";
                }

                auto ptr = mem_pool().allocate(object_size, 1);
                stack_alloc_list_[stack_alloc_size_] =
                        typename Parameters::StackAlloc(
                                ptr, destroy_cb, object_size);
                ++stack_alloc_size_;

                if (last_step_ != nullptr) {
                    ++(last_step_->stack_allocs_count);
                }

                return ptr;
            }

            FutoInAsyncSteps& binary() noexcept override
            {
                return IAsyncSteps::stack<BinarySteps>(*this);
            }

            std::unique_ptr<IAsyncSteps> wrap(
                    FutoInAsyncSteps& binary_steps) noexcept override
            {
                return wrap_binary_steps(binary_steps);
            }

            IAsyncTool& tool() noexcept override
            {
                return async_tool_;
            }

            using IAsyncSteps::promise;
            using IAsyncSteps::stack;
            using IAsyncSteps::state;

        protected:
            StepData& add_step() noexcept final
            {
                return alloc_step(last_step_);
            }

            void handle_success() noexcept final
            {
                auto current = last_step_;

                if (!is_sub_queue_empty(current)) {
                    FatalMsg() << "success() with non-empty queue";
                }

                current = current->parent;

                while (current != nullptr) {
                    cond_sub_queue_shift(current);

                    if (!is_sub_queue_empty(current)) {
                        last_step_ = current;
                        execute();
                        return;
                    }

                    sub_queue_free(current);
                    current = current->parent;
                }

                last_step_ = nullptr;

                // Got to root queue
                cond_queue_shift();

                if (!is_queue_empty()) {
                    execute();
                } else {
                    impl_.sub_completion();
                }
            }

            void handle_error(ErrorCode code) final
            {
                if (exec_handle_) {
                    exec_handle_.cancel();
                }

                if (in_exec_) {
                    // avoid double handling
                    return;
                }

                auto current = last_step_;

                for (;;) {
                    sub_queue_free(current);
                    current->sub_queue_front = current->sub_queue_start;

                    if (current->has_time_limit()) {
                        timeout_list_[timeout_size_ - 1].cancel();
                        --timeout_size_;
                        current->clear_flags(NitroStepData::HaveTimeout);
                    }

                    if (current->has_cancel()) {
                        cancel_list_[cancel_size_ - 1].func(*this);
                        --cancel_size_;
                        current->clear_flags(NitroStepData::HaveCancel);
                    }

                    asyncsteps::ErrorHandler on_error{
                            std::move(current->on_error_)};

                    if (on_error) {
                        try {
                            in_exec_ = true;
                            on_error(*this, code);
                            in_exec_ = false;

                            if (last_step_ != current) {
                                // success() was called
                                return;
                            }

                            if (!is_sub_queue_empty(current)) {
                                execute();
                                return;
                            }
                        } catch (const std::exception& e) {
                            in_exec_ = false;
                            impl_.get_state().catch_trace(e);

                            code = cache_error_code(e.what());
                        }
                    }

                    free_step(current);
                    current = current->parent;
                    last_step_ = current;

                    if (current == nullptr) {
                        break;
                    }
                }

                reset_queue();

                if (!impl_.sub_onerror(*this, code)) {
                    auto& unhandled_error = impl_.get_state().unhandled_error;

                    if (unhandled_error) {
                        unhandled_error(code);
                    } else {
                        FatalMsg() << "unhandled AsyncStep error " << code;
                    }
                }
            }

            asyncsteps::LoopState& add_loop(
                    asyncsteps::LoopLabel label) noexcept final
            {
                auto& step = alloc_step(last_step_);

                HandleLoop& hl = *this;
                step.func_ = std::ref(hl);

                auto& ls = alloc_extended(step);
                ls.label = label;
                return ls;
            }

            StepData& add_sync(ISync& obj) noexcept final
            {
                auto& step = alloc_step(last_step_);

                HandleSync& hs = *this;
                step.func_ = std::ref(hs);

                auto& ext_state = alloc_extended(step);
                ext_state.sync_object = &obj;
                return ext_state.orig_step_data;
            }

            void await_impl(AwaitPass awp) noexcept final
            {
                auto& step = alloc_step(last_step_);
                auto& ext_state = alloc_extended(step);

                HandleAwait& ha = *this;
                step.func_ = std::ref(ha);

                awp.move(ext_state.await_func_, ext_state.outer_func_storage);
            }

        private:
            void handle_execute() noexcept
            {
                exec_handle_.reset();
                NitroStepData* next = nullptr;

                auto current = last_step_;

                while (current != nullptr) {
                    if (is_sub_queue_empty(current)) {
                        sub_queue_free(current);
                        current = current->parent;
                        last_step_ = current;
                    } else {
                        next = step_front(current);
                        break;
                    }
                }

                if (next == nullptr) {
                    if (is_queue_empty()) {
                        return;
                    }

                    next = step_at(queue_begin_);
                }

                const auto qs = queue_end();
                next->sub_queue_start = qs;
                next->sub_queue_front = qs;

                last_step_ = next;

                try {
                    in_exec_ = true;
                    next->func_(*this);

                    if (last_step_ != next) {
                        // pass
                    } else if (!is_sub_queue_empty(next)) {
                        execute();
                    } else if (next->is_auto_success()) {
                        handle_success();
                    }

                    in_exec_ = false;
                } catch (const std::exception& e) {
                    in_exec_ = false;
                    impl_.get_state().catch_trace(e);
                    handle_error(e.what());
                }
            }

            void handle_timeout() noexcept
            {
                handle_error(errors::Timeout);
            }

            bool is_queue_empty() const noexcept
            {
                return queue_size_ == 0;
            }

            StepIndex queue_end() const noexcept
            {
                return (queue_begin_ + queue_size_) % Parameters::MAX_STEPS;
            }

            bool is_sub_queue_empty(NitroStepData* current) const noexcept
            {
                return current->sub_queue_front == queue_end();
            }

            void sub_queue_free(NitroStepData* current) noexcept
            {
                queue_size_ = (current->sub_queue_start + Parameters::MAX_STEPS
                               - queue_begin_)
                              % Parameters::MAX_STEPS;
            }

            void shift_queue_index(StepIndex& index) noexcept
            {
                index = (index + 1) % Parameters::MAX_STEPS;
            }

            void cond_sub_queue_shift(NitroStepData* current)
            {
                auto front = step_at(current->sub_queue_front);

                if (!front->is_step_repeat()) {
                    free_step(front);
                    shift_queue_index(current->sub_queue_front);
                }
            }

            void cond_queue_shift()
            {
                auto current = step_at(queue_begin_);

                if (!current->is_step_repeat()) {
                    free_step(current);
                    shift_queue_index(queue_begin_);
                    --queue_size_;
                }
            }

            NitroStepData* step_front(NitroStepData* current) noexcept
            {
                return &(queue_[current->sub_queue_front]);
            }

            NitroStepData* step_at(StepIndex index) noexcept
            {
                return &(queue_[index]);
            }

            ExtendedState& current_ext_state() noexcept
            {
                return extended_list_[last_step_->ext_state];
            }

            NitroStepData& alloc_step(NitroStepData* parent) noexcept
            {
                if (queue_size_ == Parameters::MAX_STEPS) {
                    FatalMsg() << "Reached NitroSteps limit";
                }

                auto index = queue_end();
                ++queue_size_;
                auto& step = queue_[index];
                step.reset();
                step.parent = parent;
                return step;
            }

            ExtendedState& alloc_extended(NitroStepData& step) noexcept
            {
                for (auto& ext_state : extended_list_) {
                    if (ext_state.is_used) {
                        continue;
                    }

                    ext_state.is_used = true;
                    step.flags |= NitroStepData::HaveExtended;
                    step.ext_state = &ext_state - &(extended_list_[0]);
                    return ext_state;
                }

                FatalMsg() << "Reached maximum number of extended state "
                              "per NitroSteps";
            }

            void free_step(NitroStepData* current) noexcept
            {
                if (current->has_time_limit()) {
                    timeout_list_[timeout_size_ - 1].cancel();
                    --timeout_size_;
                }

                if (current->has_cancel()) {
                    --cancel_size_;
                }

                if (current->has_extended()) {
                    extended_list_[current->ext_state].is_used = false;
                }

                auto& stack_allocs_count = current->stack_allocs_count;

                if (stack_allocs_count != 0) {
                    stack_dealloc(stack_allocs_count);
                    stack_allocs_count = 0;
                }

                current->clear_resource_flags();
            }

            void stack_dealloc(std::size_t count)
            {
                auto& mem_pool = this->mem_pool();

                for (auto i = count; i > 0; --i) {
                    void* ptr;
                    StackDestroyHandler destroy_cb;
                    std::size_t object_size;

                    std::tie(ptr, destroy_cb, object_size) =
                            stack_alloc_list_[stack_alloc_size_ - 1];
                    destroy_cb(ptr);
                    mem_pool.deallocate(ptr, object_size, 1);
                    --stack_alloc_size_;
                }
            }

            void reset_queue() noexcept
            {
                queue_begin_ = 0;
                queue_size_ = 0;
                timeout_size_ = 0;

                if (stack_alloc_size_ != 0) {
                    stack_dealloc(stack_alloc_size_);
                }
            }

            RawErrorCode cache_error_code(RawErrorCode code) noexcept
            {
#ifdef __clang
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wstringop-truncation"
#elif defined(__GNUC__)
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wstringop-truncation"
#endif
                strncpy(error_code_cache, code, sizeof(error_code_cache));
#ifdef __clang
#    pragma clang diagnostic pop
#elif defined(__GNUC__)
#    pragma GCC diagnostic pop
#endif

                if (error_code_cache[sizeof(error_code_cache) - 1] != 0) {
                    FatalMsg()
                            << "too long error code for NitroSteps: " << code;
                }

                return error_code_cache;
            }

            IMemPool& mem_pool() noexcept
            {
                return impl_.get_state().mem_pool();
            }

            IAsyncTool& async_tool_;
            typename Parameters::Impl impl_;
            IAsyncTool::Handle exec_handle_;
            asyncsteps::NextArgs next_args_;
            NitroStepData* last_step_{nullptr};
            bool in_exec_{false};
            StepIndex queue_begin_{0};
            StepIndex queue_size_{0};
            typename Parameters::Queue queue_;
            StepIndex timeout_size_{0};
            typename Parameters::TimeoutList timeout_list_;
            StepIndex cancel_size_{0};
            typename Parameters::CancelList cancel_list_;
            typename Parameters::template ExtendedList<NitroSteps>
                    extended_list_;
            StepIndex stack_alloc_size_{0};
            typename Parameters::StackAllocList stack_alloc_list_;
            typename Parameters::ErrorCodeCache error_code_cache;

#if 0
            static_assert(
                        std::is_trivial<NitroStepData>::value,
                        "StepData is not trivial");
#endif
        };
    } // namespace ri
} // namespace futoin

//---
#endif // FUTOIN_RI_NITROSTEPS_HPP
