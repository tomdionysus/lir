//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <vector>

#include "lir/cluster/sequence.h"

// A node's sequence (step 5 design, "The sequence counter"): a dot is
// (origin, seq), seq never repeats across restarts, and blocks are reserved
// durably so a publish costs no commit.
namespace lir::cluster {
namespace {

namespace asio = boost::asio;

// Holds each raise until the test settles it, as a node-local space's commit
// would complete later.
class HeldStore final : public SequenceStore {
 public:
  struct Raise {
    uint64_t bound = 0;
    asio::any_io_executor executor;
    Raised done;
  };

  std::optional<uint64_t> stored;
  std::vector<Raise> pending;

  std::optional<uint64_t> bound() override { return stored; }

  void raise(uint64_t bound, std::chrono::milliseconds, asio::any_io_executor executor, Raised done) override {
    pending.push_back({bound, std::move(executor), std::move(done)});
  }

  // Settles the oldest raise: durable, or refused with a reason.
  void settle(bool committed) {
    ASSERT_FALSE(pending.empty());
    auto raise = std::move(pending.front());
    pending.erase(pending.begin());
    if (committed) stored = raise.bound;
    SequenceRaised answer{committed, committed ? "" : "the state device refused"};
    asio::post(raise.executor, [done = std::move(raise.done), answer] { done(answer); });
  }
};

struct Opened {
  bool called = false;
  SequenceRaised answer;
};

// Runs what is posted. An io_context stops itself once it runs out of work,
// so it is restarted first.
void drain(asio::io_context& io) {
  io.restart();
  io.poll();
}

SequenceBlocks::Options blocks_of(uint64_t n) {
  SequenceBlocks::Options options;
  options.block = n;
  return options;
}

TEST(SequenceBlocks, ANewNodeStartsAtOneOnceItsFirstBlockIsDurable) {
  asio::io_context io;
  HeldStore store;
  SequenceBlocks seq(store, io.get_executor(), blocks_of(4));
  Opened opened;
  seq.open([&opened](SequenceRaised a) { opened = {true, a}; });

  EXPECT_FALSE(seq.next());
  ASSERT_EQ(store.pending.size(), 1u);
  EXPECT_EQ(store.pending[0].bound, 5u);

  store.settle(true);
  drain(io);
  ASSERT_TRUE(opened.called);
  EXPECT_TRUE(opened.answer.committed);
  EXPECT_EQ(seq.next(), std::optional<uint64_t>(1));
}

// Everything below the stored bound may have been handed out by an earlier
// process, so a restart begins at the bound itself.
TEST(SequenceBlocks, ARestartStartsAtTheLastDurableBound) {
  asio::io_context io;
  HeldStore store;
  store.stored = 1000;
  SequenceBlocks seq(store, io.get_executor(), blocks_of(4));
  seq.open([](SequenceRaised) {});
  ASSERT_EQ(store.pending.size(), 1u);
  EXPECT_EQ(store.pending[0].bound, 1004u);
  store.settle(true);
  drain(io);
  EXPECT_EQ(seq.next(), std::optional<uint64_t>(1000));
}

TEST(SequenceBlocks, NothingIsHandedOutPastTheDurableBound) {
  asio::io_context io;
  HeldStore store;
  SequenceBlocks seq(store, io.get_executor(), blocks_of(4));
  seq.open([](SequenceRaised) {});
  store.settle(true);
  drain(io);

  // 1 to 4 are durable; the refill to 9 is asked for but not yet durable.
  for (uint64_t expected = 1; expected <= 4; ++expected) EXPECT_EQ(seq.next(), std::optional<uint64_t>(expected));
  EXPECT_FALSE(seq.next());
  EXPECT_EQ(seq.durable_to(), 5u);

  ASSERT_EQ(store.pending.size(), 1u);
  EXPECT_EQ(store.pending[0].bound, 9u);
  store.settle(true);
  drain(io);
  EXPECT_EQ(seq.next(), std::optional<uint64_t>(5));
  EXPECT_EQ(seq.durable_to(), 9u);
}

// The next block is asked for once half the current one is spent, and only
// one reservation is in flight at a time.
TEST(SequenceBlocks, TheNextBlockIsReservedWhenHalfIsSpent) {
  asio::io_context io;
  HeldStore store;
  SequenceBlocks seq(store, io.get_executor(), blocks_of(8));
  seq.open([](SequenceRaised) {});
  store.settle(true);
  drain(io);

  for (int i = 0; i < 3; ++i) seq.next();
  EXPECT_TRUE(store.pending.empty());
  EXPECT_FALSE(seq.refilling());
  seq.next();
  ASSERT_EQ(store.pending.size(), 1u);
  EXPECT_TRUE(seq.refilling());
  seq.next();
  seq.next();
  EXPECT_EQ(store.pending.size(), 1u);
}

TEST(SequenceBlocks, AFailedReserveIsAskedAgainAndHandsOutNothingNew) {
  asio::io_context io;
  HeldStore store;
  SequenceBlocks seq(store, io.get_executor(), blocks_of(2));
  seq.open([](SequenceRaised) {});
  store.settle(true);
  drain(io);

  EXPECT_EQ(seq.next(), std::optional<uint64_t>(1));
  ASSERT_EQ(store.pending.size(), 1u);
  store.settle(false);
  drain(io);
  EXPECT_FALSE(seq.refilling());
  EXPECT_EQ(seq.durable_to(), 3u);

  EXPECT_EQ(seq.next(), std::optional<uint64_t>(2));
  EXPECT_FALSE(seq.next());
  ASSERT_EQ(store.pending.size(), 1u);
  EXPECT_EQ(store.pending[0].bound, 5u);
}

TEST(SequenceBlocks, AnOpenThatCannotReserveSaysWhyAndHandsOutNothing) {
  asio::io_context io;
  HeldStore store;
  SequenceBlocks seq(store, io.get_executor(), blocks_of(4));
  Opened opened;
  seq.open([&opened](SequenceRaised a) { opened = {true, a}; });
  store.settle(false);
  drain(io);
  ASSERT_TRUE(opened.called);
  EXPECT_FALSE(opened.answer.committed);
  EXPECT_FALSE(opened.answer.why.empty());
  EXPECT_FALSE(seq.next());
}

// The executor rule: an open's completion is posted, never called inline,
// even when the store has already answered.
TEST(SequenceBlocks, AnOpensCompletionIsPostedNeverCalledInline) {
  asio::io_context io;
  HeldStore store;
  SequenceBlocks seq(store, io.get_executor(), blocks_of(4));
  Opened opened;
  seq.open([&opened](SequenceRaised a) { opened = {true, a}; });
  store.settle(true);
  EXPECT_FALSE(opened.called);
  io.run_one();
  EXPECT_FALSE(opened.called);
  drain(io);
  EXPECT_TRUE(opened.called);
}

// A block of zero could never hand anything out.
TEST(SequenceBlocks, ABlockOfZeroIsRefused) {
  asio::io_context io;
  HeldStore store;
  EXPECT_THROW(SequenceBlocks(store, io.get_executor(), blocks_of(0)), std::invalid_argument);
}

}  // namespace
}  // namespace lir::cluster
