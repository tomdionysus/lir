//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "lir/contract/endpoints.h"

// Balancing clients at CONNECT (design, section 7; Tom 2026-11-04): a node
// with clearly more clients than the cluster's average tells a new MQTT 5.0
// client to use the least-loaded node instead (CONNACK 0x9C with Server
// Reference). Live connections are never moved; 3.x clients, which cannot
// be redirected, are always served. Pure, for tests; the cluster feeds it
// the figures each node advertises.
namespace lir::cluster {

// Where a node serves clients, carried in its Dannan record
// (lir/contract/endpoints.h, shared with lir::client).
using contract::Endpoints;
using contract::endpoints_advert;
using contract::parse_endpoints;

struct Load {
  std::string node;
  uint64_t clients = 0;
  Endpoints endpoints;
};

// Where to send a client arriving on `transport` at `self`, or nothing to
// serve it here: only when self has more than (1 + over) times the average
// over every node, and another node serving that transport has at least two
// fewer. The least-loaded such node, the lowest id among equals. A negative
// `over` turns balancing off.
struct Redirect {
  std::string node;
  std::string address;
};
std::optional<Redirect> choose_redirect(const Load& self, const std::vector<Load>& others, const std::string& transport, double over);

}  // namespace lir::cluster
