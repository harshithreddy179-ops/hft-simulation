#include "qc/engine.hpp"

namespace qc {

MatchingEngine::MatchingEngine(VenueId venue, BookConfig cfg) : venue_(venue), cfg_(cfg) {}

OrderBook* MatchingEngine::book(SymbolId symbol) {
  return symbol < books_.size() ? books_[symbol].get() : nullptr;
}
const OrderBook* MatchingEngine::book(SymbolId symbol) const {
  return symbol < books_.size() ? books_[symbol].get() : nullptr;
}

OrderBook& MatchingEngine::get_or_create(SymbolId symbol) {
  if (symbol >= books_.size()) books_.resize(static_cast<std::size_t>(symbol) + 1);
  if (!books_[symbol]) books_[symbol] = std::make_unique<OrderBook>(symbol, cfg_);
  return *books_[symbol];
}

ExecReport MatchingEngine::make_report(const Order& o, Side side, ExecType type, Qty leaves) const {
  ExecReport r;
  r.ts = o.ts;
  r.order_id = o.id;
  r.client = o.client;
  r.symbol = o.symbol;
  r.venue = venue_;
  r.side = side;
  r.type = type;
  r.leaves = leaves;
  return r;
}

// Translate what the book did (fills, removals, final status) into reports.
void MatchingEngine::emit_book_events(const Order& o, const SubmitResult* res, EngineOutput& out) {
  for (const Fill& f : scratch_.fills) {
    out.fills.push_back(f);

    ExecReport taker;
    taker.ts = f.ts;
    taker.order_id = f.taker_order;
    taker.client = f.taker_client;
    taker.symbol = f.symbol;
    taker.venue = venue_;
    taker.side = f.taker_side;
    taker.type = f.taker_leaves == 0 ? ExecType::Fill : ExecType::PartialFill;
    taker.liquidity = Liquidity::Taker;
    taker.last_px = f.price;
    taker.last_qty = f.qty;
    taker.leaves = f.taker_leaves;
    out.reports.push_back(taker);

    ExecReport maker = taker;
    maker.order_id = f.maker_order;
    maker.client = f.maker_client;
    maker.side = opposite(f.taker_side);
    maker.type = f.maker_leaves == 0 ? ExecType::Fill : ExecType::PartialFill;
    maker.liquidity = Liquidity::Maker;
    maker.leaves = f.maker_leaves;
    out.reports.push_back(maker);
  }

  for (const Removal& rm : scratch_.removals) {
    if (rm.reason == RemovalReason::Replaced) continue;  // reported as Replaced by the caller
    ExecReport r;
    r.ts = o.ts;
    r.order_id = rm.id;
    r.client = rm.client;
    r.symbol = rm.symbol;
    r.venue = venue_;
    r.side = rm.side;
    r.type = ExecType::Cancelled;
    r.leaves = 0;
    out.reports.push_back(r);
  }

  if (res && res->status == OrderStatus::Expired) {
    out.reports.push_back(make_report(o, o.side, ExecType::Expired, 0));
  }
}

void MatchingEngine::process(const Command& cmd, EngineOutput& out) {
  const Order& o = cmd.order;
  scratch_.clear();

  auto reject = [&](ExecType type, RejectReason why, Qty leaves) {
    ExecReport r = make_report(o, o.side, type, leaves);
    r.reason = why;
    out.reports.push_back(r);
  };

  if (o.symbol >= kMaxSymbols) {
    reject(cmd.type == CommandType::New ? ExecType::Rejected : ExecType::CancelRejected,
           RejectReason::UnknownSymbol, 0);
    return;
  }

  switch (cmd.type) {
    case CommandType::New: {
      OrderBook& bk = get_or_create(o.symbol);
      const SubmitResult res = bk.submit(o, scratch_);
      if (res.status == OrderStatus::Rejected) {
        reject(ExecType::Rejected, res.reason, 0);
        return;
      }
      out.reports.push_back(make_report(o, o.side, ExecType::New, o.qty));
      emit_book_events(o, &res, out);
      return;
    }

    case CommandType::Cancel: {
      OrderBook* bk = book(o.symbol);
      const auto info = bk ? bk->find(o.id) : std::nullopt;
      if (!info) {
        reject(ExecType::CancelRejected, RejectReason::UnknownOrder, 0);
        return;
      }
      if (info->client != o.client) {
        ExecReport r = make_report(o, info->side, ExecType::CancelRejected, info->leaves);
        r.reason = RejectReason::NotOrderOwner;
        out.reports.push_back(r);
        return;
      }
      bk->cancel(o.id, scratch_);
      emit_book_events(o, nullptr, out);
      return;
    }

    case CommandType::Modify: {
      OrderBook* bk = book(o.symbol);
      const auto info = bk ? bk->find(o.id) : std::nullopt;
      if (!info) {
        reject(ExecType::CancelRejected, RejectReason::UnknownOrder, 0);
        return;
      }
      if (info->client != o.client) {
        ExecReport r = make_report(o, info->side, ExecType::CancelRejected, info->leaves);
        r.reason = RejectReason::NotOrderOwner;
        out.reports.push_back(r);
        return;
      }
      const SubmitResult res = bk->modify(o.id, o.price, o.qty, o.ts, scratch_);
      if (res.status == OrderStatus::Rejected) {
        ExecReport r = make_report(o, info->side, ExecType::CancelRejected, info->leaves);
        r.reason = res.reason;
        out.reports.push_back(r);
        return;
      }
      out.reports.push_back(make_report(o, info->side, ExecType::Replaced, o.qty));
      emit_book_events(o, &res, out);
      return;
    }
  }
}

}  // namespace qc
