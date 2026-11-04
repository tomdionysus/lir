//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/system/system.h"

#include <boost/asio/steady_timer.hpp>
#include <cstdio>
#include <sstream>

#include "lir/version.h"

namespace lir::system {

namespace {

namespace asio = boost::asio;
using dannan::identity::to_string;

std::string quoted(const std::string& text) { return "\"" + escaped(text) + "\""; }

broker::Message retained(std::string topic, const std::string& json) {
  broker::Message m;
  m.topic = std::move(topic);
  m.payload = broker::Bytes(json.begin(), json.end());
  m.retain = true;
  m.payload_format_indicator = 1;
  m.content_type = "application/json";
  return m;
}

broker::Message clearing(std::string topic) {
  broker::Message m;
  m.topic = std::move(topic);
  m.retain = true;
  return m;
}

}  // namespace

std::string escaped(const std::string& text) {
  std::string out;
  for (const char c : text) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (static_cast<unsigned char>(c) < 0x20) {
      char code[8];
      std::snprintf(code, sizeof(code), "\\u%04x", static_cast<unsigned>(c));
      out += code;
    } else {
      out += c;
    }
  }
  return out;
}

std::string lir_json(const std::string& node, size_t connections, size_t sessions, size_t retained_count, const broker::Stats& s) {
  std::ostringstream out;
  out << "{\"node\":" << quoted(node) << ",\"version\":" << quoted(kVersion) << ",\"connections\":" << connections << ",\"sessions\":" << sessions
      << ",\"retained\":" << retained_count << ",\"accepted\":" << s.connections_accepted << ",\"refused\":" << s.connections_refused
      << ",\"protocol_errors\":" << s.protocol_errors << ",\"received\":" << s.messages_received << ",\"delivered\":" << s.messages_delivered
      << ",\"injected\":" << s.messages_injected << ",\"wills\":" << s.wills_published << ",\"sessions_expired\":" << s.sessions_expired
      << ",\"sessions_taken\":" << s.sessions_taken << ",\"sessions_surrendered\":" << s.sessions_surrendered
      << ",\"dropped\":{\"offline_qos0\":" << s.dropped_offline_qos0 << ",\"queue_full\":" << s.dropped_queue_full << ",\"expired\":" << s.dropped_expired
      << ",\"too_large\":" << s.dropped_too_large << "}}";
  return out.str();
}

std::string dannan_json(const dannan::membership::NodeRecord& self, const std::vector<dannan::membership::NodeRecord>& active,
                        const std::map<dannan::identity::NodeId, std::chrono::milliseconds>& latencies) {
  std::ostringstream out;
  out << "{\"id\":" << quoted(to_string(self.id)) << ",\"host\":" << quoted(self.host) << ",\"port\":" << self.port
      << ",\"domain\":" << quoted(self.failure_domain) << ",\"incarnation\":" << quoted(to_string(self.incarnation))
      << ",\"path_class\":" << quoted(dannan::membership::path_class_name(self.path_class)) << ",\"inbound\":" << (self.inbound_capable() ? "true" : "false")
      << ",\"relay\":" << (self.relays() ? "true" : "false") << ",\"peers\":[";
  bool first = true;
  for (const auto& peer : active) {
    if (peer.id == self.id) continue;
    out << (first ? "" : ",") << "{\"id\":" << quoted(to_string(peer.id));
    if (const auto it = latencies.find(peer.id); it != latencies.end()) out << ",\"latency_ms\":" << it->second.count();
    out << "}";
    first = false;
  }
  out << "]}";
  return out.str();
}

struct SystemTopics::State : std::enable_shared_from_this<SystemTopics::State> {
  State(dannan::Context& c, edge::Edge& e, SystemConfig cfg) : context(c), edge(e), config(cfg), timer(e.strand()) {}

  dannan::Context& context;
  edge::Edge& edge;
  SystemConfig config;
  asio::steady_timer timer;
  // On the strand.
  bool running = false;
  uint64_t watch_id = 0;
  std::string last_lir;
  std::string last_dannan;

  std::string id() const { return to_string(context.identity().id()); }

  void publish() {
    auto& broker = edge.broker();
    const auto lir = lir_json(id(), broker.connections(), broker.sessions(), broker.retained(), broker.stats());
    if (lir != last_lir) {
      last_lir = lir;
      broker.publish(retained("$lir/nodes/" + id(), lir));
    }
    const auto snapshot = context.nodes().snapshot();
    const auto dannan = dannan_json(context.nodes().self(), snapshot.active, context.nodes().latencies());
    if (dannan != last_dannan) {
      last_dannan = dannan;
      broker.publish(retained("$dannan/nodes/" + id(), dannan));
    }
  }

  void tick() {
    timer.expires_after(config.every);
    timer.async_wait([weak = weak_from_this()](const boost::system::error_code& ec) {
      const auto s = weak.lock();
      if (ec || !s || !s->running) return;
      s->publish();
      s->tick();
    });
  }

  void on_roster(const dannan::RosterChange& change) {
    if (!running || change.kind != dannan::RosterChange::Kind::left || change.node.id == context.identity().id()) return;
    const auto gone = to_string(change.node.id);
    edge.broker().publish(clearing("$lir/nodes/" + gone));
    edge.broker().publish(clearing("$dannan/nodes/" + gone));
  }
};

SystemTopics::SystemTopics(dannan::Context& context, edge::Edge& edge, SystemConfig config) : _state(std::make_shared<State>(context, edge, config)) {}

SystemTopics::~SystemTopics() {
  try {
    stop();
  } catch (const std::exception&) {
    // Stopping is best effort here; stop() reports when called directly.
  }
}

void SystemTopics::start() {
  _state->edge.call<bool>([s = _state](broker::Broker&) {
    if (s->running) return false;
    s->running = true;
    std::weak_ptr<State> weak = s;
    s->watch_id = s->context.nodes().watch(s->edge.strand(), [weak](dannan::RosterChange change) {
      if (const auto st = weak.lock()) st->on_roster(change);
    });
    s->publish();
    s->tick();
    return true;
  });
}

void SystemTopics::stop() {
  _state->edge.call<bool>([s = _state](broker::Broker&) {
    if (!s->running) return false;
    s->running = false;
    s->context.nodes().unwatch(s->watch_id);
    s->timer.cancel();
    return true;
  });
}

}  // namespace lir::system
