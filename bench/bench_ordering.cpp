// bench/bench_ordering.cpp — what does seq_cst actually cost here?
//
// The header and proof argue acq_rel is sufficient and seq_cst unnecessary.
// This measures the price of seq_cst instead of asserting one. `Q<Ord>` is
// the exact MpscQueue algorithm with every atomic op's ordering taken from
// a policy, so the two variants differ ONLY in ordering. "SeqCst" is every
// op seq_cst — what you get by omitting memory_order arguments. Codegen on
// x86-64 (g++ 13 -O2, push() disassembled in isolation):
//   acq_rel push: MOV (init next) ; XCHG (tail_) ; MOV (link)   -> 1 locked op
//   seq_cst push: XCHG (init)     ; XCHG (tail_) ; XCHG (link)  -> 3 locked ops
// Loads are plain MOV in both, so pop() should cost the same.
//
// Modes:
//   single   one thread: push a batch, pop it, repeat — no contention, so
//            the difference is the instruction cost itself
//   mp <P>   P producer threads + 1 consumer, end-to-end throughput — only
//            meaningful with >= P+1 real cores (run scripts/run_pinned_bench.sh)
//
// Usage: bench_ordering [single|mp P] [runs]

#include "mpsc/queue.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

struct AcqRel { static constexpr auto xchg = std::memory_order_acq_rel, st = std::memory_order_release,
                                       ld = std::memory_order_acquire, init = std::memory_order_relaxed;
                static constexpr const char* name = "acq_rel"; };
struct SeqCst { static constexpr auto xchg = std::memory_order_seq_cst, st = std::memory_order_seq_cst,
                                       ld = std::memory_order_seq_cst, init = std::memory_order_seq_cst;
                static constexpr const char* name = "seq_cst"; };

struct Node : mpsc::MpscNode { uint64_t v; };

template <class O>
class Q {
public:
    Q() : head_(&stub_), tail_(&stub_) {}
    void push(Node* n) noexcept {
        n->next.store(nullptr, O::init);
        mpsc::MpscNode* prev = tail_.exchange(n, O::xchg);
        prev->next.store(n, O::st);
    }
    Node* pop() noexcept {
        mpsc::MpscNode* h = head_.load(std::memory_order_relaxed);
        mpsc::MpscNode* nx = h->next.load(O::ld);
        if (!nx) return nullptr;
        head_.store(nx, std::memory_order_relaxed);
        return static_cast<Node*>(nx);
    }
private:
    mpsc::MpscNode stub_;
    std::atomic<mpsc::MpscNode*> head_;
    alignas(64) std::atomic<mpsc::MpscNode*> tail_;
};

using clk = std::chrono::steady_clock;
static double ns_since(clk::time_point t) { return std::chrono::duration<double, std::nano>(clk::now() - t).count(); }

// Uncontended: ns per push and per pop, batch of B through one thread.
template <class O>
static void single(std::vector<double>& push_ns, std::vector<double>& pop_ns, int runs) {
    constexpr int B = 4096, ROUNDS = 2000;
    auto nodes = std::make_unique<Node[]>(B + 1);
    for (int r = 0; r < runs + 1; ++r) {                    // run 0 = warmup
        Q<O> q;
        double tp = 0, tq = 0; uint64_t sink = 0;
        for (int k = 0; k < ROUNDS; ++k) {
            auto t0 = clk::now();
            for (int i = 0; i < B; ++i) { nodes[i].v = i; q.push(&nodes[i]); }
            tp += ns_since(t0);
            auto t1 = clk::now();
            for (int i = 0; i < B; ++i) { Node* n = q.pop(); sink += n->v; }
            tq += ns_since(t1);
            // queue now holds nodes[B-1] as sentinel: push/pop one spare to release it
            q.push(&nodes[B]); (void)q.pop();
        }
        if (sink == 42) std::puts("");
        if (r) { push_ns.push_back(tp / (double(B) * ROUNDS)); pop_ns.push_back(tq / (double(B) * ROUNDS)); }
    }
}

// Contended: P producers each push M nodes; one consumer drains. Msgs/sec.
template <class O>
static double mp(int P) {
    constexpr int M = 2'000'000;
    Q<O> q;
    auto nodes = std::make_unique<Node[]>(size_t(P) * M);
    std::atomic<int> go{0};
    std::vector<std::thread> ts;
    for (int p = 0; p < P; ++p)
        ts.emplace_back([&, p] { while (!go.load(std::memory_order_acquire)) {}
            for (int i = 0; i < M; ++i) { Node* n = &nodes[size_t(p) * M + i]; n->v = i; q.push(n); } });
    const long total = long(P) * M; long got = 0;
    auto t0 = clk::now();
    go.store(1, std::memory_order_release);
    while (got < total) { if (q.pop()) ++got; }
    const double s = ns_since(t0) / 1e9;
    for (auto& t : ts) t.join();
    return total / s;
}

static double median(std::vector<double> v) { std::sort(v.begin(), v.end()); return v[v.size() / 2]; }

int main(int argc, char** argv) {
    const bool is_mp = argc > 1 && std::strcmp(argv[1], "mp") == 0;
    const int P = is_mp && argc > 2 ? std::atoi(argv[2]) : 0;
    const int runs = std::atoi(argv[argc - 1]) > 0 && argc > (is_mp ? 3 : 2) ? std::atoi(argv[argc - 1]) : 7;
    if (!is_mp) {
        std::vector<double> ap, aq, sp, sq;
        // interleave variants run by run so drift affects both equally
        for (int r = 0; r < runs; ++r) { single<AcqRel>(ap, aq, 1); single<SeqCst>(sp, sq, 1); }
        std::printf("single-thread, uncontended, %d runs (median, ns/op)\n", runs);
        std::printf("  acq_rel  push %.2f   pop %.2f\n", median(ap), median(aq));
        std::printf("  seq_cst  push %.2f   pop %.2f\n", median(sp), median(sq));
        std::printf("  seq_cst - acq_rel: push %+.2f ns, pop %+.2f ns\n", median(sp) - median(ap), median(sq) - median(aq));
        std::printf("  per-run push acq_rel:"); for (double x : ap) std::printf(" %.2f", x);
        std::printf("\n  per-run push seq_cst:"); for (double x : sp) std::printf(" %.2f", x); std::printf("\n");
    } else {
        std::vector<double> a, s;
        for (int r = 0; r < runs; ++r) { a.push_back(mp<AcqRel>(P)); s.push_back(mp<SeqCst>(P)); }
        std::printf("%d producers + 1 consumer, %d runs (median Mmsg/s): acq_rel %.2f  seq_cst %.2f  (%+.1f%%)\n",
                    P, runs, median(a) / 1e6, median(s) / 1e6, 100.0 * (median(s) - median(a)) / median(a));
    }
}
