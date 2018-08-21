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

#include <future>

BOOST_AUTO_TEST_SUITE(asynctool) // NOLINT

//=============================================================================

using futoin::ri::AsyncTool;
auto external_poke = []() {};

BOOST_AUTO_TEST_SUITE(external_loop) // NOLINT

BOOST_AUTO_TEST_CASE(isntance) // NOLINT
{
    AsyncTool at(external_poke);
}

BOOST_AUTO_TEST_CASE(immediate) // NOLINT
{
    AsyncTool at(external_poke);
    bool fired = false;
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
    int val = 0;
    at.immediate([&]() { val = 2; });
    at.immediate([&]() { val = val * val; });

    auto res = at.iterate();

    BOOST_CHECK_EQUAL(val, 4);
    BOOST_CHECK_EQUAL(res.have_work, false);
}

BOOST_AUTO_TEST_CASE(immediate_cancel) // NOLINT
{
    AsyncTool at(external_poke);
    int val = 0;

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

BOOST_AUTO_TEST_SUITE_END() // NOLINT

//=============================================================================

BOOST_AUTO_TEST_SUITE(internal_loop) // NOLINT

BOOST_AUTO_TEST_CASE(instance) // NOLINT
{
    AsyncTool at;
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
    std::promise<void> fired;

    int val = 0;
    at.immediate([&]() { val = 2; });
    at.immediate([&]() { val = val * val; });
    at.immediate([&]() { fired.set_value(); });

    fired.get_future().wait();
    BOOST_CHECK_EQUAL(val, 4);
}

BOOST_AUTO_TEST_CASE(immediate_cancel) // NOLINT
{
    AsyncTool at;

    int val = 0;
    int fired = 0;
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

BOOST_AUTO_TEST_SUITE_END() // NOLINT

//=============================================================================

BOOST_AUTO_TEST_SUITE_END() // NOLINT
