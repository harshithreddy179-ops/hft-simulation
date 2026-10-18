# HFT trading-stack simulation — a sample model for Quantactive · Code Conflux

A working, tested model of the pieces a modern electronic-trading stack is made of: **order book and
matching engine, pre-trade risk, portfolio & P&L, smart order routing across venues, a concurrent
low-latency pipeline, and a discrete-event market simulator** that ties them together.

It was built **before the problem statement was released** (22 Oct), from the event poster's
description — *"trading infrastructure and risk management to smart order routing, portfolio & P&L
systems … algorithmic thinking, system design, concurrency, and low-latency engineering"*. The guess
about what will be asked, and the rules that had to be assumed, are written down in
[docs/PROBLEM_STATEMENT_GUESS.md](docs/PROBLEM_STATEMENT_GUESS.md); how to adapt in an hour once the
real statement arrives is in [docs/ADAPTING.md](docs/ADAPTING.md).

C++20, no dependencies, builds with GCC and Clang. 108 tests, clean under AddressSanitizer,
UBSan and ThreadSanitizer.

## Quick start

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
ctest --test-dir build --output-on-failure            # or ./build/qc_tests [name-substring]

./build/qc replay data/sample_orders.csv --out-dir out   # CSV of orders in -> trades, reports, P&L out
./build/qc gen --count 500000 --symbols 8 --out out/big.csv
./build/qc replay out/big.csv --stp resting --threads 3  # same result as single-threaded, sharded by symbol
./build/qc sim --seconds 10                              # 3 venues: market maker + TWAP + smart order router
./build/qc sim --seconds 30 --compare                    # same market, different routers
./build/qc_bench all                                     # micro-benchmarks
scripts/demo.sh                                          # all of the above, with headings
```

`qc replay` options: `--stp none|resting|taker`, `--cost-basis avg|fifo`, `--taker-fee-bps`,
`--maker-fee-bps`, `--max-order-qty`, `--max-notional`, `--collar-bps`, `--max-position`,
`--max-gross`, `--max-open-orders`, `--rate`, `--max-loss`, `--threads`, `--out-dir`.

## What is in it

| Module | What it does | Verified by |
|---|---|---|
| **Order book** `order_book.*` | price-time priority; limit GTC/IOC/FOK, market, post-only; cancel; amend (keeps priority only if size shrinks at the same price); self-trade prevention; O(1)-ish cancel via pooled intrusive FIFOs + a custom flat id hash | differential fuzz against a naive reference (360 k random ops × 3 STP modes), `validate()` after every op |
| **Matching engine** `engine.*` | multi-symbol routing, ownership checks, FIX-style execution reports (every report carries `leaves`) | scenario + golden-file tests |
| **Risk** `risk.*` | 9 pre-trade checks (kill switch, halt, size, notional, price collar, rate limit, open orders, position, gross exposure) with *worst-case* accounting; daily-loss kill switch with mass-cancel | unit tests per check; invariant fuzz: risk's open-order view always equals what is resting |
| **Portfolio / P&L** `portfolio.*` | positions, realized/unrealized, average-cost **and** FIFO, fees/rebates, short/flip handling, exposure, drawdown | hand-computed cases; accounting identity under random fills; both bases agree on total P&L |
| **Smart order router** `router.*` | consolidated multi-venue book; fee-adjusted best-price splitting; per-child limit caps; pro-rata and single-venue baselines | property: never costlier than baselines when all fully fill |
| **Concurrency** `spsc_queue.hpp`, `sharded_exchange.*` | lock-free SPSC ring; symbol-sharded exchange, one thread per shard | sharded ≡ single-threaded per symbol; multi-producer stress; ThreadSanitizer |
| **Simulator** `simulator.*`, `oms.*`, `strategies.*`, `flow.*` | seeded discrete-event market: 3 venues (fees, latency, jitter, FIFO channels), noise flow, arbitrageur, OMS with parent/child orders and retries, market maker, TWAP, per-strategy P&L | determinism, books-balance, kill-switch and risk-limit scenarios |
| **I/O + tools** `csv_io.*`, `workload.*`, `main.cpp`, `bench/` | order CSV in / trades, reports, P&L CSV out; synthetic flow generator; benchmarks incl. a jitter probe | round-trip and malformed-input tests |

## Results (measured on a 4-vCPU cloud VM; details and caveats in [docs/EXPERIMENTS.md](docs/EXPERIMENTS.md))

**Speed**

| | |
|---|---|
| Order book (mixed add/cancel/amend), 2 M ops | **14.6 M ops/s**; per-op p50 70 ns, p99 326 ns |
| Pre-trade risk, all nine checks | p50 **29 ns**, p99 49 ns |
| Whole stack on one thread (risk + match + ledger) | **2.46 M commands/s** |
| Smart order router decision (3 venues × 5 levels) | p50 **106 ns** |
| One mutex, 1 → 2 → 4 threads | 1.99 → **0.51** → 0.50 M commands/s |
| Symbol-sharded SPSC, 1 producer + 1 → 2 → 3 shards | 1.48 → 3.28 → **4.10** M commands/s |

Latency tails on this shared VM are dominated by the hypervisor (`qc_bench jitter`: a thread that
only reads the clock sees stalls up to ~2–4 ms). Medians are trustworthy; tails should be re-measured
on isolated cores before being quoted.

**Does smart routing help?** (same market, same orders, only the router differs; 30 s simulated)

| router | fill rate | all-in cost |
|---|---:|---:|
| **smart** (fee-aware best price) | 99.9 % | **2.22 bps** |
| pro-rata by displayed size | 99.9 % | 2.53 bps |
| primary venue only | 87.7 % | 3.33 bps |

The smart router won in 12 of 12 seeds against both baselines. An optional "stale-liquidity
haircut" did *not* help in any regime tested — a negative result kept in the repo on purpose
([docs/EXPERIMENTS.md §2](docs/EXPERIMENTS.md)).

## How correctness is established

Differential fuzzing against an independent naive implementation, structural self-validation after
every operation, conservation and accounting identities, golden files, equivalence of the threaded
and single-threaded paths, property tests, three sanitizers — **and mutation checks**: the code was
deliberately broken to confirm the tests fail. That exercise found a test that was too weak and was
then strengthened; the log of every bug found (including in the benchmarks) is in
[docs/EXPERIMENTS.md §4](docs/EXPERIMENTS.md).

## Honest limitations

* The simulated flow is uninformed, so the market maker never faces adverse selection: **simulated
  P&L is a property of the synthetic market, not a trading result.**
* Under sharding, risk limits that span symbols (rate, gross exposure, daily loss) are per-shard
  approximations; per-(client, symbol) limits are exact. See [docs/ARCHITECTURE.md §6](docs/ARCHITECTURE.md).
* Recovery relies on deterministic replay; there is no journal/snapshot implementation yet.
* No stop/iceberg orders, no market-data feed handler, no FIX parsing.
* Several rules (trade price, amend semantics, market-order-into-empty-book, id reuse, mark price)
  are assumptions — listed in [docs/PROBLEM_STATEMENT_GUESS.md](docs/PROBLEM_STATEMENT_GUESS.md).

## Layout

```
include/qc/   public headers (one per module; each file starts with its design notes)
src/          implementations, main.cpp (the `qc` CLI)
tests/        108 tests (self-registering mini framework, no dependencies)
bench/        qc_bench
data/         sample_orders.csv + expected/ golden outputs
docs/         PROBLEM_STATEMENT_GUESS, ARCHITECTURE, EXPERIMENTS, ADAPTING, PRESENTATION
scripts/      demo.sh
.github/      CI: gcc/clang × Release/Debug, ASan+UBSan, TSan
```
