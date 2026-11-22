//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Topic names and filters as a client sends them, already valid UTF-8 as the
// codec guarantees: up to three strings split at U+0000, which no MQTT
// string may hold. The grammar is checked against itself: a granted filter
// covers a name exactly when it matches it, coverage carries through to what
// the covered filter matches, a shared filter is its parts, and the levels
// are the topic.

#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "dannan/fuzz/fuzz.h"
#include "lir/fuzz/seeds.h"
#include "lir/mqtt/topic.h"
#include "lir/mqtt/wire.h"

namespace {

using namespace lir::mqtt;

void fail(const char* what) { throw std::logic_error(std::string("topic: ") + what); }

bool plain_filter(std::string_view filter) { return valid_topic_filter(filter) && !is_shared(filter); }

void check_one(std::string_view topic) {
  const auto parts = levels(topic);
  std::string joined;
  for (size_t index = 0; index < parts.size(); ++index) {
    if (index > 0) joined += '/';
    joined += parts[index];
  }
  if (joined != topic) fail("the grammar disagrees with itself");

  if (valid_topic_name(topic)) {
    if (has_wildcard(topic) || !valid_topic_filter(topic)) fail("the grammar disagrees with itself");
    if (!matches(topic, topic)) fail("the grammar disagrees with itself");
  }
  if (plain_filter(topic) && !covers(topic, topic)) fail("the grammar disagrees with itself");

  if (const auto shared = parse_shared(topic)) {
    if (!is_shared(topic) || shared->group.empty()) fail("the grammar disagrees with itself");
    if (shared->group.find_first_of("/+#") != std::string::npos) fail("the grammar disagrees with itself");
    if (!valid_topic_filter(shared->filter)) fail("the grammar disagrees with itself");
    if ("$share/" + shared->group + "/" + shared->filter != topic) fail("the grammar disagrees with itself");
  }
}

void check(std::span<const uint8_t> data) {
  const std::string_view input(reinterpret_cast<const char*>(data.data()), data.size());
  std::vector<std::string_view> topics;
  size_t start = 0;
  while (topics.size() < 3) {
    const size_t end = input.find('\0', start);
    topics.push_back(input.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  for (const auto topic : topics) {
    if (!valid_utf8(topic)) return;
  }
  for (const auto topic : topics) check_one(topic);
  if (topics.size() < 2) return;

  const auto granted = topics[0];
  const auto requested = topics[1];
  if (plain_filter(granted) && valid_topic_name(requested) && !is_shared(requested)) {
    if (covers(granted, requested) != matches(granted, requested)) fail("the grammar disagrees with itself");
  }
  if (topics.size() == 3 && plain_filter(granted) && plain_filter(requested) && valid_topic_name(topics[2])) {
    if (covers(granted, requested) && matches(requested, topics[2]) && !matches(granted, topics[2])) fail("the grammar disagrees with itself");
  }
}

const dannan::fuzz::Registration registration("topic", check, [] { return lir::fuzz::seeds("topic"); });

}  // namespace
