//-----------------------------------------------------------------------------
//   Copyright 2018-2023 FutoIn Project
//   Copyright 2018-2023 Andrey Galkin
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

#ifndef FUTOIN_RI_LIMITER_HPP
#define FUTOIN_RI_LIMITER_HPP
//---
#include "./mutex.hpp"
#include "./throttle.hpp"
//---

namespace futoin {
    namespace ri {
        /**
         * @brief Base implementation of FTN12 Limiter for AsyncSteps
         */
        template<typename OSMutex>
        class BaseLimiter : public ISync
        {
        public:
            using size_type = typename BaseMutex<OSMutex>::size_type;
            using milliseconds = typename BaseThrottle<OSMutex>::milliseconds;

            /**
             * @brief Configuration of limiter
             */
            struct Params
            {
                //! Maximum number of concurrent flows
                size_type concurrent{1};
                //! Max number of pending flows
                size_type max_queue{0};
                //! Max number of flow entry count per period
                size_type rate{1};
                //! Period for flow entry count reset
                milliseconds period{1000};
                //! Max number of pending flow entries (moved to the next
                //! period)
                size_type burst{0};
            };

            BaseLimiter(IAsyncTool& async_tool, const Params& prm) noexcept :
                mutex_(prm.concurrent, prm.max_queue),
                throttle_(async_tool, prm.rate, prm.period, prm.burst)
            {
                init_binary_sync(*this);
            }

            void lock(IAsyncSteps& asi) final
            {
                asi.add([this](IAsyncSteps& asi) { mutex_.lock(asi); });
                asi.add([this](IAsyncSteps& asi) { throttle_.lock(asi); });
            }
            // NOLINTNEXTLINE(bugprone-exception-escape)
            void unlock(IAsyncSteps& asi) noexcept final
            {
                throttle_.unlock(asi);
                mutex_.unlock(asi);
            }

        private:
            BaseMutex<OSMutex> mutex_;
            BaseThrottle<OSMutex> throttle_;
        };

        extern template class BaseLimiter<ISync::NoopOSMutex>;
        extern template class BaseLimiter<std::mutex>;

        using ThreadlessLimiter = BaseLimiter<ISync::NoopOSMutex>;
        using Limiter = BaseLimiter<std::mutex>;
    } // namespace ri
} // namespace futoin

//---
#endif // FUTOIN_RI_LIMITER_HPP
