//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/system/ledger.h"

#include <boost/asio/steady_timer.hpp>
#include <vector>

namespace lir::system {

struct LedgerLeases::State : std::enable_shared_from_this<LedgerLeases::State> {
  State(dannan::Context& c, edge::Edge& e, LedgerConfig cfg) : context(c), edge(e), config(cfg), timer(e.strand()) {}

  dannan::Context& context;
  edge::Edge& edge;
  LedgerConfig config;
  boost::asio::steady_timer timer;
  // On the strand.
  bool running = false;
  std::vector<dannan::sched::RetainedMemoryLedger::Lease> leases;

  void follow() {
    auto& broker = edge.broker();
    const uint64_t held = broker.held_bytes();
    const size_t needed = static_cast<size_t>((held + config.chunk_bytes - 1) / config.chunk_bytes);
    while (leases.size() > needed) leases.pop_back();
    bool full = false;
    while (leases.size() < needed) {
      auto lease = context.budget().reserve(dannan::sched::WorkClass::foreground, config.chunk_bytes);
      if (!lease) {
        full = true;
        break;
      }
      leases.push_back(std::move(*lease));
    }
    broker.ledger_full(full);
  }

  void tick() {
    timer.expires_after(config.every);
    timer.async_wait([weak = weak_from_this()](const boost::system::error_code& ec) {
      const auto s = weak.lock();
      if (ec || !s || !s->running) return;
      s->follow();
      s->tick();
    });
  }
};

LedgerLeases::LedgerLeases(dannan::Context& context, edge::Edge& edge, LedgerConfig config) : _state(std::make_shared<State>(context, edge, config)) {}

LedgerLeases::~LedgerLeases() {
  try {
    stop();
  } catch (const std::exception&) {
    // Best effort here; stop() reports when called directly.
  }
}

void LedgerLeases::start() {
  _state->edge.call<bool>([s = _state](broker::Broker&) {
    if (s->running) return false;
    s->running = true;
    s->follow();
    s->tick();
    return true;
  });
}

void LedgerLeases::stop() {
  _state->edge.call<bool>([s = _state](broker::Broker& broker) {
    if (!s->running) return false;
    s->running = false;
    s->timer.cancel();
    s->leases.clear();
    broker.ledger_full(false);
    return true;
  });
}

size_t LedgerLeases::chunks() {
  return _state->edge.call<size_t>([s = _state](broker::Broker&) { return s->leases.size(); });
}

}  // namespace lir::system
