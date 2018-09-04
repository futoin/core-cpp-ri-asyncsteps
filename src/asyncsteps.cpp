//-----------------------------------------------------------------------------
//   Copyright 2018 FutoIn Project
//   Copyright 2018 Andrey Galkin
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

#include <futoin/ri/asyncsteps.hpp>

#include <cassert>
#include <cstring>
#include <deque>
#include <future>
#include <iostream>
#include <memory>
#include <stack>

namespace futoin {
    namespace ri {
        using namespace futoin::asyncsteps;

        constexpr inline std::chrono::milliseconds AWAIT_DELAY()
        {
            return std::chrono::milliseconds{10};
        }

        //---
        [[noreturn]] static void on_invalid_call(
                const char* extra_error = nullptr)
        {
            std::cerr << std::endl;
            std::cerr << "FATAL: Invalid AsyncSteps interface usage!"
                      << std::endl;

            if (extra_error != nullptr) {
                std::cerr << extra_error << std::endl;
            }

            std::cerr << std::endl;

            std::terminate();
        }

        //---
        struct SubAsyncSteps final : public BaseAsyncSteps
        {
            SubAsyncSteps(State& state, IAsyncTool& async_tool) noexcept :
                BaseAsyncSteps(state, async_tool)
            {}
        };

        //---
        struct BaseAsyncSteps::ExtStepState : asyncsteps::LoopState
        {
            ExtStepState(IMemPool& mem_pool, bool is_loop) :
                continue_loop(is_loop),
                items_{ParallelItems::allocator_type(mem_pool)},
                error_code_{futoin::string::allocator_type(mem_pool)}
            {}

            ~ExtStepState() noexcept
            {
                if (await_thread_) {
                    if (await_thread_->get_id() == std::this_thread::get_id()) {
                        await_thread_->detach();
                    } else {
                        await_thread_->join();
                    }
                }
            }

            // Loop stuff
            //--------------------
            // Actual add() -> func
            void operator()(IAsyncSteps& asi)
            {
                if (!cond || cond(*this)) {
                    handler(*this, asi);
                } else {
                    continue_loop = false;
                }
            }
            // Actual add() -> on_error
            void operator()(IAsyncSteps& asi, ErrorCode err)
            {
                if (std::strcmp(err, errors::LoopCont) == 0) {
                    auto error_label = asi.state().error_loop_label;

                    if ((error_label == nullptr)
                        || (strcmp(error_label, label) == 0)) {
                        asi.success();
                    }
                } else if (std::strcmp(err, errors::LoopBreak) == 0) {
                    auto error_label = asi.state().error_loop_label;

                    if ((error_label == nullptr)
                        || (strcmp(error_label, label) == 0)) {
                        continue_loop = false;
                        asi.success();
                    }
                } else {
                    continue_loop = false;
                }
            }

            bool continue_loop{true};

            // Parallel step stuff
            //--------------------
            using ParallelItems = std::
                    deque<SubAsyncSteps, IMemPool::Allocator<SubAsyncSteps>>;
            ParallelItems items_;
            std::size_t completed_{0};
            futoin::string error_code_;

            // Await step stuff
            //--------------------
            AwaitPass::Storage await_storage_;
            asyncsteps::AwaitCallback await_func_;
            std::unique_ptr<std::thread> await_thread_;
            std::atomic_bool continue_await{true};
        };

        class BaseAsyncSteps::ProtectorData : public IAsyncSteps
        {
            friend class BaseAsyncSteps;
            friend struct BaseAsyncSteps::Impl;

        public:
            ProtectorData(
                    BaseAsyncSteps& root,
                    ProtectorData* parent = nullptr) noexcept :
                root_(&root),
                parent_(parent)
            {}
            ~ProtectorData() noexcept override;

        protected:
            BaseAsyncSteps* root_;
            ProtectorData* parent_;

            CancelPass::Storage on_cancel_storage_;
            StepData data_;
            CancelCallback on_cancel_;
            ExtStepState* ext_data_{nullptr};
            IAsyncTool::Handle limit_handle_;
            std::size_t sub_queue_start{0};
            std::size_t sub_queue_front{0};
        };

        //---
        struct BaseAsyncSteps::Impl
        {
            struct ProtectorDataHolder
            {
                alignas(ProtectorData) char data[sizeof(ProtectorData)];
            };

            using QueueItem = ProtectorDataHolder;
            using Queue = std::deque<QueueItem, IMemPool::Allocator<QueueItem>>;
            using Stack = std::
                    deque<ProtectorData*, IMemPool::Allocator<ProtectorData*>>;

            Impl(State& state,
                 IAsyncTool& async_tool,
                 IMemPool& mem_pool) noexcept :
                async_tool_(async_tool),
                mem_pool_(mem_pool),
                queue_{Queue::allocator_type(mem_pool)},
                state_(state),
                ext_data_allocator(mem_pool)
            {}

            void sanity_check() noexcept
            {
                if ((stack_top_ != nullptr) || exec_handle_) {
                    on_invalid_call("Out-of-order use of root AsyncSteps");
                }
            }

            void schedule_exec() noexcept;
            void execute_handler() noexcept;
            void handle_success(ProtectorData* current) noexcept;
            void handle_error(ProtectorData* current, ErrorCode code) noexcept;
            void handle_cancel() noexcept;
            void operator()() noexcept
            {
                execute_handler();
            }

            void cond_sub_queue_shift(ProtectorData* current)
            {
                auto& sqf = queue_[current->sub_queue_front];
                auto& front_step = reinterpret_cast<ProtectorData&>(sqf);
                auto loop_state = front_step.ext_data_;

                if ((loop_state == nullptr) || !loop_state->continue_loop) {
                    ++(current->sub_queue_front);
                }
            }

            void cond_queue_shift()
            {
                auto& front_step =
                        reinterpret_cast<ProtectorData&>(queue_.front());
                auto loop_state = front_step.ext_data_;

                if ((loop_state == nullptr) || !loop_state->continue_loop) {
                    front_step.~ProtectorData();
                    queue_.pop_front();
                }
            }

            ProtectorDataHolder* alloc_step()
            {
                queue_.emplace_back();
                return &(queue_.back());
            }

            bool is_sub_queue_empty(ProtectorData* current) const
            {
                return current->sub_queue_front == queue_.size();
            }

            void sub_queue_free(ProtectorData* current)
            {
                auto sub_queue_begin =
                        queue_.begin() + current->sub_queue_start;
                auto sub_queue_end = queue_.end();

                for (auto iter = sub_queue_begin; iter != sub_queue_end;
                     ++iter) {
                    auto& p = reinterpret_cast<ProtectorData&>(*iter);
                    p.~ProtectorData();
                }

                queue_.erase(sub_queue_begin, sub_queue_end);
            }

            void clear_queue()
            {
                for (auto& v : queue_) {
                    auto& p = reinterpret_cast<ProtectorData&>(v);
                    p.~ProtectorData();
                }

                queue_.clear();
            }

            bool is_safe_to_process()
            {
                if (async_tool_.is_same_thread()) {
                    return true;
                }

                if (std::this_thread::get_id() == await_thread_id_) {
                    return true;
                }

                return false;
            }

            IAsyncTool& async_tool_;
            IMemPool& mem_pool_;
            NextArgs next_args_;
            Queue queue_;
            ProtectorData* stack_top_{nullptr};
            IAsyncTool::Handle exec_handle_;
            State& state_;
            std::thread::id await_thread_id_;
            bool in_exec_ = false;

            IMemPool::Allocator<ExtStepState> ext_data_allocator;
        };

        //---

        class BaseAsyncSteps::Protector : public ProtectorData
        {
        public:
            Protector(BaseAsyncSteps& root, ProtectorData* parent) noexcept :
                ProtectorData(root, parent)
            {}

            void sanity_check() noexcept
            {
                if (root_ == nullptr) {
                    on_invalid_call("Step got invalidated!");
                }

                if (this != root_->impl_->stack_top_) {
                    on_invalid_call("Step used out-of-order!");
                }
            }

            operator bool() const noexcept override
            {
                if (root_ == nullptr) {
                    return false;
                }

                return (this == root_->impl_->stack_top_);
            }

            StepData& add_step() noexcept override
            {
                sanity_check();

                auto buf = root_->impl_->alloc_step();
                auto step = new (buf) Protector(*root_, this);
                return step->data_;
            }

            IAsyncSteps& parallel(ErrorPass on_error = {}) noexcept override;

            void handle_success() noexcept override
            {
                sanity_check();
                root_->impl_->handle_success(this);
            }

            void handle_error(ErrorCode code) override
            {
                sanity_check();
                root_->impl_->handle_error(this, code);
            }

            NextArgs& nextargs() noexcept override
            {
                sanity_check();
                return root_->nextargs();
            }

            IAsyncSteps& copyFrom(IAsyncSteps& /*asi*/) noexcept override
            {
                on_invalid_call("copyFrom() is not supported in C++");
            }

            State& state() noexcept override
            {
                return root_->state();
            }

            void setTimeout(std::chrono::milliseconds to) noexcept override
            {
                sanity_check();
                auto& async_tool = root_->impl_->async_tool_;
                limit_handle_ = async_tool.deferred(to, std::ref(*this));
            }

            void setCancel(CancelPass cb) noexcept override
            {
                sanity_check();
                cb.move(on_cancel_, on_cancel_storage_);
            }

            void waitExternal() noexcept override
            {
                sanity_check();

                if (!on_cancel_) {
                    on_cancel_ = [](IAsyncSteps&) {};
                }
            }

            void execute() noexcept override
            {
                on_invalid_call("execute() in execute()");
            }

            void cancel() noexcept override
            {
                on_invalid_call("cancel() in execute()");
            }

            ExtStepState& alloc_ext_data(bool is_loop) noexcept
            {
                auto impl = root_->impl_;
                assert(!ext_data_);
                auto pls = impl->ext_data_allocator.allocate(1);
                ext_data_ = new (pls) ExtStepState(impl->mem_pool_, is_loop);
                return *pls;
            }

            LoopState& add_loop() noexcept override
            {
                sanity_check();

                auto buf = root_->impl_->alloc_step();
                auto step = new (buf) Protector(*root_, this);

                step->data_.func_ = &Protector::loop_handler;

                return step->alloc_ext_data(true);
            }

            static void loop_handler(IAsyncSteps& asi) noexcept
            {
                auto& that = static_cast<Protector&>(asi);
                auto& ls = *(that.ext_data_);

                auto buf = that.root_->impl_->alloc_step();
                auto step = new (buf) Protector(*(that.root_), &that);

                step->data_.func_ = std::ref(ls);
                step->data_.on_error_ = std::ref(ls);
            }

            std::unique_ptr<IAsyncSteps> newInstance() noexcept override
            {
                sanity_check();

                return root_->newInstance();
            }

            SyncRootID sync_root_id() const override
            {
                return root_->sync_root_id();
            }

            // Dirty hack: the step serves as timeout functor (base
            // operator() is hidden)
            void operator()() noexcept
            {
                try {
                    this->error(errors::Timeout);
                } catch (...) {
                }
            }

            void await_impl(AwaitPass awp) noexcept override
            {
                sanity_check();

                auto buf = root_->impl_->alloc_step();
                auto step = new (buf) Protector(*root_, this);

                step->data_.func_ = &Protector::await_handler;

                auto& ext = step->alloc_ext_data(false);
                awp.move(ext.await_func_, ext.await_storage_);
            }

            static void await_handler(IAsyncSteps& asi) noexcept
            {
                auto& that = static_cast<Protector&>(asi);
                auto& ext = *(that.ext_data_);

                asi.setCancel([](IAsyncSteps& asi) {
                    auto& that = static_cast<Protector&>(asi);
                    auto& ext = *(that.ext_data_);
                    ext.continue_await.store(false, std::memory_order_release);
                });

                ext.await_thread_.reset(new std::thread(
                        [](Protector& asp) {
                            auto& ext = *(asp.ext_data_);
                            auto root_impl = asp.root_->impl_;
                            root_impl->await_thread_id_ =
                                    std::this_thread::get_id();

                            while (ext.continue_await.load(
                                    std::memory_order_consume)) {
                                try {
                                    if (ext.await_func_(asp, AWAIT_DELAY())) {
                                        break;
                                    }
                                } catch (const std::exception& e) {
                                    root_impl->state_.catch_trace(e);
                                    root_impl->handle_error(&asp, e.what());
                                }
                            }
                        },
                        std::ref(that)));
            }
        };

        //---

        class BaseAsyncSteps::ParallelStep final
            : public BaseAsyncSteps::Protector
        {
            friend class BaseAsyncSteps;

        public:
            ParallelStep(BaseAsyncSteps& root, ProtectorData* parent) noexcept :
                Protector(root, parent)
            {
                alloc_ext_data(false);
                data_.func_ = &process_cb;
                on_cancel_ = &cancel_cb;
            }

            ~ParallelStep() noexcept override = default;

            void sanity_check() noexcept
            {
                if (root_ == nullptr) {
                    on_invalid_call("Step got invalidated!");
                }
            }

            Protector* add_substep() noexcept
            {
                auto& items = ext_data_->items_;
                items.emplace_back(root_->state(), root_->impl_->async_tool_);

                auto& sub_asi = items.back();
                auto& sub_data = sub_asi.add_step();

                sub_data.func_ = [](IAsyncSteps& asi) {
                    auto& that = static_cast<ProtectorData&>(asi);
                    --(that.sub_queue_start);
                    --(that.sub_queue_front);
                };
                sub_data.on_error_ = std::ref(*this);

                // actual step
                auto sub_impl = sub_asi.impl_;
                sub_impl->alloc_step(); // completion step
                auto data = sub_impl->alloc_step();
                auto& sub_asi_p = reinterpret_cast<ProtectorData&>(
                        sub_asi.impl_->queue_.front());
                auto step = new (data) Protector(sub_asi, &sub_asi_p);
                return step;
            }

            StepData& add_step() noexcept override
            {
                sanity_check();

                return add_substep()->data_;
            }

            LoopState& add_loop() noexcept override
            {
                sanity_check();

                auto step = add_substep();
                step->data_.func_ = &Protector::loop_handler;

                return step->alloc_ext_data(true);
            }

            IAsyncSteps& parallel(ErrorPass /*on_error*/) noexcept override
            {
                on_invalid_call("parallel() on parallel()");
            }

            NextArgs& nextargs() noexcept override
            {
                on_invalid_call("nextargs() on parallel()");
            }

            void setTimeout(std::chrono::milliseconds /*to*/) noexcept override
            {
                on_invalid_call("setTimeout() on parallel()");
            }

            void setCancel(CancelPass /*cb*/) noexcept override
            {
                on_invalid_call("setCancel() on parallel()");
            }

            void waitExternal() noexcept override
            {
                on_invalid_call("waitExternal() on parallel()");
            }

            // Dirty hack: sub-step completion
            void operator()(IAsyncSteps& /*asi*/) noexcept
            {
                auto& completed = ext_data_->completed_;
                ++completed;

                if (completed == ext_data_->items_.size()) {
                    limit_handle_ = root_->impl_->async_tool_.immediate(
                            std::ref(*this));
                }
            }

            // Dirty hack: sub-step error
            void operator()(IAsyncSteps& asi, ErrorCode err) noexcept
            {
                auto current = static_cast<Protector&>(asi).root_;
                auto& items = ext_data_->items_;

                for (auto& v : items) {
                    if (&v != current) {
                        v.cancel();
                    }
                }

                ext_data_->error_code_ = err;
                limit_handle_ =
                        root_->impl_->async_tool_.immediate(std::ref(*this));
            }

            // Dirty hack: final completion
            void operator()() noexcept
            {
                auto& error_code = ext_data_->error_code_;

                if (error_code.empty()) {
                    root_->impl_->handle_success(this);
                } else {
                    root_->impl_->handle_error(this, error_code.c_str());
                }
            }

        protected:
            static void process_cb(IAsyncSteps& asi)
            {
                auto& that = static_cast<ParallelStep&>(asi);
                ExecPass completion_handler(std::ref(that));

                for (auto& v : that.ext_data_->items_) {
                    // NOTE: See add_substep() for pre-allocation
                    auto& holder = reinterpret_cast<Impl::ProtectorDataHolder&>(
                            v.impl_->queue_[1]);
                    auto step = new (&holder) Protector(v, nullptr);
                    auto& d = step->data_;
                    completion_handler.move(d.func_, d.func_storage_);
                    v.impl_->schedule_exec();
                }
                that.Protector::waitExternal();
            }

            static void cancel_cb(IAsyncSteps& asi)
            {
                auto& that = static_cast<ParallelStep&>(asi);
                auto& ext = that.ext_data_;

                if (ext->error_code_.empty()) {
                    // Not caused by inner error
                    ext->items_.clear();
                }
            }
        };

        //---
        BaseAsyncSteps::ProtectorData::~ProtectorData() noexcept
        {
            limit_handle_.cancel();

            if (ext_data_ != nullptr) {
                ext_data_->~ExtStepState();
                root_->impl_->ext_data_allocator.deallocate(ext_data_, 1);
                ext_data_ = nullptr;
            }

            root_ = nullptr;
        }

        IAsyncSteps& BaseAsyncSteps::Protector::parallel(
                ErrorPass on_error) noexcept
        {
            sanity_check();

            auto buf = root_->impl_->alloc_step();
            auto step = new (buf) ParallelStep(*root_, this);

            auto& data = step->data_;
            on_error.move(data.on_error_, data.on_error_storage_);

            return *step;
        }

        //---

        BaseAsyncSteps::BaseAsyncSteps(
                State& state, IAsyncTool& async_tool) noexcept
        {
            auto& mem_pool = async_tool.mem_pool();
            auto p = IMemPool::Allocator<Impl>(mem_pool).allocate(1);
            impl_ = new (p) Impl(state, async_tool, mem_pool);

            static_assert(
                    sizeof(Protector) == sizeof(ProtectorData),
                    "Invalid fields in Protector");
            static_assert(
                    sizeof(ParallelStep) == sizeof(ProtectorData),
                    "Invalid fields in ParallelStep");
        }

        BaseAsyncSteps::~BaseAsyncSteps() noexcept
        {
            BaseAsyncSteps::cancel();

            if (impl_ != nullptr) {
                impl_->~Impl();
                IMemPool::Allocator<Impl>(impl_->mem_pool_)
                        .deallocate(impl_, 1);
                impl_ = nullptr;
            }
        }

        BaseAsyncSteps::operator bool() const noexcept
        {
            return (impl_->stack_top_ == nullptr) && !impl_->exec_handle_;
        }

        IAsyncSteps::StepData& BaseAsyncSteps::add_step() noexcept
        {
            impl_->sanity_check();

            auto buf = impl_->alloc_step();
            auto step = new (buf) Protector(*this, nullptr);
            return step->data_;
        }

        IAsyncSteps& BaseAsyncSteps::parallel(ErrorPass on_error) noexcept
        {
            impl_->sanity_check();

            auto buf = impl_->alloc_step();
            auto step = new (buf) ParallelStep(*this, nullptr);

            auto& data = step->data_;
            on_error.move(data.on_error_, data.on_error_storage_);

            return *step;
        }

        void BaseAsyncSteps::handle_success() noexcept
        {
            on_invalid_call("success() outside of execute()");
        }

        void BaseAsyncSteps::handle_error(ErrorCode /*code*/)
        {
            on_invalid_call("error() outside of execute()");
        }

        NextArgs& BaseAsyncSteps::nextargs() noexcept
        {
            return impl_->next_args_;
        }

        IAsyncSteps& BaseAsyncSteps::copyFrom(IAsyncSteps& asi) noexcept
        {
            impl_->sanity_check();

            assert(dynamic_cast<BaseAsyncSteps*>(&asi));
            (void) asi;

            on_invalid_call("copyFrom() is not supported in C++");
        }

        void BaseAsyncSteps::setTimeout(
                std::chrono::milliseconds /*to*/) noexcept
        {
            on_invalid_call("setTimeout() outside execute()");
        }

        void BaseAsyncSteps::setCancel(CancelPass /*cb*/) noexcept
        {
            on_invalid_call("setCancel() outside execute()");
        }

        void BaseAsyncSteps::waitExternal() noexcept
        {
            on_invalid_call("waitExternal() outside execute()");
        }

        void BaseAsyncSteps::execute() noexcept
        {
            impl_->sanity_check();
            impl_->schedule_exec();
        }

        void BaseAsyncSteps::cancel() noexcept
        {
            impl_->handle_cancel();
        }

        LoopState& BaseAsyncSteps::add_loop() noexcept
        {
            impl_->sanity_check();

            auto buf = impl_->alloc_step();
            auto step = new (buf) Protector(*this, nullptr);

            step->data_.func_ = &Protector::loop_handler;

            return step->alloc_ext_data(true);
        }

        std::unique_ptr<IAsyncSteps> BaseAsyncSteps::newInstance() noexcept
        {
            return std::unique_ptr<IAsyncSteps>(
                    new ri::AsyncSteps(impl_->async_tool_));
        }

        //---
        void BaseAsyncSteps::Impl::schedule_exec() noexcept
        {
            if (exec_handle_) {
                on_invalid_call("AsyncSteps instance is already executed.");
            }

            exec_handle_ = async_tool_.immediate(std::ref(*this));
        }

        void BaseAsyncSteps::Impl::execute_handler() noexcept
        {
            exec_handle_.reset();
            ProtectorDataHolder* next_data = nullptr;

            while (stack_top_ != nullptr) {
                auto current = stack_top_;

                if (is_sub_queue_empty(current)) {
                    stack_top_ = current->parent_;
                    sub_queue_free(current);
                } else {
                    next_data = &(queue_[current->sub_queue_front]);
                    break;
                }
            }

            if (next_data == nullptr) {
                if (queue_.empty()) {
                    return;
                }

                next_data = &(queue_.front());
            }

            auto next = reinterpret_cast<ProtectorData*>(next_data);

            const auto qs = queue_.size();
            next->sub_queue_start = qs;
            next->sub_queue_front = qs;

            stack_top_ = next;

            try {
                in_exec_ = true;
                next->data_.func_(*next);

                if (stack_top_ != next) {
                    // pass
                } else if (!is_sub_queue_empty(next)) {
                    schedule_exec();
                } else if (!next->on_cancel_ && !next->limit_handle_) {
                    next->handle_success();
                }

                in_exec_ = false;
            } catch (const std::exception& e) {
                in_exec_ = false;
                state_.catch_trace(e);
                next->handle_error(e.what());
            }
        }

        void BaseAsyncSteps::Impl::handle_success(
                ProtectorData* current) noexcept
        {
            if (!is_safe_to_process()) {
                std::promise<void> done;
                auto task = [this, current, &done]() {
                    this->handle_success(current);
                    done.set_value();
                };
                async_tool_.immediate(std::ref(task));
                done.get_future().wait();
                return;
            }

            if (!is_sub_queue_empty(current)) {
                on_invalid_call("success() with sub-steps");
            }

            stack_top_ = stack_top_->parent_;

            while (stack_top_ != nullptr) {
                current = stack_top_;

                cond_sub_queue_shift(current);

                if (!is_sub_queue_empty(current)) {
                    schedule_exec();
                    return;
                }

                stack_top_ = current->parent_;
                sub_queue_free(current);
            }

            // Got to root queue
            cond_queue_shift();

            if (!queue_.empty()) {
                schedule_exec();
            }
        }

        void BaseAsyncSteps::Impl::handle_error(
                ProtectorData* current, ErrorCode code) noexcept
        {
            if (!is_safe_to_process()) {
                std::promise<void> done;
                auto task = [this, current, code, &done]() {
                    this->handle_error(current, code);
                    done.set_value();
                };
                async_tool_.immediate(std::ref(task));
                done.get_future().wait();
                return;
            }

            if (exec_handle_) {
                // Out-of-sequence error
                handle_cancel();
                return;
            }

            if (in_exec_) {
                // avoid double handling
                return;
            }

            if (current != stack_top_) {
                on_invalid_call("error() out of order");
            }

            futoin::string code_cache{
                    futoin::string::allocator_type(mem_pool_)};

            for (;;) {
                sub_queue_free(current);
                current->sub_queue_front = current->sub_queue_start;

                current->limit_handle_.cancel();

                auto& on_cancel = current->on_cancel_;

                if (on_cancel) {
                    on_cancel(*current);
                    current->on_cancel_ = nullptr;
                }

                ErrorHandler on_error{std::move(current->data_.on_error_)};

                if (on_error) {
                    try {
                        in_exec_ = true;
                        on_error(*current, code);
                        in_exec_ = false;

                        if (stack_top_ != current) {
                            // success() was called
                            return;
                        }

                        if (!is_sub_queue_empty(current)) {
                            schedule_exec();
                            return;
                        }
                    } catch (const std::exception& e) {
                        in_exec_ = false;
                        state_.catch_trace(e);
                        code_cache = e.what();
                        code = code_cache.c_str();
                    }
                }

                stack_top_ = current->parent_;

                if (stack_top_ == nullptr) {
                    break;
                }

                current = stack_top_;
            }

            clear_queue();

            if (state_.unhandled_error) {
                state_.unhandled_error(code);
            } else {
                std::cout << "FATAL: unhandled AsyncStep error " << code
                          << std::endl;
                std::terminate();
            }
        }

        void BaseAsyncSteps::Impl::handle_cancel() noexcept
        {
            if (async_tool_.is_same_thread() || queue_.empty()) {
                if (in_exec_) {
                    on_invalid_call("cancel() inside execution");
                }

                exec_handle_.cancel();

                while (stack_top_ != nullptr) {
                    auto current = stack_top_;
                    current->limit_handle_.cancel();

                    auto& on_cancel = current->on_cancel_;

                    if (on_cancel) {
                        on_cancel(*current);
                        current->on_cancel_ = nullptr;
                    }

                    stack_top_ = current->parent_;
                }

                clear_queue();
            } else {
                std::promise<void> done;
                auto task = [this, &done]() {
                    this->handle_cancel();
                    done.set_value();
                };
                async_tool_.immediate(std::ref(task));
                done.get_future().wait();
            }
        }

        IAsyncSteps::SyncRootID BaseAsyncSteps::sync_root_id() const
        {
            return reinterpret_cast<SyncRootID>(this);
        }

        void BaseAsyncSteps::await_impl(AwaitPass awp) noexcept
        {
            impl_->sanity_check();

            auto buf = impl_->alloc_step();
            auto step = new (buf) Protector(*this, nullptr);

            step->data_.func_ = &Protector::await_handler;

            auto& ext = step->alloc_ext_data(false);
            awp.move(ext.await_func_, ext.await_storage_);
        }

        State& BaseAsyncSteps::state() noexcept
        {
            return impl_->state_;
        }

        //---
        AsyncSteps::AsyncSteps(IAsyncTool& at) noexcept :
            BaseAsyncSteps(state_, at),
            state_(at.mem_pool())
        {}

        struct BaseAsyncSteps::AllocOptimizer
        {
            IMemPool::Allocator<futoin::any>::EnsureOptimized any;
            IMemPool::Allocator<BaseAsyncSteps::Impl>::EnsureOptimized impl;
            IMemPool::Allocator<BaseAsyncSteps::ExtStepState>::EnsureOptimized
                    ext_state;
        };
        BaseAsyncSteps::AllocOptimizer BaseAsyncSteps::alloc_optimizer;
    } // namespace ri
} // namespace futoin
