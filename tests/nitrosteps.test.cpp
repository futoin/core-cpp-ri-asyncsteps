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
#include <thread>
//---
#include <futoin/ri/asynctool.hpp>
#include <futoin/ri/nitrosteps.hpp>

using namespace futoin;

BOOST_AUTO_TEST_SUITE(nitrosteps) // NOLINT

const std::chrono::milliseconds TEST_DELAY{100}; // NOLINT

//=============================================================================

BOOST_AUTO_TEST_SUITE(basic) // NOLINT

BOOST_AUTO_TEST_CASE(add_success) // NOLINT
{
    ri::AsyncTool at;
    ri::NitroSteps<> asi(at);
    auto& root = asi;

    std::atomic_size_t count{0};

    asi.add([&](IAsyncSteps& asi) {
        // BOOST_CHECK(!root);
        BOOST_CHECK(root); // nitro-specific
        ++count;
        BOOST_CHECK(asi);
    });

    asi.add(
            [&](IAsyncSteps& asi) {
                ++count;
                asi(2, 1.23, "str", true);
            },
            [](IAsyncSteps&, ErrorCode) {});

    asi.add([&](IAsyncSteps& asi, int a, double b, futoin::string&& c, bool d) {
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
    BOOST_CHECK_EQUAL(count, 4U);
}

BOOST_AUTO_TEST_CASE(add_rotate) // NOLINT
{
    ri::AsyncTool at;
    ri::NitroSteps<ri::nitro::MaxSteps<4>> asi(at);

    std::atomic_size_t count{0};

    asi.add([&](IAsyncSteps&) { ++count; });

    asi.add([&](IAsyncSteps&) { ++count; });
    asi.add([&](IAsyncSteps& asi) {
        ++count;

        asi.add([&](IAsyncSteps&) { ++count; });

        asi.add([&](IAsyncSteps&) { ++count; });
    });

    std::promise<void> done;
    asi.add([&](IAsyncSteps&) { done.set_value(); });

    BOOST_CHECK(asi);
    asi.execute();

    done.get_future().wait();
    BOOST_CHECK_EQUAL(count, 5U);
}

BOOST_AUTO_TEST_CASE(inner_add_success) // NOLINT
{
    ri::AsyncTool at;
    ri::NitroSteps<> asi(at);

    std::promise<void> done;
    std::atomic_size_t count{0};

    asi.add([&](IAsyncSteps& asi) {
        BOOST_TEST_CHECKPOINT("outer");
        ++count;

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
                    futoin::string&& c,
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
            ++count;
            done.set_value();
        });
    });

    asi.execute();

    done.get_future().wait();
    BOOST_CHECK_EQUAL(count, 6U);
}

BOOST_AUTO_TEST_CASE(state) // NOLINT
{
    ri::AsyncTool at;
    ri::NitroSteps<> asi(at);

    std::promise<void> done;

    asi.state()["str"] = futoin::string("String");
    asi.state()["int"] = 123;
    asi.state()["float"] = 1.23f;
    asi.state()["boolean"] = true;
    asi.state()["done"] = std::ref(done);

    asi.add([](IAsyncSteps& asi) {
        BOOST_CHECK_EQUAL(
                any_cast<futoin::string>(asi.state()["str"]), "String");
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
    ri::NitroSteps<> asi(at);

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
            [&](IAsyncSteps& asi, futoin::string&& res) {
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
    ri::NitroSteps<> asi(at);

    using Promise = std::promise<void>;
    std::promise<IAsyncSteps*> wait;
    Promise done;

    size_t count = 0;

    asi.add([&](IAsyncSteps& asi) {
        ++count;
        asi.setCancel([](IAsyncSteps&) {});
        wait.set_value(&asi);
    });
    asi.add([&](IAsyncSteps&) {
        ++count;
        done.set_value();
    });

    asi.execute();

    const_cast<IAsyncSteps*>(wait.get_future().get())->success();

    done.get_future().wait();
    BOOST_CHECK_EQUAL(count, 2U);
}

BOOST_AUTO_TEST_CASE(wait_external_error) // NOLINT
{
    ri::AsyncTool at;
    ri::NitroSteps<> asi(at);

    std::promise<IAsyncSteps*> wait;
    std::promise<bool> done;

    size_t count = 0;

    asi.add(
            [&](IAsyncSteps& asi) {
                ++count;
                asi.waitExternal();
                wait.set_value(&asi);
            },
            [&](IAsyncSteps& asi, ErrorCode err) {
                ++count;
                done.set_value(true);
                BOOST_CHECK_EQUAL(err, "SomeError");
                asi.state<bool>("ok", true);
                asi.success();
            });
    asi.add([&](IAsyncSteps& asi) {
        if (!asi.state<bool>("ok", false)) {
            done.set_value(false);
        }
    });

    asi.execute();

    try {
        const_cast<IAsyncSteps*>(wait.get_future().get())->error("SomeError");
    } catch (...) {
        // pass
    }

    BOOST_CHECK(done.get_future().get());
    BOOST_CHECK_EQUAL(count, 2U);
}

BOOST_AUTO_TEST_CASE(set_timeout_success) // NOLINT
{
    ri::AsyncTool at;
    ri::NitroSteps<> asi(at);

    std::promise<IAsyncSteps*> wait;
    std::promise<void> done;

    size_t count = 0;

    asi.add([&](IAsyncSteps& asi) {
        ++count;
        asi.setTimeout(TEST_DELAY);
        wait.set_value(&asi);
    });
    asi.add([&](IAsyncSteps&) {
        ++count;
        done.set_value();
    });

    asi.execute();

    const_cast<IAsyncSteps*>(wait.get_future().get())->success();

    done.get_future().wait();
    BOOST_CHECK_EQUAL(count, 2U);
}

BOOST_AUTO_TEST_CASE(set_timeout_fail) // NOLINT
{
    ri::AsyncTool at;
    ri::NitroSteps<> asi(at);

    std::promise<void> done;

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
    asi.add([&](IAsyncSteps&) {
        ++count;
        done.set_value();
    });

    asi.execute();

    done.get_future().wait();
    BOOST_CHECK_EQUAL(count, 3U);
}

BOOST_AUTO_TEST_CASE(catch_trace) // NOLINT
{
    ri::AsyncTool at;
    ri::NitroSteps<> asi(at);

    using Promise = std::promise<void>;
    Promise done;

    size_t count = 0;
    asi.state().catch_trace = [&](const std::exception&) { ++count; };
    asi.state()["done"] = std::ref(done);

    asi.add(
            [](IAsyncSteps& asi) {
                asi.add([](IAsyncSteps& asi) { asi.error("test"); },
                        [](IAsyncSteps& asi, ErrorCode) {
                            asi.error("other");
                        });
            },
            [](IAsyncSteps& asi, ErrorCode) {
                asi.state<std::reference_wrapper<Promise>>("done")
                        .get()
                        .set_value();
                asi.success();
            });

    asi.execute();
    done.get_future().wait();
    BOOST_CHECK_EQUAL(count, 2U);
}

BOOST_AUTO_TEST_SUITE_END() // NOLINT

//=============================================================================

BOOST_AUTO_TEST_SUITE(loops) // NOLINT

BOOST_AUTO_TEST_CASE(repeat) // NOLINT
{
    ri::AsyncTool at;
    ri::NitroSteps<> asi(at);

    std::promise<void> done;

    asi.state()["cnt"] = 0;
    asi.repeat(100, [](IAsyncSteps& asi, size_t i) {
        BOOST_CHECK_EQUAL(asi.state<int>("cnt"), int(i));
        asi.state<int>("cnt")++;
    });
    asi.add([&](IAsyncSteps&) { done.set_value(); });
    asi.execute();
    done.get_future().wait();

    BOOST_CHECK_EQUAL(asi.state<int>("cnt"), 100);
}

BOOST_AUTO_TEST_CASE(loop_break) // NOLINT
{
    ri::AsyncTool at;
    ri::NitroSteps<> asi(at);

    using V = std::vector<int>;

    std::promise<void> done;
    asi.state()["result"] = V();

    asi.loop(
            [](IAsyncSteps& asi) {
                auto& result = asi.state<V>("result");
                result.push_back(1);

                asi.forEach(
                        V{1, 2, 3, 4},
                        [](IAsyncSteps& asi, size_t, const int&) {
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

    asi.add([&](IAsyncSteps&) { done.set_value(); });
    asi.execute();
    done.get_future().wait();

    V required{1, 2, 3, 3, 2, 3, 3};
    auto& result = asi.state<V>("result");
    BOOST_CHECK_EQUAL_COLLECTIONS(
            result.begin(), result.end(), required.begin(), required.end());
}

BOOST_AUTO_TEST_CASE(loop_continue) // NOLINT
{
    ri::AsyncTool at;
    ri::NitroSteps<> asi(at);

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
                        [](IAsyncSteps& asi, size_t, const int&) {
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

    asi.add([&](IAsyncSteps&) { done.set_value(); });
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
    ri::NitroSteps<> asi(at);

    using V = std::vector<int>;

    std::promise<void> done;
    asi.state()["result"] = V();
    asi.state().unhandled_error = [](futoin::ErrorCode) {};

    asi.loop(
            [&](IAsyncSteps& asi) {
                auto& result = asi.state<V>("result");
                result.push_back(1);

                asi.setCancel([&](IAsyncSteps&) { done.set_value(); });

                if (result.size() > 1) {
                    asi.breakLoop();
                }

                asi.forEach(
                        V{1, 2, 3, 4},
                        [](IAsyncSteps& asi, size_t, const int&) {
                            auto& result = asi.state<V>("result");
                            result.push_back(2);

                            asi.repeat(
                                    3,
                                    [](IAsyncSteps& asi, size_t) {
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
    ri::NitroSteps<> asi(at);

    std::promise<void> done;

    asi.state()["cnt"] = 0;

    // Vector copy
    asi.forEach(std::vector<int>{1, 2, 3}, [](IAsyncSteps& asi, size_t, int&) {
        asi.state<int>("cnt")++;
    });

    std::vector<int> vec{1, 2, 3};

    // Vector ref
    asi.forEach(std::ref(vec), [](IAsyncSteps& asi, size_t, int&) {
        asi.state<int>("cnt")++;
    });

    // Vector const ref
    asi.forEach(std::cref(vec), [](IAsyncSteps& asi, size_t, const int&) {
        asi.state<int>("cnt")++;
    });

    asi.add([&](IAsyncSteps&) { done.set_value(); });
    asi.execute();
    done.get_future().wait();

    BOOST_CHECK_EQUAL(asi.state<int>("cnt"), 9);
}

BOOST_AUTO_TEST_CASE(loop_foreach_map) // NOLINT
{
    ri::AsyncTool at;
    ri::NitroSteps<> asi(at);

    std::promise<void> done;

    asi.state()["cnt"] = 0;

    std::map<int, int> map{{1, 1}, {2, 2}, {3, 3}};

    // copy
    asi.forEach(decltype(map)(map), [](IAsyncSteps& asi, int, int&) {
        asi.state<int>("cnt")++;
    });

    // ref
    asi.forEach(std::ref(map), [](IAsyncSteps& asi, const int&, int&) {
        asi.state<int>("cnt")++;
    });

    // const ref
    asi.forEach(std::cref(map), [](IAsyncSteps& asi, int, const int&) {
        asi.state<int>("cnt")++;
    });

    asi.add([&](IAsyncSteps&) { done.set_value(); });
    asi.execute();
    done.get_future().wait();

    BOOST_CHECK_EQUAL(asi.state<int>("cnt"), 9);
}

BOOST_AUTO_TEST_SUITE_END() // NOLINT

//=============================================================================

BOOST_AUTO_TEST_SUITE(parallel) // NOLINT

BOOST_AUTO_TEST_CASE(execute_outer) // NOLINT
{
    ri::AsyncTool at;
    ri::NitroSteps<> asi(at);

    using V = std::vector<int>;

    std::promise<void> done;
    asi.state()["result"] = V();

    auto& p = asi.parallel([&](IAsyncSteps&, ErrorCode err) {
        std::cout << err << std::endl;
        BOOST_CHECK(false);
        done.set_value();
    });

    p.add([](IAsyncSteps& asi) {
        asi.state<V>("result").push_back(1);

        asi.add([](IAsyncSteps& asi) { asi.state<V>("result").push_back(11); });
        asi.add([](IAsyncSteps& asi) { asi.state<V>("result").push_back(12); });
    });
    p.add([](IAsyncSteps& asi) {
        asi.state<V>("result").push_back(2);

        asi.add([](IAsyncSteps& asi) { asi.state<V>("result").push_back(21); });
        asi.add([](IAsyncSteps& asi) { asi.state<V>("result").push_back(22); });
    });
    p.add([](IAsyncSteps& asi) {
        asi.state<V>("result").push_back(3);
        asi.add([](IAsyncSteps& asi) { asi.state<V>("result").push_back(31); });
        asi.add([](IAsyncSteps& asi) { asi.state<V>("result").push_back(32); });
    });
    p.repeat(3, [](IAsyncSteps& asi, size_t i) {
        asi.state<V>("result").push_back(40 + i);
    });

    asi.add([&](IAsyncSteps&) { done.set_value(); });
    asi.execute();

    done.get_future().wait();

    V required{1, 2, 3, 40, 11, 21, 31, 41, 12, 22, 32, 42};
    BOOST_CHECK_EQUAL_COLLECTIONS(
            asi.state<V>("result").begin(),
            asi.state<V>("result").end(),
            required.begin(),
            required.end());
}

BOOST_AUTO_TEST_CASE(execute_inner) // NOLINT
{
    ri::AsyncTool at;
    ri::NitroSteps<> asi(at);

    using V = std::vector<int>;

    std::promise<void> done;
    asi.state()["result"] = V();

    asi.add([](IAsyncSteps& asi) {
        asi.state<V>("result").push_back(0);
        auto& p = asi.parallel();

        p.add([](IAsyncSteps& asi) {
            asi.state<V>("result").push_back(1);

            asi.add([](IAsyncSteps& asi) {
                asi.state<V>("result").push_back(11);
            });
            asi.add([](IAsyncSteps& asi) {
                asi.state<V>("result").push_back(12);
            });
        });
        p.add([](IAsyncSteps& asi) {
            asi.state<V>("result").push_back(2);

            asi.add([](IAsyncSteps& asi) {
                asi.state<V>("result").push_back(21);
            });
            asi.add([](IAsyncSteps& asi) {
                asi.state<V>("result").push_back(22);
            });
        });
        p.add([](IAsyncSteps& asi) {
            asi.state<V>("result").push_back(3);
            asi.add([](IAsyncSteps& asi) {
                asi.state<V>("result").push_back(31);
            });
            asi.add([](IAsyncSteps& asi) {
                asi.state<V>("result").push_back(32);
            });
        });
        p.repeat(3, [](IAsyncSteps& asi, size_t i) {
            asi.state<V>("result").push_back(40 + i);
        });
    });

    asi.add([&](IAsyncSteps&) { done.set_value(); });
    asi.execute();

    done.get_future().wait();

    V required{0, 1, 2, 3, 40, 11, 21, 31, 41, 12, 22, 32, 42};
    BOOST_CHECK_EQUAL_COLLECTIONS(
            asi.state<V>("result").begin(),
            asi.state<V>("result").end(),
            required.begin(),
            required.end());
}

BOOST_AUTO_TEST_CASE(error_outer) // NOLINT
{
    ri::AsyncTool at;
    ri::NitroSteps<> asi(at);

    using V = std::vector<int>;

    std::promise<void> done;
    asi.state()["result"] = V();
    asi.state().unhandled_error = [](futoin::ErrorCode) {};

    auto& p = asi.parallel([](IAsyncSteps& asi, ErrorCode err) {
        asi.state<V>("result").push_back(0);

        if (strcmp(err, "MyError") == 0) {
            asi.success();
        }
    });

    p.add([](IAsyncSteps& asi) {
        asi.state<V>("result").push_back(1);

        asi.add([](IAsyncSteps& asi) { asi.state<V>("result").push_back(11); });
        asi.add([](IAsyncSteps& asi) {
            asi.state<V>("result").push_back(12);
            asi.error("MyError");
        });
    });
    p.add([](IAsyncSteps& asi) {
        asi.state<V>("result").push_back(2);

        asi.add([](IAsyncSteps& asi) { asi.state<V>("result").push_back(21); });
        asi.add([](IAsyncSteps& asi) { asi.state<V>("result").push_back(22); });
    });
    p.repeat(2, [](IAsyncSteps& asi, size_t i) {
        asi.state<V>("result").push_back(40 + i);
    });

    asi.add([&](IAsyncSteps&) { done.set_value(); });
    asi.execute();

    done.get_future().wait();

    V required{1, 2, 40, 11, 21, 41, 12, 0};
    BOOST_CHECK_EQUAL_COLLECTIONS(
            asi.state<V>("result").begin(),
            asi.state<V>("result").end(),
            required.begin(),
            required.end());
}

BOOST_AUTO_TEST_CASE(execute_reuse) // NOLINT
{
    ri::AsyncTool at;
    ri::NitroSteps<> asi(at);

    using V = std::vector<int>;

    std::promise<void> done;
    asi.state()["result"] = V();

    asi.repeat(2, [&](IAsyncSteps& asi, std::size_t) {
        auto& p = asi.parallel([&](IAsyncSteps&, ErrorCode err) {
            asi.state<V>("result").push_back(0);
            std::cout << err << std::endl;
            BOOST_CHECK(false);
            done.set_value();
        });

        p.add([](IAsyncSteps& asi) { asi.state<V>("result").push_back(1); });
        p.add([](IAsyncSteps& asi) { asi.state<V>("result").push_back(2); });
    });

    asi.add([&](IAsyncSteps&) {
        asi.state<V>("result").push_back(3);
        done.set_value();
    });
    asi.execute();

    done.get_future().wait();

    V required{1, 2, 1, 2, 3};
    BOOST_CHECK_EQUAL_COLLECTIONS(
            asi.state<V>("result").begin(),
            asi.state<V>("result").end(),
            required.begin(),
            required.end());
}

BOOST_AUTO_TEST_SUITE_END() // NOLINT

//=============================================================================

BOOST_AUTO_TEST_SUITE(futures) // NOLINT

BOOST_AUTO_TEST_CASE(promise_void) // NOLINT
{
    ri::AsyncTool at;
    ri::NitroSteps<> asi(at);

    std::atomic_size_t count{0};

    asi.add([&](IAsyncSteps& asi) {
        count.fetch_add(1);

        asi.add([&](IAsyncSteps&) { count.fetch_add(1); });
    });
    asi.add([&](IAsyncSteps&) { count.fetch_add(1); });

    asi.promise().wait();
    BOOST_CHECK_EQUAL(count.load(), 3U);
}

BOOST_AUTO_TEST_CASE(promise_res) // NOLINT
{
    ri::AsyncTool at;
    ri::NitroSteps<> asi(at);

    std::atomic_size_t count{0};

    asi.add([&](IAsyncSteps& asi) {
        count.fetch_add(1);

        asi.add([&](IAsyncSteps&) { count.fetch_add(1); });
    });
    asi.add([&](IAsyncSteps& asi) {
        count.fetch_add(1);
        asi(123);
    });

    BOOST_CHECK_EQUAL(asi.promise<int>().get(), 123);
    BOOST_CHECK_EQUAL(count.load(), 3U);
}

BOOST_AUTO_TEST_CASE(promise_error) // NOLINT
{
    ri::AsyncTool at;
    ri::NitroSteps<> asi(at);

    std::atomic_size_t count{0};

    asi.add([&](IAsyncSteps& asi) {
        count.fetch_add(1);

        asi.add([&](IAsyncSteps& asi) {
            count.fetch_add(1);
            asi.error("MyError");
        });
    });
    asi.add([&](IAsyncSteps& asi) {
        count.fetch_add(1);
        asi(123);
    });

    BOOST_CHECK_THROW(asi.promise().get(), Error);
    BOOST_CHECK_EQUAL(count.load(), 2U);
}

BOOST_AUTO_TEST_CASE(await_void) // NOLINT
{
    ri::AsyncTool at;
    ri::NitroSteps<> asi(at);

    std::atomic_size_t count{0};
    std::promise<void> ext;

    asi.add([&](IAsyncSteps& asi) {
        count.fetch_add(1);

        asi.await(ext.get_future());

        asi.add([&](IAsyncSteps&) { count.fetch_add(1); });
    });

    asi.execute();

    at.deferred(TEST_DELAY, [&]() {
        BOOST_CHECK_EQUAL(count.load(), 1U);
        ext.set_value();
    });

    std::promise<void> done;
    at.deferred(TEST_DELAY * 2, [&]() {
        BOOST_CHECK_EQUAL(count.load(), 2U);
        done.set_value();
    });

    done.get_future().wait();
}

BOOST_AUTO_TEST_CASE(await_res) // NOLINT
{
    ri::AsyncTool at;
    ri::NitroSteps<> asi(at);

    std::atomic_size_t count{0};
    std::promise<int> ext;

    asi.add([&](IAsyncSteps& asi) {
        count.fetch_add(1);

        asi.await(ext.get_future());

        asi.add([&](IAsyncSteps&, int a) {
            BOOST_CHECK_EQUAL(a, 123);
            count.fetch_add(1);
        });
    });

    asi.execute();

    at.deferred(TEST_DELAY, [&]() {
        BOOST_CHECK_EQUAL(count.load(), 1U);
        ext.set_value(123);
    });

    std::promise<void> done;
    at.deferred(TEST_DELAY * 2, [&]() {
        BOOST_CHECK_EQUAL(count.load(), 2U);
        done.set_value();
    });

    done.get_future().wait();
}

BOOST_AUTO_TEST_CASE(await_cancel) // NOLINT
{
    ri::AsyncTool at;
    ri::NitroSteps<> asi(at);

    std::atomic_size_t count{0};
    std::promise<int> ext;

    asi.add([&](IAsyncSteps& asi) {
        count.fetch_add(1);

        asi.await(ext.get_future());

        asi.add([&](IAsyncSteps&, int a) {
            BOOST_CHECK_EQUAL(a, 123);
            count.fetch_add(1);
        });
    });

    asi.execute();

    at.deferred(TEST_DELAY, [&]() {
        BOOST_CHECK_EQUAL(count.load(), 1U);
        asi.cancel();
    });

    std::promise<void> done;
    at.deferred(TEST_DELAY * 2, [&]() {
        BOOST_CHECK_EQUAL(count.load(), 1U);
        done.set_value();
    });

    done.get_future().wait();
}

BOOST_AUTO_TEST_CASE(await_error) // NOLINT
{
    ri::AsyncTool at;
    ri::NitroSteps<> asi(at);

    std::atomic_size_t count{0};
    std::promise<int> ext;
    std::promise<void> done;

    asi.add(
            [&](IAsyncSteps& asi) {
                count.fetch_add(1);

                asi.await(ext.get_future());

                asi.add([&](IAsyncSteps&, int a) {
                    BOOST_CHECK_EQUAL(a, 123);
                    count.fetch_add(1);
                });
            },
            [&](IAsyncSteps& asi, ErrorCode err) {
                BOOST_CHECK_EQUAL(err, "MyError");
                count.fetch_add(5);
                asi();
            });
    asi.add([&](IAsyncSteps&) {
        BOOST_CHECK_EQUAL(count.load(), 6U);
        done.set_value();
    });

    asi.execute();

    at.deferred(TEST_DELAY, [&]() {
        BOOST_CHECK_EQUAL(count.load(), 1U);
        ext.set_exception(std::make_exception_ptr(Error("MyError")));
    });

    done.get_future().wait();
}

struct AllocObject
{
    AllocObject()
    {
        ++new_count;
    }

    ~AllocObject()
    {
        ++del_count;
    }

    static size_t new_count;
    static size_t del_count;
};

size_t AllocObject::new_count;
size_t AllocObject::del_count;

BOOST_AUTO_TEST_CASE(stack_alloc) // NOLINT
{
    {
        ri::AsyncTool at;
        ri::NitroSteps<> asi(at);

        AllocObject::new_count = 0;
        AllocObject::del_count = 0;

        auto& ref = asi.stack<AllocObject>();
        (void) ref;
        BOOST_CHECK_EQUAL(AllocObject::new_count, 1U);
        BOOST_CHECK_EQUAL(AllocObject::del_count, 0U);

        asi.add(
                [&](IAsyncSteps& asi) {
                    BOOST_CHECK_EQUAL(AllocObject::new_count, 1U);
                    BOOST_CHECK_EQUAL(AllocObject::del_count, 0U);

                    asi.stack<AllocObject>();
                    asi.stack<AllocObject>();

                    asi.add([&](IAsyncSteps& asi) {
                        BOOST_CHECK_EQUAL(AllocObject::new_count, 3U);
                        BOOST_CHECK_EQUAL(AllocObject::del_count, 0U);

                        asi.stack<AllocObject>();

                        BOOST_CHECK_EQUAL(AllocObject::new_count, 4U);

                        asi.error("Test");
                    });
                },
                [&](IAsyncSteps& asi, ErrorCode) {
                    BOOST_CHECK_EQUAL(AllocObject::new_count, 4U);
                    BOOST_CHECK_EQUAL(AllocObject::del_count, 1U);
                    asi();
                });
        asi.add([&](IAsyncSteps&) {
            BOOST_CHECK_EQUAL(AllocObject::new_count, 4U);
            BOOST_CHECK_EQUAL(AllocObject::del_count, 3U);
        });

        asi.promise().wait();
    }

    BOOST_CHECK_EQUAL(AllocObject::new_count, 4U);
    BOOST_CHECK_EQUAL(AllocObject::del_count, 4U);
}

BOOST_AUTO_TEST_SUITE_END() // NOLINT

//=============================================================================

BOOST_AUTO_TEST_SUITE(spi) // NOLINT

constexpr size_t TEST_STEP_INSTANCE_COUNT = 128;

BOOST_AUTO_TEST_CASE(instance_outer) // NOLINT
{
    size_t count = 0;
    ri::AsyncTool at;

    std::atomic_bool done{false};

    at.deferred(
            TEST_DELAY, [&]() { done.store(true, std::memory_order_release); });

    while (!done.load(std::memory_order_consume)) {
        ri::NitroSteps<> asi(at);
        ++count;
    }

    std::cout << "Outer instance count: " << count << std::endl;
    BOOST_CHECK_GT(count, 1e4);
}

BOOST_AUTO_TEST_CASE(instance_inner) // NOLINT
{
    struct
    {
        ri::AsyncTool at;
        ri::AsyncTool at2;
        std::atomic_bool done{false};
        std::promise<size_t> result;
    } refs;

    refs.at2.deferred(TEST_DELAY, [&]() {
        refs.done.store(true, std::memory_order_release);
    });

    refs.at.immediate([&]() {
        size_t count = 0;

        while (!refs.done.load(std::memory_order_consume)) {
            ri::NitroSteps<> asi(refs.at);
            ++count;
        }

        refs.result.set_value(count);
    });

    size_t count = refs.result.get_future().get();
    std::cout << "Inner instance count: " << count << std::endl;
    BOOST_CHECK_GT(count, 1e4);
}

BOOST_AUTO_TEST_CASE(instance_concurrent_inner) // NOLINT
{
    struct
    {
        ri::AsyncTool at;
        ri::AsyncTool at2;
        ri::AsyncTool at3;
        std::atomic_bool done{false};
        std::promise<size_t> result;
        std::promise<size_t> result2;
    } refs;

    refs.at3.deferred(TEST_DELAY, [&]() {
        refs.done.store(true, std::memory_order_release);
    });

    refs.at.immediate([&]() {
        size_t count = 0;

        while (!refs.done.load(std::memory_order_consume)) {
            ri::NitroSteps<> asi(refs.at);
            ++count;
        }

        refs.result.set_value(count);
    });

    refs.at2.immediate([&]() {
        size_t count = 0;

        while (!refs.done.load(std::memory_order_consume)) {
            ri::NitroSteps<> asi(refs.at);
            ++count;
        }

        refs.result2.set_value(count);
    });

    size_t count =
            refs.result.get_future().get() + refs.result2.get_future().get();
    std::cout << "Inner concurrent instance count: " << count << std::endl;
    BOOST_CHECK_GT(count, 1e4);
}

BOOST_AUTO_TEST_CASE(instance_outer_add) // NOLINT
{
    size_t count = 0;
    ri::AsyncTool at;

    std::atomic_bool done{false};

    at.deferred(
            TEST_DELAY, [&]() { done.store(true, std::memory_order_release); });

    while (!done.load(std::memory_order_consume)) {
        ri::NitroSteps<ri::nitro::MaxSteps<TEST_STEP_INSTANCE_COUNT>> asi(at);

        for (auto i = TEST_STEP_INSTANCE_COUNT; i > 0; --i) {
            asi.add([](IAsyncSteps&) {});
            ++count;
        }
    }

    std::cout << "Outer instance.add count: " << count << std::endl;
    BOOST_CHECK_GT(count, 1e4);
}

BOOST_AUTO_TEST_CASE(instance_inner_add) // NOLINT
{
    struct
    {
        ri::AsyncTool at;
        ri::AsyncTool at2;
        std::atomic_bool done{false};
        std::promise<size_t> result;
    } refs;

    refs.at2.deferred(TEST_DELAY, [&]() {
        refs.done.store(true, std::memory_order_release);
    });

    refs.at.immediate([&]() {
        size_t count = 0;

        while (!refs.done.load(std::memory_order_consume)) {
            ri::NitroSteps<ri::nitro::MaxSteps<TEST_STEP_INSTANCE_COUNT>> asi(
                    refs.at);

            for (auto i = TEST_STEP_INSTANCE_COUNT; i > 0; --i) {
                asi.add([](IAsyncSteps&) {});
                ++count;
            }
        }

        refs.result.set_value(count);
    });

    size_t count = refs.result.get_future().get();
    std::cout << "Inner instance.add count: " << count << std::endl;
    BOOST_CHECK_GT(count, 1e4);
}

BOOST_AUTO_TEST_CASE(instance_outer_loop) // NOLINT
{
    size_t count = 0;
    ri::AsyncTool at;

    std::atomic_bool done{false};

    at.deferred(
            TEST_DELAY, [&]() { done.store(true, std::memory_order_release); });

    while (!done.load(std::memory_order_consume)) {
        ri::NitroSteps<
                ri::nitro::MaxSteps<TEST_STEP_INSTANCE_COUNT>,
                ri::nitro::MaxExtended<TEST_STEP_INSTANCE_COUNT>>
                asi(at);

        for (auto i = TEST_STEP_INSTANCE_COUNT; i > 0; --i) {
            asi.loop([](IAsyncSteps&) {});
            ++count;
        }
    }

    std::cout << "Outer instance.loop count: " << count << std::endl;
    BOOST_CHECK_GT(count, 1e4);
}

BOOST_AUTO_TEST_CASE(instance_inner_loop) // NOLINT
{
    struct
    {
        ri::AsyncTool at;
        ri::AsyncTool at2;
        std::atomic_bool done{false};
        std::promise<size_t> result;
    } refs;

    refs.at2.deferred(TEST_DELAY, [&]() {
        refs.done.store(true, std::memory_order_release);
    });

    refs.at.immediate([&]() {
        size_t count = 0;

        while (!refs.done.load(std::memory_order_consume)) {
            ri::NitroSteps<
                    ri::nitro::MaxSteps<TEST_STEP_INSTANCE_COUNT>,
                    ri::nitro::MaxExtended<TEST_STEP_INSTANCE_COUNT>>
                    asi(refs.at);

            for (auto i = TEST_STEP_INSTANCE_COUNT; i > 0; --i) {
                asi.loop([](IAsyncSteps&) {});
                ++count;
            }
        }

        refs.result.set_value(count);
    });

    size_t count = refs.result.get_future().get();
    std::cout << "Inner instance.loop count: " << count << std::endl;
    BOOST_CHECK_GT(count, 1e4);
}

BOOST_AUTO_TEST_CASE(instance_outer_parallel) // NOLINT
{
    size_t count = 0;
    ri::AsyncTool at;

    std::atomic_bool done{false};

    at.deferred(
            TEST_DELAY, [&]() { done.store(true, std::memory_order_release); });

    while (!done.load(std::memory_order_consume)) {
        ri::NitroSteps<
                ri::nitro::MaxSteps<TEST_STEP_INSTANCE_COUNT>,
                ri::nitro::MaxExtended<TEST_STEP_INSTANCE_COUNT>>
                asi(at);

        for (auto i = TEST_STEP_INSTANCE_COUNT; i > 0; --i) {
            asi.parallel([](IAsyncSteps&, ErrorCode) {});
            ++count;
        }
    }

    std::cout << "Outer instance.parallel count: " << count << std::endl;
    BOOST_CHECK_GT(count, 1e4);
}

BOOST_AUTO_TEST_CASE(instance_inner_parallel) // NOLINT
{
    struct
    {
        ri::AsyncTool at;
        ri::AsyncTool at2;
        std::atomic_bool done{false};
        std::promise<size_t> result;
    } refs;

    refs.at2.deferred(TEST_DELAY, [&]() {
        refs.done.store(true, std::memory_order_release);
    });

    refs.at.immediate([&]() {
        size_t count = 0;

        while (!refs.done.load(std::memory_order_consume)) {
            ri::NitroSteps<
                    ri::nitro::MaxSteps<TEST_STEP_INSTANCE_COUNT>,
                    ri::nitro::MaxExtended<TEST_STEP_INSTANCE_COUNT>>
                    asi(refs.at);

            for (auto i = TEST_STEP_INSTANCE_COUNT; i > 0; --i) {
                asi.parallel([](IAsyncSteps&, ErrorCode) {});
                ++count;
            }
        }

        refs.result.set_value(count);
    });

    size_t count = refs.result.get_future().get();
    std::cout << "Inner instance.parallel count: " << count << std::endl;
    BOOST_CHECK_GT(count, 1e4);
}

BOOST_AUTO_TEST_CASE(plain_outer_loop) // NOLINT
{
    ri::AsyncTool at;
    ri::NitroSteps<> asi(at);

    volatile size_t count = 0;

    asi.loop([&](IAsyncSteps&) { ++count; });

    std::promise<void> done;
    at.deferred(std::chrono::milliseconds(1000), [&]() {
        asi.cancel();
        done.set_value();
    });

    asi.execute();
    done.get_future().wait();

    std::cout << "Plain outer iteration count: " << count << std::endl;
    BOOST_CHECK_GT(count, 1e4);
}

BOOST_AUTO_TEST_CASE(plain_inner_loop) // NOLINT
{
    ri::AsyncTool at;
    ri::NitroSteps<> asi(at);

    volatile size_t count = 0;

    asi.add([&](IAsyncSteps& asi) {
        asi.loop([&](IAsyncSteps&) { ++count; });
    });

    std::promise<void> done;
    at.deferred(std::chrono::milliseconds(1000), [&]() {
        asi.cancel();
        done.set_value();
    });

    asi.execute();
    done.get_future().wait();

    std::cout << "Plain inner iteration count: " << count << std::endl;
    BOOST_CHECK_GT(count, 1e4);
}

BOOST_AUTO_TEST_CASE(parallel_outer_loop) // NOLINT
{
    ri::AsyncTool at;
    ri::NitroSteps<> asi(at);

    volatile size_t count = 0;

    asi.loop([&](IAsyncSteps& asi) {
        auto& p = asi.parallel();
        p.add([&](IAsyncSteps&) { ++count; });
        p.add([&](IAsyncSteps&) { ++count; });
        p.add([&](IAsyncSteps&) { ++count; });
    });

    std::promise<void> done;
    at.deferred(std::chrono::milliseconds(1000), [&]() {
        asi.cancel();
        done.set_value();
    });

    asi.execute();
    done.get_future().wait();

    std::cout << "Plain inner parallel iteration count: " << count << std::endl;
    BOOST_CHECK_GT(count, 1e4);
}

BOOST_AUTO_TEST_CASE(double_outer_loop) // NOLINT
{
    struct
    {
        ri::AsyncTool at1;
        ri::AsyncTool at2;
        ri::NitroSteps<> as1{at1};
        ri::NitroSteps<> as2{at2};
    } refs;

    volatile size_t count1 = 0;
    volatile size_t count2 = 0;

    refs.as1.loop([&](IAsyncSteps&) { ++count1; });
    refs.as2.loop([&](IAsyncSteps&) { ++count2; });

    std::promise<void> done;
    refs.at1.deferred(std::chrono::milliseconds(1000), [&]() {
        refs.as1.cancel();
        refs.as2.cancel();
        done.set_value();
    });

    refs.as1.execute();
    refs.as2.execute();
    done.get_future().wait();

    auto total = count1 + count2;
    std::cout << "Double outer iteration 1: " << count1 << std::endl;
    std::cout << "Double outer iteration 2: " << count2 << std::endl;
    std::cout << "Double outer iteration total: " << total << std::endl;
    BOOST_CHECK_GT(total, 1e4);
}

BOOST_AUTO_TEST_CASE(double_outer_loop_nomutex) // NOLINT
{
    auto f = [](size_t& count) {
        ri::AsyncTool::Params prm;
        prm.mempool_mutex = false;
        ri::AsyncTool at{[]() {}, prm};

        GlobalMemPool::set_thread_default(at.mem_pool());

        ri::NitroSteps<> asi{at};

        std::promise<void> done;
        at.deferred(std::chrono::milliseconds(1000), [&]() { asi.cancel(); });

        asi.loop([&](IAsyncSteps&) { ++count; });
        asi.execute();
        while (at.iterate().have_work) {
        }
    };

    size_t count1 = 0;
    size_t count2 = 0;

    std::thread thread1{f, std::ref(count1)};
    std::thread thread2{f, std::ref(count2)};

    thread1.join();
    thread2.join();

    auto total = count1 + count2;
    std::cout << "Double outer iteration nomutex 1: " << count1 << std::endl;
    std::cout << "Double outer iteration nomutex 2: " << count2 << std::endl;
    std::cout << "Double outer iteration nomutex total: " << total << std::endl;
    BOOST_CHECK_GT(total, 1e4);
}

BOOST_AUTO_TEST_SUITE_END() // NOLINT

//=============================================================================

BOOST_AUTO_TEST_SUITE_END() // NOLINT
