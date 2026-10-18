#include "qc/router.hpp"

#include <algorithm>
#include <cmath>

namespace qc {

// ---------------------------------------------------------------------------------------------
// ConsolidatedBook
// ---------------------------------------------------------------------------------------------

void ConsolidatedBook::update(const DepthSnapshot& s, Nanos recv_ts) {
  if (s.symbol >= kMaxSymbols || s.venue >= kMaxVenues) return;
  if (s.symbol >= entries_.size()) entries_.resize(static_cast<std::size_t>(s.symbol) + 1);
  auto& row = entries_[s.symbol];
  if (row.empty()) row.resize(kMaxVenues);
  Entry& e = row[s.venue];
  if (e.valid && s.venue_ts < e.snap.venue_ts) return;  // older than what we hold (reordered delivery)
  e.snap = s;
  e.recv_ts = recv_ts;
  e.valid = true;
}

const ConsolidatedBook::Entry* ConsolidatedBook::entry(VenueId v, SymbolId s) const {
  if (s >= entries_.size() || v >= kMaxVenues) return nullptr;
  const auto& row = entries_[s];
  if (row.empty() || !row[v].valid) return nullptr;
  return &row[v];
}

const DepthSnapshot* ConsolidatedBook::snapshot(VenueId v, SymbolId s) const {
  const Entry* e = entry(v, s);
  return e ? &e->snap : nullptr;
}

Nanos ConsolidatedBook::received_at(VenueId v, SymbolId s) const {
  const Entry* e = entry(v, s);
  return e ? e->recv_ts : 0;
}

ConsolidatedBook::Best ConsolidatedBook::best_bid(SymbolId s) const {
  Best b;
  if (s >= entries_.size()) return b;
  for (const Entry& e : entries_[s]) {
    if (!e.valid || e.snap.n_bids == 0) continue;
    const LevelQuote& top = e.snap.bids[0];
    if (!b.valid || top.price > b.price) {
      b = Best{true, top.price, top.qty, e.snap.venue};
    } else if (top.price == b.price) {
      b.qty += top.qty;  // aggregate size at the national best price
    }
  }
  return b;
}

ConsolidatedBook::Best ConsolidatedBook::best_ask(SymbolId s) const {
  Best b;
  if (s >= entries_.size()) return b;
  for (const Entry& e : entries_[s]) {
    if (!e.valid || e.snap.n_asks == 0) continue;
    const LevelQuote& top = e.snap.asks[0];
    if (!b.valid || top.price < b.price) {
      b = Best{true, top.price, top.qty, e.snap.venue};
    } else if (top.price == b.price) {
      b.qty += top.qty;
    }
  }
  return b;
}

bool ConsolidatedBook::mid(SymbolId s, double& mid_ticks) const {
  const Best bid = best_bid(s), ask = best_ask(s);
  if (!bid.valid || !ask.valid) return false;
  mid_ticks = 0.5 * (static_cast<double>(bid.price) + static_cast<double>(ask.price));
  return true;
}

bool ConsolidatedBook::crossed(SymbolId s) const {
  const Best bid = best_bid(s), ask = best_ask(s);
  return bid.valid && ask.valid && bid.price >= ask.price;
}

// ---------------------------------------------------------------------------------------------
// SmartOrderRouter
// ---------------------------------------------------------------------------------------------

namespace {

struct Acc {
  Qty qty = 0;
  Price worst = 0;
  double cost = 0.0;      // fee-adjusted notional
  double raw = 0.0;       // raw notional
};

inline bool within_limit(Side side, Price limit, Price px) {
  return limit == 0 || (side == Side::Buy ? px <= limit : px >= limit);
}

inline double fee_multiplier(Side side, double taker_bps) {
  return side == Side::Buy ? 1.0 + taker_bps * 1e-4 : 1.0 - taker_bps * 1e-4;
}

// Displayed size discounted for age + latency. Never rounds up.
inline Qty visible_qty(Qty q, double haircut) {
  if (haircut >= 1.0) return q;
  return static_cast<Qty>(std::floor(static_cast<double>(q) * haircut));
}

inline double haircut_for(const SorConfig& cfg, const VenueInfo& v, const DepthSnapshot& snap, Nanos now) {
  if (cfg.stale_decay_ns <= 0.0) return 1.0;
  const double age = static_cast<double>(std::max<Nanos>(0, now - snap.venue_ts));
  return std::exp(-(age + static_cast<double>(v.latency_in_ns)) / cfg.stale_decay_ns);
}

inline const LevelQuote* contra_levels(const DepthSnapshot& s, Side taker, std::size_t& n) {
  if (taker == Side::Buy) {
    n = s.n_asks;
    return s.asks.data();
  }
  n = s.n_bids;
  return s.bids.data();
}

// Consume up to `want` shares from one venue's visible levels (best first, within the limit).
void take_from_venue(const DepthSnapshot& snap, const VenueInfo& v, const SorConfig& cfg, const RouteRequest& rq,
                     Qty want, Acc& a) {
  std::size_t n = 0;
  const LevelQuote* lv = contra_levels(snap, rq.side, n);
  const double hc = haircut_for(cfg, v, snap, rq.now);
  const double fm = fee_multiplier(rq.side, v.taker_fee_bps);
  for (std::size_t i = 0; i < n && want > 0; ++i) {
    if (!within_limit(rq.side, rq.limit, lv[i].price)) break;  // levels only get worse from here
    const Qty q = std::min(want, visible_qty(lv[i].qty, hc));
    if (q <= 0) continue;
    a.qty += q;
    a.worst = lv[i].price;
    a.raw += static_cast<double>(q) * static_cast<double>(lv[i].price);
    a.cost += static_cast<double>(q) * static_cast<double>(lv[i].price) * fm;
    want -= q;
  }
}

// Sum of visible size within the limit (ProRata sizing).
Qty available(const DepthSnapshot& snap, const VenueInfo& v, const SorConfig& cfg, const RouteRequest& rq) {
  std::size_t n = 0;
  const LevelQuote* lv = contra_levels(snap, rq.side, n);
  const double hc = haircut_for(cfg, v, snap, rq.now);
  Qty total = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (!within_limit(rq.side, rq.limit, lv[i].price)) break;
    total += visible_qty(lv[i].qty, hc);
  }
  return total;
}

RoutePlan finalize(const RouteRequest& rq, const SorConfig& cfg, const std::vector<VenueInfo>& venues, const Acc* acc) {
  RoutePlan plan;
  for (std::size_t vi = 0; vi < venues.size() && vi < kMaxVenues; ++vi) {
    const Acc& a = acc[vi];
    if (a.qty <= 0 || a.qty < cfg.min_child_qty) continue;
    ChildPlan& c = plan.child[plan.n_children++];
    c.venue = venues[vi].id;
    c.qty = a.qty;
    Price lim = rq.side == Side::Buy ? a.worst + cfg.slippage_ticks : a.worst - cfg.slippage_ticks;
    if (rq.limit > 0) lim = rq.side == Side::Buy ? std::min(lim, rq.limit) : std::max(lim, rq.limit);
    c.limit = std::max<Price>(lim, 1);
    c.est_cost = a.cost;
    c.est_avg_px = a.raw / static_cast<double>(a.qty);
    plan.planned += a.qty;
    plan.est_cost += a.cost;
  }
  plan.unplanned = std::max<Qty>(0, rq.qty - plan.planned);
  return plan;
}

}  // namespace

SmartOrderRouter::SmartOrderRouter(std::vector<VenueInfo> venues, SorConfig cfg) : venues_(std::move(venues)), cfg_(cfg) {
  if (venues_.size() > kMaxVenues) venues_.resize(kMaxVenues);
}

RoutePlan SmartOrderRouter::route(const RouteRequest& rq, const ConsolidatedBook& book) const {
  if (rq.qty <= 0) return RoutePlan{};
  switch (cfg_.policy) {
    case RoutingPolicy::BestPrice: return route_best_price(rq, book);
    case RoutingPolicy::ProRata: return route_pro_rata(rq, book);
    case RoutingPolicy::Primary: return route_primary(rq, book);
  }
  return RoutePlan{};
}

RoutePlan SmartOrderRouter::route_best_price(const RouteRequest& rq, const ConsolidatedBook& book) const {
  struct Cand {
    double eff;
    Price px;
    Qty qty;
    Nanos latency;
    std::uint8_t vi;
  };
  std::array<Cand, kMaxVenues * kDepthLevels> cands;
  std::size_t nc = 0;

  for (std::size_t vi = 0; vi < venues_.size(); ++vi) {
    const VenueInfo& v = venues_[vi];
    const DepthSnapshot* snap = book.snapshot(v.id, rq.symbol);
    if (!snap) continue;
    std::size_t n = 0;
    const LevelQuote* lv = contra_levels(*snap, rq.side, n);
    const double hc = haircut_for(cfg_, v, *snap, rq.now);
    const double fm = fee_multiplier(rq.side, v.taker_fee_bps);
    for (std::size_t i = 0; i < n; ++i) {
      if (!within_limit(rq.side, rq.limit, lv[i].price)) break;
      const Qty q = visible_qty(lv[i].qty, hc);
      if (q <= 0) continue;
      cands[nc++] = Cand{static_cast<double>(lv[i].price) * fm, lv[i].price, q, v.latency_in_ns, static_cast<std::uint8_t>(vi)};
    }
  }

  // cheapest all-in first (for a sell: highest net proceeds first); ties -> faster venue -> lower index
  const bool buy = rq.side == Side::Buy;
  const auto better = [buy](const Cand& a, const Cand& b) {
    if (a.eff != b.eff) return buy ? a.eff < b.eff : a.eff > b.eff;
    if (a.latency != b.latency) return a.latency < b.latency;
    if (a.vi != b.vi) return a.vi < b.vi;
    return buy ? a.px < b.px : a.px > b.px;
  };
  for (std::size_t i = 1; i < nc; ++i) {  // insertion sort: <= 40 elements, no allocation
    const Cand key = cands[i];
    std::size_t j = i;
    while (j > 0 && better(key, cands[j - 1])) {
      cands[j] = cands[j - 1];
      --j;
    }
    cands[j] = key;
  }

  std::array<Acc, kMaxVenues> acc{};
  Qty remaining = rq.qty;
  for (std::size_t i = 0; i < nc && remaining > 0; ++i) {
    const Cand& c = cands[i];
    const Qty take = std::min(remaining, c.qty);
    Acc& a = acc[c.vi];
    a.qty += take;
    a.worst = c.px;  // candidates of one venue arrive best-first, so the last one taken is its worst
    a.raw += static_cast<double>(take) * static_cast<double>(c.px);
    a.cost += static_cast<double>(take) * c.eff;
    remaining -= take;
  }
  return finalize(rq, cfg_, venues_, acc.data());
}

RoutePlan SmartOrderRouter::route_pro_rata(const RouteRequest& rq, const ConsolidatedBook& book) const {
  std::array<Qty, kMaxVenues> avail{};
  Qty total = 0;
  for (std::size_t vi = 0; vi < venues_.size(); ++vi) {
    if (const DepthSnapshot* snap = book.snapshot(venues_[vi].id, rq.symbol)) {
      avail[vi] = available(*snap, venues_[vi], cfg_, rq);
      total += avail[vi];
    }
  }
  if (total <= 0) {
    const std::array<Acc, kMaxVenues> none{};
    return finalize(rq, cfg_, venues_, none.data());  // nothing visible: everything is unplanned
  }

  const Qty target = std::min(rq.qty, total);
  std::array<Qty, kMaxVenues> alloc{};
  Qty allocated = 0;
  for (std::size_t vi = 0; vi < venues_.size(); ++vi) {
    alloc[vi] = std::min(avail[vi], static_cast<Qty>(static_cast<double>(target) * static_cast<double>(avail[vi]) / static_cast<double>(total)));
    allocated += alloc[vi];
  }
  for (std::size_t vi = 0; allocated < target; vi = (vi + 1) % venues_.size()) {  // hand out rounding leftovers
    if (alloc[vi] < avail[vi]) {
      ++alloc[vi];
      ++allocated;
    }
  }

  std::array<Acc, kMaxVenues> acc{};
  for (std::size_t vi = 0; vi < venues_.size(); ++vi) {
    if (alloc[vi] <= 0) continue;
    if (const DepthSnapshot* snap = book.snapshot(venues_[vi].id, rq.symbol)) {
      take_from_venue(*snap, venues_[vi], cfg_, rq, alloc[vi], acc[vi]);
    }
  }
  return finalize(rq, cfg_, venues_, acc.data());
}

RoutePlan SmartOrderRouter::route_primary(const RouteRequest& rq, const ConsolidatedBook& book) const {
  std::array<Acc, kMaxVenues> acc{};
  for (std::size_t vi = 0; vi < venues_.size(); ++vi) {
    if (venues_[vi].id != cfg_.primary) continue;
    if (const DepthSnapshot* snap = book.snapshot(venues_[vi].id, rq.symbol)) {
      take_from_venue(*snap, venues_[vi], cfg_, rq, rq.qty, acc[vi]);
    }
  }
  return finalize(rq, cfg_, venues_, acc.data());
}

}  // namespace qc
