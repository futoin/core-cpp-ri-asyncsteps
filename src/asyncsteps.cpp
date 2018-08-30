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
        class SubAsyncSteps final : public BaseAsyncSteps
        {
        public:
            SubAsyncSteps(State& state, IAsyncTool& async_tool) noexcept :
                BaseAsyncSteps(state, async_tool),
                state_(state)
            {}

            State& state() noexcept override
            {
                return state_;
            }

        private:
            State& state_;
        };

        //---
        struct BaseAsyncSteps::ExtStepState : asyncsteps::LoopState
        {
            ExtStepState(IMemPool& mem_pool, bool is_loop) :
                continue_loop(is_loop),
                items_{ParallelItems::allocator_type(mem_pool)},
                error_code_{futoin::string::allocator_type(mem_pool)}
            {}

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
        };

        class BaseAsyncSteps::ProtectorData : public IAsyncSteps
        {
            friend BaseAsyncSteps;
            friend BaseAsyncSteps::Impl;

        public:
            ProtectorData(BaseAsyncSteps& root) noexcept : root_(&root) {}
            ~ProtectorData() noexcept override;

        protected:
            BaseAsyncSteps* root_;

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

            Impl(State& state, IAsyncTool& async_tool, IMemPool& mem_pool) :
                async_tool_(async_tool),
                mem_pool_(mem_pool),
                queue_{Queue::allocator_type(mem_pool, true)},
                stack_{Stack::allocator_type(mem_pool, true)},
                catch_trace(state.catch_trace),
                ext_data_allocator(mem_pool, true)
            {}
            void sanity_check() noexcept
            {
                if (!stack_.empty() || exec_handle_) {
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

            IAsyncTool& async_tool_;
            IMemPool& mem_pool_;
            NextArgs next_args_;
            Queue queue_;
            Stack stack_;
            IAsyncTool::Handle exec_handle_;
            State::CatchTrace& catch_trace;
            bool in_exec_ = false;

            IMemPool::Allocator<ExtStepState> ext_data_allocator;
        };

        //---

        class BaseAsyncSteps::Protector : public ProtectorData
        {
        public:
            Protector(BaseAsyncSteps& root) noexcept : ProtectorData(root) {}

            void sanity_check() noexcept
            {
                if (root_ == nullptr) {
                    on_invalid_call("Step got invalidated!");
                }

                auto& stack = root_->impl_->stack_;

                if (stack.empty() || (this != stack.back())) {
                    on_invalid_call("Step used out-of-order!");
                }
            }

            operator bool() const noexcept override
            {
                if (root_ == nullptr) {
                    return false;
                }
                auto& stack = root_->impl_->stack_;

                return (!stack.empty() && (this == stack.back()));
            }

            StepData& add_step() noexcept override
            {
                sanity_check();

                auto buf = root_->impl_->alloc_step();
                auto step = new (buf) Protector(*root_);
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
                sanity_check();
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

            LoopState& alloc_ext_data(bool is_loop) noexcept
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
                auto step = new (buf) Protector(*root_);

                step->data_.func_ = &Protector::loop_handler;

                return step->alloc_ext_data(true);
            }

            static void loop_handler(IAsyncSteps& asi) noexcept
            {
                auto& that = static_cast<Protector&>(asi);
                auto& ls = *(that.ext_data_);

                auto buf = that.root_->impl_->alloc_step();
                auto step = new (buf) Protector(*(that.root_));

                step->data_.func_ = std::ref(ls);
                step->data_.on_error_ = std::ref(ls);
            }

            std::unique_ptr<IAsyncSteps> newInstance() noexcept override
            {
                sanity_check();

                return root_->newInstance();
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
        };

        //---

        class BaseAsyncSteps::ParallelStep final
            : public BaseAsyncSteps::Protector
        {
            friend BaseAsyncSteps;

        public:
            ParallelStep(BaseAsyncSteps& root) noexcept : Protector(root)
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
                    that.sub_queue_start = 1;
                    that.sub_queue_front = 1;
                };
                sub_data.on_error_ = std::ref(*this);

                // actual step
                auto sub_impl = sub_asi.impl_;
                auto data = sub_impl->alloc_step();
                auto step = new (data) Protector(sub_asi);
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

                for (auto& v : that.ext_data_->items_) {
                    v.add(ExecPass(std::ref(that)));
                    v.impl_->schedule_exec();
                }
                that.Protector::waitExternal();
            }

            static void cancel_cb(IAsyncSteps& asi)
            {
                auto& that = static_cast<ParallelStep&>(asi);

                if (that.ext_data_->error_code_.empty()) {
                    // Not caused by inner error
                    for (auto& v : that.ext_data_->items_) {
                        v.cancel();
                    }
                }
            }
        };

        //---
        BaseAsyncSteps::ProtectorData::~ProtectorData() noexcept
        {
            limit_handle_.cancel();

            if (ext_data_ != nullptr) {
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
            auto step = new (buf) ParallelStep(*root_);

            auto& data = step->data_;
            on_error.move(data.on_error_, data.on_error_storage_);

            return *step;
        }

        //---

        BaseAsyncSteps::BaseAsyncSteps(
                State& state, IAsyncTool& async_tool) noexcept
        {
            auto& mem_pool = async_tool.mem_pool();
            auto p = IMemPool::Allocator<Impl>(mem_pool, true).allocate(1);
            impl_ = new (p) Impl(state, async_tool, mem_pool);
        }

        BaseAsyncSteps::~BaseAsyncSteps() noexcept
        {
            BaseAsyncSteps::cancel();

            if (impl_ != nullptr) {
                impl_->~Impl();
                IMemPool::Allocator<Impl>(impl_->mem_pool_, true)
                        .deallocate(impl_, 1);
                impl_ = nullptr;
            }
        }

        BaseAsyncSteps::operator bool() const noexcept
        {
            return (impl_->stack_.empty() && !impl_->exec_handle_);
        }

        IAsyncSteps::StepData& BaseAsyncSteps::add_step() noexcept
        {
            impl_->sanity_check();

            auto buf = impl_->alloc_step();
            auto step = new (buf) Protector(*this);
            return step->data_;
        }

        IAsyncSteps& BaseAsyncSteps::parallel(ErrorPass on_error) noexcept
        {
            impl_->sanity_check();

            auto buf = impl_->alloc_step();
            auto step = new (buf) ParallelStep(*this);

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
            auto step = new (buf) Protector(*this);

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
            exec_handle_.cancel();
            ProtectorDataHolder* next_data = nullptr;

            while (!stack_.empty()) {
                auto current = stack_.back();

                if (is_sub_queue_empty(current)) {
                    sub_queue_free(current);
                    stack_.pop_back();
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

            stack_.push_back(next);

            try {
                in_exec_ = true;
                next->data_.func_(*next);

                if (stack_.empty() || (stack_.back() != next)) {
                    // pass
                } else if (!is_sub_queue_empty(next)) {
                    schedule_exec();
                } else if (!next->on_cancel_ && !next->limit_handle_) {
                    next->handle_success();
                }

                in_exec_ = false;
            } catch (const std::exception& e) {
                in_exec_ = false;
                catch_trace(e);
                next->handle_error(e.what());
            }
        }

        void BaseAsyncSteps::Impl::handle_success(
                ProtectorData* current) noexcept
        {
            if (!async_tool_.is_same_thread()) {
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

            stack_.pop_back();

            while (!stack_.empty()) {
                current = stack_.back();

                cond_sub_queue_shift(current);

                if (!is_sub_queue_empty(current)) {
                    schedule_exec();
                    return;
                }

                sub_queue_free(current);
                stack_.pop_back();
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
            if (!async_tool_.is_same_thread()) {
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

            if (current != stack_.back()) {
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

                        if (stack_.empty() || (stack_.back() != current)) {
                            // success() was called
                            return;
                        }

                        if (!is_sub_queue_empty(current)) {
                            schedule_exec();
                            return;
                        }
                    } catch (const std::exception& e) {
                        in_exec_ = false;
                        catch_trace(e);
                        code_cache = e.what();
                        code = code_cache.c_str();
                    }
                }

                stack_.pop_back();

                if (stack_.empty()) {
                    break;
                }

                current = stack_.back();
            }

            queue_.clear();
        }

        void BaseAsyncSteps::Impl::handle_cancel() noexcept
        {
            if (async_tool_.is_same_thread()) {
                if (in_exec_) {
                    on_invalid_call("cancel() inside execution");
                }

                exec_handle_.cancel();

                while (!stack_.empty()) {
                    auto current = stack_.back();
                    current->limit_handle_.cancel();

                    auto& on_cancel = current->on_cancel_;

                    if (on_cancel) {
                        on_cancel(*current);
                        current->on_cancel_ = nullptr;
                    }

                    stack_.pop_back();
                }

                queue_.clear();
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

        //---
        AsyncSteps::AsyncSteps(IAsyncTool& at) noexcept :
            BaseAsyncSteps(state_, at),
            state_(at.mem_pool())
        {}

        State& AsyncSteps::state() noexcept
        {
            return state_;
        }
    } // namespace ri
} // namespace futoin
