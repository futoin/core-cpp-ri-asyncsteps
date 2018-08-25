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
//---
#include <atomic>
#include <cstdlib>
#include <future>
//---
#include <futoin/ri/asyncsteps.hpp>
#include <futoin/ri/asynctool.hpp>

using namespace futoin;

BOOST_AUTO_TEST_SUITE(asyncsteps) // NOLINT

//=============================================================================

BOOST_AUTO_TEST_SUITE(basic) // NOLINT

BOOST_AUTO_TEST_CASE(add_success) // NOLINT
{
    ri::AsyncTool at;
    ri::AsyncSteps asi(at);

    std::atomic_size_t count{0};

    asi.add([&](IAsyncSteps&) { ++count; });

    asi.add(
            [&](IAsyncSteps& asi) {
                ++count;
                asi(2, 1.23, "str", true);
            },
            [](IAsyncSteps&, ErrorCode) {});

    asi.add([&](IAsyncSteps& asi, int a, double b, std::string&& c, bool d) {
        ++count;
        BOOST_CHECK_EQUAL(a, 2);
        BOOST_CHECK_EQUAL(b, 1.23);
        BOOST_CHECK_EQUAL(c.c_str(), "str");
        BOOST_CHECK_EQUAL(d, true);

        asi.success(std::vector<int>{3, 4, 5});
    });

    asi.add([&](IAsyncSteps&, std::vector<int>&& v) {
        ++count;
        BOOST_CHECK_EQUAL(v[0], 3);
        BOOST_CHECK_EQUAL(v[1], 4);
        BOOST_CHECK_EQUAL(v[2], 5);
    });

    std::promise<void> done;
    asi.add([&](IAsyncSteps&) { done.set_value(); });
    asi.execute();

    done.get_future().wait();
    BOOST_CHECK_EQUAL(count, 4);
}

BOOST_AUTO_TEST_CASE(inner_add_success) // NOLINT
{
    ri::AsyncTool at;
    ri::AsyncSteps asi(at);

    std::promise<void> done;
    std::atomic_size_t count{0};

    asi.add([&](IAsyncSteps& asi) {
        asi.add([&](IAsyncSteps&) { ++count; });

        asi.add(
                [&](IAsyncSteps& asi) {
                    ++count;
                    asi(2, 1.23, "str", true);
                },
                [](IAsyncSteps&, ErrorCode) {});

        asi.add([&](IAsyncSteps& asi,
                    int a,
                    double b,
                    std::string&& c,
                    bool d) {
            ++count;
            BOOST_CHECK_EQUAL(a, 2);
            BOOST_CHECK_EQUAL(b, 1.23);
            BOOST_CHECK_EQUAL(c.c_str(), "str");
            BOOST_CHECK_EQUAL(d, true);

            asi.success(std::vector<int>{3, 4, 5});
        });

        asi.add([&](IAsyncSteps&, std::vector<int>&& v) {
            ++count;
            BOOST_CHECK_EQUAL(v[0], 3);
            BOOST_CHECK_EQUAL(v[1], 4);
            BOOST_CHECK_EQUAL(v[2], 5);
        });

        asi.add([&](IAsyncSteps&) { done.set_value(); });
    });

    asi.execute();

    done.get_future().wait();
    BOOST_CHECK_EQUAL(count, 4);
}

BOOST_AUTO_TEST_CASE(state) // NOLINT
{
    ri::AsyncTool at;
    ri::AsyncSteps asi(at);

    std::promise<void> done;

    asi.state()["str"] = std::string("String");
    asi.state()["int"] = 123;
    asi.state()["float"] = 1.23f;
    asi.state()["boolean"] = true;
    asi.state()["done"] = std::ref(done);

    asi.add([](IAsyncSteps& asi) {
        BOOST_CHECK_EQUAL(any_cast<std::string>(asi.state()["str"]), "String");
        BOOST_CHECK_EQUAL(any_cast<int>(asi.state()["int"]), 123);
        BOOST_CHECK_EQUAL(any_cast<float>(asi.state()["float"]), 1.23f);
        BOOST_CHECK_EQUAL(any_cast<bool>(asi.state()["boolean"]), true);

        asi.add([](IAsyncSteps& asi) {
            any_cast<std::reference_wrapper<std::promise<void>>>(
                    asi.state()["done"])
                    .get()
                    .set_value();
        });
    });

    asi.execute();
    done.get_future().wait();
}

BOOST_AUTO_TEST_CASE(handle_errors) // NOLINT
{
    ri::AsyncTool at;
    ri::AsyncSteps asi(at);

    std::promise<void> done;
    using V = std::vector<int>;
    const V required{10, 100, 1000, 10000, 1001, 101, 11, 20, 21, 210};

    asi.state()["result"] = V();

    asi.add(
            [](IAsyncSteps& asi) {
                asi.state<V>("result").push_back(10);
                asi.add(
                        [](IAsyncSteps& asi) {
                            asi.state<V>("result").push_back(100);

                            asi.add(
                                    [](IAsyncSteps& asi) {
                                        asi.state<V>("result").push_back(1000);

                                        asi.add([](IAsyncSteps& asi) {
                                            asi.state<V>("result").push_back(
                                                    10000);
                                            asi.error("FirstError");
                                        });
                                    },
                                    [](IAsyncSteps& asi, ErrorCode) {
                                        asi.state<V>("result").push_back(1001);
                                    });
                        },
                        [](IAsyncSteps& asi, ErrorCode err) {
                            asi.state<V>("result").push_back(101);

                            BOOST_CHECK_EQUAL(err, "FirstError");
                            asi.error("SecondError");
                        });
                asi.add([](IAsyncSteps& asi) {
                    asi.state<V>("result").push_back(102);
                });
            },
            [](IAsyncSteps& asi, ErrorCode err) {
                asi.state<V>("result").push_back(11);
                BOOST_CHECK_EQUAL(err, "SecondError");
                asi.success("Yes");
            });
    asi.add(
            [&](IAsyncSteps& asi, std::string&& res) {
                asi.state<V>("result").push_back(20);

                BOOST_CHECK_EQUAL(res, "Yes");

                asi.error("ThirdError");
            },
            [&](IAsyncSteps& asi, ErrorCode err) {
                asi.state<V>("result").push_back(21);

                BOOST_CHECK_EQUAL(err, "ThirdError");

                asi.add([&](IAsyncSteps&) {
                    asi.state<V>("result").push_back(210);
                    done.set_value();
                });
            });

    asi.execute();
    done.get_future().wait();

    BOOST_CHECK_EQUAL_COLLECTIONS(
            asi.state<V>("result").begin(),
            asi.state<V>("result").end(),
            required.begin(),
            required.end());
}

BOOST_AUTO_TEST_CASE(catch_trace) // NOLINT
{
    ri::AsyncTool at;
    ri::AsyncSteps asi(at);

    using Promise = std::promise<void>;
    Promise done;

    size_t count = 0;
    asi.state().catch_trace = [&](const std::exception&) { ++count; };
    asi.state()["done"] = std::ref(done);

    asi.add(
            [](IAsyncSteps& asi) {
                asi.add([](IAsyncSteps& asi) { asi.error("test"); },
                        [](IAsyncSteps& asi, ErrorCode code) {
                            asi.error("other");
                        });
            },
            [](IAsyncSteps& asi, ErrorCode code) {
                asi.state<std::reference_wrapper<Promise>>("done")
                        .get()
                        .set_value();
            });

    asi.execute();
    done.get_future().wait();
    BOOST_CHECK_EQUAL(count, 2);
}

BOOST_AUTO_TEST_SUITE_END() // NOLINT

//=============================================================================

BOOST_AUTO_TEST_SUITE(loops) // NOLINT

BOOST_AUTO_TEST_CASE(repeat) // NOLINT
{
    ri::AsyncTool at;
    ri::AsyncSteps asi(at);

    std::promise<void> done;

    asi.state()["cnt"] = 0;
    asi.repeat(100, [](IAsyncSteps& asi, size_t i) {
        BOOST_CHECK_EQUAL(asi.state<int>("cnt"), i);
        asi.state<int>("cnt")++;
    });
    asi.add([&](IAsyncSteps& asi) { done.set_value(); });
    asi.execute();
    done.get_future().wait();

    BOOST_CHECK_EQUAL(asi.state<int>("cnt"), 100);
}

BOOST_AUTO_TEST_SUITE_END() // NOLINT
//=============================================================================

BOOST_AUTO_TEST_SUITE(spi) // NOLINT

BOOST_AUTO_TEST_CASE(performance) // NOLINT
{}

BOOST_AUTO_TEST_SUITE_END() // NOLINT

//=============================================================================

BOOST_AUTO_TEST_SUITE_END() // NOLINT
