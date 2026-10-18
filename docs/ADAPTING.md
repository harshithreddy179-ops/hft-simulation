# When the real problem statement drops (22 Oct)

A checklist, in order. The goal is to spend the first hour *deciding*, not typing.

## Hour 1 — read, map, decide

1. **Read the statement and the starter repo end to end** before touching code. Note the language,
   build system, test harness, and the exact interface the hidden tests call.
2. **Map every requirement to a module** using the table in
   [PROBLEM_STATEMENT_GUESS.md](PROBLEM_STATEMENT_GUESS.md). Mark each as *covered*, *covered with a
   different rule*, or *missing*. Be suspicious of "covered": re-read the rule, not just the title.
3. **Diff the rules** against the assumptions table in that file (trade price, market-order
   behaviour, amend semantics, STP, id reuse, fee model, mark price). Anything the statement fixes
   differently is a *one-line change plus a test*, not a redesign — change the test first.
4. **Look for what the statement emphasises** (scoring weights, time limits, input sizes). Optimise
   for that; do not polish the rest.

## Decide: reuse this code, port it, or both

| Starter repo is… | Do this |
|---|---|
| C++ and has its own interfaces | Keep `order_book`, `risk`, `portfolio`, `router` as-is and write thin adapters to the starter's interfaces. Do **not** rewrite the core to match naming. |
| Python / Java / Rust / Go | Port in this order: (1) `OrderBook` + the reference model + differential fuzz, (2) `Portfolio`, (3) `RiskEngine`, (4) `SmartOrderRouter`. The *test strategy* is the real asset — reference-model fuzz and the accounting identities port directly; the C++ is a specification you can run side by side. Run both implementations on the same CSV and diff the output. |
| A harness that feeds messages through callbacks / a socket | Keep the engine core; replace `csv_io` with the harness's parser. The `Exchange::process(Command, Output&)` signature is the seam. |

Port notes that save time:

* Use **integer ticks** for prices wherever the language allows; compare floats only for P&L.
* In a garbage-collected language the latency story changes: preallocate, reuse output buffers,
  avoid boxing in the hot path, and measure with the language's own benchmark harness. Say so in
  the presentation rather than claiming C++-like numbers.
* If you cannot match the C++ speed, you can still match the **correctness** story — and the
  differential-fuzz harness is the part judges rarely see done well.

## Days 2–4 — build the vertical slice

* Get the statement's own examples passing first (they are the likeliest hidden tests).
* Add each statement example to `data/` with its expected output, as a golden test.
* Keep `scripts/demo.sh` green at all times; extend it as features land.
* Re-run the sanitizers (`-DQC_SANITIZE=address`, `thread`) before every push once threads are involved.

## Before the 29 Oct submission

- [ ] `cmake -S . -B build && cmake --build build && ctest --test-dir build` passes from a clean clone.
- [ ] README states the language/toolchain, exact commands, and what is *not* done.
- [ ] No benchmark number in the README that you have not re-run on the final commit.
- [ ] Every assumption about an ambiguous rule is written down (copy the assumptions table).
- [ ] Repo contains no build artefacts (`.gitignore` covers `build*/`, `out/`).

## Before the 31 Oct presentation

- [ ] Walk through [PRESENTATION.md](PRESENTATION.md) with the real numbers.
- [ ] Do the demo from a **fresh clone on the presentation laptop** at least once (no network assumed).
- [ ] Prepare the "what would you do with more time?" and "what is wrong with this?" answers
      (EXPERIMENTS.md §4–5 is a template for being candid).
