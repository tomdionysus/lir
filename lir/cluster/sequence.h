//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

// A node's message sequence (step 5 design, "The sequence counter"). A dot is
// (origin, seq), and seq must never repeat across restarts. Committing every
// seq would put a commit on every publish, so seq is handed out from blocks
// whose upper bound is made durable first: a restart starts at the bound,
// leaving a gap but never reusing a number.
namespace lir::cluster {

// What making a bound durable came to. Not committed: `why` says why.
struct SequenceRaised {
  bool committed = false;
  std::string why;
};

// Where a node keeps its bound: a Dagda node-local space at step 5, which
// refuses a restored older file, so a block is never handed out twice.
class SequenceStore {
 public:
  using Raised = std::function<void(SequenceRaised)>;

  virtual ~SequenceStore() = default;
  // The bound this node last made durable; nothing on a node that never did.
  // Waits on the state device.
  virtual std::optional<uint64_t> bound() = 0;
  // Makes `bound` durable on this node, and posts `done` to `executor` once
  // it is or cannot be, at the latest when `deadline` has passed. Never calls
  // `done` inline. Waits on nothing itself.
  virtual void raise(uint64_t bound, std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, Raised done) = 0;
};

struct SequenceOptions {
  // The design's block (Tom, 2026-11-13).
  uint64_t block = 65536;
  // How long one reservation may take.
  std::chrono::milliseconds deadline{5000};
};

class SequenceBlocks {
 public:
  using Options = SequenceOptions;
  using Opened = std::function<void(SequenceRaised)>;

  // The store and the executor outlive this. Throws std::invalid_argument for
  // a block of zero. Waits on nothing.
  SequenceBlocks(SequenceStore& store, boost::asio::any_io_executor executor, Options options = {});
  // A reservation still in flight then lands in nothing.
  ~SequenceBlocks();
  SequenceBlocks(const SequenceBlocks&) = delete;
  SequenceBlocks& operator=(const SequenceBlocks&) = delete;

  // Reads the bound and reserves the first block; `done` is posted to the
  // executor once that block is durable or cannot be. Waits on the store's
  // read. Once only.
  void open(Opened done);
  // The next seq, never one this node handed out before in any process; or
  // nothing until a reserved block is durable. Once half a block is spent the
  // next is reserved in the background, one reservation at a time. Waits on
  // nothing. On the executor only, like every member.
  std::optional<uint64_t> next();
  // One past the last seq that may be handed out now.
  uint64_t durable_to() const noexcept;
  // A reservation is in flight.
  bool refilling() const noexcept;

 private:
  struct State;
  std::shared_ptr<State> _state;
};

}  // namespace lir::cluster
