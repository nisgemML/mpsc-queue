# Lock-Free MPSC Queue

[![CI](https://github.com/nisgemML/mpsc-queue/actions/workflows/ci.yml/badge.svg)](https://github.com/nisgemML/mpsc-queue/actions/workflows/ci.yml)

The Vyukov intrusive MPSC (Multi-Producer Single-Consumer) queue in C++20, with a formal memory-ordering proof, ThreadSanitizer litmus tests, and a benchmark comparing it against a mutex-based baseline.

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

Typical results (Linux x86-64, Ryzen 5900X, isolated core):

```
=== Throughput: MPSC vs Mutex ===

  MPSC   1 producers  1000000 msgs    38.2 ms    26.18 M msg/s
  Mutex  1 producers  1000000 msgs    51.6 ms    19.38 M msg/s

  MPSC   4 producers  4000000 msgs   142.1 ms    28.15 M msg/s
  Mutex  4 producers  4000000 msgs   438.7 ms     9.12 M msg/s

  MPSC   8 producers  8000000 msgs   281.4 ms    28.43 M msg/s
  Mutex  8 producers  8000000 msgs  1247.3 ms     6.41 M msg/s

=== Push-to-pop latency (single producer) ===
  p50     :  42 ns
  p99     : 118 ns
  p99.9   : 312 ns
```

The lock-free advantage scales with producer count. At 8 producers, MPSC is ~4.4× faster than a mutex queue because producers only serialise on `LOCK XCHG` (~4 cycles) rather than on a kernel mutex (~20–40ns).

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

**Incomplete push window.** `pop()` may return `nullptr` when a producer has completed `tail_.exchange` but not yet stored to `prev->next`. Callers must retry on `nullptr`. This is inherent to the algorithm and cannot be eliminated without adding a separate counter (at the cost of two extra atomics per operation).

---

## References

1. Vyukov, D. (2010). *Intrusive MPSC node-based queue.*
   https://www.1024cores.net/home/lock-free-algorithms/queues/intrusive-mpsc-node-based-queue

2. ISO/IEC 14882:2020 §6.9.2 — memory ordering semantics.

3. Sewell, P. et al. (2010). *x86-TSO: A rigorous and usable programmer's model for x86 multiprocessors.* CACM 53(7).

4. Boehm, H. & Adve, S. (2008). *Foundations of the C++ concurrency memory model.* PLDI 2008.
