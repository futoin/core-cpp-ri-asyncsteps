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

#ifndef FUTOIN_RI_THROTTLE_HPP
#define FUTOIN_RI_THROTTLE_HPP
//---
#include <futoin/iasyncsteps.hpp>
#include <futoin/iasynctool.hpp>
//---
#include <chrono>
#include <cstdint>
#include <list>
#include <mutex>

namespace futoin {
    namespace ri {
        /**
         * @brief Base implementation of FTN12 Throttle for AsyncSteps
         */
        template<typename OSMutex>
        class BaseThrottle : public ISync
        {
        public:
            using size_type = std::uint32_t;
            using milliseconds = std::chrono::milliseconds;

        private:
            using ASInfoList =
                    std::list<IAsyncSteps*, IMemPool::Allocator<IAsyncSteps*>>;
            using ASInfoIterator = typename ASInfoList::iterator;
            using clock = std::chrono::steady_clock;

        public:
            BaseThrottle(
                    IAsyncTool& async_tool,
                    size_type max,
                    milliseconds period = milliseconds{1000},
                    size_type queue_max =
                            std::numeric_limits<size_type>::max()) noexcept :
                async_tool_(async_tool),
                max_(max),
                period_(period),
                last_reset_(clock::now()),
                queue_max_(queue_max),
                this_key_(key_from_pointer(this)),
                reset_callback_([this]() { this->reset_callback(); })
            {
                timer_ = async_tool.deferred(period, std::ref(reset_callback_));
            }

            ~BaseThrottle() noexcept final
            {
                timer_.cancel();
            }

            void lock(IAsyncSteps& asi) final
            {
                auto& iter = asi_iter(asi);
                assert(iter == queue_.end());

                std::lock_guard<OSMutex> lock(mutex_);

                if (queue_.empty() && (count_ < max_)) {
                    ++count_;

                    if (!timer_) {
                        last_reset_ = clock::now();
                        timer_ = async_tool_.deferred(
                                period_, std::ref(reset_callback_));
                    }
                } else if (queue_.size() < queue_max_) {
                    if (free_list_.empty()) {
                        free_list_.emplace_back();
                    }

                    iter = free_list_.begin();
                    *iter = &asi;
                    queue_.splice(queue_.end(), free_list_, iter);
                    asi.waitExternal();
                } else {
                    iter = std::move(queue_.end()); // clear
                    asi.error(errors::DefenseRejected, "Throttle queue limit");
                }
            }
            void unlock(IAsyncSteps& asi) noexcept final
            {
                auto& iter = asi_iter(asi);

                if (iter == queue_.end()) {
                    return;
                }

                std::lock_guard<OSMutex> lock(mutex_);
                free_list_.splice(free_list_.end(), queue_, iter);
                *iter = nullptr;
                iter = std::move(queue_.end()); // clear
            }

            void reset()
            {
                timer_.cancel();
                reset_callback();
            }

            void shrink_to_fit()
            {
                std::lock_guard<OSMutex> lock(mutex_);
                free_list_.clear();
            }

        protected:
            inline ASInfoIterator& asi_iter(IAsyncSteps& asi)
            {
                futoin::string full_key{this_key_};
                auto sync_id = asi.sync_root_id();
                full_key += futoin::string{reinterpret_cast<char*>(&sync_id),
                                           sizeof(sync_id)};

                return asi.state<ASInfoIterator>(full_key, queue_.end());
            }

            void reset_callback()
            {
                auto now = clock::now();
                auto delay = std::chrono::duration_cast<milliseconds>(
                        last_reset_ + (period_ * 2) - now);
                last_reset_ = now;

                if (delay <= milliseconds{0}) {
                    timer_ = async_tool_.immediate(std::ref(reset_callback_));
                } else {
                    timer_ = async_tool_.deferred(
                            delay, std::ref(reset_callback_));
                }

                //---
                std::lock_guard<OSMutex> lock(mutex_);
                count_ = 0;

                auto begin = queue_.begin();
                auto iter = begin;

                while ((count_ < max_) && (iter != queue_.end())) {
                    asi_iter(**iter) = std::move(queue_.end()); // clear
                    ++count_;
                    (*iter)->success();
                    *iter = nullptr;
                    ++iter;
                }

                if (count_ > 0) {
                    free_list_.splice(free_list_.end(), queue_, begin, iter);
                } else {
                    timer_.cancel();
                }
            }

        private:
            IAsyncTool& async_tool_;
            IAsyncTool::Handle timer_;
            OSMutex mutex_;
            size_type count_{0};
            const size_type max_;
            milliseconds period_;
            clock::time_point last_reset_;
            const size_type queue_max_;
            ASInfoList queue_;
            ASInfoList free_list_;

            const futoin::string this_key_;
            std::function<void()> reset_callback_;

            static typename IMemPool::Allocator<IAsyncSteps*>::EnsureOptimized
                    alloc_optimizer;
        };

        extern template class BaseThrottle<ISync::NoopOSMutex>;
        extern template class BaseThrottle<std::mutex>;

        template<typename OSMutex>
        typename IMemPool::Allocator<IAsyncSteps*>::EnsureOptimized
                BaseThrottle<OSMutex>::alloc_optimizer;

        using ThreadlessThrottle = BaseThrottle<ISync::NoopOSMutex>;
        using Throttle = BaseThrottle<std::mutex>;
    } // namespace ri
} // namespace futoin

//---
#endif // FUTOIN_RI_THROTTLE_HPP
