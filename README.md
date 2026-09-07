# Lock-Free MPSC Queue

[![CI](https://github.com/nisgemML/mpsc-queue/actions/workflows/ci.yml/badge.svg)](https://github.com/nisgemML/mpsc-queue/actions/workflows/ci.yml)

The Vyukov intrusive MPSC (Multi-Producer Single-Consumer) queue in C++20, with a formal memory-ordering proof, ThreadSanitizer litmus tests, and benchmarks covering peak throughput, sustained multi-producer contention with latency histograms, a real end-to-end tick-to-trade pipeline routed through the queue, and a head-to-head comparison against a mutex, a spinlock, and `boost::lockfree::queue`.

The proof is in [`proof/memory_model.md`](proof/memory_model.md). The short version: `memory_order_acquire`/`release` is sufficient. `seq_cst` would add a gratuitous `MFENCE` instruction on x86 for zero correctness benefit.

---

## The queue

```cpp
#include "mpsc/queue.hpp"
using namespace mpsc;

struct MyNode : MpscNode { int value; };

MpscQueue<MyNode> q;

// Any thread — wait-free:
MyNode n{42};
q.push(&n);

// Consumer thread only — lock-free:
MyNode* p = q.pop();   // returns nullptr if empty or incomplete push
```

Nodes must inherit `MpscNode`. The queue is intrusive (no internal allocation). Callers manage node lifetime.

---

## Algorithm

```
head_ ──► [stub] ──► [node₁] ──► [node₂] ──► ... ──► [nodeₙ]
                                                          ▲
                                                       tail_
```

**push(node) — any thread, wait-free:**
```cpp
node->next.store(nullptr,  memory_order_relaxed);        // (P1)
prev = tail_.exchange(node, memory_order_acq_rel);       // (P2)
prev->next.store(node,     memory_order_release);        // (P3)
```

**pop() — consumer thread only:**
```cpp
head = head_.load(memory_order_relaxed);
next = head->next.load(memory_order_acquire);            // (C1)
if (!next) return nullptr;   // empty or incomplete push window
head_.store(next, memory_order_relaxed);
return static_cast<T*>(next);
```

The **incomplete push window** — between (P2) and (P3) — is the only subtlety. During this window, `tail_` points to `node` but the list traversal cannot reach it yet. The consumer returns `nullptr` and retries. This is documented behaviour; callers must handle it.

---

## Memory ordering proof (summary)

The full proof is in [`proof/memory_model.md`](proof/memory_model.md), with citations to the C++20 standard and Sewell et al.'s x86-TSO model. Key claims:

**Claim 1 — Payload visibility:** If `pop()` returns `p`, all writes sequenced-before `push(p)` are visible.

*Proof sketch:*
```
user writes to p  →(seq-before)→  (P3) release-store
(P3) release-store  →(sync-with)→  (C1) acquire-load
(C1) acquire-load   →(seq-before)→  user reads from p
```
By transitivity: user writes happen-before user reads. ∎

**Claim 2 — `acq_rel` on `tail_.exchange` is sufficient:** it is not `seq_cst`. The acquire side ensures we see the previous tail-holder's writes; the release side ensures `node->next = nullptr` is visible to the next producer that reads `tail_`. No total order over all atomic operations is needed.

**Claim 3 — No ABA:** the consumer uses no CAS — only an unconditional store to `head_`. Producers use only unconditional exchange on `tail_`. ABA requires a compare-and-swap that can match a re-used pointer; without CAS there is no ABA.

**Claim 4 — Per-producer FIFO:** each producer's chain of `next` pointers is laid down sequentially. The consumer traverses the chain from `head_`, preserving each producer's push order.

**On x86-TSO:** release stores and acquire loads compile to plain `MOV`. The only cost relative to relaxed is the `LOCK XCHG` on `tail_.exchange` — required for atomicity regardless of ordering. Using `seq_cst` would add a `MFENCE` costing ~10–40ns per push, or ~100–400ms/sec at 10M msg/sec.

---

## TSan validation

```bash
cmake -S . -B build_tsan -G Ninja -DTSAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build_tsan
./build_tsan/test_tsan
```

Expected: `18 passed, 0 failed`, zero TSan reports. The four litmus tests:

| Test | What it validates |
|---|---|
| **MP (Message Passing)** | Payload written before push is visible after pop |
| **Two-producer FIFO** | Each producer's nodes arrive in push order |
| **8-producer stress** | Scales correctly under high contention |
| **Incomplete push** | Consumer correctly retries during the exchange-to-store window |

---

## Benchmark

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/bench_mpsc
```

Full results, methodology, and an explicit tier system for how much to
trust a given number (shared container vs. pinned vs. kernel-isolated
cores) are in [`BENCHMARK_RESULTS.md`](BENCHMARK_RESULTS.md). Summary of
what's there and how to reproduce it:

| Benchmark | What it measures | Status |
|---|---|---|
| `bench_mpsc` | MPSC vs mutex throughput, ping-pong latency | Committed (container, pinned) |
| `bench_batch` | Batch push vs single push, K=1..128 | Committed (container, pinned) |
| `bench_t2t` | ITCH decode -> LOB -> A-S quote, single-threaded | Committed (container, pinned) |
| `bench_stress` | Sustained (not peak) multi-producer contention, full latency histograms | Real numbers from a 1-vCPU sandbox (oversubscription behavior, not peak throughput); dedicated-core run still pending |
| `bench_t2t_queue` | Same tick-to-trade pipeline, but split across a decode thread and a strategy thread connected by `MpscQueue` — the version that actually exercises the queue | Real numbers from a 1-vCPU sandbox; dedicated-core run still pending |
| `bench_comparison` | `MpscQueue` vs `std::mutex`, a spinlock, and `boost::lockfree::queue`, same workload, with a written trade-off discussion | Real numbers from a 1-vCPU sandbox — `MpscQueue` ranks first at every producer count across two independent runs; dedicated-core run still pending |

`scripts/run_pinned_bench.sh` runs all of the above with core isolation
checks, `taskset`/`chrt -f` pinning, and governor/Turbo reporting, and
tells you plainly if the environment it's running in doesn't qualify as a
trustworthy result rather than silently printing numbers anyway. See
`BENCHMARK_RESULTS.md` for exactly which numbers below are real
measurements versus which are still pending a run on qualifying hardware —
that document does not present anything as a result that wasn't actually
produced by the command printed next to it.

Numbers below are the committed container results (see
`BENCHMARK_RESULTS.md` for the exact environment and full methodology):

```
=== Throughput: MPSC vs Mutex (msgs/sec) ===

producers   K=1 (single push)
1             46.73 M
2             67.03 M
4             75.76 M
8             74.87 M

=== Push-to-pop latency (single-thread ping-pong) ===
  p50     :  21 ns
  p99     :  25 ns
  p99.9   :  98 ns
```

These are same-thread round-trip numbers, not cross-thread contention under
sustained load — see `BENCHMARK_RESULTS.md`'s "Sustained multi-producer
contention" section for the harness built to measure that, and its
"Comparison" section for `MpscQueue` benchmarked head-to-head against a
mutex, a spinlock, and `boost::lockfree::queue` on the same workload.

---

## Properties

| Property | Value |
|---|---|
| Producer threads | Any number (M) |
| Consumer threads | Exactly one |
| Push complexity | Wait-free, O(1) |
| Pop complexity | Lock-free, O(1) |
| Memory allocation | None (intrusive) |
| ABA problem | Impossible (no CAS) |
| Per-producer ordering | FIFO guaranteed |
| Cross-producer ordering | Not guaranteed (by design) |
| Memory model | C++20 acquire/release (no seq_cst) |

---

## Building

```bash
# Release build + tests
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure

# ThreadSanitizer (proves zero data races)
cmake -S . -B build_tsan -G Ninja -DTSAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build_tsan && ctest --test-dir build_tsan --output-on-failure

# AddressSanitizer (catches memory errors)
cmake -S . -B build_asan -G Ninja -DASAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build_asan && ctest --test-dir build_asan --output-on-failure
```

---

## Limitations

**Single consumer only.** For MPMC, use a different algorithm (e.g., the Michael-Scott queue with CAS on both head and tail). This queue trades the MPMC generality for wait-free producers and simpler proof obligations.

**Intrusive nodes.** Nodes must inherit `MpscNode`. For a non-intrusive version, embed a `MpscNode` as a member of a wrapper and use a free-list allocator to amortise allocation cost.

**Incomplete push window.** `pop()` may return `nullptr` when a producer has completed `tail_.exchange` but not yet stored to `prev->next`. Callers must retry on `nullptr`. This is inherent to the algorithm and cannot be eliminated without adding a separate counter (at the cost of two extra atomics per operation). Don't busy-spin unconditionally on this — see the recommended retry pattern below.

**Node lifetime after `pop()`.** A node returned by `pop()` becomes the queue's new internal sentinel (`head_`) and stays part of the queue's bookkeeping until the *following* `pop()` call. Recycling that node back into circulation (e.g. handing it to a producer to re-push) before calling `pop()` again races the producer's reset of `node->next` against the consumer's next traversal and can corrupt the list. This bit `bench/bench_stress.cpp` during development — see `BENCHMARK_RESULTS.md`'s "Validation" section for how it manifested (a livelock, not a crash) and the fix (defer freeing a node by one `pop()`). Full detail and the fix pattern are in `queue.hpp`'s `pop()` doc comment.

**Recommended retry pattern.** A bare `while (!(p = q.pop()));` is correct but burns a full core even when idle, and can starve producers on an oversubscribed host. Escalate: spin briefly, then yield, then sleep:

```cpp
T* p; int spins = 0;
while ((p = q.pop()) == nullptr) {
    if      (spins < 1000) { __builtin_ia32_pause(); ++spins; }
    else if (spins < 1100) { std::this_thread::yield(); ++spins; }
    else                     std::this_thread::sleep_for(20us);
}
```

This is `bench::Backoff` in `bench/histogram.hpp`, used throughout this repo's own benchmarks.

---

## References

1. Vyukov, D. (2010). *Intrusive MPSC node-based queue.*
   https://www.1024cores.net/home/lock-free-algorithms/queues/intrusive-mpsc-node-based-queue

2. ISO/IEC 14882:2020 §6.9.2 — memory ordering semantics.

3. Sewell, P. et al. (2010). *x86-TSO: A rigorous and usable programmer's model for x86 multiprocessors.* CACM 53(7).

4. Boehm, H. & Adve, S. (2008). *Foundations of the C++ concurrency memory model.* PLDI 2008.
