# Changelog

## 0.1.0

The first release of the rewrite. **The broker is not yet functional:** this
release is the MQTT codec the stack's projects link, and nothing that serves
clients.

- `lir::mqtt`: every control packet of MQTT 5.0, 3.1.1 and 3.1, encoded and
  decoded, with every MUST the specification states about a packet's own
  bytes checked on both sides; MQTT 5.0 properties checked against the packet
  that carries them; the fixed header framed from a stream before the body
  has to be held.
- Topic names, topic filters, shared subscriptions (`$share/<group>/<filter>`)
  and matching, the `$` rule included; `covers` for checking a subscription
  against a granted filter.
- Tested against the specification's statements by identifier and against a
  real MQTT client (the mosquitto command-line clients); three libFuzzer
  targets (packets, properties, topics) under ASan and UBSan.
- Debian packages for trixie on x86-64 and arm64: `liblir-mqtt0.1` and
  `liblir-dev`, with a CMake package (`find_package(lir)`, `lir::mqtt`).
- The contract other systems build against, `docs/contract.md`, kept from the
  first Lir.
