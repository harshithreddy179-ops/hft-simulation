#pragma once
// Exchange = pre-trade risk gate + matching engine + per-client ledger, wired together.
//
//     Command ──► RiskEngine::check ──► MatchingEngine ──► ExecReports ──┬─► RiskEngine (positions, open exposure)
//                      │ reject                                          ├─► Portfolio per client (P&L, fees)
//                      └──► Rejected report                              └─► caller (trades, reports)
//
// Single-threaded and deterministic: the output is a pure function of the command stream, which is
// what makes journal replay a valid recovery strategy and lets the multi-threaded version be tested
// against this one.

#include <array>
#include <cstdint>
#include <map>
#include <vector>

#include "qc/engine.hpp"
#include "qc/portfolio.hpp"
#include "qc/risk.hpp"
#include "qc/types.hpp"

namespace qc {

struct FeeSchedule {
  double taker_bps = 0.0;  // cost, in basis points of notional
  double maker_bps = 0.0;  // negative = rebate
  double fee(Liquidity l, Price px, Qty qty) const {
    const double bps = l == Liquidity::Taker ? taker_bps : l == Liquidity::Maker ? maker_bps : 0.0;
    return notional(px, qty) * bps * 1e-4;
  }
};

struct ExchangeConfig {
  bool risk_enabled = true;
  RiskLimits limits;  // default limits applied to every client
  StpMode stp = StpMode::None;
  CostBasis cost_basis = CostBasis::AverageCost;
  FeeSchedule fees;
  VenueId venue = 0;
};

using ExchangeOutput = EngineOutput;

struct ExchangeStats {
  std::uint64_t commands = 0;
  std::uint64_t new_orders = 0;
  std::uint64_t cancels = 0;
  std::uint64_t modifies = 0;
  std::uint64_t book_rejects = 0;   // validation / unknown order / ownership
  std::uint64_t risk_rejects = 0;
  std::uint64_t fills = 0;          // trades
  std::uint64_t volume = 0;         // shares traded (each trade counted once)
  std::uint64_t kills = 0;          // daily-loss kill switches tripped
};

class Exchange {
 public:
  explicit Exchange(ExchangeConfig cfg = {});

  // Process one command; reports and trades are appended to `out`.
  void process(const Command& cmd, ExchangeOutput& out);

  const ExchangeConfig& config() const { return cfg_; }
  const ExchangeStats& stats() const { return stats_; }
  RiskEngine& risk() { return risk_; }
  const RiskEngine& risk() const { return risk_; }
  MatchingEngine& engine() { return engine_; }
  const MatchingEngine& engine() const { return engine_; }
  const MarkTable& marks() const { return marks_; }
  const std::map<ClientId, Portfolio>& ledger() const { return ledger_; }
  const Portfolio* portfolio(ClientId c) const {
    const auto it = ledger_.find(c);
    return it == ledger_.end() ? nullptr : &it->second;
  }
  std::uint64_t reject_count(RejectReason r) const { return reject_counts_[static_cast<std::size_t>(r)]; }

  // Cancel every open order of `c` (used when a kill switch trips). Appends reports to `out`.
  void mass_cancel(ClientId c, Nanos ts, ExchangeOutput& out);

 private:
  void dispatch(ExchangeOutput& out);
  void update_reference(SymbolId s);

  ExchangeConfig cfg_;
  MatchingEngine engine_;
  RiskEngine risk_;
  MarkTable marks_;
  std::map<ClientId, Portfolio> ledger_;
  ExchangeStats stats_;
  EngineOutput eng_out_;  // reused scratch
  std::vector<ClientId> pending_kill_;
  std::array<std::uint64_t, static_cast<std::size_t>(RejectReason::Count_)> reject_counts_{};
};

}  // namespace qc
