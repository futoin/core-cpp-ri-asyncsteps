//-----------------------------------------------------------------------------
//   Copyright 2018-2023 FutoIn Project
//   Copyright 2018-2023 Andrey Galkin
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

#include <atomic>
#include <cassert>
#include <future>
#include <mutex>
#include <thread>

void Simple_bench(unsigned count) {
    for (; count > 0; --count) {
        std::thread f([](){});
        f.join();
    }
}

void Parallel_bench(unsigned count) {
    std::mutex mtx;
    std::deque<std::thread> threads;
    std::deque<std::promise<int>> promises;
    
    // NOTE: there is OS limit for threads
    for(auto i = count / bench_param::ThreadParallel_LIMIT; i > 0; --i) {
        std::atomic<int> remaining{ int(bench_param::ThreadParallel_LIMIT) };
        
        for (auto j = remaining.load(); j > 0; --j) {
            threads.emplace_back(
                    [&](){
                        mtx.lock();
                        promises.emplace_back();
                        std::future<int> f(promises.back().get_future());
                        mtx.unlock();
                        if (f.get()) {
                            --remaining;
                        }
                    });
        }

        while ((volatile unsigned)(promises.size()) != remaining) {
            std::this_thread::yield();
        }
        
        for (auto &p : promises) {
            p.set_value(1);
        }
        
        for (auto &f : threads) {
            f.join();
        }
        
        assert(remaining == 0);
        threads.clear();
        promises.clear();
    }
}

void ParallelLoop_bench(unsigned count) {
    std::mutex mtx;
    std::deque<std::thread> threads;
    std::deque<std::promise<int>> promises;
    
    std::atomic<int> remaining{int(count)};

    for(auto i = bench_param::ThreadParallel_LIMIT; i > 0; --i) {
        threads.emplace_back(
                [&](){
                    do {
                        mtx.lock();
                        promises.emplace_back();
                        std::future<int> f(promises.back().get_future());
                        mtx.unlock();
                        if (remaining > 0) {
                            remaining -= f.get();
                        }
                    } while (remaining > 0);
                });
    }

    std::thread f(
        [&](){
            while (remaining > 0) {
                mtx.lock();
                while (!promises.empty()) {
                    promises.front().set_value(1);
                    promises.pop_front();
                }
                mtx.unlock();
                
                std::this_thread::yield();
            }
        });
    f.join();

    while (!promises.empty()) {
        promises.front().set_value(0);
        promises.pop_front();
    }

    for (auto &f : threads) {
        f.join();
    }
        
    assert(remaining <= 0);
}

int main() {
    FTN_BENCH_ALL("Std.Thread")
    return 0;
}
