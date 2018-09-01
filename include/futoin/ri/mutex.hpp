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

#ifndef FUTOIN_RI_MUTEX_HPP
#define FUTOIN_RI_MUTEX_HPP
//---
#include <futoin/iasyncsteps.hpp>
//---
#include <cstdint>
#include <list>
#include <mutex>

namespace futoin {
    namespace ri {
        /**
         * @brief Synchronization primitive for AsyncSteps
         */
        template<typename OSMutex>
        class BaseMutex final : public ISync
        {
        public:
            using size_type = std::uint16_t;

        private:
            struct ASInfo
            {
                IAsyncSteps* pending{nullptr};
                size_type count{0};
            };

            using ASInfoList =
                    std::list<ASInfo, IMemPool::Allocator<BaseMutex::ASInfo>>;
            using ASInfoIterator = typename ASInfoList::iterator;

        public:
            BaseMutex(
                    size_type max = 1,
                    size_type queue_max =
                            std::numeric_limits<size_type>::max()) :
                max_(max),
                queue_max_(queue_max),
                this_key_(key_from_pointer(this))
            {}

            void lock(IAsyncSteps& asi) override
            {
                auto& iter = asi_iter(asi);

                if (iter == locked_list_.end()) {
                    std::lock_guard<OSMutex> lock(mutex_);

                    if (free_list.empty()) {
                        free_list.emplace_back();
                    }

                    iter = free_list.begin();

                    if (locked_list_.size() < max_) {
                        iter->count = 1;
                        locked_list_.splice(
                                locked_list_.end(), free_list, iter);
                    } else if (queue_.size() < queue_max_) {
                        iter->count = 0;
                        iter->pending = &asi;
                        queue_.splice(queue_.end(), free_list, iter);
                        asi.waitExternal();
                    } else {
                        iter = std::move(locked_list_.end()); // clear
                        asi.error(errors::DefenseRejected, "Mutex queue limit");
                    }
                } else {
                    // Must be already locked
                    assert(iter->count > 0);
                    ++(iter->count);
                }
            }
            void unlock(IAsyncSteps& asi) noexcept override
            {
                auto& iter = asi_iter(asi);

                if (iter == locked_list_.end()) {
                    return;
                }

                if (iter->count > 1) {
                    --(iter->count);
                    return;
                }

                //---
                std::lock_guard<OSMutex> lock(mutex_);

                if (iter->count == 0) {
                    free_list.splice(free_list.end(), queue_, iter);
                } else {
                    free_list.splice(free_list.end(), locked_list_, iter);
                }

                iter = std::move(locked_list_.end()); // clear

                //---
                auto next = queue_.begin();

                if (next != queue_.end()) {
                    next->count = 1;
                    auto step = next->pending;
                    next->pending = nullptr;
                    locked_list_.splice(locked_list_.end(), queue_, next);
                    lock.~lock_guard();

                    // Make sure without holding mutex
                    step->success();
                }
            }

            void shrink_to_fit()
            {
                std::lock_guard<OSMutex> lock(mutex_);
                free_list.clear();
            }

        protected:
            inline ASInfoIterator& asi_iter(IAsyncSteps& asi)
            {
                futoin::string full_key{this_key_};
                auto sync_id = asi.sync_root_id();
                full_key += futoin::string{reinterpret_cast<char*>(&sync_id),
                                           sizeof(sync_id)};

                return asi.state<ASInfoIterator>(full_key, locked_list_.end());
            }

        private:
            OSMutex mutex_;
            const size_type max_;
            const size_type queue_max_;
            ASInfoList locked_list_;
            ASInfoList queue_;
            ASInfoList free_list;

            const futoin::string this_key_;

            static typename IMemPool::Allocator<ASInfo>::EnsureOptimized
                    alloc_optimizer;
        };

        template<typename OSMutex>
        typename IMemPool::Allocator<typename BaseMutex<OSMutex>::ASInfo>::
                EnsureOptimized BaseMutex<OSMutex>::alloc_optimizer;

        using ThreadlessMutex = BaseMutex<ISync::NoopOSMutex>;
        using Mutex = BaseMutex<std::mutex>;
    } // namespace ri
} // namespace futoin

//---
#endif // FUTOIN_RI_MUTEX_HPP
