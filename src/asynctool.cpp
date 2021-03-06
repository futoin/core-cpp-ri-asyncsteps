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

#include <futoin/fatalmsg.hpp>
#include <futoin/ri/asyncsteps.hpp>
#include <futoin/ri/asynctool.hpp>
#include <futoin/ri/mempool.hpp>

#include <cassert>
#include <iostream>
#include <list>
//---
#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
//---
// Make clang-tidy happy with Boost 1.67
#include <boost/next_prior.hpp>
//---
#include <boost/heap/priority_queue.hpp>
#include <boost/lockfree/spsc_queue.hpp>
#include <boost/pool/object_pool.hpp>

namespace futoin {
    namespace ri {
        using clock_type = std::chrono::steady_clock;
        using lock_guard = std::lock_guard<std::mutex>;

        constexpr size_t AsyncTool::BURST_COUNT;

        template<typename T>
        struct optimized_pool : boost::object_pool<T>
        {
            using Base = boost::object_pool<T>;
            static constexpr size_t MinSize = AsyncTool::BURST_COUNT;
            static constexpr size_t MaxSize = MinSize * MinSize * MinSize;

            optimized_pool() : Base(MinSize, MaxSize) {}

            using Base::release_memory;
        };

        template<typename T>
        struct optimized_list_node
        {
            T data;
            optimized_list_node* prev{this};
            optimized_list_node* next{this};
        };

        template<
                typename T,
                typename Allocator = optimized_pool<optimized_list_node<T>>>
        class optimized_list
        {
        public:
            using allocator = Allocator;
            using node = optimized_list_node<T>;

            struct iterator
            {
                iterator() noexcept = default;

                explicit iterator(node* n) noexcept : node_(n) {}

                T* operator->()
                {
                    return &(node_->data);
                }

                const T* operator->() const
                {
                    return &(node_->data);
                }

                T& operator*()
                {
                    return node_->data;
                }

                void operator--()
                {
                    node_ = node_->prev;
                }

                void operator++()
                {
                    node_ = node_->next;
                }

                bool operator==(const iterator& other) const
                {
                    return node_ == other.node_;
                }

                bool operator!=(const iterator& other) const
                {
                    return node_ != other.node_;
                }

                node* node_;
            };

            optimized_list(Allocator& allocator) : allocator_(allocator) {}

            iterator begin()
            {
                return iterator(anchor_.next);
            }

            iterator end()
            {
                return iterator(&anchor_);
            }

            void emplace_front()
            {
                auto node = allocator_.construct();
                node->next = anchor_.next;
                node->prev = &anchor_;
                node->next->prev = node;
                anchor_.next = node;
                ++size_;
            }

            void emplace_back()
            {
                auto node = allocator_.construct();
                node->next = &anchor_;
                node->prev = anchor_.prev;
                node->prev->next = node;
                anchor_.prev = node;
                ++size_;
            }

            bool empty() const
            {
                return size_ == 0;
            }

            void clear()
            {
                for (node* curr = anchor_.next; curr != &anchor_;) {
                    node* next = curr->next;
                    allocator_.destroy(curr);
                    curr = next;
                }

                anchor_.next = &anchor_;
                anchor_.prev = &anchor_;
                size_ = 0;
            }

            size_t size() const
            {
                return size_;
            }

            void splice(iterator pos, optimized_list& other, iterator other_pos)
            {
                node* src = other_pos.node_;
                node* dst = pos.node_;

                src->prev->next = src->next;
                src->next->prev = src->prev;
                other.size_--;

                src->prev = dst->prev;
                src->next = dst;
                dst->prev = src;
                src->prev->next = src;
                ++size_;
            }

            void splice(
                    iterator pos,
                    optimized_list& other,
                    iterator other_start,
                    iterator other_end)
            {
                node* src_start = other_start.node_;
                --other_end;
                node* src_end = other_end.node_;
                node* dst = pos.node_;

                src_start->prev->next = src_end->next;
                src_end->next->prev = src_start->prev;

                src_start->prev = dst->prev;
                src_end->next = dst;
                dst->prev = src_end;
                src_start->prev->next = src_start;

                //---
                size_t total = 1;

                for (; src_start != src_end; src_start = src_start->next) {
                    ++total;
                }

                other.size_ -= total;
                size_ += total;
            }

        private:
            Allocator& allocator_;
            node anchor_;
            size_t size_{0};
        };

        struct AsyncTool::Impl
        {
            struct UniversalHandle : InternalHandle
            {
                UniversalHandle() = default;

                UniversalHandle(const UniversalHandle& other) noexcept = delete;
                UniversalHandle& operator=(
                        const UniversalHandle& other) noexcept = delete;
                UniversalHandle(UniversalHandle&& other) noexcept = delete;
                UniversalHandle& operator=(UniversalHandle&& other) noexcept =
                        delete;

                ~UniversalHandle() noexcept = default;

                HandleCookie cookie{0};
                clock_type::time_point when;
            };

            template<typename T>
            struct DeferredCompare
            {
                bool operator()(const T& a, const T& b) const
                {
                    return a->when > b->when;
                }
            };

            using HandleTask = Callback;

            Impl(const Params& params) : params(params), is_shutdown(false)
            {
                if (params.mempool_mutex) {
                    mem_pool.reset(new MemPoolManager<std::mutex>);
                } else {
                    mem_pool.reset(new MemPoolManager<ISync::NoopOSMutex>);
                }
            }

            ~Impl() noexcept
            {
                is_shutdown = true;

                if (thread) {
                    if (std::this_thread::get_id() == thread->get_id()) {
                        FatalMsg() << "invalid d-tor call";
                    }

                    {
                        lock_guard lt(handle_mutex);
                        poke();
                    }

                    thread->join();
                }

                handle_task_queue();
            }

            // NOLINTNEXTLINE(readability-make-member-function-const)
            void poke() noexcept
            {
                poke_cb();
            }

            void process() noexcept;
            void iterate() noexcept;

            HandleCookie get_cookie() noexcept
            {
                auto cookie = ++current_cookie;

                if (cookie == 0) {
                    cookie = ++current_cookie;
                }

                return cookie;
            }

            void handle_task_queue()
            {
                // Process external requests
                for (size_t c = handle_tasks.read_available(); c > 0; --c) {
                    handle_tasks.front()->operator()();
                    handle_tasks.pop();
                }
            }

            void add_handle_task(HandleTask& task)
            {
                for (bool done = false;;) {
                    lock_guard lock(handle_mutex);
                    done = handle_tasks.push(&task);

                    if (done) {
                        poke();
                        break;
                    }

                    std::this_thread::yield();
                }
            }

            //---
            // using UniversalHeap = std::list<UniversalHandle>;

            using UniversalAllocator =
                    optimized_list<UniversalHandle>::allocator;
            using UniversalHeap = optimized_list<UniversalHandle>;

            Params params;
            HandleCookie current_cookie{1};

            UniversalAllocator handle_allocator_;
            UniversalHeap immed_queue{handle_allocator_};
            UniversalHeap defer_used_heap{handle_allocator_};
            UniversalHeap universal_free_heep{handle_allocator_};
            size_t canceled_handles{0};

            using DeferredQueueItem = UniversalHeap::iterator;
            using DeferredPriorityQueue = boost::heap::priority_queue<
                    DeferredQueueItem,
                    boost::heap::compare<DeferredCompare<DeferredQueueItem>>>;
            DeferredPriorityQueue defer_queue;

            //---
            std::condition_variable poke_var;

            //---
            std::mutex handle_mutex;
            boost::lockfree::spsc_queue<
                    HandleTask*,
                    boost::lockfree::capacity<BURST_COUNT * 10>>
                    handle_tasks;

            //---
            std::atomic_bool is_shutdown{false};
            PokeCallback poke_cb;
            std::thread::id reactor_thread_id;
            std::unique_ptr<std::thread> thread;
            std::unique_ptr<IMemPool> mem_pool;

            //---
            const clock_type::time_point& now()
            {
                if (!use_last_now) {
                    last_now = clock_type::now();
                    use_last_now = true;
                }

                return last_now;
            }

            void forget_now()
            {
                use_last_now = false;
            }

            clock_type::time_point last_now;
            bool use_last_now{false};
        };

        AsyncTool::AsyncTool(const Params& params) noexcept :
            impl_(new Impl(params))
        {
            auto& poke_var = impl_->poke_var;
            impl_->poke_cb = [&]() { poke_var.notify_one(); };

            impl_->thread.reset(new std::thread{&Impl::process, impl_.get()});
            impl_->reactor_thread_id = impl_->thread->get_id();
        }

        AsyncTool::AsyncTool(
                PokeCallback poke_external, const Params& params) noexcept :
            impl_(new Impl(params))
        {
            impl_->poke_cb = std::move(poke_external);
            impl_->reactor_thread_id = std::this_thread::get_id();
        }

        AsyncTool::~AsyncTool() noexcept = default;

        AsyncTool::Handle AsyncTool::immediate(CallbackPass&& cb) noexcept
        {
            if (!AsyncTool::is_same_thread()) {
                std::promise<AsyncTool::Handle> res;
                auto func = [this, &res, &cb]() {
                    res.set_value(
                            this->immediate(std::forward<CallbackPass>(cb)));
                };
                Impl::HandleTask task = std::ref(func);

                impl_->add_handle_task(task);
                return res.get_future().get();
            }

            auto& free_heap = impl_->universal_free_heep;
            auto& q = impl_->immed_queue;

            Impl::DeferredQueueItem it;
            auto cookie = impl_->get_cookie();

            if (free_heap.empty()) {
                q.emplace_back();
                it = q.end();
                --it;
            } else {
                it = free_heap.begin();
                q.splice(q.end(), free_heap, it);
            }

            auto& h = *it;
            cb.move(h.callback, h.storage);
            h.cookie = cookie;

            return {h, *this, cookie};
        }

        AsyncTool::Handle AsyncTool::deferred(
                std::chrono::milliseconds delay, CallbackPass&& cb) noexcept
        {
            if (!AsyncTool::is_same_thread()) {
                std::promise<AsyncTool::Handle> res;
                auto func = [this, &res, &cb, delay]() {
                    res.set_value(this->deferred(
                            delay, std::forward<CallbackPass>(cb)));
                };
                Impl::HandleTask task = std::ref(func);

                impl_->add_handle_task(task);
                return res.get_future().get();
            }

            if (delay < std::chrono::milliseconds(100)) {
                FatalMsg()
                        << "deferred AsyncTool calls are designed for timeouts!"
                        << std::endl
                        << "Avoid using it for too short delays (<100ms).";
            }

            auto when = impl_->now() + delay;

            auto& free_heap = impl_->universal_free_heep;
            auto& used_heap = impl_->defer_used_heap;
            auto& q = impl_->defer_queue;

            Impl::DeferredQueueItem it;
            auto cookie = impl_->get_cookie();

            if (free_heap.empty()) {
                used_heap.emplace_front();
                it = used_heap.begin();
            } else {
                it = free_heap.begin();
                used_heap.splice(used_heap.begin(), free_heap, it);
            }

            auto& h = *it;
            cb.move(h.callback, h.storage);
            h.cookie = cookie;
            h.when = when;
            q.push(it);

            return {h, *this, cookie};
        }

        bool AsyncTool::is_same_thread() noexcept
        {
            return std::this_thread::get_id() == impl_->reactor_thread_id;
        }

        void AsyncTool::Impl::process() noexcept
        {
            GlobalMemPool::set_thread_default(*mem_pool);

            while (!is_shutdown.load(std::memory_order_relaxed)) {
                iterate();

                if (immed_queue.empty() && handle_tasks.empty()) {
                    std::unique_lock<std::mutex> lock(handle_mutex);

                    if (immed_queue.empty() && handle_tasks.empty()) {
                        if (is_shutdown.load(std::memory_order_relaxed)) {
                            break;
                        }

                        forget_now();

                        if (defer_queue.empty()) {
                            poke_var.wait(lock);
                        } else {
                            auto when = defer_queue.top()->when
                                        + std::chrono::milliseconds(1);
                            poke_var.wait_until(lock, when);
                        }
                    }
                }
            }

            GlobalMemPool::reset_thread_default();
        }

        AsyncTool::CycleResult AsyncTool::iterate() noexcept
        {
            if (!is_same_thread()) {
                FatalMsg() << "AsyncTool::iterate() must be called from "
                              "c-tor thread!";
            }

            impl_->iterate();

            using std::chrono::milliseconds;
            auto have_work = true;
            auto delay = milliseconds(0);

            if (impl_->immed_queue.empty()) {
                if (impl_->defer_queue.empty()) {
                    have_work = false;
                } else {
                    delay = std::chrono::duration_cast<milliseconds>(
                            impl_->defer_queue.top()->when - impl_->now()
                            + milliseconds(1));
                }
            }

            impl_->forget_now();
            return {have_work, delay};
        }

        void AsyncTool::Impl::iterate() noexcept
        {
            forget_now();
            auto immed_begin = immed_queue.begin();
            auto iter = immed_begin;

            // process immediates
            for (size_t i = BURST_COUNT; (i > 0) && (iter != immed_queue.end());
                 --i, ++iter) {
                auto& h = *iter;
                auto& cookie = h.cookie;

                if (cookie != 0) {
                    cookie = 0;
                    h.callback();
                } else {
                    --canceled_handles;
                }
            }

            if (immed_begin != iter) {
                universal_free_heep.splice(
                        universal_free_heep.begin(),
                        immed_queue,
                        immed_begin,
                        iter);
            }

            if (!defer_queue.empty()) {
                const auto now = this->now();

                // NOTE: it's assumed deferred calls are almost always canceled,
                // but not executed!
                for (size_t i = BURST_COUNT; (i > 0) && !defer_queue.empty();
                     --i) {
                    iter = defer_queue.top();

                    auto& h = *iter;
                    auto& cookie = h.cookie;

                    if (cookie != 0) {
                        if (h.when > now) {
                            break;
                        }

                        cookie = 0;
                        h.callback();
                    } else {
                        --canceled_handles;
                    }

                    universal_free_heep.splice(
                            universal_free_heep.begin(), defer_used_heap, iter);
                    defer_queue.pop();
                }

                // TODO: redesign
                if (canceled_handles > (defer_used_heap.size() / 2)) {
                    auto iter = defer_used_heap.begin();
                    const auto end = defer_used_heap.end();

                    defer_queue.clear();

                    while (iter != end) {
                        if (iter->cookie != 0) {
                            defer_queue.push(iter);
                            ++iter;
                        } else {
                            --canceled_handles;
                            auto to_move = iter;
                            ++iter;
                            universal_free_heep.splice(
                                    universal_free_heep.begin(),
                                    defer_used_heap,
                                    to_move);
                        }
                    }
                }
            }

            // Process external requests
            handle_task_queue();
        }

        void AsyncTool::cancel(Handle& h) noexcept
        {
            HandleAccessor ha(h);

            auto internal = ha.internal();

            if (internal != nullptr) {
                ha.internal() = nullptr;
                auto universal = static_cast<Impl::UniversalHandle*>(internal);
                auto ha_cookie = ha.cookie();

                if (universal->cookie != ha_cookie) {
                    // pass
                } else if (!is_same_thread()) {
                    std::promise<void> res;
                    auto func = [this, universal, ha_cookie, &res]() {
                        if (universal->cookie == ha_cookie) {
                            ++(impl_->canceled_handles);
                            universal->cookie = 0;
                        }
                        res.set_value();
                    };
                    Impl::HandleTask task = std::ref(func);

                    impl_->add_handle_task(task);
                    res.get_future().wait();
                } else {
                    ++(impl_->canceled_handles);
                    universal->cookie = 0;
                }
            }
        }

        bool AsyncTool::is_valid(Handle& h) noexcept
        {
            HandleAccessor ha(h);

            auto internal = ha.internal();

            if (internal == nullptr) {
                return false;
            }

            auto universal = static_cast<Impl::UniversalHandle*>(internal);

            return universal->cookie == ha.cookie();
        }

        AsyncTool::Stats AsyncTool::stats() noexcept
        {
            // not safe

            return {
                    impl_->immed_queue.size(),
                    impl_->defer_used_heap.size(),
                    impl_->universal_free_heep.size(),
                    impl_->handle_tasks.read_available(),
            };
        }

        void AsyncTool::release_memory() noexcept
        {
            if (is_same_thread()) {
                impl_->universal_free_heep.clear();
                impl_->handle_allocator_.release_memory();
                impl_->mem_pool->release_memory();
            } else {
                std::promise<void> res;
                auto func = [this, &res]() {
                    this->release_memory();
                    res.set_value();
                };
                Impl::HandleTask task = std::ref(func);

                impl_->add_handle_task(task);
                res.get_future().wait();
            }
        }

        IMemPool& AsyncTool::mem_pool(
                size_t object_size, bool optimize) noexcept
        {
            return impl_->mem_pool->mem_pool(object_size, optimize);
        }
    } // namespace ri
} // namespace futoin
