//-----------------------------------------------------------------------------
//   Copyright 2023 FutoIn Project
//   Copyright 2023 Andrey Galkin
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

#include <functional>

#include <futoin/fatalmsg.hpp>
#include <futoin/ri/binaryapi.hpp>

namespace futoin {
    namespace ri {
        using UniqueAsyncPtr = std::unique_ptr<IAsyncSteps>;

        // --------------------------------------------------------------------
        // AsyncSteps C/Binary API implementation
        // --------------------------------------------------------------------
        namespace details {
            struct IAsyncStepsAccessor : IAsyncSteps
            {
                using IAsyncSteps::handle_error;
                using IAsyncSteps::nextargs;
            };
            struct BinarySyncWrapper : ISync
            {
                BinarySyncWrapper(FutoInSync& orig) : orig_(orig) {}

                void lock(IAsyncSteps& asi) override
                {
                    orig_.api->lock(&(asi.binary()), &orig_);
                }

                void unlock(IAsyncSteps& asi) noexcept override
                {
                    orig_.api->unlock(&(asi.binary()), &orig_);
                }

                FutoInSync& orig_;
            };

        } // namespace details

        // NOLINTNEXTLINE(cert-err58-cpp)
        const ::FutoInAsyncStepsAPI binary_steps_api{{{
                // Index 0 - add
                [](FutoInAsyncSteps* bsi,
                   void* data,
                   FutoInAsyncSteps_execute_callback f,
                   FutoInAsyncSteps_error_callback eh) -> void {
                    static_cast<BinarySteps*>(bsi)->asi.add(
                            [data, f](IAsyncSteps& asi) {
                                FutoInArgs args{};
                                static_cast<details::IAsyncStepsAccessor&>(asi)
                                        .nextargs()
                                        .moveTo(args);
                                auto& bsi = static_cast<BinarySteps&>(asi.binary());
                                bsi.before_call();
                                f(&bsi, data, &args);
                                bsi.after_call();
                            },
                            (eh != nullptr) ? [data, eh](IAsyncSteps& asi, ErrorCode code) {
                                auto& bsi = static_cast<BinarySteps&>(asi.binary());
                                bsi.before_call();
                                eh(&bsi, data, code);
                                bsi.after_call();
                            } : asyncsteps::ErrorPass{});
                },
                // Index 1 - parallel
                [](FutoInAsyncSteps* bsi,
                   void* data,
                   FutoInAsyncSteps_error_callback eh) -> FutoInAsyncSteps* {
                    auto& res = static_cast<BinarySteps*>(bsi)->asi.parallel(
                            (eh != nullptr) ? [data, eh](IAsyncSteps& asi, ErrorCode code) {
                                auto& bsi = static_cast<BinarySteps&>(asi.binary());
                                bsi.before_call();
                                eh(&bsi, data, code);
                                bsi.after_call();
                            } : asyncsteps::ErrorPass{});
                    return &(res.binary());
                },
                // Index 2 - stateVariable
                [](FutoInAsyncSteps* bsi,
                   void* data,
                   const char* name,
                   void* (*allocate)(void* data),
                   void (*cleanup)(void* data, void* value)) -> void* {
                    // NOTE: this may lead into futoin::any inside
                    // std::unique_ptr inside futoin::any,
                    //       but this is how it is...
                    using UPtr =
                            std::unique_ptr<void, std::function<void(void*)>>;
                    auto& av =
                            static_cast<BinarySteps*>(bsi)->asi.state()[name];

                    // NOTE: state is not thread-safe - no race
                    if (!av.has_value()) {
                        av = UPtr{allocate(data), [data, cleanup](void* value) {
                                      cleanup(data, value);
                                  }};
                    }

                    try {
                        return any_cast<UPtr&>(av).get();
                    } catch (const std::bad_cast& e) {
                        static_cast<BinarySteps*>(bsi)->asi.state().catch_trace(
                                e);
                        return nullptr;
                    }
                },
                // Index 3 - stack
                [](FutoInAsyncSteps* bsi,
                   size_t data_size,
                   void (*cleanup)(void* value)) -> void* {
                    return static_cast<BinarySteps*>(bsi)->asi.stack(
                            data_size, cleanup);
                },
                // Index 4 - success
                [](FutoInAsyncSteps* bsi, FutoInArgs* args) -> void {
                    auto& wbsi = *static_cast<BinarySteps*>(bsi);
                    auto& asi = wbsi.asi;

                    if (!asi.tool().is_same_thread()) {
                        std::promise<void> done;
                        auto task = [bsi, args, &done]() {
                            bsi->api->success(bsi, args);
                            done.set_value();
                        };
                        asi.tool().immediate(std::ref(task));
                        done.get_future().wait();
                        return;
                    }

                    if (!wbsi.parallel_) {
                        static_cast<details::IAsyncStepsAccessor&>(asi)
                                .nextargs()
                                .moveFrom(*args);
                    }

                    if (wbsi.waiting_) {
                        asi.success();
                    } else {
                        wbsi.succeeded_ = true;
                    }
                },
                // Index 5 - handle_error
                [](FutoInAsyncSteps* bsi, const char* code, const char* info)
                        -> void {
                    auto& wbsi = *static_cast<BinarySteps*>(bsi);
                    auto& asi = wbsi.asi;

                    if (!asi.tool().is_same_thread()) {
                        std::promise<void> done;
                        auto task = [bsi, code, info, &done]() {
                            bsi->api->handle_error(bsi, code, info);
                            done.set_value();
                        };
                        asi.tool().immediate(std::ref(task));
                        done.get_future().wait();
                        return;
                    }

                    if (wbsi.waiting_) {
                        try {
                            asi.error(code, info);
                        } catch (const futoin::Error&) {
                            // pass
                        }
                    } else {
                        wbsi.last_error_ = code;
                        wbsi.last_error_info_ = info;
                    }
                },
                // Index 6 - setTimeout
                [](FutoInAsyncSteps* bsi, uint32_t timeout_ms) -> void {
                    static_cast<BinarySteps*>(bsi)->asi.setTimeout(
                            std::chrono::milliseconds{timeout_ms});
                },
                // Index 7 - setCancel
                [](FutoInAsyncSteps* bsi,
                   void* data,
                   FutoInAsyncSteps_cancel_callback ch) -> void {
                    static_cast<BinarySteps*>(bsi)->asi.setCancel(
                            [data, ch](IAsyncSteps& asi) {
                                ch(&(asi.binary()), data);
                            });
                },
                // Index 8 - waitExternal
                [](FutoInAsyncSteps* bsi) -> void {
                    static_cast<BinarySteps*>(bsi)->asi.waitExternal();
                },
                // Index 9 - loop
                [](FutoInAsyncSteps* bsi,
                   void* data,
                   void (*f)(FutoInAsyncSteps* bsi, void* data),
                   const char* label) -> void {
                    static_cast<BinarySteps*>(bsi)->asi.loop(
                            [data, f](IAsyncSteps& asi) {
                                auto& bsi =
                                        static_cast<BinarySteps&>(asi.binary());
                                bsi.before_call();
                                f(&bsi, data);
                                bsi.after_call();
                            },
                            label);
                },
                // Index 10 - repeat
                [](FutoInAsyncSteps* bsi,
                   void* data,
                   size_t count,
                   void (*f)(FutoInAsyncSteps* bsi, void* data, size_t i),
                   const char* label) -> void {
                    static_cast<BinarySteps*>(bsi)->asi.repeat(
                            count,
                            [data, f](IAsyncSteps& asi, size_t i) {
                                auto& bsi =
                                        static_cast<BinarySteps&>(asi.binary());
                                bsi.before_call();
                                f(&bsi, data, i);
                                bsi.after_call();
                            },
                            label);
                },
                // Index 11 - breakLoop
                [](FutoInAsyncSteps* bsi, const char* label) -> void {
                    try {
                        static_cast<BinarySteps*>(bsi)->asi.breakLoop(label);
                    } catch (const futoin::Error& e) {
                        static_cast<BinarySteps*>(bsi)->last_error_ = e.what();
                    }
                },
                // Index 12 - continueLoop
                [](FutoInAsyncSteps* bsi, const char* label) -> void {
                    try {
                        static_cast<BinarySteps*>(bsi)->asi.continueLoop(label);
                    } catch (const futoin::Error& e) {
                        static_cast<BinarySteps*>(bsi)->last_error_ = e.what();
                    }
                },
                // Index 13 - execute
                [](FutoInAsyncSteps* bsi,
                   void* data,
                   FutoInAsyncSteps_error_callback unhandled_error) -> void {
                    auto& asi = static_cast<BinarySteps*>(bsi)->asi;

                    if (unhandled_error != nullptr) {
                        asi.state().unhandled_error =
                                [bsi, data, unhandled_error](ErrorCode code) {
                                    unhandled_error(bsi, data, code);
                                };
                    }

                    asi.execute();
                },
                // Index 14 - cancel
                [](FutoInAsyncSteps* bsi) -> void {
                    static_cast<BinarySteps*>(bsi)->asi.cancel();
                },
                // Index 15 - addSync
                [](FutoInAsyncSteps* bsi,
                   FutoInSync* sync,
                   void* data,
                   FutoInAsyncSteps_execute_callback f,
                   FutoInAsyncSteps_error_callback eh) -> void {
                    auto& asi = static_cast<BinarySteps*>(bsi)->asi;
                    ISync* ws;

                    if (sync->api == &binary_sync_api) {
                        ws = static_cast<ISync*>(sync);
                    } else {
                        ws = &(asi.stack<details::BinarySyncWrapper>(*sync));
                    }

                    asi.sync(
                            *ws,
                            [data, f](IAsyncSteps& asi) {
                                FutoInArgs args{};
                                static_cast<details::IAsyncStepsAccessor&>(asi)
                                        .nextargs()
                                        .moveTo(args);
                                auto& bsi = static_cast<BinarySteps&>(asi.binary());
                                bsi.before_call();
                                f(&bsi, data, &args);
                                bsi.after_call();
                            },
                            (eh != nullptr) ? [data, eh](IAsyncSteps& asi, ErrorCode code) {
                                auto& bsi = static_cast<BinarySteps&>(asi.binary());
                                bsi.before_call();
                                eh(&bsi, data, code);
                                bsi.after_call();
                            } : asyncsteps::ErrorPass{});
                },
                // Index 16 - rootId
                [](FutoInAsyncSteps* bsi) -> std::ptrdiff_t {
                    return static_cast<BinarySteps*>(bsi)->asi.sync_root_id();
                },
                // Index 17 - isValid
                [](FutoInAsyncSteps* bsi) -> int {
                    return int(bool(static_cast<BinarySteps*>(bsi)->asi));
                },
                // Index 18 - newInstance
                [](FutoInAsyncSteps* bsi) -> FutoInAsyncSteps* {
                    return new BinarySteps(static_cast<BinarySteps*>(bsi)
                                                   ->asi.newInstance()
                                                   .release());
                },
                // Index 19 - free
                [](FutoInAsyncSteps* bsi) -> void {
                    delete static_cast<BinarySteps*>(bsi);
                },
                // Index 20 - sched_immediate
                [](FutoInAsyncSteps* bsi, void* data, void (*cb)(void* data))
                        -> FutoInHandle {
                    return static_cast<BinarySteps*>(bsi)
                            ->asi.tool()
                            .immediate([data, cb]() { cb(data); })
                            .binary();
                },
                // Index 21 - sched_deferred
                [](FutoInAsyncSteps* bsi,
                   uint32_t delay_ms,
                   void* data,
                   void (*cb)(void* data)) -> FutoInHandle {
                    return static_cast<BinarySteps*>(bsi)
                            ->asi.tool()
                            .deferred(
                                    std::chrono::milliseconds{delay_ms},
                                    [data, cb]() { cb(data); })
                            .binary();
                },
                // Index 22 - sched_cancel
                [](FutoInAsyncSteps* /*bsi*/, FutoInHandle* handle) -> void {
                    IAsyncTool::Handle(*handle).cancel();
                },
                // Index 23 - sched_is_valid
                [](FutoInAsyncSteps* /*bsi*/, FutoInHandle* handle) -> int {
                    return int(bool(IAsyncTool::Handle(*handle)));
                },
                // Index 24 - is_same_thread
                [](FutoInAsyncSteps* bsi) -> int {
                    return int(static_cast<BinarySteps*>(bsi)
                                       ->asi.tool()
                                       .is_same_thread());
                },
        }}};

        // NOLINTNEXTLINE(cert-err58-cpp)
        const ::FutoInSyncAPI binary_sync_api{{{
                // Index 0 - lock
                [](FutoInAsyncSteps* bsi, FutoInSync* sync) {
                    IAsyncSteps* asi;
                    try {
                        auto& isync = static_cast<ISync&>(*sync);

                        if (bsi->api == &binary_steps_api) {
                            asi = &static_cast<BinarySteps*>(bsi)->asi;
                            isync.lock(*asi);
                        } else {
                            auto wasi = wrap_binary_steps(*bsi);
                            asi = wasi.get();
                            isync.lock(*asi);
                            asi->stack<UniqueAsyncPtr>().swap(wasi);
                        }
                    } catch (const futoin::ExtError& e) {
                        auto& s = asi->state();
                        s.catch_trace(e);
                        s.error_info = e.error_info();
                        static_cast<details::IAsyncStepsAccessor*>(asi)
                                ->handle_error(e.what());
                    } catch (const std::exception& e) {
                        asi->state().catch_trace(e);
                        static_cast<details::IAsyncStepsAccessor*>(asi)
                                ->handle_error(e.what());
                    }
                },
                // Index 1 - unlock
                [](FutoInAsyncSteps* bsi, FutoInSync* sync) {
                    auto& isync = static_cast<ISync&>(*sync);

                    if (bsi->api == &binary_steps_api) {
                        isync.unlock(static_cast<BinarySteps*>(bsi)->asi);
                    } else {
                        auto asi = wrap_binary_steps(*bsi);
                        isync.unlock(*asi);
                        asi->stack<UniqueAsyncPtr>().swap(asi);
                    }
                },
        }}};

        // --------------------------------------------------------------------
        // Wrapper for AsyncSteps C++ interface
        // --------------------------------------------------------------------
        static asyncsteps::BaseState& handle_wrap_state(
                IAsyncSteps& asi, FutoInAsyncSteps* bsi)
        {
            return (bsi->api == &binary_steps_api)
                           ? static_cast<BinarySteps*>(bsi)->asi.state()
                           : asi.state();
        }
        static void handle_wrap_error(
                IAsyncSteps& asi,
                FutoInAsyncSteps* bsi,
                const std::exception& e)
        {
            handle_wrap_state(asi, bsi).catch_trace(e);
            static_cast<details::IAsyncStepsAccessor&>(asi).handle_error(
                    e.what());
        }
#define WRAP_EXC(expr)                                            \
    try {                                                         \
        expr;                                                     \
    } catch (const futoin::ExtError& e) {                         \
        handle_wrap_state(*asi, bsi).error_info = e.error_info(); \
        handle_wrap_error(*asi, bsi, e);                          \
    } catch (const std::exception& e) {                           \
        handle_wrap_error(*asi, bsi, e);                          \
    }

        struct BinaryStepsWrapper final : IAsyncSteps
        {
            struct BinaryState : asyncsteps::BaseState
            {
                explicit BinaryState(FutoInAsyncSteps& binary_steps) noexcept :
                    BaseState(IMemPool::Allocator<futoin::any>::get_default()),
                    binary_steps_(binary_steps)
                {}

                mapped_type& operator[](const key_type& key) noexcept override
                {
                    void* data = binary_steps_.api->stateVariable(
                            &binary_steps_,
                            &mem_pool(),
                            key.c_str(),
                            [](void* data) -> void* {
                                auto& mem_pool =
                                        *reinterpret_cast<IMemPool*>(data);
                                IMemPool::Allocator<futoin::any> allocator(
                                        mem_pool);
                                futoin::any* ptr = allocator.allocate(1);
                                allocator.construct(ptr);
                                return ptr;
                            },
                            [](void* data, void* vptr) {
                                auto& mem_pool =
                                        *reinterpret_cast<IMemPool*>(data);
                                IMemPool::Allocator<futoin::any> allocator(
                                        mem_pool);
                                auto* ptr =
                                        reinterpret_cast<futoin::any*>(vptr);
                                allocator.destroy(ptr);
                                allocator.deallocate(ptr, 1);
                            });
                    return *reinterpret_cast<mapped_type*>(data);
                }

                mapped_type& operator[](key_type&& key) noexcept override
                {
                    return this->operator[](static_cast<const key_type&>(key));
                }

                FutoInAsyncSteps& binary_steps_;
            };

            struct BinaryTool : IAsyncTool
            {
                explicit BinaryTool(FutoInAsyncSteps& binary_steps) noexcept :
                    binary_steps_(binary_steps)
                {}

                struct CallbackData
                {
                    IMemPool* mem_pool;
                    Callback callback;
                    CallbackPass::Storage storage;
                };

                Handle immediate(CallbackPass&& cb) noexcept override
                {
                    IMemPool& mem_pool =
                            IMemPool::Allocator<CallbackData>::get_default();
                    IMemPool::Allocator<CallbackData> allocator(mem_pool);

                    auto* ptr = allocator.allocate(1);
                    ptr->mem_pool = &mem_pool;
                    cb.move(ptr->callback, ptr->storage);

                    Handle ret = binary_steps_.api->sched_immediate(
                            &binary_steps_, ptr, [](void* data) {
                                auto* ptr =
                                        reinterpret_cast<CallbackData*>(data);
                                ptr->callback();
                            });
                    binary_steps_.api->sched_immediate(
                            &binary_steps_, ptr, [](void* data) {
                                auto* ptr =
                                        reinterpret_cast<CallbackData*>(data);
                                IMemPool::Allocator<CallbackData> allocator(
                                        *(ptr->mem_pool));
                                allocator.deallocate(ptr, 1);
                            });
                    return ret;
                }

                Handle deferred(
                        std::chrono::milliseconds delay,
                        CallbackPass&& cb) noexcept override
                {
                    IMemPool& mem_pool =
                            IMemPool::Allocator<CallbackData>::get_default();
                    IMemPool::Allocator<CallbackData> allocator(mem_pool);

                    auto* ptr = allocator.allocate(1);
                    ptr->mem_pool = &mem_pool;
                    cb.move(ptr->callback, ptr->storage);

                    Handle ret = binary_steps_.api->sched_deferred(
                            &binary_steps_, delay.count(), ptr, [](void* data) {
                                auto* ptr =
                                        reinterpret_cast<CallbackData*>(data);
                                ptr->callback();
                            });
                    binary_steps_.api->sched_deferred(
                            &binary_steps_,
                            delay.count() + 1,
                            ptr,
                            [](void* data) {
                                auto* ptr =
                                        reinterpret_cast<CallbackData*>(data);
                                IMemPool::Allocator<CallbackData> allocator(
                                        *(ptr->mem_pool));
                                allocator.deallocate(ptr, 1);
                            });
                    return ret;
                }

                bool is_same_thread() noexcept override
                {
                    return bool(
                            binary_steps_.api->is_same_thread(&binary_steps_));
                }

                void cancel(Handle& h) noexcept override
                {
                    auto bh = h.binary();
                    binary_steps_.api->sched_cancel(&binary_steps_, &bh);
                    h.reset();
                }

                bool is_valid(Handle& h) noexcept override
                {
                    auto bh = h.binary();
                    return bool(binary_steps_.api->sched_is_valid(
                            &binary_steps_, &bh));
                }

                CycleResult iterate() noexcept override
                {
                    return {false, {}};
                }

                IMemPool& mem_pool(
                        std::size_t /*object_size*/,
                        bool /*optimize*/) noexcept override
                {
                    return GlobalMemPool::get_default();
                }

                void release_memory() noexcept override {}

                FutoInAsyncSteps& binary_steps_;
            };

            BinaryStepsWrapper(FutoInAsyncSteps& binary_steps) :
                binary_steps_(binary_steps), manage_(false)
            {}

            BinaryStepsWrapper(FutoInAsyncSteps* p_binary_steps) :
                binary_steps_(*p_binary_steps), manage_(true)
            {}

            ~BinaryStepsWrapper() override
            {
                if (manage_) {
                    binary_steps_.api->free(&binary_steps_);
                }
            }

            IAsyncSteps& parallel(ErrorPass on_error = {}) noexcept override
            {
                auto& step_data = stack<StepData>();
                on_error.move(step_data.on_error_, step_data.on_error_storage_);

                FutoInAsyncSteps* other = binary_steps_.api->parallel(
                        &binary_steps_,
                        &step_data,
                        [](FutoInAsyncSteps* bsi,
                           void* data,
                           const char* code) {
                            auto sd = reinterpret_cast<StepData*>(data);

                            if (sd->on_error_) {
                                auto asi = wrap_binary_steps(*bsi);
                                WRAP_EXC(sd->on_error_(*asi, code));
                            }
                        });

                return stack<BinaryStepsWrapper>(*other);
            }

            BaseState& state() noexcept override
            {
                return state_;
            }

            [[noreturn]] IAsyncSteps& copyFrom(
                    IAsyncSteps& /*asi*/) noexcept override
            {
                FatalMsg() << "copyFrom() is not supported";
            }

            SyncRootID sync_root_id() const override
            {
                return binary_steps_.api->rootId(&binary_steps_);
            }

            UniqueAsyncPtr newInstance() noexcept override
            {
                return UniqueAsyncPtr{new BinaryStepsWrapper(
                        binary_steps_.api->newInstance(&binary_steps_))};
            }

            using IAsyncSteps::stack;
            void* stack(
                    std::size_t object_size,
                    StackDestroyHandler destroy_cb) noexcept override
            {
                return binary_steps_.api->stack(
                        &binary_steps_, object_size, destroy_cb);
            }

            FutoInAsyncSteps& binary() noexcept override
            {
                return binary_steps_;
            }

            UniqueAsyncPtr wrap(
                    FutoInAsyncSteps& binary_steps) noexcept override
            {
                return wrap_binary_steps(binary_steps);
            }

            IAsyncTool& tool() noexcept override
            {
                return async_tool_;
            }

            void setTimeout(std::chrono::milliseconds to) noexcept override
            {
                delayed_ = true;
                binary_steps_.api->setTimeout(&binary_steps_, to.count());
            }

            struct CancelCallbackHolder
            {
                CancelPass::Storage storage;
                CancelPass::Function func;
            };

            void setCancel(CancelPass cb) noexcept override
            {
                delayed_ = true;
                auto& cancel_data = stack<CancelCallbackHolder>();
                cb.move(cancel_data.func, cancel_data.storage);
                binary_steps_.api->setCancel(
                        &binary_steps_,
                        &cancel_data,
                        [](FutoInAsyncSteps* bsi, void* data) {
                            auto asi = wrap_binary_steps(*bsi);
                            auto cd = reinterpret_cast<CancelCallbackHolder*>(
                                    data);
                            WRAP_EXC(cd->func(*asi));
                        });
            }

            void waitExternal() noexcept override
            {
                delayed_ = true;
                binary_steps_.api->waitExternal(&binary_steps_);
            }

            void execute() noexcept override
            {
                auto& unhandled_error = state().unhandled_error;
                FutoInAsyncSteps_error_callback error_handler = nullptr;

                if (unhandled_error) {
                    error_handler = [](FutoInAsyncSteps*,
                                       void* data,
                                       RawErrorCode code) {
                        auto& ue =
                                *static_cast<BaseState::UnhandledError*>(data);
                        ue(code);
                    };
                }

                binary_steps_.api->execute(
                        &binary_steps_, &unhandled_error, error_handler);
            }

            void cancel() noexcept override
            {
                binary_steps_.api->cancel(&binary_steps_);
            }

            operator bool() const noexcept override
            {
                return binary_steps_.api->isValid(&binary_steps_) != 0;
            }

            // protected
            void handle_success() override
            {
                FutoInArgs args{};
                next_args_.moveTo(args);
                binary_steps_.api->success(&binary_steps_, &args);
            }
            void handle_error(ErrorCode error_code) override
            {
                if (error_code == errors::LoopBreak) {
                    binary_steps_.api->breakLoop(
                            &binary_steps_, state().error_loop_label);
                } else if (error_code == errors::LoopCont) {
                    binary_steps_.api->continueLoop(
                            &binary_steps_, state().error_loop_label);
                } else {
                    binary_steps_.api->handle_error(
                            &binary_steps_,
                            error_code,
                            state().error_info.c_str());
                }
            }
            asyncsteps::NextArgs& nextargs() noexcept override
            {
                return next_args_;
            }
            asyncsteps::LoopState& add_loop(
                    asyncsteps::LoopLabel label) noexcept override
            {
                auto& loop_data = stack<asyncsteps::LoopState>();
                binary_steps_.api->loop(
                        &binary_steps_,
                        &loop_data,
                        [](FutoInAsyncSteps_* bsi, void* data) {
                            auto asi = wrap_binary_steps(*bsi);
                            auto ls = reinterpret_cast<asyncsteps::LoopState*>(
                                    data);

                            if (!ls->cond || ls->cond(*ls)) {
                                WRAP_EXC(ls->handler(*ls, *asi));

                                if (static_cast<BinaryStepsWrapper&>(*asi)
                                            .delayed_) {
                                    asi->stack<UniqueAsyncPtr>().swap(asi);
                                }
                            } else {
                                bsi->api->breakLoop(bsi, nullptr);
                            }
                        },
                        label);
                return loop_data;
            }
            StepData& add_step() override
            {
                auto& step_data = stack<StepData>();
                binary_steps_.api->add(
                        &binary_steps_,
                        &step_data,
                        [](FutoInAsyncSteps* bsi,
                           void* data,
                           const FutoInArgs* args) {
                            auto asi = wrap_binary_steps(*bsi);
                            auto& wasi = static_cast<BinaryStepsWrapper&>(*asi);
                            auto sd = reinterpret_cast<StepData*>(data);
                            wasi.next_args_.moveFrom(
                                    const_cast<FutoInArgs&>(*args));

                            WRAP_EXC(sd->func_(*asi));

                            if (wasi.delayed_) {
                                asi->stack<UniqueAsyncPtr>().swap(asi);
                            }
                        },
                        [](FutoInAsyncSteps* bsi,
                           void* data,
                           const char* code) {
                            auto sd = reinterpret_cast<StepData*>(data);
                            if (sd->on_error_) {
                                auto asi = wrap_binary_steps(*bsi);
                                WRAP_EXC(sd->on_error_(*asi, code));
                            }
                        });
                return step_data;
            }
            StepData& add_sync(ISync& sync) noexcept override
            {
                auto& step_data = stack<StepData>();
                binary_steps_.api->addSync(
                        &binary_steps_,
                        &sync,
                        &step_data,
                        [](FutoInAsyncSteps* bsi,
                           void* data,
                           const FutoInArgs* args) {
                            auto asi = wrap_binary_steps(*bsi);
                            auto& wasi = static_cast<BinaryStepsWrapper&>(*asi);
                            auto sd = reinterpret_cast<StepData*>(data);
                            wasi.next_args_.moveFrom(
                                    const_cast<FutoInArgs&>(*args));
                            WRAP_EXC(sd->func_(*asi));

                            if (wasi.delayed_) {
                                asi->stack<UniqueAsyncPtr>().swap(asi);
                            }
                        },
                        [](FutoInAsyncSteps* bsi,
                           void* data,
                           const char* code) {
                            auto sd = reinterpret_cast<StepData*>(data);
                            if (sd->on_error_) {
                                auto asi = wrap_binary_steps(*bsi);
                                WRAP_EXC(sd->on_error_(*asi, code));
                            }
                        });
                return step_data;
            }
            struct AwaitData
            {
                ExecPass::Storage func_orig_storage_;
                asyncsteps::AwaitCallback await_func_;
            };
            void await_impl(AwaitPass awp) noexcept override
            {
                auto& await_data = stack<AwaitData>();
                awp.move(await_data.await_func_, await_data.func_orig_storage_);

                binary_steps_.api->loop(
                        &binary_steps_,
                        &await_data,
                        [](FutoInAsyncSteps* bsi, void* data) {
                            auto asi = wrap_binary_steps(*bsi);
                            auto ad = reinterpret_cast<AwaitData*>(data);

                            WRAP_EXC(if (ad->await_func_(*asi, {}, true)) {
                                bsi->api->breakLoop(bsi, nullptr);
                            })
                        },
                        nullptr);
            }

            FutoInAsyncSteps& binary_steps_;
            asyncsteps::NextArgs next_args_;
            BinaryState state_{binary_steps_};
            BinaryTool async_tool_{binary_steps_};
            bool manage_;
            bool delayed_{false};
        };

        // --------------------------------------------------------------------
        UniqueAsyncPtr wrap_binary_steps(FutoInAsyncSteps& binary_steps)
        {
            return UniqueAsyncPtr{new BinaryStepsWrapper(binary_steps)};
        }
        void init_binary_sync(FutoInSync& sync)
        {
            const_cast<const FutoInSyncAPI*&>(sync.api) = &binary_sync_api;
        }

        // --------------------------------------------------------------------
        BinarySteps::BinarySteps(IAsyncSteps* asi) :
            FutoInAsyncSteps{&binary_steps_api}, asi(*asi), managed_(true)
        {}
        BinarySteps::BinarySteps(IAsyncSteps& asi) :
            FutoInAsyncSteps{&binary_steps_api}, asi(asi), managed_(false)
        {}
        BinarySteps::~BinarySteps()
        {
            if (managed_) {
                delete &asi;
            }
        }
    } // namespace ri
} // namespace futoin
