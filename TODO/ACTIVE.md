# Lir - Active Work

Last updated: 2026-11-15. `COMPLETED.md` is the record, with the gate counts
and the detail this file no longer carries ("Archived from ACTIVE.md",
2026-11-14 and 2026-11-15).

## Resume here

**State.** `develop` at the record commit after `7db72d1`, pushed; the
repository is public, and Lir commits and pushes on its own (the maintainer,
2026-11-15). Dannan pinned at `0.8.3`, Lugh at `dbb0573` (plugin only).
Last gate, node-d: 352 passed (347 in `tools/ci.sh release`, the 5
process tests once mosquitto-clients was installed there), `tools/ci.sh
client` clean. `tools/ci.sh lugh` 343 at the 0.8.0 repin. Hosted CI green
on every job from `e81e675`, CodeQL included. Unmerged:
`feature/fuzz-targets`, for the release gate.

**The standing cluster** runs `7db72d1` on all four nodes (2026-11-15
22:33Z, `COMPLETED.md`). To upgrade it, tell Louhi and the coordinating session first, then one node at a time, node-a (the acting
scheduler) last:
`LIR_ONLY=<name> sh deploy/test-cluster/install.sh root@<node>:/root/lir-work/build-ci/release node-a=192.0.2.1 node-b=192.0.2.2 node-c=192.0.2.3 node-d=192.0.2.4`
from a release build of `develop`, then `python3 -I build/cluster-check.py`
(untracked) after each node.

**How work is done (the maintainer, 2026-11-14):**
- No builds or test runs on the workstation; the build nodes only, in `/root/lir-work`,
  `-j2` and `nice 19` (node-d `-j1`: 4 GB). Claim cores on the board
  first (`claim-board claim <host> "<job>" --cores 2 --until +90m`), release
  when the job ends; `claim-board wait` in the background when none are free.
- No wasted runs: sanitizers at the release gate; a repeat only for a
  stated reason (a flaky test, odd behaviour, a fix).
- Never keep a node list: Dannan's roster and adverts are the one list of
  Lir's nodes (the maintainer, 2026-11-15).

## Next, in order

1. **Before the release gate:** `feature/fuzz-targets` (`3af35be`): install
   `clang` and `libclang-rt-19-dev`, build with `CXX=clang++
   -DLIR_BUILD_FUZZING=ON`, run each target once for `-max_total_time=60`,
   fix what they find, merge. The release follows step 5.
2. **Step 5, on Dagda**, by [`2026-11-13-step5-design.md`](2026-11-13-step5-design.md),
   when Dagda's slices are tagged: paused by the maintainer until Macha runs on Dagda.
   Slice 1 is on Dagda's develop (`1065b18`), untagged. Lir needs a Dagda
   tag on Dannan 0.8 (one Dannan a process); its tests
   (`OnDagda.SeqNeverRepeatsAcrossARestart` first) come with the tag.
   Ship nothing before contract v2 freezes at slice 4. Use the named
   acknowledgement levels.
3. **At Dannan 0.9.0:** repin; the MQTT-SN DTLS gateway
   (`lir/sn/gateway.cpp`) onto `DatagramListener`, then delete
   `edge::server_context` and `verified_certificate(SSL*)`; remove the TSan
   suppression of `RetainedMemoryLedger::release`
   (`lir/test/tsan-suppressions.txt`). Then, with agreement there:
   - **Guarded publish** for the claim board (its 0.5.0 or later;
     `../coordination/TODO/2026-11-15-claim-board-0.4.0.md`, part 1):
     agreed trees, a node setting (the board needs `stack/pis/#` only),
     every write and clear there through Dannan's agreement, the revision
     its log index; `GET /retained` gives each topic's revision and an
     `as_of` per tree; `POST /publish` takes `{"unchanged": {"filter",
     "since"}}` and answers 409; the minority side answers 503 there; MQTT
     QoS 0 to an agreed tree is refused. Put the design to the maintainer first.
   - **The hybrid logical clock** for retained ordering: Lir reviewed
     Dannan's design (`../dannan/TODO/2026-11-15-hlc-design.md`); adopting
     it needs the maintainer to reopen the 2026-11-04 ordering decision. Recommend it:
     the later write wins after a heal, and expiry gets an absolute time.
4. **At Lugh's first release:** repin the plugin (`tools/ci.sh lugh`). The
   hosted `lugh` job uses the secret `LUGH_READ_TOKEN`.
5. **MQTT-SN 2.0** when OASIS approves a Committee Specification.
6. **lir::client** users: Louhi (step 8), Lugh's principal-events publisher
   and Mimir's Lugh plugin pin `6b23268` or later. QoS 2, aliases and user
   properties on request.

**Watch:** `ClusterTest.ASharedGroupAcrossNodesGetsEachMessageOnce` timed
out once on hosted asan (arm64), passed on a re-run; a second time, look
at the groups channel's catch-up under ASan before touching its bound.

**Waiting on the maintainer:** nothing open.

## The standing cluster

`deploy/test-cluster/` (README there): four nodes, node-a 192.0.2.1,
node-b .170, node-c .202, node-d .228; ports 7537 (Dannan), 1983 (MQTT),
8183 (HTTP); a systemd unit as lir with `MemoryMin` and `OOMScoreAdjust`;
the cluster key in `build/test-cluster/cluster.key` on the workstation, never in the
repository. It carries the claim board (the the coordinating session
session's), whose clients and schedulers know all four. Four nodes keep a
majority through one loss; a 2 and 2 split is a minority on both sides.

## The documents

- The design, the maintainer's: [`2026-11-03-lir-plan.md`](2026-11-03-lir-plan.md) and
  [`2026-11-04-lir-sections.md`](2026-11-04-lir-sections.md). The source of
  truth; where a later decision below differs, the decision wins and the
  plan carries a note.
- Lir's needs of the libraries and its review of Dagda's design:
  [`2026-11-13-lir-needs.md`](2026-11-13-lir-needs.md).
- Step 5's design: [`2026-11-13-step5-design.md`](2026-11-13-step5-design.md).
- The stack: `../dagda/TODO/2026-11-03-stack.md`; Dagda's design for its
  three owners: `../dagda/TODO/2026-11-13-three-systems-design.md`
  ("Ruled"); Dannan's: `../dannan/TODO/2026-11-13-stack-needs.md`.
- The claim board: `deploy/test-cluster/README.md`; the plugin's
  specification: `../coordination/TODO/2026-11-14-claim-board-plugin.md`.

## What Lir is

The maintainer, 2026-11-03 night: "It must expose a MQTT interface and support all
MQTT functionality. But, it needs to be distributed, multimaster,
clustered, using Dannan as the network layer." Lir is written new; Macha
has no event system to extract.

- All of MQTT 3.1, 3.1.1 and 5.0. A feature left out is a finding.
- Distributed and clustered: a client connects, publishes, subscribes and
  resumes its session at any node.
- Multimaster: any node accepts any client operation; both sides of a
  split serve and merge after.
- Dannan is the network layer; what Lir persists lives in Dagda.

## The plan

1. **Wire and topics.** Done.
2. **The broker on one node.** Done.
3. **The edge.** Done: TCP, TLS, WebSocket and WebSocket over TLS, client
   certificates, clean closes.
4. **The cluster on Dannan.** Done as an interim: direct fan-out, sessions
   mirrored to standbys, retained on every node, PUBACK on acceptance.
   Proven across the three build nodes (`COMPLETED.md`, 2026-11-14).
5. **On Dagda.** By the step 5 design: per shard, an inflight log and a
   session-queue log (Dagda logs), a session-state head space with Lir's
   merge, a retained register-map space and a lease space, all on the
   shard's replicas; history as a log per topic filter; objects for
   payloads over 64 KiB; the sequence in reserved blocks in a node-local
   space; the retained-topic index on every node from the keys-changed
   notice. Groundwork that needs no Dagda is done (`session_shard_of`,
   `SequenceBlocks`, the stored entry forms). The first release, `0.1.0`,
   follows this step.
6. **System topics, HTTP, authentication.** System topics, HTTP and HTTPS
   publish, SSE and `GET /retained`, topic scopes and the Lugh token
   authenticator's own checks are done, and the Lugh plugin; its repin
   waits on Lugh's first release.

Beyond the steps, done: MQTT-SN 1.2 over UDP and DTLS, the rules engine,
Server Reference at CONNECT, limits and memory pressure on Dannan's ledger,
hosted CI, Linux arm64.

## Decisions

Not reopened without asking the maintainer.

- (2026-11-15, the maintainer, to Dannan, relayed by Dannan) Anything that can use
  consensus or agreement waits for Dannan's agreement primitive, which
  leads 0.9.0: no consensus, lease standing in for one, or workaround of
  Lir's own meanwhile. Lir's soft per-shard leases (who resumes a departed
  origin's entries) are a different guarantee and stand. Lir has nothing
  today that needs a single agreed decision.
- (2026-11-15, the maintainer) Lir commits and pushes `develop` on its own; tags,
  releases and any other publishing still wait for his word.
- (2026-11-15, the maintainer, relayed by Kvasir) Acknowledgement levels are named,
  not numbered, across the stack: ephemeral, persisted, decided,
  replicated, in that order; a setting for it is `ack_level`
  (`../kvasir/TODO/2026-11-15-ack-levels-standard.md`). Lir's words
  already match (QoS 0 ephemeral, QoS 1/2 replicated, the minority side
  persisted); step 5 uses the names, and `decided` once Dannan's
  agreement lands.
- (2026-11-15, the maintainer) The repository is public ("It's time"), so CodeQL's
  code scanning works as Dannan's does. Checked first: no keys or tokens
  in its history; the test cluster's LAN addresses are in the deploy docs.
- (2026-11-14, the maintainer) No builds or test runs on the workstation; the build nodes only.
  Nobody schedules the build nodes: claims on the board, first come, first served.
  The claim-board plugin is enabled for every session. Hosted CI is added,
  as the stack has it; it runs on each push. Split joins are a
  stated limit of Dannan's partition reading, not prevented.
- (2026-11-13, the maintainer) Dannan and Dagda serve Macha, Lugh and Lir as
  first-class users. From the designs that followed:
  - Dagda: the log is a noun (Lir's inflight log, session queues,
    history); built-in register-map and counter-map merges, retained as a
    register-map space with expiry per key and a keys-changed notice;
    head spaces on `replicas(N, scheme, capability)` with per-space k;
    leases on head spaces aligned with each shard's replicas; node-local
    spaces; adverts aggregated, generations on channels; one segment
    stream per node shared by its logs, appends grouped per destination
    node. Contract v1 frozen at Dagda `0.1.4`, v2 at slice 4, the catalogue
    v3. Any node may append to a log; a dot's origin must be the appender;
    acks and retires may name any origin's dots. Lir restricts retiring to
    the entry's origin, or the shard's lease holder once the origin has
    left.
  - A stated limit: a log keeps retired dots for 10 minutes, Lir's QoS 2
    dedup window, so a split longer than that can deliver a QoS 2 message
    twice.
  - PUBACK at QoS 1 with k = 2: p99 under 50 ms over the whole path, the
    pass mark for Dagda's log.
  - Dannan: an ABI promise from `0.6.0` (patches keep it; a hosted system
    pins one Dagda tag and takes its Dannan minor); the partition reading
    (an even split is a minority); a shared client listener below the
    framing, with stack Decided 9 standing, Lir keeping its own until it
    lands and Lir has reviewed it; a small-message bench.
  - Step 5: an online session acks from memory, so an inflight entry lives
    one client round trip; the sequence reserved in blocks of 65,536, gaps
    allowed, never reused; retained values read from their holders at
    SUBSCRIBE, with an index of topic names on every node and no value
    cache.
- (2026-11-04, the maintainer; stack document "Decided")
  - Durability: QoS 0 Ephemeral, QoS 1/2 Replicated (k = 2).
  - No `lir.local`: other systems are ordinary MQTT clients of Lir.
  - Multimaster: any node accepts any operation; both sides of a split
    serve and merge.
  - Authentication is pluggable (`contract::Authenticator`); a Lugh plugin
    depends on Lugh. Token verification stays Lugh's (`lugh_client`), which
    keeps HTTPS JWKS working for a node outside Lugh's cluster.
  - HTTP is each hosted system's own.
  - Message types from 512, registered freely (Lir: `0x4C00` to `0x4C03`).
  - One process per node: `lir-node` is a node and advertises `lir`.
  - Process-level tests are GoogleTest only, no Python.
- (2026-11-04, the maintainer, asked in turn) Fan-out direct on Dannan's channels;
  per-shard leases only where one writer matters. Sessions mirrored to
  configurable standbys until Dagda (default one). Retained ordered by the
  Lamport counter, then the node id. Load: up to about 10,000 messages a
  second, mostly under 1 KiB, 64 KiB inline. Header tagline "Lir - The
  Message Broker". Namespaces `mqtt`, `subs`, `shard`, `broker`, `edge`,
  `cluster` (and `system`, `http`, `auth`, `rules`, `sn`). The seven design
  sections answered (`2026-11-04-lir-sections.md`): WebSockets, MQTT-SN 1.2
  and 2.0, the rules engine, opt-in message history, Server Reference. The
  first release follows step 5.
- (2026-11-03 night, the maintainer) Lir is its own project and agent, with Dannan's
  styles, conventions and layout. Macha is not modified until the maintainer says so.
- From the stack: rules 1 to 4; dependencies point down; hosted systems
  never address one another; the async shape; versioned contracts.

## What Lir waits on

- **Dagda:** slices 0 to 4 of the three-systems design (0.1.5 on Dannan
  0.6.0, the measurement, node-local spaces, replicated spaces with per-space
  k and the advert aggregate, the merges and leases, the log), contract v2
  at slice 4. Slice 0's verdict, on node-a (Dagda, 2026-11-14):
  the log passes the PUBACK mark on a real device sync. Three processes,
  k = 2, 8,000 a second from outside the replica set: p99 12.5 ms at a
  10 ms tick, 23.5 ms at 20 ms, the same with 256 logs on one shared
  segment, nothing refused; p99 is about the tick plus 2.5 ms, so 50 ms
  holds to a tick of about 40 ms. A segment per log fails; the shared
  segment stands. A new lease holder reads a 5 s backlog (13,462 entries)
  in about 11 ms. Not yet measured: the notify channel's cost per space.
  Dagda's record: its `TODO/2026-11-13-three-systems-design.md`, "What
  slice 0 has measured (the build node)".
- **Dannan:** `0.9.0`: the agreement primitive (the maintainer: anything that can
  use agreement waits for it), the hybrid logical clock (Lir reviewed its
  design; adopting it for retained ordering needs the maintainer to reopen
  2026-11-04), the DatagramListener for the MQTT-SN DTLS gateway, and the
  ledger Lease fix that lets the TSan suppression go; the small-message
  bench.
- **Lugh:** its first release, for the plugin's repin.
- **OASIS:** an approved MQTT-SN 2.0 Committee Specification.
