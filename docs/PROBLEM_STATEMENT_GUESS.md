# What will the Code Conflux problem statement probably ask?

> **Status: educated guess.** Written on 2026-10-18, before the statement and starter repository are
> released (22 Oct). Everything below is inferred from the event poster; none of it is confirmed.
> The point of writing it down is to (a) decide what to build first and (b) have a checklist to
> compare against the moment the real statement lands — see [ADAPTING.md](ADAPTING.md).

## What the poster actually says

Quantactive — Code Conflux is *"a technical coding competition focused on quantitative finance,
trading systems, and backend engineering. Participants will tackle real-world challenges ranging
from **trading infrastructure and risk management** to **smart order routing**, **portfolio & P&L
systems**, testing **algorithmic thinking, system design, concurrency, and low-latency
engineering**."*

| Fact from the poster | What it implies |
|---|---|
| "Project Repository (Starter Code)" is released together with the statement | It is a **project**, not a puzzle set: skeleton interfaces + a harness. Expect to fill in modules, not write a program from nothing. |
| Teams must **submit the repository by 29 Oct** to present on 31 Oct | Judged on more than hidden-test output: code quality, design, and a **presentation** carry weight. |
| 22 Oct → 29 Oct is one week | Scope is "a working vertical slice done well", not a full exchange. |
| Offline final at MNNIT (NLHC-1/2), 31 Oct | Probably a live demo and Q&A: reproducibility (`scripts/demo.sh`) matters. |

## The five themes and how likely each is a graded module

Likelihood is my judgement from the wording ("ranging from ... to ..." lists them in this order).

| # | Theme (poster wording) | Likely task | Confidence | Where it lives here |
|---|---|---|---|---|
| 1 | trading infrastructure | **Limit order book + matching engine**: price-time priority, limit/market/IOC/FOK, cancel, amend; stream of orders in, trades out | very high | `order_book`, `engine`, `exchange`, `csv_io` |
| 2 | risk management | **Pre-trade risk gate**: size / notional / price-collar (fat finger), position limit, rate limit, kill switch, daily loss | very high | `risk`, `exchange` |
| 3 | smart order routing | **Split a parent order across venues** with different prices, fees, latency and depth to minimise cost | high | `router`, `oms`, `simulator` |
| 4 | portfolio & P&L systems | **Positions and P&L**: realized / unrealized, FIFO vs average cost, fees, shorts and flips, exposure | very high | `portfolio` |
| 5 | system design, concurrency, low latency | **Thread-safe / lock-free design and a benchmark**: sequencer or sharding, SPSC queues, no allocation on the hot path, latency percentiles | high | `spsc_queue`, `sharded_exchange`, `latency`, `bench/` |

Less likely, but cheap to bolt on if they appear (all are natural extensions of the existing code):
stop / stop-limit and iceberg orders; market-data feed handler (parse an ITCH-like binary or CSV feed
and rebuild the book); FIX-style message parsing; event-sourced journal with snapshot recovery;
VWAP/TWAP/POV execution algorithms (TWAP is already included); backtesting a strategy on recorded data.

## The input/output shape I expect

Because the harness is unknown, `qc replay` takes the most common shape: **a CSV of order messages
in, trades and P&L out** (format in `include/qc/csv_io.hpp`, worked example in
`data/sample_orders.csv` with expected outputs in `data/expected/`). If the real format differs
(JSON lines, FIX, a Python/Java callback interface), only `csv_io` changes — the engine, risk and
P&L modules are format-agnostic.

Hidden tests, if any, most likely check some of:

* **exact trade list** for a fixed order stream (price-time priority, trade at maker price, partial fills);
* **rejects**: invalid quantity/price, unknown cancel, duplicate id, FOK that cannot fill, post-only that would cross;
* **P&L numbers** to a tolerance (average cost vs FIFO, fee handling, position flips);
* **risk decisions**: which orders are refused and for which reason;
* **throughput / latency** under a time limit on a large stream;
* **thread-safety** under a concurrent harness (no lost or duplicated orders, no crashes).

## Rules the statement may fix that this code had to choose (assumptions)

These are the places where two reasonable engineers disagree. Each is a deliberate choice, listed so
that a one-line diff can flip it once the real rule is known.

| Question | Choice made | Where to change |
|---|---|---|
| Trade price | the **resting (maker) order's** price | `OrderBook::match` |
| Same-price priority | strict FIFO by arrival; reducing size in an amend keeps priority, increasing or repricing loses it | `OrderBook::modify` |
| Market order into an empty / thin book | remainder **expires** (not rejected, not rested) | `OrderBook::submit` |
| `MODIFY` quantity meaning | the new **remaining** quantity | `MatchingEngine::process` |
| Self-trade | allowed by default; `--stp resting` or `--stp taker` enables prevention | `BookConfig::stp` |
| Can an id be reused after the order is gone? | yes — duplicates are only detected against *live* orders | `OrderBook::validate_order` |
| Money / prices | integer ticks (0.01) for prices; `double` currency for P&L; CSV prices must lie on the tick grid | `types.hpp` |
| Cost basis | average cost by default, FIFO selectable (total P&L is identical either way) | `CostBasis` |
| Fees | bps of notional; maker rebates negative; charged on fills | `FeeSchedule` |
| Unrealized P&L mark | last trade price (replay) / consolidated mid (simulator) | `Exchange`, `Oms` |
| Rate limiting | token bucket on message time | `RiskEngine::check` |
| Daily-loss limit | net P&L (realized + unrealized − fees) at or below −limit trips a per-client kill switch and cancels that client's resting orders | `Exchange::dispatch` |

## What to build first if time gets tight

1. Matching engine + CSV replay + golden tests (themes 1 and a sliver of 4) — already the strongest module here.
2. P&L with both cost bases.
3. Risk gate.
4. Router.
5. Concurrency + benchmark numbers (they make a presentation, but a *wrong* fast engine loses to a *right* slow one).
