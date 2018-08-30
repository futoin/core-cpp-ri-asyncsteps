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

#ifndef FUTOIN_RI_MEMPOOL_HPP
#define FUTOIN_RI_MEMPOOL_HPP
//---
#include <futoin/imempool.hpp>
//---
#include <boost/pool/pool.hpp>
#include <map>
#include <mutex>
//---

namespace futoin {
    namespace ri {
        /**
         * @brief boost::pool-based type-erased memory pool
         */
        class BoostMemPool final : public IMemPool
        {
        public:
            BoostMemPool(IMemPool& root, size_t requested_size) noexcept :
                root(root),
                pool(requested_size)
            {}

            void* allocate(
                    size_t /*object_size*/, size_t count) noexcept override
            {
                std::lock_guard<std::mutex> lock(mutex);
                return pool.ordered_malloc(count);
            }

            void deallocate(
                    void* ptr,
                    size_t /*object_size*/,
                    size_t count) noexcept override
            {
                std::lock_guard<std::mutex> lock(mutex);
                pool.ordered_free(ptr, count);
            }

            void release_memory() noexcept override
            {
                std::lock_guard<std::mutex> lock(mutex);
                pool.release_memory();
            }

            IMemPool& mem_pool(
                    size_t object_size, bool optimize) noexcept override
            {
                return root.mem_pool(object_size, optimize);
            }

        private:
            IMemPool& root;
            boost::pool<boost::default_user_allocator_malloc_free> pool;
            std::mutex mutex;
        };

        /**
         * @brief Manager of size-specific allocators
         */
        class MemPoolManager final : public IMemPool
        {
        public:
            ~MemPoolManager() override
            {
                for (auto& kv : pools) {
                    delete kv.second;
                }

                pools.clear();
            }

            void* allocate(size_t object_size, size_t count) noexcept override
            {
                return mem_pool(object_size).allocate(object_size, count);
            }

            void deallocate(
                    void* ptr,
                    size_t object_size,
                    size_t count) noexcept override
            {
                return mem_pool(object_size)
                        .deallocate(ptr, object_size, count);
            }

            void release_memory() noexcept override
            {
                std::lock_guard<std::mutex> lock(mutex);

                for (auto& kv : pools) {
                    kv.second->release_memory();
                }
            }

            IMemPool& mem_pool(
                    size_t object_size, bool optimize = false) noexcept override
            {
                std::lock_guard<std::mutex> lock(mutex);

                auto iter = pools.find(object_size);

                if (iter != pools.end()) {
                    return *(iter->second);
                }

                if (optimize) {
                    iter = pools.emplace(
                                        std::piecewise_construct,
                                        std::forward_as_tuple(object_size),
                                        std::forward_as_tuple(new BoostMemPool(
                                                *this, object_size)))
                                   .first;
                    return *(iter->second);
                }

                return GlobalMemPool::get_common();
            }

        private:
            std::map<size_t, IMemPool*> pools;
            std::mutex mutex;
        };
    } // namespace ri
} // namespace futoin

//---
#endif // FUTOIN_RI_MEMPOOL_HPP
