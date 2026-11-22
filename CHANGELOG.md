# Changelog

## 0.2.0

**The broker is still not functional:** this release adds the client the
stack's projects publish and subscribe with, and nothing that serves
clients.

- `lir::client`, the contract's MQTT client (`lir/client/client.h`): MQTT
  5.0 or 3.1.1 over TCP or TLS on Dannan's connector, a client certificate
  optional; Lir's nodes found in Dannan's roster by their `lir` advert and
  tried in turn, then Dannan's backoff with jitter; a password asked at
  every connect; QoS 0 and 1, QoS 1 sent again with DUP after a reconnect
  that kept the session; a will; wildcard subscriptions subscribed again
  when the broker kept no session; the broker's CONNACK limits honoured;
  every callback posted to the caller's executor, and time injected.
- Debian package `liblir-client0.2`; the codec's is now `liblir-mqtt0.2`.
- Built on Dannan 0.10.2 (`libdannan0.10`); `lir::mqtt`'s UTF-8 check is
  Dannan's.
- The fuzz targets register with Dannan's fuzz harness, and the test
  binary smokes each one on every run.

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
