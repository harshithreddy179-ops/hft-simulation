#include <algorithm>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include "qc/flat_id_map.hpp"
#include "qc/order_book.hpp"
#include "qc/rng.hpp"
#include "testing.hpp"

using namespace qc;

namespace {

Order limit(OrderId id, ClientId c, Side s, Price px, Qty q, TimeInForce tif = TimeInForce::GTC, Nanos ts = 0) {
  Order o;
  o.id = id;
  o.client = c;
  o.side = s;
  o.type = OrderType::Limit;
  o.tif = tif;
  o.price = px;
  o.qty = q;
  o.ts = ts;
  return o;
}

Order market(OrderId id, ClientId c, Side s, Qty q) {
  Order o = limit(id, c, s, 0, q, TimeInForce::IOC);
  o.type = OrderType::Market;
  return o;
}

struct Harness {
  OrderBook book;
  BookEvents ev;
  explicit Harness(StpMode stp = StpMode::None) : book(1, BookConfig{stp, 64}) {}
  SubmitResult put(const Order& o) {
    ev.clear();
    SubmitResult r = book.submit(o, ev);
    std::string why;
    CHECK_MSG(book.validate(&why), why);
    return r;
  }
};

// ------------------------------------------------------------------------------------------------
// Naive reference book: vector of orders, linear scans, no cleverness. Obviously correct, slow.
// ------------------------------------------------------------------------------------------------
struct RefOrder {
  OrderId id;
  ClientId client;
  Side side;
  Price price;
  Qty leaves;
  std::uint64_t seq;
};
struct RefFill {
  OrderId maker, taker;
  Price price;
  Qty qty;
  bool operator==(const RefFill&) const = default;
};
struct RefOut {
  OrderStatus status = OrderStatus::Rejected;
  RejectReason reason = RejectReason::None;
  Qty filled = 0, leaves = 0, cancelled = 0;
  std::vector<RefFill> fills;
  std::vector<OrderId> stp_removed;
};

class RefBook {
 public:
  explicit RefBook(StpMode stp) : stp_(stp) {}

  RefOut submit(const Order& o) {
    RefOut out;
    if (o.qty <= 0) return rej(out, RejectReason::InvalidQty);
    if (o.type == OrderType::Limit && o.price <= 0) return rej(out, RejectReason::InvalidPrice);
    if (o.post_only && (o.type != OrderType::Limit || o.tif != TimeInForce::GTC)) return rej(out, RejectReason::InvalidOrder);
    if (find(o.id) >= 0) return rej(out, RejectReason::DuplicateOrderId);

    const bool is_market = o.type == OrderType::Market;
    const Price limit_px = is_market ? (o.side == Side::Buy ? kMaxPrice : 0) : o.price;
    const TimeInForce tif = is_market && o.tif == TimeInForce::GTC ? TimeInForce::IOC : o.tif;

    if (o.post_only) {
      const int best = best_opposite(o.side, limit_px);
      if (best >= 0) return rej(out, RejectReason::PostOnlyWouldCross);
    }
    if (tif == TimeInForce::FOK) {
      RefBook sim = *this;  // dry run on a copy
      Qty rem = o.qty;
      RefOut tmp;
      bool stopped = false;
      sim.run_match(o, limit_px, rem, tmp, stopped);
      if (rem > 0) return rej(out, RejectReason::FokUnfillable);
    }

    Qty rem = o.qty;
    bool stopped = false;
    run_match(o, limit_px, rem, out, stopped);
    if (rem == 0) {
      out.status = OrderStatus::Filled;
    } else if (!is_market && tif == TimeInForce::GTC && !stopped) {
      orders_.push_back(RefOrder{o.id, o.client, o.side, o.price, rem, seq_++});
      out.status = OrderStatus::Resting;
      out.leaves = rem;
    } else {
      out.status = OrderStatus::Expired;
      out.cancelled = rem;
    }
    return out;
  }

  bool cancel(OrderId id) {
    const int i = find(id);
    if (i < 0) return false;
    orders_.erase(orders_.begin() + i);
    return true;
  }

  RefOut modify(OrderId id, Price px, Qty qty) {
    RefOut out;
    const int i = find(id);
    if (i < 0) return rej(out, RejectReason::UnknownOrder);
    if (qty <= 0) return rej(out, RejectReason::InvalidQty);
    if (px <= 0) return rej(out, RejectReason::InvalidPrice);
    RefOrder old = orders_[static_cast<std::size_t>(i)];
    if (px == old.price && qty <= old.leaves) {
      orders_[static_cast<std::size_t>(i)].leaves = qty;
      out.status = OrderStatus::Resting;
      out.leaves = qty;
      return out;
    }
    orders_.erase(orders_.begin() + i);
    Order o = limit(id, old.client, old.side, px, qty);
    return submit(o);
  }

  std::map<Price, Qty> depth(Side s) const {
    std::map<Price, Qty> d;
    for (const auto& o : orders_) {
      if (o.side == s) d[o.price] += o.leaves;
    }
    return d;
  }
  std::size_t size() const { return orders_.size(); }
  const std::vector<RefOrder>& orders() const { return orders_; }

 private:
  static RefOut& rej(RefOut& out, RejectReason r) {
    out.status = OrderStatus::Rejected;
    out.reason = r;
    return out;
  }
  int find(OrderId id) const {
    for (std::size_t i = 0; i < orders_.size(); ++i) {
      if (orders_[i].id == id) return static_cast<int>(i);
    }
    return -1;
  }
  // index of best crossing opposite order (best price, then earliest) or -1
  int best_opposite(Side taker, Price limit_px) const {
    int best = -1;
    for (std::size_t i = 0; i < orders_.size(); ++i) {
      const RefOrder& r = orders_[i];
      if (r.side == taker) continue;
      const bool crosses = taker == Side::Buy ? r.price <= limit_px : r.price >= limit_px;
      if (!crosses) continue;
      if (best < 0) {
        best = static_cast<int>(i);
        continue;
      }
      const RefOrder& b = orders_[static_cast<std::size_t>(best)];
      const bool better = taker == Side::Buy ? r.price < b.price : r.price > b.price;
      if (better || (r.price == b.price && r.seq < b.seq)) best = static_cast<int>(i);
    }
    return best;
  }
  void run_match(const Order& o, Price limit_px, Qty& rem, RefOut& out, bool& stopped) {
    while (rem > 0) {
      const int bi = best_opposite(o.side, limit_px);
      if (bi < 0) break;
      RefOrder& m = orders_[static_cast<std::size_t>(bi)];
      if (stp_ != StpMode::None && m.client == o.client) {
        if (stp_ == StpMode::CancelTaker) {
          stopped = true;
          break;
        }
        out.stp_removed.push_back(m.id);
        orders_.erase(orders_.begin() + bi);
        continue;
      }
      const Qty q = std::min(rem, m.leaves);
      out.fills.push_back(RefFill{m.id, o.id, m.price, q});
      out.filled += q;
      rem -= q;
      m.leaves -= q;
      if (m.leaves == 0) orders_.erase(orders_.begin() + bi);
    }
  }

  StpMode stp_;
  std::vector<RefOrder> orders_;
  std::uint64_t seq_ = 0;
};

std::vector<RefFill> to_ref_fills(const BookEvents& ev) {
  std::vector<RefFill> v;
  for (const Fill& f : ev.fills) v.push_back(RefFill{f.maker_order, f.taker_order, f.price, f.qty});
  return v;
}

std::vector<OrderId> stp_ids(const BookEvents& ev) {
  std::vector<OrderId> v;
  for (const Removal& r : ev.removals) {
    if (r.reason == RemovalReason::SelfTrade) v.push_back(r.id);
  }
  return v;
}

}  // namespace

// ------------------------------------------------------------------------------------------------
// scenario tests
// ------------------------------------------------------------------------------------------------

TEST(book_rests_and_reports_top) {
  Harness h;
  CHECK(h.put(limit(1, 1, Side::Buy, 10000, 100)).status == OrderStatus::Resting);
  CHECK(h.put(limit(2, 2, Side::Sell, 10010, 50)).status == OrderStatus::Resting);
  const auto t = h.book.top();
  CHECK(t.has_bid && t.has_ask);
  CHECK_EQ(t.bid, 10000);
  CHECK_EQ(t.ask, 10010);
  CHECK_EQ(t.bid_qty, 100);
  CHECK_EQ(t.ask_qty, 50);
  CHECK(h.ev.fills.empty());
}

TEST(book_price_time_priority) {
  Harness h;
  h.put(limit(1, 1, Side::Sell, 10000, 10));
  h.put(limit(2, 2, Side::Sell, 10000, 10));
  h.put(limit(3, 3, Side::Sell, 9990, 10));  // better price, arrives last
  const auto r = h.put(limit(4, 4, Side::Buy, 10000, 25));
  CHECK(r.status == OrderStatus::Filled);
  CHECK_EQ(h.ev.fills.size(), 3u);
  CHECK_EQ(h.ev.fills[0].maker_order, 3u);  // best price first
  CHECK_EQ(h.ev.fills[1].maker_order, 1u);  // then earliest at same price
  CHECK_EQ(h.ev.fills[2].maker_order, 2u);
  CHECK_EQ(h.ev.fills[2].qty, 5);
  CHECK_EQ(h.book.volume_at(Side::Sell, 10000), 5);
}

TEST(book_trades_at_maker_price) {
  Harness h;
  h.put(limit(1, 1, Side::Sell, 10000, 10));
  const auto r = h.put(limit(2, 2, Side::Buy, 10050, 10));  // willing to pay more
  CHECK_EQ(r.filled, 10);
  CHECK_EQ(h.ev.fills[0].price, 10000);
  CHECK_EQ(r.filled_value, 10000 * 10);
}

TEST(book_partial_fill_rests_remainder) {
  Harness h;
  h.put(limit(1, 1, Side::Sell, 10000, 30));
  const auto r = h.put(limit(2, 2, Side::Buy, 10000, 100));
  CHECK(r.status == OrderStatus::Resting);
  CHECK_EQ(r.filled, 30);
  CHECK_EQ(r.leaves, 70);
  CHECK_EQ(h.book.volume_at(Side::Buy, 10000), 70);
  CHECK(!h.book.best_ask().has_value());
  CHECK_EQ(h.ev.fills[0].taker_leaves, 70);
  CHECK_EQ(h.ev.fills[0].maker_leaves, 0);
}

TEST(book_sweeps_multiple_levels) {
  Harness h;
  h.put(limit(1, 1, Side::Sell, 10000, 10));
  h.put(limit(2, 1, Side::Sell, 10010, 10));
  h.put(limit(3, 1, Side::Sell, 10020, 10));
  const auto r = h.put(limit(4, 2, Side::Buy, 10010, 100));
  CHECK_EQ(r.filled, 20);  // 10020 is beyond the limit
  CHECK_EQ(r.leaves, 80);
  CHECK_EQ(h.book.best_ask().value(), 10020);
  CHECK_EQ(h.book.best_bid().value(), 10010);
}

TEST(book_ioc_drops_remainder) {
  Harness h;
  h.put(limit(1, 1, Side::Sell, 10000, 30));
  const auto r = h.put(limit(2, 2, Side::Buy, 10000, 100, TimeInForce::IOC));
  CHECK(r.status == OrderStatus::Expired);
  CHECK_EQ(r.filled, 30);
  CHECK_EQ(r.cancelled, 70);
  CHECK_EQ(h.book.order_count(), 0u);
}

TEST(book_fok_all_or_nothing) {
  Harness h;
  h.put(limit(1, 1, Side::Sell, 10000, 30));
  h.put(limit(2, 1, Side::Sell, 10010, 30));
  auto r = h.put(limit(3, 2, Side::Buy, 10010, 70, TimeInForce::FOK));
  CHECK(r.status == OrderStatus::Rejected);
  CHECK(r.reason == RejectReason::FokUnfillable);
  CHECK(h.ev.fills.empty());
  CHECK_EQ(h.book.volume_at(Side::Sell, 10000), 30);  // untouched
  r = h.put(limit(4, 2, Side::Buy, 10010, 60, TimeInForce::FOK));
  CHECK(r.status == OrderStatus::Filled);
  CHECK_EQ(h.book.order_count(), 0u);
}

TEST(book_market_orders) {
  Harness h;
  h.put(limit(1, 1, Side::Sell, 10000, 10));
  h.put(limit(2, 1, Side::Sell, 10500, 10));
  auto r = h.put(market(3, 2, Side::Buy, 15));
  CHECK(r.status == OrderStatus::Filled);
  CHECK_EQ(h.ev.fills.size(), 2u);
  CHECK_EQ(h.ev.fills[1].price, 10500);  // market orders walk the book

  r = h.put(market(4, 2, Side::Buy, 100));  // only 5 left
  CHECK(r.status == OrderStatus::Expired);
  CHECK_EQ(r.filled, 5);
  CHECK_EQ(r.cancelled, 95);

  r = h.put(market(5, 2, Side::Buy, 10));  // empty book: nothing happens, nothing rests
  CHECK(r.status == OrderStatus::Expired);
  CHECK_EQ(r.filled, 0);
  CHECK_EQ(h.book.order_count(), 0u);
}

TEST(book_post_only) {
  Harness h;
  h.put(limit(1, 1, Side::Sell, 10000, 10));
  Order po = limit(2, 2, Side::Buy, 10000, 10);
  po.post_only = true;
  auto r = h.put(po);
  CHECK(r.status == OrderStatus::Rejected);
  CHECK(r.reason == RejectReason::PostOnlyWouldCross);
  po.id = 3;
  po.price = 9990;
  r = h.put(po);
  CHECK(r.status == OrderStatus::Resting);
  Order bad = limit(4, 2, Side::Buy, 9990, 10, TimeInForce::IOC);
  bad.post_only = true;
  CHECK(h.put(bad).reason == RejectReason::InvalidOrder);
}

TEST(book_validation) {
  Harness h;
  CHECK(h.put(limit(1, 1, Side::Buy, 10000, 0)).reason == RejectReason::InvalidQty);
  CHECK(h.put(limit(2, 1, Side::Buy, 10000, -5)).reason == RejectReason::InvalidQty);
  CHECK(h.put(limit(3, 1, Side::Buy, 0, 5)).reason == RejectReason::InvalidPrice);
  CHECK(h.put(limit(4, 1, Side::Buy, -3, 5)).reason == RejectReason::InvalidPrice);
  CHECK(h.put(limit(5, 1, Side::Buy, 10000, kMaxQty + 1)).reason == RejectReason::InvalidQty);
  CHECK(h.put(limit(6, 1, Side::Buy, kMaxLimitPrice + 1, 5)).reason == RejectReason::InvalidPrice);
  CHECK(h.put(limit(kInvalidOrderId, 1, Side::Buy, 10000, 5)).reason == RejectReason::InvalidOrder);
  h.put(limit(7, 1, Side::Buy, 10000, 5));
  CHECK(h.put(limit(7, 1, Side::Buy, 10000, 5)).reason == RejectReason::DuplicateOrderId);
  CHECK_EQ(h.book.order_count(), 1u);
}

TEST(book_cancel) {
  Harness h;
  h.put(limit(1, 1, Side::Buy, 10000, 10));
  h.put(limit(2, 1, Side::Buy, 10000, 20));
  h.put(limit(3, 1, Side::Buy, 9990, 5));
  BookEvents ev;
  CHECK(h.book.cancel(1, ev));
  CHECK_EQ(ev.removals.size(), 1u);
  CHECK_EQ(ev.removals[0].qty, 10);
  CHECK(!h.book.cancel(1, ev));  // already gone
  CHECK(!h.book.cancel(99, ev));
  CHECK_EQ(h.book.volume_at(Side::Buy, 10000), 20);
  CHECK(h.book.cancel(2, ev));
  CHECK_EQ(h.book.level_count(Side::Buy), 1u);  // emptied level is removed
  CHECK(h.book.validate());
  CHECK(h.book.cancel(3, ev));
  CHECK_EQ(h.book.order_count(), 0u);
  CHECK(h.book.validate());
}

TEST(book_modify_reduce_keeps_priority) {
  Harness h;
  h.put(limit(1, 1, Side::Sell, 10000, 50));
  h.put(limit(2, 2, Side::Sell, 10000, 50));
  BookEvents ev;
  const auto r = h.book.modify(1, 10000, 20, 0, ev);
  CHECK(r.status == OrderStatus::Resting);
  CHECK_EQ(r.leaves, 20);
  CHECK(h.book.validate());
  h.put(limit(3, 3, Side::Buy, 10000, 30));
  CHECK_EQ(h.ev.fills[0].maker_order, 1u);  // order 1 is still first in the queue
  CHECK_EQ(h.ev.fills[0].qty, 20);
  CHECK_EQ(h.ev.fills[1].maker_order, 2u);
}

TEST(book_modify_increase_or_reprice_loses_priority) {
  Harness h;
  h.put(limit(1, 1, Side::Sell, 10000, 10));
  h.put(limit(2, 2, Side::Sell, 10000, 10));
  BookEvents ev;
  auto r = h.book.modify(1, 10000, 15, 5, ev);  // increase: goes to the back
  CHECK(r.status == OrderStatus::Resting);
  CHECK_EQ(h.book.volume_at(Side::Sell, 10000), 25);
  h.put(limit(3, 3, Side::Buy, 10000, 10));
  CHECK_EQ(h.ev.fills[0].maker_order, 2u);

  // reprice into the book -> matches immediately as an aggressor
  h.put(limit(10, 4, Side::Buy, 9900, 10));
  ev.clear();
  r = h.book.modify(10, 10000, 10, 7, ev);
  CHECK(r.status == OrderStatus::Filled);
  CHECK_EQ(ev.fills.size(), 1u);
  CHECK_EQ(ev.fills[0].maker_order, 1u);
  CHECK_EQ(ev.fills[0].taker_order, 10u);
  CHECK(h.book.validate());

  r = h.book.modify(555, 10000, 10, 0, ev);
  CHECK(r.reason == RejectReason::UnknownOrder);
}

TEST(book_stp_cancel_resting) {
  Harness h(StpMode::CancelResting);
  h.put(limit(1, 7, Side::Sell, 10000, 10));  // own order, first in queue
  h.put(limit(2, 8, Side::Sell, 10000, 10));
  const auto r = h.put(limit(3, 7, Side::Buy, 10000, 10));
  CHECK_EQ(r.filled, 10);
  CHECK_EQ(h.ev.fills.size(), 1u);
  CHECK_EQ(h.ev.fills[0].maker_order, 2u);   // skipped own order
  CHECK_EQ(h.ev.removals.size(), 1u);
  CHECK_EQ(h.ev.removals[0].id, 1u);
  CHECK(h.ev.removals[0].reason == RemovalReason::SelfTrade);
  CHECK(!h.book.find(1).has_value());
}

TEST(book_stp_cancel_taker) {
  Harness h(StpMode::CancelTaker);
  h.put(limit(1, 8, Side::Sell, 10000, 5));
  h.put(limit(2, 7, Side::Sell, 10000, 10));
  const auto r = h.put(limit(3, 7, Side::Buy, 10000, 20));
  CHECK_EQ(r.filled, 5);  // trades with client 8, then hits own order and stops
  CHECK(r.status == OrderStatus::Expired);
  CHECK_EQ(r.cancelled, 15);
  CHECK(h.book.find(2).has_value());  // own resting order survives
  CHECK_EQ(h.book.volume_at(Side::Buy, 10000), 0);  // taker did not rest
}

TEST(book_stp_fok_accounts_for_self_liquidity) {
  Harness h(StpMode::CancelResting);
  h.put(limit(1, 7, Side::Sell, 10000, 10));
  h.put(limit(2, 8, Side::Sell, 10000, 5));
  const auto r = h.put(limit(3, 7, Side::Buy, 10000, 10, TimeInForce::FOK));
  CHECK(r.reason == RejectReason::FokUnfillable);  // only 5 non-self liquidity
  CHECK(h.book.find(1).has_value());               // and nothing was cancelled by the failed dry run
}

TEST(book_depth_snapshot) {
  Harness h;
  for (int i = 0; i < 5; ++i) h.put(limit(100 + i, 1, Side::Buy, 10000 - i, 10 + i));
  h.put(limit(200, 2, Side::Buy, 10000, 7));
  std::vector<OrderBook::DepthLevel> d;
  h.book.depth(Side::Buy, 3, d);
  CHECK_EQ(d.size(), 3u);
  CHECK_EQ(d[0].price, 10000);
  CHECK_EQ(d[0].qty, 17);
  CHECK_EQ(d[0].orders, 2u);
  CHECK_EQ(d[1].price, 9999);
  CHECK_EQ(d[2].price, 9998);
}

// ------------------------------------------------------------------------------------------------
// differential fuzz: optimized book vs naive reference, all STP modes
// ------------------------------------------------------------------------------------------------

namespace {

void fuzz_book(StpMode stp, std::uint64_t seed, int ops) {
  Rng rng(seed);
  OrderBook book(1, BookConfig{stp, 16});
  RefBook ref(stp);
  BookEvents ev;
  std::vector<OrderId> live_guess;
  OrderId next_id = 1;
  int divergences = 0;

  auto check_state = [&](int step) {
    std::string why;
    if (!book.validate(&why)) {
      ++divergences;
      CHECK_MSG(false, "validate failed at step " << step << ": " << why);
    }
    for (Side s : {Side::Buy, Side::Sell}) {
      std::vector<OrderBook::DepthLevel> d;
      book.depth(s, 100000, d);
      std::map<Price, Qty> got;
      for (const auto& l : d) got[l.price] = l.qty;
      if (got != ref.depth(s)) {
        ++divergences;
        CHECK_MSG(false, "depth mismatch at step " << step);
      }
    }
    if (book.order_count() != ref.size()) {
      ++divergences;
      CHECK_MSG(false, "order count mismatch at step " << step);
    }
  };

  for (int step = 0; step < ops && divergences == 0; ++step) {
    const int dice = static_cast<int>(rng.range(0, 99));
    ev.clear();
    if (dice < 70) {
      Order o;
      o.id = next_id++;
      o.client = static_cast<ClientId>(rng.range(1, 4));
      o.side = rng.chance(0.5) ? Side::Buy : Side::Sell;
      o.price = 95 + rng.range(0, 10);
      o.qty = rng.range(1, 20);
      o.ts = step;
      const int kind = static_cast<int>(rng.range(0, 99));
      if (kind < 62) {
        o.tif = TimeInForce::GTC;
      } else if (kind < 76) {
        o.tif = TimeInForce::IOC;
      } else if (kind < 84) {
        o.tif = TimeInForce::FOK;
      } else if (kind < 92) {
        o.type = OrderType::Market;
        o.tif = TimeInForce::IOC;
      } else {
        o.post_only = true;
      }
      if (rng.chance(0.02)) o.qty = rng.chance(0.5) ? 0 : -3;       // invalid qty
      if (rng.chance(0.02) && o.type == OrderType::Limit) o.price = rng.chance(0.5) ? 0 : -7;
      if (rng.chance(0.02) && next_id > 5) o.id = static_cast<OrderId>(rng.range(1, static_cast<std::int64_t>(next_id) - 1));  // maybe duplicate

      const SubmitResult got = book.submit(o, ev);
      const RefOut want = ref.submit(o);
      const bool same = got.status == want.status && got.reason == want.reason && got.filled == want.filled &&
                        got.leaves == want.leaves && got.cancelled == want.cancelled &&
                        to_ref_fills(ev) == want.fills && stp_ids(ev) == want.stp_removed;
      if (!same) {
        ++divergences;
        CHECK_MSG(false, "submit mismatch step " << step << " id " << o.id << " status " << int(got.status) << "/"
                                                 << int(want.status) << " reason " << int(got.reason) << "/"
                                                 << int(want.reason) << " filled " << got.filled << "/" << want.filled);
      }
      if (got.status == OrderStatus::Resting) live_guess.push_back(o.id);
    } else if (dice < 85) {
      const OrderId id = live_guess.empty() || rng.chance(0.1)
                             ? static_cast<OrderId>(rng.range(1, static_cast<std::int64_t>(next_id)))
                             : live_guess[static_cast<std::size_t>(rng.range(0, static_cast<std::int64_t>(live_guess.size()) - 1))];
      const bool got = book.cancel(id, ev);
      const bool want = ref.cancel(id);
      if (got != want) {
        ++divergences;
        CHECK_MSG(false, "cancel mismatch step " << step);
      }
    } else {
      const OrderId id = live_guess.empty() || rng.chance(0.1)
                             ? static_cast<OrderId>(rng.range(1, static_cast<std::int64_t>(next_id)))
                             : live_guess[static_cast<std::size_t>(rng.range(0, static_cast<std::int64_t>(live_guess.size()) - 1))];
      Price px = 95 + rng.range(0, 10);
      Qty qty = rng.range(1, 25);
      if (rng.chance(0.4)) {
        if (auto info = book.find(id)) px = info->price;  // same-price amends exercise the in-place path
      }
      if (rng.chance(0.03)) qty = 0;
      const SubmitResult got = book.modify(id, px, qty, step, ev);
      const RefOut want = ref.modify(id, px, qty);
      const bool same = got.status == want.status && got.reason == want.reason && got.filled == want.filled &&
                        got.leaves == want.leaves && got.cancelled == want.cancelled && to_ref_fills(ev) == want.fills &&
                        stp_ids(ev) == want.stp_removed;
      if (!same) {
        ++divergences;
        CHECK_MSG(false, "modify mismatch step " << step << " id " << id);
      }
    }
    if (step % 50 == 0 || divergences) check_state(step);
  }
  check_state(ops);
}

}  // namespace

TEST(book_fuzz_vs_reference_no_stp) {
  for (std::uint64_t seed = 1; seed <= 6; ++seed) fuzz_book(StpMode::None, seed, 20000);
}
TEST(book_fuzz_vs_reference_stp_cancel_resting) {
  for (std::uint64_t seed = 11; seed <= 16; ++seed) fuzz_book(StpMode::CancelResting, seed, 20000);
}
TEST(book_fuzz_vs_reference_stp_cancel_taker) {
  for (std::uint64_t seed = 21; seed <= 26; ++seed) fuzz_book(StpMode::CancelTaker, seed, 20000);
}

TEST(book_conserves_quantity) {
  // Everything that goes in either rests, trades (counted once per side) or is explicitly cancelled.
  Rng rng(99);
  OrderBook book(1);
  BookEvents ev;
  Qty submitted = 0, traded = 0, cancelled_or_expired = 0;
  for (int i = 1; i <= 30000; ++i) {
    Order o = limit(static_cast<OrderId>(i), static_cast<ClientId>(rng.range(1, 6)),
                    rng.chance(0.5) ? Side::Buy : Side::Sell, 90 + rng.range(0, 20), rng.range(1, 50),
                    rng.chance(0.3) ? TimeInForce::IOC : TimeInForce::GTC);
    ev.clear();
    const auto r = book.submit(o, ev);
    if (r.status == OrderStatus::Rejected) continue;
    submitted += o.qty;
    cancelled_or_expired += r.cancelled;
    for (const auto& f : ev.fills) traded += f.qty;
    if (i % 7 == 0) {
      ev.clear();
      const OrderId victim = static_cast<OrderId>(rng.range(1, i));
      book.cancel(victim, ev);
      for (const auto& rm : ev.removals) cancelled_or_expired += rm.qty;
    }
  }
  Qty resting = 0;
  for (Side s : {Side::Buy, Side::Sell}) {
    std::vector<OrderBook::DepthLevel> d;
    book.depth(s, 1000000, d);
    for (auto& l : d) resting += l.qty;
  }
  // each trade consumes qty from two orders
  CHECK_EQ(submitted, resting + 2 * traded + cancelled_or_expired);
}

// ------------------------------------------------------------------------------------------------
// FlatIdMap
// ------------------------------------------------------------------------------------------------

TEST(flat_id_map_basic) {
  FlatIdMap m(4);
  CHECK(m.insert(5, 50));
  CHECK(!m.insert(5, 51));
  CHECK_EQ(*m.find(5), 50u);
  CHECK(m.find(6) == nullptr);
  CHECK(m.erase(5));
  CHECK(!m.erase(5));
  CHECK(m.empty());
}

TEST(flat_id_map_fuzz_vs_unordered_map) {
  Rng rng(7);
  FlatIdMap m(16);
  std::unordered_map<std::uint64_t, std::uint32_t> ref;
  for (int i = 0; i < 400000; ++i) {
    // small key space => constant collisions, probe-chain shifting, and growth
    const std::uint64_t k = static_cast<std::uint64_t>(rng.range(0, 3000)) * 1024;  // clustered keys
    const int op = static_cast<int>(rng.range(0, 2));
    if (op == 0) {
      const auto v = static_cast<std::uint32_t>(i);
      const bool a = m.insert(k, v);
      const bool b = ref.emplace(k, v).second;
      CHECK_EQ(a, b);
    } else if (op == 1) {
      CHECK_EQ(m.erase(k), ref.erase(k) == 1);
    } else {
      const auto* p = m.find(k);
      const auto it = ref.find(k);
      CHECK_EQ(p != nullptr, it != ref.end());
      if (p && it != ref.end()) CHECK_EQ(*p, it->second);
    }
    if (i % 4096 == 0) CHECK_EQ(m.size(), ref.size());
  }
  for (const auto& [k, v] : ref) {
    const auto* p = m.find(k);
    CHECK(p != nullptr);
    if (p) CHECK_EQ(*p, v);
  }
  CHECK_EQ(m.size(), ref.size());
}
