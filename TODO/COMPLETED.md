# Lir - Completed Work

The record. Items move here from `ACTIVE.md` as they land, with what
shipped and the gate counts.

How to read it:
- Entries are in the order they happened and are never rewritten. An entry
  says what was true that day; a later entry may supersede it.
- Where an older entry points at `ACTIVE.md` (a question, an item, a
  watch), it means `ACTIVE.md` as it was then. That text is kept in the
  "Archived from ACTIVE.md" sections: 2026-11-14 (the whole file, with
  "the maintainer's answers" and the first decisions), 2026-11-15 (the shared listener
  and the Lugh plugin) and 2026-11-19 (the ledger and SUBACK watches, the
  SUBACK decision as first recorded, and Dagda's slice 0 figures).
- The gate count runs from 73 tests (step 1) to 361 (2026-11-17). It has
  never dropped.
- Everything here up to 2026-11-19 was built before the premise was written
  down (`2026-11-19-premise-and-audit.md`). An entry says what was built and
  that its tests passed. It does not say the thing built was right.
- Two files this record cites are gone from `TODO/` and are in the history
  at `53afd98`: `2026-11-13-lir-needs.md` (Lir's needs of Dannan and Dagda,
  N1 to N6, G1 to G9, and its review R1 to R11) and the first
  `2026-11-19-guarded-publish-design.md` (at `003906a`).

## 2026-11-22 to 23: step 2, the broker

Squash-merged onto develop. How it went, in the order it was built:

- On `feature/step2`, rebased on develop 2026-11-22,
  three commits: `lir::broker::Delivery` on Dannan's notify channel; the
  contract's `Connection` and `Authenticator` restored with the shared
  cluster fixture; and `lir::broker::Link`, one client's protocol
  (`lir/broker/link.h`): CONNECT with enhanced authentication and
  re-authentication, the CONNACK limits, SUBSCRIBE held on Delivery,
  No Local, UNSUBSCRIBE, PUBLISH at QoS 0, PINGREQ, keep-alive and grant
  expiry on Dannan's time, DISCONNECT; every will refused. Gate on node-d,
  on release 0.2.0 (Dannan 0.10.2, the client included): release 207 of
  207, 50 of them `Link`. The Link count fell from 52 on the maintainer's
  will ruling (five tests of a published will went with it).
  - The Link tests were written with the code, not seen failing first.
  - Then, on Dannan's develop (`9512a9b`, the stream carrier; the pin goes
    back to `0.11.0` when it is tagged, before any merge):
    `lir::edge::Edge`, each stream Dannan's listener accepts served as one
    Link; `lir::edge::Server`, Lir on one Dannan node (Delivery, the edge,
    the TCP and TLS listeners, the `lir` and `lir.endpoints` adverts,
    withdrawn first at stop); `lir::auth::Anonymous`, anonymous clients
    refused by default or held to configured publish, retain and subscribe
    filters (the design's section 1, as filters rather than Lugh's scope
    strings, so Lir parses no scope grammar). Gate on node-d: 224 of 224
    (Edge 6, EdgeServer 3, AuthAnonymous 8).
  - The optional Lugh plugin, `lir::auth::Lugh` (`-DLIR_LUGH=ON`, Lugh
    0.5.0 fetched without its node or tests, so it takes Lir's `lir::mqtt`
    and Dannan): a token as the CONNECT password or the `lugh` method's
    data, re-authentication with a fresh one, its `lir:` scopes the grant
    (`lugh::client_topics`), its exp the grant's end; a user name must be
    the subject; a certificate-bound token only for its holder, a DPoP-bound
    one refused; no credential is anonymous. Gate on node-d with it on: 232
    of 232 (AuthLugh 8). Before Lir moves to Dannan 0.11.0, Lugh needs a
    release on it without its own Boost.JSON (`client/boost_json.cpp`),
    since 0.11.0 compiles Boost.JSON once in libdannan; Lugh told.
  - A client's publish to `$lir/` or `$dannan/` reaches no one, whatever
    its grant (`lir/system/topics.h`). Real clients across the cluster:
    `mosquitto_sub` on one node and `mosquitto_pub` on another, over TCP to
    the ports their roster adverts name, MQTT 5 and 3.1.1, the simulated
    world held to real time. Gate on node-d: 234 of 234 with the plugin on.
  - The system topics (`lir::system::Views`, on Dannan's JSON at develop
    `e1c69e0`): `$lir/nodes/<id>` from each node's `lir.figures` advert
    (a node's own from its server directly: Dannan's advert watch tells
    peers only), `$lir/cluster` summed alike on every node,
    `$dannan/nodes/<id>` and `$dannan/roster/*` from the roster watch; each
    rendered by the subscriber's own node and saying so, sent at SUBSCRIBE
    with RETAIN 1 as Retain Handling says, then on change. Connection
    events published on the channel by the node that saw them, without the
    client's address. Gate on node-d: 241 of 241 (SystemViews 7).
  - Topic aliases from a 5.0 client: Topic Alias Maximum 64 in CONNACK
    (`mqtt.topic_alias_maximum`, 0 for none), each alias this connection's
    only, 0 or one past the maximum 0x94, one never set 0x82; nothing
    crosses the cluster as an alias, and Lir sends none. MQTT over
    WebSocket (section 6) on Beast over Dannan's streams, `ws` and `wss`
    listeners beside `tcp` and `tls`, in `lir.endpoints`: binary messages
    only, packets split or joined across them, "mqtt" (or "mqttv3.1")
    selected, a handshake offering neither refused. Gate on node-d: 251 of
    251 (EdgeWebSocket 6).
  - Per-topic order for QoS 0 (MQTT-4.6.0-6), on Dannan 0.11.1 (the
    maintainer's ruling of 2026-11-23): Lir's channel is
    `Delivery::ordered` (at most once, in order per origin, a late message
    dropped, no replay), the cluster fixture's links in order as one
    session's are; a test where links overtake shows a strictly increasing
    subsequence. Every Lir node needs Dannan 0.11.1: a 0.11.0 node refuses
    an ordered publish.
  - On Dannan 0.11.0 (read against its changelog; TSan runs pass its
    suppressions): `lir-node` (`tools/lir_node.cpp`, settings in
    `lir/edge/settings.h`): Dannan's shared node settings under
    `cluster.*`, Lir's `mqtt.*` (address, port, advertise, packet size,
    topic aliases, tls, ws, wss), `anonymous.*`, `lugh.*` with the plugin,
    `log.*`; refuses a start with no state or no way to prove itself.
    Smoked on node-d as a real process: anonymous mosquitto clients
    publish and subscribe, a credential with no plugin is refused 0x86,
    SIGINT stops it cleanly. Twenty named conformance tests and the MQTT
    3.1 identifier rule added; the gate counts 127 of 5.0's 252 proven, 1
    waiting. Gate on node-d: 271 of 271, 1 disabled (waiting on 0.11.1).
  - The `lir` package: `lir-node`, `lir.service` installed disabled
    (hardened, no capabilities), the `lir` user by sysusers,
    `/etc/lir/config.yaml` as a conffile (`config/config.yaml`),
    `secrets.yaml` made by postinst, `lir-node(1)`; the broker library
    static, as nothing else links it. `deb` on node-a in a clean trixie:
    seven packages, the suite inside the build, lintian clean.
  - The Lugh plugin pins Lugh 0.6.0 (on Dannan 0.11.0, Boost.JSON carried
    once in libdannan; read against its changelog: nothing in the plugin
    switches on a Fault, and its token and scope types are unchanged).

- Two findings on the way, each sent to its project: Dannan's notify
  channel promises no order, so QoS 0 broke MQTT-4.6.0-6 across nodes
  (Dannan 0.11.1 added the ordered delivery, the maintainer's ruling);
  and Dannan's ChildProcess read nothing with under a millisecond left
  (fixed in 0.11.0). In Lir's own test fixtures: a client left on a
  stalled link leaked under ASan (it now closes its end as it goes).
- Gate on the merge (node-a): release 273 of 273; ASan and UBSan, TSan
  273 of 273 each; coverage 98.0% of lines, 97.3% of functions, 281 of
  281; fuzz, tidy, clang clean; the Lugh plugin 289 of 289; `deb` lintian
  clean with the `lir` package. Conformance: 128 of 252 (5.0).

**Handoff.** Release 0.3.0 waits for the maintainer's word. Then step 4 as
Dagda's nouns ship.

## 2026-11-22, evening: release 0.2.0, the client

- **Release 0.2.0** (`9085820`, annotated tag `0.2.0`, on main by fast
  forward), on the maintainer's word: `lir::mqtt` and `lir::client` on
  Dannan 0.10.2, the broker not yet functional. The release commit bumps
  `project()` to 0.2.0, renames the packages `liblir-mqtt0.2` and
  `liblir-client0.2`, and brings the README (a `lir::client` section, the
  packages, `libdannan0.10`), CHANGELOG, CONTRIBUTING (the broker is now
  `0.3.0`) and `docs/packaging.md` up to date.
- Gate: hosted CI on the commit, all 15 jobs green (run 45054550066),
  deb on x86-64 and arm64 with lintian among them; develop's gate on the
  client before it (150 of 150, ASan, TSan, coverage 99.5%) stands, as
  nothing but versions and docs changed. The packages, built from the tag
  (run 45055166324), are staged in `../tomcully-apt/incoming/lir/0.2.0/`
  for the maintainer to publish.
- `feature/step2`, rebased on the release: 207 of 207 on node-d (release,
  -j1). It waits on Dannan 0.11.0 for the transport and the system topics.

**Handoff.** Nothing is buildable until Dannan 0.11.0 (the stream carrier
and JSON) or Dagda 0.4.0 (D6). Next release: `0.3.0`, the broker.

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
lease (the maintainer, 2026-11-04); no `lir.local` (the maintainer); authentication pluggable,
no token verification in Dannan (the maintainer); HTTP Lir's own (the maintainer); message
numbers free from 512 (the maintainer); `lir` advertised, one process per node
(the maintainer); k-of-N durability is Dagda's Replicated (the maintainer). The per-origin
sequence lives in a Dagda node-local space and never goes back (with
Dannan and Dagda; `durability_epoch` is unordered, for dedup only).

## 2026-11-04: session standbys (the maintainer's answer 2)

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
  six the maintainer kept): each node publishes, retained, as JSON with content
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

## 2026-11-04: the specification's MAYs (all of MQTT, the maintainer)

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

- The maintainer answered the seven unwritten sections in turn:
  `TODO/2026-11-04-lir-sections.md`. The repositories are the sources of
  truth; the starter Claude Docs will be deleted (the maintainer), and the stack's
  sessions were told.
- `README.md` in the stack's form (why, what, the stack, how to use it);
  remote `origin` is `git@github.com:tomdionysus/lir.git`, not pushed.
- Built against Dannan `0.3.0` (`277adab`). Gate: `lir_tests` 170.

## 2026-11-04: topic scopes; anonymous refused by default

Gate: `lir_tests` 199 passed, 0 failed (29 new); the process tests ran
against mosquitto.

- The maintainer, 2026-11-04: Lir checks token scopes itself. Dannan 0.3.0's scope
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
- The maintainer decided the grammar the same day, with Lir's review taken in
  (`../lugh/TODO/2026-11-04-oauth.md`, "Topic scopes, for Lir"):
  `{client}` dropped, retain its own action, Lir checks scopes itself.

## 2026-11-04: narrower grants drop subscriptions

Gate: `lir_tests` 202 passed, 0 failed (3 new).

- The maintainer, 2026-11-04 (via Lugh): a re-authentication whose grant no longer
  covers some subscriptions removes them from the session and sends
  DISCONNECT 0x87; the client resubscribes and sees SUBACK 0x87. A
  resumed session (any version, any node) keeps only what the new
  connection's grant allows. `Broker::prune`.

## 2026-11-04: MQTT over WebSocket

Gate: `lir_tests` 214 passed, 0 failed (12 new); the WebSocket and TLS
tests passed five runs in a row.

- In scope by the maintainer's answer to design section 5 (the specification's own
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

## 2026-11-04: the moderate default limits (design, section 4)

Gate: `lir_tests` 230 passed, 0 failed (3 new).

- Defaults now the moderate set the maintainer chose: 1 MiB packets and receive
  maximum 1024 (as before), 10,000 queued per offline session (was 1000),
  session expiry capped at 7 days (was never), 4 MiB queued for a slow
  client (was 16 MiB), and `Config::maximum_connections` 10,000, past
  which a CONNECT is refused with 0x89 Server busy (3.x: return code 3);
  existing clients are never evicted.
- `lir-node --max-packet-size --receive-maximum --max-queued
  --max-session-expiry --max-outbound-bytes --max-connections`.
- Not done from section 4: the memory budget's order under pressure
  (drop QoS 0, then refuse QoS 1/2 with 0x97, then 0x89) needs a node
  memory budget, Dannan's `budget()`, which Lir has not declared yet;
  the config file waits on Dannan's reader.

## 2026-11-04: memory pressure (design, section 4)

Gate: `lir_tests` 234 passed, 0 failed (4 new).

- The broker counts the bytes of every message it holds (queued, in
  flight, retained) as they come and go; `recount_held_bytes()` walks
  them all, and a test checks the two agree through every flow.
- `Config::memory_budget` (512 MiB; `lir-node --memory-budget`). The
  design's order: at 80% QoS 0 is no longer queued and queued QoS 0 is
  shed oldest first at each poll; at 90% QoS 1/2 publishes are refused,
  PUBACK or PUBREC 0x97 (a 3.x client, which has no code for it, is
  closed and resends on return); at 100% new clients get CONNACK 0x89.
  Existing sessions are never evicted. Counted in `$lir/nodes/<id>`
  (`memory`: held, budget, pressure, refusals), and the budget is one of
  the limits `$lir/cluster` compares.
- `lir-node` declares Lir's unit of work (one maximum-size packet,
  foreground) to Dannan's `budget()`, which refuses to start Lir on a
  node that cannot hold one (the design's "Budget" row).
- Open for the maintainer: the 512 MiB default and the 80/90/100 thresholds are
  mine, not decided; and the held bytes are Lir's own count, not leases
  in Dannan's retained-memory ledger (that would make other systems'
  memory press on Lir too).

## 2026-11-04: Dannan 0.4.0; the config file; one percent decoder

Gate: `lir_tests` 239 passed, 0 failed (5 new).

- Built against Dannan `0.4.0` (`d657a9b`).
- Percent-decoding is Dannan's (`dannan::encoding::percent_decode`,
  strict), for the HTTP query and topic scopes; Lir's copy is deleted
  (rule 3; the maintainer). Scope filters are now decoded level by level: an encoded
  '/' (`%2F`) can no longer make a level boundary, and the subject is
  never decoded. Lugh's encoder output (Lugh `2c63171`) and the strings
  Lugh refuses are tests here.
- `lir-node` reads its settings through Dannan's reader
  (`dannan::config`): a YAML file (`--config`, `$LIR_CONFIG`,
  `/usr/local/etc/lir/config.yaml`), `secrets.yaml` beside it, `LIR_*`
  variables, then the flags. Every flag kept; `reader()` lists the keys.
  Process tests: a setting from the file alone takes effect; a bad one
  stops the node with status 2, naming the file and the key.

## 2026-11-04: Server Reference at CONNECT (the maintainer's policy, design section 7)

Gate: `lir_tests` 248 passed, 0 failed (9 new); the cluster and process
tests passed three runs in a row.

- `cluster/balance.h`, pure: a node over (1 + `rebalance_over`) times the
  cluster's average sends a new 5.0 client to the least-loaded node
  serving the same transport, if that node has at least two fewer
  clients; lowest id among equals. Default 20%; `lir-node
  --rebalance-over PERCENT|-1`.
- Each node advertises where it serves (`lir.endpoints`: tcp, tls, ws,
  wss as host:port, from its advertised host) once its edge is bound
  (`Cluster::advertise_endpoints`), and its client count
  (`lir.connections`, now clients past CONNECT). A node counts the
  clients it has sent each other node since that node's count last
  changed, so a burst is spread rather than all sent to one node.
- Broker: `ClusterHooks::redirect(transport)`; CONNACK 0x9C with Server
  Reference; counted (`connections_redirected`). 3.x clients and HTTP
  requests are never redirected. `Connection::transport()`.

## 2026-11-04: memory thresholds configurable; Dannan's ledger

Gate: `lir_tests` 251 passed, 0 failed (3 new).

- The maintainer: budget configurable (512 MiB), thresholds configurable (80, 90,
  100): `Config::shed_percent`, `refuse_publish_percent`,
  `refuse_connect_percent`; `lir-node --shed-percent
  --refuse-publish-percent --refuse-connect-percent`, refused unless they
  rise.
- The maintainer: Lir's held bytes are also leased in Dannan's ledger.
  `system::LedgerLeases` follows the broker's count each second in 1 MiB
  foreground leases; a refused lease sets `Broker::ledger_full`, which
  puts the pressure at refuse_connect until room returns. Test: three
  MiB retained on a node are leased and released as they come and go.
- The memory part of the "Open for the maintainer" note under memory pressure is
  settled by this.

## 2026-11-04: the rules engine (the maintainer's decisions, sections file)

Gate: `lir_tests` 270 passed, 0 failed (19 new); the cluster tests passed
twice more.

- `lir/rules/rules.h`, namespace `rules`: a rule is a retained JSON
  message on `$lir/rules/<name>`, compiled when its retained value lands
  on a node (locally or from another node), deleted by an empty one, and
  refused (counted, logged) when it cannot be compiled. Boost.JSON, used
  header-only and compiled once as Lugh does (Boost is already a
  dependency; nothing added).
- Conditions on topic, client, qos, retain, content_type, user.<name>,
  payload and payload.<path> (= != < <= > >= exists in; all, any, not).
  An absent value equals nothing: "!=" holds, the rest fail.
- Actions: republish (templated topic and payload, QoS, retain;
  transform: pick, rename, add), set (user properties, content type,
  expiry), drop. Rules apply in name order; set changes the message for
  later rules and for delivery.
- In the broker: applied in `route`, on the publishing node, before
  retaining and fan-out; a rule's own messages are never ruled on.
  Produced messages pass through the rules again, at most 8 hops
  (`Message::hops`, never carried between nodes). Writing `$lir/rules/`
  is the one reserved prefix a client may publish to: retained, and only
  with a grant to retain there. Counted in `$lir/nodes/<id>` (`rules`).
- Cluster test: a rule written on one node applies on another.

## 2026-11-04: MQTT-SN 1.2 (design sections 5 and 7; the maintainer's MQTT-SN decisions)

Gate: `lir_tests` 288 passed, 0 failed (18 new); the gateway tests passed
three runs in a row.

- `lir/sn/packet.h`, namespace `sn`: every MQTT-SN 1.2 packet, one-byte
  and three-byte lengths, the flags byte with QoS -1 and the three topic
  id types, and the encapsulated form (its length runs to the end of the
  node id; the relayed packet follows).
- `lir/sn/gateway.h`: UDP, on the edge's strand. Each client is a client
  of the broker (a connection like HTTP's), so sessions, the
  authenticator, the rules and the cluster apply. Topic ids registered
  per client both ways (the gateway REGISTERs a new name and sends the
  PUBLISH on the REGACK); short names; predefined ids from retained
  `$lir/mqttsn/topics/<id>` (the maintainer). The will is asked for, held by the
  gateway (so WILLTOPICUPD and WILLMSGUPD work) and published when the
  client is lost (keep alive or sleep overrun), not on an orderly
  DISCONNECT. Sleep: the broker connection stays, with receive maximum
  64, and the gateway holds what arrives (bounded, oldest QoS 0 first)
  until PINGREQ wakes the client, then PINGRESP, then asleep again after
  a grace. QoS -1 from unconnected clients goes in as an anonymous
  client's. Forwarders are answered through the forwarder. ADVERTISE is
  off unless configured (the maintainer); SEARCHGW is always answered.
- `lir-node --mqttsn-port --mqttsn-listen --mqttsn-gateway-id
  --mqttsn-advertise-seconds --mqttsn-advertise-address`; the listening
  line reports `mqttsn_port`. Checked by hand: a raw MQTT-SN client
  connects to `lir-node`, publishes on a short topic, and `mosquitto_sub`
  receives it.
- Not done: DTLS (next), MQTT-SN 2.0 (after checking the OASIS text).

## 2026-11-04: MQTT-SN over DTLS (the maintainer: DTLS beside plain UDP)

Gate: `lir_tests` 291 passed, 0 failed (3 new); the TLS, DTLS and
MQTT-SN tests passed three runs in a row.

- `GatewayConfig::dtls` (an `edge::TlsConfig`): DTLS 1.2 on its own UDP
  port. Asio has no DTLS, so the gateway drives OpenSSL itself: one
  session per peer over datagram memory BIOs (OpenSSL 3.2's
  `BIO_s_dgram_mem`, so records keep their boundaries), the stateless
  cookie exchange (an HMAC of the peer's address under a secret made at
  start, with Dannan's `hmac_sha256` and `random_bytes`), handshake
  retransmission at each poll, a cap on peers mid-handshake, and idle
  sessions let go. Decrypted datagrams take the plain path.
- Client certificates as on every TLS listener; the verified subject
  reaches the authenticator. `edge::server_context(tls, datagram)` and
  `edge::verified_subject` are shared, not copied.
- Found on the way: Asio's SSL context owns the `SSL_CTX` app data slot
  and deletes what is in it; the gateway's pointer now has its own
  ex-data index.
- `lir-node --mqttsn-dtls-port N [--mqttsn-dtls-listen ADDR]`, with
  `--tls-certificate`, `--tls-key` and `--tls-client-ca`.

## 2026-11-04: close_notify, and the HTTPS certificate subject

Gate: `lir_tests` 295 passed, 0 failed (4 new); the TLS, WebSocket,
client certificate, HTTP and process tests passed three runs in a row.

- A close the broker makes (DISCONNECT, a refusal, a server DISCONNECT)
  now ends the stream cleanly once what is queued is written: TLS sends
  close_notify (RFC 8446, section 6.1), a WebSocket a close frame (RFC
  6455, section 7, then close_notify under wss), each bounded by a
  2-second deadline. Abnormal closes (errors, an overfull queue, stop)
  stay immediate. HTTPS responses end with close_notify the same way.
- HTTPS hands the authenticator the verified client certificate subject,
  as the MQTT TLS port does; the HTTPS listener already asked for
  certificates when `client_ca` was set.
- MQTT-SN 2.0: OASIS has only working drafts (the newest 2025-10-19; CSD01
  of 2023-12 is superseded). Recommended to the maintainer to wait for an approved
  Committee Specification; recorded as waiting on OASIS.

## 2026-11-04: Dannan 0.4.1, a cluster flake, and the Lugh token authenticator

Gate: `lir_tests` 307 passed, 0 failed (12 new); the certificate,
TLS, WebSocket, HTTPS, MQTT-SN and token tests passed three runs in a
row, the cluster tests five.

- Dannan 0.4.1 (additive: readiness planes; Lir declares none).
- `ClusterTest.ASharedGroupAcrossNodesGetsEachMessageOnce` failed once
  under load: it waited on an empty groups advert and then slept 500 ms.
  It now waits on `Cluster::shared_members(group, filter)`, the nodes the
  publishing node knows to hold a member.
- `auth::TokenAuthenticator` (`lir/auth/tokens.h`): the token as the
  CONNECT password or the data of the MQTT 5 method "lugh" (which also
  re-authenticates); "lir" among the audiences or 0x87; a token that does
  not verify or has expired 0x86; a DPoP-bound token refused (MQTT cannot
  carry a proof); a certificate-bound token only from that certificate
  (RFC 8705 x5t#S256); the topic scopes with `{sub}` as the grant until
  exp; anonymous clients as before. Whether a token is genuine is
  `TokenVerifier`'s, which is Lugh's code: nothing in Lir parses JOSE.
- `contract::ClientCertificate` (subject and SHA-256 thumbprint) now
  travels from every TLS, HTTPS and DTLS listener to `Peer`, and
  `Authenticator::auth` is given the peer, so a re-authentication with a
  bound token is checked too.
- Not wired into `lir-node`: there is no verifier to run until the
  linking question is answered.

## 2026-11-13: Dannan and Dagda for all three systems

Gate: `lir_tests` 307, unchanged (no code changed; not rerun).

- The maintainer ruled that Dannan and Dagda serve Macha, Lugh and Lir as first-class
  users. Lir wrote its needs of both as behaviour and numbers
  (`TODO/2026-11-13-lir-needs.md`: Dannan N1 to N6, Dagda G1 to G9),
  answered Dagda's five questions from Lir's plan files (sizes there are
  the Lir session's estimates), and reviewed Dagda's design (R1 to R11).
  Every point landed in `../dagda/TODO/2026-11-13-three-systems-design.md`
  before the maintainer ruled; Lugh's review went in beside it.
- Found while answering: a second hot path beside the inflight log
  (each QoS 1/2 delivery changes a session's state, and offline sessions
  queue up to 10,000 messages); the stack document's "2-4 commits a shard"
  assumes a 250-500 ms batch, where a 10-20 ms batch gives about 27-32;
  Dagda's per-space adverts against Dannan's 64-advert cap; and a union
  merge that would bring back a removed QoS 2 message after a split
  (closed by tombstones for 10 minutes, a stated limit).
- The rulings are under Decisions, 2026-11-13: the log noun, the
  register map, per-space placement and k, contract v2 at slice 4, the
  10-minute QoS 2 limit, PUBACK p99 under 50 ms, Dannan's ABI promise, the
  partition reading (even is a minority), and a shared listener later.
- Lugh agreed that `lugh_client` hides where its signing keys come from
  and keeps HTTPS JWKS for a node outside Lugh's cluster.
- The Macha session confirmed that measuring the head commit rate early
  costs its port nothing (stage one does not touch heads).

## 2026-11-13: Dannan 0.5.6, and step 5 designed

Gate: `lir_tests` 307 passed, 0 failed, on Dannan 0.5.6 (Release, macOS,
warnings as errors, no warnings); the 44 cluster, process and shared-group
tests passed three runs in a row.

- Repinned from Dannan 0.4.1 to 0.5.6 (`feature/dannan-0.5`), ahead of
  Dagda 0.1.5 at the maintainer's word: 0.5 is the minor Dagda 0.1.5 builds on, and
  Lir links no Dagda yet. No source changed. Since 0.4.1 Dannan's Context
  changes are additive; a call is watched for progress only when it asks,
  and deadlines follow injected time. Neither moved a test.
- Step 5's design, `TODO/2026-11-13-step5-design.md`: direct fan-out kept,
  an inflight log per shard appended before PUBACK, session queues only for
  offline or saturated sessions, session state and retained in replicated
  spaces, the sequence reserved in blocks of 65,536, minority behaviour, and
  19 tests to write first, each when its Dagda slice lands. The maintainer answered its
  three questions; Dagda answered its four (any node appends directly to a
  log's replicas; a dot is the owner's, and an append's origin must be the
  appender, so a queue entry takes the subscriber node's own dot; acks and
  retires may name any origin's dots, and Lir restricts retiring itself).
- README status brought up to date, with the QoS 2 split limit stated.
- `develop` pushed to `origin` at the maintainer's word (its first push there).

## 2026-11-13: Dannan 0.6.0, and step 5's groundwork

Gate: `lir_tests` 323 passed, 0 failed, on Dannan 0.6.0 (Release, macOS,
warnings as errors, no warnings); the cluster, process and shared-group
tests passed three runs in a row. 16 new: 2 shard, 6 stored entries, 8
sequence blocks.

- Repinned to Dannan 0.6.0, the first with the ABI promise and the minor
  Dagda 0.1.5 builds on; no source changed.
- `shard::session_shard_of`: SHA-256("lir-sess:" || client id) mod S.
- `cluster::SequenceBlocks` over a `SequenceStore` seam: seq handed out from
  blocks whose upper bound is durable first, the next block reserved at half,
  one reservation in flight, nothing handed out past the durable bound, a
  restart starting at the bound, completions posted.
- `encode_inflight`/`encode_queued` and their decoders: Lir's log entries
  under their own format and kind. Found while writing them: the wire codec
  sends an expiry as what is left, which is wrong for anything that outlives
  the process, so a stored expiry is wall-clock time (`Clocks`).
- Two test cases first failed because a drained `io_context` stops itself;
  the tests restart it (`drain`). Test code only.
- A first GCC build on node-a was started before the Macha session's
  note that the machine was Dagda's, and stopped within minutes; both
  sessions were told.

## 2026-11-14: Linux arm64, and a standing cluster on the build nodes

Gate: `lir_tests` 323 passed, 0 failed, on node-c (arm64, Debian 13,
GCC 14, Boost 1.83, Release, warnings as errors, no warnings; -j1, nice 19,
throttled=0x0 throughout), and 323 on macOS.

- Lir's first GCC build. Boost's floor lowered to 1.83, as Dannan's and
  Dagda's. GCC 14 found two shadowed names in the broker, dangling elses
  around test macros and lambda parameters shadowing a fixture's node, all
  fixed; and a false positive on MQTT-SN framing's growing vector, twice
  (a reserve traded one warning for another), fixed by sizing the frame
  once. No behaviour changed.
- `deploy/test-cluster/`: a systemd unit, a node config template, an install
  script run from the workstation, and a README of the coordination topics. Three
  nodes joined, `$lir/cluster` reporting 3 with consistent limits.
- Trial across the three build nodes: QoS 2 from node-a to a subscriber on
  node-c, in order; a retained value published on node-b served from
  node-c; a persistent session leaving node-a and resuming on node-c
  with its three queued messages; with node-c stopped, the same session
  resuming on node-a from its standby with both messages queued since;
  node-c rejoining and serving retained values again.
- Announced to the Macha, Dagda, Dannan, Lugh, Corvus LoRa, Corvus
  Radioberry, AthenaSIP and Miach sessions.

## 2026-11-14: GET /retained, built on the build nodes

Gate: `lir_tests` 329 passed, 0 failed, on node-b (GCC 14, Release,
warnings as errors, no warnings, throttled=0x0). 6 new HTTP cases, and the
token case extended. From now on every Lir build and test run is on the
build nodes, never the workstation (the maintainer, 2026-11-14), taking turns with the other sessions
and claiming the machine on the board first.

- `GET /retained?filter=F[&filter=G]`: the retained messages the filters
  match, as one JSON array of the event stream's objects, then the
  connection closes. The snapshot is exact: the broker sends a
  subscription's retained messages inside the SUBSCRIBE, on the edge's
  strand, so nothing live can be among them. Past
  `HttpConfig::maximum_retained_bytes` (4 MiB) it is refused whole with 413.
- `message_json` is the one JSON form of a delivered message, shared by the
  event stream and the snapshot.

## 2026-11-14: hosted CI, fuzz targets, and the board's rule

- Hosted CI as the stack has it, copied from Dannan's and renamed:
  `tools/ci.sh` (check, release, clang, asan, tsan, coverage), the CI and
  CodeQL workflows, Dependabot, Codecov and the README's badges. Dannan is
  fetched from its public repository where there is no sibling checkout.
  `tools/ci.sh check` passes on the whole tree; the build steps are not yet
  run anywhere.
- Four fuzz targets written (`feature/fuzz-targets`), not yet built: no build node
  has clang yet.
- The standing cluster upgraded to the build with `GET /retained`; claims
  and statuses survived the rolling restart. The maintainer's rule that nobody
  schedules the build nodes posted on the board (`stack/rules/scheduling`), in the
  board's README, and to every session sharing the build nodes. Claims moved to one
  topic per session per machine.

## Archived from ACTIVE.md, 2026-11-14

`ACTIVE.md` was rewritten to where things stand (the maintainer: "rationalise TODO and
prep for clear"). Its text until then, word for word, with its headings
pushed down one level:

Last updated: 2026-11-13. Steps 1 to 4, session standbys, the system
topics (all but `$lir/shards`, which follows step 5), HTTP and HTTPS,
MQTT over TLS and over WebSocket, client certificates, the
specification's MAYs, Dannan 0.4.0 (the config file, its percent
decoder), topic scopes, the moderate default limits and memory pressure
are on `develop`; gate `lir_tests` 248. `COMPLETED.md` is the record.
Server Reference at CONNECT is done (248); memory thresholds and the
ledger (251), the rules engine (270), MQTT-SN 1.2 (288) and over DTLS
(291); close_notify on TLS closes and the HTTPS certificate subject (295);
Dannan 0.4.1 (295); the Lugh token authenticator's own checks, behind a
`TokenVerifier` seam (307).
"Resume here" (2026-11-13): Dannan and Dagda now serve Macha, Lugh and
Lir as first-class users (the maintainer, 2026-11-13; Decisions below). Lir's needs
of both, and Lir's review of Dagda's design, are
[`2026-11-13-lir-needs.md`](2026-11-13-lir-needs.md). The design the maintainer ruled
on is `../dagda/TODO/2026-11-13-three-systems-design.md` ("Ruled",
Dagda `a058df9`); Dannan's is `../dannan/TODO/2026-11-13-stack-needs.md`.
Lir's work while they build, all of it chosen by the maintainer on 2026-11-13:
1. Step 5's design rewritten onto the ruled nouns, with its tests written
   first. No code against an unfrozen tag until (3).
2. ~~Repin to Dannan 0.5.6~~ Done 2026-11-13, ahead of Dagda 0.1.5 at
   The maintainer's word (Lir links no Dagda yet): `feature/dannan-0.5`, gate 307
   (`COMPLETED.md`). Then, by the maintainer's ruling relayed by Dannan the same
   day, the ABI promise ships as Dannan 0.6.0 (`libdannan.so.0.6`), since
   0.5.4 had already changed 0.5's ABI, and Dagda 0.1.5 will be on 0.6.0.
   So Lir repins once more, to 0.6.0, when it is tagged. The interface work
   (deferred reply, partition reading, the listener) is Dannan 0.7.0.
   ~~Repin to 0.6.0~~ Done 2026-11-13: `feature/dannan-0.6`, gate 323.
2a. ~~Step 5's groundwork that needs no Dagda~~ Done 2026-11-13
   (`feature/step5-groundwork`, 16 new tests): `session_shard_of`, the
   sequence blocks behind `SequenceStore` (a node-local space fills it), and
   the stored forms of inflight and queue entries, whose expiry is
   wall-clock time because a log entry outlives the steady clock.
2c. ~~Linux arm64~~ Done 2026-11-14 on node-c: GCC 14, Debian 13,
   Boost 1.83, warnings as errors, all 323 tests (`COMPLETED.md`). The build nodes
   are shared (node-a .169, node-b .170, node-c
   .202; root over ssh): ask the session using a machine first, post a claim
   on the standing cluster, modest -j, nice 19, one heavy job per build node.
2d. The standing Lir cluster on the three build nodes (the maintainer, 2026-11-14: "install a
   Lir cluster on these machines and use it to test lir, and coordinate
   work"): `deploy/test-cluster/`, ports 7537, 1983 and 8183, the key kept in
   `build/test-cluster/` on the workstation. Sessions post claims on
   `stack/pis/<host>/claim` and status on `stack/sessions/<name>/status`.
   Upgrade it with `deploy/test-cluster/install.sh` after each Lir change
   worth trying on real machines.
2f. ~~Hosted CI~~ Done 2026-11-14 (`tools/ci.sh`, `.github/`, `codecov.yml`,
   badges): check, release, clang, asan, tsan and coverage on Debian 13
   x86-64 and arm64, and macOS. `tools/ci.sh check` passes; the build steps
   are first proven on a build node when one is free (`tools/ci.sh release`).
2g. Fuzz targets for every parser of untrusted bytes, on
   `feature/fuzz-targets` (MQTT-SN, the HTTP target, the cluster codec, the
   stored entries), unmerged until built with clang on a build node and each run
   for a bounded time.
2e. The claim board plugin, specified at the maintainer's request for another session
   to implement (`../coordination/TODO/2026-11-14-claim-board-plugin.md`).
   Lir owes it two things:
   - ~~`GET /retained?filter=F`~~ Done 2026-11-14: the retained messages
     matching F as a JSON array, then close; 413 past
     `maximum_retained_bytes`, never cut short. Gate 329 on node-b.
   - Once the plugin lands: migrate the board to one topic per session per
     machine (`stack/pis/<host>/claims/<session>`), clear the old
     `stack/pis/<host>/claim` topics, and update `deploy/test-cluster/README.md`.
     The first layout lets a second claim overwrite the first.
2b. When Dannan lands its partition reading (the maintainer, 2026-11-13): wire it
   into the interim, so a node on a minority side (an even split is one)
   degrades or refuses QoS 1/2 per its setting, ahead of Dagda.
3. Step 5 code against each Dagda slice tag as it lands, at Lir's own risk;
   nothing ships before contract v2 freezes at slice 4.
4. The stale plan text: the plan file's session and wildcard heads (the
   layout of 2026-11-04 replaced them), and the stack document's "2-4
   commits a shard", which is Dagda's file (asked of it).
Still waiting elsewhere: MQTT-SN 2.0 on OASIS (only working drafts; the maintainer
said continue); the Lugh plugin on Lugh's `lugh_client` (P10), which is
the `TokenVerifier`, hides where Lugh's signing keys come from, and keeps
HTTPS JWKS working for a node outside Lugh's cluster (agreed by Lugh,
2026-11-13).

The design is the maintainer's, 2026-11-03:
[`2026-11-03-lir-plan.md`](2026-11-03-lir-plan.md), with the seven
further sections in [`2026-11-04-lir-sections.md`](2026-11-04-lir-sections.md).
These files are the source of truth; the Claude Doc it began as is a
starter that will be deleted (the maintainer, 2026-11-04). How the projects divide the work is
`../dagda/TODO/2026-11-03-stack.md`, the one copy, which has Lir in it
("Lir on Dagda", and the maintainer's "Decided (the maintainer, 2026-11-04)").

### What Lir is

The maintainer, 2026-11-03 night: "It must expose a MQTT interface and support all
MQTT functionality. But, it needs to be distributed, multimaster,
clustered, using Dannan as the network layer." Then: "Create a MQTT event
system that is clusterable and distributed based on Dannan." Lir is
written new; Macha has no event system to extract (`COMPLETED.md`,
2026-11-03 night).

- All of MQTT 3.1, 3.1.1 and 5.0. A feature left out is a finding.
- Distributed and clustered: a client connects, publishes, subscribes and
  resumes its session at any node.
- Multimaster: any node accepts any client operation; both sides of a
  split serve and merge after.
- Dannan is the network layer; what Lir persists lives in Dagda.

### Decisions

- (2026-11-14, the maintainer) No builds or test runs on the workstation: every Lir build is
  on the shared build nodes. Nobody schedules the build nodes: a session claims a free
  machine on the board (`deploy/test-cluster/README.md`) and has it, first
  come, first served; one heavy job per build node. Hosted CI is added, as the
  stack has it; it runs only when the maintainer pushes. Split joins (nodes added
  during a partition can give two majorities) are a stated limit in
  Dannan's partition reading, not prevented.
- (2026-11-13, the maintainer) "You all need to discuss your needs and ensure the
  the more fundamental projects, namely Dannan and Dagda, support Macha,
  Lugh and Lir as first class citizens. This was the purpose of splitting
  out the technologies." Then, on the designs that followed:
  - Dagda (`../dagda/TODO/2026-11-13-three-systems-design.md`, "Ruled"):
    the log is a Dagda noun, for Lir's inflight log, offline session
    queues and message history; built-in register-map and counter-map
    merges, with retained as a register-map space (expiry per key, and a
    keys-changed notice for nodes that do not hold the space); head spaces
    on `replicas(N, scheme, capability)` with per-space k; leases on head
    spaces aligned with each shard's replicas; node-local spaces; adverts
    aggregated, generations on channels. Contract v1 frozen at Dagda
    `0.1.4`, v2 frozen at slice 4 (everything Lir needs), the catalogue as
    v3. Order: slice 0 (0.1.5 on Dannan 0.5.6, then the measurement) first.
  - A stated limit: a log keeps removed dots for 10 minutes, Lir's QoS 2
    dedup window, so a split longer than that can deliver a QoS 2 message
    twice.
  - PUBACK at QoS 1 with k = 2: p99 under 50 ms over the whole path
    (Lir's tick, the append, the group commit synced on k replicas), the
    pass mark for Dagda's log prototype.
  - Dannan (`../dannan/TODO/2026-11-13-stack-needs.md`): an ABI promise
    (patch releases keep it, abidiff in CI, `libdannan.so.0.5`, CMake
    `SameMinorVersion`); a hosted system pins one Dagda tag and takes its
    Dannan minor. A partition reading, majority or minority of the roster
    as last agreed (a join adds, only a retire removes); an even split is
    a minority. A shared client listener below the framing, with stack
    Decided 9 standing; Lir keeps its own listeners until it lands and Lir
    has reviewed it. A small-message bench, here first, the build nodes at the maintainer's
    later word.
- (2026-11-04, the maintainer; stack document "Decided", and `../dannan/TODO/ACTIVE.md`)
  - Dagda durability levels: Ephemeral (memory), Persisted (disk, one
    node), Replicated (disk, k nodes per space, default 2), Durable (all
    available replicas). Lir: QoS 0 Ephemeral, QoS 1/2 Replicated.
  - No `lir.local`: other systems are ordinary MQTT clients of Lir.
  - Multimaster as Lir described it: any node accepts any operation, "the
    per-shard lease coordinates fan-out", both sides of a split serve and
    merge. (Step 4's interim fans out without it: question 1 below.)
  - Dagda's `open()` takes Lir's placement salt; Dagda reports each
    space's replica set.
  - Authentication is pluggable (`contract::Authenticator`); a Lugh plugin
    depends on Lugh. Token verification does not move into Dannan.
  - HTTP is each hosted system's own; Lir writes its own publish and SSE.
  - Message types from 512, registered freely (Lir: `0x4C00`-`0x4C02`).
  - One process per node: `lir-node` is a node and advertises `lir`.
  - Process-level tests are GoogleTest only, no Python.
  - Dannan `0.2.0` is cut; Lir builds against the tag.
- (2026-11-03 night, the maintainer) Lir is its own project and agent, with
  Dannan's styles, conventions and layout. Macha is not modified until
  The maintainer says so.
- From the stack: rules 1 to 4; dependencies point down; hosted systems
  never address one another; the async shape; versioned contracts.

### The plan

1. **Wire and topics.** Done (73).
2. **The broker on one node.** Done (125).
3. **The edge.** Done (132; the mosquitto process test landed in step 4;
   TLS on the MQTT port, 165; HTTPS, 170; WebSocket, 214; client
   certificates, 227; close_notify on close, and the HTTPS certificate
   subject, 295).
4. **The cluster on Dannan.** Done as an interim (151): see
   `COMPLETED.md`. Sessions mirrored to configurable standbys (answer
   2): done (152).
5. **On Dagda.** As ruled 2026-11-13: per shard, an inflight log and a
   session-queue log (Dagda's log noun), a session-state head space with
   Lir's merge, a retained register-map space and a lease space, all on
   the shard's replicas; history as a log per topic filter; objects for
   payloads over 64 KiB; the sequence counter in a node-local space; the
   retained-topic index on every node from the keys-changed notice.
   Builds on Dagda's slice tags 1 to 4 as they land; pins contract v2 at
   slice 4. Its design is rewritten first (Resume here, 1).
6. **System topics, HTTP, authentication.** System topics done (154);
   HTTP publish and SSE done (163); the Lugh plugin waits on Lugh. `$dannan/` and `$lir/`
   retained topics from the Context's status; HTTP publish and SSE; a
   Lugh authentication plugin. No dependency but Lugh's tokens for the
   last.

### The maintainer's answers (2026-11-04, asked in turn)

1. Fan-out: direct, on Dannan's channels. Per-shard leases only where
   one writer matters: the inflight log and retained heads on Dagda
   (step 5).
2. Interim: sessions are replicated now, before Dagda: each persistent
   session mirrored in memory to standby nodes chosen by Dannan's
   placement, updated as it changes, taken from a standby when its node
   is gone. The number of standbys is configurable (Lir's default: one,
   two copies, as Dagda's k = 2). Retained everywhere and PUBACK on
   acceptance stay interim until step 5.
3. Retained ordering: the Lamport counter, then the node id, in the
   design too.
4. Load: up to about 10,000 messages a second across the cluster;
   messages mostly under 1 KiB; 64 KiB inline limit kept.
5. Header tagline: "Lir - The Message Broker".
6. Storage layout on Dagda approved: per-shard spaces for retained,
   inflight and sessions (3 x 256), writes batched per shard, the
   wildcard index in memory.
7. Namespaces kept: `mqtt`, `subs`, `shard`, `broker`, `edge`, `cluster`.
8. Next: session replication, then step 6 (system topics, then HTTP
   publish and SSE).
9. The design doc's seven unwritten sections: answered in turn on
   2026-11-04, recorded in
   [`2026-11-04-lir-sections.md`](2026-11-04-lir-sections.md). New scope from them: MQTT over WebSockets, MQTT-SN
   1.2 and 2.0, a declarative rules engine, opt-in message history,
   Server Reference for load balancing. Order: Dannan 0.3.0 and scopes,
   anonymous refused, WebSockets, configurable system topics, config
   file (Dannan's), client certificates; first release after step 5.

### What Lir waits on

- Dagda: slices 0 to 4 of the three-systems design (about 18 sessions):
  0.1.5 and the measurement, node-local spaces, replicated spaces with
  per-space k, the merges and leases, the log; contract v2 at slice 4.
- Lugh: `lugh_client` (its P10), whose offline token verification fills
  the plugin's `TokenVerifier` (the maintainer, 2026-11-04); the tokens have landed (O7).
- Dannan: the partition reading (Lir's minority behaviour), the shared
  listener (optional for Lir until reviewed), the small-message bench.
  Lir is on `0.5.6`; Dagda 0.1.5 will be on `0.6.0`, the first minor
  with the ABI promise, and Lir follows it. The partition reading comes in
  Dannan `0.7.0`.
- Lugh: the grammar of scopes in its tokens.
- OASIS: an approved MQTT-SN 2.0 Committee Specification.

## 2026-11-14, early: Dannan 0.7.0 and the partition reading

Gate: `lir_tests` 334 passed, 0 failed (329 before, five new), by one
`tools/ci.sh release` run on node-b (arm64, Debian 13, GCC 14,
Ninja, Release, warnings as errors, no warnings, `throttled=0x0`). That
run is also the CI script's first real build on Debian. Merged to
`develop` as `9fced0a`; nothing pushed.

- Dannan repinned at `0.7.0` (`c05ec2a`).
- The minority side (`aaa3a83`): the cluster watches
  `Nodes::watch_partition()` on the edge's strand and gives each reading
  to the broker (`Broker::partition`). On a minority side QoS 1 and 2
  from clients are accepted and counted (`Stats::under_replicated`,
  `--minority degrade`, the default) or refused with 0x97
  (`Stats::refused_minority_publishes`, `--minority refuse`; a 3.x client
  is closed), in `Link::on_publish` beside the memory-pressure refusal,
  so MQTT, MQTT-SN and HTTP publishes all pass it. QoS 0 always flows.
  `$lir/nodes/<id>` carries `"partition"`: side, members, reachable, the
  unreachable ids, the setting and both counts. Interim limits, stated:
  `refuse` is at once (nothing to wait on before Dagda); session and
  retained commits and server-made messages (wills) are not covered until
  step 5. Tests: `BrokerTest.AMinoritySideDegradesByDefault`,
  `BrokerRefusingOnMinority.QoS1And2AreRefusedWith0x97AndQoS0Flows`,
  `BrokerRefusingOnMinority.NoReadingOrAMajorityRefusesNothingAndAHealEndsTheRefusal`,
  `SystemJson.ANodeReportsItsSideOfASplitAndWhoItCannotReach`,
  `ClusterTest.ANodeCutOffFromMostOfTheClusterReadsMinorityAndSaysSo`.
- `tools/ci.sh` takes `LIR_CI_JOBS` (`8ba3dd6`).
- `feature/fuzz-targets`: the whole build instrumented for fuzzing, not
  only the harnesses, which left libFuzzer without coverage of the
  parsers (`9543948`); not built, it waits for the release gate under
  The maintainer's rule against wasted runs (recorded in `ACTIVE.md`).
- Recorded: Dagda's slice 0 verdict on the build node; the maintainer's P10 rulings for the
  Lugh plugin; Lugh on Dannan 0.7.0 and building on Lir's Dannan.
- The standing cluster upgraded to `9fced0a` at the maintainer's word (03:52Z): every
  node's `$lir/nodes` reads majority, 3 of 3; the board's claims survived.

## 2026-11-14, morning: a token's exp on the wall clock

Gate: `lir_tests` 335 passed, 0 failed (one new), by one `tools/ci.sh
release` run on node-c (warnings as errors, no warnings,
`throttled=0x0`). Merged to `develop` as `dc8a022`; nothing pushed.

- Found by the Lugh session: `TokenVerifier::verify(token, now)` passed
  the steady clock, while a token's exp and nbf are wall-clock Unix
  seconds, so no verifier could check a lifetime and the grant's end was
  timed on the wrong clock. Now `verify(token)` (the verifier checks nbf
  and exp on a WallClock of its own), `TokenClaims::expires_unix`, and a
  `TokenAuthenticator` given a WallClock: it refuses an exp at or before
  the wall's now (stricter than Lugh's 60 s skew, stated in
  `lir/auth/tokens.h`) and ends the grant on the steady clock when the
  time left on the wall runs out, capped at ten years (`c85251a`,
  `2c770b0`). Test: `TokenAuthenticatorTest.TheExpiryIsReadOnTheWallClockAndKeptOnTheSteadyOne`.
- Reviewed Dannan's shared listener design (P3); Lir's requests all taken
  in (`ACTIVE.md`, item 3).

## 2026-11-14, afternoon: the shutdown DISCONNECT and the Lugh plugin

Gate, one tree with both, on node-c (warnings as errors, no
warnings, `throttled=0x0`): `tools/ci.sh release` 337 passed, 0 failed;
`tools/ci.sh lugh` (the plugin on) 343 passed, 0 failed. A first run failed
one test, `WebSocketTest.StopClosesWebSocketClients`, written before a
stopping node told 5.0 clients why: it read the new DISCONNECT as the
connection still open. Its expectation now follows the design
(`add9faa`); the run was repeated for that fix and nothing else. Merged to
`develop` as `08447d6` and `d7bd0d9`. `develop` pushed to `69b248d` at
The maintainer's word (the maintainer, 2026-11-14), the hosted CI's first run.

- The shutdown DISCONNECT (`2e77e48`, `add9faa`): `Broker::shutting_down()`
  ends every link, a 5.0 client told 0x8B (Server shutting down) first;
  `Edge::stop` calls it, waits for each connection's clean end within the
  2 s bound, then closes what is left at once. Wills and sessions go as
  before. Tests: `BrokerTest.ShuttingDownEndsEveryConnectionAndTellsA5ClientWhy`,
  `EdgeTest.StoppingTellsA5ClientTheServerIsShuttingDown`. A stop can now
  take up to the clean-end bound.
- The Lugh plugin (`77a4b17`): `LughVerifier`, Lir's `TokenVerifier` over
  `lugh::client::Verifier`, `x5t` decoded with Lugh's base64url and refused
  unless a SHA-256 thumbprint; `LughPlugin` owning the HTTPS client, the
  Verifier and the key source; `LIR_BUILD_LUGH` fetching `lugh_client` at
  `2b60b52` on Lir's Dannan, linked into `liblir` whole with Lir's own
  Boost.JSON definitions left out; `lir-node --lugh-issuer --lugh-ca`;
  `tools/ci.sh lugh`. Tests (real tokens, signed by Lugh's `jws_sign`):
  `LughVerifierTest.AGenuineTokenGivesWhatLirReads`,
  `.ACertificateBoundTokenCarriesItsThumbprint`,
  `.AThumbprintThatIsNotSHA256IsRefused`, `.AKeyBoundTokenIsMarkedBound`,
  `.AForgedTokenSaysWhy`, `.AClientSignsInWithALughToken`.
- Reviewed Dannan's P3 code (`68c3cc5`): five findings, all fixed in
  `0940e73` (`ACTIVE.md`, item 3).
- Recorded: The maintainer's answers (push develop, tag Dagda 0.2.0, yes to P3's name
  and phasing, fuzzing at the release); Dagda's pause for Macha's port;
  node-d a claim host only; the board the only channel for the
  build nodes (later reversed by the maintainer: a holder may be asked to make room; claim
  board 0.2.0 claims cores and queues waits).

## 2026-11-14, evening: the Lugh plugin repinned at the end of P10

Gate: `tools/ci.sh lugh` 343 passed, 0 failed, on node-c (2 cores
claimed, warnings as errors, no warnings, `throttled=0x0`); the release
configuration is untouched by it. Merged to `develop` as `e018338`;
nothing pushed.

- Lugh at develop `dbb0573` (P10 complete, on Dannan 0.7.0, `f163c97`):
  `AccessToken::x5t` arrives decoded and a malformed one is refused by
  Lugh, so `LughVerifier` copies it; the tests sign with Lugh's public
  `lugh/client/testing.h` (`lt::sign_access_token`, `lt::jwks`). Lir no
  longer includes Lugh's `jose.h` or `core/encoding.h`.

## 2026-11-15: Dannan 0.8.0, and Lir's listeners on Dannan's

Gates on node-c (2 cores claimed each time, warnings as errors,
no warnings, `throttled=0x0`); merged to `develop`, nothing pushed:
- The repin (`b0a949f`, merged `bb3c4fc`): `tools/ci.sh release` 337
  passed, `tools/ci.sh lugh` 343 passed, both linking `libdannan.so.0.8.0`.
  Lugh's `dbb0573` builds and passes on 0.8.0 in Lir's build. The pin
  `LIR_DANNAN_TAG` is no longer a cache variable: an existing build tree
  kept the old tag before.
- The edge (`0ef5d58`, `f849320`, merged `5312550`): 337 passed. MQTT over
  TCP and TLS straight over a `listen::Stream` (`RawConnection`); over
  WebSocket and WebSocket-over-TLS as `websocket::stream<Stream&>` with
  Dannan's teardown (`WebSocketConnection`). A first run failed
  `ClientCertificateTest.ACertificateFromAnotherCaIsRefused`: Dannan's
  `optional` client certificates let an unverified one through, and the
  edge read it as none, so an intruder came in anonymous. The edge now
  refuses a stream that presented a certificate which did not verify; the
  test was right and is unchanged; the run was repeated for that fix.
- HTTP and HTTPS (`eda9990`, merged `f49f3d0`): 337 passed. `Exchange`
  runs over a `listen::Stream`, Dannan's queue and draining close in place
  of its own; HTTPS refuses an unverified certificate as MQTT over TLS
  does.
- Open with Dannan: TCP_NODELAY (`ACTIVE.md`, item 3).

## 2026-11-15: Dannan 0.8.1

- Lir reviewed Dannan's `feature/no-delay` (three points, all taken: the
  io context found by type, the restart race stated, `no_delay()` on the
  stream's executor only). 0.8.1 sets TCP_NODELAY on every accepted
  socket and `stop_accepting` no longer hangs from an io thread or a
  context that never ran.
- The repin (`9bef0c3`, merged `508f1d6`), no code change: `tools/ci.sh
  release` 337 passed on node-c (2 cores claimed,
  `libdannan.so.0.8.1`, `throttled=0x0`).
- Lugh is on 0.8.0 at `ab28e2d` with `lugh_client` unchanged since
  `dbb0573`; Lir's Lugh pin stays until Lugh's first release.
- Dannan 0.8.2 (placement copies only its answer; a listener key refused
  for what is wrong with it), the repin (`4cdccbd`), no code change:
  `tools/ci.sh release` 337 passed on node-c (2 cores,
  `libdannan.so.0.8.2`, `throttled=0x0`).
- Answered Louhi (the new compute project) on using Lir for its
  lifecycle events: as any MQTT client, its own topic root, scopes from
  Lugh tokens; durability waits on step 5.

## 2026-11-15: lir::client

The maintainer approved it and its interface in Lir's session (asked by Louhi, whose
events step needs it; Lugh's principal-events publisher and Mimir's Lugh
plugin will use it too).
- `c1e637d`: the codec is `lir_mqtt` (`lir::mqtt`) alone; `lir` links it.
  `LIR_BUILD_SERVER=OFF` builds only the codec and the client; Dannan is
  fetched only when the consumer has no `dannan::dannan` already.
- `27f775d`: `lir/client/client.h`: MQTT 5.0 or 3.1.1 over TCP or TLS
  (trust bundle or system store, IP or name checked, no SNI for an IP),
  password asked at each CONNECT, QoS 0 and 1 publish (queued while
  down, bounded, resent until PUBACK) and receive, retained, a will,
  wildcard and shared subscribe kept across reconnects (5.0 subscription
  identifiers, 3.1.1 matched locally), keep alive, Server Reference
  followed, jittered backoff. One strand; every callback posted there.
  The outbound connection is a local seam (`lir/client/transport.cpp`)
  until Dannan's StreamConnector, whose design Lir reviewed (executor
  constructor, IP SANs and no SNI for literals, the stream on the
  executor given: all taken).
- Gate on node-c (2 cores, `throttled=0x0`): `tools/ci.sh release`
  351 passed (337 and 14 new `ClientTest`, `ClientTlsTest`); `tools/ci.sh
  client` builds with no warnings. Merged `5a03682`; a `client` job in the
  hosted CI.
- `9f300c4`: `LIR_DANNAN_REPOSITORY` and `LIR_LUGH_REPOSITORY` are CACHE
  STRING, not PATH: a URL given with -D became a path under the source
  tree, and every hosted CI job had failed at configure since the jobs
  started (the logs of run 43632325244 show it). Found by Louhi. Proven
  only by the hosted CI on the next push; no build node run, as the build nodes pass a
  path. Dannan's public repository carries the 0.8.2 tag.

## 2026-11-15: the first full hosted CI

- After the repository URL fix, every job builds. TSan found a Dannan 0.8
  use-after-free (a ledger Lease released by a call completion after its
  Node was destroyed): reported, fixed by Dannan in 0.9.0, suppressed by one
  line until the repin (`5251686`). CodeQL needed `actions: read`
  (`3eaa8f9`) and code scanning, which came with the repository going
  public at the maintainer's word.
- Flaky, observed once: `ClusterTest.ASharedGroupAcrossNodesGetsEachMessageOnce`
  timed out on asan (arm64) waiting 10 s for `shared_members == 2` (run
  43749501256); the job passed when re-run once. Groups go on a stream
  channel, so a lost publish is unlikely; slowness under ASan on the arm64
  runner is the likely cause. Watch for a second occurrence before
  changing the bound.
- Dannan 0.8.3 and `lir::client` on its `StreamConnector` (`304fd43`):
  the transport seam deleted, a QoS 0 publish done once handed to the
  connection. `tools/ci.sh release` 351 passed and `tools/ci.sh client`
  clean on node-b (2 cores, `libdannan.so.0.8.3`,
  `throttled=0x0`). Hosted CI all green from `91cd389` (CodeQL included,
  the repository public).

## 2026-11-15: lir::client finds Lir's nodes in Dannan's roster

The maintainer: Lir keeps no node list of its own, "that's what Dannan is FOR". A
configured broker list and a Lir topic of endpoints were both dropped.
- `79c10d7`: `ClientConfig::nodes` (the process's `dannan::Nodes`): each
  attempt reads the active nodes advertising `lir` with a `lir.endpoints`
  for its transport, takes the next in turn from a random start, so a lost
  node is passed over at once; the backoff waits only once each has failed
  since the last connection. `host`/`port` remain for a process not on
  Dannan. The advert's key and format are `lir/contract/endpoints.h`, one
  copy for the cluster and the client.
  `ClusterTest.AClientFindsLirsNodesInDannansRosterAndFailsOver`.
- `bace340`: `install.sh` ships `liblir_mqtt.so` (lir-node links it now)
  and takes `LIR_ONLY=<name>` for a node-by-node upgrade; the unit gets
  `MemoryMin=192M` and `OOMScoreAdjust=-500`; node-d is the fourth node.
- Gate on node-d (2 cores, `-j1`, `throttled=0x0`): `tools/ci.sh
  release` 347 passed and 5 skipped (the process tests, without
  mosquitto-clients there); mosquitto-clients installed and the 5 run:
  passed. 352 in all. `tools/ci.sh client` clean.

## Archived from ACTIVE.md, 2026-11-15

Done items the plan carried in full until today, kept here verbatim.

3. **Dannan's shared listener** (P3, `dannan::listen`). Done for TCP
   (2026-11-15): Lir is on Dannan 0.8.0 (`bb3c4fc`), MQTT over TCP, TLS,
   WebSocket and WebSocket over TLS (`5312550`) and HTTP and HTTPS
   (`f49f3d0`) run on `StreamListener`s on the edge's strand
   (`strand_per_stream = false`); Lir's own acceptors, TLS handshakes and
   write queues for MQTT and HTTP are gone. `edge::listener_config`,
   `verified_certificate(Stream)` and `presented_unverified` are shared by
   both. A client certificate that did not verify is refused, as Lir's
   handshake refused it (Dannan's `optional` lets it through with the
   verdict). TCP_NODELAY: Dannan 0.8.1 sets it on every accepted
   socket (`Stream::set_no_delay` opts out), so small MQTT packets no
   longer wait on Nagle; Lir repinned with no code change (`508f1d6`,
   337 passed), after reviewing the branch. Next: the MQTT-SN DTLS gateway (`lir/sn/gateway.cpp`) onto
   `DatagramListener` at Dannan 0.9.0, then `edge::server_context` and
   `verified_certificate(SSL*)` go. At the 0.9.0 repin, also remove the TSan suppression of
   `RetainedMemoryLedger::release` (`lir/test/tsan-suppressions.txt`), a
   Dannan 0.8 use-after-free fixed there.
4. **The Lugh plugin.** Done and merged (`d7bd0d9`; `lir/auth/lugh.h`,
   `LIR_BUILD_LUGH`, `--lugh-issuer`, `--lugh-ca`, `tools/ci.sh lugh`),
   repinned (`e018338`) at Lugh's develop `dbb0573`, the end of P10, on
   Dannan 0.7.0: `AccessToken::x5t` comes decoded, so the adapter copies
   it, and the tests sign with Lugh's public `lugh/client/testing.h`;
   nothing internal to Lugh is included any more. Next: repin at Lugh's
   first release (the maintainer paused Lugh after P10). The hosted CI has a `lugh`
   job (the maintainer said yes, 2026-11-14): it checks out Lugh's private develop
   with the repository secret `LUGH_READ_TOKEN` and runs `tools/ci.sh
   lugh`; without the secret it says so and builds nothing. The maintainer adds the
   secret. The record of how it was
   shaped (`../lugh/TODO/2026-11-14-p10-plan.md`): The maintainer's rulings (2026-11-14): the
   plugin is optional and Lir builds without Lugh when it is off; it is a
   few-line adapter, `lir::auth::TokenVerifier` over
   `lugh::client::Verifier(issuers, WallClock)`: `set_keys(issuer, jwks)`
   from the key source, `verify_access(token, audience)` answering an
   `AccessToken` (subject, audiences, scopes, expires in Unix seconds,
   jkt, x5t as base64url text, decoded to 32 bytes) or a reason; headers
   `lugh/client/verifier.h` and `lugh/client/tokens.h`. Lir's seam carries
   exp as wall time since `dc8a022` (Lugh's finding), and Lir refuses a
   token at or past exp despite Lugh's 60 s skew (`lir/auth/tokens.h`); `lugh_client` links Dannan, OpenSSL and Boost; Lir pins a
   commit on Lugh's develop by FetchContent with `LUGH_BUILD_NODE=OFF`
   until Lugh releases. Its Dannan must be the one Lir pins: Lugh is on
   Dannan 0.7.0 (develop `7b0c717`), uses Lir's `dannan::dannan` when
   fetched into Lir's build (one Dannan a process), and moves with Lir
   when Lir moves past 0.7 (tell the Lugh session first). Boost.JSON
   (the maintainer, 2026-11-14): `lugh_client` compiles `boost/json/src.hpp`
   itself, and so does Lir, for the rules engine
   (`lir/rules/boost_json.cpp`, listed at `lir/CMakeLists.txt:25`). With
   the plugin on, Lir leaves that file out and takes the definitions from
   `lugh::client`; with it off, Lir compiles its own. A test of both
   configurations' link comes with the plugin. Lugh agreed: the
   definitions live in the exported `lugh_client` library, its Boost.JSON
   macros are PUBLIC on `lugh::client`, and its P10.6 tests a consumer
   that links it and uses Boost.JSON.

**The standing cluster** on the three build nodes (`deploy/test-cluster/`): one
`lir-node` each, ports 7537 (Dannan), 1983 (MQTT) and 8183 (HTTP), a
systemd unit as lir, the cluster key in `build/test-cluster/cluster.key` on
the workstation and never in the repository. It carries the claim board. Upgrade it
with `deploy/test-cluster/install.sh root@<node>:/root/lir-work/build
node-a=192.0.2.1 node-b=192.0.2.2
node-c=192.0.2.3` after a Lir change worth trying on real
machines; the restart is rolling and the board survives it.

The standing cluster runs `9fced0a` (Dannan 0.7.0) since 2026-11-14
03:52Z, upgraded at the maintainer's word from the gate run's build; each node's
`$lir/nodes` reads majority, 3 of 3, and the board's claims survived.

## Handoff, 2026-11-15 (end of session)

The session's work, each recorded above with its gate: Lir's listeners on
`dannan::listen` (337); Dannan 0.8.1, 0.8.2 and 0.8.3 repins (337, 337,
351); `lir::client` and the codec as `lir::mqtt` (351), on Dannan's
`StreamConnector` (351), finding Lir's nodes in Dannan's roster (352 on
node-d); the repository URL fix, CodeQL's permission, the repository made
public, and the first all-green hosted CI; a Dannan TSan finding
suppressed until 0.9.0; node-d joined the standing cluster as its fourth
node. Reviews given: Dannan's no-delay patch, stream connector (design and
code) and hybrid logical clock; Mimir's and Kvasir's designs; Louhi's,
Lugh's and Mimir's use of `lir::client`. Corrected: the Dagda log rate
given to Kvasir (8,000 a second was measured by Dagda's slice 0, not only
computed). Open: the cluster upgrade of node-a, node-b and node-c, first
next session (`ACTIVE.md`); Louhi's reboot of node-a is also deferred to
its next session and will not overlap. No claims held. `develop` pushed.

## 2026-11-15: the standing cluster on 6c3a78c

All four nodes run `6c3a78c` (Dannan 0.8.3) since 22:33Z, the same binary
on each (md5 `9ed89541c601`), installed from node-d's release build with
`LIR_ONLY` one node at a time: node-b, node-c, then node-a. Louhi and the
the coordinating session were told before and after. After each
restart: `lir-node` and `claim-board-scheduler` active everywhere,
node-a's scheduler acting and the others standing by, `$lir/cluster` 4
nodes, each node's `$lir/nodes` partition majority 4 of 4, and retained
`stack/#` identical on all four. The untracked `build/cluster-check.py`
now fetches the four retained sets in parallel and retries, since a board
write landing between sequential samples read as a false divergence, and
it finds each node's `$lir/nodes` entry by the id in
`state/node-id`, which fills the partition column. No build or test run;
no cores claimed.

## 2026-11-15: the fuzz targets, merged

`feature/fuzz-targets` merged into `develop` after `develop` was merged
into it: five libFuzzer targets (`mqtt_packet`, `sn_packet`,
`http_target`, `cluster_codec`, `cluster_stored`), the whole build
instrumented with ASan and UBSan under `-DLIR_BUILD_FUZZING=ON` (clang
only). On node-b (clang 19 and `libclang-rt-19-dev` installed
there; node-c and node-a already had them): `CXX=clang++`,
RelWithDebInfo, `-DLIR_BUILD_TESTING=OFF`, `-j2`; each target once for
`-max_total_time=60`, between 520,000 and 730,000 runs each.

One finding, fixed (`7e57fcf`): `sn_packet` found `03 1a 6d`, a
WILLTOPICUPD with a flags byte and no topic. It decoded, but encoded back
as the two-octet form that deletes the will, so the packet changed on the
round trip. MQTT-SN 1.2 (5.4.7, 5.4.23) has only that form or flags with a
topic name, so `lir/sn/packet.cpp` now refuses flags without a topic, for
WILLTOPIC too; test `SnPacket.AWillTopicIsEmptyOrFlagsAndATopic`. The
crash input then passed and a second 60 s run was clean. The other four
targets found nothing.

Gate, node-b: `tools/ci.sh release` 353 passed (352 and the new
test), warnings as errors.

## 2026-11-15: the cluster's join window

Hosted CI was not green on every job since `91cd389`, as `ACTIVE.md`
said. Three intermittent failures:
`RetainedValuesAreEverywhereAndClearedEverywhere` (tsan, run 43757235866)
and `ASharedGroupAcrossNodesGetsEachMessageOnce` (release arm64, run
43765626713, its second failure; the first was on asan), and a TSan
heap-use-after-free in Dannan (below).

The first two were Lir's own (`698fb96`). `Cluster::start` advertised `lir`
before subscribing to `lir.retained` and `lir.groups`. A peer takes a node's
snapshot when it first sees the node serving Lir, and Dannan sends a stream
publish only to the nodes whose interest advert it already sees
(`dannan/net/fanout.h:29`). So a retained value or a group published while
the record carried `lir` but not that interest never reached the node:
retained values until the node rejoined, groups until the 30 s resync.
This was a real loss on any join, not only in the tests. Now `lir` is
advertised last, after the strand has set `running` and subscribed (a
subscription made before `running` would drop what arrives). Test
`ClusterTest.ANodeIsSeenServingLirOnlyWithItsChannelsSubscribed` watches
a's roster while d joins. It failed 4 of 30 runs before the fix and passed
100 of 100 after; `ClusterTest.*` passed 22 of 22 ten times.

The third is Dannan's, reported to the Dannan session with the stacks:
`~NodeRecord` under the dial's `ClientHandshake` (`session.cpp:462`) and
under `Node::observe_direct`'s `before` vector (`node.cpp:1036`), freeing
one map node from two threads, in
`ConnectingOnAnotherNodeTakesTheClientOver` (tsan, run 43773090610).
Dannan has ruled out its locking and is reproducing it under TSan. It is
not suppressed in Lir. The build's flags match across both libraries
(asked by Dannan, answered).

Gate, node-b: `lir_tests` release 354 passed.

The hunt for Dannan (2026-11-16): node-c, g++ 14 TSan with `tools/ci.sh`'s
flags, Lir at `ec68ff7` (the CI run's commit, without the join fix). Results:
`ConnectingOnAnotherNodeTakesTheClientOver` alone 40 of 40 clean,
`ClusterTest.*` 9 of 10 clean, the full suite 3 of 3 clean. The one failure
was the join window (`RetainedValuesAreEverywhereAndClearedEverywhere`),
which reproduces that bug on a build node. That is 0 TSan reports in 53 processes.
Dannan's own reproduction, `Node.NodesJoinWhileAdvertsChangeAndNothingIsShared`,
was also clean. It stays open as unexplained, not suppressed. Lir's audit:
it keeps no NodeRecord across threads. It does pass `routes().call` a
short-lived record (`cluster.cpp:361`, `446`). Dannan confirmed it is
copied before `call` returns (`place_call` takes `std::optional<NodeRecord>`
by value, `dannan/node.cpp:211`), so it is safe as written. The report is
open in Dannan's plan as unexplained. If it recurs, send Dannan the new
frames.

## 2026-11-16: louhi/# on the standing cluster, and Dannan 0.8.4

`louhi/#` opened to anonymous clients (publish, retain, subscribe) on every
node, kept open (the maintainer). It went out with the new `LIR_CONFIG_ONLY=1` mode of
`install.sh`, which installs only `node.yaml` and restarts: node-b, node-c
and node-a at 01:29-01:32Z, and node-d at 02:55Z once it was back from a
power cycle. The check passed after each node. Each node granted
`louhi/#`, delivered on it, and refused other trees (0x80). Louhi's first
live test node joined at 01:34Z, giving majority 4 of 5, and left at 01:35Z.
Its retained topic was cleared. A departed member stays in the count until
Dannan's horizon, so the reading stayed at 5 members afterwards.

Repinned Dannan `0.8.3` to `0.8.4` (ABI kept: StartTLS without an answer,
a stop token on `wait_for_change`, and a deferred Answer giving back its
place before replying). Gate, node-d `-j1`: `tools/ci.sh release`
354 passed, `tools/ci.sh client` clean.

Repinned `0.8.4` to `0.8.5` (ABI kept: `dannan::listen::Backoff`, and a fix
for a node destroyed after its io stopped hanging in its second stop, which
Louhi's test node hit). Gate, node-a: `tools/ci.sh release` 354
passed, `tools/ci.sh client` clean. `mosquitto-clients` installed on node-a
for the process tests (CMake finds them at configure time).

## 2026-11-16: the SUBACK hold, on a branch until Dannan 0.9.0

`feature/suback-hold` (`0147437`, local) pins Dannan's develop (`e78b66c`)
for its advert push. Each new interest prefix is subscribed with
`Channels::subscribe` with a deadline (`subscribe_deadline`, 1 s), and
`Link::on_subscribe` holds the SUBACK and its retained messages on the new
`ClusterHooks::interest_held`. On a miss the SUBACK goes anyway, counted in
`subscribe_missed` (`$lir/nodes`) and logged. The "lir" advert is pushed too.
Test `SlowGossipClusterTest.AMessagePublishedElsewhereRightAfterSubackArrives`
uses a 30 s heartbeat. Before the change, the behaviour it tests failed on
the standing cluster: 0 of 20 delivered at 0 ms (`build/suback-probe.py`).
Gate, node-a: 355 passed, the new test and the join-window test 20
of 20. Merge at Dannan 0.9.0: repin the tag on the branch, gate, merge, and
remove the README's stated limit.

## 2026-11-17: Dannan 0.9.0, and the SUBACK hold merged

Dannan tagged 0.9.0 (`59b7abc`), with the advert push (`e78b66c`) and the
ledger Lease fix (`79dfa2e`). On `feature/suback-hold`: merged `develop` (one
conflict, the pin line), pinned `0.9.0`, removed the TSan suppression of
`RetainedMemoryLedger::release`, and replaced the README's stated limit on
the window after SUBACK with the behaviour: SUBACK held until every node
holds the subscription, 1 s deadline, a miss counted in `subscribe_missed`.
No Lir call site reads `CallOutcome::peer`, whose type changed in 0.9.0.
Gate, node-a `-j2`: `tools/ci.sh release` 355 passed, none skipped.
Merged into `develop`. The standing cluster still runs `6c3a78c` (0.8.x)
until it is upgraded.

## 2026-11-17: HTTP waits for the held SUBACK

Found when upgrading the standing cluster: node-d, the first node on the new
build, answered 400 ("the subscription was refused") to every GET /retained
from about 20:35 to 20:42Z. `cluster-check.py` showed it unreachable, and
node-d's standby claim-board scheduler logged one failed round. node-d was
rolled back to the old build (node-a's installed bin and lib) and the
upgrade stopped. Cause: `/retained` and `/subscribe` in `lir/http/http.cpp`
read the SUBACK inside the SUBSCRIBE, and a cluster node now sends it later.
Both now wait for it, bounded by `read_timeout` (503); the wait holds the
exchange. Tests: `HttpHeldSubackTest` (four; every SUBACK held 100 ms and
one peer reported missed, as beside a 0.8.5 node) and
`ClusterTest.HttpRetainedAnswersOnAClusterNode`. On the old code four of
the five failed (the refused-filter one passed). Gate, node-b `-j2`:
360 passed, none skipped; the HTTP tests 10 repeats clean (new
asynchronous code). The SN gateway already took a later SUBACK. The board's
plugin treats a 4xx as final and does not try the next node; the coordinating session is taking that to the maintainer.

## 2026-11-17: the standing cluster on Dannan 0.9.0

A rolling upgrade, with the maintainer's approval and after Louhi and the coordinating session were told: node-d, node-c, node-b, then
node-a, from node-b's build of `a78b2cd` (the same binary on all four).
After each node: `GET /retained` on it answered 200 in 24 to 32 ms, and
`cluster-check.py` showed 4 nodes, majority 4/5, retained identical. The
SUBACK probe went from 0/20 at 0 ms before 0.9.0 to 20/20. The probe also
lost 1 of 20 at 50 ms and 1 of 20 at 250 ms, with misses logged for every
peer; ACTIVE.md, Watch.

## 2026-11-17: lir-node --retire, and the stale member retired

Every node counted a fifth member in Dannan's agreed roster, 8e23d6f7, so
the majority read 4 of 5. It was the louhi-node Louhi ran from node-c at
01:34Z on 2026-11-16: node-a, node-b and node-c rewrote their roster at
01:33:51 to 01:33:57Z that night. Louhi confirmed it is gone for good.
The maintainer chose a retire command. `lir-node --retire NODE_ID` opens a stopped
node's state, calls `Nodes::retire`, prints the outcome and stops; the
state lock refuses it while the node runs (`tools/lir_node.cpp`). Test:
`ProcessRetire.ARetiredNodeLeavesTheAgreedRosterForGood`. Also fixed the
test client to keep a PUBLISH that comes before its SUBACK
(`ClusterTest.ClientEventsReachEveryNode` failed on hosted TSan, run
44174274779, after the hold made that order real). Gate, node-b
`-j2`: 361 passed, none skipped; the new process test and the client-events
test 5 repeats clean. On the standing cluster, one node at a time: install
`cb13e37`, stop, retire, start, check. All four read majority 4/4; the same
binary everywhere (md5 `0dfe87a09dec`); `/retained` 200 in 23 to 31 ms on
each.

## 2026-11-17: Dannan 0.9.1

Repinned `0.9.0` to `0.9.1` (`7206e53`, ABI 0.9.0's), which fixes the
advert stamp tie. Gate, node-b `-j2`: `tools/ci.sh release` 361
passed, none skipped. `SlowGossipClusterTest` still timed out in its fixture:
17 of 20 in the gate, then 6 of 30 and 6 of 20 on a loaded node-b. A
diagnostic copy showed the same thing in every failure: the first node holds
the second's record, active, without its "lir" advert, and the second logged
no peer missed. The likely cause is a peer joining while the start-up push
goes out, so it gets a copy from before the advert and nothing newer until a
heartbeat. Sent to Dannan with two questions. 0.9.0 timed out there too, so
the repin goes ahead. The standing cluster still runs `cb13e37` on 0.9.0
until it is upgraded.

## 2026-11-17: the standing cluster on Dannan 0.9.1

Rolling, with the maintainer's approval, from node-b's gate build of `1cb5efd`:
node-d, node-c, node-b, then node-a. After each node, `/retained` answered
200 and the cluster was whole, majority 4/4 (md5 `c5dc83632a0d` on all
four). Each node's start-up minority warning is now followed in the same
second by "on the majority side again". At about 22:36Z node-d dropped off
the network (no ping, no ssh), 3 minutes after its upgrade; the other three
held the majority, 3/4. The SUBACK probe across the three live nodes
delivered 20/20 at every delay from 0 to 2000 ms, with no misses counted.
On 0.9.0 it had lost 1/20 at 50 ms and 1/20 at 250 ms.

## 2026-11-17: Dannan 0.9.2

Repinned `0.9.1` to `0.9.2` (`36d6ded`, ABI 0.9.0's). A node now pushes its
current record to each peer at first direct contact, which closes the gap
where a joining peer missed the "lir" advert until a heartbeat. Gate,
node-b `-j2`: `tools/ci.sh release` 361 passed, none skipped;
`SlowGossipClusterTest` 20 of 20 (6 of 30 failed on 0.9.1). node-d came back
by 23:20Z and rejoined on its own: majority 4/4. The standing cluster still
runs `1cb5efd` on 0.9.1 until it is upgraded.

## 2026-11-17: the standing cluster on Dannan 0.9.2

Rolling, with the maintainer's approval, from node-b's gate build of `4762186`:
node-d (back since about 23:20Z), node-c, node-b, then node-a. After each
node: `/retained` answered 200 in under 40 ms, and the cluster stayed whole
at majority 4/4. md5 `46afb1bfdf26` on all four. Hosted CI on `4762186`
was fully green (run 44200063109). The SUBACK probe on all four nodes,
23:50 to 23:51Z: 20/20 delivered at every delay from 0 to 2000 ms, control
20/20, and `subscribe_missed` 0 on every node before and after. The window
after SUBACK is closed on the standing cluster.

## 2026-11-18 to 19: the hold, the wipe, and the clock ceiling write

- The maintainer gave Miach's work priority on the build nodes for the evening of 2026-11-18.
  Lir held no claims and started nothing there.
- The maintainer's wipe of the build nodes to a bare OS (2026-11-19, 16:05Z), done by the coordinating session. Lir told it what a node needs outside
  `/opt/lir-cluster`, now in `deploy/test-cluster/README.md`. Every node's
  directory and `state/` were kept. Afterwards Lir's check found:
  - `/retained` answered 200 on all four in about 25 ms;
  - the same ids on all four, and node-a acting;
  - majority 4/4 on every node;
  - `stack/#` identical everywhere (6 messages, digest `a9ccb090d6d7`).
  `/root/lir-work` is gone on every build node.
- The cluster session measured each node rewriting `state/clock/hlc-ceiling`
  once a second, with two fsyncs; the maintainer called it wasteful. The write is
  Dannan's, so it went to the Dannan session (the maintainer: "Dannan's code is
  Dannan's code"). The maintainer decided the fix for Dannan 0.9.3: the ceiling follows
  the stamps issued, not the wall clock. ACTIVE.md, Next, item 3.

## Archived from ACTIVE.md, 2026-11-19

**The ledger watch, in full.** The Dannan TSan finding was two `~NodeRecord`
freeing one map node. It was first seen in
`ConnectingOnAnotherNodeTakesTheClientOver` (run 43773090610) and recurred in
`ARuleWrittenOnOneNodeAppliesOnEvery` (run 43802763961), on one thread:
`SystemTopics::on_roster` takes `nodes().snapshot()` and publishes, and
`Fanout::destinations`' roster copies free a node the snapshot's records
still point at. Dannan's explanation (2026-11-16) is the ledger defect:
`RetainedMemoryLedger::release` on a destroyed ledger frees memory it no
longer owns and corrupts the heap. That explanation comes from reading the
code; nobody reproduced the link. On node-c, Lir `bc5018a` on Dannan 0.8.5:
- ASan: `HeldMessagesAreLeasedInTheNodesLedger` failed in 1 of 30 runs of
  `ClusterTest.*` with a use-after-free in `release`, from a
  `Fanout::ask_replay` completion after `~LirNode`
  (`build/ci-logs/node-c-asan-ledger-uaf.log`).
- TSan with `print_suppressions`: none of 30 runs reported anything.
Dannan's fix `79dfa2e` is in 0.9.0, and the suppression was removed then.

**The SUBACK watch, in full.**
- On 0.9.0, with all four nodes upgraded, peers were still reported missed
  ("the peer holds an older record of this node"). The probe
  (`build/suback-probe.py`, 2026-11-17 20:58Z) delivered 20/20 at 0 ms, but
  19/20 at 50 ms and at 250 ms.
- Dannan's cause: an advert change made in the same millisecond as a record
  snapshot got the snapshot's stamp, and peers that held the snapshot refused
  it. The claim board's frequent `/retained` reads made those ties common.
- The same tie timed out `SlowGossipClusterTest`'s fixture: 3 of 10 runs on a
  loaded node-b, and 3 of 4 hosted runs on 2026-11-17 (44177572057 tsan,
  44178256902 asan arm64, 44178722443 release arm64), none of them a
  sanitizer report.
- 0.9.1 fixed the tie: the probe delivered 20/20 at every delay with no
  misses. A second gap remained. A peer that connected while a node's
  start-up push was going out kept the older record, without "lir", until a
  heartbeat; the fixture failed 6 of 30 on node-b.
- 0.9.2 closed it, by pushing the current record to a peer at first direct
  contact. The fixture passed 20 of 20, and the probe 20/20 on all four nodes.

**The SUBACK decision, as `ACTIVE.md` carried it until the docs were
rationalised.** It is carried out; `ACTIVE.md` keeps what still binds.

- (2026-11-16, the maintainer, asked in turn) The window after SUBACK: a subscription
  reaches other nodes at Dannan's heartbeat pace (5 s), so a message
  published elsewhere in that time was lost for the new subscriber
  (measured on the standing cluster: 0 of 20 at 0 ms, 8 of 8 at 5 s).
  Dannan adds `Nodes::advertise` with a deadline and evidence (held,
  missed), and `Fanout::subscribe` completing when the interest is held,
  in 0.9, with no 0.8.x stopgap. Lir holds SUBACK on it. On a miss, SUBACK
  anyway, with the missed peers counted and logged. A stated limit in the
  README until then.

**Dagda's slice 0 verdict, in full** (Dagda, 2026-11-14, on node-a).
The log passes the PUBACK mark on a real device sync. Three processes,
k = 2, 8,000 a second from outside the replica set: p99 12.5 ms at a 10 ms
tick, 23.5 ms at 20 ms, the same with 256 logs on one shared segment,
nothing refused; p99 is about the tick plus 2.5 ms, so 50 ms holds to a
tick of about 40 ms. A segment per log fails; the shared segment stands. A
new lease holder reads a 5 s backlog (13,462 entries) in about 11 ms. Not
yet measured: the notify channel's cost per space. Dagda's record: its
`TODO/2026-11-13-three-systems-design.md`, "What slice 0 has measured (the
build node)".

## 2026-11-19: the docs and TODO rationalised

No code changed. No build or test run, and no cores claimed: the gate
stands at 361 (`4762186`, 2026-11-17). Every claim changed below was
checked against the code, the build files, the CI workflow or the standing
cluster first.

- `README.md`: it says Lir will persist in Dagda, where it said it does;
  the Lugh plugin is listed as built, where it was "waiting to be wired
  in"; Dannan is fetched from `../dannan` or from the repository
  `-DLIR_DANNAN_REPOSITORY` names; `lir-node --retire`, the `/retained`
  example, `lir::client` finding nodes in Dannan's roster and `tools/ci.sh`
  are there; the layout lists `lir/client/`, `lir/include/` and `deploy/`.
- `CONTRIBUTING.md`: rewritten where it was stale. It said there was no
  hosted CI, that everything runs on the laptop, that Lir never links Lugh
  and that Lir builds against sibling checkouts of Dannan and Dagda. It now
  gives `tools/ci.sh` and its steps, the hosted jobs, the build nodes-only rule,
  the fuzz targets and the standing cluster.
- `CLAUDE.md`: the header block's tagline is "Lir - The Message Broker", as
  The maintainer decided on 2026-11-04 and as every source file and `tools/ci.sh
  check` have it (it read "The Distributed Event System"); Lir commits and
  pushes `develop` on its own (the maintainer, 2026-11-15; it read "only when asked");
  builds on the build nodes only; the `client` and `contract` namespaces.
- `deploy/test-cluster/README.md`: the scheduler runs on all four nodes (read
  from `stack/scheduler/#`); the upgrade's rule, command and order moved
  here from `ACTIVE.md`'s words.
- `TODO/2026-11-03-lir-plan.md`: its text unchanged and its lines unmoved;
  "Later decisions that differ" added at the end, twelve of them.
- `TODO/2026-11-04-lir-sections.md`: a note at each answer since carried
  out or decided.
- `TODO/2026-11-13-lir-needs.md`: what became of each need, at its head;
  its "plan N" citations past line 93 were nine lines short since the
  plan's "Superseded" note went in, and are corrected.
- `TODO/2026-11-13-step5-design.md`: where it stands, at its head (it still
  read "a draft, nothing built"); its file:line references corrected
  (`cluster.h:16-38`, `broker.h:57-61` and `111-120`, plan 112).
- `TODO/ACTIVE.md`: the `client` gate's last run corrected (the 0.8.5
  repin, not `6c3a78c`); the SUBACK decision of 2026-11-16 cut to what
  still binds, with its first wording kept here; Dagda's slice 0 figures
  moved here.
- This file: "How to read it" at its head.
- Not checked: the two references into Dagda's code in the step 5 design
  (`dagda/store/object_id.h:21`, `dagda/context.h:246-258`), which Dagda
  confirmed on 2026-11-16, and the line numbers the needs file cites in
  Dannan, Dagda and the stack document as they were on 2026-11-13.
- `docs/principles-and-laws.md` is unchanged: identical to Dannan's copy,
  and to Macha's but for the source note.

## 2026-11-19, later: a design withdrawn, and the premise written down

No code changed, nothing was built or run, and no cores were claimed. The
gate stands at 361 (`4762186`).

- Checked upstream: Dannan has no 0.9.3; Lugh's tag `0.1.0` is from
  2026-11-04, before `lugh_client`, and is not the release the plugin waits
  for; Dependabot's moves of two CI actions to v7 were not taken, as Dannan
  is on v4 and v5.
- Lir wrote a guarded-publish design for the claim board on Dannan 0.9.2's
  agreement (`003906a`): a whole tree in one register, acceptor state in
  Lir's memory, a fixed set of four nodes moved by an operator, and perhaps
  a restart of all four nodes to introduce it. It put four questions to
  The maintainer.
- The maintainer's answers: wait for multi-name commits; wait for Dagda; "Leaderless
  does not mean 'group of immovable leaders'."; "You need to replan." Then:
  "The questions you're asking me mean you haven't understood what Lir is
  supposed to do."
- The design was withdrawn (`53afd98`) and the ruling recorded under
  Decisions.
- The maintainer then asked for a plan "to keep what is good, and rewrite the rest",
  warning that "the temptation will be to keep code which was written with
  a fundamentally flawed premise", and then to write it up, rewrite the
  TODO and docs, and clear what does not support the goal.
- Written: `TODO/2026-11-19-premise-and-audit.md` (the goal, five
  statements to check Lir against, three bins for the code, what is
  already suspected, seven guards against keeping what should go, and the
  order: ask the maintainer, audit, a second reader, findings to the maintainer, then code).
- `ACTIVE.md` rewritten around it. Removed from it: the six-step plan with
  its "done" marks, the old "Next" list, and the guarded-publish sketch.
  Every decision is kept; those of 2026-11-04 that set up the interim are
  marked as under the question.
- Removed from `TODO/`: `2026-11-13-lir-needs.md` and the withdrawn design.
- `README.md` states which parts of the cluster are interim and under
  audit. `CONTRIBUTING.md` and `CLAUDE.md` carry the premise.

**Handoff.** The next session starts at `ACTIVE.md`, "Resume here": one
question to the maintainer, then the audit. Nothing is built before the audit's
findings have been to him. The standing cluster is untouched and runs
`4762186`. No claims held. `develop` pushed.

## 2026-11-20: the plan rewritten: built on Dannan

No code changed; nothing built or run. The gate stands at 361.

- The maintainer: Dannan has no special nodes, so Lir has none; and Lir is "built on
  Dannan", not merely using it. His only real question: what does full MQTT
  require that Dannan does not already provide, and how is that built on
  top of Dannan?
- `TODO/2026-11-19-premise-and-audit.md` rewritten around that question
  (the first version is at `d005e79`): Lir's first answer as a table, not
  yet checked against the contracts; three things not known (one
  connection per client id, the subscriptions of an offline session,
  endings by time with nobody connected); and the work in order.
- Found when the maintainer asked how much of MQTT Lir supports now: it is not
  measured. No source or test names a conformance statement and no list
  maps statements to tests, though the project's rule defines "supports
  MQTT" by exactly that. The README no longer says "each tested".
- `ACTIVE.md`, `CLAUDE.md` and the README brought into line.

**Handoff.** Start at `ACTIVE.md`, "Resume here": read the contracts,
write the conformance gate, map needs to nouns, a second reader, then
The maintainer. Nothing is built or deleted before that. The standing cluster is
untouched. No claims held. `develop` pushed.

## 2026-11-20: step 1, the contracts read

No code changed; nothing built or run. The gate stands at 361. basemind's
MCP server was down; its CLI served.

- Read whole: Dannan `0.9.2`'s `Context`, `Agreement`, `Fanout` and
  placement (the `../dannan` checkout is another session's branch older
  than the tag, so the tag was read through git); Dagda's `Context` at
  `0.3.2`; Dagda's three-systems design for what it has designed for Lir.
- The plan's table corrected (`533bb25`). Shared subscriptions: Dannan's
  placement chooses among nodes, not a group's members, so the publisher
  picks a node with interest by rendezvous and that node picks a member.
  Order per publisher: `notify` carries no sequence; `stream` does, to its
  replay buffer's bound. Wildcards: an interest advert past 1 KiB reads as
  everything. Wills: a split fires the wills of clients still connected
  on the other side, a question for the maintainer.
- The unknowns sharpened. One connection per client id: agreement refuses
  a minority side; a channel per client id overflows the interest advert.
  Offline subscriptions: a candidate (the publisher matches and appends
  under its own dot), unchecked with Dagda. Endings by time: a queued
  message's expiry is the log entry's; a session's ends with no notice,
  and nothing fires its will.
- Nothing Lir waits on from Dagda (D1, D2, D5, D6, D9) is in its contract;
  slices 2 to 4 are paused until Macha's cutover. Lir does not link Dagda
  at all today, though `CLAUDE.md` says it does.

**Handoff.** Start at `ACTIVE.md`, "Resume here", step 2: the conformance
gate. It needs the MQTT 5.0 and 3.1.1 specifications' conformance
statements and a pass over `test/`. The standing cluster is untouched. No
claims held. `develop` pushed.

## 2026-11-20: steps 2 to 4, and the rewrite begins

- Step 2, the conformance gate (`529a7ce`): `docs/conformance.md`, 252
  statements of 5.0 and 139 of 3.1.1 taken from the OASIS texts, each read
  against the tests by four agents in parallel; every test they named was
  checked to exist. 5.0: 84 tested, 84 partial, 82 none, 2 n/a. 3.1.1: 51,
  64, 22, 2. Confirmed in the code: resends walk a map keyed by packet id,
  so order breaks once ids wrap; a Subscription Identifier on a client's
  PUBLISH is accepted; three MQTT 3.1 deviations.
- Step 3, the mapping (`c1d2cda`): the 108 statements that span nodes in
  twelve needs, each with its noun. Unknowns sent to Dannan and Dagda.
  Dannan: client-id uniqueness is Dagda's store lease (two holders in a
  split, the older connection ended at heal); acting at a time with nobody
  connected has no noun, put to the maintainer; shared groups exact by one advert per
  group up to the 64-advert bound, the rest put to the maintainer. Dagda: its queue is
  open again (the maintainer ruled Macha's cutover is not its concern) and it will
  answer the offline queue and the timed action.
- Step 4, the second reader (`e809114`): `TODO/2026-11-20-second-reader.md`;
  two corrections taken (shared choice past the advert bound; order when a
  publisher moves nodes).
- Step 5: The maintainer rejected a question that split the work into "single-node
  gaps" and "cross-node parts": "If you're asking about single-node
  specific code, you've missed the whole point, Again." Then ruled the
  rewrite from scratch, keeping the contract.
- On `feature/rewrite` (local): `docs/contract.md` and the headers verbatim
  under `docs/contract-v1/`; `lir/`, `tools/lir_node.cpp` and the old tests
  removed (85 files); `CMakeLists.txt`, `lir/CMakeLists.txt`,
  `lir/test/CMakeLists.txt` written for the skeleton; `tools/ci.sh` and the
  CI workflow without the Lugh step; the README rewritten. The gate count
  falls from 361 to 2 by the maintainer's ruling.

**Handoff.** `feature/rewrite`, local. Next: the design of the new Lir, to
The maintainer before code. The standing cluster is untouched and runs `4762186`.

## 2026-11-20: the rewrite starts, and its MQTT codec

The maintainer: "realistically you'll have to go for a rewrite from scratch. Store
Lir's existing contract, because it's used by other systems, and start from
scratch." Built on `feature/rewrite` and squashed onto `develop`.

- **The contract,** stored: `docs/contract.md`, with the headers verbatim
  under `docs/contract-v1/`. It now also holds lugh_client's use of the
  topic grammar and the claim board's three HTTP endpoints.
- **The old code removed:** `lir/`, `tools/lir_node.cpp` and the old tests,
  85 files; it is in the history at `e809114`. The gate count fell from 361
  to the skeleton's 2 by the maintainer's ruling.
- **The codec, `lir::mqtt`,** written test first, each test named by the
  statement it proves and seen failing against stubs before the code:
  - data representations;
  - topics and shared filters;
  - properties;
  - every control packet of 5.0, 3.1.1 and 3.1.
- **Tested against a real MQTT client** (the maintainer: "You will test with a real
  MQTT client"). `mosquitto_pub` and `mosquitto_sub` at 5.0, 3.1.1 and 3.1
  talk to a socket the test serves with Lir's codec: a QoS 1 publish with a
  will, credentials and properties; a subscribe receiving QoS 2 from Lir;
  and a refused connect.
- **The conformance gate,** rebased on the rewrite and generated from test
  names (`tools/conformance/`). 3.1.1 has 141 statements, not 139: two
  misprinted identifiers were recovered. Proven so far: 66 of 5.0's 252,
  48 of 3.1.1's 141, and 3 of 3.1's differences.
- **The gate on node-d:**
  - check, release, client, clang, ASan/UBSan and TSan all passed: 113
    tests, no sanitizer report;
  - coverage 99.9% of lines and 100% of functions;
  - `lir::mqtt` builds alone with `LIR_BUILD_SERVER=OFF`.
- **Settled with the stack** through the coordinating session:
  - The maintainer's Q1 to Q4;
  - Dagda's answers on D6 and the offline queue;
  - Dannan's items 8, 17, 24, 25 and 31;
  - the topic grammar staying in `lir::mqtt`, which `lugh_client` will
    link;
  - the shared admin UI in Lugh;
  - every dependant's requirements: `TODO/2026-11-20-requirements.md`.
- **The design of the rest:** `TODO/2026-11-20-rewrite-design.md`.

**Handoff.** Next, step 2 of the design: the connection, authentication
and delivery across nodes, on Dannan 0.10.0's `dannan::testing` once it is
tagged, tested on the simulated cluster and against real clients. Owed
before 0.1.0: Debian packaging and libFuzzer targets for the codec. Tell
Lugh the develop commit to pin. Set `fail_ci_if_error` once a Codecov upload
is seen to succeed. The standing cluster is untouched and runs `4762186`.

**After the merge (2026-11-20).** Hosted CI on `develop` is fully green at
`ab582a5` (run 44672757084): eleven jobs, macOS among them. Getting there
took two fixes to the real-client test, each found by CI's macOS job:
- the test server now closes after a client's DISCONNECT, as a broker
  does;
- Homebrew's `mosquitto_pub` ends with a plain close rather than a
  DISCONNECT, which the test now allows.

Codecov receives coverage with the token the maintainer set, and a failed upload now
fails CI. Lugh pins `d36356d` for `lir::mqtt`.

## 2026-11-20: the codec's fuzz targets

Three libFuzzer targets in `lir/fuzz/`, one for each parser of what a
client sends. Each one checks the codec against itself, beyond surviving
the input:
- `packet`: every input is read as each of the three versions. Whatever
  decodes must encode, and the encoding must decode to the same packet.
  The fixed header must read the same from any prefix of the stream, and a
  maximum below the packet's size must refuse it as `packet_too_large`.
- `properties`: a block at each place. Whatever reads must write to the
  size `properties_size` gives, then read back the same.
- `topic`: `covers` must agree with `matches` for a name, and coverage must
  carry through to what the covered filter matches. A shared filter must
  rebuild from its parts, and the levels must rejoin to the topic.

The shape is Lugh's: plain `LLVMFuzzerTestOneInput` files, with no
harness of their own. Dannan's harness is not exported, so nothing is
duplicated. `LIR_BUILD_FUZZING` instruments everything with ASan and
UBSan, Dannan included, and only Lir's code for libFuzzer's coverage.
`lir/fuzz/seeds.py` writes the corpus by hand from the specification,
not from Lir's encoder. `tools/ci.sh fuzz` runs the three targets side
by side, `LIR_FUZZ_SECONDS` each; CI runs it for 60 seconds on a push.
The targets run as separate processes because libFuzzer's `-fork` driver
leaks memory of its own, which LeakSanitizer reported on the first run;
that report would have hidden a leak in Lir's code.

On node-a, 60 seconds each, clang 19, no finding:
- packet: 350,630 runs; the corpus grew from 26 seeds to 311 units in
  the first run, plus 1.18 million in fork mode;
- properties: 1.27 million;
- topic: 2.58 million.

GitHub's fuzz job failed at first: Debian trixie's `clang` package leaves
out `libclang-rt-19-dev`, which libFuzzer needs. `20bc0db` installs it, and
CI is fully green there (run 44691325265, twelve jobs).

## 2026-11-20: the Debian packages

`lir::mqtt` is now packaged to the stack's convention. The coordinator
settled that convention on 2026-11-20, and where Lir took Dannan's
practice over the message's wording, the coordinator accepted it.
- **Packages:** `liblir-mqtt0.1`, named for its SONAME
  `liblir_mqtt.so.0.1`, and `liblir-dev`, which holds the headers and the
  CMake config (`find_package(lir)` gives `lir::mqtt`). Each has debug
  symbols.
- **Build:** debhelper from `debian/` drives the CMake build, with
  `dh_makeshlibs -V`. The packages build only against the installed
  `libdannan-dev`, which `-DLIR_SYSTEM_DANNAN=ON` finds with
  `find_package`. The install rules exist only in that mode, so a project
  that fetches Lir with its own Dannan target never meets them.
- **Script:** `tools/build-deb.sh` is Dannan's in shape. Versions come
  from `git describe`; on develop the version is `0.1.0+git.g<hash>`. The
  script refuses a build when the tag and `project()` disagree, or when
  the library package does not match major.minor.
- **CI steps:**
  - `tools/ci.sh package` installs Dannan at its tag, then Lir into a
    prefix, and builds `test/package/consumer` from the prefix alone.
  - `tools/ci.sh deb` runs in a clean trixie. It builds Dannan's debs from
    the 0.9.2 tag with Dannan's own `tools/build-deb.sh` (nocheck) and
    installs them. Then it builds Lir's, with the tests inside the build,
    and runs `lintian --fail-on error,warning`.
  - CI runs `package` on x86-64, and `deb` on x86-64 and arm64.
- **Docs:** `docs/packaging.md`; the README says how to install.

On node-a (arm64):
- `package` passed: the consumer linked `lir::mqtt` from the prefix.
- `deb` passed, in a `debian:trixie` container. Docker there remaps root,
  so the container clones the tree into its own filesystem.
  `liblir-mqtt0.1_0.1.0+git.gd0b74f6`, `liblir-dev` and the dbgsym package
  were built; all 113 tests passed inside the build, the real mosquitto
  clients included; lintian reported nothing.

The coordinator has asked Dannan for one shared entry point that builds
and installs its debs from a tag. Until it exists, Lir's `deb` step does
that itself.

## 2026-11-20: use after move, checked

Lugh found an argument moved from inside the same call that read it. C++
leaves the order of evaluation open, and GCC on x86-64 read the argument
after the move. At the coordinator's request, clang-tidy's
`bugprone-use-after-move` ran over all 14 of Lir's sources on
node-a and found nothing. The check was first shown to catch
Lugh's shape, so the zero is real.

`tools/ci.sh tidy` configures with clang, builds nothing, and runs the
check with warnings as errors. CI runs it on every push.

All 15 CI jobs are green at `879feef` (run 44692880068), among them
`tidy`, `fuzz`, `package`, and `deb` on both architectures.

## 2026-11-20 to 22: release 0.1.0, the history scrubbed, Dannan 0.10.1, step 2 begun

- **The history scrubbed** (the maintainer, 2026-11-20). Every tracked file
  and commit message was rewritten with git filter-repo so that none names
  the development setup: no host names (node-a to node-d), no real
  addresses (RFC 5737 192.0.2.x), no user paths, "the maintainer" for his
  name outside copyright, no session or hardware names.
  The deploy folder became `deploy/test-cluster/`, with
  `LIR_DEPLOY_DIR` and `LIR_DEPLOY_USER`. develop and
  feature/dannan-0.5 were force-pushed; the two Dependabot branches were
  deleted. The pre-scrub bundle, the commit map and the rules are in the
  maintainer's backup archive outside the repository. Commit hashes cited
  in these docs were remapped (`e558b97`). A later check for a scrubbed
  literal split across a line break found none.
- **Release 0.1.0** (`9074dcb`, annotated tag object `d21297b`, on main),
  the codec alone; the README says the broker is not yet functional.
  Gate on the tag: check, release, clang, ASan and UBSan, TSan, coverage,
  fuzz, tidy, package, deb (lintian clean); the sanitizer builds shown by
  nm to instrument Dannan. CODE_OF_CONDUCT.md (Contributor Covenant 2.1)
  added and linked. GitHub description and topics set. The packages,
  built by hosted CI for amd64 and arm64 from the tag (run 44770925157,
  which `workflow_dispatch` with a tag input now allows), are staged in
  `../tomcully-apt/incoming/lir/0.1.0/` for the maintainer to publish.
  The README's install section now points at the apt repository
  (develop; main keeps the release's text until the next merge).
- **Dannan 0.10.0, then 0.10.1** (`885a266`, `4e3e270`), each read against
  its changelog first. Lir's own UTF-8 decoder became
  `dannan::encoding::valid_utf8`; the asan and tsan steps build Dannan
  through `DANNAN_SANITIZE` and check libdannan's runtime symbols (35
  `__asan_`, 36 `__tsan_`); the deb step uses Dannan's `install-debs`,
  passing its job count; debian/control bounds libdannan-dev to
  `>= 0.10.1, << 0.11~`. Gate on node-b each time: check, release 113 of
  113, ASan, TSan, deb lintian clean.
- **CI quieter** (`bf75a3a`): push on develop and main and pull requests
  only, no markdown-only runs, superseded runs cancelled, CodeQL on main
  and weekly.
- **Step 2, Delivery** (feature/step2, local, `63adf83` rebased on
  develop): `lir::broker::Delivery` on Dannan's notify channel, written
  test first. Gate on node-b on Dannan 0.9.2: release 121 of 121, the 7
  Delivery tests among them. Not yet rerun on 0.10.1. Then `93b9a9a`, the
  connection core's start (unbuilt): the contract's `Connection` and
  `Authenticator` restored under `lir/contract/`, the cluster fixture
  shared.
- **Owed:** Louhi's requirements for the new `lir::client` recorded
  (`eba9b3e`). Python audited for the coordinating session (three
  dev-only scripts, no build or runtime use; none rewritten).
- **Config naming** (the maintainer, 2026-11-20): the node follows
  Dannan's `docs/config.md`.

## 2026-11-22: Link, Dannan 0.10.2, the fuzz smoke

- **Branches:** every branch merged into develop deleted, local and
  remote (44 local, `origin/feature/dannan-0.5`); `feature/rewrite` and
  `feature/packaging`, squash-landed as `d36356d` and `f560b35`, deleted
  on the maintainer's word. Left: `main`, `develop`, `feature/step2`.
- **Step 2 on Dannan 0.10.1** (feature/step2, rebased on develop): the
  Delivery and contract commits built for the first time on 0.10.1:
  release 121 of 121 on node-d.
- **`lir::broker::Link`** (feature/step2): one client's protocol on
  Delivery, the authenticator and the node's time: CONNECT with enhanced
  authentication and re-authentication, the CONNACK limits, SUBSCRIBE
  answered once the cluster holds each filter, UNSUBSCRIBE, PUBLISH at
  QoS 0, PINGREQ, keep-alive, grant expiry, DISCONNECT. Its tests were
  written with the code, not seen failing first. First gate 173 of 173.
- **The maintainer's rulings** (ACTIVE.md, Decisions): every will refused
  until Dagda's leases; a 3.x retained PUBLISH closes; No Local honoured
  by carrying the publisher's client id ahead of the PUBLISH on the
  channel. Five tests of a published will went with the first ruling and
  two were added: release 170 of 170 on node-d, 50 of them `Link`.
- **Dannan 0.10.2** (`92ff63a`, develop): read against its changelog;
  nothing of Lir's affected. Release 113 of 113.
- **The fuzz smoke** (`0de12b3`, develop): the three targets register with
  `dannan::testing` and `lir_tests` smokes them on every gate. Release 115
  of 115; the clang libFuzzer build clean; each target 30 s on node-d,
  about 1.2 million runs, no finding.
- **Owed:** `feature/step2` rebased onto `0de12b3` and not built since; its
  next gate covers it. Coverage, ASan, TSan and the fuzz run before step
  2's merge. The transport waits on Dannan 0.11.0 (0.10.2 is the latest
  tag).

## 2026-11-22: lir::client

- **`lir::client`** (feature/client, squashed onto develop): written new to
  `docs/contract-v1/lir/client/client.h`, kept byte for byte, with the
  contract's `endpoints.h` restored. Built on Dannan's `StreamConnector`,
  `ClientTrust` and `Certificates`, `RetryPolicy::unbounded` (jitter, half
  to all, on top), and the roster's `lir` and `lir.endpoints` adverts.
  Timers wait on the injected time source. It honours the CONNACK's
  Maximum QoS, Retain Available, Maximum Packet Size and Receive Maximum.
- **Tests:** 35, against a broker each test scripts on Dannan's listener on
  the loopback, TLS included (a certificate made by openssl). Release 150
  of 150; ASan and TSan 150 of 150 with Dannan instrumented; coverage
  99.5% of lines (client.cpp: a closing brace and the packet-id wrap left);
  the client-only build and the package consumer build clean; five debs,
  `liblir-client0.1` new, lintian clean (installed on node-d for it).
- **Louhi** (through the coordinating session): it keeps its pin until D5
  and D6, since its retained statuses and retained QoS 1 will need them.
  Owed to it: this commit, and word when retain and wills are accepted.

## Archived from ACTIVE.md, 2026-11-22

What `ACTIVE.md` carried before this date's rationalisation and no longer
needs:
- "Do this, in this order", steps 1 to 6 (2026-11-19 and 20), all done:
  the contracts read, the conformance gate written (5.0: 252 statements;
  3.1.1: 139), each cross-node need mapped to its noun, a second reader
  (`2026-11-20-second-reader.md`), the findings taken to the maintainer,
  who ruled the rewrite from scratch; then the rewrite on
  `feature/rewrite`, squashed to develop.
- The converged placement plan (2026-11-20), kept by the coordinating
  session: Dannan's next minor exports the simulation harness as
  `dannan::testing` and bounds the per-peer answer that settles the SUBACK
  hold (L2); Dagda's D6 first, then D5, with `HlcClock::admit` for the
  retained stamp; Lugh's scope parsing in `lugh_client`. The maintainer's
  answers: HTTP and JSON move into Dannan on Boost.JSON; the register map
  orders by (hlc stamp, node id); Dannan's DNS responder gives plain
  clients an address that survives a node; retirement stays an operator
  command. Dagda's D6 review: retained needs no index of Lir's; D6 opens
  capable("lir") once D1 exists; a QoS 1 or 2 retained PUBACK waits on
  D2's replicated commit.
- Dannan 0.9.3 (the clock ceiling) was overtaken by 0.10.0.
- Dagda's slices 1 to 4 and the step 5 design
  (`2026-11-13-step5-design.md`), built on the interim's shape, are
  superseded by the rewrite's design.
- The watch on two Dannan defects believed fixed in 0.9.x (the ledger's
  TSan report, the join gap in `SlowGossipClusterTest`): neither was seen
  again; the code that showed them is gone with the rewrite.
- The HLC question for retained ordering: answered (L1).
