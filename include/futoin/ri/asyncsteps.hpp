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
//! @file
//! @brief Reference Implementation of AsyncSteps (FTN8)e for C++
//! @sa https://specs.futoin.org/final/preview/ftn12_async_api.html
//-----------------------------------------------------------------------------

#ifndef FUTOIN_RI_ASYNCSTEPS_HPP
#define FUTOIN_RI_ASYNCSTEPS_HPP
//---
#include <futoin/asyncsteps.hpp>
//---

namespace futoin {
    namespace ri {
        /**
         * @brief Interface of async reactor.
         */
        class IAsyncTool
        {
        public:
            IAsyncTool() = default;
            IAsyncTool(const IAsyncTool&) = delete;
            IAsyncTool& operator=(const IAsyncTool&) = delete;
            IAsyncTool(const IAsyncTool&&) = delete;
            IAsyncTool& operator=(const IAsyncTool&&) = delete;

            using Callback = std::function<void()>;

            class InternalHandle
            {
            public:
                virtual void cancel() noexcept = 0;
                virtual ~InternalHandle() noexcept = default;
            };

            class Handle
            {
            public:
                Handle() = default;
                Handle(const Handle&) = delete;
                Handle& operator=(const Handle&) = delete;

                Handle(InternalHandle& internal) noexcept : internal_(&internal)
                {}

                Handle(Handle&& other) noexcept : internal_(other.internal_)
                {
                    other.internal_ = nullptr;
                }

                Handle& operator=(Handle&& other) noexcept
                {
                    if (&other != this) {
                        if (internal_ != nullptr) {
                            internal_->cancel();
                        }
                        internal_ = other.internal_;
                        other.internal_ = nullptr;
                    }

                    return *this;
                }

                void cancel() noexcept
                {
                    if (internal_ != nullptr) {
                        internal_->cancel();
                        internal_ = nullptr;
                    }
                }

                ~Handle() noexcept
                {
                    cancel();
                }

                operator bool() const noexcept
                {
                    return internal_ != nullptr;
                }

            private:
                InternalHandle* internal_ = nullptr;
            };

            Handle setImmediate(Callback);
            Handle setTimeout(std::chrono::milliseconds, Callback);
            void cancel(Handle);
        };

        /**
         * @brief Common implementation of AsyncSteps
         */
        class BaseAsyncSteps : public futoin::AsyncSteps
        {
        public:
            BaseAsyncSteps(const BaseAsyncSteps&) = delete;
            BaseAsyncSteps& operator=(const BaseAsyncSteps&) = delete;
            BaseAsyncSteps(BaseAsyncSteps&&) = default;
            BaseAsyncSteps& operator=(BaseAsyncSteps&&) = default;
            ~BaseAsyncSteps() noexcept override;

            void add_step(
                    asyncsteps::ExecHandler&& func,
                    asyncsteps::ErrorHandler&& on_error) noexcept override;
            futoin::AsyncSteps& parallel(
                    asyncsteps::ErrorHandler on_error) noexcept override;
            void handle_success() noexcept override;
            void handle_error(ErrorCode /*code*/) override;

            asyncsteps::NextArgs& nextargs() noexcept override;
            futoin::AsyncSteps& copyFrom(
                    futoin::AsyncSteps& /*asi*/) noexcept override;

            void setTimeout(std::chrono::milliseconds /*to*/) noexcept override;
            void setCancel(asyncsteps::CancelCallback /*cb*/) noexcept override;
            void waitExternal() noexcept override;
            void execute() noexcept override;
            void cancel() noexcept override;
            void loop_logic(asyncsteps::LoopState&& ls) noexcept override;
            std::unique_ptr<futoin::AsyncSteps> newInstance() noexcept override;

        protected:
            BaseAsyncSteps(IAsyncTool&);

        private:
            class ParallelStep;
            class Protector;
            struct Impl;

            std::unique_ptr<Impl> impl_;
        };

        /**
         * @brief AsyncSteps reference implementation
         */
        class AsyncSteps final : public BaseAsyncSteps
        {
        public:
            AsyncSteps(IAsyncTool&);

            AsyncSteps(const AsyncSteps&) = delete;
            AsyncSteps& operator=(const AsyncSteps&) = delete;
            AsyncSteps(AsyncSteps&&) = default;
            AsyncSteps& operator=(AsyncSteps&&) = default;
            ~AsyncSteps() noexcept override = default;

            asyncsteps::State& state() noexcept override;

        private:
            asyncsteps::State state_;
        };
    } // namespace ri
} // namespace futoin

//---
#endif // FUTOIN_RI_ASYNCSTEPS_HPP
