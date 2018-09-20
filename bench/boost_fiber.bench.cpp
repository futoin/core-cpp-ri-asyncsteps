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

#include "./common.hpp"

// NOTE: leads to compilation failure on Boost 1.67
//#define BOOST_FIBERS_NO_ATOMICS
#include <boost/fiber/all.hpp>

using namespace boost::fibers;
using namespace boost::this_fiber;

void Simple_bench(unsigned count) {
    boost::fibers::protected_fixedsize_stack stack_alloc;
    
    for (; count > 0; --count) {
        boost::fibers::fiber f(
                boost::fibers::launch::dispatch,
                std::allocator_arg, stack_alloc,
                [](){});
        f.join();
    }
}

void Parallel_bench(unsigned count) {
    boost::fibers::protected_fixedsize_stack stack_alloc;

    std::deque<boost::fibers::fiber> fibers;
    std::deque<boost::fibers::promise<int>> promises;
    
    // NOTE: there is OS limit for memory maps used for fiber stacks
    //       so, we run small count for count times
    for(auto i = count / bench_param::Parallel_LIMIT; i > 0; --i) {
        unsigned remaining = bench_param::Parallel_LIMIT;
        
        for (auto j = remaining; j > 0; --j) {
            fibers.emplace_back(
                    boost::fibers::launch::dispatch,
                    std::allocator_arg, stack_alloc,
                    [&](){
                        promises.emplace_back();
                        boost::fibers::future<int> f(promises.back().get_future());
                        remaining -= f.get();
                    });
        }
        
        for (auto &p : promises) {
            p.set_value(1);
        }
        
        for (auto &f : fibers) {
            f.join();
        }
        
        assert(remaining == 0);
        fibers.clear();
        promises.clear();
    }
}

void ParallelLoop_bench(unsigned count) {
    boost::fibers::protected_fixedsize_stack stack_alloc;

    std::deque<boost::fibers::fiber> fibers;
    std::deque<boost::fibers::promise<int>> promises;
    
    int remaining = count;

    // NOTE: there is OS limit for memory maps used for fiber stacks
    //       so, we run small count for count times
    for(auto i = bench_param::Parallel_LIMIT; i > 0; --i) {
        fibers.emplace_back(
                boost::fibers::launch::dispatch,
                std::allocator_arg, stack_alloc,
                [&](){
                    do {
                        promises.emplace_back();
                        boost::fibers::future<int> f(promises.back().get_future());
                        remaining -= f.get();
                    } while (remaining > 0);
                });
    }

    boost::fibers::fiber f(
        boost::fibers::launch::dispatch,
        std::allocator_arg, stack_alloc,
        [&](){
            while (remaining > 0) {
                while (!promises.empty()) {
                    promises.front().set_value(1);
                    promises.pop_front();
                }
                
                boost::this_fiber::yield();
            }
        });
    f.join();

    while (!promises.empty()) {
        promises.front().set_value(0);
        promises.pop_front();
    }

    for (auto &f : fibers) {
        f.join();
    }
        
    assert(remaining <= 0);
}

int main() {
    FTN_BENCH_ALL("Boost.Fiber")
    return 0;
}
