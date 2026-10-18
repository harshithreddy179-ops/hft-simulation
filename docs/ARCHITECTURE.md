# Architecture

## 1. The layers

```
                         ┌───────────────────────── qc replay / qc sim / qc_bench ─────────────────────────┐
                         │                                                                                  │
  CSV orders ──► csv_io ─┤                         simulator (discrete-event, seeded)                       │
                         │   ┌─────────────────────────────────────────────────────────────────────────┐   │
                         │   │ noise flow ─► Venue 0..N ─► depth snapshots ─► ConsolidatedBook (stale) │   │
                         │   │ arbitrageur    ▲   (MatchingEngine + fees + latency)        │           │   │
                         │   │                │ orders (latency, jitter, FIFO)             ▼           │   │
                         │   │                └────────── Oms ◄─────── strategies (MM, TWAP, flow)     │   │
                         │   │                     risk ─ SmartOrderRouter ─ per-strategy Portfolio    │   │
                         │   └─────────────────────────────────────────────────────────────────────────┘   │
                         │                                                                                  │
                         │  Exchange  = RiskEngine ─► MatchingEngine ─► {RiskEngine, Portfolio per client}  │
                         │  ShardedExchange = N × Exchange, one thread each, SPSC queues, shard = symbol%N  │
                         └──────────────────────────────────────────────────────────────────────────────────┘
            MatchingEngine = per-symbol OrderBook + FIX-style ExecReports      (the only place orders trade)
```

Dependencies point downward only. `OrderBook` knows nothing about risk, clients' P&L or threads;
`RiskEngine` and `Portfolio` know nothing about the book (they consume `ExecReport`s); the simulator
is just another client of `MatchingEngine`. That is what makes each layer testable alone.

| Layer | Files | Responsibility |
|---|---|---|
| vocabulary | `types.hpp`, `events.hpp` | ids, enums, `Order`, `Fill`, `ExecReport`, reject reasons |
| order book | `order_book.*`, `flat_id_map.hpp` | price-time matching, cancel, amend, STP, depth, self-validation |
| engine | `engine.*` | multi-symbol routing, ownership checks, book events → execution reports |
| P&L | `portfolio.*` | positions, FIFO/average cost, fees, exposure, drawdown |
| risk | `risk.*` | nine pre-trade checks, open-order mirror, kill switches |
| glue | `exchange.*` | risk → engine → ledgers; mass-cancel on kill |
| I/O | `csv_io.*`, `workload.*` | order CSV in, trades/reports/P&L CSV out; synthetic flow |
| concurrency | `spsc_queue.hpp`, `sharded_exchange.*`, `latency.hpp` | lock-free queue, symbol sharding, latency histogram |
| routing | `router.*` | consolidated multi-venue book, fee/size aware order splitting |
| simulation | `sim_types.hpp`, `flow.*`, `oms.*`, `strategies.*`, `simulator.*` | venues, network, OMS, strategies, report |

## 2. Matching rules (the contract)

* **Priority**: best price first; within a price, strictly first-in-first-out.
* **Trade price**: the *resting* order's price. An aggressor willing to pay more gets the improvement.
* **Order types**: Limit (GTC / IOC / FOK), Market (always IOC), post-only (GTC limit that is
  rejected rather than allowed to cross).
* **FOK** is decided by a dry run over the opposite side *before* any mutation (with self-trade
  prevention the dry run skips/stops at the client's own orders), so a refused FOK leaves the book untouched.
* **Cancel** is by id and checks ownership (`NOT_ORDER_OWNER`); an unknown id is `UNKNOWN_ORDER`.
  Cancels/amends that fail report `CANCEL_REJECTED` — a distinct, *non-terminal* report, so a client
  tracking order state can never mistake a refused cancel for the order dying.
* **Amend** (`MODIFY`): `price`, `qty` = new price, new *remaining* quantity. Same price and smaller
  or equal size → amended in place, **queue position kept**. Anything else → cancel/replace: goes to
  the back of the queue and may match immediately as an aggressor.
* **Self-trade prevention** (`BookConfig::stp`): `None` (allow), `CancelResting` (remove the client's
  own resting order and keep matching), `CancelTaker` (stop and drop the aggressor's remainder).
* **Validation**: qty ∈ (0, 10⁹]; limit price ∈ (0, 10⁹] ticks (so `price × qty` cannot overflow
  int64); duplicate *live* id rejected; post-only requires a GTC limit.
* **Reports** always carry `leaves` (remaining quantity after the event), so a consumer mirrors
  order state without bookkeeping. A New order produces `New`, then taker+maker fill reports per
  trade, then `Cancelled` for makers removed by STP, then `Expired` for an IOC/market remainder.

### Why this book design

`std::map<price, Level>` per side (bids keyed by `-price`, so `begin()` is the best price on both
sides and the matching loop is side-agnostic). Orders live in a pooled array addressed by `uint32`
index and form an intrusive FIFO list per level; each order keeps a pointer to its `Level`
(`std::map` nodes are stable), so **cancel is one hash lookup plus pointer surgery** — it never walks a
list or searches for the level (only erasing a level that just became empty costs O(log levels)). The id→slot index is a custom open-addressing table (`FlatIdMap`,
Fibonacci hashing, linear probing, backward-shift deletion so there are no tombstones). After
warm-up the hot path allocates nothing. The usual next optimisation is a flat array indexed by price
tick instead of the tree; it needs a bounded price range, so it was left out (and not measured).

## 3. Risk

`RiskEngine::check(order, now, replacing_leaves)` runs cheapest-first and returns the first
violated limit:

1. kill switch (client or global) · 2. symbol halt · 3. max order quantity · 4. max order notional ·
5. price collar vs reference (last trade, else book mid) · 6. rate limit (token bucket) ·
7. open-order count · 8. position limit · 9. gross exposure.

Design points worth defending:

* **Worst-case accounting.** Position and exposure are `position + all open buys + this order`
  (or minus all open sells). Open and *in-flight* quantity counts until a terminal report arrives,
  so the gate stays safe when fill reports lag — the situation in the simulator (venue latency)
  and in any decoupled pipeline. Risk-reducing orders are never blocked by the other side's open orders.
* **State mirrored from reports, not from intent.** Every `ExecReport` carries `leaves`; the engine
  keeps a tracked-order table and sets `remaining = leaves`. This also gives it the exact list of
  orders to cancel when a kill switch trips.
* **Cancels are never gated** (risk-reducing). A duplicate-id reject does not release the original
  order's exposure; a rejected cancel does not either.
* **Daily-loss kill switch** watches net P&L and is re-evaluated whenever *anyone's* trade moves a
  mark, not only on the client's own fills (a position can lose money while its owner is idle —
  found by a test, see EXPERIMENTS.md). When it trips, the client's resting orders are cancelled.
* Market orders are priced at the reference price for notional checks; with no reference yet,
  price-based checks are skipped (size and position checks still apply).

## 4. P&L

For each fill: `cash -= side × price × qty + fee`. Position changes by `±qty`.

* **Average cost**: adding to a position blends the average; reducing realizes
  `(fill − avg) × closed × direction`; crossing through zero realizes the closed part and opens the
  remainder at the fill price.
* **FIFO**: a queue of lots; reducing fills consume the oldest lot first.
* Identity, checked in tests for both methods under random flow with flips and fees:
  `cash + Σ qty×mark − initial_cash = realized + unrealized − fees`. Hence **total P&L is the same
  for both methods** — they only differ in the realized/unrealized split.
* Across all clients trading is zero-sum before fees (`Σ realized + unrealized = 0` at common marks),
  which the exchange fuzz test asserts.

## 5. Smart order router

For a parent order, from the latest depth snapshot of every venue:

1. flatten all visible contra-side levels into candidates;
2. price each *all-in*: buy `px·(1+taker_fee)`, sell `px·(1−taker_fee)`;
3. sort cheapest first; ties go to the lower-latency venue;
4. take greedily until the quantity is placed or the limit price is reached (greedy-by-cost is
   optimal here: each level's cost is independent);
5. one IOC child per venue, limited at the worst price planned there (+ optional slippage allowance,
   never past the parent's limit).

`route()` is pure and allocation-free (stack arrays). Two baselines exist for comparison:
`ProRata` (split by displayed size, price-blind) and `Primary` (one venue).

The **OMS** adds what a pure function cannot: the data is stale (quotes lag, our own earlier fills
are not yet reflected), so a round usually fills most of the parent; the OMS waits `retry_delay`,
re-reads the book, and routes the remainder (up to 4 rounds). Parent-level risk check, child-level
exposure tracking, per-strategy P&L attribution, and implementation-shortfall accounting live there.

## 6. Concurrency

**Single-writer principle.** An order book is mutated by exactly one thread. There is no lock and no
atomic on the matching path; parallelism comes from *partitioning*, not locking.

```
 producer 0 ─┐        SPSC[p][s]            ┌─► shard 0 (thread) ─► Exchange 0 (books for symbols ≡ 0 mod N)
 producer 1 ─┼─ shard = symbol % N ────────►├─► shard 1 (thread) ─► Exchange 1
 producer P ─┘                              └─► shard N-1        ─► Exchange N-1
```

* One SPSC queue per (producer, shard) pair keeps every queue single-producer/single-consumer; a
  shard polls its inbound queues round-robin.
* `SpscQueue`: power-of-two ring, `tail` release-stored by the producer / acquire-loaded by the
  consumer, symmetrically for `head`. Each side caches the other's index and only reloads the shared
  atomic when the cache says full/empty. Producer-owned and consumer-owned fields sit on separate
  cache lines. The test streams 2 M items through a 1 024-slot ring (constant full/empty
  transitions) under ThreadSanitizer.
* **Equivalence is tested**: with one producer, per-symbol fills are *identical* to the
  single-threaded exchange, including with tiny queues that force back-pressure. With several
  producers, cross-producer interleaving is non-deterministic (as in any real system), so the test
  asserts invariants instead: nothing lost, books valid, positions zero-sum, risk's open-order view
  equals what is resting.
* **What sharding costs you — said plainly.** Everything per *(client, symbol)* — size, notional,
  collar, position, open orders — is exact because that state lives in the symbol's shard. Limits
  that span symbols (rate limit, gross exposure, daily loss) become **per-shard approximations**.
  Making them exact needs a shared counter or a feedback loop between shards, which adds latency or
  contention on the hot path. A real system picks per limit; the usual answer is to keep
  position/size checks in-shard and put coarse global limits in a slower supervisory layer.
* **Determinism / recovery.** `Exchange` output is a pure function of the command stream. A write-ahead
  journal of commands plus replay is therefore a complete recovery story (tested via the
  determinism test and the golden files).

## 7. The simulator

A seeded event queue; simulated time moves only when an event is popped, so a 10 s run is
reproducible bit-for-bit and costs ~50 ms of CPU.

* **Market**: a random-walk fundamental; per-venue noise traders price around it plus a slowly
  mean-reverting venue-specific offset (so venues quote different prices); cancels rise with book
  size to hold depth; a zero-latency **arbitrageur** trades away strictly crossed cross-venue
  quotes. (Without it the venues drift apart and the consolidated book is crossed ~35–60% of the
  time — unrealistic, and it makes any router look good.)
* **Network**: per-venue one-way latency + uniform jitter on **FIFO channels** — jitter delays
  messages but never reorders them (like TCP / a sequenced feed).
* **Venues** have different taker fees, maker rebates, latencies, depth and quote noise.
* **Strategies** (through the `Oms` facade only): inventory-skewed post-only market maker on the
  best-rebate venue; random-size parent orders through the SOR; TWAP that makes up under-fills.
* **Not modelled** (so do not over-read the P&L): informed/toxic flow (the market maker never
  suffers adverse selection from better-informed traders), queue-position competition, market impact
  beyond the visible book, exchange fee tiers, order-to-trade limits.

## 8. How it is tested

| Technique | What it proves | Where |
|---|---|---|
| **Differential fuzz** vs a naive reference book | optimised book ≡ obviously-correct slow model over 360 000 random operations, all three STP modes | `test_order_book.cpp` |
| `validate()` after every op | no structural corruption (links, level sums, index, pool leaks, crossed book) | order book tests |
| Conservation / zero-sum / identity checks | quantities and money are neither created nor destroyed | book, portfolio, exchange, simulator tests |
| Golden files | the documented scenario reproduces byte-for-byte | `golden_sample_scenario` |
| Cross-implementation equivalence | sharded ≡ single-threaded; FlatIdMap ≡ `unordered_map` | concurrency / book tests |
| Property tests | e.g. smart routing is never costlier than the baselines when all fully fill | `test_router.cpp` |
| Sanitizers | ASan + UBSan + TSan clean on the whole suite | CI |
| **Mutation checks** (manual) | the tests actually fail when the code is broken — see EXPERIMENTS.md | — |

## 9. Walkthrough of `data/sample_orders.csv`

```
t=1000  SELL 100 @150.10 (c10)      asks: 150.10 [#1:100]
t=2000  SELL  50 @150.10 (c11)            150.10 [#1:100, #2:50]    ← same price, later: behind #1
t=3000  SELL 200 @150.20 (c12)            150.20 [#3:200]
t=4000  BUY  100 @149.90 (c13)      bids: 149.90 [#4:100]
t=5000  BUY  120 @150.10 (c14)      fills #1 100 and #2 20 at 150.10 (FIFO)  → trades 1,2
t=6000  BUY  MARKET 150 (c13)       #2's last 30 @150.10, then 120 of #3 @150.20 → trades 3,4
t=7000  CANCEL #4                   cancelled
t=8000  BUY FOK 500 @150.20 (c12)   only 80 available → REJECTED FOK_UNFILLABLE, book untouched
t=9000  SELL 100 @150.50 post-only  rests
t=10000 BUY 10 @150.50 post-only    would cross the 150.20 ask → REJECTED POST_ONLY_WOULD_CROSS
t=11000 BUY 40 @150.00 (c16)        rests
t=12000 MODIFY #10 → 150.20 ×40     reprice through the ask → trade 5 (40 @150.20)
t=13000 SELL IOC 10 @150.00         no bids → EXPIRED
t=14000 CANCEL #999                 CANCEL_REJECTED UNKNOWN_ORDER
t=15000 CANCEL #3 by c10            CANCEL_REJECTED NOT_ORDER_OWNER
t=16000 BUY qty 0                   REJECTED INVALID_QTY
t=17000 MSFT: SELL 10 @300.00, BUY 10 @300.00 → trade 1 (separate book)
```
