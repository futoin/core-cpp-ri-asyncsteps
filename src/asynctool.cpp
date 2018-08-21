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

#include <futoin/ri/asynctool.hpp>

#include <cassert>
#include <deque>
#include <list>
#include <queue>
//---
#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>

namespace futoin {
    namespace ri {
        using std::chrono::steady_clock;
        using lock_guard = std::lock_guard<std::mutex>;

        constexpr size_t AsyncTool::BURST_COUNT;

        struct AsyncTool::Impl
        {
            using ImmediateHandle = InternalHandle;

            struct DeferredHandle : ImmediateHandle
            {
                DeferredHandle(Callback&& cb, steady_clock::time_point when) :
                    ImmediateHandle(std::forward<Callback>(cb)), when(when)
                {}

                steady_clock::time_point when;
            };

            template<typename T>
            struct DeferredCompare
            {
                bool operator()(const T& a, const T& b)
                {
                    return a->when > b->when;
                }
            };

            Impl() : is_shutdown(false) {}

            ~Impl() noexcept
            {
                is_shutdown = true;

                if (thread) {
                    if (std::this_thread::get_id() == thread->get_id()) {
                        std::cerr << "FATAL: invalid d-tor call" << std::endl;
                        std::terminate();
                    }

                    {
                        lock_guard lt(handle_mutex);
                        poke();
                    }

                    thread->join();
                }

                lock_guard le(exec_mutex);
                lock_guard lt(handle_mutex);

                for (auto& v : handle_tasks) {
                    v();
                }

                for (auto& v : immed_queue) {
                    v.callback = Callback();

                    if (v.outer != nullptr) {
                        HandleAccessor(*(v.outer)).internal_ = nullptr;
                        v.outer = nullptr;
                    }
                }

                for (auto& v : defer_used_heap) {
                    v.callback = Callback();

                    if (v.outer != nullptr) {
                        HandleAccessor(*(v.outer)).internal_ = nullptr;
                        v.outer = nullptr;
                    }
                }
            }

            void poke() noexcept
            {
                poke_cb();
            }

            void process() noexcept;
            void iterate(std::unique_lock<std::mutex>& exec_lock) noexcept;

            //---
            std::mutex exec_mutex;

            //---
            std::deque<ImmediateHandle> immed_queue;

            using DeferredHeap = std::list<DeferredHandle>;
            DeferredHeap defer_used_heap;
            DeferredHeap defer_free_heap;
            size_t canceled_handles{0};

            using DeferredQueueItem = DeferredHeap::iterator;
            using DeferredPriorityQueue = std::priority_queue<
                    DeferredQueueItem,
                    std::vector<DeferredQueueItem>,
                    DeferredCompare<DeferredQueueItem>>;
            DeferredPriorityQueue defer_queue;

            //---
            std::condition_variable poke_var;

            //---
            std::mutex handle_mutex;
            using HandleTask = std::packaged_task<Handle()>;
            std::deque<HandleTask> handle_tasks;

            //---
            std::atomic_bool is_shutdown{false};
            std::function<void()> poke_cb;
            std::thread::id reactor_thread_id;
            std::unique_ptr<std::thread> thread;
        };

        AsyncTool::AsyncTool() noexcept : impl_(new Impl)
        {
            auto& poke_var = impl_->poke_var;
            impl_->poke_cb = [&]() { poke_var.notify_one(); };

            impl_->thread.reset(new std::thread{&Impl::process, impl_.get()});
            impl_->reactor_thread_id = impl_->thread->get_id();
        }

        AsyncTool::AsyncTool(std::function<void()> poke_external) noexcept :
            impl_(new Impl)
        {
            impl_->poke_cb = std::move(poke_external);
            impl_->reactor_thread_id = std::this_thread::get_id();
        }

        AsyncTool::~AsyncTool() noexcept = default;

        AsyncTool::Handle AsyncTool::immediate(Callback&& cb) noexcept
        {
            if (!AsyncTool::is_same_thread()) {
                Impl::HandleTask task([this, &cb]() {
                    return this->immediate(std::forward<Callback>(cb));
                });
                auto res = task.get_future();

                {
                    std::lock_guard<std::mutex> lock(impl_->handle_mutex);
                    impl_->handle_tasks.emplace_back(std::move(task));
                }

                impl_->poke();
                return res.get();
            }

            auto& q = impl_->immed_queue;
            q.emplace_back(std::forward<Callback>(cb));
            return {q.back(), *this};
        }

        AsyncTool::Handle AsyncTool::deferred(
                std::chrono::milliseconds delay, Callback&& cb) noexcept
        {
            if (!AsyncTool::is_same_thread()) {
                Impl::HandleTask task([this, delay, &cb]() {
                    return this->deferred(delay, std::forward<Callback>(cb));
                });
                auto res = task.get_future();

                {
                    std::lock_guard<std::mutex> lock(impl_->handle_mutex);
                    impl_->handle_tasks.emplace_back(std::move(task));
                }

                impl_->poke();
                return res.get();
            }

            auto when = steady_clock::now() + delay;

            auto& free_heap = impl_->defer_free_heap;
            auto& used_heap = impl_->defer_used_heap;
            auto& q = impl_->defer_queue;

            Impl::DeferredQueueItem it;

            if (free_heap.empty()) {
                used_heap.emplace_front(std::forward<Callback>(cb), when);
                it = used_heap.begin();
            } else {
                it = free_heap.begin();
                *it = Impl::DeferredHandle(std::forward<Callback>(cb), when);
                used_heap.splice(used_heap.begin(), free_heap, it);
            }

            q.push(it);
            return {*it, *this};
        }

        bool AsyncTool::is_same_thread() noexcept
        {
            return std::this_thread::get_id() == impl_->reactor_thread_id;
        }

        void AsyncTool::Impl::process() noexcept
        {
            std::unique_lock<std::mutex> exec_lock(exec_mutex);

            for (;;) {
                iterate(exec_lock);

                std::unique_lock<std::mutex> lock(handle_mutex);

                if (is_shutdown) {
                    break;
                }

                if (immed_queue.empty() && handle_tasks.empty()) {
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

        AsyncTool::CycleResult AsyncTool::iterate() noexcept
        {
            if (!is_same_thread()) {
                std::cerr << "FATAL: AsyncTool::iterate() must be called from "
                             "c-tor thread!"
                          << std::endl;
                std::terminate();
            }

            std::unique_lock<std::mutex> lock(impl_->exec_mutex);
            impl_->iterate(lock);

            using std::chrono::milliseconds;

            if (impl_->immed_queue.empty()) {
                if (impl_->defer_queue.empty()) {
                    return {false, milliseconds(0)};
                }

                auto delay = impl_->defer_queue.top()->when
                             - steady_clock::now() + milliseconds(1);
                return {true, std::chrono::duration_cast<milliseconds>(delay)};
            }

            return {true, milliseconds(0)};
        }

        void AsyncTool::Impl::iterate(
                std::unique_lock<std::mutex>& /*exec_lock*/) noexcept
        {
            size_t immed_to_remove = 0;

            // process immediates
            {
                auto immed_iter = immed_queue.begin();
                const auto immed_end = (immed_queue.size() < BURST_COUNT)
                                               ? immed_queue.end()
                                               : immed_iter + BURST_COUNT;

                for (; immed_iter != immed_end; ++immed_iter) {
                    auto& cb = immed_iter->callback;

                    if (cb) {
                        cb();
                    } else {
                        --canceled_handles;
                    }

                    ++immed_to_remove;
                }
            }

            // process deferred
            const auto now = steady_clock::now();
            DeferredHeap defer_just_freed;

            for (size_t i = std::min(BURST_COUNT, defer_queue.size()); i > 0;
                 --i) {
                auto& h_it = defer_queue.top();

                if (h_it->when > now) {
                    break;
                }

                auto& cb = h_it->callback;

                if (cb) {
                    cb();
                } else {
                    --canceled_handles;
                }

                defer_just_freed.splice(
                        defer_just_freed.begin(), defer_used_heap, h_it);

                defer_queue.pop();
            }

            // cleanup & requests
            {
                lock_guard lock(handle_mutex);

                for (size_t i = immed_to_remove; i > 0; --i) {
                    auto& h = immed_queue.front();

                    auto outer = h.outer;

                    if (outer != nullptr) {
                        HandleAccessor(*outer).internal_ = nullptr;
                        h.outer = nullptr;
                    }

                    immed_queue.pop_front();
                }

                while (!defer_just_freed.empty()) {
                    auto h_it = defer_just_freed.begin();

                    auto outer = h_it->outer;

                    if (outer != nullptr) {
                        HandleAccessor(*outer).internal_ = nullptr;
                        h_it->outer = nullptr;
                    }

                    defer_free_heap.splice(
                            defer_free_heap.begin(), defer_just_freed, h_it);
                }

                // TODO: temporary workaround -> use heap on list instead
                if (canceled_handles > (defer_used_heap.size() / 2)) {
                    auto iter = defer_used_heap.begin();
                    const auto end = defer_used_heap.end();

                    defer_queue = DeferredPriorityQueue();

                    while (iter != end) {
                        if (iter->callback) {
                            defer_queue.push(iter);
                            ++iter;
                        } else {
                            --canceled_handles;
                            auto to_move = iter;
                            ++iter;
                            defer_free_heap.splice(
                                    defer_free_heap.begin(),
                                    defer_used_heap,
                                    to_move);
                        }
                    }
                }

                while (!handle_tasks.empty()) {
                    handle_tasks.front()();
                    handle_tasks.pop_front();
                }
            }
        }

        void AsyncTool::cancel(Handle& h) noexcept
        {
            HandleAccessor ha(h);
            std::unique_lock<std::mutex> lock(
                    impl_->handle_mutex, std::defer_lock);

            if (!is_same_thread()) {
                lock.lock();

                if (ha.internal_ == nullptr) {
                    return;
                }
            }

            auto internal = static_cast<Impl::ImmediateHandle*>(ha.internal_);
            internal->callback = Callback();
            internal->outer = nullptr;
            ha.internal_ = nullptr;

            ++(impl_->canceled_handles);
        }

        void AsyncTool::move(Handle& src, Handle& dst) noexcept
        {
            HandleAccessor srca(src);
            HandleAccessor dsta(dst);
            std::unique_lock<std::mutex> lock(
                    impl_->handle_mutex, std::defer_lock);

            if (!is_same_thread()) {
                lock.lock();

                if (srca.internal_ == nullptr) {
                    dsta.internal_ = nullptr;
                    return;
                }
            }

            dsta.internal_ = srca.internal_;
            dsta.async_tool_ = srca.async_tool_;
            dsta.internal_->outer = &dst;

            srca.internal_ = nullptr;
            srca.async_tool_ = nullptr;
        }

        void AsyncTool::free(Handle& h) noexcept
        {
            HandleAccessor ha(h);
            std::unique_lock<std::mutex> lock(
                    impl_->handle_mutex, std::defer_lock);

            if (!is_same_thread()) {
                lock.lock();

                if (ha.internal_ == nullptr) {
                    return;
                }
            }

            ha.internal_->outer = nullptr;
            ha.internal_ = nullptr;
        }

        AsyncTool::Stats AsyncTool::stats() noexcept
        {
            lock_guard lock(impl_->handle_mutex);

            return {
                    impl_->immed_queue.size(),
                    impl_->defer_used_heap.size(),
                    impl_->defer_free_heap.size(),
                    impl_->handle_tasks.size(),
            };
        }

        void AsyncTool::shrink_to_fit() noexcept
        {
            if (!is_same_thread()) {
                Impl::HandleTask task([this]() {
                    this->shrink_to_fit();
                    return Handle();
                });
                auto res = task.get_future();

                {
                    std::lock_guard<std::mutex> lock(impl_->handle_mutex);
                    impl_->handle_tasks.emplace_back(std::move(task));
                }

                impl_->poke();
                res.wait();
            } else {
                impl_->immed_queue.shrink_to_fit();
                impl_->handle_tasks.shrink_to_fit();
                impl_->defer_free_heap.clear();
            }
        }
    } // namespace ri
} // namespace futoin
