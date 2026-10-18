#include "qc/workload.hpp"

#include <algorithm>
#include <cmath>

#include "qc/rng.hpp"

namespace qc {

namespace {
struct Live {
  OrderId id;
  ClientId client;
  SymbolId symbol;
  Side side;
  Price price;
};
}  // namespace

std::vector<Command> generate_workload(const WorkloadConfig& cfg) {
  Rng rng(cfg.seed);
  std::vector<double> mid(cfg.symbols, static_cast<double>(cfg.start_price));
  for (std::uint32_t s = 0; s < cfg.symbols; ++s) mid[s] += static_cast<double>(s) * 2500.0;  // distinct price levels

  std::vector<Command> out;
  out.reserve(cfg.count);
  std::vector<Live> live;
  OrderId next_id = 1;
  Nanos ts = 0;

  // Real flow cancels/amends orders it placed recently (stale ones have mostly traded), so pick a
  // target with an exponential bias towards the newest entries instead of uniformly.
  auto pick_recent = [&]() -> std::size_t {
    const auto back = static_cast<std::size_t>(rng.exponential(250.0));
    return live.size() - 1 - std::min(back, live.size() - 1);
  };

  auto qty = [&] {
    const auto q = static_cast<Qty>(1.0 + rng.exponential(cfg.mean_qty));
    return std::clamp<Qty>(q, 1, 5000);
  };

  while (out.size() < cfg.count) {
    ts += 1 + static_cast<Nanos>(rng.exponential(static_cast<double>(cfg.mean_gap_ns)));
    const SymbolId sym = static_cast<SymbolId>(rng.range(0, cfg.symbols - 1));
    mid[sym] = std::max(50.0, mid[sym] + rng.normal() * cfg.vol_ticks);

    const double dice = rng.uniform();
    Command c;
    c.order.ts = ts;
    c.order.symbol = sym;

    if (dice < cfg.cancel_frac && !live.empty()) {
      const std::size_t i = pick_recent();
      const Live l = live[i];
      live[i] = live.back();
      live.pop_back();
      c.type = CommandType::Cancel;
      c.order.id = l.id;
      c.order.symbol = l.symbol;
      c.order.client = rng.chance(0.02) ? static_cast<ClientId>(rng.range(1, cfg.clients)) : l.client;
      out.push_back(c);
      continue;
    }
    if (dice < cfg.cancel_frac + cfg.modify_frac && !live.empty()) {
      Live& l = live[pick_recent()];
      c.type = CommandType::Modify;
      c.order.id = l.id;
      c.order.symbol = l.symbol;
      c.order.client = l.client;
      c.order.price = std::max<Price>(1, l.price + rng.range(-2, 2));
      c.order.qty = qty();
      l.price = c.order.price;
      out.push_back(c);
      continue;
    }

    Order& o = c.order;
    o.id = next_id++;
    o.client = static_cast<ClientId>(rng.range(1, cfg.clients));
    o.side = rng.chance(0.5) ? Side::Buy : Side::Sell;
    o.qty = qty();
    const double kind = rng.uniform();
    if (kind < cfg.market_frac) {
      o.type = OrderType::Market;
      o.tif = TimeInForce::IOC;
    } else {
      double offset = rng.exponential(cfg.mean_offset_ticks);
      if (rng.chance(cfg.aggressive_frac)) offset = -rng.exponential(2.0);  // through the mid: marketable
      const double px = o.side == Side::Buy ? mid[sym] - offset : mid[sym] + offset;
      o.price = std::max<Price>(1, static_cast<Price>(std::llround(px)));
      o.tif = kind < cfg.market_frac + cfg.ioc_frac ? TimeInForce::IOC : TimeInForce::GTC;
    }
    if (rng.chance(cfg.invalid_frac)) {
      if (rng.chance(0.5)) o.qty = 0; else if (o.type == OrderType::Limit) o.price = -5;
    }
    c.type = CommandType::New;
    out.push_back(c);
    if (o.type == OrderType::Limit && o.tif == TimeInForce::GTC && o.qty > 0 && o.price > 0) {
      live.push_back(Live{o.id, o.client, o.symbol, o.side, o.price});
    }
  }
  return out;
}

}  // namespace qc
