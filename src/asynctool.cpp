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

        struct AsyncTool::Impl
        {
            struct ImmediateHandle : InternalHandle
            {
                ImmediateHandle(Callback&& cb) :
                    InternalHandle(std::forward<Callback>(cb))
                {}

                bool canceled = false;
            };

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

            Impl() : have_tasks(false), is_shutdown(false) {}

            ~Impl() noexcept
            {
                is_shutdown = true;

                if (thread) {
                    if (std::this_thread::get_id() == thread->get_id()) {
                        std::cerr << "FATAL: invalid d-tor call" << std::endl;
                        std::terminate();
                    }

                    poke();
                    thread->join();
                }

                std::lock_guard<std::mutex> le(exec_mutex);
                std::lock_guard<std::mutex> lt(task_mutex);

                for (auto& v : handle_tasks) {
                    v();
                }

                for (auto& v : immed_queue) {
                    v.canceled = true;

                    if (v.outer != nullptr) {
                        HandleAccessor(*(v.outer)).internal_ = nullptr;
                        v.outer = nullptr;
                    }
                }

                for (auto& v : defer_used_heap) {
                    v.canceled = true;

                    if (v.outer != nullptr) {
                        HandleAccessor(*(v.outer)).internal_ = nullptr;
                        v.outer = nullptr;
                    }
                }
            }

            void poke() noexcept
            {
                poke_external();
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

            using DeferredQueueItem = DeferredHeap::iterator;
            std::priority_queue<
                    DeferredQueueItem,
                    std::vector<DeferredQueueItem>,
                    DeferredCompare<DeferredQueueItem>>
                    defer_queue;

            //---
            std::condition_variable poke_var;

            //---
            std::mutex task_mutex;
            using HandleTask = std::packaged_task<Handle()>;
            std::deque<HandleTask> handle_tasks;
            std::atomic_bool have_tasks = ATOMIC_FLAG_INIT;

            //---
            std::atomic_bool is_shutdown = ATOMIC_FLAG_INIT;
            std::function<void()> poke_external;
            std::thread::id reactor_thread_id;
            std::unique_ptr<std::thread> thread;

            static constexpr size_t IMMED_BURST = 100;
            static constexpr size_t DEFER_BURST = IMMED_BURST;
        };

        AsyncTool::AsyncTool() noexcept : impl_(new Impl)
        {
            auto& poke_var = impl_->poke_var;
            impl_->poke_external = [&]() { poke_var.notify_one(); };

            impl_->thread.reset(new std::thread{&Impl::process, impl_.get()});
            impl_->reactor_thread_id = impl_->thread->get_id();
        }

        AsyncTool::AsyncTool(std::function<void()> poke_external) noexcept :
            impl_(new Impl)
        {
            impl_->poke_external = std::move(poke_external);
            impl_->reactor_thread_id = std::this_thread::get_id();
        }

        AsyncTool::Handle AsyncTool::immediate(Callback&& cb) noexcept
        {
            if (!AsyncTool::is_same_thread()) {
                Impl::HandleTask task([this, &cb]() {
                    return this->immediate(std::forward<Callback>(cb));
                });
                auto res = task.get_future();

                {
                    std::lock_guard<std::mutex> lock(impl_->task_mutex);
                    impl_->handle_tasks.emplace_back(std::move(task));
                }

                impl_->poke();
                res.wait();
                return res.get();
            }

            auto& q = impl_->immed_queue;
            q.emplace_back(std::forward<Callback>(cb));
            return q.back();
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
                    std::lock_guard<std::mutex> lock(impl_->task_mutex);
                    impl_->handle_tasks.emplace_back(std::move(task));
                }

                impl_->poke();
                res.wait();
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
            return *it;
        }

        bool AsyncTool::is_same_thread() noexcept
        {
            return std::this_thread::get_id() == impl_->reactor_thread_id;
        }

        void AsyncTool::Impl::process() noexcept
        {
            while (!is_shutdown) {
                std::unique_lock<std::mutex> lock(exec_mutex);

                iterate(lock);

                if (immed_queue.empty()) {
                    if (defer_queue.empty()) {
                        poke_var.wait(lock);
                    } else {
                        poke_var.wait_until(lock, defer_queue.top()->when);
                    }
                }
            }
        }

        AsyncTool::CycleResult AsyncTool::iterate() noexcept
        {
            std::unique_lock<std::mutex> lock(impl_->exec_mutex);
            impl_->iterate(lock);

            using std::chrono::milliseconds;

            if (impl_->immed_queue.empty()) {
                if (impl_->defer_queue.empty()) {
                    return {false, milliseconds(0)};
                }

                auto delay =
                        impl_->defer_queue.top()->when - steady_clock::now();
                return {true, std::chrono::duration_cast<milliseconds>(delay)};
            }

            return {true, milliseconds(0)};
        }

        void AsyncTool::Impl::iterate(
                std::unique_lock<std::mutex>& /*exec_lock*/) noexcept
        {
            // process immediates
            for (size_t i = 0; (i < IMMED_BURST) && !immed_queue.empty(); ++i) {
                auto& h = immed_queue.front();

                if (!h.canceled) {
                    if (h.outer != nullptr) {
                        HandleAccessor(*(h.outer)).internal_ = nullptr;
                        h.outer = nullptr;
                    }

                    h.callback();
                }

                immed_queue.pop_front();
            }

            // process deferred
            auto now = steady_clock::now();

            for (size_t i = 0; (i < DEFER_BURST) && !defer_queue.empty(); ++i) {
                auto& h_it = defer_queue.top();

                if (h_it->when > now) {
                    break;
                }

                if (!h_it->canceled) {
                    if (h_it->outer != nullptr) {
                        HandleAccessor(*(h_it->outer)).internal_ = nullptr;
                        h_it->outer = nullptr;
                    }

                    h_it->callback();
                }

                defer_free_heap.splice(
                        defer_free_heap.begin(), defer_used_heap, h_it);

                defer_queue.pop();
            }

            if (have_tasks) {
                std::lock_guard<std::mutex> lock(task_mutex);

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
                    impl_->exec_mutex, std::defer_lock);

            if (!is_same_thread()) {
                lock.lock();

                if (ha.internal_ == nullptr) {
                    return;
                }
            }

            auto internal = static_cast<Impl::ImmediateHandle*>(ha.internal_);
            internal->canceled = true;
            internal->outer = nullptr;
            ha.internal_ = nullptr;
        }

        void AsyncTool::move(Handle& src, Handle& dst) noexcept
        {
            HandleAccessor srca(src);
            HandleAccessor dsta(dst);
            std::unique_lock<std::mutex> lock(
                    impl_->exec_mutex, std::defer_lock);

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
                    impl_->exec_mutex, std::defer_lock);

            if (!is_same_thread()) {
                lock.lock();

                if (ha.internal_ == nullptr) {
                    return;
                }
            }

            ha.internal_->outer = nullptr;
            ha.internal_ = nullptr;
        }
    } // namespace ri
} // namespace futoin
