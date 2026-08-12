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

#include <deque>
#include <iostream>

#include <boost/timer/timer.hpp>

#define FTN_BENCH(bench_type) \
    timer.start(); \
    bench_type ## _bench(bench_param::bench_type ## _COUNT); \
    std::cout << #bench_type << ": " << timer.format() << std::endl;
    
#define FTN_BENCH_ALL(impl_type) \
    std::cout << impl_type << " benchmark" << std::endl; \
    boost::timer::cpu_timer timer; \
    FTN_BENCH(Simple); \
    FTN_BENCH(Parallel); \
    FTN_BENCH(ParallelWaitLoop); \
    

namespace bench_param {
    constexpr unsigned Simple_COUNT = 1e6;
    constexpr unsigned Parallel_COUNT = 1e6;
    constexpr unsigned Parallel_LIMIT = 3e4;
    constexpr unsigned ThreadParallel_LIMIT = 1e4;
    constexpr unsigned ParallelWaitLoop_COUNT = 1e7;
}
