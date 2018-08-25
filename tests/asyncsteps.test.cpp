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

const std::chrono::milliseconds TEST_DELAY{100}; // NOLINT

//=============================================================================

BOOST_AUTO_TEST_SUITE(basic) // NOLINT

BOOST_AUTO_TEST_CASE(add_success) // NOLINT
{
    ri::AsyncTool at;
    ri::AsyncSteps asi(at);
    auto &root = asi;

    std::atomic_size_t count{0};

    asi.add([&](IAsyncSteps& asi) {
        BOOST_CHECK(!root);
        ++count;
        BOOST_CHECK(asi);
    });

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

    BOOST_CHECK(asi);
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
        BOOST_TEST_CHECKPOINT("outer");
        asi.add([&](IAsyncSteps&) {
            BOOST_TEST_CHECKPOINT("inner 2");
            ++count;
        });

        asi.add(
                [&](IAsyncSteps& asi) {
                    BOOST_TEST_CHECKPOINT("inner 2");
                    ++count;
                    asi(2, 1.23, "str", true);
                },
                [](IAsyncSteps&, ErrorCode) {});

        asi.add([&](IAsyncSteps& asi,
                    int a,
                    double b,
                    std::string&& c,
                    bool d) {
            BOOST_TEST_CHECKPOINT("inner 3");
            ++count;
            BOOST_CHECK_EQUAL(a, 2);
            BOOST_CHECK_EQUAL(b, 1.23);
            BOOST_CHECK_EQUAL(c.c_str(), "str");
            BOOST_CHECK_EQUAL(d, true);

            asi.success(std::vector<int>{3, 4, 5});
        });

        asi.add([&](IAsyncSteps&, std::vector<int>&& v) {
            BOOST_TEST_CHECKPOINT("inner 4");
            ++count;
            BOOST_CHECK_EQUAL(v[0], 3);
            BOOST_CHECK_EQUAL(v[1], 4);
            BOOST_CHECK_EQUAL(v[2], 5);
        });

        asi.add([&](IAsyncSteps&) {
            BOOST_TEST_CHECKPOINT("inner 5");
            done.set_value();
        });
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

                asi.add([&](IAsyncSteps& asi) {
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

BOOST_AUTO_TEST_CASE(set_cancel_success) // NOLINT
{
    ri::AsyncTool at;
    ri::AsyncSteps asi(at);

    using Promise = std::promise<void>;
    std::promise<IAsyncSteps*> wait;
    Promise done;

    size_t count = 0;

    asi.add([&](IAsyncSteps& asi) {
        ++count;
        asi.setCancel([](IAsyncSteps&) {});
        wait.set_value(&asi);
    });
    asi.add([&](IAsyncSteps& asi) {
        ++count;
        done.set_value();
    });

    asi.execute();

    const_cast<IAsyncSteps*>(wait.get_future().get())->success();

    done.get_future().wait();
    BOOST_CHECK_EQUAL(count, 2);
}

BOOST_AUTO_TEST_CASE(wait_external_error) // NOLINT
{
    ri::AsyncTool at;
    ri::AsyncSteps asi(at);

    using Promise = std::promise<void>;
    std::promise<IAsyncSteps*> wait;
    Promise done;

    size_t count = 0;

    asi.add(
            [&](IAsyncSteps& asi) {
                ++count;
                asi.waitExternal();
                wait.set_value(&asi);
            },
            [&](IAsyncSteps& asi, ErrorCode err) {
                ++count;
                done.set_value();
            });
    asi.add([&](IAsyncSteps& asi) {
        count += 5;
        done.set_value();
    });

    asi.execute();

    try {
        const_cast<IAsyncSteps*>(wait.get_future().get())->error("SomeError");
    } catch (...) {
        // pass
    }

    done.get_future().wait();
    BOOST_CHECK_EQUAL(count, 2);
}

BOOST_AUTO_TEST_CASE(set_timeout_success) // NOLINT
{
    ri::AsyncTool at;
    ri::AsyncSteps asi(at);

    using Promise = std::promise<void>;
    std::promise<IAsyncSteps*> wait;
    Promise done;

    size_t count = 0;

    asi.add([&](IAsyncSteps& asi) {
        ++count;
        asi.setTimeout(TEST_DELAY);
        wait.set_value(&asi);
    });
    asi.add([&](IAsyncSteps& asi) {
        ++count;
        done.set_value();
    });

    asi.execute();

    const_cast<IAsyncSteps*>(wait.get_future().get())->success();

    done.get_future().wait();
    BOOST_CHECK_EQUAL(count, 2);
}

BOOST_AUTO_TEST_CASE(set_timeout_fail) // NOLINT
{
    ri::AsyncTool at;
    ri::AsyncSteps asi(at);

    using Promise = std::promise<void>;
    Promise done;

    size_t count = 0;

    asi.add(
            [&](IAsyncSteps& asi) {
                ++count;
                asi.setTimeout(TEST_DELAY);
            },
            [&](IAsyncSteps& asi, ErrorCode err) {
                ++count;
                BOOST_CHECK_EQUAL(err, "Timeout");
                asi.success();
            });
    asi.add([&](IAsyncSteps& asi) {
        ++count;
        done.set_value();
    });

    asi.execute();

    done.get_future().wait();
    BOOST_CHECK_EQUAL(count, 3);
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

BOOST_AUTO_TEST_CASE(loop_break) // NOLINT
{
    ri::AsyncTool at;
    ri::AsyncSteps asi(at);

    using V = std::vector<int>;

    std::promise<void> done;
    asi.state()["result"] = V();

    asi.loop(
            [](IAsyncSteps& asi) {
                auto& result = asi.state<V>("result");
                result.push_back(1);

                asi.forEach(
                        V{1, 2, 3, 4},
                        [](IAsyncSteps& asi, size_t index, const int& val) {
                            auto& result = asi.state<V>("result");
                            result.push_back(2);

                            asi.repeat(
                                    3,
                                    [](IAsyncSteps& asi, size_t i) {
                                        auto& result = asi.state<V>("result");
                                        result.push_back(3);

                                        if (i == 1) {
                                            if (result.size() == 4) {
                                                asi.breakLoop();
                                            } else {
                                                asi.breakLoop("Outer");
                                            }
                                        }
                                    },
                                    "Inner");
                        },
                        "Middle");
            },
            "Outer");

    asi.add([&](IAsyncSteps& asi) { done.set_value(); });
    asi.execute();
    done.get_future().wait();

    V required{1, 2, 3, 3, 2, 3, 3};
    BOOST_CHECK_EQUAL_COLLECTIONS(
            asi.state<V>("result").begin(),
            asi.state<V>("result").end(),
            required.begin(),
            required.end());
}

BOOST_AUTO_TEST_CASE(loop_continue) // NOLINT
{
    ri::AsyncTool at;
    ri::AsyncSteps asi(at);

    using V = std::vector<int>;

    std::promise<void> done;
    asi.state()["result"] = V();

    asi.loop(
            [](IAsyncSteps& asi) {
                auto& result = asi.state<V>("result");
                result.push_back(1);

                if (result.size() > 1) {
                    asi.breakLoop();
                }

                asi.forEach(
                        V{1, 2, 3, 4},
                        [](IAsyncSteps& asi, size_t index, const int& val) {
                            auto& result = asi.state<V>("result");
                            result.push_back(2);

                            asi.repeat(
                                    3,
                                    [](IAsyncSteps& asi, size_t i) {
                                        auto& result = asi.state<V>("result");
                                        result.push_back(3);

                                        if (i == 1) {
                                            if (result.size() == 4) {
                                                asi.continueLoop();
                                            } else {
                                                asi.continueLoop("Outer");
                                            }
                                        }
                                    },
                                    "Inner");
                        },
                        "Middle");
            },
            "Outer");

    asi.add([&](IAsyncSteps& asi) { done.set_value(); });
    asi.execute();
    done.get_future().wait();

    V required{1, 2, 3, 3, 3, 2, 3, 3, 1};
    BOOST_CHECK_EQUAL_COLLECTIONS(
            asi.state<V>("result").begin(),
            asi.state<V>("result").end(),
            required.begin(),
            required.end());
}

BOOST_AUTO_TEST_CASE(loop_error) // NOLINT
{
    ri::AsyncTool at;
    ri::AsyncSteps asi(at);

    using V = std::vector<int>;

    std::promise<void> done;
    asi.state()["result"] = V();

    asi.loop(
            [&](IAsyncSteps& asi) {
                auto& result = asi.state<V>("result");
                result.push_back(1);

                asi.setCancel([&](IAsyncSteps& asi) { done.set_value(); });

                if (result.size() > 1) {
                    asi.breakLoop();
                }

                asi.forEach(
                        V{1, 2, 3, 4},
                        [](IAsyncSteps& asi, size_t index, const int& val) {
                            auto& result = asi.state<V>("result");
                            result.push_back(2);

                            asi.repeat(
                                    3,
                                    [](IAsyncSteps& asi, size_t i) {
                                        auto& result = asi.state<V>("result");
                                        result.push_back(3);

                                        asi.error("MyError");
                                    },
                                    "Inner");
                        },
                        "Middle");
            },
            "Outer");

    asi.execute();
    done.get_future().wait();

    V required{1, 2, 3};
    BOOST_CHECK_EQUAL_COLLECTIONS(
            asi.state<V>("result").begin(),
            asi.state<V>("result").end(),
            required.begin(),
            required.end());
}

BOOST_AUTO_TEST_CASE(loop_foreach_vector) // NOLINT
{
    ri::AsyncTool at;
    ri::AsyncSteps asi(at);

    std::promise<void> done;

    asi.state()["cnt"] = 0;

    // Vector copy
    asi.forEach(
            std::vector<int>{1, 2, 3},
            [](IAsyncSteps& asi, size_t i, int&) { asi.state<int>("cnt")++; });

    std::vector<int> vec{1, 2, 3};

    // Vector ref
    asi.forEach(std::ref(vec), [](IAsyncSteps& asi, size_t i, int&) {
        asi.state<int>("cnt")++;
    });

    // Vector const ref
    asi.forEach(std::cref(vec), [](IAsyncSteps& asi, size_t i, const int&) {
        asi.state<int>("cnt")++;
    });

    asi.add([&](IAsyncSteps& asi) { done.set_value(); });
    asi.execute();
    done.get_future().wait();

    BOOST_CHECK_EQUAL(asi.state<int>("cnt"), 9);
}

BOOST_AUTO_TEST_CASE(loop_foreach_map) // NOLINT
{
    ri::AsyncTool at;
    ri::AsyncSteps asi(at);

    std::promise<void> done;

    asi.state()["cnt"] = 0;

    std::map<int, int> map{{1, 1}, {2, 2}, {3, 3}};

    // copy
    asi.forEach(decltype(map)(map), [](IAsyncSteps& asi, int i, int&) {
        asi.state<int>("cnt")++;
    });

    // ref
    asi.forEach(std::ref(map), [](IAsyncSteps& asi, const int& i, int&) {
        asi.state<int>("cnt")++;
    });

    // const ref
    asi.forEach(std::cref(map), [](IAsyncSteps& asi, int i, const int&) {
        asi.state<int>("cnt")++;
    });

    asi.add([&](IAsyncSteps& asi) { done.set_value(); });
    asi.execute();
    done.get_future().wait();

    BOOST_CHECK_EQUAL(asi.state<int>("cnt"), 9);
}

BOOST_AUTO_TEST_SUITE_END() // NOLINT

//=============================================================================

BOOST_AUTO_TEST_SUITE(parallel) // NOLINT
#if 0
BOOST_AUTO_TEST_CASE(execute_outer) // NOLINT
{
    ri::AsyncTool at;
    ri::AsyncSteps asi(at);

    using V = std::vector<int>;

    std::promise<void> done;
    asi.state()["result"] = V();

    auto& p = asi.parallel();

    p.add([](IAsyncSteps& asi) {
        asi.state<V>("result").push_back(1);

        asi.add([](IAsyncSteps& asi) { asi.state<V>("result").push_back(11); });
    });
    p.add([](IAsyncSteps& asi) {
        asi.state<V>("result").push_back(2);

        asi.add([](IAsyncSteps& asi) { asi.state<V>("result").push_back(21); });
    });
    p.add([](IAsyncSteps& asi) {
        asi.state<V>("result").push_back(31);
        asi.add([](IAsyncSteps& asi) { asi.state<V>("result").push_back(31); });
    });
    p.repeat(2, [](IAsyncSteps& asi, size_t i) {
        asi.state<V>("result").push_back(40 + i);
    });

    asi.add([&](IAsyncSteps& asi) { done.set_value(); });
    asi.execute();

    done.get_future().wait();

    V required{1, 2, 3, 40, 11, 21, 31, 41};
    BOOST_CHECK_EQUAL_COLLECTIONS(
            asi.state<V>("result").begin(),
            asi.state<V>("result").end(),
            required.begin(),
            required.end());
}
#endif

BOOST_AUTO_TEST_SUITE_END() // NOLINT

//=============================================================================

BOOST_AUTO_TEST_SUITE(spi) // NOLINT

BOOST_AUTO_TEST_CASE(performance) // NOLINT
{}

BOOST_AUTO_TEST_SUITE_END() // NOLINT

//=============================================================================

BOOST_AUTO_TEST_SUITE_END() // NOLINT
