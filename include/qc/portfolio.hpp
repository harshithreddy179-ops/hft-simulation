#pragma once
// Positions and P&L accounting.
//
// Money is in currency units (double); prices are ticks; quantities are signed (long > 0, short < 0).
//
// Two cost-basis methods are supported and they *must* agree on total P&L; they only differ in how
// P&L is split between realized and unrealized:
//   AverageCost : the open position carries one volume-weighted average price
//   Fifo        : the open position is a queue of lots; closing fills consume the oldest lot first
//
// Accounting identity (checked in the tests):
//   cash + sum(qty * mark)  -  initial_cash  ==  realized + unrealized - fees

#include <cstddef>
#include <deque>
#include <vector>

#include "qc/types.hpp"

namespace qc {

enum class CostBasis : std::uint8_t { AverageCost, Fifo };

// Latest mark price per symbol. 0 means "no mark yet" (positions then carry zero unrealized P&L).
class MarkTable {
 public:
  void set(SymbolId s, Price px) {
    if (s >= px_.size()) px_.resize(static_cast<std::size_t>(s) + 1, 0);
    px_[s] = px;
  }
  Price get(SymbolId s) const { return s < px_.size() ? px_[s] : 0; }

 private:
  std::vector<Price> px_;
};

class Position {
 public:
  explicit Position(CostBasis cb = CostBasis::AverageCost) : basis_(cb) {}

  // Apply a fill and return the realized P&L it produced (before fees).
  double apply_fill(Side side, Qty qty, Price px, double fee);

  Qty qty() const { return qty_; }
  double avg_cost() const;  // ticks; 0 when flat
  double realized() const { return realized_; }
  double fees() const { return fees_; }
  Qty volume() const { return volume_; }  // gross traded quantity
  std::size_t fills() const { return fills_; }
  double unrealized(Price mark) const;

 private:
  struct Lot {
    Qty qty;  // signed
    Price px;
  };
  CostBasis basis_;
  Qty qty_ = 0;
  double avg_cost_ticks_ = 0.0;  // AverageCost mode
  std::deque<Lot> lots_;         // Fifo mode
  double realized_ = 0.0;
  double fees_ = 0.0;
  Qty volume_ = 0;
  std::size_t fills_ = 0;
};

struct SymbolPnl {
  SymbolId symbol = 0;
  Qty qty = 0;
  double avg_cost = 0.0;  // currency units per share
  double mark = 0.0;      // currency units per share
  double realized = 0.0;
  double unrealized = 0.0;
  double fees = 0.0;
  double net() const { return realized + unrealized - fees; }
};

class Portfolio {
 public:
  explicit Portfolio(CostBasis cb = CostBasis::AverageCost, double initial_cash = 0.0)
      : basis_(cb), initial_cash_(initial_cash), cash_(initial_cash) {}

  // `fee` is positive for a cost and negative for a rebate.
  void on_fill(SymbolId s, Side side, Qty qty, Price px, double fee = 0.0);

  Qty position(SymbolId s) const { return s < positions_.size() ? positions_[s].qty() : 0; }
  const Position* find(SymbolId s) const { return s < positions_.size() && positions_[s].fills() ? &positions_[s] : nullptr; }

  double cash() const { return cash_; }
  double realized() const;
  double fees() const;
  double unrealized(const MarkTable& marks) const;
  double net_pnl(const MarkTable& marks) const { return realized() + unrealized(marks) - fees(); }
  double equity(const MarkTable& marks) const;
  double gross_exposure(const MarkTable& marks) const;  // sum |qty| * mark
  double net_exposure(const MarkTable& marks) const;    // sum qty * mark
  Qty volume() const;
  std::size_t fill_count() const;

  std::vector<SymbolPnl> breakdown(const MarkTable& marks) const;

  // Track peak equity / drawdown. Call periodically (e.g. after every fill or every N ms).
  void snapshot(const MarkTable& marks);
  double peak_equity() const { return peak_; }
  double max_drawdown() const { return max_dd_; }

  CostBasis basis() const { return basis_; }
  double initial_cash() const { return initial_cash_; }

 private:
  CostBasis basis_;
  double initial_cash_;
  double cash_;
  std::vector<Position> positions_;  // indexed by SymbolId
  double peak_ = 0.0;
  double max_dd_ = 0.0;
  bool have_peak_ = false;
};

}  // namespace qc
