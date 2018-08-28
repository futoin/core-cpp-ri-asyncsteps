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
        struct BaseAsyncSteps::Impl
        {
            using QueueItem = std::unique_ptr<Protector>;
            using Queue = std::deque<QueueItem>;
            using Stack = std::stack<Protector*>;

            Impl(State& state, IAsyncTool& async_tool) :
                async_tool_(async_tool), mem_pool_(async_tool.mem_pool()),
                catch_trace(state.catch_trace)
            {}
            void sanity_check() noexcept
            {
                if (!stack_.empty() || exec_handle_) {
                    on_invalid_call("Out-of-order use of root AsyncSteps");
                }
            }

            void schedule_exec() noexcept;
            void execute_handler() noexcept;
            void handle_success(Protector* current) noexcept;
            void handle_error(Protector* current, ErrorCode code) noexcept;
            void handle_cancel() noexcept;
            void operator()() noexcept
            {
                execute_handler();
            }

            template<typename Q>
            void cond_queue_pop(Q& q)
            {
                auto loop_state = q.front()->loop_state_.get();

                if ((loop_state == nullptr) || !loop_state->continue_loop) {
                    q.pop_front();
                }
            }

            IAsyncTool& async_tool_;
            IMemPool& mem_pool_;
            NextArgs next_args_;
            Queue queue_;
            Stack stack_;
            IAsyncTool::Handle exec_handle_;
            State::CatchTrace& catch_trace;
            bool in_exec_ = false;
        };

        //---
        class SubAsyncSteps final : public BaseAsyncSteps
        {
        public:
            SubAsyncSteps(State& state, IAsyncTool& async_tool) noexcept :
                BaseAsyncSteps(state, async_tool), state_(state)
            {}

            State& state() noexcept override
            {
                return state_;
            }

        private:
            State& state_;
        };

        //---
        struct BaseAsyncSteps::ExtLoopState : LoopState
        {
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
        };

        //---

        class BaseAsyncSteps::Protector : public IAsyncSteps
        {
            friend BaseAsyncSteps;
            friend BaseAsyncSteps::Impl;

            using QueueItem = Impl::QueueItem;
            using Queue = Impl::Queue;

        public:
            Protector(BaseAsyncSteps& root) noexcept : root_(&root) {}

            ~Protector() noexcept override
            {
                limit_handle_.cancel();
                root_ = nullptr;
            }

            void sanity_check() noexcept
            {
                if (root_ == nullptr) {
                    on_invalid_call("Step got invalidated!");
                }

                auto& stack = root_->impl_->stack_;

                if (stack.empty() || (this != stack.top())) {
                    on_invalid_call("Step used out-of-order!");
                }
            }

            operator bool() const noexcept override
            {
                if (root_ == nullptr) {
                    return false;
                }
                auto& stack = root_->impl_->stack_;

                return (!stack.empty() && (this == stack.top()));
            }

            StepData& add_step() noexcept override
            {
                sanity_check();

                auto step = new Protector(*root_);
                queue_.emplace_back(step);
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

            LoopState& add_loop() noexcept override
            {
                sanity_check();

                auto step = new Protector(*root_);
                queue_.emplace_back(step);

                step->data_.func_ = &Protector::loop_handler;

                auto& pls = step->loop_state_;
                pls.reset(new ExtLoopState);
                return *pls;
            }

            static void loop_handler(IAsyncSteps& asi) noexcept
            {
                auto& that = static_cast<Protector&>(asi);
                auto& ls = *(that.loop_state_);

                auto step = new Protector(*(that.root_));
                that.queue_.emplace_back(step);

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

        protected:
            BaseAsyncSteps* root_;
            Queue queue_;

            CancelPass::Storage on_cancel_storage_;
            StepData data_;
            CancelCallback on_cancel_;
            std::unique_ptr<ExtLoopState> loop_state_;
            IAsyncTool::Handle limit_handle_;
        };

        //---

        class BaseAsyncSteps::ParallelStep final
            : public BaseAsyncSteps::Protector
        {
            friend BaseAsyncSteps;

            using ParallelItems = std::deque<SubAsyncSteps>;

        public:
            ParallelStep(BaseAsyncSteps& root) noexcept : Protector(root)
            {
                data_.func_ = [](IAsyncSteps& asi) {
                    static_cast<ParallelStep&>(asi).process();
                };
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
                items_.emplace_back(root_->state(), root_->impl_->async_tool_);

                auto& root_step = items_.back();
                auto& root_data = root_step.add_step();

                root_data.func_ = [](IAsyncSteps&) {};
                root_data.on_error_ = std::ref(*this);

                // actual step
                auto step = new Protector(root_step);
                root_step.impl_->queue_[0]->queue_.emplace_back(step);
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

                auto& pls = step->loop_state_;
                pls.reset(new ExtLoopState);
                return *pls;
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
                ++completed_;

                if (completed_ == items_.size()) {
                    root_->impl_->async_tool_.immediate(std::ref(*this));
                }
            }

            // Dirty hack: sub-step error
            void operator()(IAsyncSteps& asi, ErrorCode err) noexcept
            {
                auto current = static_cast<Protector&>(asi).root_;

                for (auto& v : items_) {
                    if (&v != current) {
                        v.cancel();
                    }
                }

                error_code_ = err;
                root_->impl_->async_tool_.immediate(std::ref(*this));
            }

            // Dirty hack: final completion
            void operator()() noexcept
            {
                if (error_code_.empty()) {
                    root_->impl_->handle_success(this);
                } else {
                    root_->impl_->handle_error(this, error_code_.c_str());
                }
            }

        protected:
            ParallelItems items_;
            size_t completed_{0};
            futoin::string error_code_;

            void process() noexcept
            {
                for (auto& v : items_) {
                    v.add(ExecPass(std::ref(*this)));
                    v.impl_->schedule_exec();
                }
                Protector::waitExternal();
            }
        };

        //---

        IAsyncSteps& BaseAsyncSteps::Protector::parallel(
                ErrorPass on_error) noexcept
        {
            sanity_check();

            queue_.emplace_back(new ParallelStep(*root_));
            auto& res = queue_.back();

            auto& data = res->data_;
            on_error.move(data.on_error_, data.on_error_storage_);

            return *res;
        }

        //---

        BaseAsyncSteps::BaseAsyncSteps(
                State& state, IAsyncTool& async_tool) noexcept :
            impl_(new Impl(state, async_tool))
        {}

        BaseAsyncSteps::~BaseAsyncSteps() noexcept
        {
            BaseAsyncSteps::cancel();
        }

        BaseAsyncSteps::operator bool() const noexcept
        {
            return (impl_->stack_.empty() && !impl_->exec_handle_);
        }

        IAsyncSteps::StepData& BaseAsyncSteps::add_step() noexcept
        {
            impl_->sanity_check();

            auto step = new Protector(*this);
            impl_->queue_.emplace_back(step);
            return step->data_;
        }

        IAsyncSteps& BaseAsyncSteps::parallel(ErrorPass on_error) noexcept
        {
            impl_->sanity_check();

            auto step = new ParallelStep(*this);
            impl_->queue_.emplace_back(step);

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

            auto* other = dynamic_cast<BaseAsyncSteps*>(&asi);
            assert(other);

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

            auto step = new Protector(*this);
            impl_->queue_.emplace_back(step);

            step->data_.func_ = &Protector::loop_handler;

            auto& pls = step->loop_state_;
            pls.reset(new ExtLoopState);
            return *pls;
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
            Protector* next = nullptr;

            while (!stack_.empty()) {
                auto& q = stack_.top()->queue_;

                if (q.empty()) {
                    stack_.pop();
                } else {
                    next = q.front().get();
                    break;
                }
            }

            if (next == nullptr) {
                if (queue_.empty()) {
                    return;
                }

                next = queue_.front().get();
            }

            stack_.push(next);

            try {
                in_exec_ = true;
                next->data_.func_(*next);

                if (stack_.empty() || (stack_.top() != next)) {
                    // pass
                } else if (!next->queue_.empty()) {
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

        void BaseAsyncSteps::Impl::handle_success(Protector* current) noexcept
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

            if (!current->queue_.empty()) {
                on_invalid_call("success() with sub-steps");
            }

            stack_.pop();

            while (!stack_.empty()) {
                current = stack_.top();

                auto& q = current->queue_;
                cond_queue_pop(q);

                if (!q.empty()) {
                    schedule_exec();
                    return;
                }

                stack_.pop();
            }

            // Got to root queue
            cond_queue_pop(queue_);

            if (!queue_.empty()) {
                schedule_exec();
            }
        }

        void BaseAsyncSteps::Impl::handle_error(
                Protector* current, ErrorCode code) noexcept
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

            if (current != stack_.top()) {
                on_invalid_call("error() out of order");
            }

            futoin::string code_cache;

            for (;;) {
                current->queue_.clear();

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

                        if (stack_.empty() || (stack_.top() != current)) {
                            // success() was called
                            return;
                        }

                        if (!current->queue_.empty()) {
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

                stack_.pop();

                if (stack_.empty()) {
                    break;
                }

                current = stack_.top();
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
            BaseAsyncSteps(state_, at), state_(at.mem_pool())
        {}

        State& AsyncSteps::state() noexcept
        {
            return state_;
        }
    } // namespace ri
} // namespace futoin
