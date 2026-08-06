// bench_batch.cpp — single push vs batch push under producer contention.
//
// Hypothesis: at P producers, single push serialises every node on one
// LOCK XCHG against the shared tail_ cache line.  push_batch(K) amortises
// that to one contended exchange per K nodes, so throughput should approach
// K× the single-push figure until memory bandwidth or the consumer becomes
// the bottleneck.
//
// Methodology follows bench_mpsc.cpp: wall-clock throughput over the full
// producer+consumer run (coordinated-omission-safe), fixed pre-allocated
// node storage (zero allocation during timing), one warm-up round before
// recording.  Run pinned on isolated cores for stable numbers:
//
//   taskset -c 4-7 chrt -f 80 ./bench_batch
//
// Reported per configuration: msgs/sec and the implied amortised cost of
// the contended exchange per node.

#include "mpsc/queue.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

using namespace mpsc;
using Clock = std::chrono::steady_clock;

struct BenchNode : MpscNode {
    uint64_t payload = 0;
};

// One run: P producers push MSGS_PER_PRODUCER nodes each, batched K at a
// time (K=1 uses plain push).  Consumer drains until all arrive.
// Returns msgs/sec.
static double run_once(unsigned P, unsigned K, uint64_t msgs_per_producer) {
    const uint64_t total = uint64_t(P) * msgs_per_producer;

    MpscQueue<BenchNode> q;
    std::vector<BenchNode> storage(total);
    std::atomic<bool> go{false};
    std::atomic<uint64_t> consumed{0};

    std::thread consumer([&] {
        uint64_t got = 0;
        while (got < total) {
            BenchNode* n = q.pop();
            if (n == nullptr) continue;
            ++got;
        }
        consumed.store(got, std::memory_order_release);
    });

    std::vector<std::thread> producers;
    producers.reserve(P);
    for (unsigned p = 0; p < P; ++p) {
        producers.emplace_back([&, p] {
            BenchNode* base = storage.data() + uint64_t(p) * msgs_per_producer;
            while (!go.load(std::memory_order_acquire)) { /* spin */ }
            if (K <= 1) {
                for (uint64_t i = 0; i < msgs_per_producer; ++i)
                    q.push(base + i);
            } else {
                std::vector<BenchNode*> ptrs(K);
                uint64_t i = 0;
                while (i < msgs_per_producer) {
                    const uint64_t k = std::min<uint64_t>(K, msgs_per_producer - i);
                    for (uint64_t j = 0; j < k; ++j) ptrs[j] = base + i + j;
                    q.push_batch(ptrs.data(), k);
                    i += k;
                }
            }
        });
    }

    const auto t0 = Clock::now();
    go.store(true, std::memory_order_release);
    for (auto& t : producers) t.join();
    consumer.join();
    const auto t1 = Clock::now();

    const double sec =
        std::chrono::duration_cast<std::chrono::duration<double>>(t1 - t0).count();
    return double(total) / sec;
}

int main() {
    constexpr uint64_t MSGS = 1'000'000;   // per producer
    const unsigned producer_counts[] = {1, 2, 4, 8};
    const unsigned batch_sizes[]     = {1, 8, 32, 128};

    std::printf("=== Batch push vs single push (msgs/sec) ===\n");
    std::printf("%u msgs per producer, fixed pre-allocated storage\n\n", unsigned(MSGS));
    std::printf("%-10s", "producers");
    for (unsigned K : batch_sizes) std::printf("  K=%-9u", K);
    std::printf("\n");

    for (unsigned P : producer_counts) {
        // Warm-up round (untimed): touch pages, warm caches and branch predictors.
        (void)run_once(P, 1, MSGS / 10);

        std::printf("%-10u", P);
        for (unsigned K : batch_sizes) {
            const double rate = run_once(P, K, MSGS);
            std::printf("  %7.2f M ", rate / 1e6);
            std::fflush(stdout);
        }
        std::printf("\n");
    }

    std::printf("\nExpect: K=1 matches bench_mpsc; throughput scales with K under\n"
                "contention (P>=4) until the single consumer becomes the bottleneck.\n");
    return 0;
}
