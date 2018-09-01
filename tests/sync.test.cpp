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

namespace ri = futoin::ri;
using futoin::ErrorCode;
using futoin::IAsyncSteps;

BOOST_AUTO_TEST_SUITE(synctest) // NOLINT

//=============================================================================

BOOST_AUTO_TEST_SUITE(mutex) // NOLINT

BOOST_AUTO_TEST_CASE(outer) // NOLINT
{
    ri::AsyncTool at{[]() {}};
    ri::Mutex mtx;

    ri::AsyncSteps as1{at};
    ri::AsyncSteps as2{at};

    std::atomic_size_t count{0};
    std::atomic_size_t max{0};

    auto f = [&](IAsyncSteps& asi) {
        count.fetch_add(1);
        asi.add([&](IAsyncSteps& asi) {
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
    BOOST_CHECK_EQUAL(max, 1);
    BOOST_CHECK_EQUAL(count, 0);
}

BOOST_AUTO_TEST_CASE(inner) // NOLINT
{
    ri::AsyncTool at{[]() {}};
    ri::Mutex mtx;

    ri::AsyncSteps as1{at};
    ri::AsyncSteps as2{at};

    std::atomic_size_t count{0};
    std::atomic_size_t max{0};

    auto f = [&](IAsyncSteps& asi) {
        asi.sync(mtx, [&](IAsyncSteps& asi) {
            count.fetch_add(1);
            asi.add([&](IAsyncSteps& asi) {
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
    BOOST_CHECK_EQUAL(max, 1);
    BOOST_CHECK_EQUAL(count, 0);
}

BOOST_AUTO_TEST_CASE(args) // NOLINT
{
    ri::AsyncTool at{[]() {}};
    ri::Mutex mtx;

    ri::AsyncSteps asi{at};

    asi.add([](IAsyncSteps& asi) { asi(123, true); });
    asi.sync(mtx, [](IAsyncSteps& asi, int a, bool b) {
        BOOST_CHECK_EQUAL(a, 123);
        BOOST_CHECK_EQUAL(b, true);
    });

    asi.execute();
    while (at.iterate().have_work) {
    }
}

BOOST_AUTO_TEST_CASE(recursion) // NOLINT
{
    ri::AsyncTool at{[]() {}};
    ri::Mutex mtx;

    ri::AsyncSteps as1{at};
    ri::AsyncSteps as2{at};

    std::atomic_size_t count{0};
    std::atomic_size_t max{0};

    auto f = [&](IAsyncSteps& asi) {
        asi.sync(mtx, [&](IAsyncSteps& asi) {
            count.fetch_add(1);
            asi.sync(mtx, [&](IAsyncSteps& asi) {
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
    BOOST_CHECK_EQUAL(max, 1);
    BOOST_CHECK_EQUAL(count, 0);
}

BOOST_AUTO_TEST_CASE(queue_max) // NOLINT
{
    ri::AsyncTool at{[]() {}};
    ri::Mutex mtx(1, 1);

    ri::AsyncSteps as1{at};
    ri::AsyncSteps as2{at};
    ri::AsyncSteps as3{at};

    std::atomic_size_t count{0};
    std::atomic_size_t max{0};

    auto f = [&](IAsyncSteps& asi) {
        asi.sync(mtx, [&](IAsyncSteps& asi) {
            count.fetch_add(1);
            asi.sync(mtx, [&](IAsyncSteps& asi) {
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
    BOOST_CHECK_EQUAL(max, 1);
    BOOST_CHECK_EQUAL(count, 0);
}

BOOST_AUTO_TEST_CASE(multi_max) // NOLINT
{
    ri::AsyncTool at{[]() {}};
    ri::Mutex mtx(2);

    ri::AsyncSteps as1{at};
    ri::AsyncSteps as2{at};
    ri::AsyncSteps as3{at};
    ri::AsyncSteps as4{at};

    std::atomic_size_t count{0};
    std::atomic_size_t max{0};

    auto f = [&](IAsyncSteps& asi) {
        asi.sync(mtx, [&](IAsyncSteps& asi) {
            count.fetch_add(1);
            asi.sync(mtx, [&](IAsyncSteps& asi) {
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

    BOOST_CHECK_EQUAL(max, 2);
    BOOST_CHECK_EQUAL(count, 0);
}

BOOST_AUTO_TEST_SUITE_END() // NOLINT

//=============================================================================

BOOST_AUTO_TEST_SUITE_END() // NOLINT
