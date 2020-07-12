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
#include <futoin/fatalmsg.hpp>
#include <futoin/imempool.hpp>
//---
#include <array>
#include <boost/pool/pool.hpp>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
//---

namespace futoin {
    namespace ri {
        /**
         * @brief boost::pool-based type-erased memory pool
         */
        template<typename Mutex>
        class BoostMemPool : public IMemPool
        {
        public:
            BoostMemPool(size_t requested_size) noexcept :
                pool(requested_size, 16 * 1024 / requested_size)
            {}

            void* allocate(size_t object_size, size_t count) noexcept final
            {
                if (object_size != pool.get_requested_size()) {
                    FatalMsg() << "invalid optimized allocator use"
                               << " object_size=" << object_size
                               << " pool_size=" << pool.get_requested_size();
                }

                std::lock_guard<Mutex> lock(mutex);

                if (count == 1) {
                    return pool.ordered_malloc();
                }

                return pool.ordered_malloc(count);
            }

            void deallocate(
                    void* ptr,
                    size_t /*object_size*/,
                    size_t count) noexcept final
            {
                std::lock_guard<Mutex> lock(mutex);

                if (count == 1) {
                    pool.free(ptr);
                } else {
                    pool.free(ptr, count);
                }
            }

            void release_memory() noexcept final
            {
                std::lock_guard<Mutex> lock(mutex);
                pool.release_memory();
            }

        private:
            boost::pool<boost::default_user_allocator_malloc_free> pool;
            Mutex mutex;
        };

        template<typename Base>
        class OptimizeableMemPool final : public Base
        {
        public:
            OptimizeableMemPool(IMemPool& root) noexcept : root(root) {}

            template<typename... Args>
            OptimizeableMemPool(IMemPool& root, Args&&... args) noexcept :
                Base(std::forward<Args>(args)...), root(root)
            {}

            IMemPool& mem_pool(size_t object_size, bool optimize) noexcept final
            {
                return root.mem_pool(object_size, optimize);
            }

        private:
            IMemPool& root;
        };

        /**
         * @brief Manager of size-specific allocators
         */
        template<typename Mutex = std::mutex>
        class MemPoolManager final : public IMemPool
        {
        public:
            MemPoolManager() noexcept
            {
                auto res = std::getenv("FUTOIN_USE_MEMPOOL");
                allow_optimize_ =
                        ((res == nullptr) || (std::strcmp(res, "true") == 0));
            }

            ~MemPoolManager() noexcept final = default;

            void* allocate(size_t object_size, size_t count) noexcept final
            {
                return mem_pool(object_size).allocate(object_size, count);
            }

            void deallocate(
                    void* ptr, size_t object_size, size_t count) noexcept final
            {
                return mem_pool(object_size)
                        .deallocate(ptr, object_size, count);
            }

            void release_memory() noexcept final
            {
                std::lock_guard<Mutex> lock(mutex);

                for (auto& p : pools) {
                    if (p) {
                        p->release_memory();
                    }
                }
            }

            IMemPool& mem_pool(
                    size_t object_size, bool optimize = false) noexcept final
            {
                if (optimize && allow_optimize_) {
                    size_t aligned_size = (object_size + sizeof(ptrdiff_t) - 1)
                                          / sizeof(ptrdiff_t);
                    auto key = aligned_size - 1;

                    if (key < pools.size()) {
                        auto& p = pools[key];

                        if (!p) {
                            std::lock_guard<Mutex> lock(mutex);

                            if (!p) {
                                auto pool_size =
                                        aligned_size * sizeof(ptrdiff_t);

                                using PoolType = OptimizeableMemPool<
                                        BoostMemPool<Mutex>>;
                                p.reset(new PoolType(*this, pool_size));
                            }
                        }

                        return *p;
                    }

                    FatalMsg()
                            << "unable to optimize object_size=" << object_size;
                }

                return default_pool;
            }

        private:
            // MAX_OBJECT_SIZE_IN_POINTERS x ptrdiff_t
            static constexpr size_t MAX_OBJECT_SIZE_IN_POINTERS = 128;
            std::array<std::unique_ptr<IMemPool>, MAX_OBJECT_SIZE_IN_POINTERS>
                    pools;
            Mutex mutex;
            OptimizeableMemPool<PassthroughMemPool> default_pool{*this};
            bool allow_optimize_;
        };
    } // namespace ri
} // namespace futoin

//---
#endif // FUTOIN_RI_MEMPOOL_HPP
