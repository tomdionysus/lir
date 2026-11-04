# Lir - Completed Work

The record. Items move here from `ACTIVE.md` as they land, with what
shipped and the gate counts.

## 2026-11-03, night: the project starts

- `git init`; basemind set up (`basemind.toml`, rules in the gitignored
  `CLAUDE.local.md`). The index is not built: the basemind MCP server
  was down this session.
- Dannan's conventions taken whole: `.clang-format`, `.gitignore`,
  `LICENSE`, `docs/principles-and-laws.md` (Macha's master, with Dannan's
  source note), `CLAUDE.md` and `CONTRIBUTING.md` in Dannan's form.
- The design rendered in `TODO/2026-11-03-lir-plan.md` (revision 7);
  findings against the stack in `ACTIVE.md`.

## 2026-11-03, night: step 1, wire and topics

Uncommitted (commit only when asked). Gate: `lir_tests` 73 passed, 0
failed; built with `-DLIR_WARNINGS_AS_ERRORS=ON`, AppleClang 17.

- Build: `CMakeLists.txt` in Dannan's form (`LIR_BUILD_TESTING`,
  `LIR_BUILD_FUZZING`, `LIR_WARNINGS_AS_ERRORS`, no presets); Dannan
  fetched from `../dannan` at `63c2f51` into the build tree, as Lugh
  does; library `lir`, version header generated.
- `lir/mqtt/wire.h`: MQTT's representations over Dannan's codec:
  variable byte integer (shortest form enforced), UTF-8 strings with the
  section 1.5.4 MUSTs, binary data; `MalformedPacket` carries the reason
  code a server sends, `UnsupportedVersion` the level.
- `lir/mqtt/properties.h`: every 5.0 property; where each may appear
  (table 2-4), repeats, and value ranges checked on decode and encode.
- `lir/mqtt/packet.h`: all fifteen packet types for 3.1 (MQIsdp, level
  3), 3.1.1 and 5.0; fixed-header flags, reserved bits, packet id 0,
  QoS 3, DUP at QoS 0, will flags, password without user name (3.x),
  per-packet reason code tables, 5.0 short forms, trailing bytes.
  `read_frame` sizes a packet from its fixed header and refuses one over
  the maximum before its body is held.
- `lir/mqtt/topic.h`: names, filters, `$share/<group>/<filter>`,
  matching with the `$` rule.
- `lir/subs/trie.h`: the subscription trie, checked against the
  reference matcher on random filters and names.
- `lir/shard/shard.h`: `shard_of`, `shard_of_filter` (shard-local
  filters under `ownership_prefix_levels`), the placement key.
- `lir/fuzz/mqtt_packet_fuzz.cpp`: decode, re-encode, decode equal.
  Not built here: this machine has no clang with libFuzzer (AppleClang
  only, no MacPorts LLVM).

## 2026-11-04: step 2, the broker on one node

Gate: `lir_tests` 125 passed, 0 failed (52 new); warnings as errors.

- `lir/contract/connection.h`: the transport under a client.
  `lir/contract/authenticator.h`: connect, enhanced authentication,
  publish and subscribe authorisation, synchronous by contract;
  `AllowAll`.
- `lir/broker/broker.h`: `Broker` and `Link`, confined to one strand,
  time injected, `poll()` for what time makes due. Covered by tests:
  CONNECT for all three versions, first packet must be CONNECT,
  unsupported level (return code 1), assigned client ids, 3.x id rules,
  second CONNECT, packets in any split, inbound maximum packet size;
  QoS 0/1/2 in both directions with QoS 2 dedup until PUBREL, PUBREL
  for an unknown id, "no matching subscribers", the client's and the
  server's receive maximum; wildcards and the `$` rule, overlapping
  subscriptions (one delivery, greatest QoS, every identifier), No
  Local, per-filter refusals, unsubscribe reasons, shared
  subscriptions (in turn, connected members first, no retained, No
  Local refused); retained messages (sent on subscribe, cleared by an
  empty payload, Retain As Published, Retain Handling 0/1/2);
  persistent sessions (QoS 1/2 queued offline, QoS 0 dropped, resend
  with DUP, clean start, session expiry, expiry 0 at disconnect, the
  DISCONNECT expiry rule), takeover, keep alive at 1.5 periods, connect
  timeout; wills (abnormal close, DISCONNECT 0x00 and 0x04, delay,
  cancelled by reconnecting, cut short by session end); inbound topic
  aliases, message expiry (counted down, queued messages dropped), 5.0
  properties forwarded, the client's maximum packet size, reserved
  `$lir/` and `$dannan/` prefixes, payload format checks, wildcard in a
  topic name; enhanced authentication and authorisation.
- Not done (step 2), all optional in the specification (MAY): topic aliases from
  server to client, Response Information, queueing QoS 0 for offline
  sessions. Server Reference (redirecting a client) waits for the
  cluster, where it means something.

## 2026-11-04: step 3, the edge

Gate: `lir_tests` 132 passed, 0 failed (7 new); warnings as errors.
Dannan pinned at `295ca4b` (channels, the membership feed, the
`dannan::dannan` alias). Changing the pin in `CMakeLists.txt` does not
reach an existing build tree, which caches it: reconfigure with
`-U LIR_DANNAN_COMMIT`.

- `lir/edge/edge.h`: the TCP listener, one connection per socket, the
  broker and its poll timer, all on one strand. A client with more than
  `maximum_outbound_bytes` queued is closed as an abnormal close (its
  will goes), never waited for. `stop()` closes the listener and every
  client; `post()` and `call()` reach the broker on its strand.
- `tools/lir_node.cpp`: `lir-node`, a Dannan node that advertises `lir`
  and serves MQTT; JSON lines on stdout as `dannan-node` writes them;
  `--version`.
- Tests over real sockets: connect and ping, publish across two sockets,
  a protocol error closing the socket, a dropped socket's will, stop
  closing every client, a client that does not read closed while the
  publisher carries on.
- Checked by hand with mosquitto 2 against `lir-node`: `mosquitto_pub`
  over 3.1, 3.1.1 and 5.0 at QoS 1 and 2, `mosquitto_sub` on a wildcard
  at QoS 1 (5.0) receiving both, a retained message read back over
  3.1.1; `lir-node` stopped cleanly. The automated process test with
  mosquitto waits for Dannan's `child_process.h` fixture (asked for;
  the Dannan session adds it after its 0.2.0 sanitizer run).
- Not done: TLS on the MQTT port.

## 2026-11-04: step 4, the cluster on Dannan (interim, before Dagda)

Gate: `lir_tests` 151 passed, 0 failed (19 new); the cluster tests
passed five repeats in a row. Dannan pinned at the `0.2.0` tag
(FetchContent of the tag; `-U LIR_DANNAN_COMMIT` on an old build tree).

- The broker gains a cluster side (`broker.h`): `ClusterHooks`
  (published, interest_changed, take), `inject`, `apply_retained` and
  `retained_entries` with `RetainedVersion` (Lamport, node), clearings
  kept as versions, `deliver_shared`, `surrender`, `SessionImage`,
  `import`, and an asynchronous CONNECT (`taking`) that waits while
  other nodes let go of the client, buffering what the client sends
  meanwhile. Alone, the broker behaves as before (125 tests unchanged).
- `lir/cluster/codec.h`: messages, snapshots, session images, groups,
  take and shared delivery, each with a format number, counts bounded
  before allocation, expiry carried as what is left.
- `lir/cluster/cluster.h`: channels `lir.notify` (QoS 0), `lir.seq`
  (QoS 1/2, deduplicated by origin, incarnation and sequence),
  `lir.retained` and `lir.groups`; subscriptions by the minimal set of
  literal filter prefixes; routes `0x4C00` snapshot (retained and
  groups for a node that joins or restarts), `0x4C01` session take,
  `0x4C02` shared delivery; the roster feed for peers.
- `lir-node` assembles node, edge and cluster; the cluster's routes are
  registered before the node starts.
- `lir/test/fixtures/mqtt_client.h`: the socket client, with reads
  bounded by `poll()`; Asio's blocking read retries a timed-out read for
  ever, which hung the first cluster run.
- Tests: QoS 0, 1 and 2 across nodes, once each; subscribers on three
  nodes each get one copy; retained values everywhere, cleared
  everywhere, the later value winning on every node, a late joiner
  catching up; a persistent session resumed on another node with what
  was queued; takeover across nodes; a will across nodes; a shared group
  across two nodes getting each of ten messages exactly once.
- `process_test.cpp` (Dannan's `child_process.h`): mosquitto 2 against
  one `lir-node` over 3.1, 3.1.1 and 5.0; two `lir-node` processes as one
  broker, live and retained.
- Interim and its limits are `ACTIVE.md`, questions 1 to 3.

## Findings resolved (2026-11-03 night to 2026-11-04)

The design predates the stack's rules; Lir's findings and their ends:
store nouns are Dagda's (stack document); multimaster with a per-shard
lease (Tom, 2026-11-04); no `lir.local` (Tom); authentication pluggable,
no token verification in Dannan (Tom); HTTP Lir's own (Tom); message
numbers free from 512 (Tom); `lir` advertised, one process per node
(Tom); k-of-N durability is Dagda's Replicated (Tom). The per-origin
sequence lives in a Dagda node-local space and never goes back (with
Dannan and Dagda; `durability_epoch` is unordered, for dedup only).

## 2026-11-04: session standbys (Tom's answer 2)

Gate: `lir_tests` 152 passed, 0 failed; cluster and process tests five
repeats clean.

- The broker marks persistent sessions (expiry above 0) as changed after
  every packet their link handles and at delivery, connect, disconnect,
  import and end; `take_changes()` hands them over as images, with those
  that ended or left; `adopt()` takes up a mirrored session offline.
- The cluster sends changed sessions to `session_standbys` other Lir
  nodes (rendezvous over the client id; default one, `lir-node
  --session-standbys N`, zero for none) at most every `mirror_every`
  (100 ms), on route `0x4C03` lir.session_mirror. A standby holds the
  image. When the owner leaves the roster or restarts, the first node in
  rendezvous order takes the session up offline (subscribed, queueing);
  the others note when the owner went and drop the copy once the
  session's expiry has run from then. A session take prefers a live
  session to a mirror, and every node drops its copy of a client that
  is taken.
- Limits, until step 5: a change made in the last `mirror_every` before
  a node dies is lost; a partition can leave a live session on one side
  and an adopted one on the other.
- Test: a persistent session survives its node being stopped: taken up
  by its standby, receiving a QoS 1 message published meanwhile, and
  resumed by its client on another node with that message.

## 2026-11-04: step 6, the system topics

Gate: `lir_tests` 154 passed, 0 failed; cluster and process tests three
repeats clean.

- `lir/system/system.h` (`lir::system`, a seventh namespace beside the
  six Tom kept): each node publishes, retained, as JSON with content
  type `application/json`, `$lir/nodes/<node id>` (connections,
  sessions, retained, accepted, refused, protocol errors, messages
  received, delivered and injected, wills, session counts, drops) and
  `$dannan/nodes/<node id>` (its record, path class, incarnation, peers
  and their latency), when they change, checked every 5 s; every node
  clears a node's topics when it leaves the roster. Clients may read
  them; publishing there is refused (5.0 reason 0x87).
- `lir-node` runs them.
- Tests: every node's topics retained on every node and readable by a
  client; a forged publish refused; a node's topics cleared when it
  leaves.

## 2026-11-04: step 6, HTTP publish and Server-Sent Events

Gate: `lir_tests` 163 passed, 0 failed; HTTP, cluster and process tests
three repeats clean.

- `lir/http/http.h` (`lir::http`): `POST /publish?topic=T[&qos][&retain]`
  and `GET /subscribe?filter=F[&filter=G]` as `text/event-stream`. Each
  request is an MQTT 5.0 client of the broker for its length, fed
  packets in process, so the authenticator, retained messages and the
  cluster apply as to any client. `Authorization: Bearer <token>` is the
  CONNECT password, for the authenticator (a Lugh plugin) to check.
  Only %XX is decoded in the query: '+' is the MQTT wildcard. Events
  carry the payload as text when it is UTF-8, else as hex (Dannan's
  `identity::hex`); retained first; a comment every 15 s; a stream not
  being read is closed past 4 MiB queued. Boost.Beast parses requests:
  it is part of Boost, already a dependency.
- MQTT refusals map to HTTP: 0x86 and 0x8C to 401, 0x87 and 0x8A to 403,
  busy and quota to 503, the rest to 400; 404, 405 and 413 for the
  request itself.
- `lir-node --http-port N|-1 --http-listen ADDR` (default 8083; -1 for
  none); the listening line reports `http_port`.
- Tests: publish reaching an MQTT subscriber with its content type;
  "matched" false with no subscriber; a retained HTTP publish read over
  MQTT; every refusal's status; an event stream getting a retained then
  a live message and its keep-alive; a closed stream leaving the broker;
  a bearer token as the CONNECT password. Checked by hand with curl
  against `lir-node`.
- A bug found on the way: an event stream's exchange was held only by
  weak references and went as soon as its headers were written; its
  pending read now holds it.

## 2026-11-04: TLS on the MQTT port (step 3's open item)

Gate: `lir_tests` 165 passed, 0 failed.

- `EdgeConfig::tls` (`TlsConfig`: listen address, port, PEM certificate
  chain and key, handshake timeout): a second listener for MQTT over TLS
  (1.2 and up), the same broker as the plain port. The connection is one
  template over a TCP socket or a TLS stream; a TLS client becomes a
  connection once its handshake is done, within the timeout.
- `lir-node --mqtt-tls-port N --tls-certificate PEM --tls-key PEM
  [--mqtt-tls-listen ADDR]`; the listening line reports `mqtt_tls_port`.
- Tests: a client verifying a self-signed certificate (made by the
  `openssl` command for the test) receives over TLS what a plain client
  publishes; plaintext on the TLS port gets nothing and leaves no
  connection. Checked by hand with `mosquitto_pub` and `mosquitto_sub
  --cafile` against `lir-node`.
- Not done: a TLS close_notify on close (the socket is closed); HTTPS on
  the HTTP port; client certificates.

## 2026-11-04: the specification's MAYs (all of MQTT, Tom)

Gate: `lir_tests` 169 passed, 0 failed (4 new).

- Topic aliases from server to client (5.0, 3.3.2.3.4): per connection,
  up to the client's Topic Alias Maximum, the topic the first time and
  the alias alone after; an alias counts only once a packet carrying it
  has gone (one over the client's maximum size is not sent and assigns
  nothing). Reset with each connection.
- Response Information (`Config::response_information`, "{client}" for
  the client id): sent in the CONNACK only when the client asks.
- `Config::queue_qos0_offline`: QoS 0 messages queued for an offline
  session (off by default: dropped and counted).
- With these, step 2's "not done" list is empty. Server Reference
  (redirecting a client) is not used: in a multimaster cluster every
  node serves every client.

## 2026-11-04: HTTPS

Gate: `lir_tests` 170 passed, 0 failed.

- `HttpConfig::tls` (an `edge::TlsConfig`): HTTPS on its own port beside
  plain HTTP, the same endpoints; the exchange is one template over a
  TCP socket or a TLS stream, as the edge's connection is.
- `lir-node --https-port N [--https-listen ADDR]` with `--tls-certificate`
  and `--tls-key`; the listening line reports `https_port`.
- Test: a verifying client publishes over HTTPS to an MQTT subscriber.
  Checked by hand with `curl --cacert` (a retained publish, then an
  event stream reading it back) against `lir-node`.

## 2026-11-04: the design's seven sections, the README, Dannan 0.3.0

- Tom answered the seven unwritten sections in turn:
  `TODO/2026-11-04-lir-sections.md`. The repositories are the sources of
  truth; the starter Claude Docs will be deleted (Tom), and the stack's
  sessions were told.
- `README.md` in the stack's form (why, what, the stack, how to use it);
  remote `origin` is `git@github.com:tomdionysus/lir.git`, not pushed.
- Built against Dannan `0.3.0` (`277adab`). Gate: `lir_tests` 170.

## 2026-11-04: topic scopes; anonymous refused by default

Gate: `lir_tests` 199 passed, 0 failed (29 new); the process tests ran
against mosquitto.

- Tom, 2026-11-04: Lir checks token scopes itself. Dannan 0.3.0's scope
  seam (`ScopeSource`, `Identity::verify`) answers only "may P write under
  prefix S", with data a Lugh source installs on a node; it is not used
  for MQTT.
- Contract: an accept carries a `contract::Grant` (may publish, with the
  retain flag; may subscribe; when it ends), held by the link and
  replaced on 5.0 re-authentication. The authenticator keeps no
  per-client state. `Unrestricted` and `Nothing` grants; `AllowAll`
  gives the first.
- Broker: publish, retain and subscribe checked against the grant; the
  will checked once the client is accepted, against pub or retain; a
  connection whose grant ends is sent DISCONNECT 0x87 and its session
  kept (design, section 1).
- `mqtt::covers(granted, requested)`: filter coverage with the '$' rule
  and "a/#" covering "a".
- `lir/auth/scopes.h`: the grammar Lugh proposed (`lir:pub|sub|retain:`
  percent-encoded filter, "{sub}" a whole level), `ScopeGrant`, and
  `AnonymousAuthenticator`: anonymous clients refused unless allowed,
  credentials refused until the Lugh plugin. The percent decoder moved
  here from `http` (one copy).
- `lir-node --allow-anonymous [--anonymous-scope S]...`: without a scope,
  anything but writing the system topics.
- Tom decided the grammar the same day, with Lir's review taken in
  (`../lugh/TODO/2026-11-04-oauth.md`, "Topic scopes, for Lir"):
  `{client}` dropped, retain its own action, Lir checks scopes itself.

## 2026-11-04: narrower grants drop subscriptions

Gate: `lir_tests` 202 passed, 0 failed (3 new).

- Tom, 2026-11-04 (via Lugh): a re-authentication whose grant no longer
  covers some subscriptions removes them from the session and sends
  DISCONNECT 0x87; the client resubscribes and sees SUBACK 0x87. A
  resumed session (any version, any node) keeps only what the new
  connection's grant allows. `Broker::prune`.

## 2026-11-04: MQTT over WebSocket

Gate: `lir_tests` 214 passed, 0 failed (12 new); the WebSocket and TLS
tests passed five runs in a row.

- In scope by Tom's answer to design section 5 (the specification's own
  transport, section 6). `EdgeConfig::websocket` (ws) and
  `EdgeConfig::secure_websocket` (wss, its own certificate and key), each
  on its own port beside TCP and TLS, the same broker.
- The upgrade (Boost.Beast, already linked for HTTP): any path; the
  server answers "mqtt" when offered, else "mqttv3.1" for a 3.1 client,
  serves a client that offers none, and refuses with 400 one that offers
  only others, or a request that is not an upgrade. MQTT goes in binary
  frames, one byte stream however the frames split it; a text frame
  closes the connection (section 6 MUST). One deadline covers the TLS
  handshake and the upgrade.
- The edge's connection is still one template, now over a socket, TLS,
  or a WebSocket over either.
- Fixed on the way: a handshake deadline that was already due when its
  handshake finished could close a stream that had moved into its
  connection (TLS too); the deadline now checks a settled flag.
- `lir-node --ws-port N [--ws-listen ADDR] --wss-port N [--wss-listen
  ADDR]`; the listening line reports `ws_port` and `wss_port`. Checked by
  hand: a raw upgrade to `lir-node` gets 101 with
  `Sec-WebSocket-Protocol: mqtt` and a CONNACK in a binary frame.

## 2026-11-04: the configurable system topics (design, section 3)

Gate: `lir_tests` 222 passed, 0 failed (8 new); the cluster and process
tests passed three runs in a row.

- `SystemConfig` switches `nodes`, `cluster`, `clients`, `roster`, all on
  by default; `lir-node --no-system-topics NAME`, repeatable.
- `$lir/cluster` (retained): nodes, connections, sessions, messages
  received a second, and the limits that differ between nodes (design
  section 4, "mismatch reported"). Each Lir node advertises its figures
  and limits in its Dannan record (`lir.connections`, `lir.sessions`,
  `lir.received_per_second`, `lir.limits`), changed only when they
  change; the Lir node with the lowest id publishes, so a subscriber sees
  one summary, not one per node.
- `$lir/clients/connected` and `/disconnected` (events): each node its own
  clients, from a new `Broker::observe` hook (`ClientEvent`: version,
  peer, user name, session present; by client, server or network, and
  the reason code). The hook posts the publish to the strand, never
  calling the broker back from inside it.
- `$dannan/roster/joined`, `/left`, `/changed` (events), published by the
  lowest Lir node only; an advert's change is not an event.
- `$lir/shards/<n>` is not built: shards have no holders, primary or
  lease until step 5, and the topic would report nothing true. It
  follows step 5.

## 2026-11-04: client certificates (mutual TLS)

Gate: `lir_tests` 227 passed, 0 failed (5 new).

- `TlsConfig::client_ca` and `require_client_certificate`: on every TLS
  listener (MQTT, wss, HTTPS), a client certificate verified against the
  named CAs, required (refused at the handshake without one) or
  optional. The context is built once, `edge::server_context`, which the
  HTTP server now uses too (it had its own copy).
- `contract::Connection::certificate_subject()` and `contract::Peer`:
  the authenticator's `connect` is given the peer's address and the
  verified subject (RFC 2253). A certificate grants nothing by itself:
  the anonymous authenticator treats a client with one and no credential
  as anonymous. Whether a subject should map to scopes is open (Lugh's
  devices are service principals with tokens).
- `lir-node --tls-client-ca PEM [--tls-client-certificate
  required|optional]`. Checked by hand: `mosquitto_pub --cert` connects
  to `lir-node` over TLS; without a certificate the connection is lost.
- Not done: HTTPS hands no subject to the authenticator (its exchanges
  are virtual clients).

