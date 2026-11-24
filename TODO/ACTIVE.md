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

**State (2026-11-23).**
- `develop` (pushed): step 2, the broker, squash-merged: one client's
  protocol (`lir::broker::Link`) over Dannan's streams (`lir::edge`), on
  every node of the cluster alike (`lir::edge::Server`), with `lir-node`
  and its `lir` package; QoS 0 across nodes on Dannan's ordered delivery,
  wildcards, subscription options, topic aliases, keep-alive, enhanced
  authentication, MQTT over TCP, TLS and WebSocket, anonymous clients by
  filter, the optional Lugh plugin, and the system topics rendered by each
  node for its own subscribers. On Dannan `0.12.1` and Lugh `0.6.0` (the
  plugin). Since 2026-11-24 `lir-node`'s main is Dannan's node-program
  helper, and its listeners are named as the stack's (`mqtt`, `mqtts`, `ws`,
  `wss`, each `listen.*` and `tls.*`). The limits until Dagda's nouns ship stand, said in MQTT's own
  terms: Maximum QoS 0, Retain Available 0, every will refused, session
  expiry answered as 0.
  - Gate on the merge (node-a, 2026-11-23): release 273 of 273; ASan and
    UBSan 273 of 273, no leak; TSan 273 of 273; coverage 98.0% of lines,
    97.3% of functions, 281 of 281 (what is unrun is defensive, and the
    server's WebSocket closing handshake, which no test reaches); fuzz,
    tidy and the clang build clean (281 of 281); with the Lugh plugin 289
    of 289; `deb` in a clean trixie, seven packages, lintian clean. The
    conformance gate: 128 of 5.0's 252 statements proven, none waiting.
  - The tests of step 2 were written with the code, not seen failing
    first, except where the record says so.
- `main` and tag `0.2.0` (`9085820`): `lir::mqtt` and `lir::client`. Its
  packages and `0.1.0`'s are staged in `../tomcully-apt/incoming/lir/`;
  publishing them is the maintainer's.
- The standing cluster runs the old code (`4762186`, Dannan 0.9.2) on all
  four build nodes and carries the claim board. Nothing is added to it;
  every restart needs the maintainer's approval
  (`deploy/test-cluster/README.md`).

**Next, in this order.**
1. Step 4 of the design as Dagda ships each noun: client-id leases (D5),
   sessions (D1, D2), the log for QoS 1 and 2 (D9), retained (D6, in
   Dagda 0.5.0, with D5 and Dagda's simulated-cluster fixture), wills and
   timed ends. Each lifts its stated limit. Dagda 0.5.0 is the group's top
   priority (the maintainer, 2026-11-24); after it Dagda builds caught-up
   before the AcceptorStore, so sessions and CONNACK's Session Present come
   sooner.
   - Release `0.3.0`, the broker, once it accepts retained messages (D6)
     and wills (D5) (the maintainer, 2026-11-23), by the release procedure
     below.
   - Owed to Louhi, directly: word when the broker accepts retained
     messages and wills.
   - AthenaSIP's liveness scenario
     (`../athenasip/TODO/2026-11-23-lir-liveness-scenario.md`): a retained
     status and a will on one topic per node, a node dying, the client's
     Lir node dying, a split and a heal. It is step 4's acceptance test:
     retained (D6), wills on the client-id lease with its epoch (D5), and
     for the heal the order of retained values, (hlc stamp, node id), which
     the maintainer settled as L1 on 2026-11-20 and D6 carries. Owed to
     AthenaSIP: word as each step lifts.
2. The status and admin dashboard, on `@lugh/admin-ui` (Lugh 0.6.0), with
   Lir's own payload viewer; then the later pieces (shared subscriptions on
   Dannan's placement, Server Reference, MQTT-SN, the rules engine,
   history).

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
- No node is preferred (the maintainer, 2026-11-23): a job takes whichever
  build node has the cores free now, the tree synced there and built from
  scratch if need be, never a queue for one node because it holds a warm
  build tree or an earlier build. A missing tool or -dev package is
  installed there (one core claimed, `nice -n 19 apt-get install -y`, a
  board note naming it and the node). A wait names every node
  (`claim-board wait --any`); a job that must run on one node (a service
  that lives there, or TSan needing a particular clang) says so in its
  claim.
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

- **Dagda, D6 on its develop (`00e0a9f`, contract 3), 0.5.0 with D5:**
  Lir's design for retained messages is
  [`2026-11-23-retained.md`](2026-11-23-retained.md), with Dagda's answers.
  Code waits for Dagda's fixture on Dannan's simulated cluster, coming
  with 0.5.0 (Decisions: functional, not exactly replayable, for now).
  lir-node's Dagda settings: storage.data path, limit, reserve_free.
- **Dagda, D6 public (history):** Dagda 0.4.0 (2026-11-23, on Dannan 0.11.1) has the
  register map in its tree (`dagda/heads/register_map.h`), but stamping
  with a Lamport stand-in "until Dannan's HlcClock::admit is released; no
  space is opened on it in a release", and its changelog says the
  contract is unchanged. Dagda's answer (2026-11-23): D6 is not public in
  0.4.0; it comes in Dagda 0.5.0 with D5, and Dagda will say when D6
  reaches its develop. Then D5 (leases: the client-id lease, wills), D1
  and D2 (replicated spaces, a caught-up signal per space: sessions), D9
  (the log: QoS 1 and 2).
- **Dannan, later:** HTTP (0.13.0, after its register layer: the dashboard
  and HTTP publish), the `DatagramListener` (the MQTT-SN DTLS gateway), the
  atomic commit across names (guarded publish), and the per-peer answer
  bound for the SUBACK hold (L2): not in 0.12.x, whose `Channels::subscribe`
  still takes one deadline (`Answer::hold` is a deferring handler's held
  reply, not L2). L2 is 0.13.0's slow-peers item (Dannan, 2026-11-24),
  after the register layer, the HTTP core and the DNS client:
  `Channels::subscribe` takes a `per_peer` bound beside its deadline; a peer
  past it is not waited for (its push still lands later), `Pushed` names
  slow peers apart from failed; the bound is the larger of `per_peer` and a
  multiple of that peer's latency, which Lir accepted. Dannan sends the
  signature before coding.
- **Dannan 0.13.0** (about 2026-12-05, the maintainer's ruling): a DNS
  client (asynchronous, with a deadline; A, AAAA, SRV, TXT; resolv.conf's
  servers by default). Lir calls none itself: lir::client's `host` mode
  dials through Dannan's connector, which moves onto it. Asked of it: the
  lookup inside the dial's deadline, each address of several tried.
- **OASIS:** an approved MQTT-SN 2.0 Committee Specification.
- **The maintainer:** publishing the 0.1.0 and 0.2.0 packages from
  `../tomcully-apt/incoming/lir/`. Release 0.3.0 waits on Dagda (Decisions).

Not taken: Dependabot's moves of `actions/checkout` and
`codecov/codecov-action` to v7. Dannan, the reference for the stack's CI,
is on v4 and v5.

## Decisions

Not reopened without asking the maintainer.

- (2026-11-23, the maintainer) Lir's Dagda-backed tests run on Dagda's
  fixture on Dannan's simulated cluster as it comes with Dagda 0.5.0:
  functionally right under faults, splits and skew, not bit-for-bit
  replayable (Dagda's own threads and fsyncs). They wait on conditions,
  never on step counts or sleeps; a flaky run is a finding. Exact replay
  when Dagda moves its threads and disk onto the simulation's schedule.
- (2026-11-23, the maintainer) The broker is released as 0.3.0 once it
  accepts retained messages and wills (Dagda D6 and D5), not before: what
  Louhi and AthenaSIP need of it.
- (2026-11-23, the maintainer) QoS 0 travels on Dannan's ordered delivery
  (0.11.1), so every topic is ordered (MQTT-4.6.0-6); a late message is
  dropped, as QoS 0 allows.
- (2026-11-22, the maintainer) Anonymous clients, when a node allows them,
  are held to three lists of MQTT topic filters in its configuration
  (publish, retain, subscribe), checked by `lir::mqtt`, not Lugh's scope
  strings: Lir parses no scope grammar and allows them with or without the
  Lugh plugin.
- (2026-11-22, the maintainer) `$lir/clients/connected` and
  `/disconnected` carry the client id, the MQTT version, the transport and,
  on disconnect, the reason, with "from"; never the client's address.
- (2026-11-22, the maintainer) The Lugh plugin: a user name sent with a
  token must be its subject (else 0x86); a DPoP-bound token is refused; a
  certificate-bound one serves only the connection that presented that
  certificate.
- (2026-11-22, the maintainer) A node's Dannan settings are Dannan's shared
  reader (`dannan/config/node_settings.h`, 0.11.0), all under `cluster.*`
  (`--cluster-listen-port`, `LIR_CLUSTER_LISTEN_PORT`); Lir's own keys are
  `mqtt.*` and the like, which never collide.
- (2026-11-22, the maintainer) Dannan's JSON moves into 0.11.0, HTTP
  stays in 0.12.0, so step 2 finishes on one Dannan tag.
- (2026-11-22, the maintainer) The system topics as designed in
  `2026-11-22-system-topics.md`: views each node renders for its own
  subscribers from Dannan's roster, adverts and telemetry, each payload
  saying which node rendered it; connection events published by the node
  that saw them. No node publishes for the others.
- (2026-11-22, the maintainer) Until Dagda's leases (D5), every will is
  refused at CONNECT: 0x9B above QoS 0, 0x9A retained, otherwise 0x83; 3.x
  return code 3. A 3.x retained PUBLISH closes the connection, as one above
  QoS 0 does. No Local is honoured by carrying the publisher's client id
  ahead of the 5.0 PUBLISH on Lir's channel.

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
  a DNS name (L8): Dannan's responder then; since 2026-11-23 no cluster
  DNS is planned (Dannan's responder dropped, and the maintainer ruled
  that no project needs Manannan to serve DNS), so plain clients are
  given node addresses or the operator's own DNS, and lir::client reads
  Dannan's roster; retirement is an operator command.
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
