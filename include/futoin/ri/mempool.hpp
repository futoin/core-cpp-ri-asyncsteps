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
//---

namespace futoin {
    namespace ri {
        /**
         * @brief boost::pool-based type-erased memory pool
         */
        class BoostMemPool final : public IMemPool
        {
        public:
            BoostMemPool(size_t requested_size) noexcept : pool(requested_size)
            {}

            void* allocate(
                    size_t /*object_size*/, size_t count) noexcept override
            {
                return pool.ordered_malloc(count);
            }

            void deallocate(
                    void* ptr,
                    size_t /*object_size*/,
                    size_t count) noexcept override
            {
                pool.ordered_free(ptr, count);
            }

            void release_memory() noexcept override
            {
                pool.release_memory();
            }

        private:
            boost::pool<boost::default_user_allocator_malloc_free> pool;
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
                auto iter = pools.find(object_size);

                if (iter == pools.end()) {
                    iter = pools.emplace(
                                        std::piecewise_construct,
                                        std::forward_as_tuple(object_size),
                                        std::forward_as_tuple(
                                                new BoostMemPool(object_size)))
                                   .first;
                }

                return iter->second->allocate(object_size, count);
            }

            void deallocate(
                    void* ptr,
                    size_t object_size,
                    size_t count) noexcept override
            {
                pools[object_size]->deallocate(ptr, object_size, count);
            }

            void release_memory() noexcept override
            {
                for (auto& kv : pools) {
                    kv.second->release_memory();
                }
            }

        private:
            std::map<size_t, IMemPool*> pools;
        };
    } // namespace ri
} // namespace futoin

//---
#endif // FUTOIN_RI_MEMPOOL_HPP
