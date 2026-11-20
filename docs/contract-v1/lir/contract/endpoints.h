//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <map>
#include <string>
#include <string_view>

// What a lir-node says about itself in Dannan's roster, read by the cluster
// and by lir::client alike: Dannan's gossip is the one list of Lir's nodes
// (the maintainer, 2026-11-15), and nothing keeps another.
namespace lir::contract {

// Every lir-node advertises this key (its value empty).
inline constexpr std::string_view lir_advert = "lir";
// And where it serves clients, by transport ("tcp", "tls", "ws", "wss"), as
// "tcp=host:port;tls=host:port".
inline constexpr std::string_view endpoints_advert_key = "lir.endpoints";

using Endpoints = std::map<std::string, std::string>;

inline std::string endpoints_advert(const Endpoints& endpoints) {
  std::string out;
  for (const auto& [transport, address] : endpoints) {
    if (!out.empty()) out += ';';
    out += transport + '=' + address;
  }
  return out;
}

// Pairs that are not "name=value" are skipped.
inline Endpoints parse_endpoints(std::string_view advert) {
  Endpoints out;
  std::string_view rest = advert;
  while (!rest.empty()) {
    const size_t semi = rest.find(';');
    const auto pair = rest.substr(0, semi);
    const size_t equals = pair.find('=');
    if (equals != std::string_view::npos && equals > 0 && equals + 1 < pair.size())
      out[std::string(pair.substr(0, equals))] = std::string(pair.substr(equals + 1));
    if (semi == std::string_view::npos) break;
    rest.remove_prefix(semi + 1);
  }
  return out;
}

}  // namespace lir::contract
