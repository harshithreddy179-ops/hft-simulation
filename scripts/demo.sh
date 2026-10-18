#!/usr/bin/env bash
# One-command tour of the project, in the order a presentation would show it.
#   scripts/demo.sh            build (Release), run tests, then every tool
#   QC_SKIP_BUILD=1 scripts/demo.sh   reuse an existing ./build
set -euo pipefail
cd "$(dirname "$0")/.."

if [[ -z "${QC_SKIP_BUILD:-}" ]]; then
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release > /dev/null
  cmake --build build -j > /dev/null
fi

banner() { printf '\n\033[1m== %s ==\033[0m\n' "$*"; }

banner "1. unit, property and fuzz tests"
./build/qc_tests | tail -1

banner "2. hand-checkable scenario (data/sample_orders.csv) -> trades"
./build/qc replay data/sample_orders.csv --out-dir out/sample --quiet
cat out/sample/trades.csv

banner "3. 500k-command replay: single thread vs 3 symbol shards (identical results)"
./build/qc gen --count 500000 --symbols 8 --clients 16 --seed 3 --out out/big.csv
./build/qc replay out/big.csv --stp resting --taker-fee-bps 0.3 --maker-fee-bps -0.1 --out-dir out/single | head -4
./build/qc replay out/big.csv --stp resting --taker-fee-bps 0.3 --maker-fee-bps -0.1 --out-dir out/sharded --threads 3 | head -3
diff <(sort out/single/trades.csv) <(sort out/sharded/trades.csv) && echo "trades.csv identical"

banner "4. pre-trade risk on the same flow: size, collar, position limits + per-client daily-loss kill switch"
./build/qc replay out/big.csv --max-order-qty 300 --max-position 2000 --collar-bps 150 --max-loss 500 | sed -n 1,14p

banner "5. multi-venue simulation: market maker + TWAP + smart order router"
./build/qc sim --seconds 10

banner "6. does smart routing actually help? (same market, only the router changes)"
./build/qc sim --seconds 30 --compare

banner "7. micro-benchmarks"
./build/qc_bench book --ops 1000000
./build/qc_bench risk --ops 1000000
