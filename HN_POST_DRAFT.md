# HN Post — Ready to Submit

**Title option 1 (recommended):**
`Ask HN: What's wrong with this C++20 memory model proof for a lock-free MPSC queue?`

**Title option 2:**
`Show HN: I proved Vyukov's MPSC queue correct under C++20 and added a batch-publish API`

**URL:** (text post — no URL field)

---

## Body

I've been building a lock-free MPSC queue in C++20 and wanted to understand
*why* it's correct, not just that it works empirically. The result is a
six-claim formal proof under the C++20 memory model, a batch-publish API
(push_chain/push_batch) with its own correctness claim, and benchmark results.

**Repo:** https://github.com/nisgemML/mpsc-queue
**Proof:** proof/memory_model.md
**Implementation:** include/mpsc/queue.hpp

---

### The core proof (Claims 1–5)

The queue is Vyukov's intrusive MPSC, adapted to C++20 concepts:

```cpp
template<typename T>
    requires std::is_base_of_v<MpscNode, T>
class MpscQueue { ... };
```

Push:
```cpp
(P1) node->next.store(nullptr,  relaxed)
(P2) prev = tail_.exchange(node, acq_rel)
(P3) prev->next.store(node,     release)
```

Pop (consumer only):
```cpp
(C2) next = head->next.load(acquire)
```

**Claim 1 (visibility):** P1 →_sb P3 →_sw C2 (release/acquire
synchronises-with). By transitivity, P1 →_hb C2. All producer writes
are visible to the consumer after the acquire load. ∎

**Claim 2 (acq_rel sufficiency):** We need only a directed happens-before
edge from producer to consumer via the `next` pointer. seq_cst would
add a total order over ALL seq_cst operations — unnecessary and expensive.
On x86-TSO: release store = plain MOV, acquire load = plain MOV,
acq_rel exchange = LOCK XCHG (required for atomicity regardless of ordering).
seq_cst would add a gratuitous MFENCE (~10–40ns). Zero correctness benefit.

**Claim 3 (incomplete-push window):** Between P2 and P3, `tail_` points
to `node` but `prev->next` is still null. The consumer reads null and
returns nullptr — the documented retry behaviour. No data race:
null was written by P1 (a valid write, sequenced-before P3).

**Claim 4 (no ABA):** ABA requires CAS. This queue has no CAS —
`tail_` is updated by unconditional exchange; `head_` is written only
by the single consumer. ABA cannot arise.

**Claim 5 (FIFO within a producer):** Each producer's exchange sees
the previous exchange's result (sequenced-before in a single thread).
The linked list preserves push order within a single producer.

---

### The new claim: batch publish (Claim 6)

Under P concurrent producers, single push serialises every node on one
LOCK XCHG against the shared `tail_` cache line.

`push_batch(nodes, K)` links K nodes locally with K relaxed stores
(thread-local, no inter-thread contention) then publishes with one
`tail_.exchange`. Amortised cost: 1 contended operation per K nodes.

```cpp
(B1) for i in [0, K-2]: nodes[i]->next.store(nodes[i+1], relaxed)
(B2) last->next.store(nullptr, relaxed)
(B3) prev = tail_.exchange(last, acq_rel)
(B4) prev->next.store(first, release)
```

**Claim 6:** B1/B2 →_sb B4 (sequenced-before). B4 →_sw C2
(release/acquire). By transitivity: B1/B2 →_hb C2. The entire chain
is visible to the consumer before traversal. The chain is published
atomically via the single edge B4 — no partially-linked interior
is ever observable. ∎

---

### Benchmark results (committed to BENCHMARK_RESULTS.md)

**Batch throughput (1M msgs/producer, x86-64 container):**

```
producers   K=1      K=8      K=32     K=128
1          46.7 M  142.7 M  148.0 M  364.1 M msg/sec
2          67.0 M  225.6 M  203.1 M  359.3 M
4          75.8 M  173.0 M  223.7 M  250.5 M
8          74.9 M  144.8 M  171.7 M  162.2 M
```

K=128 at 1 producer: **364M msg/sec** (7.8× vs K=1). Consumer becomes
the bottleneck around P=4–8, which limits further batching gains.

**Software tick-to-trade (ITCH decode → LOB → A-S quote, 500K events):**

```
p50  :  80 ns
p90  :  98 ns
p99  : 150 ns
p99.9: 331 ns
```

18 TSan litmus tests, zero data races.

---

### The part I'm least confident in

Claim 2 argues `acq_rel` on `tail_.exchange` is sufficient. Specifically:
the acquire side of the exchange ensures the current producer sees the
previous producer's (P1) nullptr write before executing P3. This is
guaranteed by the C++20 release sequence rules (**[atomics.order]**
§31.4) which extend synchronises-with through rmw chains.

Is there an interleaving on ARM or POWER (weaker than x86-TSO) where
this fails? I believe not — because the exchange is always an rmw,
and rmw operations participate in release sequences regardless of
the underlying architecture. But I'd welcome a counterexample or
a pointer to a litmus test that probes this edge.

The six-claim proof and all benchmark output are committed to the repo.
Happy to be told where the proof is wrong.

---

## Posting checklist

- [ ] Push all 9 repos to GitHub (all public)
- [ ] Verify proof/memory_model.md covers all 6 claims (done above)
- [ ] Verify BENCHMARK_RESULTS.md has committed real numbers (done above)
- [ ] Submit on weekday 9–11am US Eastern (HN peak traffic)
- [ ] Monitor first 30 minutes — respond to technical comments quickly
- [ ] If someone finds a proof gap: engage seriously and publicly acknowledge

## Secondary venues

- **r/cpp** — the C++20 memory model angle
- **lobste.rs** — better signal-to-noise for systems posts
- **OCaml Discord** — link the probability.ml module separately
- **Trading Tech Discord** (#low-latency channel) — the tick-to-trade numbers
