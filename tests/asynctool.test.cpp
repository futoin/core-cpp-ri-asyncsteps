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

#include <boost/test/unit_test.hpp>

#include <futoin/ri/asynctool.hpp>

#include <atomic>
#include <future>
#include <iostream>
#include <list>

BOOST_AUTO_TEST_SUITE(asynctool) // NOLINT

//=============================================================================

using futoin::ri::AsyncTool;
auto external_poke = []() {};
const std::chrono::milliseconds TEST_DELAY{100}; // NOLINT

BOOST_AUTO_TEST_SUITE(external_loop) // NOLINT

BOOST_AUTO_TEST_CASE(instance) // NOLINT
{
    AsyncTool at(external_poke);
}

BOOST_AUTO_TEST_CASE(is_same_thread) // NOLINT
{
    AsyncTool at(external_poke);

    BOOST_CHECK(at.is_same_thread());

    std::thread([&]() { BOOST_CHECK(!at.is_same_thread()); }).join();
}

BOOST_AUTO_TEST_CASE(immediate) // NOLINT
{
    AsyncTool at(external_poke);
    std::atomic_bool fired{false};
    at.immediate([&]() { fired = true; });

    BOOST_CHECK_EQUAL(fired, false);
    auto res = at.iterate();

    BOOST_CHECK_EQUAL(fired, true);
    BOOST_CHECK_EQUAL(res.have_work, false);

    auto res2 = at.iterate();
    BOOST_CHECK_EQUAL(res.have_work, false);
}

BOOST_AUTO_TEST_CASE(immediate_order) // NOLINT
{
    AsyncTool at(external_poke);
    std::atomic_int val{0};
    at.immediate([&]() { val = 2; });
    at.immediate([&]() { val = val * val; });

    auto res = at.iterate();

    BOOST_CHECK_EQUAL(val, 4);
    BOOST_CHECK_EQUAL(res.have_work, false);
}

BOOST_AUTO_TEST_CASE(immediate_cancel) // NOLINT
{
    AsyncTool at(external_poke);
    std::atomic_int val{0};

    at.immediate([&]() { val = 2; });
    at.immediate([&]() { val = val * val; }).cancel();
    at.iterate();

    BOOST_CHECK_EQUAL(val, 2);

    val = 3;

    at.immediate([&]() { val = 2; }).cancel();
    at.immediate([&]() { val = val * val; });
    at.iterate();

    BOOST_CHECK_EQUAL(val, 9);
}

BOOST_AUTO_TEST_CASE(defer) // NOLINT
{
    AsyncTool at(external_poke);
    std::atomic_bool fired{false};
    at.deferred(TEST_DELAY, [&]() { fired = true; });

    BOOST_CHECK_EQUAL(fired, false);
    auto res1 = at.iterate();

    BOOST_CHECK_EQUAL(fired, false);
    BOOST_CHECK_EQUAL(res1.have_work, true);
    BOOST_CHECK_LE(res1.delay.count(), TEST_DELAY.count());
    BOOST_CHECK_GT(res1.delay.count(), TEST_DELAY.count() / 2);

    std::this_thread::sleep_for(res1.delay);
    auto res2 = at.iterate();

    BOOST_CHECK_EQUAL(fired, true);
    BOOST_CHECK_EQUAL(res2.have_work, false);

    BOOST_CHECK_EQUAL(at.iterate().have_work, false);
}

BOOST_AUTO_TEST_CASE(defer_order) // NOLINT
{
    AsyncTool at(external_poke);
    std::atomic_bool fired1{false};
    std::atomic_bool fired2{false};
    at.deferred(TEST_DELAY * 2, [&]() { fired1 = true; });
    at.deferred(TEST_DELAY, [&]() { fired2 = true; });

    BOOST_CHECK_EQUAL(fired1, false);
    auto res1 = at.iterate();

    BOOST_CHECK_EQUAL(fired1, false);
    BOOST_CHECK_EQUAL(fired2, false);
    BOOST_CHECK_EQUAL(res1.have_work, true);

    std::this_thread::sleep_for(res1.delay);
    auto res2 = at.iterate();

    BOOST_CHECK_EQUAL(fired1, false);
    BOOST_CHECK_EQUAL(fired2, true);
    BOOST_CHECK_EQUAL(res2.have_work, true);

    std::this_thread::sleep_for(res2.delay);
    auto res3 = at.iterate();

    BOOST_CHECK_EQUAL(fired1, true);
    BOOST_CHECK_EQUAL(res3.have_work, false);
    BOOST_CHECK_EQUAL(res3.delay.count(), 0);
}

BOOST_AUTO_TEST_CASE(defer_cancel) // NOLINT
{
    AsyncTool at(external_poke);
    std::atomic_bool fired1{false};
    std::atomic_bool fired2{false};
    at.deferred(TEST_DELAY * 2, [&]() { fired1 = true; });
    auto handle = at.deferred(TEST_DELAY, [&]() { fired2 = true; });

    BOOST_CHECK_EQUAL(fired1, false);
    auto res1 = at.iterate();

    BOOST_CHECK_EQUAL(fired1, false);
    BOOST_CHECK_EQUAL(fired2, false);
    BOOST_CHECK_EQUAL(res1.have_work, true);

    handle.cancel();

    std::this_thread::sleep_for(res1.delay);
    auto res2 = at.iterate();

    BOOST_CHECK_EQUAL(fired1, false);
    BOOST_CHECK_EQUAL(fired2, false);
    BOOST_CHECK_EQUAL(res2.have_work, true);

    std::this_thread::sleep_for(res2.delay);
    auto res3 = at.iterate();

    BOOST_CHECK_EQUAL(fired1, true);
    BOOST_CHECK_EQUAL(fired2, false);
    BOOST_CHECK_EQUAL(res3.have_work, false);
    BOOST_CHECK_EQUAL(res3.delay.count(), 0);
}

BOOST_AUTO_TEST_SUITE_END() // NOLINT

//=============================================================================

BOOST_AUTO_TEST_SUITE(internal_loop) // NOLINT

BOOST_AUTO_TEST_CASE(instance) // NOLINT
{
    AsyncTool at;
}

BOOST_AUTO_TEST_CASE(is_same_thread) // NOLINT
{
    AsyncTool at;

    BOOST_CHECK(!at.is_same_thread());

    std::promise<void> fired;
    at.immediate([&]() {
        BOOST_CHECK(at.is_same_thread());
        fired.set_value();
    });

    fired.get_future().wait();
}

BOOST_AUTO_TEST_CASE(immediate) // NOLINT
{
    AsyncTool at;
    std::promise<bool> fired;
    at.immediate([&]() { fired.set_value(true); });

    auto future = fired.get_future();

    BOOST_CHECK(future.valid());
    BOOST_CHECK_EQUAL(future.get(), true);
}

BOOST_AUTO_TEST_CASE(immediate_order) // NOLINT
{
    AsyncTool at;

    // start from waiting state
    std::this_thread::sleep_for(TEST_DELAY);

    std::promise<void> fired;

    std::atomic_int val{0};
    at.immediate([&]() { val = 2; });
    at.immediate([&]() { val = val * val; });
    at.immediate([&]() { fired.set_value(); });

    fired.get_future().wait();
    BOOST_CHECK_EQUAL(val, 4);
}

BOOST_AUTO_TEST_CASE(immediate_cancel) // NOLINT
{
    AsyncTool at;

    std::atomic_int val{0};
    std::atomic_int fired{0};
    AsyncTool::Handle handle;

    std::promise<void> ready_to_cancel;
    std::promise<void> canceled;
    std::promise<void> ready_to_test;

    at.immediate([&]() {
        at.immediate([&]() {
            val = 2;
            ++fired;
        });

        // make gap for the next burst
        for (size_t i = 0; i < AsyncTool::BURST_COUNT - 2; ++i) {
            at.immediate([&]() { ++fired; });
        }

        at.immediate([&]() {
            ++fired;
            canceled.get_future().wait();
        });
        handle = at.immediate([&]() {
            val = val * val;
            ++fired;
        });

        ready_to_cancel.set_value();
        std::this_thread::sleep_for(TEST_DELAY);

        at.immediate([&]() {
            ++fired;
            ready_to_test.set_value();
        });
    });

    ready_to_cancel.get_future().wait();
    handle.cancel();
    canceled.set_value();

    ready_to_test.get_future().wait();
    BOOST_CHECK_EQUAL(val, 2);
    BOOST_CHECK_EQUAL(fired, 1 + AsyncTool::BURST_COUNT - 2 + 2);
}

BOOST_AUTO_TEST_CASE(defer) // NOLINT
{
    AsyncTool at;
    std::atomic_bool fired{false};
    at.deferred(TEST_DELAY, [&]() { fired = true; });

    BOOST_CHECK_EQUAL(fired, false);

    std::this_thread::sleep_for(TEST_DELAY * 1.5);

    BOOST_CHECK_EQUAL(fired, true);
}

BOOST_AUTO_TEST_CASE(defer_order) // NOLINT
{
    AsyncTool at;
    std::atomic_bool fired1{false};
    std::atomic_bool fired2{false};
    at.deferred(TEST_DELAY * 2, [&]() { fired1 = true; });
    at.deferred(TEST_DELAY, [&]() { fired2 = true; });

    BOOST_CHECK_EQUAL(fired1, false);
    BOOST_CHECK_EQUAL(fired2, false);

    std::this_thread::sleep_for(TEST_DELAY * 1.1);

    BOOST_CHECK_EQUAL(fired1, false);
    BOOST_CHECK_EQUAL(fired2, true);

    std::this_thread::sleep_for(TEST_DELAY * 1.1);

    BOOST_CHECK_EQUAL(fired1, true);
}

BOOST_AUTO_TEST_CASE(defer_cancel) // NOLINT
{
    AsyncTool at;
    std::atomic_bool fired1{false};
    std::atomic_bool fired2{false};
    at.deferred(TEST_DELAY * 2, [&]() { fired1 = true; });
    auto handle = at.deferred(TEST_DELAY, [&]() { fired2 = true; });

    BOOST_CHECK_EQUAL(fired1, false);
    BOOST_CHECK_EQUAL(fired2, false);

    handle.cancel();

    std::this_thread::sleep_for(TEST_DELAY * 1.1);

    BOOST_CHECK_EQUAL(fired1, false);
    BOOST_CHECK_EQUAL(fired2, false);

    std::this_thread::sleep_for(TEST_DELAY * 1.1);

    BOOST_CHECK_EQUAL(fired1, true);
    BOOST_CHECK_EQUAL(fired2, false);
}

BOOST_AUTO_TEST_SUITE_END() // NOLINT

//=============================================================================

BOOST_AUTO_TEST_SUITE(spi) // NOLINT

struct StepEmu
{
    AsyncTool& at;
    AsyncTool::Handle handle;
    AsyncTool::Handle limit;
    volatile size_t count = 0;

    StepEmu(AsyncTool& at) : at(at) {}

    void start()
    {
        (*this)();
    }

    void stop()
    {
        handle.cancel();
        limit.cancel();
    }

    void operator()() noexcept
    {
        handle = at.immediate(std::ref(*this));

        if (count % 10 == 0) {
            limit.cancel();
            limit = at.deferred(std::chrono::seconds(30), std::ref(*this));
        }

        ++count;
    }
};

BOOST_AUTO_TEST_CASE(performance) // NOLINT
{
    AsyncTool at;

    StepEmu step_emu1{at};
    StepEmu step_emu2{at};
    StepEmu step_emu3{at};

    auto print_stats = [&]() {
        auto stats = at.stats();

        std::cout << "Step iterations: " << std::endl
                  << " 1=" << step_emu1.count << std::endl
                  << " 2=" << step_emu2.count << std::endl
                  << " 3=" << step_emu3.count << std::endl;

        std::cout << "Stats: " << std::endl
                  << " immediate_used=" << stats.immediate_used << std::endl
                  << " deferred_used=" << stats.deferred_used << std::endl
                  << " universal_free=" << stats.universal_free << std::endl
                  << " handle_task_count=" << stats.handle_task_count
                  << std::endl;
        BOOST_CHECK_LE(stats.immediate_used, 6);
        BOOST_CHECK_LE(stats.deferred_used, 18);
        BOOST_CHECK_LE(stats.universal_free, AsyncTool::BURST_COUNT * 2);
        BOOST_CHECK_EQUAL(stats.handle_task_count, 0);
    };

    std::promise<void> done;

    at.immediate([&]() {
        // NOTE: due to unfair std::mutex scheduling, we need to do it this way
        //       from internal loop thread.
        step_emu1.start();
        step_emu2.start();
        step_emu3.start();

        at.deferred(std::chrono::seconds(1), [&]() {
            print_stats();
            at.shrink_to_fit();
        });

        at.deferred(std::chrono::seconds(2), [&]() {
            print_stats();
            step_emu1.stop();
            step_emu2.stop();
            step_emu3.stop();
            done.set_value();
        });
    });

    done.get_future().wait();
    BOOST_CHECK_GT(step_emu1.count, 1e4);
    BOOST_CHECK_GT(step_emu2.count, 1e4);
    BOOST_CHECK_GT(step_emu3.count, 1e4);
}

BOOST_AUTO_TEST_CASE(stress) // NOLINT
{
    AsyncTool at;

    constexpr size_t STEP_COUNT = 1e5;
    std::list<StepEmu> steps;

    for (size_t c = STEP_COUNT; c > 0; --c) {
        steps.emplace_back(at);
    }

    auto print_stats = [&]() {
        auto stats = at.stats();
        size_t iterations = 0;

        for (auto& v : steps) {
            iterations += v.count;
        }

        std::cout << "Step iterations: " << iterations << std::endl;

        std::cout << "Stats: " << std::endl
                  << " immediate_used=" << stats.immediate_used << std::endl
                  << " deferred_used=" << stats.deferred_used << std::endl
                  << " universal_free=" << stats.universal_free << std::endl
                  << " handle_task_count=" << stats.handle_task_count
                  << std::endl;

        BOOST_CHECK_GT(iterations, 1e4);
        BOOST_CHECK_LE(stats.immediate_used, STEP_COUNT * 2);
        BOOST_CHECK_LE(
                stats.deferred_used + stats.universal_free, STEP_COUNT * 3);
        BOOST_CHECK_EQUAL(stats.handle_task_count, 0);
    };

    std::promise<void> done;

    at.immediate([&]() {
        // NOTE: due to unfair std::mutex scheduling, we need to do it this way
        //       from internal loop thread.
        for (auto& v : steps) {
            v.start();
        }

        at.deferred(std::chrono::seconds(1), [&]() {
            print_stats();
            at.shrink_to_fit();
        });

        at.deferred(std::chrono::seconds(2), [&]() {
            print_stats();
            for (auto& v : steps) {
                v.stop();
            }
            done.set_value();
        });
    });

    done.get_future().wait();
}

BOOST_AUTO_TEST_CASE(external_stress) // NOLINT
{
    AsyncTool at;

    auto measure = [&](size_t thread_count) {
        std::cout << "Running threads: " << thread_count << std::endl;

        std::atomic_bool run{true};
        volatile size_t call_count = 0;
        std::atomic_size_t scheduled{0};

        auto step = [&]() { ++call_count; };

        at.deferred(std::chrono::seconds(1), [&]() {
            run.store(false, std::memory_order_release);
        });

        auto ext_run = [&]() {
            while (run.load(std::memory_order_consume)) {
                at.immediate(std::ref(step));
                ++scheduled;
            }
        };

        std::list<std::thread> threads;

        while (thread_count-- > 0) {
            threads.emplace_back(std::ref(ext_run));
        }

        for (auto& t : threads) {
            t.join();
        }

        std::promise<void> done;
        at.immediate([&]() { done.set_value(); });
        done.get_future().wait();

        std::cout << "Call count: " << call_count << std::endl
                  << "Scheduled count: " << scheduled << std::endl;
        BOOST_CHECK_GT(call_count, 1e4);
        return call_count;
    };

    measure(1);
    measure(3);
    measure(5);
    measure(50);
}

BOOST_AUTO_TEST_SUITE_END() // NOLINT

//=============================================================================

BOOST_AUTO_TEST_SUITE_END() // NOLINT
