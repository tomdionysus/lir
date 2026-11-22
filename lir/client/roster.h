//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dannan/membership/node_record.h"

// Where lir::client finds Lir: the nodes of Dannan's roster that advertise
// `lir`, at their `lir.endpoints` (lir/contract/endpoints.h). The roster is
// the one list of Lir's nodes; the client keeps none of its own.
namespace lir::client {

struct Address {
  std::string host;
  uint16_t port = 0;
  bool operator==(const Address&) const = default;
};

// "host:port", "[v6]:port" or "v6-without-brackets:port" (the last colon
// splits); nothing for anything else.
std::optional<Address> parse_address(std::string_view address);

// Each record advertising `lir` that serves `transport` ("tcp" or "tls"), in
// the roster's order.
std::vector<Address> lir_addresses(const std::vector<dannan::membership::NodeRecord>& records, std::string_view transport);

}  // namespace lir::client
