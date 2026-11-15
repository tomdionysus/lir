# Lir - Active Work

Last updated: 2026-11-14. `COMPLETED.md` is the record; the history this
file carried until today is there, "Archived from ACTIVE.md, 2026-11-14".

## Resume here

**State.** `develop` at `51381af` (lir::client);
pushed to `488e89e` at Tom's word, the rest local. Gate on corvus-neptune
(Pi 5, Debian 13, GCC 14, Boost 1.83, Ninja, Release, warnings as errors,
no warnings, never throttled): `tools/ci.sh release` 351 passed at
`51381af`; `tools/ci.sh lugh` (the plugin on) 343 passed at the 0.8.0
repin (`912d33f`). Dannan pinned at `0.8.2`, Lugh at `dbb0573` (plugin
only). Lugh is on 0.8.0 at `ab28e2d` (724 passed there) with `lugh_client`
unchanged since `dbb0573`, so the pin stays until Lugh's first release
(Lugh, 2026-11-15). Unmerged: `feature/fuzz-targets`,
for the release gate.

**How work is done now (Tom, 2026-11-14):**
- **No builds or test runs on the Mac.** Every build is on the shared Pi 5
  build servers: corvus-jupiter 10.44.1.169, corvus-saturn 10.44.1.170,
  corvus-neptune 10.44.1.202, root over ssh; and corvus-uranus
  10.44.1.228 (Tom, 2026-11-14; 3 GB RAM, so `-j1` for Lir's heavier
  translation units; a claimable host only, not a cluster node). Work there goes in
  `/root/lir-work`.
- **Nobody schedules the Pis.** A session claims a free machine on the
  board and has it, first come, first served; one heavy job per Pi, `-j2`,
  `nice 19`; release the moment the job ends. Claim board 0.2.0
  (2026-11-14): claims name cores (4 a Pi; Lir's gate takes 2, its `-j`),
  and when none are free, `claim-board wait --any --cores 2 --job "..."
  --for 2h` in the background waits in the scheduler's queue instead of a
  sleep. Tom reversed the board-only rule: a session may also ask a
  machine's holder to make room.
- **The claim-board plugin is enabled.** From this session's next start,
  use its tools (`claims_list`, `claim_take`, `claim_release`,
  `status_set`, ...), not hand-written curl. A heavy command over ssh to a
  Pi is blocked without a heavy claim there. Docs:
  `../lir-develop-cluster/docs/how-it-works.md`; questions to the Lir
  Development Cluster session.
- **No wasted runs (Tom, 2026-11-14, to every session).** ASan and TSan
  runs are for releases. The same tests run more than once only for a
  stated reason: a flaky test or odd behaviour. "We're spending 50% of our
  time heating silicon for no good reason, and it stops now." The fuzz
  targets are sanitizer builds, so they run at the release gate.
- `build/pi.sh [host] [gtest filter]` (untracked, in `build/`) syncs the
  working tree to a Pi, builds incrementally at `-j2`, `nice 19`, stops on
  any undervoltage flag, and runs the tests. Claim the Pi first.

**Next, in order:**
1. **At the release gate:** `feature/fuzz-targets` (instrumenting the
   whole build, `3af35be`): install `clang` and `libclang-rt-19-dev`,
   build it (`CXX=clang++ -DLIR_BUILD_FUZZING=ON`), run each target once
   for `-max_total_time=60`, fix what they find, merge.
2. **Dagda's slices** (the Dagda session tells Lir as each lands): step 5,
   by [`2026-11-13-step5-design.md`](2026-11-13-step5-design.md), its tests
   written as each slice's tag lands. Ship nothing before contract v2
   freezes at slice 4. Lir is now on Dannan 0.7.0 and Dagda 0.1.5 on
   0.6.0: Lir needs a Dagda tag built on 0.7.0 before it links Dagda.
   There the minority choice extends to session and retained commits, and
   `refuse` waits to the deadline as the design says.
   Slice 1 is proven and on Dagda's develop (`1065b18`, 2026-11-14), not
   tagged: node-local spaces (`Policy::node_local()`, for the sequence
   blocks), the change notice (`Heads::watch`, `Heads::changed`), the
   installed in-process cluster fixture (`dagda::test_fixtures`); contract
   v2 served beside v1. Its tests (`OnDagda.SeqNeverRepeatsAcrossARestart`
   first) are written when the tag lands. The notify channel per space and
   `watch_keys` are slices 2 and 3. Dagda's develop is on Dannan 0.7.0
   since `f31c52e` (proven on corvus-jupiter, 1062 passed, 3 opt-in
   skipped), so slice 1's tag will be linkable. Tom said tag 0.2.0, then
   ruled (2026-11-14 evening) that Macha's port to Dagda comes first: all
   slice work is paused until Macha runs on Dagda as well as on its own
   store, and 0.2.0 stays local and unpushed until Tom says. Step 5 waits.
3. **Dannan's shared listener** (P3, `dannan::listen`). Done for TCP
   (2026-11-15): Lir is on Dannan 0.8.0 (`912d33f`), MQTT over TCP, TLS,
   WebSocket and WebSocket over TLS (`ad65541`) and HTTP and HTTPS
   (`ce4bcd0`) run on `StreamListener`s on the edge's strand
   (`strand_per_stream = false`); Lir's own acceptors, TLS handshakes and
   write queues for MQTT and HTTP are gone. `edge::listener_config`,
   `verified_certificate(Stream)` and `presented_unverified` are shared by
   both. A client certificate that did not verify is refused, as Lir's
   handshake refused it (Dannan's `optional` lets it through with the
   verdict). TCP_NODELAY: Dannan 0.8.1 sets it on every accepted
   socket (`Stream::set_no_delay` opts out), so small MQTT packets no
   longer wait on Nagle; Lir repinned with no code change (`13553c0`,
   337 passed), after reviewing the branch. Next: the MQTT-SN DTLS gateway (`lir/sn/gateway.cpp`) onto
   `DatagramListener` at Dannan 0.9.0, then `edge::server_context` and
   `verified_certificate(SSL*)` go.
4. **The Lugh plugin.** Done and merged (`9452fd0`; `lir/auth/lugh.h`,
   `LIR_BUILD_LUGH`, `--lugh-issuer`, `--lugh-ca`, `tools/ci.sh lugh`),
   repinned (`33b7c8b`) at Lugh's develop `dbb0573`, the end of P10, on
   Dannan 0.7.0: `AccessToken::x5t` comes decoded, so the adapter copies
   it, and the tests sign with Lugh's public `lugh/client/testing.h`;
   nothing internal to Lugh is included any more. Next: repin at Lugh's
   first release (Tom paused Lugh after P10). The hosted CI has a `lugh`
   job (Tom said yes, 2026-11-14): it checks out Lugh's private develop
   with the repository secret `LUGH_READ_TOKEN` and runs `tools/ci.sh
   lugh`; without the secret it says so and builds nothing. Tom adds the
   secret. The record of how it was
   shaped (`../lugh/TODO/2026-11-14-p10-plan.md`): Tom's rulings (2026-11-14): the
   plugin is optional and Lir builds without Lugh when it is off; it is a
   few-line adapter, `lir::auth::TokenVerifier` over
   `lugh::client::Verifier(issuers, WallClock)`: `set_keys(issuer, jwks)`
   from the key source, `verify_access(token, audience)` answering an
   `AccessToken` (subject, audiences, scopes, expires in Unix seconds,
   jkt, x5t as base64url text, decoded to 32 bytes) or a reason; headers
   `lugh/client/verifier.h` and `lugh/client/tokens.h`. Lir's seam carries
   exp as wall time since `8d78c95` (Lugh's finding), and Lir refuses a
   token at or past exp despite Lugh's 60 s skew (`lir/auth/tokens.h`); `lugh_client` links Dannan, OpenSSL and Boost; Lir pins a
   commit on Lugh's develop by FetchContent with `LUGH_BUILD_NODE=OFF`
   until Lugh releases. Its Dannan must be the one Lir pins: Lugh is on
   Dannan 0.7.0 (develop `7b0c717`), uses Lir's `dannan::dannan` when
   fetched into Lir's build (one Dannan a process), and moves with Lir
   when Lir moves past 0.7 (tell the Lugh session first). Boost.JSON
   (Tom, 2026-11-14): `lugh_client` compiles `boost/json/src.hpp`
   itself, and so does Lir, for the rules engine
   (`lir/rules/boost_json.cpp`, listed at `lir/CMakeLists.txt:25`). With
   the plugin on, Lir leaves that file out and takes the definitions from
   `lugh::client`; with it off, Lir compiles its own. A test of both
   configurations' link comes with the plugin. Lugh agreed: the
   definitions live in the exported `lugh_client` library, its Boost.JSON
   macros are PUBLIC on `lugh::client`, and its P10.6 tests a consumer
   that links it and uses Boost.JSON.
5. **MQTT-SN 2.0** when OASIS approves a Committee Specification.
6. **lir::client** (Tom, 2026-11-15, asked by Louhi; Lugh's events and
   Mimir use it too). Done (`51381af`): `lir/client/client.h`, the codec
   alone as `lir::mqtt`, `LIR_BUILD_SERVER=OFF` for consumers (`tools/ci.sh
   client`); 351 passed. Next: move `lir/client/transport.cpp` onto
   Dannan's `StreamConnector` (a 0.8 patch, design reviewed 2026-11-15)
   and delete the seam. QoS 2, aliases and user properties on request.

**The standing cluster** on the three Pis (`deploy/pi-cluster/`): one
`lir-node` each, ports 7537 (Dannan), 1983 (MQTT) and 8183 (HTTP), a
systemd unit as tom, the cluster key in `build/pi-cluster/cluster.key` on
the Mac and never in the repository. It carries the claim board. Upgrade it
with `deploy/pi-cluster/install.sh root@<pi>:/root/lir-work/build
corvus-jupiter=10.44.1.169 corvus-saturn=10.44.1.170
corvus-neptune=10.44.1.202` after a Lir change worth trying on real
machines; the restart is rolling and the board survives it.

The standing cluster runs `a559b0a` (Dannan 0.7.0) since 2026-11-14
03:52Z, upgraded at Tom's word from the gate run's build; each node's
`$lir/nodes` reads majority, 3 of 3, and the board's claims survived.

**Waiting on Tom:** nothing open.

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
   authenticator's own checks are done. The Lugh plugin waits on Lugh.

Beyond the steps, done: MQTT-SN 1.2 over UDP and DTLS, the rules engine,
Server Reference at CONNECT, limits and memory pressure on Dannan's ledger,
hosted CI, Linux arm64.

## Decisions

Not reopened without asking Tom.

- (2026-11-14, Tom) No builds or test runs on the Mac; the Pis only.
  Nobody schedules the Pis: claims on the board, first come, first served.
  The claim-board plugin is enabled for every session. Hosted CI is added,
  as the stack has it; it runs only when Tom pushes. Split joins are a
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
- **Dannan:** `0.7.0`: the deferred reply, the partition reading, then the
  shared listener; the small-message bench.
- **Lugh:** `lugh_client` (P10); the grammar of scopes in its tokens.
- **OASIS:** an approved MQTT-SN 2.0 Committee Specification.
