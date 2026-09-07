// bench_comparison.cpp — MpscQueue vs three strong alternatives, same
// workload, same harness, same metrics.
//
// Alternatives compared:
//
//   1. std::mutex + std::queue        — the obvious "just lock it" baseline.
//   2. Spinlock (atomic_flag) + queue — what people reach for when they've
//                                        heard mutexes are "slow" but don't
//                                        want to think about lock-freedom.
//   3. boost::lockfree::queue         — a general MPMC lock-free queue
//                                        (Michael & Scott style, CAS-based).
//   4. mpsc::MpscQueue (this repo)    — MPSC-specialised, exchange-based.
//
// All four run through the identical sustained-contention harness from
// bench_stress.cpp (fixed wall-clock duration, per-message latency via
// RDTSCP, bounded per-producer ring with backpressure). The only thing
// that varies is which synchronisation primitive moves a Node* from
// producer to consumer. This isolates the cost of the synchronisation
// strategy itself from unrelated differences (allocation, node layout,
// warm-up) that would otherwise confound the comparison.
//
// ── Why these three, and not others ──────────────────────────────────────────
//
// Mutex and spinlock are the two "no special algorithm" baselines everyone
// already has in their toolbox — the comparison that actually matters for
// deciding whether the extra correctness burden of a lock-free queue (see
// proof/memory_model.md) is worth it for a given workload.
//
// boost::lockfree::queue represents the "general MPMC lock-free" family:
// it solves a strictly harder problem (any number of consumers, not just
// one) using CAS loops on both ends. The trade-off this benchmark is
// designed to surface: does MpscQueue's exchange-only approach — which
// only works because it gives up multi-consumer support — actually buy
// back throughput/latency versus a general-purpose lock-free queue, or is
// the specialisation not worth the reduced generality? See the printed
// trade-off discussion at the end of main().

#include "histogram.hpp"
#include "mpsc/queue.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include <boost/lockfree/queue.hpp>

using bench::Backoff;
using bench::LatencyHistogram;
using bench::TscClock;
using Clock = std::chrono::steady_clock;

namespace {

constexpr std::size_t kRingSize = 4096;

struct Node : mpsc::MpscNode {
    uint64_t push_ns = 0;
    std::atomic<bool> in_use{false};
};

// ── Backends ──────────────────────────────────────────────────────────────────
//
// Each backend exposes push(Node*)/pop()->Node* and a kDeferFree flag.
// kDeferFree=true means: the value returned by pop() may still be read
// internally by the backend on a subsequent call (true for MpscQueue,
// where the popped node becomes the new sentinel — see queue.hpp and the
// comment in bench_stress.cpp). Backends that copy the pointer out of
// their own storage on pop (mutex/spinlock/boost queues all do) can have
// their slot freed immediately.

struct MutexBackend {
    static constexpr bool kDeferFree = false;
    static constexpr const char* kName = "std::mutex + std::queue";

    std::mutex mtx;
    std::queue<Node*> q;

    void push(Node* n) {
        std::lock_guard<std::mutex> lk(mtx);
        q.push(n);
    }
    Node* pop() {
        std::lock_guard<std::mutex> lk(mtx);
        if (q.empty()) return nullptr;
        Node* n = q.front();
        q.pop();
        return n;
    }
};

struct SpinlockBackend {
    static constexpr bool kDeferFree = false;
    static constexpr const char* kName = "spinlock + std::queue";

    std::atomic_flag flag = ATOMIC_FLAG_INIT;
    std::queue<Node*> q;

    void lock() { while (flag.test_and_set(std::memory_order_acquire)) __builtin_ia32_pause(); }
    void unlock() { flag.clear(std::memory_order_release); }

    void push(Node* n) { lock(); q.push(n); unlock(); }
    Node* pop() {
        lock();
        Node* n = q.empty() ? nullptr : q.front();
        if (n) q.pop();
        unlock();
        return n;
    }
};

struct BoostLockfreeBackend {
    static constexpr bool kDeferFree = false;
    static constexpr const char* kName = "boost::lockfree::queue (MPMC, CAS-based)";

    // Fixed capacity, sized generously relative to the per-producer rings
    // below so the boost queue is never the bottleneck by construction.
    boost::lockfree::queue<Node*> q{1 << 16};

    void push(Node* n) {
        while (!q.push(n)) __builtin_ia32_pause();   // capacity-bounded: retry on full
    }
    Node* pop() {
        Node* n = nullptr;
        return q.pop(n) ? n : nullptr;
    }
};

struct MpscBackend {
    static constexpr bool kDeferFree = true;
    static constexpr const char* kName = "mpsc::MpscQueue (this repo)";

    mpsc::MpscQueue<Node> q;

    void push(Node* n) { q.push(n); }
    Node* pop() { return q.pop(); }
};

// ── Generic sustained-contention harness (shared with bench_stress.cpp) ──────

struct RunResult {
    double msgs_per_sec = 0.0;
    LatencyHistogram hist;
};

template <typename Backend>
RunResult run_sustained(Backend& backend, unsigned n_producers,
                         std::chrono::seconds duration, const TscClock& tsc) {
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
                bool aborted = false;
                while (n->in_use.load(std::memory_order_acquire)) {
                    if (stop.load(std::memory_order_relaxed)) { aborted = true; break; }
                    backoff.wait();
                }
                if (aborted) break;
                backoff.reset();
                n->in_use.store(true, std::memory_order_relaxed);
                n->push_ns = tsc.now_ns();
                backend.push(n);
                ++pushed;
                slot = (slot + 1) % kRingSize;
                if ((pushed & 0xFFF) == 0 && stop.load(std::memory_order_relaxed)) break;
            }
            total_pushed.fetch_add(pushed, std::memory_order_relaxed);
        });
    }

    std::thread timer([&] {
        while (!start.load(std::memory_order_acquire)) __builtin_ia32_pause();
        std::this_thread::sleep_for(duration);
        stop.store(true, std::memory_order_release);
    });

    LatencyHistogram hist;
    Node* pending_free = nullptr;   // only meaningful when Backend::kDeferFree

    auto record_and_free = [&](Node* n) {
        const uint64_t now = tsc.now_ns();
        hist.record(now - n->push_ns);
        if constexpr (Backend::kDeferFree) {
            if (pending_free) pending_free->in_use.store(false, std::memory_order_release);
            pending_free = n;
        } else {
            n->in_use.store(false, std::memory_order_release);
        }
    };

    const auto t_start = Clock::now();
    start.store(true, std::memory_order_release);

    Backoff drain_backoff;
    for (;;) {
        Node* n = backend.pop();
        if (n) { record_and_free(n); drain_backoff.reset(); continue; }
        if (stop.load(std::memory_order_acquire)) {
            bool got_more = false;
            for (int i = 0; i < 1000; ++i) {
                Node* n2 = backend.pop();
                if (n2) { record_and_free(n2); got_more = true; }
            }
            if (!got_more) break;
        }
        drain_backoff.wait();
    }
    if constexpr (Backend::kDeferFree)
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

template <typename Backend>
void run_and_report(unsigned producers, std::chrono::seconds duration, const TscClock& tsc) {
    Backend backend;
    RunResult r = run_sustained(backend, producers, duration, tsc);
    char label[96];
    std::snprintf(label, sizeof(label), "  %-42s P=%u", Backend::kName, producers);
    std::printf("%-46s throughput=%7.2f M msg/s\n", label, r.msgs_per_sec / 1e6);
    r.hist.print("    latency          ");
}

} // namespace

int main(int argc, char** argv) {
    const int duration_s = (argc > 1) ? std::atoi(argv[1]) : 2;

    TscClock tsc;
    tsc.calibrate();

    std::printf("=== Queue comparison: same workload, four synchronisation strategies ===\n");
    std::printf("Duration per (backend, producer count): %ds\n", duration_s);
    std::printf("TSC: %.4f ns/cycle\n\n", tsc.ns_per_cycle);
    std::printf("Run pinned for stable numbers, e.g.:\n");
    std::printf("  taskset -c 4-7 chrt -f 80 ./bench_comparison %d\n\n", duration_s);

    for (unsigned p : {1u, 4u, 8u}) {
        std::printf("── %u producer%s ──────────────────────────────────────────\n",
                    p, p == 1 ? "" : "s");
        run_and_report<MutexBackend>(p, std::chrono::seconds(duration_s), tsc);
        run_and_report<SpinlockBackend>(p, std::chrono::seconds(duration_s), tsc);
        run_and_report<BoostLockfreeBackend>(p, std::chrono::seconds(duration_s), tsc);
        run_and_report<MpscBackend>(p, std::chrono::seconds(duration_s), tsc);
        std::printf("\n");
    }

    std::printf(
        "── Trade-off discussion ──────────────────────────────────────────────\n"
        "mutex:      simplest to reason about, no memory-ordering proof needed.\n"
        "            Cost: kernel-mediated lock under contention; every producer\n"
        "            serialises fully (push AND its queue-internal bookkeeping).\n"
        "spinlock:   avoids the kernel round-trip, but still serialises the\n"
        "            *entire* critical section per producer, and burns CPU on\n"
        "            every waiting thread instead of blocking — worse than a\n"
        "            mutex on an oversubscribed or shared host.\n"
        "boost::lockfree::queue: lock-free and general (MPMC): correct with any\n"
        "            number of consumers. That generality costs a CAS retry\n"
        "            loop on the enqueue path (vs. this repo's single\n"
        "            unconditional exchange) and extra indirection for its\n"
        "            internal free-list. The right choice if you actually need\n"
        "            multiple consumers.\n"
        "MpscQueue:  exploits the single-consumer restriction: producers only\n"
        "            ever contend on one exchange (no CAS retry loop), and the\n"
        "            consumer never contends with anything. The trade-off is\n"
        "            explicit in the name — this is not a drop-in replacement\n"
        "            for boost::lockfree::queue if you need >1 consumer.\n"
        "\n"
        "Read the throughput numbers above with the producers-per-core ratio in\n"
        "mind: on a shared/oversubscribed host, ALL FOUR backends are dominated\n"
        "by scheduler latency, not by the synchronisation primitive under test.\n"
        "The comparison is only meaningful pinned on dedicated cores — see\n"
        "BENCHMARK_RESULTS.md.\n");
    return 0;
}
