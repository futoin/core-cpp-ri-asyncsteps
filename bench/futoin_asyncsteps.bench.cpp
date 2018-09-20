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

#include <future>

#include <futoin/ri/asynctool.hpp>
#include <futoin/ri/asyncsteps.hpp>

using namespace futoin;

void Simple_bench(unsigned count) {
    ri::AsyncTool::Params prm;
    prm.mempool_mutex = false;
    ri::AsyncTool at{[]() {}, prm};

    for (; count > 0; --count) {
        ri::AsyncSteps asi(at);
        asi.add([](IAsyncSteps&){});
        asi.execute();
        bool res = at.iterate().have_work;
        assert(!res);
        (void) res;
    }
}

void Parallel_bench(unsigned count) {
    ri::AsyncTool::Params prm;
    prm.mempool_mutex = false;
    ri::AsyncTool at{[]() {}, prm};

    std::deque<ri::AsyncSteps> steps;
    std::deque<IAsyncSteps*> waiting;

    // NOTE: see boost::fibers benchmark for explanations
    //       of its limitations.
    for(auto i = count / bench_param::Parallel_LIMIT; i > 0; --i) {
        unsigned remaining = bench_param::Parallel_LIMIT;
        
        for (auto j = remaining; j > 0; --j) {
            steps.emplace_back(at);
            auto &asi = steps.back();
            asi.add([&](IAsyncSteps& asi){
                waiting.emplace_back(&asi);
                asi.waitExternal();
            });
            asi.add([&](IAsyncSteps&, int res){
                remaining -= res;
            });
            asi.execute();
            
            bool res = at.iterate().have_work;
            assert(!res);
            (void) res;
        }
        
        for (auto asi : waiting) {
            asi->success(1);
        }
        
        while (at.iterate().have_work) {};
        assert(remaining == 0);
        
        steps.clear();
        waiting.clear();
    }
}

void ParallelLoop_bench(unsigned count) {
    ri::AsyncTool::Params prm;
    prm.mempool_mutex = false;
    ri::AsyncTool at{[]() {}, prm};

    std::deque<ri::AsyncSteps> steps;
    std::deque<IAsyncSteps*> waiting;
    
    int remaining = count;
    
    // NOTE: see boost::fibers benchmark for explanations
    //       of its limitations.
    for(auto i = bench_param::Parallel_LIMIT; i > 0; --i) {
        steps.emplace_back(at);
        
        auto &asi = steps.back();
        asi.loop([&](IAsyncSteps& asi){
            asi.add([&](IAsyncSteps& asi){
                waiting.emplace_back(&asi);
                asi.waitExternal();
            });
            asi.add([&](IAsyncSteps&, int res){
                remaining -= res;
            });
        });
        asi.execute();
            
        bool res = at.iterate().have_work;
        assert(!res);
        (void) res;
    }
    
    ri::AsyncSteps f(at);
    
    f.loop([&](IAsyncSteps& asi){
        if (remaining <= 0) {
            asi.breakLoop();
        }
        
        if (!waiting.empty() && (remaining > 0)) {
            waiting.front()->success(1);
            waiting.pop_front();
        }
    });
    
    f.execute();
        
    while (at.iterate().have_work) {};
        
    assert(remaining <= 0);
}


int main() {
    FTN_BENCH_ALL("FutoIn::AsyncSteps")
    return 0;
}
