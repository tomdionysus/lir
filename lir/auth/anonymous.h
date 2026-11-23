//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lir/contract/authenticator.h"

// Clients that sign in with nothing (the design's section 1): refused by
// default; a node may allow them, held to a configured set of topic filters.
// A client that presents a credential is not anonymous, and is refused here,
// since nothing here can check it: credentials are the Lugh plugin's.
namespace lir::auth {

// Topic filters, as MQTT writes them: `publish` lets a PUBLISH without the
// retain flag reach a topic one of them matches, `retain` one with it (and
// does not imply `publish`), `subscribe` a SUBSCRIBE whose filter one of them
// covers, the filter inside `$share/<group>/` for a shared one. As Lugh's
// topic scopes mean the same three words.
struct Filters {
  std::vector<std::string> publish;
  std::vector<std::string> retain;
  std::vector<std::string> subscribe;
};

class FilterGrant final : public contract::Grant {
 public:
  // Throws std::invalid_argument naming a filter that is not a valid MQTT
  // topic filter, or is shared.
  explicit FilterGrant(Filters filters);
  bool may_publish(std::string_view topic, bool retain) const override;
  bool may_subscribe(std::string_view filter) const override;

 private:
  Filters _filters;
};

class Anonymous final : public contract::Authenticator {
 public:
  // Nothing: every client is refused. Otherwise an anonymous client is held
  // to `allowed`; throws as FilterGrant does.
  explicit Anonymous(std::optional<Filters> allowed);

  contract::AuthDecision connect(const mqtt::Connect& connect, const contract::Peer& peer) override;
  contract::AuthDecision auth(const std::string& client_id, const std::string& method, const mqtt::Bytes& data, const contract::Peer& peer) override;

 private:
  std::shared_ptr<const FilterGrant> _grant;
};

}  // namespace lir::auth
