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

#include <futoin/ri/asyncsteps.hpp>
#include <futoin/ri/asynctool.hpp>
#include <futoin/ri/limiter.hpp>
#include <futoin/ri/mutex.hpp>
#include <futoin/ri/throttle.hpp>

#include <atomic>
#include <future>

namespace ri = futoin::ri;
using futoin::ErrorCode;
using futoin::IAsyncSteps;

BOOST_AUTO_TEST_SUITE(synctest) // NOLINT

//=============================================================================

BOOST_AUTO_TEST_SUITE(mutex) // NOLINT

BOOST_AUTO_TEST_CASE(outer) // NOLINT
{
    ri::Mutex mtx;
    ri::AsyncTool at{[]() {}};

    ri::AsyncSteps as1{at};
    ri::AsyncSteps as2{at};

    std::atomic_size_t count{0};
    std::atomic_size_t max{0};

    auto f = [&](IAsyncSteps& asi) {
        count.fetch_add(1);
        asi.add([&](IAsyncSteps&) {
            max.store(std::max(max, count));
            count.fetch_sub(1);
        });
    };

    as1.sync(mtx, f);
    as2.sync(mtx, f);

    as1.execute();
    as2.execute();
    while (at.iterate().have_work) {
    }
    BOOST_CHECK_EQUAL(max, 1U);
    BOOST_CHECK_EQUAL(count, 0U);
}

BOOST_AUTO_TEST_CASE(inner) // NOLINT
{
    ri::Mutex mtx;
    ri::AsyncTool at{[]() {}};

    ri::AsyncSteps as1{at};
    ri::AsyncSteps as2{at};

    std::atomic_size_t count{0};
    std::atomic_size_t max{0};

    auto f = [&](IAsyncSteps& asi) {
        asi.sync(mtx, [&](IAsyncSteps& asi) {
            count.fetch_add(1);
            asi.add([&](IAsyncSteps&) {
                max.store(std::max(max, count));
                count.fetch_sub(1);
            });
        });
    };

    as1.add(f);
    as2.add(f);

    as1.execute();
    as2.execute();
    while (at.iterate().have_work) {
    }
    BOOST_CHECK_EQUAL(max, 1U);
    BOOST_CHECK_EQUAL(count, 0U);
}

BOOST_AUTO_TEST_CASE(args) // NOLINT
{
    ri::Mutex mtx;
    ri::AsyncTool at{[]() {}};

    ri::AsyncSteps asi{at};

    asi.add([](IAsyncSteps& asi) { asi(123, true); });
    asi.sync(mtx, [](IAsyncSteps&, int a, bool b) {
        BOOST_CHECK_EQUAL(a, 123);
        BOOST_CHECK_EQUAL(b, true);
    });

    asi.execute();
    while (at.iterate().have_work) {
    }
}

BOOST_AUTO_TEST_CASE(recursion) // NOLINT
{
    ri::Mutex mtx;
    ri::AsyncTool at{[]() {}};

    ri::AsyncSteps as1{at};
    ri::AsyncSteps as2{at};

    std::atomic_size_t count{0};
    std::atomic_size_t max{0};

    auto f = [&](IAsyncSteps& asi) {
        asi.sync(mtx, [&](IAsyncSteps& asi) {
            count.fetch_add(1);
            asi.sync(mtx, [&](IAsyncSteps&) {
                max.store(std::max(max, count));
                count.fetch_sub(1);
            });
        });
    };

    as1.add(f);
    as2.add(f);

    as1.execute();
    as2.execute();
    while (at.iterate().have_work) {
    }
    BOOST_CHECK_EQUAL(max, 1U);
    BOOST_CHECK_EQUAL(count, 0U);
}

BOOST_AUTO_TEST_CASE(queue_max) // NOLINT
{
    ri::Mutex mtx(1, 1);
    ri::AsyncTool at{[]() {}};

    ri::AsyncSteps as1{at};
    ri::AsyncSteps as2{at};
    ri::AsyncSteps as3{at};

    std::atomic_size_t count{0};
    std::atomic_size_t max{0};

    auto f = [&](IAsyncSteps& asi) {
        asi.sync(mtx, [&](IAsyncSteps& asi) {
            count.fetch_add(1);
            asi.sync(mtx, [&](IAsyncSteps&) {
                max.store(std::max(max, count));
                count.fetch_sub(1);
            });
        });
    };

    as1.add(f);
    as2.add(f);
    bool called = false;
    as3.add(f, [&](IAsyncSteps& asi, ErrorCode err) {
        BOOST_CHECK_EQUAL(err, "DefenseRejected");
        BOOST_CHECK_EQUAL(asi.state().error_info, "Mutex queue limit");
        called = true;
        asi();
    });

    as1.execute();
    as2.execute();
    as3.execute();
    while (at.iterate().have_work) {
    }

    BOOST_CHECK(called);
    BOOST_CHECK_EQUAL(max, 1U);
    BOOST_CHECK_EQUAL(count, 0U);
}

BOOST_AUTO_TEST_CASE(multi_max) // NOLINT
{
    ri::Mutex mtx(2);
    ri::AsyncTool at{[]() {}};

    ri::AsyncSteps as1{at};
    ri::AsyncSteps as2{at};
    ri::AsyncSteps as3{at};
    ri::AsyncSteps as4{at};

    std::atomic_size_t count{0};
    std::atomic_size_t max{0};

    auto f = [&](IAsyncSteps& asi) {
        asi.sync(mtx, [&](IAsyncSteps& asi) {
            count.fetch_add(1);
            asi.sync(mtx, [&](IAsyncSteps&) {
                max.store(std::max(max, count));
                count.fetch_sub(1);
            });
        });
    };

    as1.add(f);
    as2.add(f);
    as3.add(f);
    as4.add(f);

    as1.execute();
    as2.execute();
    as3.execute();
    as4.execute();
    while (at.iterate().have_work) {
    }

    BOOST_CHECK_EQUAL(max, 2U);
    BOOST_CHECK_EQUAL(count, 0U);
}

BOOST_AUTO_TEST_SUITE_END() // NOLINT

//=============================================================================

BOOST_AUTO_TEST_SUITE(throttle) // NOLINT

BOOST_AUTO_TEST_CASE(outer) // NOLINT
{
    ri::AsyncTool at{[]() {}};
    ri::Throttle thr(at, 1, std::chrono::milliseconds(150));

    ri::AsyncSteps as1{at};
    ri::AsyncSteps as2{at};

    std::atomic_size_t count{0};
    std::atomic_size_t max{0};
    std::atomic_size_t done{0};

    auto f = [&](IAsyncSteps& asi) {
        count.fetch_add(1);
        asi.add([&](IAsyncSteps&) {
            max.store(std::max(max, count));
            count.fetch_sub(1);
        });
    };
    auto df = [&](IAsyncSteps&) { done.fetch_add(1); };

    as1.sync(thr, f);
    as2.sync(thr, f);

    as1.add(df);
    as2.add(df);

    as1.execute();
    as2.execute();
    while (at.iterate().have_work && (done.load() != 2)) {
    }

    BOOST_CHECK_EQUAL(max, 1U);
    BOOST_CHECK_EQUAL(count, 0U);
}

BOOST_AUTO_TEST_CASE(args) // NOLINT
{
    ri::AsyncTool at;
    ri::Throttle thr(at, 1);

    ri::AsyncSteps asi{at};

    asi.add([](IAsyncSteps& asi) { asi(123, true); });
    asi.sync(thr, [](IAsyncSteps&, int a, bool b) {
        BOOST_CHECK_EQUAL(a, 123);
        BOOST_CHECK_EQUAL(b, true);
    });

    std::promise<void> done;
    asi.add([&](IAsyncSteps&) { done.set_value(); });

    asi.execute();
    done.get_future().wait();
}

BOOST_AUTO_TEST_CASE(queue_max) // NOLINT
{
    ri::AsyncTool at;
    ri::Throttle thr(at, 1, ri::Throttle::milliseconds(1000), 1);

    ri::AsyncSteps as1{at};
    ri::AsyncSteps as2{at};
    ri::AsyncSteps as3{at};

    std::atomic_size_t count{0};
    std::atomic_size_t max{0};

    auto f = [&](IAsyncSteps& asi) {
        asi.sync(thr, [&](IAsyncSteps& asi) {
            count.fetch_add(1);
            asi.add([&](IAsyncSteps& asi) {
                max.store(std::max(max, count));
                count.fetch_sub(1);
                asi(false);
            });
        });
    };

    as1.add(f);
    as2.add(f);
    as3.add(f, [&](IAsyncSteps& asi, ErrorCode err) {
        BOOST_CHECK_EQUAL(err, "DefenseRejected");
        BOOST_CHECK_EQUAL(asi.state().error_info, "Throttle queue limit");
        asi(true);
    });

    as1.execute();
    as2.execute();
    BOOST_CHECK(as3.promise<bool>().get());
    BOOST_CHECK_EQUAL(max, 1U);
    BOOST_CHECK_EQUAL(count, 0U);
}

BOOST_AUTO_TEST_CASE(multi_max) // NOLINT
{
    ri::AsyncTool at;
    ri::Throttle thr(at, 2, ri::Throttle::milliseconds(150));

    ri::AsyncSteps as1{at};
    ri::AsyncSteps as2{at};
    ri::AsyncSteps as3{at};
    ri::AsyncSteps as4{at};

    std::atomic_size_t count{0};
    std::atomic_size_t max{0};

    auto f = [&](IAsyncSteps& asi) {
        asi.sync(thr, [&](IAsyncSteps& asi) {
            count.fetch_add(1);
            asi.add([&](IAsyncSteps&) {
                max.store(std::max(max, count));
                count.fetch_sub(1);
            });
        });
    };

    as1.add(f);
    as2.add(f);
    as3.add(f);
    as4.add(f);

    as1.execute();
    as2.execute();
    as3.execute();
    as4.promise().wait();

    BOOST_CHECK_EQUAL(max, 2U);
    BOOST_CHECK_EQUAL(count, 0U);
}

BOOST_AUTO_TEST_CASE(time_stop_start) // NOLINT
{
    ri::AsyncTool at{[]() {}};

    ri::Throttle::milliseconds delay{150};

    ri::Throttle thr(at, 2, delay);

    ri::AsyncSteps as{at};

    std::atomic_size_t count{0};

    auto f = [&](IAsyncSteps& asi, size_t) {
        asi.sync(thr, [&](IAsyncSteps&) { count.fetch_add(1); });
    };

    as.repeat(3, f);
    at.deferred(delay * 4 / 5, [&]() { BOOST_CHECK_EQUAL(count, 2U); });
    at.deferred(delay * 3 / 2, [&]() { BOOST_CHECK_EQUAL(count, 3U); });

    as.execute();

    while (at.iterate().have_work) {
    }

    auto f2 = [&]() {
        as.repeat(3, f);
        at.deferred(delay * 4 / 5, [&]() { BOOST_CHECK_EQUAL(count, 5U); });
        at.deferred(delay * 3 / 2, [&]() { BOOST_CHECK_EQUAL(count, 6U); });
        as.execute();
    };

    at.deferred(delay * 3, [&]() { f2(); });

    while (at.iterate().have_work) {
    }

    BOOST_CHECK_EQUAL(count, 6U);
}

BOOST_AUTO_TEST_SUITE_END() // NOLINT

//=============================================================================

BOOST_AUTO_TEST_SUITE(limiter) // NOLINT

BOOST_AUTO_TEST_CASE(outer_concurrent) // NOLINT
{
    ri::AsyncTool at{[]() {}};

    ri::Limiter::Params prm;
    prm.rate = 2;
    prm.max_queue = 1;
    ri::Limiter lmtr(at, prm);

    ri::AsyncSteps as1{at};
    ri::AsyncSteps as2{at};

    std::atomic_size_t count{0};
    std::atomic_size_t max{0};

    auto f = [&](IAsyncSteps& asi) {
        count.fetch_add(1);
        asi.add([&](IAsyncSteps&) {
            max.store(std::max(max, count));
            count.fetch_sub(1);
        });
    };

    as1.sync(lmtr, f);
    as2.sync(lmtr, f);

    //---
    std::atomic_size_t done{0};

    auto df = [&](IAsyncSteps&) { done.fetch_add(1); };

    as1.add(df);
    as2.add(df);

    //---
    as1.execute();
    as2.execute();

    while (at.iterate().have_work && (done.load() != 2)) {
    }

    BOOST_CHECK_EQUAL(max, 1U);
    BOOST_CHECK_EQUAL(count, 0U);
}

BOOST_AUTO_TEST_CASE(inner_concurrent) // NOLINT
{
    ri::AsyncTool at{[]() {}};

    ri::Limiter::Params prm;
    prm.rate = 2;
    prm.max_queue = 1;
    ri::Limiter lmtr(at, prm);

    ri::AsyncSteps as1{at};
    ri::AsyncSteps as2{at};

    std::atomic_size_t count{0};
    std::atomic_size_t max{0};

    auto f = [&](IAsyncSteps& asi) {
        asi.sync(lmtr, [&](IAsyncSteps& asi) {
            count.fetch_add(1);
            asi.add([&](IAsyncSteps&) {
                max.store(std::max(max, count));
                count.fetch_sub(1);
            });
        });
    };

    as1.add(f);
    as2.add(f);

    //---
    std::atomic_size_t done{0};

    auto df = [&](IAsyncSteps&) { done.fetch_add(1); };

    as1.add(df);
    as2.add(df);
    //---

    as1.execute();
    as2.execute();

    while (at.iterate().have_work && (done.load() != 2)) {
    }

    BOOST_CHECK_EQUAL(max, 1U);
    BOOST_CHECK_EQUAL(count, 0U);
}

BOOST_AUTO_TEST_CASE(args) // NOLINT
{
    ri::AsyncTool at;

    ri::Limiter lmtr(at, {});

    ri::AsyncSteps asi{at};

    asi.add([](IAsyncSteps& asi) { asi(123, true); });
    asi.sync(lmtr, [](IAsyncSteps&, int a, bool b) {
        BOOST_CHECK_EQUAL(a, 123);
        BOOST_CHECK_EQUAL(b, true);
    });

    asi.promise().wait();
}

BOOST_AUTO_TEST_CASE(recursion) // NOLINT
{
    ri::AsyncTool at{[]() {}};

    ri::Limiter::Params prm;
    prm.rate = 4;
    prm.max_queue = 1;
    ri::Limiter lmtr(at, prm);

    ri::AsyncSteps as1{at};
    ri::AsyncSteps as2{at};

    std::atomic_size_t count{0};
    std::atomic_size_t max{0};

    auto f = [&](IAsyncSteps& asi) {
        asi.sync(lmtr, [&](IAsyncSteps& asi) {
            count.fetch_add(1);
            asi.sync(lmtr, [&](IAsyncSteps&) {
                max.store(std::max(max, count));
                count.fetch_sub(1);
            });
        });
    };

    as1.add(f);
    as2.add(f);

    //---
    std::atomic_size_t done{0};

    auto df = [&](IAsyncSteps&) { done.fetch_add(1); };

    as1.add(df);
    as2.add(df);

    //---
    as1.execute();
    as2.execute();

    while (at.iterate().have_work && (done.load() != 2)) {
    }

    BOOST_CHECK_EQUAL(max, 1U);
    BOOST_CHECK_EQUAL(count, 0U);
}

BOOST_AUTO_TEST_CASE(queue_max) // NOLINT
{
    ri::AsyncTool at{[]() {}};

    ri::Limiter::Params prm;
    prm.max_queue = 1;
    prm.rate = 4;
    ri::Limiter lmtr(at, prm);

    ri::AsyncSteps as1{at};
    ri::AsyncSteps as2{at};
    ri::AsyncSteps as3{at};

    std::atomic_size_t count{0};
    std::atomic_size_t max{0};

    auto f = [&](IAsyncSteps& asi) {
        asi.sync(lmtr, [&](IAsyncSteps& asi) {
            count.fetch_add(1);
            asi.sync(lmtr, [&](IAsyncSteps&) {
                max.store(std::max(max, count));
                count.fetch_sub(1);
            });
        });
    };

    as1.add(f);
    as2.add(f);
    bool called = false;
    as3.add(f, [&](IAsyncSteps& asi, ErrorCode err) {
        BOOST_CHECK_EQUAL(err, "DefenseRejected");
        BOOST_CHECK_EQUAL(asi.state().error_info, "Mutex queue limit");
        called = true;
        asi();
    });

    //---
    std::atomic_size_t done{0};

    auto df = [&](IAsyncSteps&) { done.fetch_add(1); };

    as1.add(df);
    as2.add(df);
    as3.add(df);

    //---

    as1.execute();
    as2.execute();
    as3.execute();

    while (at.iterate().have_work && (done.load() != 3)) {
    }

    BOOST_CHECK(called);
    BOOST_CHECK_EQUAL(max, 1U);
    BOOST_CHECK_EQUAL(count, 0U);
}

BOOST_AUTO_TEST_CASE(multi_max) // NOLINT
{
    ri::AsyncTool at{[]() {}};

    ri::Limiter::Params prm;
    prm.concurrent = 2;
    prm.max_queue = 2;
    prm.rate = 4;
    prm.burst = 4;
    prm.period = ri::Limiter::milliseconds{150};
    ri::Limiter lmtr(at, prm);

    ri::AsyncSteps as1{at};
    ri::AsyncSteps as2{at};
    ri::AsyncSteps as3{at};
    ri::AsyncSteps as4{at};

    std::atomic_size_t count{0};
    std::atomic_size_t max{0};

    auto f = [&](IAsyncSteps& asi) {
        asi.sync(lmtr, [&](IAsyncSteps& asi) {
            count.fetch_add(1);
            asi.sync(lmtr, [&](IAsyncSteps&) {
                max.store(std::max(max, count));
                count.fetch_sub(1);
            });
        });
    };

    as1.add(f);
    as2.add(f);
    as3.add(f);
    as4.add(f);

    //---
    std::atomic_size_t done{0};

    auto df = [&](IAsyncSteps&) { done.fetch_add(1); };

    as1.add(df);
    as2.add(df);
    as3.add(df);
    as4.add(df);

    //---
    as1.execute();
    as2.execute();
    as3.execute();
    as4.execute();

    while (at.iterate().have_work && (done.load() != 4)) {
    }

    BOOST_CHECK_EQUAL(max, 2U);
    BOOST_CHECK_EQUAL(count, 0U);
}

BOOST_AUTO_TEST_SUITE_END() // NOLINT

//=============================================================================

BOOST_AUTO_TEST_SUITE(spi) // NOLINT

BOOST_AUTO_TEST_CASE(mutex_performance) // NOLINT
{
    struct
    {
        ri::AsyncTool at;
        std::unique_ptr<ri::ThreadlessMutex> mtx;
        ri::AsyncSteps as1{at};
        ri::AsyncSteps as2{at};
        ri::AsyncSteps as3{at};
        std::size_t count{0};
    } refs;

    auto f = [&](IAsyncSteps& asi) {
        asi.sync(*(refs.mtx), [&](IAsyncSteps&) { ++(refs.count); });
    };

    refs.at.immediate([&]() {
        refs.mtx.reset(new ri::ThreadlessMutex());
        refs.as1.loop(f);
        refs.as2.loop(f);
        refs.as3.loop(f);

        refs.as1.execute();
        refs.as2.execute();
        refs.as3.execute();
    });

    std::promise<void> done;
    refs.at.deferred(std::chrono::milliseconds{1000}, [&]() {
        // TODO: fix race on d-tor/cancel
        refs.as1.cancel();
        refs.as2.cancel();
        refs.as3.cancel();

        done.set_value();
    });
    done.get_future().wait();

    std::cout << "Mutex count: " << refs.count << std::endl;
    BOOST_CHECK_GT(refs.count, 1e4);
}

BOOST_AUTO_TEST_CASE(throttle_performance) // NOLINT
{
    struct
    {
        ri::AsyncTool at;
        std::unique_ptr<ri::ThreadlessThrottle> thr;
        ri::AsyncSteps as1{at};
        std::size_t count{0};
    } refs;

    auto f = [&](IAsyncSteps& asi) {
        asi.sync(*(refs.thr), [&](IAsyncSteps&) { ++(refs.count); });
    };

    refs.at.immediate([&]() {
        refs.thr.reset(new ri::ThreadlessThrottle(
                refs.at,
                std::numeric_limits<ri::ThreadlessThrottle::size_type>::max()));
        refs.as1.loop(f);
        refs.as1.execute();
    });

    std::promise<void> done;
    refs.at.deferred(std::chrono::milliseconds{1000}, [&]() {
        // TODO: fix race on d-tor/cancel
        refs.as1.cancel();

        done.set_value();
    });
    done.get_future().wait();

    std::cout << "Throttle count: " << refs.count << std::endl;
    BOOST_CHECK_GT(refs.count, 1e4);
}

BOOST_AUTO_TEST_CASE(stress) // NOLINT
{
    struct TestLimiterParams : ri::ThreadlessLimiter::Params
    {
        TestLimiterParams()
        {
            concurrent = 100;
            max_queue = 850;
            rate = 10000;
            period = std::chrono::milliseconds{150};
            burst = 3000;
        }
    };

    struct
    {
        ri::AsyncTool at;
        std::unique_ptr<ri::ThreadlessLimiter> lmtr;
        std::list<ri::AsyncSteps> steps;
        std::size_t count{0};
        std::size_t limit_count{0};
        std::promise<void> done;
    } refs;

    auto f = [&](IAsyncSteps& asi) {
        asi.add(
                [&](IAsyncSteps& asi) {
                    asi.sync(*(refs.lmtr), [&](IAsyncSteps& asi) {
                        asi.sync(*(refs.lmtr), [&](IAsyncSteps&) {
                            ++(refs.count);
                        });
                    });
                },
                [&](IAsyncSteps& asi, ErrorCode code) {
                    if (code == futoin::errors::DefenseRejected) {
                        ++(refs.limit_count);
                        asi();
                    }
                });
    };

    refs.at.immediate([&]() {
        refs.lmtr.reset(
                new ri::ThreadlessLimiter(refs.at, TestLimiterParams()));

        for (size_t i = 1000; i > 0; --i) {
            refs.steps.emplace_back(refs.at);
            auto& asi = refs.steps.back();
            asi.loop(f);
            asi.execute();
        }

        refs.at.deferred(std::chrono::milliseconds{1000}, [&]() {
            for (auto& asi : refs.steps) {
                asi.cancel();
            }

            refs.done.set_value();
        });
    });

    refs.done.get_future().wait();

    std::cout << "Stress count: " << refs.count << std::endl;
    std::cout << "Limit count: " << refs.limit_count << std::endl;
    BOOST_CHECK_GT(refs.count, 1e4);
}

BOOST_AUTO_TEST_SUITE_END() // NOLINT

//=============================================================================

BOOST_AUTO_TEST_SUITE_END() // NOLINT
