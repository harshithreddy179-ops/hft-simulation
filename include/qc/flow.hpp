#pragma once
// Background ("noise") order flow for one venue. Noise traders price their orders around a shared
// random-walk fundamental value plus a venue-specific, slowly mean-reverting offset, so venues
// quote slightly different prices (fragmentation) that a router can exploit.

#include <cstdint>
#include <utility>
#include <vector>

#include "qc/rng.hpp"
#include "qc/sim_types.hpp"
#include "qc/types.hpp"

namespace qc {

struct FlowConfig {
  double fundamental_start = 10000.0;  // ticks
  double vol_ticks_per_sqrt_s = 3.0;   // random-walk volatility of the fundamental
  Nanos fundamental_step = 1 * kMilli;
  double mean_offset_ticks = 2.5;      // passive orders sit this far (mean) behind the fundamental
  double market_frac = 0.10;
  double aggressive_frac = 0.10;       // limit orders priced through the fundamental (marketable)
  double mean_qty = 120.0;
  std::size_t target_resting = 80;     // cancel rate rises with the live count to hold depth near this
  std::size_t seed_levels = 12;        // initial ladder depth per side
};

class FlowGenerator {
 public:
  FlowGenerator(VenueId venue, const VenueConfig& vc, const FlowConfig& fc, std::uint64_t seed);

  Nanos next_gap();                               // exponential inter-arrival time
  void step_offset();                             // advance the venue's idiosyncratic quote offset
  Command next(Nanos now, double fundamental);    // next background command
  std::vector<Command> initial_book(Nanos ts, double fundamental);

 private:
  Order base_order(Nanos now);

  FlowConfig fc_;
  double rate_;
  double qty_scale_;
  double offset_amp_;
  double offset_ = 0.0;
  Rng rng_;
  OrderId next_id_;
  std::vector<std::pair<OrderId, ClientId>> live_;  // resting noise orders we may cancel later
};

}  // namespace qc
