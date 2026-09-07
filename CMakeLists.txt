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

add_executable(test_batch tests/test_batch.cpp)
target_link_libraries(test_batch PRIVATE mpsc_iface)
add_test(NAME BatchCorrectness COMMAND test_batch)

# ── Benchmarks ────────────────────────────────────────────────────────────────
# None of these are registered with ctest — they're manual, and several
# take multiple seconds to minutes by design (sustained-load measurement).
# See BENCHMARK_RESULTS.md for what each one measures and how to reproduce
# results on dedicated hardware.

add_executable(bench_mpsc bench/bench_mpsc.cpp)
target_link_libraries(bench_mpsc PRIVATE mpsc_iface)

add_executable(bench_batch bench/bench_batch.cpp)
target_link_libraries(bench_batch PRIVATE mpsc_iface)

add_executable(bench_stress bench/bench_stress.cpp)
target_link_libraries(bench_stress PRIVATE mpsc_iface)
# Sustained multi-producer contention + latency histograms.
# Run: ./bench_stress [duration_seconds]

# bench_t2t_standalone: no external deps, measures
# ITCH decode → LOB update → A-S quote decision latency, single thread.
add_executable(bench_t2t bench/bench_tick_to_trade_standalone.cpp)
target_link_libraries(bench_t2t PRIVATE mpsc_iface)

add_executable(bench_t2t_queue bench/bench_tick_to_trade_queue.cpp)
target_link_libraries(bench_t2t_queue PRIVATE mpsc_iface)
# Same decode/book/quote logic as bench_t2t, split across a decode thread
# and a strategy thread connected by MpscQueue — measures the queue's
# actual contribution to an end-to-end pipeline latency number.
# Run: ./bench_t2t_queue [num_events]

# bench_comparison needs Boost headers (header-only boost::lockfree) for
# one of its four backends. It's optional: the rest of the build/tests do
# not depend on Boost, so this target is skipped with a warning if Boost
# isn't found rather than failing the whole configure step.
find_package(Boost QUIET)
if(Boost_FOUND)
    add_executable(bench_comparison bench/bench_comparison.cpp)
    target_link_libraries(bench_comparison PRIVATE mpsc_iface Boost::boost)
else()
    message(WARNING
        "Boost not found — skipping bench_comparison "
        "(MpscQueue vs mutex/spinlock/boost::lockfree::queue). "
        "Install libboost-dev (or any package providing boost/lockfree/queue.hpp) "
        "and re-run cmake to enable it.")
endif()

