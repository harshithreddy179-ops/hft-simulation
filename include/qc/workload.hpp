#pragma once
// Deterministic synthetic order-flow generator: random-walk mid price per symbol, passive and
// marketable limit orders, IOC/market takers, cancels (some for already-filled orders and a few for
// someone else's order, to exercise reject paths) and amends. Same seed => identical stream.

#include <cstdint>
#include <vector>

#include "qc/types.hpp"

namespace qc {

struct WorkloadConfig {
  std::uint64_t seed = 1;
  std::size_t count = 100'000;     // number of commands
  std::uint32_t symbols = 4;
  std::uint32_t clients = 8;
  Price start_price = 10000;       // ticks (100.00)
  double vol_ticks = 0.35;         // random-walk step per command (ticks, std dev)
  double mean_offset_ticks = 4.0;  // passive orders sit ~this far behind the mid
  double cancel_frac = 0.20;
  double modify_frac = 0.05;
  double market_frac = 0.03;
  double ioc_frac = 0.07;
  double aggressive_frac = 0.15;   // limit orders priced through the mid (marketable)
  double invalid_frac = 0.001;     // zero-qty / bad-price orders
  double mean_qty = 40.0;
  Nanos mean_gap_ns = 2'000;       // exponential inter-arrival
};

std::vector<Command> generate_workload(const WorkloadConfig& cfg);

}  // namespace qc
