//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "dannan/fuzz/fuzz.h"

// Every parser of what clients send, through Dannan's smoke run on each gate:
// its seeds, then mutations of them, then noise. A finding is an exception
// out of the target (its own check failing, or anything the parser does not
// document); the libFuzzer binaries (tools/ci.sh fuzz) go further.
namespace lir {
namespace {

TEST(FuzzSmoke, EveryTargetIsRegistered) {
  std::vector<std::string> names;
  for (const auto& target : dannan::fuzz::targets()) names.push_back(target.name);
  std::sort(names.begin(), names.end());
  EXPECT_EQ(names, (std::vector<std::string>{"packet", "properties", "topic"}));
}

TEST(FuzzSmoke, EveryTargetHoldsOverItsSeedsMutationsAndNoise) {
  for (const auto& target : dannan::fuzz::targets()) {
    SCOPED_TRACE(target.name);
    EXPECT_GT(dannan::fuzz::smoke(target), dannan::fuzz::SmokeOptions{}.mutations);
  }
}

}  // namespace
}  // namespace lir
