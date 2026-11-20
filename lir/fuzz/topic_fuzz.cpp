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

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "lir/mqtt/topic.h"
#include "lir/mqtt/wire.h"

namespace {

using namespace lir::mqtt;

bool plain_filter(std::string_view filter) { return valid_topic_filter(filter) && !is_shared(filter); }

void check_one(std::string_view topic) {
  const auto parts = levels(topic);
  std::string joined;
  for (size_t index = 0; index < parts.size(); ++index) {
    if (index > 0) joined += '/';
    joined += parts[index];
  }
  if (joined != topic) std::abort();

  if (valid_topic_name(topic)) {
    if (has_wildcard(topic) || !valid_topic_filter(topic)) std::abort();
    if (!matches(topic, topic)) std::abort();
  }
  if (plain_filter(topic) && !covers(topic, topic)) std::abort();

  if (const auto shared = parse_shared(topic)) {
    if (!is_shared(topic) || shared->group.empty()) std::abort();
    if (shared->group.find_first_of("/+#") != std::string::npos) std::abort();
    if (!valid_topic_filter(shared->filter)) std::abort();
    if ("$share/" + shared->group + "/" + shared->filter != topic) std::abort();
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  std::vector<std::string_view> topics;
  size_t start = 0;
  while (topics.size() < 3) {
    const size_t end = input.find('\0', start);
    topics.push_back(input.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  for (const auto topic : topics) {
    if (!valid_utf8(topic)) return 0;
  }
  for (const auto topic : topics) check_one(topic);
  if (topics.size() < 2) return 0;

  const auto granted = topics[0];
  const auto requested = topics[1];
  if (plain_filter(granted) && valid_topic_name(requested) && !is_shared(requested)) {
    if (covers(granted, requested) != matches(granted, requested)) std::abort();
  }
  if (topics.size() == 3 && plain_filter(granted) && plain_filter(requested) && valid_topic_name(topics[2])) {
    if (covers(granted, requested) && matches(requested, topics[2]) && !matches(granted, topics[2])) std::abort();
  }
  return 0;
}
