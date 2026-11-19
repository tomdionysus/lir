# Lir - Active Work

Last updated: 2026-11-19. `COMPLETED.md` is the record, with the gate counts
and the detail this file no longer carries ("Archived from ACTIVE.md",
2026-11-14, 2026-11-15 and 2026-11-19).

## Resume here

**State.** `develop` is pushed and nothing is unmerged. The repository is
public, and Lir commits and pushes `develop` on its own (the maintainer, 2026-11-15).
Dannan is pinned at `0.9.2` and Lugh at `dbb0573` (plugin only).

The last gate was on node-b at `e839b4a`, 2026-11-17:
- `tools/ci.sh release`: 361 passed, none skipped.
- `SlowGossipClusterTest` repeated: 20 of 20.
- Hosted CI on `e839b4a`: fully green (run 44200063109).
- `tools/ci.sh client` was last run on a build node at the 0.8.5 repin (node-a,
  2026-11-16), clean; the hosted `client` job passed on `e839b4a`.
- `tools/ci.sh lugh`: 343, at the 0.8.0 repin (2026-11-15).
- No code has changed since; the docs and TODO were rationalised on
  2026-11-19 with no build or test run.

**The standing cluster** runs `e839b4a` on Dannan 0.9.2 on all four nodes,
majority 4 of 4. It came through the maintainer's wipe of the build nodes (2026-11-19) intact:
the same ids, `/retained` 200 on every node, and retained identical. The
wipe removed `/root/lir-work` on every build node, so the next build starts from a
fresh clone there.

To upgrade the cluster:
1. Get the maintainer's approval: every restart or upgrade of the standing cluster
   needs it, each time.
2. Tell Louhi and the coordinating session.
3. Do one node at a time, with node-a (the acting scheduler) last:
   `LIR_ONLY=<name> sh deploy/test-cluster/install.sh root@<node>:/root/lir-work/<build>/release node-a=192.0.2.1 node-b=192.0.2.2 node-c=192.0.2.3 node-d=192.0.2.4`.
   Install from a release build of `develop`.
4. After each node, check `GET /retained` on that node as well as
   `python3 -I build/cluster-check.py` (untracked). An HTTP failure shows
   only as "unreachable" in the check.

A configuration change goes out with
`LIR_CONFIG_ONLY=1 LIR_ONLY=<name> sh deploy/test-cluster/install.sh - <the four NAME=ADDRESS>`.
`louhi/#` is open to anonymous clients on all four (the maintainer, 2026-11-16).

**How work is done (the maintainer, 2026-11-14):**
- No builds or test runs on the workstation. They run on the build nodes only, in
  `/root/lir-work`, at `-j2` and `nice 19` (node-d `-j1`: it has 4 GB).
- Claim cores on the board first, and release them when the job ends. When
  no build node has free cores, run `claim-board wait` in the background.
- When the maintainer or the cluster session puts a hold on the build nodes (another project's
  priority), start nothing there until it is lifted. The standing
  `lir-node` service is not a job.
- No wasted runs: sanitizers run at the release gate. A test is repeated
  only for a stated reason: a flaky test, odd behaviour, or a fix.
- Sanitizers (ASan, UBSan and TSan) run on every build node: g++ anywhere, or
  clang++-19 on node-a, node-b and node-c. Use the 16 GB build nodes and check
  `free -m` first; node-d ran out of memory on one.
- Never keep a node list: Dannan's roster and adverts are the one list of
  Lir's nodes (the maintainer, 2026-11-15).
- Dannan's code is Dannan's (the maintainer, 2026-11-19). When a problem is in Dannan,
  Dagda or Lugh, tell that project's session; do not investigate or patch
  its code from here.

## Next, in order

1. **At the release gate:** the fuzz targets again (merged 2026-11-15,
   `COMPLETED.md`; clang and the fuzzer runtime are on node-b, node-c and
   node-a): `CXX=clang++ -DLIR_BUILD_FUZZING=ON -DLIR_BUILD_TESTING=OFF`,
   each target for `-max_total_time=60`, with the sanitizer runs. The
   release follows step 5.
2. **Step 5, on Dagda**, by [`2026-11-13-step5-design.md`](2026-11-13-step5-design.md),
   when Dagda's slices are tagged. Paused by the maintainer until Macha runs on Dagda.
   - Slice 1 is on Dagda's develop (`1065b18`), untagged.
   - Lir needs a Dagda tag on Lir's Dannan, since a process runs one Dannan.
     Its tests come with the tag, `OnDagda.SeqNeverRepeatsAcrossARestart`
     first.
   - Ship nothing before contract v2 freezes at slice 4.
   - Use the named acknowledgement levels.
3. **At Dannan 0.9.3** (after the Macha cutover; Dannan says when it exists):
   repin, gate, then a rolling upgrade once the maintainer approves it.
   - It changes the hybrid clock's ceiling write. Today every node rewrites
     `state/clock/hlc-ceiling` once a second: a temp file, an fsync, a rename
     and a directory fsync. That is about 5-6 disk writes/s on an idle node
     (measured on node-d, 2026-11-19).
   - The fix, as the maintainer decided it (2026-11-19): the ceiling follows only the
     stamps issued, never the wall clock. An idle node writes nothing. While
     a node is stamping, the ceiling goes 30 s ahead whenever it comes within
     2 s of the last stamp, about one write every 28 s. A clean stop writes
     the last stamp.
   - Lir changes nothing for it.
4. **Agreement and the clock, on Dannan 0.9**. Both are in Dannan since
   0.9.0; Lir's use of each needs the maintainer first.
   - **Guarded publish** for the claim board (its 0.5.0 or later;
     `../coordination/TODO/2026-11-15-claim-board-0.4.0.md`, part 1):
     - agreed trees, configured per node (the board needs `stack/pis/#` only);
     - every write and clear there goes through Dannan's agreement, and the
       revision is its log index;
     - `GET /retained` gives each topic's revision and an `as_of` per tree;
     - `POST /publish` takes `{"unchanged": {"filter", "since"}}` and answers
       409;
     - the minority side answers 503 there;
     - MQTT QoS 0 to an agreed tree is refused.
     Put the design to the maintainer first.
   - **The hybrid logical clock** for retained ordering. Lir reviewed
     Dannan's design (`../dannan/TODO/2026-11-15-hlc-design.md`); it is
     `Context::hlc()`. Adopting it needs the maintainer to reopen the 2026-11-04
     ordering decision. Recommend it: the later write wins after a heal, and
     expiry gets an absolute time.
     - A known limit: on a node without key custody, the ceiling is a plain
       durable file, so restoring an old state directory on its own is not
       refused. The contract states that limit, and Dannan 0.10.0's
       peer-witnessed high-water mark covers it (the maintainer, 2026-11-17).
5. **At Dannan 0.10.0:** move the MQTT-SN DTLS gateway
   (`lir/sn/gateway.cpp`) onto `DatagramListener`. Then delete
   `edge::server_context` and `verified_certificate(SSL*)`.
6. **At Lugh's first release:** repin the plugin and run `tools/ci.sh lugh`.
   The hosted `lugh` job uses the secret `LUGH_READ_TOKEN`. `lugh_client`'s
   interface is unchanged (Lugh, 2026-11-16). Since `2d75176`, Lugh
   percent-encodes a scope level that holds '"' or '\'; Lir's usual topics
   never contain either.
7. **MQTT-SN 2.0** when OASIS approves a Committee Specification.
8. **lir::client** users: Louhi (step 8), Lugh's principal-events publisher
   and Mimir's Lugh plugin, at pin `6b23268` or later. QoS 2, aliases and
   user properties on request.

**Watch: two Dannan defects that 0.9.x is believed to fix.** If either comes
back, tell Dannan at once. The history is in `COMPLETED.md` ("Archived from
ACTIVE.md", 2026-11-19).
- The ledger. A TSan report in `RetainedMemoryLedger`, or two `~NodeRecord`
  freeing one map node, would mean Dannan's explanation (`79dfa2e`, in
  0.9.0) is wrong. The suppression is gone.
- The join gap. If `SlowGossipClusterTest` times out in its fixture on
  hosted CI, the gap 0.9.2 closed is open again. Before 0.9.2 it failed 6
  of 30 runs on node-b.

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
  truth; where a later decision below differs, the decision wins. The plan
  lists those at its end ("Later decisions that differ"), and the sections
  file carries a note at each.
- Step 5's design: [`2026-11-13-step5-design.md`](2026-11-13-step5-design.md),
  with where it stands at its head.
- Lir's needs of the libraries and its review of Dagda's design, a record
  of 2026-11-13 with what became of each point:
  [`2026-11-13-lir-needs.md`](2026-11-13-lir-needs.md).
- For a reader and an operator: `README.md`, `CONTRIBUTING.md`, and the
  laws and disciplines, `docs/principles-and-laws.md` (Macha's, verbatim).
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

- (2026-11-16, the maintainer, asked in turn) Step 5's payloads over 64 KiB are
  never shared between messages. Dagda names an object by its content's
  SHA-256, and a remove releases every claim the passed clock dominates. So
  retiring one message would delete the payload of another message with the
  same content. Each object starts with its message's dot, so its id is
  unique. There is no reference count in Lir and nothing new from Dagda.
  Details: `2026-11-13-step5-design.md`, "Large payloads".

- (2026-11-16, the maintainer, asked in turn) Dannan's ledger fix: wait for the 0.9.0
  tag rather than pin a Dannan develop commit. (Carried out at the 0.9.0
  repin, 2026-11-17.)
- (2026-11-16, the maintainer, asked in turn) Lir holds SUBACK until every other node
  holds the new subscription, on Dannan's advert push (0.9.0). The wait has
  a deadline (1 s). On a miss the SUBACK goes anyway, with the missed peers
  counted (`subscribe_missed`) and logged. How it was measured and landed:
  `COMPLETED.md`, 2026-11-16 and 2026-11-17.
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
  at slice 4, each tagged on Lir's Dannan. Slice 0 is done: on a build node the log
  passes the PUBACK mark, p99 12.5 ms at a 10 ms tick at 8,000 a second
  (the figures are in `COMPLETED.md`, "Archived from ACTIVE.md,
  2026-11-19", and in Dagda's `TODO/2026-11-13-three-systems-design.md`,
  "What slice 0 has measured (the build node)"). Slice 1 is on Dagda's develop,
  untagged. Not yet measured: the notify channel's cost per space.
- **Dannan:** `0.9.3`, the clock ceiling write (after the Macha cutover).
  `0.10.0`: the DatagramListener for the MQTT-SN DTLS gateway, and the
  peer-witnessed high-water mark (the clock's restore case). Also the
  small-message bench. Agreement and the hybrid clock are in 0.9.0; Lir's
  use of each waits on the maintainer: the guarded-publish design for agreement, and
  reopening 2026-11-04 for the clock.
- **Lugh:** its first release, for the plugin's repin.
- **OASIS:** an approved MQTT-SN 2.0 Committee Specification.
