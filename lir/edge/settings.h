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
#include "dannan/status/log.h"
#include "lir/auth/anonymous.h"
#include "lir/edge/server.h"

// lir-node's settings, read by Dannan's one reader in the stack's pattern
// (Dannan's docs/config.md): defaults, the YAML file (--config, $LIR_CONFIG,
// else /etc/lir/config.yaml if it exists), secrets.yaml beside it, LIR_*
// variables, then flags. The node's Dannan settings are Dannan's, under
// `cluster.*` (dannan/config/node_settings.h; the maintainer, 2026-11-22);
// Lir's own are:
//
//   mqtt.address, mqtt.port, mqtt.advertise, mqtt.maximum_packet_size,
//   mqtt.topic_alias_maximum, mqtt.ws.port, mqtt.wss.port
//   mqtt.tls.port, mqtt.tls.certificate, mqtt.tls.key, mqtt.tls.client_ca,
//   mqtt.tls.client_certificates (never, optional or required)
//   anonymous.allow, anonymous.publish, anonymous.retain, anonymous.subscribe
//   lugh.issuers, lugh.audience, lugh.ca (with the Lugh plugin)
//   log.level, log.format (the stack's shared keys)
namespace lir::edge {

inline constexpr std::string_view settings_prefix = "LIR";

struct NodeSettings {
  dannan::NodeConfig node;
  // Given to the node once it has started (Context::identity().revoke).
  std::vector<dannan::identity::Revocation> revocations;
  ServerConfig server;
  // Nothing: anonymous clients are refused (the design's section 1).
  std::optional<auth::Filters> anonymous;
  // The Lugh plugin's trusted issuers; none: no tokens are taken.
  std::vector<std::string> lugh_issuers;
  std::string lugh_audience;
  std::string lugh_ca;
  dannan::status::LogLevel log_level = dannan::status::LogLevel::info;
  dannan::status::LogFormat log_format = dannan::status::LogFormat::text;
};

// Every setting lir-node takes, Dannan's included.
dannan::config::Reader node_reader(const dannan::config::Environment* environment = &dannan::config::process_environment());

// The settings read, checked. Throws dannan::config::ConfigError naming the
// key and the layer that gave it: a value that cannot be used, a node with
// no state directory, or no cluster key and no certificate to run by,
// anonymous filters given while anonymous clients are not allowed, or Lugh
// settings without the plugin or without an audience.
NodeSettings node_settings(const dannan::config::Settings& settings);

}  // namespace lir::edge
