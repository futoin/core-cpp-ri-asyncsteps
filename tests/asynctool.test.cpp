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

    struct
    {
        std::atomic_int val{0};
        std::atomic_int fired{0};
        AsyncTool::Handle handle;

        std::promise<void> ready_to_cancel;
        std::promise<void> canceled;
        std::promise<void> ready_to_test;
    } refs;

    at.immediate([&]() {
        at.immediate([&]() {
            refs.val = 2;
            ++refs.fired;
        });

        // make gap for the next burst
        for (size_t i = 0; i < AsyncTool::BURST_COUNT - 2; ++i) {
            at.immediate([&]() { ++refs.fired; });
        }

        at.immediate([&]() {
            ++refs.fired;
            refs.canceled.get_future().wait();
        });
        refs.handle = at.immediate([&]() {
            refs.val = refs.val * refs.val;
            ++refs.fired;
        });

        refs.ready_to_cancel.set_value();
        std::this_thread::sleep_for(TEST_DELAY);

        at.immediate([&]() {
            ++refs.fired;
            refs.ready_to_test.set_value();
        });
    });

    refs.ready_to_cancel.get_future().wait();
    refs.handle.cancel();
    refs.canceled.set_value();

    refs.ready_to_test.get_future().wait();
    BOOST_CHECK_EQUAL(refs.val, 2);
    BOOST_CHECK_EQUAL(refs.fired, 1 + AsyncTool::BURST_COUNT - 2 + 2);
}

BOOST_AUTO_TEST_CASE(immediate_variations) // NOLINT
{
    AsyncTool at;

    struct Test
    {
        static void test() {}
    };

    at.immediate(&Test::test);
    at.immediate(AsyncTool::Callback(&Test::test));

    AsyncTool::Callback f(&Test::test);
    const auto& cf = f;
    at.immediate(f);
    at.immediate(cf);

    std::promise<bool> fired;
    at.immediate([&]() { fired.set_value(true); });

    auto future = fired.get_future();

    BOOST_CHECK(future.valid());
    BOOST_CHECK_EQUAL(future.get(), true);
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
    size_t count = 0;

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

struct SimpleStepEmu : public StepEmu
{
    SimpleStepEmu(AsyncTool& at) : StepEmu(at) {}

    void start()
    {
        (*this)();
    }

    void operator()() noexcept
    {
        handle = at.immediate(std::ref(*this));
        ++count;
    }
};

size_t measure_raw_count()
{
    static size_t raw_count = 0;

    if (raw_count == 0) {
        using namespace std::chrono;

        for (steady_clock::time_point end = steady_clock::now() + seconds(1);
             end > steady_clock::now();) {
            for (size_t i = AsyncTool::BURST_COUNT; i > 0; --i) {
                ++raw_count;
            }
        }
    }

    return raw_count;
}

BOOST_AUTO_TEST_CASE(performance_deferred) // NOLINT
{
    struct
    {
        AsyncTool at;
        StepEmu step_emu1{at};
        StepEmu step_emu2{at};
        StepEmu step_emu3{at};
        size_t raw_count{0};
        std::promise<void> done;
    } refs;

    auto print_stats = [&]() {
        auto stats = refs.at.stats();

        std::cout << std::endl;
        std::cout << "Raw iterations: " << refs.raw_count << std::endl;
        std::cout << "Step iterations with deferred: " << std::endl
                  << " 1=" << refs.step_emu1.count << std::endl
                  << " 2=" << refs.step_emu2.count << std::endl
                  << " 3=" << refs.step_emu3.count << std::endl;

        std::cout << "Stats: " << std::endl
                  << " immediate_used=" << stats.immediate_used << std::endl
                  << " deferred_used=" << stats.deferred_used << std::endl
                  << " universal_free=" << stats.universal_free << std::endl
                  << " handle_task_count=" << stats.handle_task_count
                  << std::endl;
        BOOST_CHECK_LE(stats.immediate_used, 6);
        BOOST_CHECK_LE(stats.deferred_used, AsyncTool::BURST_COUNT / 10 + 10);
        BOOST_CHECK_LE(stats.universal_free, AsyncTool::BURST_COUNT * 2);
        BOOST_CHECK_EQUAL(stats.handle_task_count, 0);
    };

    refs.at.immediate([&]() { refs.raw_count = measure_raw_count(); });

    refs.at.immediate([&]() {
        refs.at.deferred(std::chrono::seconds(1), [&]() {
            print_stats();
            refs.at.mem_pool().release_memory();
        });

        refs.at.deferred(std::chrono::seconds(2), [&]() {
            print_stats();
            refs.step_emu1.stop();
            refs.step_emu2.stop();
            refs.step_emu3.stop();
            refs.done.set_value();
        });

        // NOTE: due to unfair std::mutex scheduling, we need to do it this way
        //       from internal loop thread.
        refs.step_emu1.start();
        refs.step_emu2.start();
        refs.step_emu3.start();
    });

    refs.done.get_future().wait();

    const size_t ref_count = refs.raw_count / 3 / 150;
    BOOST_CHECK_GT(refs.step_emu1.count, ref_count);
    BOOST_CHECK_GT(refs.step_emu2.count, ref_count);
    BOOST_CHECK_GT(refs.step_emu3.count, ref_count);
}

BOOST_AUTO_TEST_CASE(performance_pure_immediates) // NOLINT
{
    struct
    {
        AsyncTool at;
        AsyncTool at_deferred;
        SimpleStepEmu step{at};
        size_t raw_count{0};
        std::promise<void> done;
    } refs;

    auto print_stats = [&]() {
        auto stats = refs.at.stats();

        std::cout << std::endl;
        std::cout << "Raw iterations: " << refs.raw_count << std::endl;
        std::cout << "Step iterations with immediate: " << refs.step.count
                  << std::endl;

        std::cout << "Stats: " << std::endl
                  << " immediate_used=" << stats.immediate_used << std::endl
                  << " deferred_used=" << stats.deferred_used << std::endl
                  << " universal_free=" << stats.universal_free << std::endl
                  << " handle_task_count=" << stats.handle_task_count
                  << std::endl;
        BOOST_CHECK_LE(stats.immediate_used, AsyncTool::BURST_COUNT + 1);
        BOOST_CHECK_LE(stats.deferred_used, 2);
        BOOST_CHECK_LE(stats.universal_free, AsyncTool::BURST_COUNT * 2);
        BOOST_CHECK_EQUAL(stats.handle_task_count, 0);
    };

    refs.at.immediate([&]() { refs.raw_count = measure_raw_count(); });

    refs.at.immediate([&]() {
        refs.at_deferred.immediate([&]() {
            refs.at_deferred.deferred(std::chrono::seconds(1), [&]() {
                print_stats();
                refs.at.mem_pool().release_memory();
            });

            refs.at_deferred.deferred(std::chrono::seconds(2), [&]() {
                refs.step.stop();
                print_stats();
                refs.done.set_value();
            });
        });

        // NOTE: due to unfair std::mutex scheduling, we need to do it this way
        //       from internal loop thread.
        refs.step.start();
    });

    refs.done.get_future().wait();

    const size_t ref_count = refs.raw_count / 150;
    BOOST_CHECK_GT(refs.step.count, ref_count);
}

BOOST_AUTO_TEST_CASE(performance_immediates_with_defer) // NOLINT
{
    struct
    {
        AsyncTool at;
        SimpleStepEmu step{at};
        size_t raw_count{0};
        std::promise<void> done;
    } refs;

    auto print_stats = [&]() {
        auto stats = refs.at.stats();

        std::cout << std::endl;
        std::cout << "Raw iterations: " << refs.raw_count << std::endl;
        std::cout << "Step iterations with immediate & defer queue: "
                  << refs.step.count << std::endl;

        std::cout << "Stats: " << std::endl
                  << " immediate_used=" << stats.immediate_used << std::endl
                  << " deferred_used=" << stats.deferred_used << std::endl
                  << " universal_free=" << stats.universal_free << std::endl
                  << " handle_task_count=" << stats.handle_task_count
                  << std::endl;
        BOOST_CHECK_LE(stats.immediate_used, AsyncTool::BURST_COUNT + 1);
        BOOST_CHECK_LE(stats.deferred_used, 2);
        BOOST_CHECK_LE(stats.universal_free, AsyncTool::BURST_COUNT * 2);
        BOOST_CHECK_EQUAL(stats.handle_task_count, 0);
    };

    refs.at.immediate([&]() { refs.raw_count = measure_raw_count(); });

    refs.at.immediate([&]() {
        refs.at.deferred(std::chrono::seconds(1), [&]() {
            print_stats();
            refs.at.mem_pool().release_memory();
        });

        refs.at.deferred(std::chrono::seconds(2), [&]() {
            refs.step.stop();
            print_stats();
            refs.done.set_value();
        });

        // NOTE: due to unfair std::mutex scheduling, we need to do it this way
        //       from internal loop thread.
        refs.step.start();
    });

    refs.done.get_future().wait();

    const size_t ref_count = refs.raw_count / 150;
    BOOST_CHECK_GT(refs.step.count, ref_count);
}

BOOST_AUTO_TEST_CASE(stress) // NOLINT
{
    constexpr size_t STEP_COUNT = 1e5;

    struct
    {
        AsyncTool at;
        std::list<StepEmu> steps;
        size_t raw_count{0};
        std::promise<void> done;
    } refs;

    for (size_t c = STEP_COUNT; c > 0; --c) {
        refs.steps.emplace_back(refs.at);
    }

    auto print_stats = [&]() {
        auto stats = refs.at.stats();
        size_t iterations = 0;

        for (auto& v : refs.steps) {
            iterations += v.count;
        }

        std::cout << std::endl;
        std::cout << "Raw iterations: " << refs.raw_count << std::endl;

        std::cout << "Step iterations: " << iterations << std::endl;

        std::cout << "Stats: " << std::endl
                  << " immediate_used=" << stats.immediate_used << std::endl
                  << " deferred_used=" << stats.deferred_used << std::endl
                  << " universal_free=" << stats.universal_free << std::endl
                  << " handle_task_count=" << stats.handle_task_count
                  << std::endl;

        BOOST_CHECK_GT(iterations, refs.raw_count / 300);
        BOOST_CHECK_LE(stats.immediate_used, STEP_COUNT * 2);
        BOOST_CHECK_LE(
                stats.deferred_used + stats.universal_free, STEP_COUNT * 3);
        BOOST_CHECK_EQUAL(stats.handle_task_count, 0);
    };

    refs.at.immediate([&]() { refs.raw_count = measure_raw_count(); });

    refs.at.immediate([&]() {
        // NOTE: due to unfair std::mutex scheduling, we need to do it this way
        //       from internal loop thread.
        for (auto& v : refs.steps) {
            v.start();
        }

        refs.at.deferred(std::chrono::seconds(1), [&]() {
            print_stats();
            refs.at.mem_pool().release_memory();
        });

        refs.at.deferred(std::chrono::seconds(2), [&]() {
            print_stats();
            for (auto& v : refs.steps) {
                v.stop();
            }
            refs.done.set_value();
        });
    });

    refs.done.get_future().wait();
}

BOOST_AUTO_TEST_CASE(external_stress) // NOLINT
{
    AsyncTool at;

    auto measure = [&](size_t thread_count) {
        std::cout << "Running threads: " << thread_count << std::endl;

        auto test_coeff = 30 * std::max<int>(1, 50 - thread_count);
        size_t raw_count = measure_raw_count();
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
        BOOST_CHECK_GT(call_count, raw_count / 150 / test_coeff);
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
