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

#include "dannan/config/node_settings.h"
#include "lir/version.h"

namespace lir::edge {

namespace config = dannan::config;
using Kind = config::Setting::Kind;

namespace {

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

}  // namespace

config::Reader node_reader(const config::Environment* environment) {
  config::Reader out;
  out.environment = environment;
  out.config_variable = std::string(settings_prefix) + "_CONFIG";
  out.default_path = "/etc/lir/config.yaml";
  config::add_node_settings(out, settings_prefix);
  for (auto s : {
           setting("mqtt.address", Kind::value, "an address to serve MQTT on"),
           setting("mqtt.port", Kind::value, "a port, 0 to 65535"),
           setting("mqtt.advertise", Kind::value, "the host clients dial"),
           setting("mqtt.maximum_packet_size", Kind::value, "bytes, 128 to 268435455"),
           setting("mqtt.topic_alias_maximum", Kind::value, "the most topic aliases a client may set, 0 to 65535"),
           setting("mqtt.tls.port", Kind::value, "a port, 0 to 65535"),
           setting("mqtt.ws.port", Kind::value, "a port for MQTT over WebSocket, 0 to 65535"),
           setting("mqtt.wss.port", Kind::value, "a port for MQTT over WebSocket on TLS, 0 to 65535"),
           setting("mqtt.tls.certificate", Kind::value, "the certificate chain, PEM"),
           setting("mqtt.tls.key", Kind::value, "the certificate's key, PEM"),
           setting("mqtt.tls.client_ca", Kind::value, "the client certificates' CA bundle, PEM"),
           setting("mqtt.tls.client_certificates", Kind::value, "never, optional or required"),
           setting("anonymous.allow", Kind::toggle, "whether clients with no credential are served"),
           setting("anonymous.publish", Kind::list, "topic filters an anonymous client may publish to"),
           setting("anonymous.retain", Kind::list, "topic filters an anonymous client may retain on"),
           setting("anonymous.subscribe", Kind::list, "topic filters an anonymous client may subscribe to"),
           setting("lugh.issuers", Kind::list, "the Lugh issuers whose tokens are taken"),
           setting("lugh.audience", Kind::value, "the audience a token must name"),
           setting("lugh.ca", Kind::value, "the roots the issuers' HTTPS chains to, PEM"),
           setting("log.level", Kind::value, "debug, info, warn, error or all"),
           setting("log.format", Kind::value, "text or json"),
       })
    out.settings.push_back(std::move(s));
  return out;
}

NodeSettings node_settings(const config::Settings& settings) {
  NodeSettings out;
  out.node = config::node_config(settings);
  out.revocations = config::node_revocations(settings);
  out.node.version = std::string("lir-node ") + kVersion;
  // Lir's rule for a node: a place to keep its identity, and a way to prove
  // it to the cluster.
  if (out.node.state_path.empty()) throw config::ConfigError("cluster.state is required: the node's state directory");
  if (!settings.find("cluster.key") && !settings.toggle("cluster.identity.no_cluster_key").value_or(false))
    throw config::ConfigError("cluster.key is required, unless the node runs by certificate alone (cluster.identity.no_cluster_key)");

  auto& server = out.server;
  server.address = settings.text("mqtt.address").value_or(server.address);
  server.port = static_cast<uint16_t>(settings.number("mqtt.port", 0, 65535).value_or(server.port));
  server.advertise = settings.text("mqtt.advertise").value_or("");
  server.limits.maximum_packet_size =
      static_cast<uint32_t>(settings.number("mqtt.maximum_packet_size", 128, 268435455).value_or(server.limits.maximum_packet_size));
  server.limits.topic_alias_maximum = static_cast<uint16_t>(settings.number("mqtt.topic_alias_maximum", 0, 65535).value_or(server.limits.topic_alias_maximum));
  if (const auto port = settings.number("mqtt.ws.port", 0, 65535)) server.ws_port = static_cast<uint16_t>(*port);
  if (const auto port = settings.number("mqtt.wss.port", 0, 65535)) server.wss_port = static_cast<uint16_t>(*port);
  const auto certificate = settings.path_of("mqtt.tls.certificate");
  const auto key = settings.path_of("mqtt.tls.key");
  if (certificate.has_value() != key.has_value()) throw config::ConfigError("mqtt.tls.certificate and mqtt.tls.key go together");
  if (certificate) {
    server.tls = dannan::listen::CertificatePaths{*certificate, *key, settings.path_of("mqtt.tls.client_ca").value_or("")};
    server.tls_port = static_cast<uint16_t>(settings.number("mqtt.tls.port", 0, 65535).value_or(server.tls_port));
    if (const auto mode = settings.choice("mqtt.tls.client_certificates", {"never", "optional", "required"})) {
      using dannan::listen::ClientCertificates;
      server.client_certificates = *mode == "never"      ? ClientCertificates::never
                                   : *mode == "optional" ? ClientCertificates::optional
                                                         : ClientCertificates::required;
    }
  } else {
    for (const auto* tls_only : {"mqtt.tls.port", "mqtt.tls.client_ca", "mqtt.tls.client_certificates", "mqtt.wss.port"})
      if (settings.find(tls_only)) throw settings.refuse(tls_only, "set only with mqtt.tls.certificate and mqtt.tls.key");
  }

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

  try {
    if (const auto level = settings.text("log.level")) out.log_level = dannan::status::parse_log_level(*level);
  } catch (const std::exception&) {
    throw settings.refuse("log.level", "debug, info, warn, error or all");
  }
  try {
    if (const auto format = settings.text("log.format")) out.log_format = dannan::status::parse_log_format(*format);
  } catch (const std::exception&) {
    throw settings.refuse("log.format", "text or json");
  }
  return out;
}

}  // namespace lir::edge
