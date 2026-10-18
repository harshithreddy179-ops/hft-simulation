#include "qc/strategies.hpp"

#include <algorithm>
#include <cmath>

namespace qc {

// =============================================================================================
// MarketMaker
// =============================================================================================

void MarketMaker::on_start(Oms& oms) { oms.set_timer(id, cfg_.interval, 0); }

void MarketMaker::on_timer(Oms& oms, std::uint32_t) {
  oms.set_timer(id, cfg_.interval, 0);

  const ConsolidatedBook& book = oms.book();
  double mid = 0.0;
  if (!book.mid(symbol_, mid) || book.crossed(symbol_)) return;  // no view, or the market is dislocated: stand aside

  const Qty inv = oms.position(id, symbol_);
  const double skew = -static_cast<double>(inv) / 100.0 * cfg_.skew_ticks_per_100;
  Price bid = static_cast<Price>(std::floor(mid - cfg_.half_spread_ticks + skew));
  Price ask = static_cast<Price>(std::ceil(mid + cfg_.half_spread_ticks + skew));
  if (ask <= bid) ask = bid + 1;

  update_side(oms, Side::Buy, bid, inv + cfg_.quote_size <= cfg_.max_inventory);
  update_side(oms, Side::Sell, ask, inv - cfg_.quote_size >= -cfg_.max_inventory);
}

void MarketMaker::update_side(Oms& oms, Side side, Price desired, bool allowed) {
  Quote& q = side == Side::Buy ? bid_ : ask_;
  if (!allowed) {
    if (q.id != 0 && !q.cancel_pending) {
      q.cancel_pending = oms.cancel(q.id);
    }
    return;
  }
  if (q.id == 0) {
    OrderRequest req;
    req.strategy = id;
    req.symbol = symbol_;
    req.side = side;
    req.qty = cfg_.quote_size;
    req.limit = std::max<Price>(1, desired);
    req.algo = Algo::Direct;
    req.venue = cfg_.venue;
    req.tif = TimeInForce::GTC;
    req.post_only = true;
    q.id = oms.send(req);
    if (q.id != 0) {
      q.px = req.limit;
      q.amend_pending = q.cancel_pending = false;
      ++quotes_sent_;
    }
    return;
  }
  if (q.amend_pending || q.cancel_pending) return;
  if (std::llabs(q.px - desired) >= cfg_.requote_threshold) {
    if (oms.modify(q.id, std::max<Price>(1, desired), cfg_.quote_size)) {
      q.amend_pending = true;
      q.target_px = std::max<Price>(1, desired);
      ++amends_sent_;
    }
  }
}

void MarketMaker::on_exec(Oms&, const ExecReport& r) {
  if (r.order_id == 0) return;
  Quote* matched = nullptr;
  if (r.order_id == bid_.id) matched = &bid_;
  else if (r.order_id == ask_.id) matched = &ask_;
  if (matched == nullptr) return;
  Quote& q = *matched;

  if (r.type == ExecType::Replaced) {
    q.amend_pending = false;
    q.px = q.target_px;
  } else if (r.type == ExecType::CancelRejected) {
    q.amend_pending = false;
    q.cancel_pending = false;
  }
  if (r.is_terminal()) {  // filled, cancelled, or refused (e.g. post-only would cross): quote again next tick
    q.id = 0;
    q.px = q.target_px = 0;
    q.amend_pending = q.cancel_pending = false;
  }
}

// =============================================================================================
// ParentFlow
// =============================================================================================

void ParentFlow::on_start(Oms& oms) {
  oms.set_timer(id, static_cast<Nanos>(rng_.exponential(cfg_.mean_interval_ms * 1e6)) + 1 * kSecond / 2, 0);
}

void ParentFlow::on_timer(Oms& oms, std::uint32_t) {
  // draw everything first so the random stream does not depend on market state
  const bool buy = rng_.chance(0.5);
  const double u = rng_.uniform();
  const Nanos gap = 1 + static_cast<Nanos>(rng_.exponential(cfg_.mean_interval_ms * 1e6));
  oms.set_timer(id, gap, 0);

  const Qty qty = static_cast<Qty>(std::exp(std::log(static_cast<double>(cfg_.min_qty)) +
                                            u * (std::log(static_cast<double>(cfg_.max_qty)) - std::log(static_cast<double>(cfg_.min_qty)))));
  const ConsolidatedBook& book = oms.book();
  const auto touch = buy ? book.best_ask(symbol_) : book.best_bid(symbol_);
  if (!touch.valid) return;

  OrderRequest req;
  req.strategy = id;
  req.symbol = symbol_;
  req.side = buy ? Side::Buy : Side::Sell;
  req.qty = qty;
  req.limit = buy ? touch.price + cfg_.limit_ticks : touch.price - cfg_.limit_ticks;
  req.algo = Algo::Sor;
  if (oms.send(req) != 0) ++sent_;
}

// =============================================================================================
// TWAP
// =============================================================================================

void TwapExecutor::on_start(Oms& oms) { oms.set_timer(id, cfg_.start, 0); }

void TwapExecutor::on_timer(Oms& oms, std::uint32_t) {
  if (slice_ >= cfg_.slices) return;
  const int left = cfg_.slices - slice_;
  const Qty todo = cfg_.total_qty - sent_qty_;
  const Qty qty = std::max<Qty>(1, (todo + left - 1) / left);
  ++slice_;
  oms.set_timer(id, cfg_.duration / cfg_.slices, 0);

  const ConsolidatedBook& book = oms.book();
  double mid = 0.0;
  const bool buy = cfg_.side == Side::Buy;
  const auto touch = buy ? book.best_ask(symbol_) : book.best_bid(symbol_);
  if (!book.mid(symbol_, mid) || !touch.valid) return;  // no market: this slice is skipped, later ones catch up
  if (arrival_mid_ == 0.0) arrival_mid_ = mid;
  mid_sum_ += mid;
  ++mid_samples_;

  OrderRequest req;
  req.strategy = id;
  req.symbol = symbol_;
  req.side = cfg_.side;
  req.qty = std::min(qty, todo);
  req.limit = buy ? touch.price + cfg_.limit_ticks : touch.price - cfg_.limit_ticks;
  req.algo = Algo::Sor;
  if (oms.send(req) != 0) {
    sent_qty_ += req.qty;
    ++slices_sent_;
  }
}

void TwapExecutor::on_parent_done(Oms&, const ParentResult& r) {
  filled_ += r.filled;
  notional_ticks_ += r.avg_px_ticks * static_cast<double>(r.filled);
  fees_ += r.fees;
  sent_qty_ -= (r.qty - r.filled);  // unfilled part of a slice goes back into the pool for later slices
}

TwapSummary TwapExecutor::summary() const {
  TwapSummary s;
  s.target = cfg_.total_qty;
  s.filled = filled_;
  s.avg_px_ticks = filled_ > 0 ? notional_ticks_ / static_cast<double>(filled_) : 0.0;
  s.arrival_mid_ticks = arrival_mid_;
  s.interval_mid_ticks = mid_samples_ > 0 ? mid_sum_ / mid_samples_ : 0.0;
  const double dir = cfg_.side == Side::Buy ? 1.0 : -1.0;
  if (filled_ > 0 && arrival_mid_ > 0) s.vs_arrival_bps = dir * (s.avg_px_ticks - arrival_mid_) / arrival_mid_ * 1e4;
  if (filled_ > 0 && s.interval_mid_ticks > 0) s.vs_twap_bps = dir * (s.avg_px_ticks - s.interval_mid_ticks) / s.interval_mid_ticks * 1e4;
  s.fees = fees_;
  s.slices_sent = slices_sent_;
  return s;
}

}  // namespace qc
