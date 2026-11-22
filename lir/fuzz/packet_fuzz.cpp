//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Every byte a client sends: one whole packet, read as each of the three
// versions. A decode may throw only MalformedPacket. What decodes must encode,
// and the encoding must decode to the same packet: the server never sends a
// packet it would refuse, and never refuses one it would send. The fixed
// header of a packet that decodes must be read the same way from a stream
// that has only part of it.

#include <span>
#include <stdexcept>
#include <string>
#include <variant>

#include "dannan/fuzz/fuzz.h"
#include "lir/fuzz/seeds.h"
#include "lir/mqtt/packet.h"

namespace {

using namespace lir::mqtt;

void fail(const char* what) { throw std::logic_error(std::string("packet: ") + what); }

void check(std::span<const uint8_t> input, Version version) {
  Packet packet;
  try {
    packet = decode(input, version);
  } catch (const MalformedPacket&) {
    return;
  }
  // A CONNECT names its own version, and the session takes it from there.
  const auto* connect = std::get_if<Connect>(&packet);
  const Version speaks = connect ? connect->version : version;
  const Bytes encoded = encode(packet, speaks);
  if (!(decode(encoded, speaks) == packet)) fail("the encoding does not decode to the packet");

  const auto frame = read_frame(input, varint_max + 5);
  if (!frame || frame->size() != input.size()) fail("the frame is not the packet");
  for (size_t part = 0; part < frame->header_size; ++part) {
    if (read_frame(input.first(part), varint_max + 5)) fail("a partial header framed");
  }
  // A maximum below the packet's size refuses it before its body is read.
  try {
    (void)read_frame(input, static_cast<uint32_t>(input.size() - 1));
  } catch (const MalformedPacket& error) {
    if (error.reason() != reason::packet_too_large) fail("too large, refused for another reason");
    return;
  }
  fail("a packet over the maximum framed");
}

const dannan::fuzz::Registration registration(
    "packet",
    [](std::span<const uint8_t> input) {
      for (const auto version : {Version::v3_1, Version::v3_1_1, Version::v5}) check(input, version);
    },
    [] { return lir::fuzz::seeds("packet"); });

}  // namespace
