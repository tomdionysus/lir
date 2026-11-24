//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/edge/settings.h"

#include <cctype>
#include <stdexcept>
#include <utility>

#include "dannan/supervise/node_program.h"

namespace lir::edge {

namespace config = dannan::config;
namespace supervise = dannan::supervise;
using Kind = config::Setting::Kind;

namespace {

inline constexpr uint16_t mqtt_port = 1883;
inline constexpr uint16_t mqtts_port = 8883;
inline constexpr uint16_t ws_port = 8080;
inline constexpr uint16_t wss_port = 8443;

// The flag and the variable made from the key, by docs/config.md's rule.
config::Setting setting(std::string key, Kind kind, std::string what) {
  std::string flag = "--";
  std::string variable = std::string(settings_prefix) + "_";
  for (const char c : key) {
    flag += (c == '.' || c == '_') ? '-' : c;
    variable += (c == '.' || c == '-') ? '_' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return {std::move(key), kind, std::move(flag), std::move(variable), std::move(what)};
}

Endpoint endpoint_of(const supervise::ListenerSettings& listener) {
  Endpoint out;
  if (!listener.address.empty()) out.address = listener.address;
  out.port = listener.port;
  out.tls = listener.tls;
  return out;
}

}  // namespace

void add_settings(config::Reader& reader) {
  const std::string prefix(settings_prefix);
  supervise::add_listener_settings(reader, prefix, "mqtt", mqtt_port);
  supervise::add_listener_settings(reader, prefix, "mqtts", mqtts_port);
  supervise::add_listener_settings(reader, prefix, "ws", ws_port);
  supervise::add_listener_settings(reader, prefix, "wss", wss_port);
  for (auto s : {
           setting("mqtt.advertise", Kind::value, "the host clients dial"),
           setting("mqtt.client_certificates", Kind::value, "never, optional or required, on mqtts and wss"),
           setting("mqtt.maximum_packet_size", Kind::value, "bytes, 128 to 268435455"),
           setting("mqtt.topic_alias_maximum", Kind::value, "the most topic aliases a client may set, 0 to 65535"),
           setting("anonymous.allow", Kind::toggle, "whether clients with no credential are served"),
           setting("anonymous.publish", Kind::list, "topic filters an anonymous client may publish to"),
           setting("anonymous.retain", Kind::list, "topic filters an anonymous client may retain on"),
           setting("anonymous.subscribe", Kind::list, "topic filters an anonymous client may subscribe to"),
           setting("lugh.issuers", Kind::list, "the Lugh issuers whose tokens are taken"),
           setting("lugh.audience", Kind::value, "the audience a token must name"),
           setting("lugh.ca", Kind::value, "the roots the issuers' HTTPS chains to, PEM"),
       })
    reader.settings.push_back(std::move(s));
}

void require_node(const config::Settings& settings, const dannan::NodeConfig& node) {
  if (node.state_path.empty()) throw config::ConfigError("cluster.state is required: the node's state directory");
  if (!settings.find("cluster.key") && !settings.toggle("cluster.identity.no_cluster_key").value_or(false))
    throw config::ConfigError("cluster.key is required, unless the node runs by certificate alone (cluster.identity.no_cluster_key)");
}

LirSettings lir_settings(const config::Settings& settings) {
  LirSettings out;
  auto& server = out.server;

  // The listeners: mqtt and ws plaintext, mqtts and wss TLS.
  server.mqtt = endpoint_of(supervise::listener_settings(settings, "mqtt", mqtt_port));
  if (server.mqtt.tls) throw settings.refuse("mqtt.tls.certificate", "not given: mqtt is plaintext, and MQTT over TLS is mqtts");
  if (const auto mqtts = supervise::listener_settings(settings, "mqtts", mqtts_port); mqtts.tls) {
    server.mqtts = endpoint_of(mqtts);
  } else if (const auto* given = settings.find("mqtts.listen.port")      ? "mqtts.listen.port"
                                 : settings.find("mqtts.listen.address") ? "mqtts.listen.address"
                                                                         : nullptr) {
    throw settings.refuse(given, "given with mqtts.tls.certificate and mqtts.tls.key");
  }
  if (settings.find("ws.listen.port") || settings.find("ws.listen.address")) {
    server.ws = endpoint_of(supervise::listener_settings(settings, "ws", ws_port));
    if (server.ws->tls) throw settings.refuse("ws.tls.certificate", "not given: ws is plaintext, and WebSocket on TLS is wss");
  }
  if (const auto wss = supervise::listener_settings(settings, "wss", wss_port); wss.tls) {
    server.wss = endpoint_of(wss);
  } else if (const auto* given = settings.find("wss.listen.port") ? "wss.listen.port" : settings.find("wss.listen.address") ? "wss.listen.address" : nullptr) {
    throw settings.refuse(given, "given with wss.tls.certificate and wss.tls.key");
  }
  server.advertise = settings.text("mqtt.advertise").value_or("");
  if (const auto mode = settings.choice("mqtt.client_certificates", {"never", "optional", "required"})) {
    using dannan::listen::ClientCertificates;
    server.client_certificates = *mode == "never"      ? ClientCertificates::never
                                 : *mode == "optional" ? ClientCertificates::optional
                                                       : ClientCertificates::required;
  }
  server.limits.maximum_packet_size =
      static_cast<uint32_t>(settings.number("mqtt.maximum_packet_size", 128, 268435455).value_or(server.limits.maximum_packet_size));
  server.limits.topic_alias_maximum = static_cast<uint16_t>(settings.number("mqtt.topic_alias_maximum", 0, 65535).value_or(server.limits.topic_alias_maximum));

  auth::Filters filters;
  filters.publish = settings.list("anonymous.publish");
  filters.retain = settings.list("anonymous.retain");
  filters.subscribe = settings.list("anonymous.subscribe");
  if (settings.toggle("anonymous.allow").value_or(false)) {
    try {
      // Checked now, so a bad filter stops the start, naming it.
      (void)auth::FilterGrant(filters);
    } catch (const std::invalid_argument& error) {
      throw config::ConfigError(std::string("anonymous: ") + error.what());
    }
    out.anonymous = std::move(filters);
  } else if (!filters.publish.empty() || !filters.retain.empty() || !filters.subscribe.empty()) {
    throw config::ConfigError("anonymous.publish, anonymous.retain and anonymous.subscribe are set, but anonymous.allow is not");
  }

  out.lugh_issuers = settings.list("lugh.issuers");
  out.lugh_audience = settings.text("lugh.audience").value_or("");
  out.lugh_ca = settings.path_of("lugh.ca").value_or("");
#ifndef LIR_WITH_LUGH
  if (!out.lugh_issuers.empty()) throw settings.refuse("lugh.issuers", "set only in a lir-node built with the Lugh plugin");
#endif
  if (!out.lugh_issuers.empty() && out.lugh_audience.empty()) throw config::ConfigError("lugh.audience is required with lugh.issuers");
  return out;
}

}  // namespace lir::edge
