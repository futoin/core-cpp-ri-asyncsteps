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

namespace futoin {
    namespace ri {

        using namespace futoin::asyncsteps;
        using std::forward;

        //---
        [[noreturn]] static void on_invalid_call()
        {
            std::cerr << "FATAL: Invalid AsyncSteps interface usage!";
            std::terminate();
        }

        //---

        class BaseAsyncSteps::Protector : public futoin::AsyncSteps
        {
            friend BaseAsyncSteps;

        public:
            using QueueItem = std::unique_ptr<Protector>;
            using Queue = std::deque<QueueItem>;

            Protector(
                    BaseAsyncSteps& root,
                    ExecHandler&& func = {},
                    ErrorHandler&& on_error = {}) :
                root_(&root),
                func_(forward<ExecHandler>(func)),
                on_error_(forward<ErrorHandler>(on_error))
            {}

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

            void success() noexcept override
            {
                sanity_check();
            }

            void handle_error(ErrorCode /*code*/) override
            {
                sanity_check();
            }

            NextArgs& nextargs() noexcept override
            {
                sanity_check();
                return root_->nextargs();
            }

            futoin::AsyncSteps& copyFrom(
                    futoin::AsyncSteps& /*asi*/) noexcept override
            {
                on_invalid_call();
            }

            State& state() noexcept override
            {
                sanity_check();
                return root_->state();
            }

            void setTimeout(std::chrono::milliseconds /*to*/) noexcept override
            {
                // TODO:
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
                on_invalid_call();
            }

            void cancel() noexcept override
            {
                on_invalid_call();
            }

            void loop_logic(LoopState&& ls) noexcept override
            {
                sanity_check();

                QueueItem qi(new Protector(*root_, &Protector::loop_handler));
                qi->loop_state_.reset(new LoopState(forward<LoopState>(ls)));
                queue_.push_back(std::move(qi));
            }

            static void loop_handler(futoin::AsyncSteps& asi)
            {
                auto& that = static_cast<Protector&>(asi);
                auto& ls = *(that.loop_state_);
                // TODO
                (void) ls;
            }

        protected:
            BaseAsyncSteps* root_;
            Queue queue_;

            ExecHandler func_;
            ErrorHandler on_error_;
            CancelCallback on_cancel_;
            std::unique_ptr<LoopState> loop_state_;
        };

        //---

        class BaseAsyncSteps::ParallelStep : public BaseAsyncSteps::Protector
        {
            friend BaseAsyncSteps;

            using ParallelItems = std::deque<BaseAsyncSteps>;

        public:
            ParallelStep(BaseAsyncSteps& root, ErrorHandler&& on_error) :
                Protector(
                        root,
                        [](futoin::AsyncSteps& asi) {
                            static_cast<ParallelStep&>(asi).process();
                        },
                        forward<ErrorHandler>(on_error))
            {}

            void add_step(
                    ExecHandler&& func,
                    ErrorHandler&& on_error) noexcept override
            {
                sanity_check();

                BaseAsyncSteps asi(state());
                asi.add(forward<ExecHandler>(func),
                        forward<ErrorHandler>(on_error));

                items_.push_back(std::move(asi));
            }

            void loop_logic(LoopState&& ls) noexcept override
            {
                sanity_check();

                BaseAsyncSteps asi(state());
                asi.loop_logic(forward<LoopState>(ls));

                items_.push_back(std::move(asi));
            }

            futoin::AsyncSteps& parallel(
                    ErrorHandler /*on_error*/) noexcept override
            {
                on_invalid_call();
            }

            void success() noexcept override
            {
                on_invalid_call();
            }

            void handle_error(ErrorCode /*code*/) override
            {
                on_invalid_call();
            }

            NextArgs& nextargs() noexcept override
            {
                on_invalid_call();
            }

            void setTimeout(std::chrono::milliseconds /*to*/) noexcept override
            {
                on_invalid_call();
            }

            void setCancel(CancelCallback /*cb*/) noexcept override
            {
                on_invalid_call();
            }

            void waitExternal() noexcept override
            {
                on_invalid_call();
            }

        protected:
            ParallelItems items_;

            void process() {}
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
        class BaseAsyncSteps::Impl
        {
            friend BaseAsyncSteps;

            void sanity_check() {}

            NextArgs next_args_;
            Protector::Queue queue_;
        };

        //---

        BaseAsyncSteps::BaseAsyncSteps(State& state_) :
            impl_(new Impl), state_(&state_)
        {}

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

        State& BaseAsyncSteps::state() noexcept
        {
            return *state_;
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

        void BaseAsyncSteps::success() noexcept
        {
            on_invalid_call();
        }

        void BaseAsyncSteps::handle_error(ErrorCode /*code*/)
        {
            impl_->sanity_check();
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

            std::cerr << "FATAL: copyFrom() is not supported in C++"
                      << std::endl;
            on_invalid_call();
        }

        void BaseAsyncSteps::setTimeout(
                std::chrono::milliseconds /*to*/) noexcept
        {
            on_invalid_call();
        }

        void BaseAsyncSteps::setCancel(CancelCallback /*cb*/) noexcept
        {
            on_invalid_call();
        }

        void BaseAsyncSteps::waitExternal() noexcept
        {
            on_invalid_call();
        }

        void BaseAsyncSteps::execute() noexcept {
            // TODO
        }

        void BaseAsyncSteps::cancel() noexcept {
            // TODO
        }

        void BaseAsyncSteps::loop_logic(LoopState&& ls) noexcept
        {
            impl_->sanity_check();

            Protector::QueueItem qi(
                    new Protector(*this, &Protector::loop_handler));
            qi->loop_state_.reset(new LoopState(forward<LoopState>(ls)));
            impl_->queue_.push_back(std::move(qi));
        }

    } // namespace ri
} // namespace futoin
