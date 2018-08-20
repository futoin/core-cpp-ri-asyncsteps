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
#include <deque>
#include <iostream>
#include <memory>
#include <stack>

namespace futoin {
    namespace ri {

        using namespace futoin::asyncsteps;
        using std::forward;

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

            Impl(IAsyncTool& async_tool) : async_tool_(async_tool) {}
            void sanity_check() {}

            void schedule_exec() noexcept;
            void execute_handler() noexcept;
            void handle_success() noexcept;
            void handle_error(ErrorCode code) noexcept;
            void handle_cancel() noexcept;

            IAsyncTool& async_tool_;
            NextArgs next_args_;
            Queue queue_;
            Stack stack_;
            IAsyncTool::Handle exec_handle_;
            Protector* current_ = nullptr;
            bool in_exec_ = false;
        };

        //---
        class SubAsyncSteps final : public BaseAsyncSteps
        {
        public:
            SubAsyncSteps(State& state, IAsyncTool& async_tool) noexcept :
                BaseAsyncSteps(async_tool), state_(state)
            {}

            State& state() noexcept override
            {
                return state_;
            }

        private:
            State& state_;
        };

        //---

        class BaseAsyncSteps::Protector : public futoin::AsyncSteps
        {
            friend BaseAsyncSteps;
            friend BaseAsyncSteps::Impl;

            using QueueItem = Impl::QueueItem;
            using Queue = Impl::Queue;

        public:
            Protector(
                    BaseAsyncSteps& root,
                    ExecHandler&& func = {},
                    ErrorHandler&& on_error = {}) noexcept :
                root_(&root),
                func_(forward<ExecHandler>(func)),
                on_error_(forward<ErrorHandler>(on_error))
            {}

            ~Protector() noexcept override
            {
                limit_handle_.cancel();
                root_ = nullptr;
            }

            void sanity_check()
            {
                assert(root_);
            }

            void add_step(
                    ExecHandler&& func,
                    ErrorHandler&& on_error) noexcept override
            {
                sanity_check();

                QueueItem qi(new Protector(
                        *root_,
                        forward<ExecHandler>(func),
                        forward<ErrorHandler>(on_error)));

                queue_.push_back(std::move(qi));
            }

            futoin::AsyncSteps& parallel(
                    ErrorHandler on_error) noexcept override;

            void handle_success() noexcept override
            {
                sanity_check();
                root_->impl_->handle_success();
            }

            void handle_error(ErrorCode code) override
            {
                sanity_check();
                root_->impl_->handle_error(code);
            }

            NextArgs& nextargs() noexcept override
            {
                sanity_check();
                return root_->nextargs();
            }

            futoin::AsyncSteps& copyFrom(
                    futoin::AsyncSteps& /*asi*/) noexcept override
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
                limit_handle_ = root_->impl_->async_tool_.deferred(
                        to, [this]() { this->cancel(); });
            }

            void setCancel(CancelCallback cb) noexcept override
            {
                sanity_check();
                on_cancel_ = std::move(cb);
            }

            void waitExternal() noexcept override
            {
                sanity_check();

                if (!on_cancel_) {
                    on_cancel_ = [](futoin::AsyncSteps&) {};
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

            void loop_logic(LoopState&& ls) noexcept override
            {
                sanity_check();

                QueueItem qi(new Protector(*root_, &Protector::loop_handler));
                qi->loop_state_.reset(new LoopState(forward<LoopState>(ls)));
                queue_.push_back(std::move(qi));
            }

            static void loop_handler(futoin::AsyncSteps& asi) noexcept
            {
                auto& that = static_cast<Protector&>(asi);
                auto& ls = *(that.loop_state_);
                // TODO
                (void) ls;
            }

            std::unique_ptr<futoin::AsyncSteps> newInstance() noexcept override
            {
                sanity_check();

                return root_->newInstance();
            }

        protected:
            BaseAsyncSteps* root_;
            Queue queue_;

            ExecHandler func_;
            ErrorHandler on_error_;
            CancelCallback on_cancel_;
            IAsyncTool::Handle limit_handle_;
            std::unique_ptr<LoopState> loop_state_;
        };

        //---

        class BaseAsyncSteps::ParallelStep final
            : public BaseAsyncSteps::Protector
        {
            friend BaseAsyncSteps;

            using ParallelItems = std::deque<SubAsyncSteps>;

        public:
            ParallelStep(
                    BaseAsyncSteps& root, ErrorHandler&& on_error) noexcept :
                Protector(
                        root,
                        [](futoin::AsyncSteps& asi) {
                            static_cast<ParallelStep&>(asi).process();
                        },
                        forward<ErrorHandler>(on_error))
            {}

            ~ParallelStep() noexcept override
            {
                for (auto& v : items_) {
                    v.cancel();
                }
            }

            void add_step(
                    ExecHandler&& func,
                    ErrorHandler&& on_error) noexcept override
            {
                sanity_check();

                SubAsyncSteps asi(root_->state(), root_->impl_->async_tool_);
                asi.add(forward<ExecHandler>(func),
                        forward<ErrorHandler>(on_error));

                items_.push_back(std::move(asi));
            }

            void loop_logic(LoopState&& ls) noexcept override
            {
                sanity_check();

                SubAsyncSteps asi(root_->state(), root_->impl_->async_tool_);
                asi.loop_logic(forward<LoopState>(ls));

                items_.push_back(std::move(asi));
            }

            futoin::AsyncSteps& parallel(
                    ErrorHandler /*on_error*/) noexcept override
            {
                on_invalid_call("parallel() on parallel()");
            }

            void handle_success() noexcept override
            {
                on_invalid_call("success() on parallel()");
            }

            void handle_error(ErrorCode /*code*/) override
            {
                on_invalid_call("error() on parallel()");
            }

            NextArgs& nextargs() noexcept override
            {
                on_invalid_call("nextargs() on parallel()");
            }

            void setTimeout(std::chrono::milliseconds /*to*/) noexcept override
            {
                on_invalid_call("setTimeout() on parallel()");
            }

            void setCancel(CancelCallback /*cb*/) noexcept override
            {
                on_invalid_call("setCancel() on parallel()");
            }

            void waitExternal() noexcept override
            {
                on_invalid_call("waitExternal() on parallel()");
            }

        protected:
            ParallelItems items_;

            void process() noexcept
            {
                // TODO
            }
        };

        //---

        futoin::AsyncSteps& BaseAsyncSteps::Protector::parallel(
                ErrorHandler on_error) noexcept
        {
            sanity_check();

            QueueItem qi(
                    new ParallelStep(*root_, forward<ErrorHandler>(on_error)));
            queue_.push_back(std::move(qi));

            return *this;
        }

        //---

        BaseAsyncSteps::BaseAsyncSteps(IAsyncTool& async_tool) noexcept :
            impl_(new Impl(async_tool))
        {}

        BaseAsyncSteps::~BaseAsyncSteps() noexcept
        {
            BaseAsyncSteps::cancel();
        }

        void BaseAsyncSteps::add_step(
                ExecHandler&& func, ErrorHandler&& on_error) noexcept
        {
            impl_->sanity_check();

            Protector::QueueItem qi(new Protector(
                    *this,
                    forward<ExecHandler>(func),
                    forward<ErrorHandler>(on_error)));

            impl_->queue_.push_back(std::move(qi));
        }

        futoin::AsyncSteps& BaseAsyncSteps::parallel(
                ErrorHandler on_error) noexcept
        {
            impl_->sanity_check();

            Protector::QueueItem qi(
                    new ParallelStep(*this, forward<ErrorHandler>(on_error)));
            impl_->queue_.push_back(std::move(qi));

            return *this;
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

        futoin::AsyncSteps& BaseAsyncSteps::copyFrom(
                futoin::AsyncSteps& asi) noexcept
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

        void BaseAsyncSteps::setCancel(CancelCallback /*cb*/) noexcept
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
            impl_->sanity_check();
            impl_->handle_cancel();
        }

        void BaseAsyncSteps::loop_logic(LoopState&& ls) noexcept
        {
            impl_->sanity_check();

            Protector::QueueItem qi(
                    new Protector(*this, &Protector::loop_handler));
            qi->loop_state_.reset(new LoopState(forward<LoopState>(ls)));
            impl_->queue_.push_back(std::move(qi));
        }

        std::unique_ptr<futoin::AsyncSteps>
        BaseAsyncSteps::newInstance() noexcept
        {
            return std::unique_ptr<futoin::AsyncSteps>(
                    new ri::AsyncSteps(impl_->async_tool_));
        }

        //---
        void BaseAsyncSteps::Impl::schedule_exec() noexcept
        {
            if (exec_handle_) {
                on_invalid_call("AsyncSteps instance is already executed.");
            }

            exec_handle_ = async_tool_.immediate(
                    [this]() { this->execute_handler(); });
        }

        void BaseAsyncSteps::Impl::execute_handler() noexcept
        {
            exec_handle_.cancel();
            Protector* next = nullptr;

            while (!stack_.empty()) {
                auto& q = stack_.top()->queue_;

                if (!q.empty()) {
                    next = q.front().get();
                }
            }

            if (next == nullptr) {
                if (queue_.empty()) {
                    return;
                }

                next = queue_.front().get();
            }

            try {
                current_ = next;
                in_exec_ = true;
                next->func_(*next);

                if (!next->queue_.empty()) {
                    schedule_exec();
                } else if (!next->on_cancel_ && !next->limit_handle_) {
                    next->success();
                }

                in_exec_ = false;
            } catch (const std::exception& e) {
                next->handle_error(e.what());
                in_exec_ = false;
            }
        }

        void BaseAsyncSteps::Impl::handle_success() noexcept
        {
            if (current_ != stack_.top()) {
                on_invalid_call("success() out of order");
            }

            if (!current_->queue_.empty()) {
                on_invalid_call("success() with sub-steps");
            }

            while (!stack_.empty()) {
                stack_.pop();

                current_ = stack_.top();

                auto& q = current_->queue_;
                q.pop_front();

                if (!q.empty()) {
                    schedule_exec();
                    return;
                }
            }

            // Got to root queue
            queue_.pop_front();

            if (!queue_.empty()) {
                schedule_exec();
            }
        }

        void BaseAsyncSteps::Impl::handle_error(ErrorCode code) noexcept
        {
            if (exec_handle_) {
                // Out-of-sequence error
                handle_cancel();
                return;
            }

            if (in_exec_) {
                // avoid double handling
                return;
            }

            if (current_ != stack_.top()) {
                on_invalid_call("error() out of order");
            }

            while (!stack_.empty()) {
                current_->queue_.clear();

                current_->limit_handle_.cancel();

                auto& on_cancel = current_->on_cancel_;

                if (on_cancel) {
                    on_cancel(*current_);
                    current_->on_cancel_ = nullptr;
                }

                ErrorHandler on_error{std::move(current_->on_error_)};

                if (on_error) {
                    try {
                        in_exec_ = true;
                        auto slen = stack_.size();
                        on_error(*current_, code);

                        if (stack_.size() != slen) {
                            // success() was called
                            return;
                        }

                        if (!current_->queue_.empty()) {
                            schedule_exec();
                            return;
                        }
                    } catch (const std::exception& e) {
                        in_exec_ = false;
                        code = e.what();
                    }
                }

                stack_.pop();
                current_ = stack_.top();
            }

            queue_.clear();
            current_ = nullptr;
        }

        void BaseAsyncSteps::Impl::handle_cancel() noexcept
        {
            exec_handle_.cancel();
            queue_.clear();
        }

        //---
        State& AsyncSteps::state() noexcept
        {
            return state_;
        }
    } // namespace ri
} // namespace futoin
