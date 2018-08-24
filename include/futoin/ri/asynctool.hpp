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

#ifndef FUTOIN_RI_ASYNCTOOL_HPP
#define FUTOIN_RI_ASYNCTOOL_HPP
//---
#include <futoin/iasynctool.hpp>
//---
#include <memory>
//---

namespace futoin {
    namespace ri {
        /**
         * @brief Async reactor implementation
         */
        class AsyncTool final : public IAsyncTool
        {
        public:
            static constexpr size_t BURST_COUNT = 100U;

            /**
             * @brief Initialize with internal thread loop
             */
            AsyncTool() noexcept;

            /**
             * @brief Initialize for external thread loop
             */
            AsyncTool(std::function<void()> poke_external) noexcept;

            ~AsyncTool() noexcept override;
            AsyncTool(const AsyncTool&) = delete;
            AsyncTool& operator=(const AsyncTool&) = delete;
            AsyncTool(AsyncTool&&) = delete;
            AsyncTool& operator=(AsyncTool*&) = delete;

            Handle immediate(Callback&& cb) noexcept override;
            Handle deferred(
                    std::chrono::milliseconds delay,
                    Callback&& cb) noexcept override;
            bool is_same_thread() noexcept override;
            CycleResult iterate() noexcept override;

            struct Stats
            {
                size_t immediate_count;
                size_t deferred_used;
                size_t deferred_free;
                size_t handle_task_count;
            };

            Stats stats() noexcept;
            void shrink_to_fit() noexcept;

        protected:
            void cancel(Handle& h) noexcept override;
            bool is_valid(Handle& h) noexcept override;

        private:
            struct Impl;
            std::unique_ptr<Impl> impl_;
        };
    } // namespace ri
} // namespace futoin

//---
#endif // FUTOIN_RI_ASYNCTOOL_HPP
