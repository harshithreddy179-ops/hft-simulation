#pragma once
// CSV interchange for the replay tooling.
//
// Input (orders.csv) -- header optional, '#' comments and blank lines ignored, prices in currency
// units on the 0.01 tick grid:
//
//   ts_ns,action,order_id,client,symbol,side,type,tif,price,qty[,flags]
//   1000,NEW,1,10,AAPL,BUY,LIMIT,GTC,150.25,100
//   1500,NEW,2,11,AAPL,SELL,MARKET,IOC,,50
//   2000,MODIFY,1,10,AAPL,,,,150.30,80        (price/qty = new price / new remaining qty)
//   2500,CANCEL,1,10,AAPL
//   3000,NEW,3,10,AAPL,BUY,LIMIT,GTC,150.00,10,POST_ONLY
//
// Outputs: trades.csv, reports.csv, pnl.csv (see the write_* functions).

#include <iosfwd>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "qc/exchange.hpp"
#include "qc/types.hpp"

namespace qc {

class SymbolTable {
 public:
  SymbolId intern(std::string_view name);
  std::string name(SymbolId id) const;
  std::size_t size() const { return names_.size(); }

 private:
  std::vector<std::string> names_;
  std::unordered_map<std::string, SymbolId> ids_;
};

struct CsvError {
  std::size_t line = 0;
  std::string message;
};

std::vector<Command> read_commands_csv(std::istream& in, SymbolTable& symbols, std::vector<CsvError>& errors);
void write_commands_csv(std::ostream& out, const std::vector<Command>& cmds, const SymbolTable& symbols);

std::string format_price(Price px);  // ticks -> "123.45"

void write_trades_header(std::ostream& out);
void write_reports_header(std::ostream& out);
void write_trades(std::ostream& out, const std::vector<Fill>& fills, const SymbolTable& symbols);
void write_reports(std::ostream& out, const std::vector<ExecReport>& reports, const SymbolTable& symbols);
void write_pnl_csv(std::ostream& out, const Exchange& ex, const SymbolTable& symbols);
// Same output for a sharded run: every (client, symbol) lives in exactly one shard, so rows are
// gathered across shards and the per-client ALL row sums them.
void write_pnl_csv(std::ostream& out, const std::vector<const Exchange*>& shards, const SymbolTable& symbols);

}  // namespace qc
