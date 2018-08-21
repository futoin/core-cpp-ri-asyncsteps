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
        protected:
            struct HandleAccessor;

        public:
            IAsyncTool() = default;
            IAsyncTool(const IAsyncTool&) = delete;
            IAsyncTool& operator=(const IAsyncTool&) = delete;
            IAsyncTool(const IAsyncTool&&) = delete;
            IAsyncTool& operator=(const IAsyncTool&&) = delete;
            virtual ~IAsyncTool() noexcept = default;

            using Callback = std::function<void()>;
            class Handle;

            struct InternalHandle
            {
                InternalHandle(Callback&& cb) :
                    callback(std::forward<Callback>(cb))
                {}

                Callback callback;
                Handle* outer = nullptr;
            };

            /**
             * @brief Handle to scheduled callback
             */
            class Handle
            {
            public:
                Handle() = default;
                Handle(const Handle&) = delete;
                Handle& operator=(const Handle&) = delete;

                Handle(InternalHandle& internal,
                       IAsyncTool& async_tool) noexcept :
                    internal_(&internal),
                    async_tool_(&async_tool)
                {
                    internal.outer = this;
                }

                Handle(Handle&& other) noexcept : internal_(other.internal_)
                {
                    if (other.async_tool_ != nullptr) {
                        other.async_tool_->move(other, *this);
                    }
                }

                ~Handle() noexcept
                {
                    if (internal_ != nullptr) {
                        async_tool_->free(*this);
                    }
                }

                Handle& operator=(Handle&& other) noexcept
                {
                    if (&other != this) {
                        if (other.async_tool_ != nullptr) {
                            if (internal_ != nullptr) {
                                async_tool_->free(*this);
                            }

                            other.async_tool_->move(other, *this);
                        } else {
                            cancel();
                        }
                    }

                    return *this;
                }

                void cancel() noexcept
                {
                    if (internal_ != nullptr) {
                        async_tool_->cancel(*this);
                        internal_ = nullptr;
                    }
                }

                operator bool() const noexcept
                {
                    return internal_ != nullptr;
                }

            private:
                friend struct IAsyncTool::HandleAccessor;

                InternalHandle* internal_ = nullptr;
                IAsyncTool* async_tool_ = nullptr;
            };

            /**
             * @brief Schedule immediate callback
             */
            virtual Handle immediate(Callback&& cb) noexcept = 0;

            /**
             * @brief Schedule deferred callback
             */
            virtual Handle deferred(
                    std::chrono::milliseconds delay,
                    Callback&& cb) noexcept = 0;

            /**
             * @brief Check, if the same thread as internal event loop
             */
            virtual bool is_same_thread() noexcept = 0;

            /**
             * Result of one internal cycle
             */
            struct CycleResult
            {
                CycleResult(
                        bool have_work,
                        std::chrono::milliseconds delay) noexcept :
                    delay(delay),
                    have_work(have_work)
                {}

                std::chrono::milliseconds delay;
                bool have_work;
            };

            /**
             * @brief Iterate a cycle of internal loop.
             * @note For integration with external event loop.
             */
            CycleResult iterate() noexcept;

        protected:
            struct HandleAccessor
            {
                HandleAccessor(Handle& h) :
                    internal_(h.internal_), async_tool_(h.async_tool_)
                {}

                InternalHandle*& internal_;
                IAsyncTool*& async_tool_;
            };

            virtual void cancel(Handle& h) noexcept = 0;
            virtual void move(Handle& src, Handle& dst) noexcept = 0;
            virtual void free(Handle& h) noexcept = 0;
        };

        /**
         * @brief Common implementation of AsyncSteps
         */
        class BaseAsyncSteps : public futoin::AsyncSteps
        {
        public:
            BaseAsyncSteps(const BaseAsyncSteps&) = delete;
            BaseAsyncSteps& operator=(const BaseAsyncSteps&) = delete;
            BaseAsyncSteps(BaseAsyncSteps&&) noexcept = default;
            BaseAsyncSteps& operator=(BaseAsyncSteps&&) noexcept = default;
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
            BaseAsyncSteps(IAsyncTool&) noexcept;

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
            AsyncSteps(IAsyncTool&) noexcept;

            AsyncSteps(const AsyncSteps&) = delete;
            AsyncSteps& operator=(const AsyncSteps&) = delete;
            AsyncSteps(AsyncSteps&&) noexcept = default;
            AsyncSteps& operator=(AsyncSteps&&) noexcept = default;
            ~AsyncSteps() noexcept override = default;

            asyncsteps::State& state() noexcept override;

        private:
            asyncsteps::State state_;
        };
    } // namespace ri
} // namespace futoin

//---
#endif // FUTOIN_RI_ASYNCSTEPS_HPP
