//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/cluster/sequence.h"

#include <boost/asio/post.hpp>
#include <stdexcept>

namespace lir::cluster {

struct SequenceBlocks::State : std::enable_shared_from_this<SequenceBlocks::State> {
  State(SequenceStore& s, boost::asio::any_io_executor e, Options o) : store(s), executor(std::move(e)), options(o) {}

  SequenceStore& store;
  boost::asio::any_io_executor executor;
  Options options;
  bool open = false;
  bool refilling = false;
  uint64_t next = 0;
  uint64_t durable_to = 0;

  // Asks for `bound`; the answer is `landed` on the executor. The blocks own
  // the only reference, so an answer after they are gone lands in nothing.
  void reserve(uint64_t bound, std::function<void(SequenceRaised)> landed) {
    refilling = true;
    std::weak_ptr<State> weak = shared_from_this();
    store.raise(bound, options.deadline, executor, [weak, bound, landed = std::move(landed)](SequenceRaised answer) {
      const auto self = weak.lock();
      if (!self) return;
      self->refilling = false;
      if (answer.committed && bound > self->durable_to) self->durable_to = bound;
      landed(std::move(answer));
    });
  }

  void refill_if_low() {
    if (!open || refilling) return;
    if (durable_to - next > options.block / 2) return;
    reserve(durable_to + options.block, [](SequenceRaised) {});
  }
};

SequenceBlocks::SequenceBlocks(SequenceStore& store, boost::asio::any_io_executor executor, Options options) {
  if (options.block == 0) throw std::invalid_argument("a sequence block of zero hands nothing out");
  _state = std::make_shared<State>(store, std::move(executor), options);
}

SequenceBlocks::~SequenceBlocks() = default;

void SequenceBlocks::open(Opened done) {
  auto& s = *_state;
  // Everything below the stored bound may have been handed out by an earlier
  // process; zero is never a seq.
  s.next = s.store.bound().value_or(1);
  if (s.next == 0) s.next = 1;
  s.durable_to = s.next;
  std::weak_ptr<State> weak = _state;
  s.reserve(s.next + s.options.block, [weak, done = std::move(done)](SequenceRaised answer) {
    const auto self = weak.lock();
    if (!self) return;
    self->open = answer.committed;
    // The executor rule: posted again, so the caller never runs inside the
    // store's completion.
    boost::asio::post(self->executor, [done, answer = std::move(answer)] { done(answer); });
  });
}

std::optional<uint64_t> SequenceBlocks::next() {
  auto& s = *_state;
  if (!s.open) return std::nullopt;
  std::optional<uint64_t> out;
  if (s.next < s.durable_to) out = s.next++;
  s.refill_if_low();
  return out;
}

uint64_t SequenceBlocks::durable_to() const noexcept { return _state->durable_to; }

bool SequenceBlocks::refilling() const noexcept { return _state->refilling; }

}  // namespace lir::cluster
