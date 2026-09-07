// bench_stress.cpp — sustained multi-producer contention, not just peak.
//
// bench_mpsc.cpp and bench_batch.cpp both measure *peak* throughput: fire
// a fixed batch of pre-allocated messages as fast as possible and time the
// whole run. That is the right way to measure headline msgs/sec, but it
// says nothing about what a caller experiences while the queue is under
// continuous load for seconds at a time — which is when scheduler jitter,
// cache eviction, and memory-bandwidth contention actually show up in the
// tail.
//
// This benchmark instead:
//   • Runs each configuration for a fixed WALL-CLOCK duration (default 3s),
//     not a fixed message count — throughput is measured in steady state,
//     not as a one-shot burst.
//   • Records push-to-pop latency for EVERY message via a per-slot
//     "in_use" handshake (see Node below), not just a ping-pong subset,
//     so the histogram reflects real concurrent contention, not an
//     artificially serialised access pattern.
//   • Reports the full latency distribution (p50/p90/p99/p99.9/max) per
//     producer count, so tail behaviour under sustained load is visible,
//     not just the mean implied by aggregate throughput.
//
// Methodology notes (see BENCHMARK_RESULTS.md for full detail and how to
// reproduce with core isolation):
//   • Each producer owns a fixed-size ring of RING_SIZE nodes and must wait
//     for the consumer to release a slot before reusing it. This bounds
//     memory (no unbounded allocation over a multi-second run) and mirrors
//     how a real producer with a fixed message pool experiences
//     backpressure when the consumer falls behind — this is deliberately
//     NOT the same as an unbounded queue that hides consumer lag from the
//     producer.
//   • RDTSCP timestamps are taken on both sides; latency is push-timestamp
//     to pop-timestamp, i.e. it includes time spent sitting in the queue,
//     not just the two calls' own overhead.

#include "histogram.hpp"
#include "mpsc/queue.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

using namespace mpsc;
using bench::Backoff;
using bench::LatencyHistogram;
using bench::TscClock;
using Clock = std::chrono::steady_clock;

namespace {

constexpr std::size_t kRingSize = 4096;   // per-producer in-flight slot count

struct Node : MpscNode {
    uint64_t push_ns = 0;
    // Set by the producer before push(); cleared by the consumer once the
    // node has been popped and its latency recorded. The producer spins on
    // this before reusing the slot — this is the backpressure mechanism
    // described above, not part of the queue's own contract.
    std::atomic<bool> in_use{false};
};

struct RunResult {
    double msgs_per_sec = 0.0;
    LatencyHistogram hist;
};

RunResult run_sustained(unsigned n_producers, std::chrono::seconds duration,
                         const TscClock& tsc) {
    MpscQueue<Node> q;

    std::vector<std::unique_ptr<Node[]>> rings(n_producers);
    for (unsigned p = 0; p < n_producers; ++p)
        rings[p] = std::make_unique<Node[]>(kRingSize);

    std::atomic<bool> start{false};
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> total_pushed{0};

    std::vector<std::thread> producers;
    producers.reserve(n_producers);
    for (unsigned p = 0; p < n_producers; ++p) {
        producers.emplace_back([&, p] {
            Node* ring = rings[p].get();
            while (!start.load(std::memory_order_acquire)) __builtin_ia32_pause();

            uint64_t pushed = 0;
            std::size_t slot = 0;
            Backoff backoff;
            while (!stop.load(std::memory_order_relaxed)) {
                Node* n = &ring[slot];
                // Wait for the consumer to free this slot (backpressure).
                // Also bail out on `stop` — otherwise, if the consumer
                // stops draining before this slot is freed (e.g. run end),
                // this loop would spin forever and the final join() would
                // hang. We haven't called push() yet, so aborting here is
                // clean: no partially-published node.
                bool aborted = false;
                while (n->in_use.load(std::memory_order_acquire)) {
                    if (stop.load(std::memory_order_relaxed)) { aborted = true; break; }
                    backoff.wait();
                }
                if (aborted) break;
                backoff.reset();
                n->in_use.store(true, std::memory_order_relaxed);
                n->push_ns = tsc.now_ns();
                q.push(n);
                ++pushed;
                slot = (slot + 1) % kRingSize;
                // Amortise the stop-flag / clock check; checking every
                // iteration would itself perturb the latency being measured.
                if ((pushed & 0xFFF) == 0 && stop.load(std::memory_order_relaxed))
                    break;
            }
            total_pushed.fetch_add(pushed, std::memory_order_relaxed);
        });
    }

    // Consumer: drain continuously, record latency, free the slot.
    LatencyHistogram hist;
    std::atomic<int> producers_running{int(n_producers)};

    std::thread timer([&] {
        while (!start.load(std::memory_order_acquire)) __builtin_ia32_pause();
        std::this_thread::sleep_for(duration);
        stop.store(true, std::memory_order_release);
    });

    const auto t_start = Clock::now();
    start.store(true, std::memory_order_release);

    // Consumer loop: keep popping until every producer has observed `stop`
    // and the queue is empty. We can't distinguish "empty" from
    // "incomplete push" (by design — see queue.hpp), so on a null pop we
    // just retry; that's the documented contract.
    //
    // Freeing a slot: a node returned by pop() becomes the queue's new
    // sentinel (head_) internally. Its `next` field will be read by the
    // *following* pop() to find the node after it. If we let the producer
    // recycle a node (reset its `next` and re-publish it) while it is
    // still serving as that sentinel, we corrupt the internal list. So we
    // defer marking a node free until one pop *later* — by then head_ has
    // already moved past it and nothing will read its `next` again.
    Node* pending_free = nullptr;
    Backoff drain_backoff;
    for (;;) {
        Node* n = q.pop();
        if (n) {
            const uint64_t now = tsc.now_ns();
            hist.record(now - n->push_ns);
            if (pending_free) pending_free->in_use.store(false, std::memory_order_release);
            pending_free = n;
            drain_backoff.reset();
            continue;
        }
        if (stop.load(std::memory_order_acquire)) {
            // One more drain pass in case a push landed just as we saw stop.
            bool got_more = false;
            for (int i = 0; i < 1000; ++i) {
                Node* n2 = q.pop();
                if (n2) {
                    const uint64_t now = tsc.now_ns();
                    hist.record(now - n2->push_ns);
                    if (pending_free) pending_free->in_use.store(false, std::memory_order_release);
                    pending_free = n2;
                    got_more = true;
                }
            }
            if (!got_more) break;
        }
        drain_backoff.wait();
    }
    // Safe now: no further pop() will occur, so the last node can be freed.
    if (pending_free) pending_free->in_use.store(false, std::memory_order_release);
    const auto t_end = Clock::now();

    for (auto& t : producers) t.join();
    timer.join();

    const double elapsed_sec =
        std::chrono::duration_cast<std::chrono::duration<double>>(t_end - t_start).count();

    RunResult r;
    r.msgs_per_sec = double(total_pushed.load()) / elapsed_sec;
    r.hist = std::move(hist);
    return r;
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const int duration_s = (argc > 1) ? std::atoi(argv[1]) : 3;

    TscClock tsc;
    tsc.calibrate();

    std::printf("=== Sustained multi-producer contention ===\n");
    std::printf("Duration per configuration: %ds (steady-state, not a burst)\n", duration_s);
    std::printf("TSC: %.4f ns/cycle\n\n", tsc.ns_per_cycle);
    std::printf("Run pinned for stable numbers, e.g.:\n");
    std::printf("  taskset -c 4-7 chrt -f 80 ./bench_stress %d\n\n", duration_s);

    for (unsigned p : {1u, 2u, 4u, 8u}) {
        RunResult r = run_sustained(p, std::chrono::seconds(duration_s), tsc);
        std::printf("producers=%u  throughput=%.2f M msg/s\n", p, r.msgs_per_sec / 1e6);
        char label[64];
        std::snprintf(label, sizeof(label), "  latency (P=%u)", p);
        r.hist.print(label);
        std::printf("\n");
    }

    std::printf("Note: latency here includes real concurrent contention on tail_\n"
                "(unlike the ping-pong latency test in bench_mpsc.cpp, which\n"
                "serialises producer and consumer to isolate round-trip cost).\n"
                "Expect p50 to rise and the tail to fatten as producer count grows,\n"
                "since more producers contend for the same tail_ cache line.\n");
    return 0;
}
