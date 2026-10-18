#pragma once
// Building blocks of the discrete-event simulator: venue configuration, the event queue, and the
// network model (one-way latency + jitter on FIFO channels).
//
// Simulated time only moves when an event is popped, so a run is a pure function of its seed:
// identical config + seed => identical event order, fills, and P&L.

#include <algorithm>
#include <cstdint>
#include <queue>
#include <string>
#include <variant>
#include <vector>

#include "qc/events.hpp"
#include "qc/order_book.hpp"
#include "qc/rng.hpp"
#include "qc/router.hpp"
#include "qc/types.hpp"

namespace qc {

using StrategyId = std::uint32_t;
inline constexpr ClientId kFirmClient = 1;  // our own account; background flow uses ids >= 1000

struct VenueConfig {
  VenueId id = 0;
  std::string name;
  double taker_fee_bps = 0.0;
  double maker_fee_bps = 0.0;        // negative = rebate
  Nanos latency_in = 100 * kMicro;   // firm -> venue, one way
  Nanos latency_out = 100 * kMicro;  // venue -> firm (execution reports)
  Nanos md_latency = 50 * kMicro;    // venue -> firm (market data)
  Nanos jitter = 20 * kMicro;        // uniform extra delay in [0, jitter] per message
  // background (noise) flow characteristics of this venue
  double flow_rate_per_sec = 5000.0;
  double qty_scale = 1.0;            // relative size of background orders (liquidity depth)
  double quote_noise_ticks = 1.0;    // amplitude of the venue's idiosyncratic quote offset
};

// ---- events ---------------------------------------------------------------------------------------

struct EvFundamentalStep {};
struct EvNoise { VenueId venue; };
struct EvOrderArrive { VenueId venue; Command cmd; };
struct EvReportArrive { ExecReport rep; };
struct EvQuoteArrive { DepthSnapshot snap; };
struct EvTimer { StrategyId strategy; std::uint32_t id; };
struct EvRetry { OrderId parent; };
struct EvSample {};

using EventPayload = std::variant<EvFundamentalStep, EvNoise, EvOrderArrive, EvReportArrive, EvQuoteArrive, EvTimer,
                                  EvRetry, EvSample>;

struct Event {
  Nanos time = 0;
  std::uint64_t seq = 0;  // tie-break: events at the same instant run in scheduling order
  EventPayload payload;
};

class EventQueue {
 public:
  Nanos now() const { return now_; }
  void schedule(Nanos at, EventPayload p) {
    if (at < now_) at = now_;  // never schedule into the past
    q_.push(Event{at, seq_++, std::move(p)});
  }
  bool empty() const { return q_.empty(); }
  Nanos next_time() const { return q_.top().time; }
  Event pop() {
    Event e = q_.top();
    q_.pop();
    now_ = e.time;
    return e;
  }
  std::uint64_t scheduled() const { return seq_; }

 private:
  struct Later {
    bool operator()(const Event& a, const Event& b) const {
      return a.time != b.time ? a.time > b.time : a.seq > b.seq;
    }
  };
  std::priority_queue<Event, std::vector<Event>, Later> q_;
  Nanos now_ = 0;
  std::uint64_t seq_ = 0;
};

// One-way latency model. Each (venue, direction) is a FIFO channel (like a TCP session or a
// sequenced multicast feed): jitter delays messages but can never reorder them.
class Network {
 public:
  Network(const std::vector<VenueConfig>& venues, std::uint64_t seed) : venues_(venues), rng_(seed), chan_(venues.size() * 3, 0) {}

  Nanos to_venue(VenueId v, Nanos now) { return deliver(v, 0, now, venues_[v].latency_in); }
  Nanos from_venue(VenueId v, Nanos now) { return deliver(v, 1, now, venues_[v].latency_out); }
  Nanos market_data(VenueId v, Nanos now) { return deliver(v, 2, now, venues_[v].md_latency); }

 private:
  Nanos deliver(VenueId v, std::size_t dir, Nanos now, Nanos base) {
    Nanos extra = venues_[v].jitter > 0 ? static_cast<Nanos>(rng_.uniform() * static_cast<double>(venues_[v].jitter)) : 0;
    Nanos& last = chan_[v * 3 + dir];
    const Nanos at = std::max(now + base + extra, last);
    last = at;
    return at;
  }
  const std::vector<VenueConfig>& venues_;
  Rng rng_;
  std::vector<Nanos> chan_;
};

}  // namespace qc
