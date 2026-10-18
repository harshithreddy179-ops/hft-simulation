#include "qc/exchange.hpp"

namespace qc {

Exchange::Exchange(ExchangeConfig cfg)
    : cfg_(cfg), engine_(cfg.venue, BookConfig{cfg.stp, 4096}), risk_(cfg.limits) {}

void Exchange::update_reference(SymbolId s) {
  // Reference price for risk checks: last trade, falling back to the book mid until one exists.
  if (risk_.reference_price(s) != 0) return;
  const OrderBook* bk = engine_.book(s);
  if (!bk) return;
  const auto top = bk->top();
  if (top.has_bid && top.has_ask) risk_.set_reference_price(s, (top.bid + top.ask) / 2);
}

void Exchange::process(const Command& cmd, ExchangeOutput& out) {
  ++stats_.commands;
  const Order& o = cmd.order;
  eng_out_.clear();

  auto risk_reject = [&](RejectReason why, ExecType type, Qty leaves) {
    ExecReport r;
    r.ts = o.ts;
    r.order_id = o.id;
    r.client = o.client;
    r.symbol = o.symbol;
    r.venue = cfg_.venue;
    r.side = o.side;
    r.type = type;
    r.reason = why;
    r.leaves = leaves;
    eng_out_.reports.push_back(r);
  };

  bool rejected_by_risk = false;
  switch (cmd.type) {
    case CommandType::New:
      ++stats_.new_orders;
      if (cfg_.risk_enabled) {
        const RejectReason why = risk_.check(o, o.ts);
        if (why != RejectReason::None) {
          risk_reject(why, ExecType::Rejected, 0);
          rejected_by_risk = true;
          break;
        }
        risk_.track(o);
      }
      engine_.process(cmd, eng_out_);
      break;

    case CommandType::Cancel:
      ++stats_.cancels;  // cancels are risk-reducing: never gated
      engine_.process(cmd, eng_out_);
      break;

    case CommandType::Modify:
      ++stats_.modifies;
      if (cfg_.risk_enabled) {
        const OrderBook* bk = engine_.book(o.symbol);
        const auto info = bk ? bk->find(o.id) : std::nullopt;
        if (info && info->client == o.client) {
          Order probe = o;
          probe.side = info->side;
          probe.type = OrderType::Limit;
          const RejectReason why = risk_.check(probe, o.ts, info->leaves);
          if (why != RejectReason::None) {
            risk_reject(why, ExecType::CancelRejected, info->leaves);
            rejected_by_risk = true;
            break;
          }
          risk_.on_modify_sent(o.id, o.price, o.qty);
        }
      }
      engine_.process(cmd, eng_out_);
      break;
  }

  if (rejected_by_risk) ++stats_.risk_rejects;
  dispatch(out);
  if (cmd.type != CommandType::Cancel) update_reference(o.symbol);

  // A kill triggered while handling fills cancels everything that client still has resting.
  while (!pending_kill_.empty()) {
    const ClientId c = pending_kill_.back();
    pending_kill_.pop_back();
    mass_cancel(c, o.ts, out);
  }
}

// Feed the engine's reports to risk and the ledgers, then append them to the caller's output.
void Exchange::dispatch(ExchangeOutput& out) {
  for (const ExecReport& r : eng_out_.reports) {
    if (r.type == ExecType::Rejected || r.type == ExecType::CancelRejected) {
      ++reject_counts_[static_cast<std::size_t>(r.reason)];
      if (!is_risk_reject(r.reason)) ++stats_.book_rejects;
    }
    if (cfg_.risk_enabled) risk_.on_report(r);

    if (r.is_fill()) {
      marks_.set(r.symbol, r.last_px);
      risk_.set_reference_price(r.symbol, r.last_px);
      auto [it, inserted] = ledger_.try_emplace(r.client, cfg_.cost_basis);
      Portfolio& pf = it->second;
      pf.on_fill(r.symbol, r.side, r.last_qty, r.last_px, cfg_.fees.fee(r.liquidity, r.last_px, r.last_qty));
      pf.snapshot(marks_);
    }
  }

  // Marks moved: a client can breach its loss limit without trading (someone else's trade moved the
  // price of a position it holds), so re-check every client that has a loss limit. O(clients) per
  // command that trades, and only when loss limits are configured.
  if (cfg_.risk_enabled && !eng_out_.fills.empty() && risk_.has_loss_limits()) {
    for (const auto& [c, pf] : ledger_) {
      if (risk_.on_pnl(c, pf.net_pnl(marks_))) {
        ++stats_.kills;
        pending_kill_.push_back(c);
      }
    }
  }
  stats_.fills += eng_out_.fills.size();
  for (const Fill& f : eng_out_.fills) stats_.volume += static_cast<std::uint64_t>(f.qty);

  out.reports.insert(out.reports.end(), eng_out_.reports.begin(), eng_out_.reports.end());
  out.fills.insert(out.fills.end(), eng_out_.fills.begin(), eng_out_.fills.end());
}

void Exchange::mass_cancel(ClientId c, Nanos ts, ExchangeOutput& out) {
  for (const auto& [id, sym] : risk_.open_orders(c)) {
    Command cancel;
    cancel.type = CommandType::Cancel;
    cancel.order.id = id;
    cancel.order.client = c;
    cancel.order.symbol = sym;
    cancel.order.ts = ts;
    eng_out_.clear();
    engine_.process(cancel, eng_out_);
    dispatch(out);
  }
}

}  // namespace qc
