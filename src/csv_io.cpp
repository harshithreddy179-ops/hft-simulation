#include "qc/csv_io.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <istream>
#include <map>
#include <ostream>

namespace qc {

// ---------------------------------------------------------------------------------------------
// symbols
// ---------------------------------------------------------------------------------------------

SymbolId SymbolTable::intern(std::string_view name) {
  const std::string key(name);
  const auto it = ids_.find(key);
  if (it != ids_.end()) return it->second;
  const auto id = static_cast<SymbolId>(names_.size());
  names_.push_back(key);
  ids_.emplace(key, id);
  return id;
}

std::string SymbolTable::name(SymbolId id) const {
  return id < names_.size() ? names_[id] : "S" + std::to_string(id);
}

// ---------------------------------------------------------------------------------------------
// parsing helpers
// ---------------------------------------------------------------------------------------------

namespace {

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
  return s;
}

std::string upper(std::string_view s) {
  std::string r(s);
  for (char& c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return r;
}

template <class T>
bool parse_int(std::string_view s, T& out) {
  s = trim(s);
  if (s.empty()) return false;
  const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
  return ec == std::errc() && p == s.data() + s.size();
}

// "150.25" -> 15025 ticks. Rejects values that are not on the tick grid.
bool parse_price(std::string_view s, Price& out) {
  s = trim(s);
  if (s.empty()) return false;
  double v = 0;
  const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc() || p != s.data() + s.size() || !std::isfinite(v)) return false;
  const double scaled = v * kTicksPerUnit;
  if (std::fabs(scaled) > 9e15) return false;
  const double rounded = std::nearbyint(scaled);
  if (std::fabs(scaled - rounded) > 1e-6) return false;  // e.g. 100.005
  out = static_cast<Price>(rounded);
  return true;
}

std::size_t split(std::string_view line, std::string_view* f, std::size_t max_fields) {
  std::size_t n = 0;
  while (n < max_fields) {
    const std::size_t pos = line.find(',');
    if (pos == std::string_view::npos) {
      f[n++] = line;
      return n;
    }
    f[n++] = line.substr(0, pos);
    line.remove_prefix(pos + 1);
  }
  return n;
}

void add_int(std::string& s, long long v) {
  char buf[24];
  const auto r = std::to_chars(buf, buf + sizeof buf, v);
  s.append(buf, r.ptr);
}

void add_price(std::string& s, Price px) {
  if (px < 0) {
    s.push_back('-');
    px = -px;
  }
  add_int(s, px / 100);
  s.push_back('.');
  const Price frac = px % 100;
  if (frac < 10) s.push_back('0');
  add_int(s, frac);
}

}  // namespace

std::string format_price(Price px) {
  std::string s;
  add_price(s, px);
  return s;
}

// ---------------------------------------------------------------------------------------------
// reading
// ---------------------------------------------------------------------------------------------

std::vector<Command> read_commands_csv(std::istream& in, SymbolTable& symbols, std::vector<CsvError>& errors) {
  std::vector<Command> cmds;
  std::string line;
  std::size_t lineno = 0;
  bool first_data_line = true;

  auto fail = [&](const std::string& msg) { errors.push_back(CsvError{lineno, msg}); };

  while (std::getline(in, line)) {
    ++lineno;
    const std::string_view sv = trim(line);
    if (sv.empty() || sv.front() == '#') continue;

    std::string_view f[11];
    const std::size_t nf = split(sv, f, 11);
    for (std::size_t i = 0; i < nf; ++i) f[i] = trim(f[i]);
    for (std::size_t i = nf; i < 11; ++i) f[i] = {};

    Nanos ts = 0;
    if (first_data_line) {
      first_data_line = false;
      if (!parse_int(f[0], ts)) continue;  // header row
    }
    if (nf < 5) {
      fail("expected at least ts,action,order_id,client,symbol");
      continue;
    }
    if (!parse_int(f[0], ts)) {
      fail("bad timestamp '" + std::string(f[0]) + "'");
      continue;
    }

    Command c;
    const std::string action = upper(f[1]);
    if (action == "NEW") c.type = CommandType::New;
    else if (action == "CANCEL") c.type = CommandType::Cancel;
    else if (action == "MODIFY" || action == "AMEND" || action == "REPLACE") c.type = CommandType::Modify;
    else {
      fail("unknown action '" + std::string(f[1]) + "'");
      continue;
    }

    Order& o = c.order;
    o.ts = ts;
    if (!parse_int(f[2], o.id)) {
      fail("bad order_id '" + std::string(f[2]) + "'");
      continue;
    }
    if (!parse_int(f[3], o.client)) {
      fail("bad client '" + std::string(f[3]) + "'");
      continue;
    }
    if (f[4].empty()) {
      fail("missing symbol");
      continue;
    }
    o.symbol = symbols.intern(f[4]);

    if (c.type == CommandType::New) {
      const std::string side = upper(f[5]);
      if (side == "BUY" || side == "B") o.side = Side::Buy;
      else if (side == "SELL" || side == "S") o.side = Side::Sell;
      else {
        fail("bad side '" + std::string(f[5]) + "'");
        continue;
      }
      const std::string type = f[6].empty() ? "LIMIT" : upper(f[6]);
      if (type == "LIMIT" || type == "L") o.type = OrderType::Limit;
      else if (type == "MARKET" || type == "M") o.type = OrderType::Market;
      else {
        fail("bad type '" + std::string(f[6]) + "'");
        continue;
      }
      const std::string tif = f[7].empty() ? (o.type == OrderType::Market ? "IOC" : "GTC") : upper(f[7]);
      if (tif == "GTC") o.tif = TimeInForce::GTC;
      else if (tif == "IOC") o.tif = TimeInForce::IOC;
      else if (tif == "FOK") o.tif = TimeInForce::FOK;
      else {
        fail("bad tif '" + std::string(f[7]) + "'");
        continue;
      }
      if (o.type == OrderType::Limit) {
        if (!parse_price(f[8], o.price)) {
          fail("bad price '" + std::string(f[8]) + "' (must be on the 0.01 grid)");
          continue;
        }
      }
      if (!parse_int(f[9], o.qty)) {
        fail("bad qty '" + std::string(f[9]) + "'");
        continue;
      }
      if (upper(f[10]).find("POST") != std::string::npos) o.post_only = true;
    } else if (c.type == CommandType::Modify) {
      if (!parse_price(f[8], o.price)) {
        fail("bad price '" + std::string(f[8]) + "' (must be on the 0.01 grid)");
        continue;
      }
      if (!parse_int(f[9], o.qty)) {
        fail("bad qty '" + std::string(f[9]) + "'");
        continue;
      }
    }
    cmds.push_back(c);
  }
  return cmds;
}

// ---------------------------------------------------------------------------------------------
// writing
// ---------------------------------------------------------------------------------------------

void write_commands_csv(std::ostream& out, const std::vector<Command>& cmds, const SymbolTable& symbols) {
  out << "ts_ns,action,order_id,client,symbol,side,type,tif,price,qty,flags\n";
  std::string s;
  for (const Command& c : cmds) {
    const Order& o = c.order;
    s.clear();
    add_int(s, o.ts);
    s.push_back(',');
    s += to_string(c.type);
    s.push_back(',');
    add_int(s, static_cast<long long>(o.id));
    s.push_back(',');
    add_int(s, o.client);
    s.push_back(',');
    s += symbols.name(o.symbol);
    s.push_back(',');
    if (c.type == CommandType::New) {
      s += to_string(o.side);
      s.push_back(',');
      s += to_string(o.type);
      s.push_back(',');
      s += to_string(o.tif);
      s.push_back(',');
      if (o.type == OrderType::Limit) add_price(s, o.price);
      s.push_back(',');
      add_int(s, o.qty);
      s.push_back(',');
      if (o.post_only) s += "POST_ONLY";
    } else if (c.type == CommandType::Modify) {
      s += ",,,";
      add_price(s, o.price);
      s.push_back(',');
      add_int(s, o.qty);
      s.push_back(',');
    } else {
      s += ",,,,,";
    }
    s.push_back('\n');
    out << s;
  }
}

void write_trades_header(std::ostream& out) {
  out << "ts_ns,symbol,trade_id,price,qty,aggressor,maker_order,taker_order,maker_client,taker_client\n";
}

void write_reports_header(std::ostream& out) {
  out << "ts_ns,order_id,client,symbol,side,exec_type,reason,liquidity,last_px,last_qty,leaves\n";
}

void write_trades(std::ostream& out, const std::vector<Fill>& fills, const SymbolTable& symbols) {
  std::string s;
  for (const Fill& f : fills) {
    s.clear();
    add_int(s, f.ts);
    s.push_back(',');
    s += symbols.name(f.symbol);
    s.push_back(',');
    add_int(s, static_cast<long long>(f.trade_id));
    s.push_back(',');
    add_price(s, f.price);
    s.push_back(',');
    add_int(s, f.qty);
    s.push_back(',');
    s += to_string(f.taker_side);
    s.push_back(',');
    add_int(s, static_cast<long long>(f.maker_order));
    s.push_back(',');
    add_int(s, static_cast<long long>(f.taker_order));
    s.push_back(',');
    add_int(s, f.maker_client);
    s.push_back(',');
    add_int(s, f.taker_client);
    s.push_back('\n');
    out << s;
  }
}

void write_reports(std::ostream& out, const std::vector<ExecReport>& reports, const SymbolTable& symbols) {
  std::string s;
  for (const ExecReport& r : reports) {
    s.clear();
    add_int(s, r.ts);
    s.push_back(',');
    add_int(s, static_cast<long long>(r.order_id));
    s.push_back(',');
    add_int(s, r.client);
    s.push_back(',');
    s += symbols.name(r.symbol);
    s.push_back(',');
    // a cancel/amend of an unknown order has no meaningful side
    if (!(r.type == ExecType::CancelRejected && r.reason == RejectReason::UnknownOrder)) s += to_string(r.side);
    s.push_back(',');
    s += to_cstr(r.type);
    s.push_back(',');
    s += to_string(r.reason);
    s.push_back(',');
    s += r.liquidity == Liquidity::Maker ? "MAKER" : r.liquidity == Liquidity::Taker ? "TAKER" : "";
    s.push_back(',');
    if (r.is_fill()) add_price(s, r.last_px);
    s.push_back(',');
    if (r.is_fill()) add_int(s, r.last_qty);
    s.push_back(',');
    add_int(s, r.leaves);
    s.push_back('\n');
    out << s;
  }
}

void write_pnl_csv(std::ostream& out, const Exchange& ex, const SymbolTable& symbols) {
  write_pnl_csv(out, std::vector<const Exchange*>{&ex}, symbols);
}

void write_pnl_csv(std::ostream& out, const std::vector<const Exchange*>& shards, const SymbolTable& symbols) {
  out << "client,symbol,position,avg_cost,mark,realized,unrealized,fees,net\n";
  out << std::fixed << std::setprecision(4);
  const auto z = [](double v) { return std::fabs(v) < 5e-5 ? 0.0 : v; };  // avoid printing "-0.0000"
  std::map<ClientId, std::vector<SymbolPnl>> rows;
  for (const Exchange* ex : shards) {
    for (const auto& [client, pf] : ex->ledger()) {
      for (const SymbolPnl& p : pf.breakdown(ex->marks())) rows[client].push_back(p);
    }
  }
  for (auto& [client, v] : rows) {
    std::sort(v.begin(), v.end(), [](const SymbolPnl& a, const SymbolPnl& b) { return a.symbol < b.symbol; });
    double realized = 0, unrealized = 0, fees = 0;
    for (const SymbolPnl& p : v) {
      out << client << ',' << symbols.name(p.symbol) << ',' << p.qty << ',' << p.avg_cost << ',' << p.mark << ','
          << z(p.realized) << ',' << z(p.unrealized) << ',' << z(p.fees) << ',' << z(p.net()) << '\n';
      realized += p.realized;
      unrealized += p.unrealized;
      fees += p.fees;
    }
    out << client << ",ALL,,,," << z(realized) << ',' << z(unrealized) << ',' << z(fees) << ',' << z(realized + unrealized - fees)
        << '\n';
  }
}

}  // namespace qc
