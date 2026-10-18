#include "qc/flow.hpp"

#include <algorithm>
#include <cmath>

namespace qc {

FlowGenerator::FlowGenerator(VenueId venue, const VenueConfig& vc, const FlowConfig& fc, std::uint64_t seed)
    : fc_(fc),
      rate_(vc.flow_rate_per_sec),
      qty_scale_(vc.qty_scale),
      offset_amp_(vc.quote_noise_ticks),
      rng_(seed),
      next_id_(1'000'000'000'000ull + static_cast<OrderId>(venue) * 1'000'000'000ull) {}

Nanos FlowGenerator::next_gap() {
  return 1 + static_cast<Nanos>(rng_.exponential(1e9 / rate_));
}

void FlowGenerator::step_offset() {
  // Ornstein-Uhlenbeck: stationary std ~= offset_amp_, correlation time ~ 200 steps
  constexpr double theta = 0.005;
  offset_ += -theta * offset_ + offset_amp_ * std::sqrt(2.0 * theta) * rng_.normal();
}

Order FlowGenerator::base_order(Nanos now) {
  Order o;
  o.client = 1000 + static_cast<ClientId>(rng_.range(0, 49));
  o.symbol = 0;
  o.ts = now;
  return o;
}

Command FlowGenerator::next(Nanos now, double fundamental) {
  Command c;
  const double center = fundamental + offset_;
  const double p_cancel = std::clamp(0.5 * static_cast<double>(live_.size()) / static_cast<double>(fc_.target_resting), 0.05, 0.55);
  const double r = rng_.uniform();

  if (r < p_cancel && !live_.empty()) {
    const auto i = static_cast<std::size_t>(rng_.range(0, static_cast<std::int64_t>(live_.size()) - 1));
    const auto [id, client] = live_[i];
    live_[i] = live_.back();
    live_.pop_back();
    c.type = CommandType::Cancel;
    c.order = base_order(now);
    c.order.id = id;
    c.order.client = client;
    return c;
  }

  c.type = CommandType::New;
  Order& o = c.order;
  o = base_order(now);
  o.id = next_id_++;
  o.side = rng_.chance(0.5) ? Side::Buy : Side::Sell;
  o.qty = std::clamp<Qty>(static_cast<Qty>(1.0 + rng_.exponential(fc_.mean_qty * qty_scale_)), 1, 3000);

  if (r < p_cancel + fc_.market_frac) {
    o.type = OrderType::Market;
    o.tif = TimeInForce::IOC;
    return c;
  }

  double d = rng_.exponential(fc_.mean_offset_ticks);
  if (rng_.chance(fc_.aggressive_frac)) d = -rng_.exponential(1.5);  // through the fundamental
  const double px = o.side == Side::Buy ? std::floor(center - 0.5 - d) : std::ceil(center + 0.5 + d);
  o.price = std::max<Price>(1, static_cast<Price>(px));
  o.tif = TimeInForce::GTC;
  live_.emplace_back(o.id, o.client);
  return c;
}

std::vector<Command> FlowGenerator::initial_book(Nanos ts, double fundamental) {
  std::vector<Command> v;
  const Price bid0 = static_cast<Price>(std::floor(fundamental - 0.5));
  const Price ask0 = static_cast<Price>(std::ceil(fundamental + 0.5));
  for (std::size_t i = 0; i < fc_.seed_levels; ++i) {
    for (const Side side : {Side::Buy, Side::Sell}) {
      Command c;
      c.type = CommandType::New;
      c.order = base_order(ts);
      c.order.id = next_id_++;
      c.order.side = side;
      c.order.price = side == Side::Buy ? bid0 - static_cast<Price>(i) : ask0 + static_cast<Price>(i);
      c.order.qty = std::max<Qty>(1, static_cast<Qty>((50.0 + rng_.uniform() * 250.0) * qty_scale_));
      c.order.tif = TimeInForce::GTC;
      live_.emplace_back(c.order.id, c.order.client);
      v.push_back(c);
    }
  }
  return v;
}

}  // namespace qc
