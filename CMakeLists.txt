cmake_minimum_required(VERSION 3.22)
project(mpsc_queue VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -mtune=native -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -march=native -g -DNDEBUG")

add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)

find_package(Threads REQUIRED)

option(TSAN "Enable ThreadSanitizer" OFF)
if(TSAN)
    add_compile_options(-fsanitize=thread -g -O1)
    add_link_options(-fsanitize=thread)
endif()

option(ASAN "Enable AddressSanitizer" OFF)
if(ASAN)
    add_compile_options(-fsanitize=address -g -O1)
    add_link_options(-fsanitize=address)
endif()

# ── Header-only library ───────────────────────────────────────────────────────

add_library(mpsc_iface INTERFACE)
target_include_directories(mpsc_iface INTERFACE include)
target_link_libraries(mpsc_iface INTERFACE Threads::Threads)

# ── Tests ─────────────────────────────────────────────────────────────────────

enable_testing()

add_executable(test_correctness tests/test_correctness.cpp)
target_link_libraries(test_correctness PRIVATE mpsc_iface)
add_test(NAME Correctness COMMAND test_correctness)

# TSan test: runs correctly under both normal and -fsanitize=thread builds.
# Under the TSAN cmake option, the executable is instrumented.
add_executable(test_tsan tests/test_tsan.cpp)
target_link_libraries(test_tsan PRIVATE mpsc_iface)
add_test(NAME TSanLitmus COMMAND test_tsan)

# ── Benchmark ─────────────────────────────────────────────────────────────────

add_executable(bench_mpsc bench/bench_mpsc.cpp)
target_link_libraries(bench_mpsc PRIVATE mpsc_iface)
# Not added to ctest — run manually.

add_executable(bench_batch bench/bench_batch.cpp)
target_link_libraries(bench_batch PRIVATE mpsc_iface)
# Not in ctest — run manually: ./bench_batch

add_executable(test_batch tests/test_batch.cpp)
target_link_libraries(test_batch PRIVATE mpsc_iface)
add_test(NAME BatchCorrectness COMMAND test_batch)

# bench_tick_to_trade_standalone: no external deps, measures
# ITCH decode → LOB update → A-S quote decision latency.
add_executable(bench_t2t bench/bench_tick_to_trade_standalone.cpp)
target_link_libraries(bench_t2t PRIVATE mpsc_iface)
# Not in ctest — run manually: taskset -c 4 chrt -f 80 ./bench_t2t
