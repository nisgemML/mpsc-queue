// bench_mpsc.cpp — MPSC queue benchmark.
//
// Measures two things:
//
//   1. Throughput: messages/second sustained across N producers.
//      Compare: lock-free MPSC vs mutex-protected std::queue.
//
//   2. Single-producer latency: push-to-pop round trip time in nanoseconds.
//      Measured with RDTSCP to avoid the ~20ns overhead of clock_gettime.
//
// ── Why the mutex baseline matters ────────────────────────────────────────────
//
// "Lock-free is faster" is not always true.  A mutex costs ~20-40ns
// uncontended on modern hardware.  The MPSC queue's advantage comes from:
//   a) No lock acquisition on the producer side (wait-free push)
//   b) No false sharing between producers
//   c) Better cache behaviour under high concurrency
//
// At low producer counts (1-2) the difference is small.  Under contention
// (8+ producers) the lock-free advantage is significant because:
//   • Mutex: producers serialise on the lock; one thread holds it at a time.
//   • MPSC: producers only serialise on tail_.exchange (~4 cycles on x86).
//
// ── Benchmark methodology ─────────────────────────────────────────────────────
//
// Coordinated omission note: we measure wall-clock throughput (total messages
// / elapsed time) rather than per-message latency from the producer's side.
// This avoids the classic mistake of measuring how fast you can submit,
// which understates latency when the queue is backpressured.

#include "mpsc/queue.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>
#include <algorithm>
#include <x86intrin.h>

using namespace mpsc;
using Clock = std::chrono::steady_clock;

// ── TSC timer ─────────────────────────────────────────────────────────────────

static double g_ns_per_cycle = 1.0;

static void calibrate_tsc() {
    using namespace std::chrono;
    const auto t0 = Clock::now();
    const uint64_t c0 = __rdtsc();
    std::this_thread::sleep_for(milliseconds(20));
    const uint64_t c1 = __rdtsc();
    const auto t1 = Clock::now();
    const double elapsed_ns = duration_cast<nanoseconds>(t1 - t0).count();
    g_ns_per_cycle = elapsed_ns / double(c1 - c0);
}

static inline uint64_t rdtscp_ns() {
    unsigned aux;
    return uint64_t(double(__rdtscp(&aux)) * g_ns_per_cycle);
}

// ── MPSC throughput benchmark ─────────────────────────────────────────────────

struct BenchNode : MpscNode {
    uint64_t ts_push_ns = 0;
};

static void bench_mpsc_throughput(int n_producers, int n_per_producer) {
    const int N_TOTAL = n_producers * n_per_producer;

    // Allocate all nodes upfront (no per-push allocation).
    std::vector<std::unique_ptr<BenchNode[]>> nodes(n_producers);
    for (int t = 0; t < n_producers; ++t)
        nodes[t] = std::make_unique<BenchNode[]>(n_per_producer);

    MpscQueue<BenchNode> q;
    std::atomic<int> producers_ready{0};
    std::atomic<bool> start{false};

    std::vector<std::thread> threads;
    threads.reserve(n_producers);

    for (int t = 0; t < n_producers; ++t) {
        threads.emplace_back([&, t] {
            producers_ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) __builtin_ia32_pause();
            for (int i = 0; i < n_per_producer; ++i)
                q.push(&nodes[t][i]);
        });
    }

    while (producers_ready.load(std::memory_order_acquire) < n_producers)
        __builtin_ia32_pause();

    const auto t0 = Clock::now();
    start.store(true, std::memory_order_release);

    int consumed = 0;
    while (consumed < N_TOTAL) {
        BenchNode* p = q.pop();
        if (!p) { __builtin_ia32_pause(); continue; }
        ++consumed;
    }
    const auto t1 = Clock::now();

    for (auto& th : threads) th.join();

    const double elapsed_ms =
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() / 1000.0;
    const double msgs_sec = N_TOTAL / (elapsed_ms / 1000.0);

    std::printf("  MPSC  %2d producers  %8d msgs  %6.1f ms  %8.2f M msg/s\n",
                n_producers, N_TOTAL, elapsed_ms, msgs_sec / 1e6);
}

// ── Mutex-queue throughput benchmark (baseline) ───────────────────────────────

static void bench_mutex_throughput(int n_producers, int n_per_producer) {
    const int N_TOTAL = n_producers * n_per_producer;

    struct Item { int value; };
    std::mutex mtx;
    std::queue<Item> q;
    std::atomic<int> producers_ready{0};
    std::atomic<bool> start{false};
    std::atomic<int>  pushed{0};

    std::vector<std::thread> threads;
    threads.reserve(n_producers);

    for (int t = 0; t < n_producers; ++t) {
        threads.emplace_back([&, t] {
            producers_ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) __builtin_ia32_pause();
            for (int i = 0; i < n_per_producer; ++i) {
                std::lock_guard<std::mutex> lk(mtx);
                q.push({t * n_per_producer + i});
            }
        });
    }

    while (producers_ready.load(std::memory_order_acquire) < n_producers)
        __builtin_ia32_pause();

    const auto t0 = Clock::now();
    start.store(true, std::memory_order_release);

    int consumed = 0;
    while (consumed < N_TOTAL) {
        std::unique_lock<std::mutex> lk(mtx);
        if (q.empty()) {
            lk.unlock();
            __builtin_ia32_pause();  // release lock so producers can push
            continue;
        }
        q.pop();
        ++consumed;
    }
    const auto t1 = Clock::now();

    for (auto& th : threads) th.join();

    const double elapsed_ms =
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() / 1000.0;
    const double msgs_sec = N_TOTAL / (elapsed_ms / 1000.0);

    std::printf("  Mutex %2d producers  %8d msgs  %6.1f ms  %8.2f M msg/s\n",
                n_producers, N_TOTAL, elapsed_ms, msgs_sec / 1e6);
}

// ── Single-producer push-to-pop latency ──────────────────────────────────────
//
// Measures true cross-thread push-to-pop latency: the time from when the
// producer calls push() to when the consumer's pop() returns the same node.
//
// Methodology:
//   • Producer timestamps with RDTSCP, pushes the node, then blocks on a
//     semaphore (atomic spin) waiting for the consumer to acknowledge.
//   • Consumer pops, timestamps with RDTSCP, records the delta, then signals
//     the producer to push the next node.
//   • One outstanding node at a time: this is a ping-pong pattern, not a
//     sustained throughput measurement.  It isolates round-trip latency.
//
// Why not have the producer call pop()?
//   If the producer calls pop() after push(), it races with the consumer and
//   almost always wins (same core scheduling).  The "latency" measured would
//   be push()+pop() overhead in the same thread — typically ~20ns — with no
//   cross-core communication.  True cross-thread latency on a shared-memory
//   system involves a cache-line transfer, which costs ~50–300ns depending on
//   the CPU's cache coherence protocol and core topology.
//
// The ping-pong pattern does serialize producer and consumer (throughput = 0.5×
// the sustained rate), but it is the correct way to measure latency in isolation.

static void bench_latency() {
    struct LatNode : MpscNode {
        uint64_t push_cycles = 0;
    };

    constexpr int N      = 200'000;
    constexpr int WARMUP = 20'000;
    auto nodes = std::make_unique<LatNode[]>(N + WARMUP);

    MpscQueue<LatNode> q;

    // Ping-pong synchronisation: producer waits for consumer to signal
    // after each pop before pushing the next node.
    // 0 = consumer ready for next push; 1 = producer has pushed, consumer should pop.
    std::atomic<int> ping_pong{0};
    std::atomic<bool> producer_done{false};

    std::vector<uint64_t> latencies;
    latencies.reserve(N);

    std::thread producer([&] {
        for (int i = 0; i < WARMUP + N; ++i) {
            // Wait for consumer to be ready (ping_pong == 0).
            while (ping_pong.load(std::memory_order_acquire) != 0)
                __builtin_ia32_pause();

            nodes[i].next.store(nullptr, std::memory_order_relaxed);
            unsigned aux;
            nodes[i].push_cycles = __rdtscp(&aux);    // timestamp before push
            q.push(&nodes[i]);

            // Signal consumer: a node is waiting.
            ping_pong.store(1, std::memory_order_release);
        }
        producer_done.store(true, std::memory_order_release);
    });

    // Consumer: pop each node, record latency, signal producer.
    int consumed = 0;
    while (consumed < WARMUP + N) {
        // Wait for producer to push (ping_pong == 1).
        while (ping_pong.load(std::memory_order_acquire) != 1)
            __builtin_ia32_pause();

        LatNode* p = nullptr;
        while (!(p = q.pop())) __builtin_ia32_pause();

        unsigned aux;
        const uint64_t pop_cycles = __rdtscp(&aux);    // timestamp after pop
        const uint64_t lat_ns = uint64_t(
            double(pop_cycles - p->push_cycles) * g_ns_per_cycle);

        if (consumed >= WARMUP)
            latencies.push_back(lat_ns);

        ++consumed;
        ping_pong.store(0, std::memory_order_release);  // signal producer
    }

    producer.join();

    // Percentiles.
    std::sort(latencies.begin(), latencies.end());
    const size_t n = latencies.size();
    if (n == 0) return;

    std::printf("\n=== Push-to-pop latency (ping-pong, cross-thread) ===\n");
    std::printf("  Samples : %zu\n", n);
    std::printf("  p50     : %lu ns\n", latencies[n * 50  / 100]);
    std::printf("  p90     : %lu ns\n", latencies[n * 90  / 100]);
    std::printf("  p99     : %lu ns\n", latencies[n * 99  / 100]);
    std::printf("  p99.9   : %lu ns\n", latencies[n * 999 / 1000]);
    std::printf("  max     : %lu ns\n", latencies.back());
    std::printf("\n");
    std::printf("Note: ping-pong serialises producer+consumer; measures round-trip latency,\n");
    std::printf("not throughput.  Typical cross-core p50 = 100-300 ns depending on CPU topology.\n");
    std::printf("For sub-100 ns: pin threads to adjacent cores sharing L2 cache (taskset -c 0,1).\n");
}

// ── main ──────────────────────────────────────────────────────────────────────

int main() {
    std::printf("=== MPSC Queue Benchmark ===\n\n");
    calibrate_tsc();
    std::printf("TSC calibration: %.3f ns/cycle\n\n", g_ns_per_cycle);

    std::printf("=== Throughput: MPSC vs Mutex ===\n");
    std::printf("  (larger is better)\n\n");

    constexpr int N_PER_PRODUCER = 200'000;

    for (int n : {1, 2, 4, 8}) {
        bench_mpsc_throughput(n, N_PER_PRODUCER);
        bench_mutex_throughput(n, N_PER_PRODUCER);
        std::printf("\n");
    }

    bench_latency();
    return 0;
}
