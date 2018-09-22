
cmake_minimum_required(VERSION 3.2 FATAL_ERROR)

project(futoin_asyncsteps_bench LANGUAGES CXX)


hunter_add_package(Boost COMPONENTS chrono context fiber system timer)
find_package(Boost CONFIG REQUIRED chrono context fiber system timer)
find_package(Threads REQUIRED)

set(FUTOIN_BENCH_GCCLIKE_COMMON_FLAGS
    -std=c++11
    -Wall
    -Wextra
    -Werror
)


add_executable(BoostFiberBench ${CMAKE_CURRENT_LIST_DIR}/boost_fiber.bench.cpp)
target_link_libraries(BoostFiberBench
    PRIVATE
        Boost::fiber Boost::context Threads::Threads
        Boost::timer Boost::system Boost::chrono Boost::boost)


add_executable(FutoInAsyncStepsBench ${CMAKE_CURRENT_LIST_DIR}/futoin_asyncsteps.bench.cpp)
target_link_libraries(FutoInAsyncStepsBench
    PRIVATE
        futoin::asyncsteps
        Boost::timer Boost::system Boost::chrono Boost::boost)
        
add_executable(FutoInNitroStepsBench ${CMAKE_CURRENT_LIST_DIR}/futoin_nitrosteps.bench.cpp)
target_link_libraries(FutoInNitroStepsBench
    PRIVATE
        futoin::asyncsteps
        Boost::timer Boost::system Boost::chrono Boost::boost)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_COMPILER_IS_CLANG)
    target_compile_options(BoostFiberBench PRIVATE ${FUTOIN_BENCH_GCCLIKE_COMMON_FLAGS})
    target_compile_options(FutoInAsyncStepsBench PRIVATE ${FUTOIN_BENCH_GCCLIKE_COMMON_FLAGS})
    target_compile_options(FutoInNitroStepsBench PRIVATE ${FUTOIN_BENCH_GCCLIKE_COMMON_FLAGS})
endif()
