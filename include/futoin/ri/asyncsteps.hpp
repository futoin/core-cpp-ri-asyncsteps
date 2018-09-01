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
//! @brief Reference Implementation of AsyncSteps (FTN12) for C++
//! @sa https://specs.futoin.org/final/preview/ftn12_async_api.html
//-----------------------------------------------------------------------------

#ifndef FUTOIN_RI_ASYNCSTEPS_HPP
#define FUTOIN_RI_ASYNCSTEPS_HPP
//---
#include "./asynctool.hpp"
#include <futoin/iasyncsteps.hpp>
//---

namespace futoin {
    namespace ri {
        /**
         * @brief Common implementation of AsyncSteps
         */
        class BaseAsyncSteps : public IAsyncSteps
        {
        public:
            BaseAsyncSteps(const BaseAsyncSteps&) = delete;
            BaseAsyncSteps& operator=(const BaseAsyncSteps&) = delete;
            BaseAsyncSteps(BaseAsyncSteps&&) noexcept = default;
            BaseAsyncSteps& operator=(BaseAsyncSteps&&) noexcept = default;
            ~BaseAsyncSteps() noexcept override;

            StepData& add_step() noexcept override;
            IAsyncSteps& parallel(ErrorPass on_error = {}) noexcept override;
            void handle_success() noexcept override;
            void handle_error(ErrorCode /*code*/) override;

            asyncsteps::NextArgs& nextargs() noexcept override;
            IAsyncSteps& copyFrom(IAsyncSteps& /*asi*/) noexcept override;

            void setTimeout(std::chrono::milliseconds /*to*/) noexcept override;
            void setCancel(CancelPass /*cb*/) noexcept override;
            void waitExternal() noexcept override;
            void execute() noexcept override;
            void cancel() noexcept override;
            asyncsteps::LoopState& add_loop() noexcept override;
            operator bool() const noexcept override;
            std::unique_ptr<IAsyncSteps> newInstance() noexcept override;
            SyncRootID sync_root_id() const override;

        protected:
            BaseAsyncSteps(
                    asyncsteps::State& state, IAsyncTool& async_tool) noexcept;

        private:
            struct ExtStepState;
            struct ProtectorData;
            class ParallelStep;
            class Protector;
            struct Impl;
            struct AllocOptimizer;

            Impl* impl_;
            static AllocOptimizer alloc_optimizer;
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
            AsyncSteps(AsyncSteps&&) = default;
            AsyncSteps& operator=(AsyncSteps&&) = default;
            ~AsyncSteps() noexcept override = default;

            asyncsteps::State& state() noexcept override;
            using BaseAsyncSteps::state;

        private:
            asyncsteps::State state_;
        };
    } // namespace ri
} // namespace futoin

//---
#endif // FUTOIN_RI_ASYNCSTEPS_HPP
