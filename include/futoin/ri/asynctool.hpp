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
#include <futoin/imempool.hpp>
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
            static constexpr size_t BURST_COUNT = 128U;
            using PokeCallback = std::function<void()>;

            /**
             * @brief Parameters for AsyncTool
             */
            struct Params
            {
                Params() noexcept : mempool_mutex(true) {}
                Params(const Params&) noexcept = default;

                // There is some GCC/CC+11 bug
                // NOLINTNEXTLINE(modernize-use-default-member-init)
                bool mempool_mutex;
            };

            /**
             * @brief Initialize with internal thread loop
             */
            AsyncTool(const Params& params = {}) noexcept;

            /**
             * @brief Initialize for external thread loop
             */
            AsyncTool(
                    PokeCallback poke_external,
                    const Params& params = {}) noexcept;

            ~AsyncTool() noexcept final;
            AsyncTool(const AsyncTool&) = delete;
            AsyncTool& operator=(const AsyncTool&) = delete;
            AsyncTool(AsyncTool&&) = delete;
            AsyncTool& operator=(AsyncTool*&) = delete;

            Handle immediate(CallbackPass&& cb) noexcept final;
            Handle deferred(
                    std::chrono::milliseconds delay,
                    CallbackPass&& cb) noexcept final;
            bool is_same_thread() noexcept final;
            CycleResult iterate() noexcept final;

            IMemPool& mem_pool(
                    size_t object_size = 1,
                    bool optimize = false) noexcept final;
            void release_memory() noexcept final;

            struct Stats
            {
                size_t immediate_used;
                size_t deferred_used;
                size_t universal_free;
                size_t handle_task_count;
            };

            Stats stats() noexcept;

        protected:
            void cancel(Handle& h) noexcept final;
            bool is_valid(Handle& h) noexcept final;

        private:
            struct Impl;
            std::unique_ptr<Impl> impl_;
        };
    } // namespace ri
} // namespace futoin

//---
#endif // FUTOIN_RI_ASYNCTOOL_HPP
