//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>

#include "dannan/context.h"
#include "lir/edge/edge.h"

// Lir's held messages in the node's one memory ledger (Dannan's budget(),
// shared by every system on the node; the maintainer 2026-11-04): leases, in chunks,
// follow the bytes the broker holds, so other systems' pressure presses on
// Lir and Lir's shows in Dannan's status. A lease the ledger refuses puts
// the broker at refuse_connect until room returns. Lir's own budget and
// thresholds apply as well.
namespace lir::system {

struct LedgerConfig {
  std::chrono::milliseconds every{1000};
  uint64_t chunk_bytes = 1u << 20;
};

class LedgerLeases {
 public:
  // The context and the edge outlive it.
  LedgerLeases(dannan::Context& context, edge::Edge& edge, LedgerConfig config = {});
  ~LedgerLeases();
  LedgerLeases(const LedgerLeases&) = delete;
  LedgerLeases& operator=(const LedgerLeases&) = delete;

  // Follows the broker every `every`; waits on the strand, never from an io
  // thread.
  void start();
  // Releases every lease.
  void stop();
  // How many chunks are leased now; for tests. Waits on the strand.
  size_t chunks();

 private:
  struct State;
  std::shared_ptr<State> _state;
};

}  // namespace lir::system
