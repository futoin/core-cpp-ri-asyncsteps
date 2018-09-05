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
#include <array>
#include <boost/pool/pool.hpp>
#include <iostream>
#include <mutex>
//---

namespace futoin {
    namespace ri {
        /**
         * @brief boost::pool-based type-erased memory pool
         */
        template<typename Mutex>
        class BoostMemPool final : public IMemPool
        {
        public:
            BoostMemPool(IMemPool& root, size_t requested_size) noexcept :
                root(root),
                pool(requested_size, 16 * 1024 / requested_size)
            {}

            void* allocate(size_t object_size, size_t count) noexcept override
            {
                if (count != 1) {
                    std::cout
                            << "FATAL: optimized allocator was used for arrays "
                               "object_size="
                            << object_size << std::endl;
                    std::terminate();
                }

                if (object_size != pool.get_requested_size()) {
                    std::cout << "FATAL: invalid optimized allocator use"
                              << " object_size=" << object_size
                              << " pool_size=" << pool.get_requested_size()
                              << std::endl;
                    std::terminate();
                }

                std::lock_guard<Mutex> lock(mutex);
                return pool.malloc();
            }

            void deallocate(
                    void* ptr,
                    size_t /*object_size*/,
                    size_t count) noexcept override
            {
                std::lock_guard<Mutex> lock(mutex);
                pool.free(ptr, count);
            }

            void release_memory() noexcept override
            {
                std::lock_guard<Mutex> lock(mutex);
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
            Mutex mutex;
        };

        class PassthroughMemPool final : public futoin::PassthroughMemPool
        {
        public:
            PassthroughMemPool(IMemPool& root) noexcept : root(root) {}

            IMemPool& mem_pool(
                    size_t object_size, bool optimize) noexcept override
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
            ~MemPoolManager() noexcept override
            {
                for (auto& p : pools) {
                    if (p != nullptr) {
                        delete p;
                        p = nullptr;
                    }
                }
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
                std::lock_guard<Mutex> lock(mutex);

                for (auto p : pools) {
                    if (p != nullptr) {
                        p->release_memory();
                    }
                }
            }

            IMemPool& mem_pool(
                    size_t object_size, bool optimize = false) noexcept override
            {
                if (optimize) {
                    size_t aligned_size = (object_size + sizeof(ptrdiff_t) - 1)
                                          / sizeof(ptrdiff_t);
                    auto key = aligned_size - 1;

                    if (key < pools.size()) {
                        auto p = pools[key];

                        if (p == nullptr) {
                            std::lock_guard<Mutex> lock(mutex);
                            p = pools[key];

                            if (p == nullptr) {
                                auto pool_size =
                                        aligned_size * sizeof(ptrdiff_t);

                                p = new BoostMemPool<Mutex>(*this, pool_size);
                                pools[key] = p;
                            }
                        }

                        return *p;
                    }

                    std::cout << "FATAL: unable to optimize "
                                 "object_size="
                              << object_size << std::endl;
                    std::terminate();
                }

                return default_pool;
            }

        private:
            // MAX_OBJECT_SIZE_IN_POINTERS x ptrdiff_t
            static constexpr size_t MAX_OBJECT_SIZE_IN_POINTERS = 128;
            std::array<IMemPool*, MAX_OBJECT_SIZE_IN_POINTERS> pools{nullptr};
            Mutex mutex;
            PassthroughMemPool default_pool{*this};
        };
    } // namespace ri
} // namespace futoin

//---
#endif // FUTOIN_RI_MEMPOOL_HPP
