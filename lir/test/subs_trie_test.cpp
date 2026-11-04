//
// Lir - The Distributed Event System
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <set>
#include <string>

#include "lir/mqtt/topic.h"
#include "lir/subs/trie.h"

namespace lir::subs {
namespace {

using Hits = std::multiset<std::string>;

Hits hits(const Trie<std::string, int>& trie, std::string_view name) {
  Hits out;
  trie.match(name, [&out](const std::string& key, int) { out.insert(key); });
  return out;
}

TEST(SubsTrie, MatchesExactAndWildcards) {
  Trie<std::string, int> trie;
  trie.insert("a/b", "exact", 0);
  trie.insert("a/+", "plus", 0);
  trie.insert("a/#", "hash", 0);
  trie.insert("#", "all", 0);
  trie.insert("+/b", "plus-first", 0);
  trie.insert("x/y", "other", 0);
  EXPECT_EQ(hits(trie, "a/b"), (Hits{"exact", "plus", "hash", "all", "plus-first"}));
  EXPECT_EQ(hits(trie, "a"), (Hits{"hash", "all"}));
  EXPECT_EQ(hits(trie, "a/b/c"), (Hits{"hash", "all"}));
  EXPECT_EQ(hits(trie, "q"), (Hits{"all"}));
}

TEST(SubsTrie, DollarNamesSkipLeadingWildcards) {
  Trie<std::string, int> trie;
  trie.insert("#", "all", 0);
  trie.insert("+/x", "plus", 0);
  trie.insert("$SYS/#", "sys", 0);
  trie.insert("$SYS/+", "sys-plus", 0);
  EXPECT_EQ(hits(trie, "$SYS/x"), (Hits{"sys", "sys-plus"}));
}

TEST(SubsTrie, SameKeyUnderTwoFiltersIsVisitedTwice) {
  Trie<std::string, int> trie;
  trie.insert("a/+", "s", 1);
  trie.insert("a/#", "s", 2);
  EXPECT_EQ(hits(trie, "a/b"), (Hits{"s", "s"}));
}

TEST(SubsTrie, InsertReplacesAndEraseRemoves) {
  Trie<std::string, int> trie;
  EXPECT_TRUE(trie.insert("a/b", "s", 1));
  EXPECT_FALSE(trie.insert("a/b", "s", 2));
  EXPECT_EQ(trie.size(), 1u);
  int seen = 0;
  trie.match("a/b", [&seen](const std::string&, int v) { seen = v; });
  EXPECT_EQ(seen, 2);
  EXPECT_FALSE(trie.erase("a/c", "s"));
  EXPECT_FALSE(trie.erase("a/b", "t"));
  EXPECT_TRUE(trie.erase("a/b", "s"));
  EXPECT_TRUE(trie.empty());
  EXPECT_EQ(hits(trie, "a/b"), Hits{});
}

TEST(SubsTrie, EachListsEveryEntryWithItsFilter) {
  Trie<std::string, int> trie;
  trie.insert("a/b", "s", 1);
  trie.insert("#", "t", 2);
  trie.insert("/x", "u", 3);
  std::set<std::string> seen;
  trie.each([&seen](const std::string& filter, const std::string& key, int) { seen.insert(filter + "=" + key); });
  EXPECT_EQ(seen, (std::set<std::string>{"a/b=s", "#=t", "/x=u"}));
}

// The trie and the reference matcher agree on random filters and names.
TEST(SubsTrie, AgreesWithReferenceMatcher) {
  std::mt19937 rng(7);
  const std::vector<std::string> words{"a", "b", "", "$SYS", "c"};
  auto random_levels = [&](bool filter) {
    std::string out;
    const int n = 1 + static_cast<int>(rng() % 4);
    for (int i = 0; i < n; ++i) {
      if (i) out += '/';
      const auto pick = rng() % (filter ? 7 : 5);
      if (pick == 5) {
        out += '+';
      } else if (pick == 6) {
        out += '#';
        break;
      } else {
        out += words[pick];
      }
    }
    return out;
  };
  Trie<std::string, int> trie;
  std::vector<std::string> filters;
  for (int i = 0; i < 200; ++i) {
    const auto f = random_levels(true);
    if (!mqtt::valid_topic_filter(f)) continue;
    filters.push_back(f);
    trie.insert(f, f, 0);
  }
  for (int i = 0; i < 500; ++i) {
    const auto name = random_levels(false);
    if (!mqtt::valid_topic_name(name)) continue;
    Hits expected;
    for (const auto& f : std::set<std::string>(filters.begin(), filters.end())) {
      if (mqtt::matches(f, name)) expected.insert(f);
    }
    EXPECT_EQ(hits(trie, name), expected) << name;
  }
}

}  // namespace
}  // namespace lir::subs
