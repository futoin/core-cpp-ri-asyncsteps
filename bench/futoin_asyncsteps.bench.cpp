//-----------------------------------------------------------------------------
// Copyright 2018-2026 FutoIn Project (https://futoin.org)
// Copyright 2018-2026 Andrey Galkin <andrey@futoin.org>
//
// Licensed under the FutoIn Public License 1.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://specs.futoin.org/LICENSE.txt
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//-----------------------------------------------------------------------------

#include "./common.hpp"

#include <atomic>
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

void ParallelWaitLoop_bench(unsigned count) {
    ri::AsyncTool::Params prm;
    prm.mempool_mutex = false;
    ri::AsyncTool at{[]() {}, prm};

    std::deque<ri::AsyncSteps> steps;
    std::deque<IAsyncSteps*> waiting;
    
    std::atomic<int> remaining{int(count)};
    
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
    }
    
    ri::AsyncSteps f(at);
    
    f.loop([&](IAsyncSteps& asi){
        if (remaining <= 0) {
            asi.breakLoop();
        }

        while (!waiting.empty()) {
            waiting.front()->success(1);
            waiting.pop_front();
        }

        // Emulate yield
        asi.waitExternal();
        at.immediate([&](){ asi.success(); });
    });
    
    f.execute();
        
    while (at.iterate().have_work) {};
        
    assert(remaining <= 0);
}


int main() {
    FTN_BENCH_ALL("FutoIn::AsyncSteps")
    return 0;
}
