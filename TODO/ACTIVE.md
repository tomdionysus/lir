# Lir - Active Work

Last updated: 2026-11-15. `COMPLETED.md` is the record, with the gate counts
and the detail this file no longer carries ("Archived from ACTIVE.md",
2026-11-14 and 2026-11-15).

## Resume here

**State.** `develop` at the record commit after `7db72d1`, pushed; the
repository is public, and Lir commits and pushes on its own (Tom,
2026-11-15). Dannan pinned at `0.8.5`, Lugh at `dbb0573` (plugin only).
Last gate, corvus-saturn: `lir_tests` release 354 passed, with the fuzz
targets merged and the cluster's join window fixed. `tools/ci.sh client`
clean on uranus at `7db72d1`. `tools/ci.sh lugh` 343 at the 0.8.0 repin.
Hosted CI: intermittent failures since `e81e675`. Two were Lir's join window,
now fixed. One is a TSan finding in Dannan (`COMPLETED.md`, 2026-11-15, "the
cluster's join window"), with Dannan and left unsuppressed, so the tsan job
may still fail on it. Nothing unmerged.

**The standing cluster** runs `7db72d1` on all four nodes (2026-11-15
22:33Z, `COMPLETED.md`). `louhi/#` is open to anonymous clients on all
four (Tom, 2026-11-16, kept open). A configuration change goes out with
`LIR_CONFIG_ONLY=1 LIR_ONLY=<name> sh deploy/pi-cluster/install.sh - <the four NAME=ADDRESS>`. To upgrade it, tell Louhi and the Lir Development
Cluster session first, then one node at a time, jupiter (the acting
scheduler) last:
`LIR_ONLY=<name> sh deploy/pi-cluster/install.sh root@<pi>:/root/lir-work/build-ci/release corvus-jupiter=10.44.1.169 corvus-saturn=10.44.1.170 corvus-neptune=10.44.1.202 corvus-uranus=10.44.1.228`
from a release build of `develop`, then `python3 -I build/cluster-check.py`
(untracked) after each node.

**How work is done (Tom, 2026-11-14):**
- No builds or test runs on the Mac; the Pis only, in `/root/lir-work`,
  `-j2` and `nice 19` (uranus `-j1`: 4 GB). Claim cores on the board
  first (`claim-board claim <host> "<job>" --cores 2 --until +90m`), release
  when the job ends; `claim-board wait` in the background when none are free.
- No wasted runs: sanitizers at the release gate; a repeat only for a
  stated reason (a flaky test, odd behaviour, a fix).
- Sanitizers run on every Pi since the 4K-page, 48-bit-VA kernels
  (2026-11-16): ASan, UBSan and TSan, with g++ anywhere or clang++-19 on
  jupiter, saturn and neptune. uranus has 4 GB and ran out of memory on
  one, so use the 16 GB Pis and check `free -m` first.
- Never keep a node list: Dannan's roster and adverts are the one list of
  Lir's nodes (Tom, 2026-11-15).

## Next, in order

1. **At the release gate:** the fuzz targets again (merged 2026-11-15,
   `COMPLETED.md`; clang and the fuzzer runtime are on saturn, neptune and
   jupiter): `CXX=clang++ -DLIR_BUILD_FUZZING=ON -DLIR_BUILD_TESTING=OFF`,
   each target for `-max_total_time=60`, with the sanitizer runs. The
   release follows step 5.
2. **Step 5, on Dagda**, by [`2026-11-13-step5-design.md`](2026-11-13-step5-design.md),
   when Dagda's slices are tagged: paused by Tom until Macha runs on Dagda.
   Slice 1 is on Dagda's develop (`1065b18`), untagged. Lir needs a Dagda
   tag on Dannan 0.8 (one Dannan a process); its tests
   (`OnDagda.SeqNeverRepeatsAcrossARestart` first) come with the tag.
   Ship nothing before contract v2 freezes at slice 4. Use the named
   acknowledgement levels.
3. **At Dannan 0.9.0:** repin; the MQTT-SN DTLS gateway
   (`lir/sn/gateway.cpp`) onto `DatagramListener`, then delete
   `edge::server_context` and `verified_certificate(SSL*)`; remove the TSan
   suppression of `RetainedMemoryLedger::release`
   (`lir/test/tsan-suppressions.txt`). Merge `feature/suback-hold`
   (`f35526d`, local, built and gated on Dannan's develop: `COMPLETED.md`,
   2026-11-16): set its pin to the 0.9.0 tag, gate, merge, and remove the
   README's stated limit on the window after SUBACK. Then, with agreement
   there:
   - **Guarded publish** for the claim board (its 0.5.0 or later;
     `../lir-develop-cluster/TODO/2026-11-15-claim-board-0.4.0.md`, part 1):
     agreed trees, a node setting (the board needs `stack/pis/#` only),
     every write and clear there through Dannan's agreement, the revision
     its log index; `GET /retained` gives each topic's revision and an
     `as_of` per tree; `POST /publish` takes `{"unchanged": {"filter",
     "since"}}` and answers 409; the minority side answers 503 there; MQTT
     QoS 0 to an agreed tree is refused. Put the design to Tom first.
   - **The hybrid logical clock** for retained ordering: Lir reviewed
     Dannan's design (`../dannan/TODO/2026-11-15-hlc-design.md`); adopting
     it needs Tom to reopen the 2026-11-04 ordering decision. Recommend it:
     the later write wins after a heal, and expiry gets an absolute time.
4. **At Lugh's first release:** repin the plugin (`tools/ci.sh lugh`). The
   hosted `lugh` job uses the secret `LUGH_READ_TOKEN`. Lugh after `2d75176`
   pins Dannan 0.8.5, the same as Lir, and `lugh_client`'s interface hasn't
   changed (Lugh, 2026-11-16). Since `2d75176`, Lugh percent-encodes a scope
   level that holds '"' or '\'. Lir's usual topics never contain either.
5. **MQTT-SN 2.0** when OASIS approves a Committee Specification.
6. **lir::client** users: Louhi (step 8), Lugh's principal-events publisher
   and Mimir's Lugh plugin pin `6b23268` or later. QoS 2, aliases and user
   properties on request.

**Watch:** the Dannan TSan finding: two `~NodeRecord` free one map node.
First seen in `ConnectingOnAnotherNodeTakesTheClientOver` (run 43773090610).
It recurred in `ARuleWrittenOnOneNodeAppliesOnEvery` (run 43802763961), on
one thread: `SystemTopics::on_roster` takes `nodes().snapshot()`
(`system.cpp:356`) and publishes (`system.cpp:358`), and
`Fanout::destinations`' roster copies free a node the snapshot's records
still point at. That is a use-after-free that happens deterministically on
some interleaving, and the first item in Dannan's plan for its next
session. Not suppressed; the tsan job may fail on it until Dannan's fix is
tagged. Send Dannan any new report. If a cluster test times out again, it is a new cause:
the join window is closed.

**Waiting on Tom:** nothing open.

## The standing cluster

`deploy/pi-cluster/` (README there): four nodes, jupiter 10.44.1.169,
saturn .170, neptune .202, uranus .228; ports 7537 (Dannan), 1983 (MQTT),
8183 (HTTP); a systemd unit as tom with `MemoryMin` and `OOMScoreAdjust`;
the cluster key in `build/pi-cluster/cluster.key` on the Mac, never in the
repository. It carries the claim board (the Lir Development Cluster
session's), whose clients and schedulers know all four. Four nodes keep a
majority through one loss; a 2 and 2 split is a minority on both sides.

## The documents

- The design, Tom's: [`2026-11-03-lir-plan.md`](2026-11-03-lir-plan.md) and
  [`2026-11-04-lir-sections.md`](2026-11-04-lir-sections.md). The source of
  truth; where a later decision below differs, the decision wins and the
  plan carries a note.
- Lir's needs of the libraries and its review of Dagda's design:
  [`2026-11-13-lir-needs.md`](2026-11-13-lir-needs.md).
- Step 5's design: [`2026-11-13-step5-design.md`](2026-11-13-step5-design.md).
- The stack: `../dagda/TODO/2026-11-03-stack.md`; Dagda's design for its
  three owners: `../dagda/TODO/2026-11-13-three-systems-design.md`
  ("Ruled"); Dannan's: `../dannan/TODO/2026-11-13-stack-needs.md`.
- The claim board: `deploy/pi-cluster/README.md`; the plugin's
  specification: `../lir-develop-cluster/TODO/2026-11-14-claim-board-plugin.md`.

## What Lir is

Tom, 2026-11-03 night: "It must expose a MQTT interface and support all
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
   Proven across the three Pis (`COMPLETED.md`, 2026-11-14).
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

Not reopened without asking Tom.

- (2026-11-16, Tom, asked in turn) The window after SUBACK: a subscription
  reaches other nodes at Dannan's heartbeat pace (5 s), so a message
  published elsewhere in that time was lost for the new subscriber
  (measured on the standing cluster: 0 of 20 at 0 ms, 8 of 8 at 5 s).
  Dannan adds `Nodes::advertise` with a deadline and evidence (held,
  missed), and `Fanout::subscribe` completing when the interest is held,
  in 0.9, with no 0.8.x stopgap. Lir holds SUBACK on it. On a miss, SUBACK
  anyway, with the missed peers counted and logged. A stated limit in the
  README until then.

- (2026-11-15, Tom, to Dannan, relayed by Dannan) Anything that can use
  consensus or agreement waits for Dannan's agreement primitive, which
  leads 0.9.0: no consensus, lease standing in for one, or workaround of
  Lir's own meanwhile. Lir's soft per-shard leases (who resumes a departed
  origin's entries) are a different guarantee and stand. Lir has nothing
  today that needs a single agreed decision.
- (2026-11-15, Tom) Lir commits and pushes `develop` on its own; tags,
  releases and any other publishing still wait for his word.
- (2026-11-15, Tom, relayed by Kvasir) Acknowledgement levels are named,
  not numbered, across the stack: ephemeral, persisted, decided,
  replicated, in that order; a setting for it is `ack_level`
  (`../kvasir/TODO/2026-11-15-ack-levels-standard.md`). Lir's words
  already match (QoS 0 ephemeral, QoS 1/2 replicated, the minority side
  persisted); step 5 uses the names, and `decided` once Dannan's
  agreement lands.
- (2026-11-15, Tom) The repository is public ("It's time"), so CodeQL's
  code scanning works as Dannan's does. Checked first: no keys or tokens
  in its history; the Pi cluster's LAN addresses are in the deploy docs.
- (2026-11-14, Tom) No builds or test runs on the Mac; the Pis only.
  Nobody schedules the Pis: claims on the board, first come, first served.
  The claim-board plugin is enabled for every session. Hosted CI is added,
  as the stack has it; it runs on each push. Split joins are a
  stated limit of Dannan's partition reading, not prevented.
- (2026-11-13, Tom) Dannan and Dagda serve Macha, Lugh and Lir as
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
- (2026-11-04, Tom; stack document "Decided")
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
- (2026-11-04, Tom, asked in turn) Fan-out direct on Dannan's channels;
  per-shard leases only where one writer matters. Sessions mirrored to
  configurable standbys until Dagda (default one). Retained ordered by the
  Lamport counter, then the node id. Load: up to about 10,000 messages a
  second, mostly under 1 KiB, 64 KiB inline. Header tagline "Lir - The
  Message Broker". Namespaces `mqtt`, `subs`, `shard`, `broker`, `edge`,
  `cluster` (and `system`, `http`, `auth`, `rules`, `sn`). The seven design
  sections answered (`2026-11-04-lir-sections.md`): WebSockets, MQTT-SN 1.2
  and 2.0, the rules engine, opt-in message history, Server Reference. The
  first release follows step 5.
- (2026-11-03 night, Tom) Lir is its own project and agent, with Dannan's
  styles, conventions and layout. Macha is not modified until Tom says so.
- From the stack: rules 1 to 4; dependencies point down; hosted systems
  never address one another; the async shape; versioned contracts.

## What Lir waits on

- **Dagda:** slices 0 to 4 of the three-systems design (0.1.5 on Dannan
  0.6.0, the measurement, node-local spaces, replicated spaces with per-space
  k and the advert aggregate, the merges and leases, the log), contract v2
  at slice 4. Slice 0's verdict, on corvus-jupiter (Dagda, 2026-11-14):
  the log passes the PUBACK mark on a real device sync. Three processes,
  k = 2, 8,000 a second from outside the replica set: p99 12.5 ms at a
  10 ms tick, 23.5 ms at 20 ms, the same with 256 logs on one shared
  segment, nothing refused; p99 is about the tick plus 2.5 ms, so 50 ms
  holds to a tick of about 40 ms. A segment per log fails; the shared
  segment stands. A new lease holder reads a 5 s backlog (13,462 entries)
  in about 11 ms. Not yet measured: the notify channel's cost per space.
  Dagda's record: its `TODO/2026-11-13-three-systems-design.md`, "What
  slice 0 has measured (the Pi)".
- **Dannan:** `0.9.0`: the agreement primitive (Tom: anything that can
  use agreement waits for it), the hybrid logical clock (Lir reviewed its
  design; adopting it for retained ordering needs Tom to reopen
  2026-11-04), the DatagramListener for the MQTT-SN DTLS gateway, and the
  ledger Lease fix that lets the TSan suppression go; the small-message
  bench.
- **Lugh:** its first release, for the plugin's repin.
- **OASIS:** an approved MQTT-SN 2.0 Committee Specification.
