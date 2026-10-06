// tests/test_lifetime.cpp — the node-lifetime contract of MpscQueue::pop().
//
// A popped node stays the queue's sentinel until a LATER pop() returns a
// DIFFERENT node. This file pins that rule from three sides:
//   1. the correct rule works (deterministic, single thread);
//   2. the rule this repo used to document — "free after the next pop(),
//      even if it returns nullptr" — demonstrably corrupts the queue. Kept
//      as a tripwire: if pop() is ever changed to release the node on an
//      empty pop (e.g. Vyukov's stub re-push), this check flips and forces
//      the header comment and this file to be updated with it;
//   3. a fixed node pool recycled by the correct rule under real concurrency
//      (run under TSan and ASan in CI): every value delivered exactly once.

#include "mpsc/queue.hpp"

#include <atomic>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL: %s\n", m); } } while (0)

struct N : mpsc::MpscNode { int v{0}; std::atomic<bool> in_use{false}; };

static void test_correct_rule() {
    mpsc::MpscQueue<N> q;
    N a, b; a.v = 1; b.v = 2;
    q.push(&a);
    CHECK(q.pop() == &a, "pop returns a");
    q.push(&b);
    CHECK(q.pop() == &b, "pop returns b -> a is now released");
    q.push(&a);                                  // recycle a: allowed now
    CHECK(q.pop() == &a, "recycled a delivered");
    CHECK(q.pop() == nullptr, "then empty (no duplicate)");
    CHECK(q.pop() == nullptr, "still empty");
}

static void test_old_documented_rule_corrupts() {
    mpsc::MpscQueue<N> q;
    N a; a.v = 1;
    q.push(&a);
    CHECK(q.pop() == &a, "pop returns a");
    CHECK(q.pop() == nullptr, "pop returns nullptr (old docs: a is now free)");
    q.push(&a);                                  // what the old docs allowed
    CHECK(a.next.load() == &a, "TRIPWIRE: re-push after an empty pop self-links a (old rule is unsafe)");
    int dup = 0;
    for (int i = 0; i < 4; ++i) if (q.pop() == &a) ++dup;
    CHECK(dup == 4, "TRIPWIRE: a is then returned on every pop");
}

// Producers draw nodes from a shared fixed pool (claim with CAS on in_use);
// the consumer releases node k only after pop() has returned node k+1.
static void test_pool_recycling_concurrent() {
    constexpr int kProducers = 4, kPerProducer = 50'000, kPool = 64;
    mpsc::MpscQueue<N> q;
    auto pool = std::make_unique<N[]>(kPool);
    std::vector<std::atomic<int>> seen(kProducers * kPerProducer);
    for (auto& s : seen) s.store(0, std::memory_order_relaxed);

    auto claim = [&]() -> N* {
        for (unsigned i = 0;; ++i) {
            N& n = pool[i % kPool];
            bool f = false;
            if (n.in_use.compare_exchange_weak(f, true, std::memory_order_acquire)) return &n;
            if (i % kPool == kPool - 1) std::this_thread::yield();
        }
    };

    std::vector<std::thread> ps;
    for (int p = 0; p < kProducers; ++p)
        ps.emplace_back([&, p] {
            for (int i = 0; i < kPerProducer; ++i) {
                N* n = claim();
                n->v = p * kPerProducer + i;
                q.push(n);
            }
        });

    int got = 0, bad = 0;
    N* pending = nullptr;
    while (got < kProducers * kPerProducer) {
        N* n = q.pop();
        if (!n) { std::this_thread::yield(); continue; }
        if (n->v < 0 || n->v >= kProducers * kPerProducer) ++bad;
        else seen[n->v].fetch_add(1, std::memory_order_relaxed);
        if (pending) pending->in_use.store(false, std::memory_order_release);  // release k when k+1 arrives
        pending = n;
        ++got;
    }
    for (auto& t : ps) t.join();
    int once = 0;
    for (auto& s : seen) once += (s.load() == 1);
    CHECK(bad == 0, "no out-of-range values");
    CHECK(once == kProducers * kPerProducer, "every value delivered exactly once");
    CHECK(q.pop() == nullptr, "queue drained");
}

int main() {
    test_correct_rule();
    test_old_documented_rule_corrupts();
    test_pool_recycling_concurrent();
    std::printf("Results: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
