# Lir - The Contract Kept Through the Rewrite

The maintainer, 2026-11-20: Lir is rewritten from scratch, and its existing contract is
stored, because other systems use it. This file is that contract: what the
rewrite must still provide, so that nothing built on Lir breaks. The code it
was taken from is `develop` at `e809114`; nothing else of that code binds the
rewrite.

The headers named below are kept verbatim under
[`contract-v1/`](contract-v1/), at the paths they had, as the reference. They
are not compiled. Where the rewrite changes one, it says so here first, and
the systems that use it are told before it lands.

## Who uses it

| System | Uses | Pinned at |
|---|---|---|
| Louhi | `lir::client` and the `lir::mqtt` types it exposes, fetched with `LIR_BUILD_SERVER=OFF` (`../louhi/CMakeLists.txt:75-82`, `louhi/events/lir.cpp`) | `e40dd2e` |
| Lugh | Its principal-events publisher is to use `lir::client` (`ACTIVE.md`, "Owed to others"); its topic-scope grammar for Lir (`lugh/scopes/topic_scope.h`) | not yet linked |
| Mimir | Its Lugh plugin is to use `lir::client` (named in Mimir's design only) | not yet linked |
| Lugh's `lugh_client` | `lir::mqtt`'s topic grammar (`topic.h`: validity, matching, coverage, shared filters) for its topic-scope checks; one copy, settled by the stack on 2026-11-20 (placement item 58) | to come |
| The Lugh plugin (in Lir) | `lir::contract::Authenticator`, `Grant`, `Peer` | in tree |
| Any MQTT client, the claim board's included | The wire, the transports, the token as password, the system topics | - |

A pinned commit keeps building whatever the rewrite does; the contract binds
what those systems get when they next move their pin.

## 1. The client library, `lir::client`

[`contract-v1/lir/client/client.h`](contract-v1/lir/client/client.h), whole:
`Client` (`start`, `stop`, `publish`, `subscribe`, `unsubscribe`,
`on_state`), `ClientConfig`, `Tls`, `Message`, `Outcome`,
`SubscribeOutcome`, `ConnectionState`, `StateChange`, `SubscriptionId`.

What it promises, as its header states: MQTT 5.0 or 3.1.1 over TCP or TLS; a
password asked at every CONNECT (a fresh Lugh token); QoS 0 and 1 publish and
receive; retained publish; a will; wildcard subscribe kept across
reconnects; reconnect with jittered backoff; finding Lir's nodes in the
process's Dannan roster by the `lir` advert and their `lir.endpoints`, or a
fixed host and port without Dannan. Thread-safe, every callback posted to
the client's strand, in order. Owed on request: QoS 2, topic aliases, user
properties.

## 2. The MQTT types the client exposes, `lir::mqtt`

`client.h` and `authenticator.h` name `mqtt::Bytes`, `mqtt::QoS`,
`mqtt::Version`, the reason-code constants (`mqtt::reason::*`) and
`mqtt::Connect` with its `Properties`. Louhi names `lir::mqtt::Bytes` and
`lir::mqtt::QoS` directly. Kept as declared in
[`packet.h`](contract-v1/lir/mqtt/packet.h),
[`properties.h`](contract-v1/lir/mqtt/properties.h) and
[`wire.h`](contract-v1/lir/mqtt/wire.h); `topic.h` is kept for reference
(topic validation and matching, which a client of the library may call).
The codec behind them is rewritten.

## 3. The build a consumer sees

- CMake targets `lir::mqtt` and `lir::client`; option `LIR_BUILD_SERVER`,
  which off builds only those two (Louhi forces it off). `lir::mqtt`
  builds alone on Dannan and nothing else, which `lugh_client` relies on.
- Dannan is taken from the consumer when it already has `dannan::dannan`
  (one Dannan a process), else fetched at Lir's pinned tag.
- Headers included from the repository root as `lir/client/client.h`,
  `lir/mqtt/packet.h`.

## 4. The authentication plug-in, `lir::contract`

[`authenticator.h`](contract-v1/lir/contract/authenticator.h): `Grant`
(`may_publish`, `may_subscribe`, `expires`), `Unrestricted`, `Nothing`,
`AuthDecision` (accept, refuse, challenge), `Peer`, `Authenticator`
(`connect`, `auth`), `AllowAll`. Authentication is pluggable (decided
2026-11-04); token verification stays Lugh's (`lugh_client`), through the
optional Lugh plugin.
[`connection.h`](contract-v1/lir/contract/connection.h) (`Connection`,
`ClientCertificate`) is the transport seam under a client, kept as a
reference, not a promise to others.

## 5. What a node says in Dannan's roster

[`endpoints.h`](contract-v1/lir/contract/endpoints.h): every Lir node
advertises `lir` (empty value) and `lir.endpoints`, the transports it serves
as `tcp=host:port;tls=host:port` (also `ws`, `wss`). `lir::client` finds Lir
by these; Dannan's roster is the one list of Lir's nodes (the maintainer, 2026-11-15).

## 6. What a client of the broker sees

- **The wire:** MQTT 5.0, 3.1.1 and 3.1, over TCP, TLS, WebSocket and
  secure WebSocket (subprotocols `mqtt`, `mqttv3.1`). MQTT-SN 1.2 over UDP
  and DTLS. What "supports MQTT" means is
  [`conformance.md`](conformance.md).
- **Signing in:** a Lugh access token as the CONNECT password, or by the
  MQTT 5 authentication method, when the Lugh plugin is on; anonymous
  clients refused unless a node allows them, then held to a configured
  scope set.
- **Topic scopes**, the grammar the maintainer decided on 2026-11-04
  (`../lugh/TODO/2026-11-04-oauth.md`, "Topic scopes, for Lir"):
  `"lir:" ("pub" | "sub" | "retain") ":" percent-encoded MQTT filter`.
  `pub` is a PUBLISH without the retain flag to a topic the filter
  matches; `retain` is one with it, and does not imply `pub`; `sub` is a
  SUBSCRIBE whose filter the granted one covers, the filter inside
  `$share/<g>/` for a shared one. Each level is decoded on its own, so an
  encoded `/` makes no level boundary. A level that is exactly `{sub}` is
  the token's subject.
- **The system topics**, JSON, subscribe-only for clients: `$lir/nodes/<id>`
  and `$dannan/nodes/<id>` (retained, each node its own), `$lir/cluster`
  (retained), `$lir/clients/connected` and `/disconnected` (events),
  `$dannan/roster/joined`, `/left`, `/changed` (events). The old code
  published `$lir/cluster` and the roster events from "the Lir node with
  the lowest id", a special node the premise forbids: the rewrite keeps the
  topics and finds them a shape with no special node, or asks the maintainer.
- **The operator's side** (the `lir-node` command line, `config.yaml`, the
  HTTP API, the rules engine, MQTT-SN's predefined topics as retained
  messages under `$lir/mqttsn/topics/`) is described in the old README at
  `e809114`. It is Lir's product, built again from the design's sections
  (`TODO/2026-11-04-lir-sections.md`), and not a contract with another
  system.

## 7. The HTTP endpoints the claim board uses

The development cluster's claim board talks to Lir over HTTP, so these are
kept, same paths and semantics, rebuilt on Dannan's HTTP core (the maintainer,
2026-11-20, Q1):
- `POST /publish?topic=&qos=&retain=`: the body is the payload; an empty
  body with `retain=1` clears that retained topic.
- `GET /retained?filter=`: a JSON array of `{topic, qos, retain, payload}`,
  with `payload_hex` in place of `payload` when the payload is not UTF-8.
- `GET /subscribe?filter=`: Server-Sent Events, one event per message.
