# Memory Model Proof: Vyukov MPSC Queue (C++20)

This document proves that the MPSC queue in `include/mpsc/queue.hpp` is correct
under the C++20 memory model. We prove six claims, structured from the simplest
(single-push visibility) to the most complex (batch-publish correctness).

No claim requires `seq_cst`. All synchronisation uses `acquire`/`release`
and the C++20 happens-before relation defined in **[intro.races]**.

---

## Background: C++20 Memory Model Primitives

We use three concepts from **[intro.races]** and **[atomics.order]**:

**Sequenced-before (→_sb):** Within a single thread, evaluation A is
sequenced-before evaluation B if A appears earlier in program order.
Sequenced-before is the intra-thread order.

**Synchronises-with (→_sw):** A release store to atomic object M
*synchronises-with* an acquire load from M that observes that store.
This is the inter-thread synchronisation edge.

**Happens-before (→_hb):** The transitive closure of sequenced-before
and synchronises-with. If A →_hb B, all effects of A are visible at B.

**Key lemma (release sequence):** If a release store R is followed by
a sequence of `rmw` (read-modify-write) operations on the same object,
an acquire load that reads from any operation in the sequence
synchronises-with R. We use this implicitly for `tail_.exchange`.

---

## Data Structure Invariants

```
head_ → [stub] → [node₁] → [node₂] → ... → [nodeₙ] ← tail_
```

- `tail_` points to the last enqueued node (or stub when empty).
- `head_` points to the node *before* the next node to dequeue (consumer-only).
- A node is *ready* when `head_->next` is non-null.
- The *incomplete-push window*: after the producer's `tail_.exchange` but
  before `prev->next.store(node, release)`, `head_->next` may be null
  even though the queue is non-empty. The consumer returns `nullptr` and retries.

---

## Claim 1: Producer writes are visible to the consumer after `pop()`

**Operations (single push):**

```
Producer:
  (P1) node->next.store(nullptr, relaxed)    ← initialise node
  (P2) prev = tail_.exchange(node, acq_rel)  ← splice into list
  (P3) prev->next.store(node, release)       ← publish to consumer

Consumer:
  (C1) head = head_.load(relaxed)
  (C2) next = head->next.load(acquire)       ← observe publication
```

**Proof:**

P1 →_sb P3 (sequenced-before in producer thread).

P3 is a release store to `prev->next`.
C2 is an acquire load from the same address (`head->next` = `prev->next`
after the producer's exchange makes `prev` the node before `node`).

P3 →_sw C2 (release/acquire synchronises-with).

By transitivity:

```
P1 →_sb P3 →_sw C2
∴ P1 →_hb C2
```

Any user data written by the producer before P1 is also happens-before C2
(user writes →_sb P1 →_hb C2). After C2, the consumer can safely read
all fields of `*next`. ∎

---

## Claim 2: `acq_rel` on `tail_.exchange` is sufficient — `seq_cst` is not needed

**What `acq_rel` on the exchange provides:**

- **Release side:** the store of `node` into `tail_` is a release store.
  Any write sequenced-before the exchange (including P1: `node->next = nullptr`)
  is visible to any thread that acquires from `tail_`.

- **Acquire side:** the load of the old `tail_` value is an acquire load,
  so everything sequenced-before the *previous* producer's exchange
  happens-before ours. The write that matters is that producer's P1,
  `prev->next.store(nullptr)`. Our P3 then writes `prev->next = node`.
  Because P1 happens-before our P3, write-write coherence
  ([intro.races]) puts our store *after* P1 in `prev->next`'s
  modification order. Without the acquire, the two stores to `prev->next`
  would be unordered, P1's null could land last, and our node (and
  everything linked after it) would be silently lost.

  (An earlier version of this section said the acquire makes the previous
  producer's P3 visible. It cannot: that P3 is sequenced *after* the
  exchange we synchronize with, so acquire guarantees nothing about it —
  and nothing here needs it.)

**What `seq_cst` would add:**

A total order over all `seq_cst` operations across all threads.
We do not need a total order — we need only the directed edge:
*"this producer's writes are visible to the consumer."*
That edge is established by the release/acquire on `prev->next` (Claim 1).

**x86-TSO cost:**

| Operation | acq_rel | seq_cst |
|---|---|---|
| Store | plain `MOV` | `XCHG` or `MOV + MFENCE` |
| Load | plain `MOV` | plain `MOV` |
| RMW (exchange) | `LOCK XCHG` | `LOCK XCHG` |

The `tail_.exchange` is a `LOCK XCHG` regardless of ordering because x86
requires `LOCK` for atomic RMW, so `seq_cst` on the exchange adds nothing.
GCC compiles a `seq_cst` store to `XCHG`, not `MOV + MFENCE`. Disassembled
(g++ 13 -O2): an acq_rel `push()` is `MOV; XCHG; MOV` (one locked op), and
the same function with every op `seq_cst` is `XCHG; XCHG; XCHG` (three).

**Measured, not estimated** (`bench/bench_ordering.cpp`, same algorithm,
only the orderings differ; single thread, uncontended, Xeon @ 2.1 GHz
container, median of 9 runs, two separate invocations):

| | push (ns) | pop (ns) |
|---|---|---|
| acq_rel | 8.74 / 8.81 | 1.85 / 1.87 |
| all seq_cst | 20.92 / 20.47 | 1.91 / 1.86 |

Two extra locked instructions cost ~12 ns per push, 2.3x the acq_rel push;
pop is unchanged, as the identical load codegen predicts. Under contention
the gap depends on core count and topology; `scripts/run_pinned_bench.sh`
runs the 2- and 4-producer comparison, which needs real dedicated cores.

**Conclusion:** `acq_rel` on `tail_.exchange` is necessary — its release
half (in the previous producer) and acquire half (in ours) together form
the synchronizes-with edge the coherence argument above relies on — and
sufficient. `seq_cst` throughout would cost ~2.3x on the uncontended push
path with no correctness benefit. ∎

---

## Claim 3: The incomplete-push window is safe

**The window:** Between P2 (`tail_.exchange`) and P3 (`prev->next = node`),
`tail_` points to `node` but `prev->next` is still the old value
(either `nullptr` or a previously enqueued node).

**Why the consumer is safe during this window:**

The consumer reads `head->next` (C2). During the window, `head->next`
is the node that was at the head before this push began — not `prev->next`.
The window only affects the *last link in the chain*. If the consumer
reaches `prev` before P3 completes, it reads `prev->next == nullptr`
and returns `nullptr`, then retries.

Crucially, `nullptr` was written by P1 with a sequenced-before edge to P3.
The consumer's C2 acquire will, upon a subsequent retry after P3 completes,
observe P3's release and correctly see `node`.

**No data race:** `prev->next` is written by exactly one producer (the one
that holds the exchange result). No other thread writes `prev->next` between
P2 and P3. The consumer only reads `head->next`; it does not read `prev->next`
until it has advanced `head_` past all prior nodes. ∎

---

## Claim 4: No ABA problem

**Classic ABA:** Thread A reads `ptr = P`. Thread B pops P and reuses it.
Thread A executes `CAS(ptr, P, new)` — succeeds spuriously because ptr
still reads P, even though the queue state has changed.

**This queue uses no CAS:**

- `tail_` is updated by unconditional `exchange` (not compare-and-swap).
  An unconditional exchange cannot spuriously succeed — it always swaps,
  regardless of the current value.
- `head_` is written only by the single consumer via plain store.
  No other thread writes or CAS-es `head_`.

Since no CAS operation exists in the queue, the ABA problem cannot arise
by definition. Reusing a previously popped node is safe provided the
caller re-initialises `node->next` before the next push (which `push()`
does unconditionally at P1). ∎

---

## Claim 5: FIFO ordering within a single producer

**Observation:** A single producer executing pushes A, B, C (in that order)
will have them dequeued in order A, B, C.

**Proof:**

Each push atomically splices one node onto the tail. For pushes A then B:

```
push(A): prev_A = tail_.exchange(A, acq_rel)   → tail_ == A
         prev_A->next.store(A, release)

push(B): prev_B = tail_.exchange(B, acq_rel)   → prev_B == A (from same thread)
         prev_B->next.store(B, release)         → A->next = B
```

The trace above is the single-producer case. With other producers running,
push(B)'s exchange need **not** return `A`: another producer's exchange can
land between them, giving `A → X → B`. The real argument does not need
`prev_B == A`:

1. Every exchange on `tail_` is an RMW, so all of them form one total
   modification order of `tail_`, and each exchange returns the value
   written by its immediate predecessor in that order.
2. Each node is linked after exactly the node that preceded it in that
   order (its `prev`), so **list order = `tail_`'s modification order.**
3. push(A)'s exchange is sequenced-before push(B)'s in one thread, so by
   write-write coherence A's exchange precedes B's in that order.

Hence A precedes B in the list, with zero or more other producers' nodes
between them, and the single consumer dequeues A before B. Induction over
A₁…Aₙ gives per-producer FIFO. ∎

**Note:** FIFO across producers is *not* guaranteed. Two producers racing
may have their nodes interleaved in any order depending on the scheduling
of their `tail_.exchange` operations.

---

## Claim 6: Batch publish (`push_chain` / `push_batch`) is correct

This is the most important new claim, corresponding to the `push_chain` and
`push_batch` APIs introduced in `queue.hpp`.

### Motivation

Under P concurrent producers, each single `push()` call serialises on one
`LOCK XCHG` against the shared `tail_` cache line. For bursty workloads —
draining N items from a local ring buffer before publishing — this means
N contended cache-line transfers per batch.

`push_chain(first, last)` links N nodes locally with N relaxed stores
(thread-local, zero inter-thread contention) then publishes the entire
chain with a single `tail_.exchange`. Amortised cost: 1 contended operation
per N nodes instead of N.

### Operations

```
Producer (push_chain(first, last)):
  (B1) for i in [0, N-2]:
         nodes[i]->next.store(nodes[i+1], relaxed)   ← link chain internally
  (B2) last->next.store(nullptr, relaxed)             ← terminate chain
  (B3) prev = tail_.exchange(last, acq_rel)           ← splice into global list
  (B4) prev->next.store(first, release)               ← publish to consumer

Consumer:
  (C2) next = head->next.load(acquire)                ← observe B4
```

### Proof

**Part A: All chain-internal links are visible to the consumer before
it traverses the chain.**

B1 (all iterations) →_sb B4 (sequenced-before in producer thread).
B2 →_sb B4 (sequenced-before).

B4 is a release store to `prev->next`.
C2 is an acquire load from the same address (when `head` reaches `prev`).

B4 →_sw C2 (release/acquire synchronises-with).

By transitivity:

```
B1 →_sb B4 →_sw C2   ∴ B1 →_hb C2
B2 →_sb B4 →_sw C2   ∴ B2 →_hb C2
```

When the consumer's acquire load (C2) observes B4, all chain-internal
links (B1) and the chain terminator (B2) have happened-before C2.
The consumer will never observe a partially linked interior node. ∎

**Part B: The chain is published atomically — the consumer never observes
a partially published chain boundary.**

The consumer enters the chain via `prev->next`, which transitions from
its old value to `first` precisely when B4's release store is observed
by C2's acquire load. Before that transition, the consumer cannot reach
any node in the chain. After that transition, the entire chain is
reachable (by Part A). There is no intermediate state. ∎

**Part C: FIFO ordering within the batch.**

Within the batch, `nodes[0]->next == nodes[1]`, ..., `nodes[N-2]->next == nodes[N-1]`
(set by B1). The consumer traverses `next` pointers in insertion order,
so nodes are dequeued in the order they were passed to `push_batch`. ∎

**Part D: The incomplete-push window between B3 and B4 is handled correctly.**

Between B3 and B4, `tail_` points to `last` but `prev->next` still holds
its old value. This is identical in shape to the single-push window
(Claim 3). The consumer reads `head->next`; if it reaches `prev` before
B4 completes, it reads `nullptr` (or the previous value) and returns
`nullptr`, retrying. The same retry mechanism handles both cases. ∎

**Part E: No ABA problem introduced.**

`push_chain` uses one `tail_.exchange` (unconditional) and one release store.
No CAS is introduced. The no-ABA argument from Claim 4 is unchanged. ∎

**Part F: Interaction with concurrent single-push producers.**

A concurrent single-push producer and a `push_chain` producer both call
`tail_.exchange`. The exchange is atomic — exactly one will observe the
other's tail value, and the `prev->next` store of the winner will correctly
point into the other's node. The happens-before chain is established
identically to Claims 1 and 6 respectively. No special coordination is
required. ∎

---

## Claim 7: Node lifetime — when a popped node may be reused

**Statement:** after `pop()` returns node `a`, `a` may be reused (mutated,
freed, re-pushed) only once a *later* `pop()` has returned a different node.

**Proof:** after the pop, `head_ == a`, so the next pop reads `a->next`.
If the queue is now empty, `tail_ == a` as well, so the next push writes
`a->next` (its P3). Neither pointer moves off `a` until a pop *returns* the
node after it — a pop that returns `nullptr` changes nothing. ∎

**This repo used to document the wrong rule** ("after the next pop(), even
if it returns nullptr"). `tests/test_lifetime.cpp` follows that rule — push
`a`, pop `a`, pop `nullptr`, re-push `a` — and gets `a->next == a` (the
re-push's P3 runs with `prev == a`), after which `pop()` returns `a`
forever. The test keeps that case as a tripwire, checks the correct rule,
and recycles a 64-node pool across 4 producers under the correct rule
(200,000 values, each delivered exactly once, clean under TSan and ASan).

---

## Progress and linearizability

These are the properties most often overstated for this queue, including
by an earlier version of this repo.

**push() is wait-free on x86 and on AArch64 with LSE.** It is straight-line
code: P1, one exchange, P3. That bound holds only if the exchange is
wait-free in hardware: `LOCK XCHG` and LSE `SWPAL` are; ARMv8.0's
`LDAXR/STLXR` loop can in principle retry forever under contention, which
makes push lock-free, not wait-free, on such cores.

**The queue is not lock-free.** `pop()` never loops, but returning `nullptr`
is not progress. Suppose producer P is preempted between its exchange (P2)
and its link store (P3). Every node pushed after P's exchange is linked
behind P's node, which is unreachable until P3 runs. The consumer cannot
dequeue any of them, however many other pushes complete, until a
suspended thread is scheduled. A lock-free algorithm guarantees some
thread makes progress in a bounded number of steps regardless of others'
scheduling; this one does not. Vyukov's own description says the same. In
practice the window is two instructions wide, so a stall needs a
preemption at exactly that point — rare, not impossible, and most likely
on oversubscribed hosts.

**The queue is not linearizable.** Same scenario: P exchanges and is
preempted; Q then pushes and *returns*; the consumer's `pop()` returns
`nullptr`. Q's push completed before the pop started, so any linearization
must order Q's push first, and then a FIFO queue containing Q's node
cannot be empty. What does hold:

- per-producer FIFO (Claim 5);
- no loss and no duplication;
- eventual visibility: a completed push is visible to the consumer once
  every push whose exchange preceded it has also completed.

An empty `pop()` therefore means "nothing is currently reachable", not
"the queue is empty". Callers must treat it as a retry hint, which is how
every consumer in this repo uses it.

---

## Summary

| Claim | Statement | Proof mechanism |
|---|---|---|
| 1 | Producer writes visible after pop | P1 →_sb P3 →_sw C2, transitivity |
| 2 | acq_rel necessary and sufficient; seq_cst unnecessary | Release/acquire edge between producers orders stores to `prev->next` (coherence); seq_cst measured at ~2.3x push cost |
| 3 | Incomplete-push window safe | Consumer reads nullptr, retries; no data race |
| 4 | No ABA problem | No CAS in the queue |
| 5 | FIFO within a single producer | List order = `tail_` modification order; coherence |
| 6 | Batch publish correct | B1/B2 →_sb B4 →_sw C2; atomic boundary; no new ABA |
| 7 | Reuse a popped node only after a later pop returns another | head_/tail_ still point at it until then |
| — | Progress | push wait-free (x86, ARM LSE); queue **not** lock-free; **not** linearizable |

All six claims hold under the C++20 memory model (**[intro.races]**, **[atomics.order]**).
The claims are exercised by 18 TSan litmus tests (`tests/test_tsan.cpp`,
zero reports) and run on both x86-64 and AArch64 in CI. Neither is a
proof: TSan checks the C++ memory model on the interleavings that actually
ran, and hardware testing only shows behaviour the hardware happened to
produce. AArch64 matters because it is weakly ordered, so a missing
acquire or release can cause real misbehaviour there that x86's TSO hides.

---

## References

1. Vyukov, D. (2010). *Intrusive MPSC node-based queue.*
   https://www.1024cores.net/home/lock-free-algorithms/queues/intrusive-mpsc-node-based-queue

2. ISO/IEC 14882:2020 (C++20), **[intro.races]** §6.9.2.1 — Data races.

3. ISO/IEC 14882:2020 (C++20), **[atomics.order]** §31.4 — Order and consistency.

4. Sewell, P. et al. (2010). *x86-TSO: A rigorous and usable programmer's model
   for x86 multiprocessors.* CACM 53(7).

5. Herlihy, M. & Shavit, N. (2012). *The Art of Multiprocessor Programming.*
   Chapter 10: Concurrent Queues and the ABA Problem.

6. Boehm, H. & Adve, S. (2008). *Foundations of the C++ concurrency memory model.*
   PLDI 2008.
