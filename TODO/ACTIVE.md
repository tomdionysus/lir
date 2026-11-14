# Lir - Active Work

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
Lir as first-class users (Tom, 2026-11-13; Decisions below). Lir's needs
of both, and Lir's review of Dagda's design, are
[`2026-11-13-lir-needs.md`](2026-11-13-lir-needs.md). The design Tom ruled
on is `../dagda/TODO/2026-11-13-three-systems-design.md` ("Ruled",
Dagda `a058df9`); Dannan's is `../dannan/TODO/2026-11-13-stack-needs.md`.
Lir's work while they build, all of it chosen by Tom on 2026-11-13:
1. Step 5's design rewritten onto the ruled nouns, with its tests written
   first. No code against an unfrozen tag until (3).
2. ~~Repin to Dannan 0.5.6~~ Done 2026-11-13, ahead of Dagda 0.1.5 at
   Tom's word (Lir links no Dagda yet): `feature/dannan-0.5`, gate 307
   (`COMPLETED.md`). Then, by Tom's ruling relayed by Dannan the same
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
2c. ~~Linux arm64~~ Done 2026-11-14 on corvus-neptune: GCC 14, Debian 13,
   Boost 1.83, warnings as errors, all 323 tests (`COMPLETED.md`). The Pis
   are shared (corvus-jupiter .169, corvus-saturn .170, corvus-neptune
   .202; root over ssh): ask the session using a machine first, post a claim
   on the standing cluster, modest -j, nice 19, one heavy job per Pi.
2d. The standing Lir cluster on the three Pis (Tom, 2026-11-14: "install a
   Lir cluster on these machines and use it to test lir, and coordinate
   work"): `deploy/pi-cluster/`, ports 7537, 1983 and 8183, the key kept in
   `build/pi-cluster/` on the Mac. Sessions post claims on
   `stack/pis/<host>/claim` and status on `stack/sessions/<name>/status`.
   Upgrade it with `deploy/pi-cluster/install.sh` after each Lir change
   worth trying on real machines.
2e. The claim board plugin, specified at Tom's request for another session
   to implement (`../lir-develop-cluster/TODO/2026-11-14-claim-board-plugin.md`).
   Lir owes it two things:
   - ~~`GET /retained?filter=F`~~ Done 2026-11-14: the retained messages
     matching F as a JSON array, then close; 413 past
     `maximum_retained_bytes`, never cut short. Gate 329 on corvus-saturn.
   - Once the plugin lands: migrate the board to one topic per session per
     machine (`stack/pis/<host>/claims/<session>`), clear the old
     `stack/pis/<host>/claim` topics, and update `deploy/pi-cluster/README.md`.
     The first layout lets a second claim overwrite the first.
2b. When Dannan lands its partition reading (Tom, 2026-11-13): wire it
   into the interim, so a node on a minority side (an even split is one)
   degrades or refuses QoS 1/2 per its setting, ahead of Dagda.
3. Step 5 code against each Dagda slice tag as it lands, at Lir's own risk;
   nothing ships before contract v2 freezes at slice 4.
4. The stale plan text: the plan file's session and wildcard heads (the
   layout of 2026-11-04 replaced them), and the stack document's "2-4
   commits a shard", which is Dagda's file (asked of it).
Still waiting elsewhere: MQTT-SN 2.0 on OASIS (only working drafts; Tom
said continue); the Lugh plugin on Lugh's `lugh_client` (P10), which is
the `TokenVerifier`, hides where Lugh's signing keys come from, and keeps
HTTPS JWKS working for a node outside Lugh's cluster (agreed by Lugh,
2026-11-13).

The design is Tom's, 2026-11-03:
[`2026-11-03-lir-plan.md`](2026-11-03-lir-plan.md), with the seven
further sections in [`2026-11-04-lir-sections.md`](2026-11-04-lir-sections.md).
These files are the source of truth; the Claude Doc it began as is a
starter that will be deleted (Tom, 2026-11-04). How the projects divide the work is
`../dagda/TODO/2026-11-03-stack.md`, the one copy, which has Lir in it
("Lir on Dagda", and Tom's "Decided (Tom, 2026-11-04)").

## What Lir is

Tom, 2026-11-03 night: "It must expose a MQTT interface and support all
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

## Decisions

- (2026-11-13, Tom) "You all need to discuss your needs and ensure the
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
    has reviewed it. A small-message bench, here first, the Pis at Tom's
    later word.
- (2026-11-04, Tom; stack document "Decided", and `../dannan/TODO/ACTIVE.md`)
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
- (2026-11-03 night, Tom) Lir is its own project and agent, with
  Dannan's styles, conventions and layout. Macha is not modified until
  Tom says so.
- From the stack: rules 1 to 4; dependencies point down; hosted systems
  never address one another; the async shape; versioned contracts.

## The plan

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

## Tom's answers (2026-11-04, asked in turn)

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

## What Lir waits on

- Dagda: slices 0 to 4 of the three-systems design (about 18 sessions):
  0.1.5 and the measurement, node-local spaces, replicated spaces with
  per-space k, the merges and leases, the log; contract v2 at slice 4.
- Lugh: `lugh_client` (its P10), whose offline token verification fills
  the plugin's `TokenVerifier` (Tom, 2026-11-04); the tokens have landed (O7).
- Dannan: the partition reading (Lir's minority behaviour), the shared
  listener (optional for Lir until reviewed), the small-message bench.
  Lir is on `0.5.6`; Dagda 0.1.5 will be on `0.6.0`, the first minor
  with the ABI promise, and Lir follows it. The partition reading comes in
  Dannan `0.7.0`.
- Lugh: the grammar of scopes in its tokens.
- OASIS: an approved MQTT-SN 2.0 Committee Specification.
