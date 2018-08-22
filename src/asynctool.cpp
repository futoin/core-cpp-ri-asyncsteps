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
//---
#include <boost/lockfree/spsc_queue.hpp>
#include <boost/pool/pool_alloc.hpp>

namespace futoin {
    namespace ri {
        using std::chrono::steady_clock;
        using lock_guard = std::lock_guard<std::mutex>;

        constexpr size_t AsyncTool::BURST_COUNT;

        struct AsyncTool::Impl
        {
            struct ImmediateHandle : InternalHandle
            {
                ImmediateHandle(Callback&& cb, HandleCookie cookie) noexcept :
                    InternalHandle(std::forward<Callback>(cb)), cookie(cookie)
                {}

                ImmediateHandle(ImmediateHandle&& other) noexcept = default;
                ImmediateHandle& operator=(ImmediateHandle&& other) noexcept =
                        default;

                ~ImmediateHandle() noexcept = default;

                HandleCookie cookie;
            };

            struct DeferredHandle : ImmediateHandle
            {
                DeferredHandle(
                        Callback&& cb,
                        HandleCookie cookie,
                        steady_clock::time_point when) :
                    ImmediateHandle(std::forward<Callback>(cb), cookie),
                    when(when)
                {}

                DeferredHandle(DeferredHandle&&) noexcept = default;
                DeferredHandle& operator=(DeferredHandle&&) noexcept = default;

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

            using HandleTask = std::function<void()>;

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

                handle_task_queue();
            }

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
                while (handle_tasks.read_available() != 0) {
                    handle_tasks.front()->operator()();
                    handle_tasks.pop();
                }
            }

            void add_handle_task(HandleTask& task)
            {
                for (bool done = false; !done;) {
                    std::lock_guard<std::mutex> lock(handle_mutex);
                    done = handle_tasks.push(&task);
                    poke();
                }
            }

            //---
            std::deque<Callback> burst_queue;

            HandleCookie current_cookie{1};
            std::deque<ImmediateHandle> immed_queue;

            using DeferredAllocator =
                    boost::fast_pool_allocator<DeferredHandle>;
            using DeferredHeap = std::list<DeferredHandle, DeferredAllocator>;
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
            boost::lockfree::spsc_queue<
                    HandleTask*,
                    boost::lockfree::capacity<BURST_COUNT * 10>>
                    handle_tasks;

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
                std::promise<AsyncTool::Handle> res;
                Impl::HandleTask task = [this, &res, &cb]() {
                    res.set_value(this->immediate(std::forward<Callback>(cb)));
                };

                impl_->add_handle_task(task);
                return res.get_future().get();
            }

            auto cookie = impl_->get_cookie();

            auto& q = impl_->immed_queue;
            q.emplace_back(std::forward<Callback>(cb), cookie);
            auto& iq = q.back();
            return {iq, *this, cookie};
        }

        AsyncTool::Handle AsyncTool::deferred(
                std::chrono::milliseconds delay, Callback&& cb) noexcept
        {
            if (!AsyncTool::is_same_thread()) {
                std::promise<AsyncTool::Handle> res;
                Impl::HandleTask task = [this, &res, &cb, delay]() {
                    res.set_value(
                            this->deferred(delay, std::forward<Callback>(cb)));
                };

                impl_->add_handle_task(task);
                return res.get_future().get();
            }

            if (delay < std::chrono::milliseconds(100)) {
                std::cerr << "FATAL: deferred AsyncTool calls are designed for "
                             "timeouts!"
                          << std::endl
                          << "       Avoid using it for too short delays "
                             "(<100ms)."
                          << std::endl;
                std::terminate();
            }

            auto when = steady_clock::now() + delay;

            auto& free_heap = impl_->defer_free_heap;
            auto& used_heap = impl_->defer_used_heap;
            auto& q = impl_->defer_queue;

            Impl::DeferredQueueItem it;
            auto cookie = impl_->get_cookie();

            if (free_heap.empty()) {
                used_heap.emplace_front(
                        std::forward<Callback>(cb), cookie, when);
                it = used_heap.begin();
            } else {
                it = free_heap.begin();
                *it = Impl::DeferredHandle(
                        std::forward<Callback>(cb), cookie, when);
                used_heap.splice(used_heap.begin(), free_heap, it);
            }

            q.push(it);

            auto& iq = *it;
            return {iq, *this, cookie};
        }

        bool AsyncTool::is_same_thread() noexcept
        {
            return std::this_thread::get_id() == impl_->reactor_thread_id;
        }

        void AsyncTool::Impl::process() noexcept
        {
            while (!is_shutdown) {
                iterate();

                if (immed_queue.empty() && handle_tasks.empty()) {
                    std::unique_lock<std::mutex> lock(handle_mutex);

                    if (is_shutdown) {
                        break;
                    }

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

            impl_->iterate();

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

        void AsyncTool::Impl::iterate() noexcept
        {
            // process immediates
            for (size_t i = BURST_COUNT; (i > 0) && !immed_queue.empty(); --i) {
                auto& ih = immed_queue.front();
                auto& cookie = ih.cookie;

                if (cookie != 0) {
                    cookie = 0;
                    ih.callback();
                } else {
                    --canceled_handles;
                }

                immed_queue.pop_front();
            }

            const auto now = steady_clock::now();

            // NOTE: it's assumed deferred calls are almost always canceled, but
            // not executed!
            for (size_t i = BURST_COUNT; (i > 0) && !defer_queue.empty(); --i) {
                auto& h_it = defer_queue.top();

                auto& cookie = h_it->cookie;

                if (cookie != 0) {
                    if (h_it->when > now) {
                        break;
                    }

                    cookie = 0;
                    h_it->callback();
                } else {
                    --canceled_handles;
                }

                defer_free_heap.splice(
                        defer_free_heap.begin(), defer_used_heap, h_it);
                defer_queue.pop();
            }

            // TODO: redesign
            if (canceled_handles > (defer_used_heap.size() / 2)) {
                auto iter = defer_used_heap.begin();
                const auto end = defer_used_heap.end();

                defer_queue = DeferredPriorityQueue();

                while (iter != end) {
                    if (iter->cookie != 0) {
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

            // Process external requests
            handle_task_queue();
        }

        void AsyncTool::cancel(Handle& h) noexcept
        {
            HandleAccessor ha(h);

            auto internal = ha.internal();

            if (internal != nullptr) {
                ha.internal() = nullptr;
                auto immed = static_cast<Impl::ImmediateHandle*>(internal);
                auto ha_cookie = ha.cookie();

                if (immed->cookie != ha_cookie) {
                    // pass
                } else if (!is_same_thread()) {
                    std::promise<void> res;
                    Impl::HandleTask task = [this, immed, ha_cookie, &res]() {
                        if (immed->cookie == ha_cookie) {
                            ++(impl_->canceled_handles);
                            immed->cookie = 0;
                        }
                        res.set_value();
                    };

                    impl_->add_handle_task(task);
                    res.get_future().wait();
                } else {
                    ++(impl_->canceled_handles);
                    immed->cookie = 0;
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

            auto immed = static_cast<Impl::ImmediateHandle*>(internal);

            return immed->cookie == ha.cookie();
        }

        AsyncTool::Stats AsyncTool::stats() noexcept
        {
            // not safe

            return {
                    impl_->immed_queue.size(),
                    impl_->defer_used_heap.size(),
                    impl_->defer_free_heap.size(),
                    impl_->handle_tasks.read_available(),
            };
        }

        void AsyncTool::shrink_to_fit() noexcept
        {
            if (is_same_thread()) {
                if (impl_->immed_queue.empty()) {
                    impl_->immed_queue.shrink_to_fit();
                }

                impl_->defer_free_heap.clear();
            } else {
                std::promise<void> res;
                Impl::HandleTask task = [this, &res]() {
                    this->shrink_to_fit();
                    res.set_value();
                };

                impl_->add_handle_task(task);
                res.get_future().wait();
            }
        }
    } // namespace ri
} // namespace futoin
