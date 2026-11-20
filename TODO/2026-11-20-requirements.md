# Lir's requirements, up and down the stack (2026-11-20)

Tom: "Every project that is a dependency should follow suite - confirm with
their dependants that the functionality they need is covered." This file
holds Lir's answers to its dependencies (Dannan, Dagda, Lugh) and Lir's
request to its own dependants. It is answered from the rewrite's design
(`2026-11-20-rewrite-design.md`), since the rewrite has only its codec so
far. Item numbers are the converged plan's
(`../lir-develop-cluster/TODO/2026-11-20-cluster-stack-placement.md`).

## Sizes Lir plans for

These are Lir's working figures, not Tom's; a target from him replaces
them:
- **Nodes:** 4 to 16.
- **Sessions:** 10,000 per node, a third offline at any time.
- **Filters:** about 5 per session (50,000 a node), a quarter with
  wildcards.
- **Shared groups:** a few dozen per node.
- **Load:** 10,000 messages a second cluster-wide, mostly under 1 KiB,
  inline up to 64 KiB (Tom, 2026-11-04).
- **Retained topics:** up to about 100,000.
- **PUBACK** at QoS 1 with k = 2: p99 under 50 ms over the whole path
  (2026-11-13).

## To Dannan

### A. Will use, at 0.9.2

- **`context.h`:**
  - `Context::version`.
  - `Nodes`: `watch`, `watch_advert`, `self`, `active`, `snapshot`,
    `placement(key, copies, scheme, only_advertising)`, both forms of
    `advertise`, `withdraw`, `partition`, `watch_partition`, `members`,
    `absent_since`.
  - `Routes`: `add`, `bind_deferred`, `call`, for the take-over call to a
    client id's lease holder.
  - `Channels`: `open` (`notify`, `stream`), `subscribe` in its pushed form
    for the SUBACK hold, `unsubscribe`, `publish`.
  - `Identity`: `id`, `durability_epoch`.
  - `Budget`: `declare`, `reserve`, `acquire`.
  - `Time`: `now`, `unix_ms`, `source`, `executor`, `after`.
  - `Telemetry::all`, for the system topics.
  - The rest of the context: `hlc()`, `status()`, `logger`, `events`,
    `readiness`, `wait_guard`, `threads`, `diagnostics`.
  - Not used: `agreement()` (client ids go on Dagda's lease).
- **`net/`:**
  - `fanout.h` (`Delivery`, `ChannelMessage`, `Published`).
  - `codec.h` (`Reader`, `Writer`: `lir::mqtt` is built on them).
- **`membership/node_record.h`:** adverts, for `lir` and `lir.endpoints`.
- **`placement/placement.h`:** `Scheme`.
- **`listen/`:**
  - the listener and `Stream`, for TCP and TLS;
  - `beast.h`, for WebSocket;
  - `connector.h` and `backoff.h`, for `lir::client`;
  - `limits.h`.
- **`config/config.h`:** `lir-node`'s settings.

### B. Needed, by item

**17, `dannan::testing`.** This blocks every test past the codec: the
rewrite's step 2 onward. There is no stand-in; those tests wait.

Lir needs four things from it:
- `SimNodes` worlds of 1 to 7 nodes, each node giving a `dannan::Context`
  that Lir's node is built on;
- a way to open a client connection to a chosen node inside the world (a
  sim `listen::Stream`, or an injectable accepted stream), so MQTT clients
  run under the seed;
- `Time::after` and the hosted system's timers on the world's schedule;
- Dagda's nodes in the same world (Dagda's D13), since Lir's node is
  built on both.

Proved when Lir's test runs a 5-node world under a seed: a client on node
1 publishes, a client on node 4 receives it, a partition is healed, and a
second seed replays exactly.

Would not work: a harness without a client-connection seam, since MQTT is
tested through its clients.

**8, the per-peer answer bound.** This settles the SUBACK hold (L2).

- Lir writes against `Channels::subscribe(channel, prefix, executor,
  handler, deadline, per_peer, done_executor, done)`.
- `Pushed` says which peers hold the interest, which were slow past
  `per_peer`, and which missed.
- SUBACK goes once every peer that is not slow holds it.

This blocks the SUBACK hold at step 2. The stand-in is the 2026-11-16
ruling, a whole deadline of 1 s.

Proved on 5 nodes with one peer delayed by 2 s:
- SUBACK comes within the per-peer bound plus a tick;
- the slow peer is named;
- a publish made after SUBACK from any other node reaches the
  subscriber.

**6, clock rules, and `HlcClock::admit`.** Lir uses these through Dagda's
register map, for retained values. Lir itself uses `hlc()`'s physical
time to measure a message's wait across nodes, for Message Expiry
(3.3.2-6).

This blocks retained values (step 4). There is no stand-in: Retain
Available is 0 until then.

Proved by a test on 3 nodes, one with a clock offset beyond the bound:
- that node leaves ready;
- its retained write is refused, not applied.

**21, the HTTP and JSON core.** Lir's HTTP needs, also recorded for the
claim board:
- routing by method and path, with body limits;
- a response that stays open and streams, for Server-Sent Events, with
  backpressure;
- a WebSocket upgrade on the same port, handed to Lir as a `Stream` for
  MQTT;
- the client's address behind trusted proxies;
- keyed limits (item 10);
- JSON on Boost.JSON.

This blocks Lir's HTTP endpoints, which the claim board uses. There is no
stand-in.

Proved by the board's three endpoints running on it: `POST /publish`,
`GET /retained`, and `GET /subscribe` as SSE.

**DNS responder (Q3).** Lir needs:
- A and AAAA records for a configured name, giving the active, ready nodes
  that advertise `lir.endpoints`, with a TTL of a few seconds (5 s
  proposed);
- SRV records `_mqtt._tcp` and `_secure-mqtt._tcp`, wanted but optional:
  common MQTT clients (Paho, mosquitto, MQTT.js) do not resolve SRV, so A
  and AAAA are what matter.

There is no stand-in: until then, operators point plain clients at a list
of node addresses.

Proved when a node stops: within the TTL plus one heartbeat, the name no
longer gives it.

**7, readiness.** Lir registers its checks: its Dagda spaces caught up,
and its listeners up. Placement skipping nodes that are not ready serves
Lir's shared-subscription choice too. This blocks nothing yet.

**10, keyed limits.** Lir uses connection-rate limits keyed by address,
and by client id. MQTT has no Retry-After, so Lir only refuses. This
blocks nothing.

**13, contract types.** Lir adopts the ack-level enum for its own
settings.

**14, the node helper.** `lir-node` is built on it.

**16, a random 16-byte id.** It gives MQTT 5's Assigned Client Identifier,
unique with no word between nodes, instead of node id plus a counter.

**New, not on the list: interest past the 1 KiB advert.** At 50,000
filters a node, every node's interest advert coarsens to "everything"
(`net/fanout.h:73-76`), so every publish goes to every node. That is
correct but defeats routing at Lir's sizes.

Lir needs an interest representation that stays selective at that size,
and that is Dannan's to shape. One option: a Bloom filter or hash set of
literal prefixes, carried paged or in a larger advert; Dannan's design.

Until then, routing is correct and delivery goes everywhere.

Proved when 50,000 filters on a node still keep a publish to an unmatched
topic off that node.

Not used: 1 to 5, the agreement items, and 22. 9, 11, 12, 15, 19, 20 and
23 are not Lir's (15 only through `Backoff`).

### C. Constraints

- **Executors:** Lir calls Dannan from strands of the node's io executor,
  one per connection and one per subsystem.
- **Never waits:** publishing on channels, matching, and the connection's
  strand, which never waits on a peer.
- **Deadlines:**
  - SUBACK: 1 s, or the per-peer bound.
  - Take-over call to a lease holder: 2 s.
  - CONNECT handling: within the client's keep-alive.
- **Size:** as above.

## To Dagda

### A. Used today

Nothing: the rewrite links no Dagda yet. It will fetch Dagda by
FetchContent, with its tools off (`DAGDA_BUILD_TOOLS`), beside Dannan.

What it will use:
- **`context.h`:**
  - `Heads` `open`, `read`, `mutate`, `watch`, `changed`;
  - `Objects` `write`, `read`, for retained or queued payloads over the
    inline limit;
  - `Tickets`.
- **The register map:** `RegisterMap::spec`, `put` (with expiry and refs),
  `remove`, `get`, `list(view, prefix, cursor, budget)`, `now()`.
- **D5 leases:** `acquire`, `holder`, `standing`.
- **D9 logs.**
- **The caught-up signal.**
- **The installed fixture** (D13), on `dannan::testing`.

### B. Needed, by item

**D6, the register map (2), for retained values.**
- The shape is as reviewed.
- The space is `capable("lir")`.
- Values carry the MQTT properties; a large payload is a ref.

This blocks step 4. The stand-in is Retain Available 0.

Proved on 3 nodes:
- a retained publish on node A is read by a SUBSCRIBE on node C after the
  commit;
- an empty payload clears it;
- a value with expiry is served with its remaining interval, and is
  absent after it.

**D5, the leases (4).** Lir needs:
- `acquire(scope, name, deadline) -> {holder, epoch, since}`;
- `holder(name)`;
- `standing(mine)`;
- a lease moving to another node when its holder leaves the roster, at
  epoch + 1.

Lir uses them for one connection per client id, and for wills and timed
ends.

This blocks client-id uniqueness across nodes and wills at step 4. There
is no stand-in: until then, MQTT-3.1.4-3 holds only within a node, a
stated limit.

Proved by:
- two CONNECTs with one id on two nodes: the first is ended, carrying the
  holder's epoch;
- in a split, both hold the id, and at the heal the lower epoch's
  connection ends.

**Caught-up signal (3).** Lir uses the per-space query, plus a notice
when the signal turns true, giving `{covers_all, peers_compared,
behind[]}`, as settled.
- CONNACK's Session Present waits on it for the session space.
- A SUBSCRIBE's retained read waits on it for the retained space.

This blocks step 4. There is no stand-in.

Proved when a node joining mid-write answers Session Present 1 only once
it covers its peers.

**Key reads and `changed` (the register-map note, section 1).**
- `get` and `list` for retained values.
- For sessions, every node needs every session's filters, for
  offline-queue matching (12a). The session space is `replicas(3)`, so a
  node needs a keys-changed notice and paged reads for a space it does not
  hold: the three-systems design's `watch_keys` and forwarded `entries`.

This blocks offline queues at step 4.

Proved by:
- a filter added to a session on node A being matched on node D within
  one notice;
- a node joining with paged reads.

**D9, logs, with (stream, dot) identity (12a).** Lir needs:
- `append(log, entries, level, deadline, executor, done)`, with entries
  of (dot, stream, bytes up to the inline limit, expiry);
- `ack(log, stream, dot, stage, bytes)`;
- `retire(log, stream, dot)`;
- `read(log, stream, after, budget)`;
- `scan`;
- `live`;
- `watch`;
- bounds per stream (10,000), dropping the oldest or refusing, by
  setting.

Lir uses them for in-flight QoS 1 and 2 state, offline queues, and the
QoS 2 tombstone window.

This blocks QoS 1 and 2 at step 4. The stand-in is Maximum QoS 0.

Proved by:
- a QoS 2 publish on node A whose node is killed after PUBREC: the client
  reconnects to B, finishes once, and the subscriber receives once;
- a queue past its bound behaves as set.

**D1 and D2, placement and per-space k.**
- Sessions use a `replicas(3)` space per shard with k = 2.
- Retained values use `capable("lir")`.
- Lir waits on "replicated" for every acknowledgement of a QoS 1 or 2
  publish, a retained write, or a session change.

This blocks step 4.

**Store health into readiness (6).** Lir's readiness follows it.

**FetchContent and `DAGDA_BUILD_TOOLS`.** Lir fetches Dagda as it fetches
Dannan, taking the consumer's when one is there.

Not used: the durable `AcceptorStore` (1), retire and drain (5),
Macha's items (7 to 9, 13), the catalogue (5a), and group commit (10),
except as D9 uses it.

### C. Constraints

- **Executors:** Lir calls Dagda's asynchronous forms only, from
  subsystem strands, and never a blocking form on an io thread.
- **Ack levels:**
  - QoS 0: ephemeral, not in Dagda.
  - QoS 1 and 2, retained values, session changes: replicated, k = 2.
- **Deadlines:** a PUBACK's commit within 50 ms at p99; a SUBSCRIBE's
  retained read within 1 s.
- **Sizes:** as above. Keys are topics (up to 65,535 bytes, mostly under
  128) and client ids (mostly under 64 bytes). Values are mostly under
  1 KiB.

## To Lugh

### A. Used, by the old plugin at `92ab3a0`

- **`lugh/client/verifier.h`:** `Verifier`, and its `AccessToken` result
  (`lir/auth/lugh.cpp:17`).
- **`lugh/client/key_source.h`:** `HttpsKeySource::make`, `Settings`,
  `IssuerStatus`, `status()` (`lir/auth/lugh.h:47`, `:67`).
- **`lugh/client/https.h`:** `HttpsClient`.

The rewrite's plugin uses the same, plus `TopicGrants` (L3).

### B. Needed

**Topic grants.**
- `TopicGrants::parse`, `publish()`, `retain()`, `subscribe()` and
  `refused()` in `lugh_client`.
- `may_publish` and `may_subscribe` in `lugh::client_topics`, on
  `lir::mqtt`.

These block the plugin's grants. The stand-in is anonymous clients only.

Proved by Lir's scope tests passing on `lugh_client`, with control
characters allowed in topics.

**The relying-party pieces.** `RelyingParty`, `bearer_token`, `discover`,
`TokenSource` and `FakeHttps`: the plugin needs the verifier, the key
source and `FakeHttps` for its tests. It needs no `RelyingParty`, because
Lir only verifies tokens and does not obtain them. The shape as sent to
Louhi works.

**Lugh publishing principal events over Lir.** Lugh is an ordinary MQTT
client of Lir through `lir::client`. Lir needs only:
- a Lugh client identity whose token grants `lir:pub:` (and `lir:retain:`,
  if any event is retained) on Lugh's topics;
- a client id unique per Lugh node.

QoS 1 needs Dagda's log under Lir (D9). Until then, Lir's staged Maximum
QoS is 0.

### C. Constraints

Verification is answered from memory on the connection's strand, never
waiting. Key fetches happen beforehand, off the strand, as today.

## To Lir's dependants: what Lir asks, and what it promises

The rewrite keeps `docs/contract.md`. Two facts every dependant must know:

1. **A pinned commit keeps working.** Louhi's `6268fef` builds and runs
   against the standing cluster's old Lir (`e839b4a`) until it moves its
   pin.
2. **The rewrite is functional for a dependant only when the nouns its
   use needs have shipped:**
   - QoS 1 and 2 on Dagda's log (D9).
   - Retained values on the register map (D6).
   - Persistent sessions on D1 and D2.
   - Client-id uniqueness across nodes on D5.

   Until then, the rewrite states those limits in MQTT's own terms
   (Maximum QoS 0, Retain Available 0, Session Expiry 0). A dependant that
   publishes at QoS 1 or retains (Louhi does both) stays on the old
   cluster until those ship.

Each dependant is asked, in the same shape as above:
- A, what it uses;
- B, what it needs, per item: the calls, what it blocks, the test that
  proves it;
- C, its constraints.

### The claim board (the coordinator answered for it)

It needs:
- retained publish and clear;
- a retained snapshot by filter;
- a live stream by filter;
- non-retained notes, all under `stack/`, over HTTP today.

Lir's answer:
- **The three HTTP endpoints are kept, same paths and semantics**, rebuilt
  on Dannan's HTTP core:
  - `POST /publish?topic=&qos=&retain=`, where an empty body clears a
    retained topic;
  - `GET /retained?filter=`, a JSON array of `{topic, qos, retain,
    payload | payload_hex}`;
  - `GET /subscribe?filter=`, as SSE.

  They become part of the contract (`docs/contract.md`, section 7). The
  board needs no change and need not move to `lir::client`. MQTT has no
  end-of-retained marker, so a snapshot over MQTT is less exact than
  `GET /retained`.
- **The board is functional on the rewrite** once D6 (retained), D9 (QoS
  1), D1 and D2 (replicated), and Dannan's HTTP core have shipped and
  Lir's steps 2 to 4 are done. Lir will say so when they are; it is not
  soon.

### Louhi's answer (2026-11-20)

- **Uses**, pinned at `6268fef`:
  - `Client`: construct, `on_state`, `start`, `stop`, `publish`.
  - `ClientConfig`: `nodes` (the roster), `tls.trust`, `client_id`,
    `password` (called at each CONNECT), `version`, `will`.
  - `Message`.
  - `StateChange`: `.state`, `.why`.
  - `Outcome`: `.ok`, `.why`.
  - `mqtt::Version::v5`, `QoS::at_least_once`, `mqtt::Bytes`.
  - FetchContent with `LIR_BUILD_SERVER=OFF`, Lir taking Louhi's Dannan,
    linking `lir::client`.
  - Paths are in `../louhi/louhi/events/lir.{h,cpp}`.
- **Topics**, all QoS 1:
  - `louhi/projects/{p}/tasks/{t}/nodes/{n}`, retained;
  - `louhi/projects/{p}/events`;
  - `louhi/nodes/{n}`, retained and cleared by the will.
- **Needs nothing beyond the contract.** Not needed: QoS 2, aliases, user
  properties, subscribing.
- Waits on the old cluster until D9, D6 and D1/D2 make the rewrite's QoS 1,
  retain and sessions true. The proof is its `events_test` against the new
  Lir, then a live run with Tom's word.
- **Constraints:**
  - The client runs on its own strand of a multi-threaded io context.
  - A publish past the queue's bound fails at once and is counted, so Lir
    being away never holds up the agent.
  - The password callable must not block.
  - A few publishes a second per node, under 4 KiB.

### Mimir's answer (2026-11-20)

Plans `lir::client` only, for its stage 6 Lugh plugin:
- **Connection:** over TLS, with a Lugh service token as the password.
- **Subscribes to** `lugh/principals/+` and `lugh/principals/$heartbeat`.
- **Client behaviour:** callbacks posted to its executor, reconnect with
  backoff and resubscribe.
- **Build:** FetchContent at a pinned tag with `LIR_BUILD_SERVER=OFF`,
  Dannan taken from Mimir.

It needs nothing beyond the contract:
- QoS 0, no retain and no persistent sessions are fine.
- A version gap or a missed heartbeat (30 s stale) makes Mimir recheck
  through Lugh's status read.

Constraints:
- Callbacks are never inline under a Lir lock.
- Connect and subscribe never block.
- A handful of events a second, under 1 KiB.
- One connection per Mimir node.
