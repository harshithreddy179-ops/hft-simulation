#include "qc/portfolio.hpp"

#include <algorithm>
#include <cmath>

namespace qc {

namespace {
inline Qty abs_qty(Qty q) { return q < 0 ? -q : q; }
}  // namespace

double Position::apply_fill(Side side, Qty qty, Price px, double fee) {
  const Qty signed_qty = side == Side::Buy ? qty : -qty;
  const bool reducing = qty_ != 0 && ((qty_ > 0) != (signed_qty > 0));
  double realized_now = 0.0;

  if (basis_ == CostBasis::AverageCost) {
    if (!reducing) {
      // opening or adding: blend into the average
      const double old_abs = static_cast<double>(abs_qty(qty_));
      const double new_abs = old_abs + static_cast<double>(qty);
      avg_cost_ticks_ = (avg_cost_ticks_ * old_abs + static_cast<double>(px) * static_cast<double>(qty)) / new_abs;
      qty_ += signed_qty;
    } else {
      const Qty closing = std::min(qty, abs_qty(qty_));
      const double dir = qty_ > 0 ? 1.0 : -1.0;  // long profits when price rises
      realized_now = dir * (static_cast<double>(px) - avg_cost_ticks_) * static_cast<double>(closing) / kTicksPerUnit;
      qty_ += signed_qty;
      if (qty_ == 0) {
        avg_cost_ticks_ = 0.0;
      } else if ((qty_ > 0) == (signed_qty > 0)) {
        avg_cost_ticks_ = static_cast<double>(px);  // flipped through zero: remainder opened at this price
      }
    }
  } else {
    Qty remaining = qty;
    if (reducing) {
      const double dir = qty_ > 0 ? 1.0 : -1.0;
      while (remaining > 0 && !lots_.empty()) {
        Lot& lot = lots_.front();
        const Qty take = std::min(remaining, abs_qty(lot.qty));
        realized_now += dir * (static_cast<double>(px) - static_cast<double>(lot.px)) * static_cast<double>(take) / kTicksPerUnit;
        lot.qty += lot.qty > 0 ? -take : take;
        remaining -= take;
        if (lot.qty == 0) lots_.pop_front();
      }
    }
    if (remaining > 0) lots_.push_back(Lot{side == Side::Buy ? remaining : -remaining, px});
    qty_ += signed_qty;
  }

  realized_ += realized_now;
  fees_ += fee;
  volume_ += qty;
  ++fills_;
  return realized_now;
}

double Position::avg_cost() const {
  if (qty_ == 0) return 0.0;
  if (basis_ == CostBasis::AverageCost) return avg_cost_ticks_;
  double notional_ticks = 0.0;
  for (const Lot& l : lots_) notional_ticks += static_cast<double>(abs_qty(l.qty)) * static_cast<double>(l.px);
  return notional_ticks / static_cast<double>(abs_qty(qty_));
}

double Position::unrealized(Price mark) const {
  if (qty_ == 0 || mark == 0) return 0.0;
  if (basis_ == CostBasis::AverageCost) {
    return (static_cast<double>(mark) - avg_cost_ticks_) * static_cast<double>(qty_) / kTicksPerUnit;
  }
  double u = 0.0;
  for (const Lot& l : lots_) u += (static_cast<double>(mark) - static_cast<double>(l.px)) * static_cast<double>(l.qty);
  return u / kTicksPerUnit;
}

void Portfolio::on_fill(SymbolId s, Side side, Qty qty, Price px, double fee) {
  if (s >= positions_.size()) positions_.resize(static_cast<std::size_t>(s) + 1, Position(basis_));
  positions_[s].apply_fill(side, qty, px, fee);
  // buying spends cash, selling receives it; fees always reduce cash (rebates add)
  cash_ -= static_cast<double>(sign(side)) * notional(px, qty);
  cash_ -= fee;
}

double Portfolio::realized() const {
  double r = 0.0;
  for (const auto& p : positions_) r += p.realized();
  return r;
}
double Portfolio::fees() const {
  double f = 0.0;
  for (const auto& p : positions_) f += p.fees();
  return f;
}
double Portfolio::unrealized(const MarkTable& marks) const {
  double u = 0.0;
  for (std::size_t s = 0; s < positions_.size(); ++s) u += positions_[s].unrealized(marks.get(static_cast<SymbolId>(s)));
  return u;
}
double Portfolio::equity(const MarkTable& marks) const {
  double e = cash_;
  for (std::size_t s = 0; s < positions_.size(); ++s) {
    e += notional(marks.get(static_cast<SymbolId>(s)), positions_[s].qty());
  }
  return e;
}
double Portfolio::gross_exposure(const MarkTable& marks) const {
  double g = 0.0;
  for (std::size_t s = 0; s < positions_.size(); ++s) {
    g += notional(marks.get(static_cast<SymbolId>(s)), abs_qty(positions_[s].qty()));
  }
  return g;
}
double Portfolio::net_exposure(const MarkTable& marks) const {
  double n = 0.0;
  for (std::size_t s = 0; s < positions_.size(); ++s) {
    n += notional(marks.get(static_cast<SymbolId>(s)), positions_[s].qty());
  }
  return n;
}
Qty Portfolio::volume() const {
  Qty v = 0;
  for (const auto& p : positions_) v += p.volume();
  return v;
}
std::size_t Portfolio::fill_count() const {
  std::size_t n = 0;
  for (const auto& p : positions_) n += p.fills();
  return n;
}

std::vector<SymbolPnl> Portfolio::breakdown(const MarkTable& marks) const {
  std::vector<SymbolPnl> out;
  for (std::size_t s = 0; s < positions_.size(); ++s) {
    const Position& p = positions_[s];
    if (p.fills() == 0) continue;
    SymbolPnl r;
    r.symbol = static_cast<SymbolId>(s);
    r.qty = p.qty();
    r.avg_cost = p.avg_cost() / kTicksPerUnit;
    r.mark = to_units(marks.get(r.symbol));
    r.realized = p.realized();
    r.unrealized = p.unrealized(marks.get(r.symbol));
    r.fees = p.fees();
    out.push_back(r);
  }
  return out;
}

void Portfolio::snapshot(const MarkTable& marks) {
  const double e = equity(marks);
  if (!have_peak_ || e > peak_) {
    peak_ = e;
    have_peak_ = true;
  }
  max_dd_ = std::max(max_dd_, peak_ - e);
}

}  // namespace qc
