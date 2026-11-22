# Lir - Active Work

Last updated: 2026-11-22. `COMPLETED.md` is the record; what this file
carried before is in its "Archived from ACTIVE.md" sections.

## The goal

The maintainer, 2026-11-03 night: "It must expose a MQTT interface and support all MQTT
functionality. But, it needs to be distributed, multimaster, clustered, using
Dannan as the network layer."

- All of MQTT 3.1, 3.1.1 and 5.0. A feature left out is a finding.
- Any node accepts any client operation. Both sides of a split serve, and
  merge after.
- Dannan is the network layer. What Lir persists lives in Dagda.

## Built on Dannan

The premise is [`2026-11-19-premise-and-audit.md`](2026-11-19-premise-and-audit.md);
the design being built is [`2026-11-20-rewrite-design.md`](2026-11-20-rewrite-design.md).
In short (the maintainer, 2026-11-19 and 20):

- Lir is built on Dannan, not a system of its own that uses Dannan. Dannan
  is the distributed system; Lir is MQTT expressed in Dannan's nouns, and
  Dagda's for what persists.
- The question for every piece is "which Dannan or Dagda noun is this?" A
  piece that is Lir's own mechanism is wrong by construction.
- A gap is never Lir's to fill: it is a need for Dannan or Dagda, and Lir
  waits. No interim.
- Lir has no kind of node that Dannan does not have. Dannan has no special
  nodes. There is no single-node Lir: one node is a cluster of one.
- Nothing acknowledges a guarantee it does not have. The cluster is never
  stopped as a whole to change it.

## Resume here

**State (2026-11-22).**
- `develop` (pushed): the rewrite's `lir::mqtt` (every packet of 5.0,
  3.1.1 and 3.1, properties, topics), its fuzz targets, the conformance
  gate generated from test names, the Debian packages, on Dannan `0.10.1`.
  Gate on node-b: release 113 of 113, ASan and TSan with Dannan
  instrumented, deb lintian clean. CI green on develop and main.
- `main` and tag `0.1.0` (`9074dcb`): the codec alone; the README says the
  broker is not yet functional. Its packages are staged in
  `../tomcully-apt/incoming/lir/0.1.0/`; publishing them is the
  maintainer's. The next release is `0.2.0` (Dannan's ABI moved to
  `libdannan0.10`), cut when step 2 is done.
- `feature/step2` (local, not pushed), two commits on develop:
  - `63adf83`, `lir::broker::Delivery` on Dannan's notify channel, 7
    tests; passed on node-b on Dannan 0.9.2 (121 of 121), not yet run on
    0.10.1.
  - `93b9a9a`, the connection core's start, **unbuilt**:
    `lir/contract/connection.h` and `authenticator.h` restored unchanged
    from `docs/contract-v1/`, and the cluster fixture shared as
    `lir/test/fixtures/cluster.h` (the delivery test uses it). Build and
    run it first.
- The standing cluster runs the old code (`4762186`, Dannan 0.9.2) on all
  four build nodes and carries the claim board. Nothing is added to it;
  every restart needs the maintainer's approval
  (`deploy/test-cluster/README.md`).

**Next, in this order.**
1. **Step 2, the connection core** on `feature/step2`: `lir::broker::Link`,
   one client's protocol with no I/O of its own, fed bytes and answering
   through `contract::Connection`, tested on the cluster fixture with a
   fake `Connection`, every test named by its statement. Scope settled for
   the first piece:
   - CONNECT: first packet must be CONNECT (3.1.0-1), a second is a
     protocol error (3.1.0-2); authentication through
     `contract::Authenticator`; CONNACK with Maximum QoS 0, Retain
     Available 0, Shared Subscription Available 0, Subscription
     Identifier Available 0, a requested Session Expiry answered as 0,
     Session Present 0; an empty client id assigned (3.2.2-16); a 5.0 will
     at QoS above 0 refused 0x9B, a retained will 0x9A (3.2.2-12, -13); a
     refusal closes (3.2.2-8), 3.x return codes mapped from the 5.0
     reasons.
   - SUBSCRIBE: each filter through `Delivery`, SUBACK once every hold is
     in (3.8.4-1 to -8), granted QoS 0; a missed hold fails that filter
     (5.0) or closes (3.x) and unsubscribes it; an identical filter
     replaces the old subscription (3.8.4-3); a shared filter 0x9E;
     Subscription Identifiers 0xA1.
   - UNSUBSCRIBE and UNSUBACK (3.10.4-1 to -6); PINGREQ (3.12.4-1);
     DISCONNECT (3.14.4-1, -2).
   - PUBLISH at QoS 0 through `Delivery`, retain and dup cleared on the
     way out, properties dropped for a 3.x receiver; QoS 1 or 2 is a 5.0
     DISCONNECT 0x9B or a 3.x close; a 5.0 retain 0x9A; a topic alias
     0x94 (Topic Alias Maximum 0).
   - Then keep-alive (needs a time source: Dannan's, injected) and 5.0
     enhanced authentication (AUTH).
   - Open, not guessed: a 3.x will Lir cannot publish (wills wait on
     Dagda D5); No Local, which needs the publishing connection's origin
     on the channel; a 3.x retained publish, which 3.x cannot refuse.
     Settle each through the coordinating session before coding it.
   - The system topics: designed in
     [`2026-11-22-system-topics.md`](2026-11-22-system-topics.md), each a
     view every node renders for its own subscribers from Dannan's roster,
     adverts and telemetry, or an event published by the one node that saw
     it; no node publishes for the others. Its payloads are JSON, which is
     Dannan 0.11.0's (item 24): code waits on that tag. The design is approved
     (Decisions).
2. **Step 2, the transport** on Dannan 0.11.0's stream carrier
   (`listen/memory.h`'s `memory_pair`, `StreamListener::inject`,
   `test/fixtures/sim/streams.h`): `lir-node` serving MQTT on the
   cluster, wildcards, the system topics. Its API is as agreed with Dannan;
   0.11.0 is not tagged yet.
3. **`lir::client`** (step 3 of the design), so Louhi, Lugh's events
   publisher and Mimir can move off the old pin. Written new, not
   restored. Louhi's requirements (2026-11-20): construct on an executor
   with a config, Dannan's time source and a scoped logger; the config
   carries the broker endpoints (from Dannan's roster), optional TLS trust
   roots, the client id, MQTT 5 and a will; a state callback with each
   change's reason; start, and stop with a callback once closed; publish
   with its outcome (ok, why) to a callback; a token source called on
   each connect (a Lugh access token); reconnection with Dannan's backoff;
   every callback posted to the caller's executor. Ships as
   `liblir-client0.1`; tell Louhi the names and the commit.
   - Done (2026-11-22): `lir::client` on develop, to the contract header
     byte for byte, packaged as `liblir-client0.1`. Its tests run against a
     scripted broker on Dannan's listener; an end-to-end test against
     `Link` waits for step 2's transport.
   - Louhi (2026-11-22, through the coordinating session): it keeps its pin
     at `e40dd2e`, events off by default, until D5 and D6 ship. Its retained
     statuses and its retained QoS 1 will need them; it builds no QoS 0
     interim. Owed to Louhi, directly: (1) the `lir::client` develop commit
     when it lands; (2) word when the broker accepts retained messages and
     wills.
4. Done on `feature/fuzz-smoke` (2026-11-22): the three targets register
   with `dannan::testing`, `lir_tests` smokes each on every gate
   (`FuzzSmoke`), and the libFuzzer binaries share one driver.
5. **Release `0.2.0`** when step 2 is done, by the release procedure
   below.
6. Step 4 of the design as Dagda ships each noun: client-id leases (D5),
   sessions (D1, D2), the log for QoS 1 and 2 (D9), retained (D6, in
   Dagda 0.4.0), wills and timed ends. Each lifts its stated limit.

## How work is done

- Before a design or a question to the maintainer, name the Dannan or
  Dagda noun each part is built on. A missing noun means wait and tell that
  project. Never offer the maintainer an option that is Lir's own
  mechanism. Say "I don't know" when that is the answer.
- No builds or test runs on the workstation: the build nodes only, in
  `/root/lir-work`, at `-j2` and `nice 19` (node-d has 4 GB: no ASan or
  TSan there). Claim cores on the board only for a job ready to start,
  release them the moment it ends, and when none are free run
  `claim-board wait` in the background. Work other projects wait on goes
  first. A node's `/root/lir-work/dannan` is stale: set
  `LIR_DANNAN_REPOSITORY` to Dannan's GitHub URL there.
- Features and milestones are built on branches and land on `develop` as
  one squash commit each (the maintainer, 2026-11-20). Test once; a test is
  repeated only for a stated reason; no flaky test is tolerated.
  Sanitizers and fuzzing run before a merge or a release, or to hunt a bug
  nothing else finds. Coverage is kept near 100%, measured before each
  merge.
- Every dependency repin reads that release's changelog against Lir's
  code first, and its commit says what changed and what was kept.
- Versions are written in full, `x.y.z`, everywhere.
- Nothing tracked names the development setup: no hosts, addresses, user
  paths, session or hardware names, and "the maintainer" for him outside
  copyright. The real values are in the gitignored `CLAUDE.local.md`.
- A problem in Dannan, Dagda or Lugh goes to that project's session. Lir
  does not investigate or patch their code.
- The stack negotiates through the coordinating session, which keeps the
  record (`../coordination/TODO/2026-11-20-cluster-stack-placement.md`).
  Only what the group cannot settle goes to the maintainer.
- Releases (the maintainer, 2026-11-20): the docs rationalised and the
  README about installation and use; the Debian packages built; the full
  tests, sanitizers and fuzzing included; then tag `x.y.z`, merge to
  `main` and push, on his word. Only minor or patch versions are bumped.
  The release badge is kept. Packages go to the apt repository: hosted CI
  builds them from the tag (`gh workflow run CI --ref develop -f
  ref=<tag>`), and they are staged in `../tomcully-apt/incoming/lir/<tag>/`
  for the maintainer.
- The node, when it exists: the `lir` package (node binary, config.yaml
  conffile, secrets.yaml from postinst, unit installed disabled, `lir`
  system user, dashboard under /usr/share/lir); its settings follow
  Dannan's `docs/config.md` (defaults, YAML, secrets.yaml, environment,
  command line; key `a.b` is `--a-b` and `LIR_A_B`; `log.level`,
  `log.format`); the status and admin dashboard as the rewrite design
  states it.
- Report findings before fixing.

## What waits, and on whom

- **Dannan 0.11.0:** the stream carrier, which step 2's transport is built
  and tested on (feature/stream-carrier, first in line; no date).
- **Dannan 0.11.0, also:** JSON (item 24's Boost.JSON half), for every
  system topic's payload: moved forward from 0.12.0 by the maintainer
  (2026-11-22, Decisions), HTTP left in 0.12.0; Dannan told.
- **Dagda 0.4.0** (on Dannan 0.10.0): D6, the register map, for retained
  messages; read its notes when it lands. Then D5 (leases), D1 and D2
  (replicated spaces, a caught-up signal per space), D9 (the log).
- **Dannan, later:** HTTP and JSON (the dashboard and HTTP publish), the
  `DatagramListener` (the MQTT-SN DTLS gateway), the atomic commit across
  names (guarded publish), the per-peer answer bound for the SUBACK hold
  (L2).
- **Lugh:** a release with `lugh_client`'s scope parsing, for the optional
  Lugh plugin.
  Lugh 0.4.0 (2026-11-22): lugh-node's flags renamed to the stack's config
  names (`--server-address`, `--server-port`, `--oauth-issuer`,
  `--oauth-mtls-port`), and Lugh pins `lir::mqtt` at `0.1.0`; nothing of
  Lir's used the old names. In Lugh 0.5.0 `lugh_client`'s Fault, Outcome
  and Completion become aliases of Dannan's `contract/completion.h`, with
  two new Fault kinds a caller sees, `unavailable` and `timed_out` (where it
  said `failed`), and `lugh::undecided(fault)` covering the three: the
  plugin is written against that.
- **OASIS:** an approved MQTT-SN 2.0 Committee Specification.
- **The maintainer:** publishing the 0.1.0 packages from
  `../tomcully-apt/incoming/lir/0.1.0/`; his word for the next merge to
  `main`.

Not taken: Dependabot's moves of `actions/checkout` and
`codecov/codecov-action` to v7. Dannan, the reference for the stack's CI,
is on v4 and v5.

## Decisions

Not reopened without asking the maintainer.

- (2026-11-22, the maintainer) Dannan's JSON moves into 0.11.0, HTTP
  stays in 0.12.0, so step 2 finishes on one Dannan tag.
- (2026-11-22, the maintainer) The system topics as designed in
  `2026-11-22-system-topics.md`: views each node renders for its own
  subscribers from Dannan's roster, adverts and telemetry, each payload
  saying which node rendered it; connection events published by the node
  that saw them. No node publishes for the others.

- (2026-11-20, the maintainer) Lir is rewritten from scratch. Its contract with other
  systems is stored and kept (`docs/contract.md`); nothing else of the old
  code binds the rewrite, and its tests go with it (the gate count fell
  from 361 to the skeleton's 2 by this ruling, not as a finding). There is
  no single-node Lir: one node is a cluster of one, and no work, test or
  question is split by "one node" and "many".
- (2026-11-20, the maintainer) Lir is built on Dannan, not merely using it. Dannan has
  no special nodes and neither has Lir. The only real question is what full
  MQTT requires that Dannan does not already provide, and how that is built
  on top of Dannan. "I don't know" is an acceptable answer.
- (2026-11-20, the maintainer) Release `0.1.0` is the codec alone; the broker that
  serves clients across nodes is `0.2.0`.
- (2026-11-20, the maintainer) Retained values order by (hlc stamp, node id) (L1);
  HTTP and JSON are Dannan's; plain clients find a live node through
  Dannan's DNS responder (L8); retirement is an operator command.
- (2026-11-19, the maintainer, on the first guarded-publish design) Lir builds no
  workaround for what Dannan or Dagda has not shipped, and does not offer
  one to the maintainer as a choice.
  - Guarded publish waits for Dannan's atomic commit across names, with a
    register per topic. A whole tree in one register is not done.
  - Its acceptor state waits for Dagda. No memory store and no file of
    Lir's own.
  - "Leaderless does not mean 'group of immovable leaders'." No fixed or
    operator-moved replica set: placement is Dannan's and follows the
    roster.
  - The cluster is never restarted as a whole to introduce a feature.
- (2026-11-19, the maintainer) Dannan's code is Dannan's.
- (2026-11-16, the maintainer) Lir holds SUBACK until every other node holds the new
  subscription, on Dannan's advert push, with a deadline.
- (2026-11-16, the maintainer) A fix of Dannan's is taken at its tag, never by pinning
  a Dannan develop commit.
- (2026-11-15, the maintainer, to Dannan) Anything that can use consensus or
  agreement waits for Dannan's agreement primitive: no consensus, lease
  standing in for one, or workaround of Lir's own meanwhile.
- (2026-11-15, the maintainer) Lir commits and pushes `develop` on its own; tags,
  releases and any other publishing wait for his word.
- (2026-11-15, the maintainer, relayed by Kvasir) Acknowledgement levels are named
  across the stack: ephemeral, persisted, decided, replicated, in that
  order; a setting for it is `ack_level`.
- (2026-11-15, the maintainer) The repository is public.
- (2026-11-15, the maintainer) Lir keeps no node list: Dannan's roster and adverts are
  the one list of Lir's nodes.
- (2026-11-14, the maintainer) No builds or test runs on the workstation; the build
  nodes only, claimed on the board. Split joins are a stated limit of
  Dannan's partition reading, not prevented.
- (2026-11-14, the maintainer) The Lugh plugin is optional, and Lir builds without
  Lugh.
- (2026-11-13, the maintainer) Dannan and Dagda serve Macha, Lugh and Lir as
  first-class users. From the designs that followed:
  - Dagda: the log is a noun (Lir's inflight log, session queues, history);
    built-in register-map and counter-map merges, retained as a
    register-map space with expiry per key and a keys-changed notice; head
    spaces on `replicas(N, scheme, capability)` with per-space k; leases on
    head spaces; node-local spaces.
  - A stated limit: a log keeps retired dots for 10 minutes, Lir's QoS 2
    dedup window, so a split longer than that can deliver a QoS 2 message
    twice.
  - PUBACK at QoS 1 with k = 2: p99 under 50 ms over the whole path.
- (2026-11-04, the maintainer; stack document "Decided")
  - Durability: QoS 0 Ephemeral, QoS 1/2 Replicated (k = 2).
  - No `lir.local`: other systems are ordinary MQTT clients of Lir.
  - Multimaster: any node accepts any operation; both sides of a split
    serve and merge.
  - Authentication is pluggable (`contract::Authenticator`). Token
    verification stays Lugh's (`lugh_client`).
  - Message types from 512, registered freely (Lir: `0x4C00` to `0x4C03`).
  - One process per node: `lir-node` is a node and advertises `lir`.
  - Process-level tests are GoogleTest only, no Python.
- (2026-11-04, the maintainer, asked in turn)
  - Fan-out direct on Dannan's channels.
  - Load: up to about 10,000 messages a second, mostly under 1 KiB, 64 KiB
    inline.
  - Header tagline "Lir - The Message Broker". Namespaces `mqtt`, `subs`,
    `shard`, `broker`, `edge`, `cluster` (and `system`, `http`, `auth`,
    `rules`, `sn`, `client`, `contract`).
  - The seven design sections (`2026-11-04-lir-sections.md`): WebSockets,
    MQTT-SN 1.2 and 2.0, the rules engine, opt-in message history, Server
    Reference.
- (2026-11-03 night, the maintainer) Lir is its own project and agent, with Dannan's
  styles, conventions and layout. Macha is not modified until the maintainer says so.
- From the stack: rules 1 to 4; dependencies point down; hosted systems
  never address one another; the async shape; versioned contracts.

Decisions of 2026-11-04 to 16 that set up the interim cluster (session
standbys, retained on every node, the Lamport order, soft per-shard leases)
were Lir's own mechanisms and went with the rewrite; they are in
`COMPLETED.md`'s archives.

## The documents

- The premise and the audit: [`2026-11-19-premise-and-audit.md`](2026-11-19-premise-and-audit.md).
  Read it first.
- The rewrite's design: [`2026-11-20-rewrite-design.md`](2026-11-20-rewrite-design.md),
  with Lir's requirements up and down the stack (what it needs of Dannan,
  Dagda and Lugh, and what its dependants need of it) in
  [`2026-11-20-requirements.md`](2026-11-20-requirements.md), and an
  independent answer to what full MQTT needs that Dannan does not provide in
  [`2026-11-20-second-reader.md`](2026-11-20-second-reader.md).
- The maintainer's original design: [`2026-11-03-lir-plan.md`](2026-11-03-lir-plan.md)
  and [`2026-11-04-lir-sections.md`](2026-11-04-lir-sections.md).
- Superseded, kept for the record: [`2026-11-13-step5-design.md`](2026-11-13-step5-design.md),
  built on the interim's shape.
- What "supports MQTT" means: [`../docs/conformance.md`](../docs/conformance.md).
  The contract other systems build against: [`../docs/contract.md`](../docs/contract.md).
  Packaging: [`../docs/packaging.md`](../docs/packaging.md).
- The stack: `../dagda/TODO/2026-11-03-stack.md`; Dannan's config pattern:
  `../dannan/docs/config.md`.
- The standing cluster and the claim board: `deploy/test-cluster/README.md`.
