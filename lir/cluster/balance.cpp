//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/cluster/balance.h"

namespace lir::cluster {

std::optional<Redirect> choose_redirect(const Load& self, const std::vector<Load>& others, const std::string& transport, double over) {
  if (over < 0 || others.empty()) return std::nullopt;
  uint64_t total = self.clients;
  for (const auto& other : others) total += other.clients;
  const double average = static_cast<double>(total) / static_cast<double>(others.size() + 1);
  if (static_cast<double>(self.clients) <= average * (1.0 + over)) return std::nullopt;
  const Load* best = nullptr;
  for (const auto& other : others) {
    if (!other.endpoints.contains(transport) || other.clients + 2 > self.clients) continue;
    if (!best || other.clients < best->clients || (other.clients == best->clients && other.node < best->node)) best = &other;
  }
  if (!best) return std::nullopt;
  return Redirect{best->node, best->endpoints.at(transport)};
}

}  // namespace lir::cluster
