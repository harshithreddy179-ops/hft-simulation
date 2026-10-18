#pragma once
// Multi-symbol matching engine: routes commands to per-symbol order books and turns book
// results into FIX-style execution reports (the "venue" side of the system).

#include <memory>
#include <vector>

#include "qc/events.hpp"
#include "qc/order_book.hpp"
#include "qc/types.hpp"

namespace qc {

struct EngineOutput {
  std::vector<ExecReport> reports;  // one stream per event, for every client involved
  std::vector<Fill> fills;          // the trade tape
  void clear() {
    reports.clear();
    fills.clear();
  }
};

class MatchingEngine {
 public:
  explicit MatchingEngine(VenueId venue = 0, BookConfig cfg = {});

  // Handle one inbound command. Appends to `out` (callers reuse `out` to avoid allocation).
  //
  // Report sequence for a New order:  New, then for each trade a Fill/PartialFill for the taker
  // and for the maker, then Cancelled for makers removed by self-trade prevention, then Expired
  // if an IOC/market/STP remainder was dropped. Rejected replaces the whole sequence.
  void process(const Command& cmd, EngineOutput& out);

  OrderBook* book(SymbolId symbol);
  const OrderBook* book(SymbolId symbol) const;
  VenueId venue() const { return venue_; }

 private:
  OrderBook& get_or_create(SymbolId symbol);
  ExecReport make_report(const Order& o, Side side, ExecType type, Qty leaves) const;
  void emit_book_events(const Order& cmd_order, const SubmitResult* res, EngineOutput& out);

  VenueId venue_;
  BookConfig cfg_;
  std::vector<std::unique_ptr<OrderBook>> books_;  // indexed by SymbolId
  BookEvents scratch_;
};

}  // namespace qc
