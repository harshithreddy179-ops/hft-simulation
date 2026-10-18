# Presentation outline (≈ 10–12 minutes + Q&A)

Rewrite the numbers from your own final run. The structure is what matters: **problem → design →
proof it is right → proof it is fast → honesty about limits.** Judges at a systems/quant event
reward candour about trade-offs much more than a wall of superlatives.

## Slide plan

| # | Slide | Say | Show |
|---|---|---|---|
| 1 | **The problem in one line** | "A trading stack: accept orders, match them fairly, refuse dangerous ones, route across venues, account for P&L — fast, concurrently, and provably correct." | the layer diagram (ARCHITECTURE §1) |
| 2 | **Design principles** | single-writer per book; partition, don't lock; integer prices; every report carries `leaves`; state is a pure function of the command stream | 4 bullets, no code |
| 3 | **Matching engine** | price-time priority, trade at maker price, FOK dry-run, amend keeps priority only if size shrinks at same price, STP; the data structure (map of levels + pooled intrusive FIFO + flat id table) | the 18-line sample scenario → trades (`data/sample_orders.csv`) |
| 4 | **How we know it's right** | differential fuzz vs a naive reference book (360 k ops × 3 STP modes), `validate()` after every op, accounting identities, golden files, sanitizers; **and mutation checks: we broke the code on purpose and the tests failed** | test counts + the mutation list (EXPERIMENTS §4) |
| 5 | **Risk** | nine checks, cheapest first; *worst-case* accounting (open + in-flight counts); cancels never gated; kill switch re-evaluates on anyone's trade and mass-cancels | `qc replay … --max-loss 500` output: 7 clients killed, 119 k orders blocked |
| 6 | **P&L** | average-cost vs FIFO give different realized/unrealized but **identical total** (and we test the identity under random flips and fees); trading is zero-sum before fees | the 100/110/120 example: 1500+1500 vs 2000+1000 |
| 7 | **Smart order routing** | all-in price = price ± fee; greedy by cost is optimal; one IOC child per venue capped at the planned worst price; OMS retries unfilled remainder with fresh data | `qc sim --compare`: 2.22 vs 2.53 vs 3.33 bps; 12/12 seeds |
| 8 | **Concurrency** | sharded by symbol, SPSC queues, no locks on the matching path; sharded run **equals** single-threaded run per symbol; TSan clean | mutex 1.99 → 0.51 M/s (2 threads) vs sharded 1.48 → 4.10 M/s |
| 9 | **Latency** | p50 book op 70 ns, risk check 29 ns, route 106 ns; **tails are the VM** (jitter probe: 1.8 ms stalls on a thread doing nothing) — say what you'd do on real hardware | the `qc_bench jitter` line next to the p99s |
| 10 | **What went wrong and what we learned** | the kill-switch bug a hand-built test found; the benchmark that was 98 % rejects; the negative result on the stale-liquidity haircut | EXPERIMENTS §2 and §4 |
| 11 | **Limits and next steps** | no informed flow in the sim (MM P&L is flattering), per-shard approximation of global risk limits, no journal yet | EXPERIMENTS §5 |

## Live demo (3 minutes, from a fresh clone, no network)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/qc_tests | tail -1                                   # 108 tests, 0 failed
./build/qc replay data/sample_orders.csv --out-dir out --quiet && cat out/trades.csv
./build/qc gen --count 500000 --symbols 8 --out out/big.csv
./build/qc replay out/big.csv --stp resting --threads 3 | head -3   # then without --threads: same trades
./build/qc sim --seconds 10                                  # MM + TWAP + parent flow through the SOR
./build/qc sim --seconds 30 --compare                        # the router table
```

`scripts/demo.sh` runs all of it with headings. Have its output saved in a file as a fallback.

## Questions you should expect (and honest answers)

**Why C++?** Predictable latency and layout control matter for the low-latency theme; no GC pauses,
no allocation on the hot path. The architecture and test strategy are language-independent.

**Why not lock the book and use many threads?** Measured: two threads on one mutex are ~4× *slower*
than one thread (1.99 → 0.51 M/s); sharding by symbol with SPSC queues reached 4.1 M/s on four cores.
A book is inherently sequential (price-time priority); the parallelism is *across* books.

**What does sharding cost you?** Per-(client, symbol) risk is exact; cross-symbol limits (rate,
gross exposure, daily loss) become per-shard approximations. Exact global limits need a shared
counter or feedback between shards — that is a latency/consistency trade-off, not a free lunch.

**How do you know the order book is correct?** It is checked against an independent, deliberately
naive implementation on random operation streams, with structural validation after every step —
and we verified the tests can fail by breaking the code (mutation checks).

**Your market maker makes money — isn't that unrealistic?** Yes, and we say so: the simulated
flow is uninformed, so the maker never suffers adverse selection. The P&L is a property of the
synthetic market, not a trading result. The simulator's purpose is to exercise the infrastructure
(risk gate, router, accounting) under latency and staleness.

**Why does the stale-liquidity haircut not help?** We built it expecting it to, tested it from 1× to
30× latency and 3 to 15 volatility, and it never beat "off" while filling the orders. A uniform shrink
is the wrong model of staleness; retrying with fresh data works better. It stays in as an
off-by-default experiment.

**Prices as integers?** Exact comparisons, no rounding in the book, and `price × qty` is bounded so it
cannot overflow int64; P&L uses doubles because averages are fractional and the identity is checked
to 1e-6.

**What happens on a crash?** The engine's output is a pure function of the command stream
(determinism is tested), so a write-ahead journal + replay is a complete recovery path. The journal
itself is not built yet — we say so.

**Latency tails look bad.** They are the shared VM: threads that only read the clock see multi-ms
stalls (`qc_bench jitter`). Medians are reliable; on isolated cores with pinned threads we expect the
tails to collapse, but we have not measured that and do not claim it.
