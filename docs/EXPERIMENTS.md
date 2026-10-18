# Experiments, measurements and bugs the tests found

All numbers were produced on a 4-vCPU cloud VM (Intel Xeon @ 2.1 GHz, Linux 6.18, GCC 13.3,
`-O2`, Release) — a *shared* machine, which matters for tail latencies (§3). Reproduce with
`scripts/demo.sh`, `build/qc_bench all`, `build/qc sim --compare`.

## 1. Does smart order routing help?

Same simulated market, same parent orders (identical seed; the market maker is off so the
strategies' own fills cannot make the runs diverge for other reasons). Only the router changes.
30 simulated seconds, 1 158 parent orders, three venues with different fees/latency/depth.

| Router | fill rate | slippage vs arrival mid | fees | **all-in** | cost (currency) | child orders / parent |
|---|---:|---:|---:|---:|---:|---:|
| **smart** (fee-aware best price) | 99.9 % | 1.44 bps | 0.78 bps | **2.22 bps** | 24 722 | 2.27 |
| pro-rata by displayed size | 99.9 % | 1.69 bps | 0.84 bps | 2.53 bps | 28 223 | 3.15 |
| primary venue only | 87.7 % | 2.13 bps | 1.20 bps | 3.33 bps | 32 788 | 1.10 |

Robustness: over **12 different seeds (10 s each)** the smart router beat pro-rata in 12/12 runs
(by 0.29–0.34 bps) and the single-venue router in 12/12 (by 0.96–1.21 bps). A scaled-down version of this
is a regression test (`smart_router_beats_the_baselines_on_execution_cost`).

Reading it correctly: 1 bp on $100 is exactly one tick, so these are fractions of a tick per share.
The baselines are intentionally naive; a real router would be compared against the exchange's
own best-execution benchmark. The result shows the *mechanism* (price + fee awareness + not
trusting one venue) works in the model, not that 0.3 bps is a universal constant.

## 2. A negative result: the stale-liquidity haircut does not help

The router has an optional discount `size × exp(−(age + latency)/decay)` meant to avoid "phantom
liquidity" in stale quotes. It was built on the theory that it would pay off when data is stale.
Tested directly (`all-in` cost in bps, 20 s runs, market maker off; "off" = no haircut):

| latency × | vol (ticks/√s) | off | decay 0.5 ms | 2 ms | 10 ms |
|---:|---:|---:|---:|---:|---:|
| 1 | 3 | **2.22** | 2.51 | 2.29 | 2.23 |
| 1 | 15 | **2.33** | 2.56 | 2.40 | 2.34 |
| 10 | 3 | **2.19** | 2.57 (fill 59 %) | 2.60 | 2.31 |
| 10 | 15 | **2.31** | 2.45 (fill 50 %) | 2.60 | 2.40 |
| 30 | 3 | **2.12** | 2.16 (fill **1 %**) | 2.68 (fill 87 %) | 2.45 |
| 30 | 15 | **2.20** | 1.97 (fill **0.7 %**) | 2.27 (fill 73 %) | 2.49 |

(The one cell below its "off" figure — 1.97 vs 2.20 at 30× latency, 15 vol, 0.5 ms — is not a win:
it is measured on 0.7 % of the shares because the orders almost never filled.) In no regime did
the haircut beat leaving it off while still filling the orders.
A uniform shrink is the wrong model — staleness removes the *top* levels, it does not scale every
level — and what actually handles stale data is the OMS retry (wait, re-read, route the remainder).
The knob stays in the code, labelled experimental and **off by default**, because the negative
result is more useful in a design discussion than a deleted feature.

## 3. Performance (single machine; read the caveats)

| Benchmark | Result |
|---|---|
| Order book, mixed flow (70 % add, 20 % cancel, 5 % amend), 2 M ops | **14.6 M ops/s**, 68.6 ns/op mean |
| …per-operation latency (timer overhead 25 ns subtracted) | add-rests p50 74 ns / p99 246 ns · add-crosses p50 90 / p99 438 · cancel p50 42 / p99 174 · amend p50 94 / p99 278 |
| Pre-trade risk, all nine checks | `check()` p50 **29 ns**, p99 49 ns; check + track + fill-report cycle 141 ns/order |
| Whole stack, one thread (risk + match + ledger, 4 symbols) | **2.46 M commands/s**, 406 ns/command |
| Smart order router `route()` (3 venues × 5 levels) | p50 **106 ns**, p99 146 ns |
| One mutex around one exchange, 1 / 2 / 4 threads | 1.99 M → **0.51 M → 0.50 M** commands/s |
| Symbol-sharded SPSC pipeline, 1 producer + 1 / 2 / 3 shards | 1.48 M → 3.28 M → **4.10 M** commands/s |
| Pipeline latency at a *sustainable* paced load (2 shards, 250 k–2 M cmd/s offered) | enqueue→start **p50 ≈ 640 ns**, processing p50 ≈ 350–430 ns |

What these do and do not show:

* **Locking collapses; partitioning scales.** Two threads on one lock are 4× *slower* than one
  thread; sharding by symbol scaled to 4.1 M/s on 4 cores while the producer thread also occupies
  one. With one producer + three shards on four cores that is about the most this box can show.
* **"queue wait" at saturation is not a latency.** When the producer pushes as fast as it can, the
  16 384-slot queue stays full and "wait" is just queue depth × service time (11 ms in the 1-shard
  row). The honest latency figure is the *paced* one above.
* **Tails here are the VM, not the code.** `qc_bench jitter` runs threads that do nothing but read
  the clock: even those see gaps up to **1.8 ms (1 thread) and 3.9 ms (3 threads)** — the hypervisor
  taking the core away. The paced p99/p99.9 of 0.2–11 ms are of that size. Medians are trustworthy;
  tails need a quiet machine (`isolcpus`, pinned threads — `ShardedConfig::pin_threads` — real-time
  priority) and should be re-measured there before being quoted.
* Timer overhead is measured and subtracted but not zero; sub-50 ns figures carry real uncertainty.
* Workload is synthetic: 20 % cancels, 5 % amends, most orders resting near the touch. A book with a
  different shape (deep queues, huge cancel storms) will differ.

## 4. Bugs found, and by what

Honest log — the point is that the test strategy works, not that the first draft was perfect.

| Bug | Found by | Fix |
|---|---|---|
| Daily-loss kill switch only re-checked a client on *its own* fills, so a client whose position lost money because *other people's* trades moved the mark never tripped it | writing the kill-switch test with a hand-built scenario | re-evaluate all loss-limited clients whenever a command produces trades |
| `ProRata` router returned `unplanned = 0` when no liquidity was visible (should equal the requested quantity) | property test `planned + unplanned == qty` over 5 000 random markets | route through `finalize()` on the empty path |
| OMS erased a child order through an iterator that a strategy callback (which may place new orders) could have invalidated by rehashing | review of the callback path; fixed before it could bite, now erased by key | erase by key; copy ids before erasing parents |
| Risk engine: a rejected *duplicate* order id would have released the exposure of the original live order | designing the engine's tracking invariants; regression test added | ignore duplicate-id rejects in `on_report` |
| Risk engine: absurd symbol ids (e.g. 4·10⁹) would allocate gigabytes | review | `UNKNOWN_SYMBOL` guard in `check()` |
| Simulator: consolidated book was crossed 35–60 % of the time | the first `qc sim` run printed it | not a code bug — a missing market participant; added the arbitrageur |
| **Benchmark** bug: the risk benchmark reported 1 960 000 rejects of 2 000 000 and a flattering 20 ns/check | the bench prints the reject count; it looked wrong | side pattern was correlated with client id, so every client ran into the position limit; fixed and the bench now warns if > 1 % reject |
| Pipeline "queue wait" of 11 ms looked like a latency problem | reading the numbers critically | saturation artefact; added a paced-load mode and the jitter probe |
| Kill-switch test passed even with the mass-cancel disabled | **mutation check** (below) | assert that fills stop within one in-flight window after the kill |
| Clang: unused private field; GCC: a false-positive `-Wstringop-overflow` | building with both compilers + sanitizers | removed / restructured |

### Mutation checks

After the suites passed I deliberately broke the code (on a copy of the tree) and confirmed the
tests fail. Caught: buy-side crossing `<` instead of `<=`; amend-in-place disabled; stale level
quantity after a partial fill; FOK dry run ignoring the self-trade stop; new orders joining the
*front* of the queue; OMS not feeding reports to risk; maker/taker fee swap; SOR ignoring fees.
Not caught on the first attempt, then strengthened: *kill switch does not cancel* (see table).
One run of this exercise was itself buggy — a mutation was applied to the real header instead of a
copy, so later "mutations" ran on top of it and all printed identical output. Identical output from
different mutations is the tell; the header was restored and the experiment redone on a copy.

## 5. Things I would do with more time

* Informed/toxic flow in the simulator, so the market maker faces adverse selection.
* A flat-array price ladder and a measured comparison with the tree.
* A write-ahead journal + snapshot to turn "replay is deterministic" into an actual recovery tool.
* Exact global risk limits across shards (shared token bucket / supervisory layer) and a measurement of what they cost.
* Stop / iceberg orders; a market-data feed handler.
