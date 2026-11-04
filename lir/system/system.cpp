//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/system/system.h"

#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <cstdio>
#include <set>
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

broker::Message event(std::string topic, const std::string& json) {
  auto m = retained(std::move(topic), json);
  m.retain = false;
  return m;
}

const char* version_name(mqtt::Version version) {
  switch (version) {
    case mqtt::Version::v3_1:
      return "3.1";
    case mqtt::Version::v3_1_1:
      return "3.1.1";
    case mqtt::Version::v5:
      return "5.0";
  }
  return "?";
}

const char* by_name(broker::ClientEvent::By by) {
  switch (by) {
    case broker::ClientEvent::By::client:
      return "client";
    case broker::ClientEvent::By::server:
      return "server";
    case broker::ClientEvent::By::network:
      return "network";
  }
  return "?";
}

// The adverts a Lir node puts in its Dannan record for the cluster summary.
constexpr const char* kConnections = "lir.connections";
constexpr const char* kSessions = "lir.sessions";
constexpr const char* kRate = "lir.received_per_second";
constexpr const char* kLimits = "lir.limits";

std::string text_of(std::span<const uint8_t> bytes) { return {bytes.begin(), bytes.end()}; }

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

std::string lir_json(const std::string& node, size_t connections, size_t sessions, size_t retained_count, const broker::Stats& s, uint64_t held_bytes,
                     uint64_t memory_budget, broker::Pressure pressure) {
  std::ostringstream out;
  out << "{\"node\":" << quoted(node) << ",\"version\":" << quoted(kVersion) << ",\"connections\":" << connections << ",\"sessions\":" << sessions
      << ",\"retained\":" << retained_count << ",\"accepted\":" << s.connections_accepted << ",\"refused\":" << s.connections_refused
      << ",\"protocol_errors\":" << s.protocol_errors << ",\"received\":" << s.messages_received << ",\"delivered\":" << s.messages_delivered
      << ",\"injected\":" << s.messages_injected << ",\"wills\":" << s.wills_published << ",\"sessions_expired\":" << s.sessions_expired
      << ",\"sessions_taken\":" << s.sessions_taken << ",\"sessions_surrendered\":" << s.sessions_surrendered
      << ",\"dropped\":{\"offline_qos0\":" << s.dropped_offline_qos0 << ",\"queue_full\":" << s.dropped_queue_full << ",\"expired\":" << s.dropped_expired
      << ",\"too_large\":" << s.dropped_too_large << ",\"pressure\":" << s.dropped_pressure << "},\"memory\":{\"held\":" << held_bytes
      << ",\"budget\":" << memory_budget << ",\"pressure\":" << quoted(broker::pressure_name(pressure))
      << ",\"refused_publishes\":" << s.refused_pressure_publishes << ",\"refused_connections\":" << s.refused_pressure_connections << "}}";
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

std::string client_event_json(const std::string& node, const broker::ClientEvent& e) {
  std::ostringstream out;
  out << "{\"client\":" << quoted(e.client_id) << ",\"node\":" << quoted(node) << ",\"version\":" << quoted(version_name(e.version))
      << ",\"peer\":" << quoted(e.peer);
  if (e.username) out << ",\"username\":" << quoted(*e.username);
  if (e.kind == broker::ClientEvent::Kind::connected) {
    out << ",\"session_present\":" << (e.session_present ? "true" : "false");
  } else {
    out << ",\"by\":" << quoted(by_name(e.by));
    if (e.reason) out << ",\"reason\":" << static_cast<unsigned>(*e.reason);
  }
  out << "}";
  return out.str();
}

std::string roster_event_json(const dannan::membership::NodeRecord& node) {
  std::ostringstream out;
  out << "{\"id\":" << quoted(to_string(node.id)) << ",\"host\":" << quoted(node.host) << ",\"port\":" << node.port
      << ",\"domain\":" << quoted(node.failure_domain) << ",\"incarnation\":" << quoted(to_string(node.incarnation))
      << ",\"path_class\":" << quoted(dannan::membership::path_class_name(node.path_class)) << ",\"inbound\":" << (node.inbound_capable() ? "true" : "false")
      << ",\"relay\":" << (node.relays() ? "true" : "false") << ",\"lir\":" << (node.advert("lir") ? "true" : "false") << "}";
  return out.str();
}

std::string limits_advert(const broker::Config& c) {
  std::ostringstream out;
  out << "maximum_packet_size=" << c.maximum_packet_size << ";receive_maximum=" << c.receive_maximum << ";topic_alias_maximum=" << c.topic_alias_maximum
      << ";maximum_qos=" << static_cast<unsigned>(c.maximum_qos) << ";maximum_session_expiry=" << c.maximum_session_expiry
      << ";maximum_queued=" << c.maximum_queued << ";memory_budget=" << c.memory_budget << ";retain_available=" << c.retain_available
      << ";shared_subscription_available=" << c.shared_subscription_available;
  return out.str();
}

std::vector<std::string> differing_limits(const std::vector<std::string>& adverts) {
  std::map<std::string, std::set<std::string>> values;
  std::set<std::string> names;
  for (const auto& advert : adverts) {
    std::map<std::string, std::string> pairs;
    std::string_view rest = advert;
    while (!rest.empty()) {
      const size_t semi = rest.find(';');
      const auto pair = rest.substr(0, semi);
      const size_t equals = pair.find('=');
      if (equals != std::string_view::npos) pairs[std::string(pair.substr(0, equals))] = std::string(pair.substr(equals + 1));
      if (semi == std::string_view::npos) break;
      rest.remove_prefix(semi + 1);
    }
    for (const auto& [name, value] : pairs) names.insert(name);
    for (const auto& name : names) values[name].insert(pairs.contains(name) ? pairs[name] : "");
  }
  // A name one node lacks counts as differing.
  std::vector<std::string> out;
  for (const auto& name : names) {
    size_t having = 0;
    for (const auto& advert : adverts) {
      if (advert.find(name + "=") != std::string::npos) ++having;
    }
    if (values[name].size() > 1 || having != adverts.size()) out.push_back(name);
  }
  return out;
}

std::string cluster_json(const ClusterFigures& f) {
  std::ostringstream out;
  out << "{\"publisher\":" << quoted(f.publisher) << ",\"nodes\":" << f.nodes << ",\"connections\":" << f.connections << ",\"sessions\":" << f.sessions
      << ",\"received_per_second\":" << f.received_per_second << ",\"limits\":{\"consistent\":" << (f.differing.empty() ? "true" : "false")
      << ",\"differing\":[";
  for (size_t i = 0; i < f.differing.size(); ++i) out << (i ? "," : "") << quoted(f.differing[i]);
  out << "]}}";
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
  std::string last_cluster;
  // The rate's last sample.
  uint64_t last_received = 0;
  std::chrono::steady_clock::time_point last_sample;
  // Each node's record as the roster events show it, to tell a change worth
  // an event from an advert's.
  std::map<dannan::identity::NodeId, std::string> shown;

  std::string id() const { return to_string(context.identity().id()); }

  // The Lir node with the lowest id among `active` (less `gone`): the one
  // that publishes what every node would otherwise publish once each.
  bool publisher(const std::vector<dannan::membership::NodeRecord>& active, std::optional<dannan::identity::NodeId> gone = std::nullopt) const {
    std::optional<dannan::identity::NodeId> lowest;
    for (const auto& node : active) {
      if (gone && node.id == *gone) continue;
      const bool lir = node.id == context.identity().id() || node.advert("lir");
      if (lir && (!lowest || to_string(node.id) < to_string(*lowest))) lowest = node.id;
    }
    return lowest && *lowest == context.identity().id();
  }

  void advertise() {
    auto& broker = edge.broker();
    auto& nodes = context.nodes();
    const auto now = std::chrono::steady_clock::now();
    const uint64_t received = broker.stats().messages_received;
    uint64_t rate = 0;
    if (last_sample.time_since_epoch().count() != 0) {
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_sample).count();
      if (ms > 0) rate = (received - last_received) * 1000 / static_cast<uint64_t>(ms);
    }
    last_received = received;
    last_sample = now;
    // Only what changed: each advert travels in the node's gossiped record.
    const auto self = nodes.self();
    const auto set = [&](const char* key, const dannan::net::Bytes& value) {
      const auto held = self.advert(key);
      if (!held || !std::equal(held->begin(), held->end(), value.begin(), value.end())) nodes.advertise(key, value);
    };
    set(kConnections, dannan::membership::u64_advert(broker.clients()));
    set(kSessions, dannan::membership::u64_advert(broker.sessions()));
    set(kRate, dannan::membership::u64_advert(rate));
    const auto limits = limits_advert(broker.config());
    set(kLimits, dannan::net::Bytes(limits.begin(), limits.end()));
  }

  void publish() {
    auto& broker = edge.broker();
    advertise();
    if (config.nodes) {
      const auto lir = lir_json(id(), broker.connections(), broker.sessions(), broker.retained(), broker.stats(), broker.held_bytes(),
                                broker.config().memory_budget, broker.pressure());
      if (lir != last_lir) {
        last_lir = lir;
        broker.publish(retained("$lir/nodes/" + id(), lir));
      }
    }
    const auto snapshot = context.nodes().snapshot();
    if (config.nodes) {
      const auto dannan = dannan_json(context.nodes().self(), snapshot.active, context.nodes().latencies());
      if (dannan != last_dannan) {
        last_dannan = dannan;
        broker.publish(retained("$dannan/nodes/" + id(), dannan));
      }
    }
    if (config.cluster && publisher(snapshot.active)) {
      ClusterFigures figures;
      figures.publisher = id();
      std::vector<std::string> limits;
      for (const auto& node : snapshot.active) {
        const auto record = node.id == context.identity().id() ? context.nodes().self() : node;
        const auto advert = record.advert(kLimits);
        if (!advert) continue;
        ++figures.nodes;
        figures.connections += record.advert_u64(kConnections).value_or(0);
        figures.sessions += record.advert_u64(kSessions).value_or(0);
        figures.received_per_second += record.advert_u64(kRate).value_or(0);
        limits.push_back(text_of(*advert));
      }
      figures.differing = differing_limits(limits);
      const auto cluster = cluster_json(figures);
      if (cluster != last_cluster) {
        last_cluster = cluster;
        broker.publish(retained("$lir/cluster", cluster));
      }
    } else {
      last_cluster.clear();
    }
  }

  // Called by the broker, which must not be called back before it returns.
  void on_client(const broker::ClientEvent& e) {
    const auto topic = e.kind == broker::ClientEvent::Kind::connected ? "$lir/clients/connected" : "$lir/clients/disconnected";
    auto message = event(topic, client_event_json(id(), e));
    asio::post(edge.strand(), [weak = weak_from_this(), message = std::move(message)]() mutable {
      if (const auto s = weak.lock(); s && s->running) s->edge.broker().publish(std::move(message));
    });
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
    if (!running) return;
    using Kind = dannan::RosterChange::Kind;
    const auto json = roster_event_json(change.node);
    bool worth = true;
    if (change.kind == Kind::left) {
      shown.erase(change.node.id);
    } else {
      // An advert's change (a Lir node's figures) is not a roster event.
      auto& held = shown[change.node.id];
      worth = change.kind == Kind::joined || held != json;
      held = json;
    }
    if (config.roster && worth) {
      const auto snapshot = context.nodes().snapshot();
      const auto gone = change.kind == Kind::left ? std::optional(change.node.id) : std::nullopt;
      if (publisher(snapshot.active, gone)) edge.broker().publish(event(std::string("$dannan/roster/") + dannan::roster_change_name(change.kind), json));
    }
    if (change.kind != Kind::left || change.node.id == context.identity().id()) return;
    const auto left = to_string(change.node.id);
    edge.broker().publish(clearing("$lir/nodes/" + left));
    edge.broker().publish(clearing("$dannan/nodes/" + left));
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
    if (s->config.clients) {
      s->edge.broker().observe([weak](const broker::ClientEvent& e) {
        if (const auto st = weak.lock()) st->on_client(e);
      });
    }
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
    s->edge.broker().observe({});
    s->timer.cancel();
    return true;
  });
}

}  // namespace lir::system
