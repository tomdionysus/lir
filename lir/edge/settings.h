//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "dannan/config/config.h"
#include "dannan/node.h"
#include "lir/auth/anonymous.h"
#include "lir/edge/server.h"

// lir-node's own settings. Its main is Dannan's node-program helper, which
// reads cluster.*, log.* and node.* for every node program of the stack, in
// the stack's pattern (Dannan's docs/config.md): defaults, the YAML file
// (--config, $LIR_CONFIG, else /etc/lir/config.yaml if it exists),
// secrets.yaml beside it, LIR_* variables, then flags. Lir adds:
//
//   mqtt, mqtts, ws, wss: each listener named as every program names its
//     own (Dannan's add_listener_settings): <name>.listen.address,
//     <name>.listen.port, <name>.tls.certificate, <name>.tls.key,
//     <name>.tls.ca. mqtt is always served (1883); mqtts (8883) and wss
//     (8443) when given their pair; ws (8080) when given a port.
//   mqtt.advertise, mqtt.client_certificates (never, optional, required),
//     mqtt.maximum_packet_size, mqtt.topic_alias_maximum
//   anonymous.allow, anonymous.publish, anonymous.retain, anonymous.subscribe
//   lugh.issuers, lugh.audience, lugh.ca (with the Lugh plugin)
namespace lir::edge {

inline constexpr std::string_view settings_prefix = "LIR";

struct LirSettings {
  ServerConfig server;
  // Nothing: anonymous clients are refused (the design's section 1).
  std::optional<auth::Filters> anonymous;
  // The Lugh plugin's trusted issuers; none: no tokens are taken.
  std::vector<std::string> lugh_issuers;
  std::string lugh_audience;
  std::string lugh_ca;
};

// Adds Lir's own settings to the helper's reader.
void add_settings(dannan::config::Reader& reader);

// Lir's rule for a node, after Dannan's: a place to keep its identity, and
// a way to prove it to the cluster. Throws dannan::config::ConfigError.
void require_node(const dannan::config::Settings& settings, const dannan::NodeConfig& node);

// Lir's settings, checked. Throws dannan::config::ConfigError naming the
// key and the layer that gave it: a value that cannot be used, TLS on a
// plaintext listener, anonymous filters given while anonymous clients are
// not allowed, or Lugh settings without the plugin or without an audience.
LirSettings lir_settings(const dannan::config::Settings& settings);

}  // namespace lir::edge
