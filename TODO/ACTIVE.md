# Lir - Active Work

Last updated: 2026-11-19. `COMPLETED.md` is the record. This file was
rewritten on 2026-11-19 around the audit; what it carried before is in the
history at `53afd98` and in `COMPLETED.md`'s archives.

## The goal

The maintainer, 2026-11-03 night: "It must expose a MQTT interface and support all MQTT
functionality. But, it needs to be distributed, multimaster, clustered, using
Dannan as the network layer."

- All of MQTT 3.1, 3.1.1 and 5.0. A feature left out is a finding.
- Any node accepts any client operation. Both sides of a split serve, and
  merge after.
- Dannan is the network layer. What Lir persists lives in Dagda.

## Built on Dannan

The plan is [`2026-11-19-premise-and-audit.md`](2026-11-19-premise-and-audit.md).
Read it before anything else. In short (the maintainer, 2026-11-19 and 20):

- Lir is built on Dannan, not a system of its own that uses Dannan. Dannan
  is the distributed system; Lir is MQTT expressed in Dannan's nouns, and
  Dagda's for what persists.
- The question for every piece is "which Dannan or Dagda noun is this?" A
  piece that is Lir's own mechanism is wrong by construction.
- A gap is never Lir's to fill: it is a need for Dannan or Dagda, and Lir
  waits. No interim.
- Lir has no kind of node that Dannan does not have. Dannan has no special
  nodes.
- Nothing acknowledges a guarantee it does not have. The cluster is never
  stopped as a whole to change it.

## Resume here

**What happened (2026-11-19 and 20).** Lir wrote a guarded-publish design
made of four workarounds and put them to the maintainer as choices. The maintainer withdrew it:
"The questions you're asking me mean you haven't understood what Lir is
supposed to do." The fault is older than that design: the cluster that runs
today is Lir's own distributed system beside Dannan. The maintainer's "only real
question": what does full MQTT require that Dannan does not already
provide, and how is that built on top of Dannan? The plan holds Lir's first
answer, unchecked, and three things it does not know.

**Do this, in this order.** Build and delete nothing until step 6.
1. ~~Read Dannan's and Dagda's contracts whole, and correct the plan's
   table against them.~~ Done 2026-11-20: the plan's "What the contracts
   hold". Shared subscriptions corrected (placement picks nodes, not
   members); the three unknowns sharpened; nothing Lir waits on from
   Dagda is in its contract yet.
2. ~~Write the conformance gate.~~ Done 2026-11-20:
   [`docs/conformance.md`](../docs/conformance.md). 5.0: 252 statements,
   84 tested, 84 partial, 82 none, 2 n/a; 70 bear on more than one node.
   3.1.1: 139, 51 tested, 64 partial, 22 none, 2 n/a; 38 on more than one
   node. Two defects confirmed (resend order, a Subscription Identifier
   on an inbound PUBLISH) and three 3.1 deviations, none fixed.
3. ~~Map each cross-node need to its noun, or state the need.~~ Done
   2026-11-20: the plan's "The mapping", twelve needs over the 108
   statements. Needs only Dannan: delivery, assigned ids, shared choice,
   SUBACK and UNSUBSCRIBE, live order, the expiry clock. Waits on Dagda:
   every guarantee past one connection. Unknowns 1 to 3 sent to the
   Dannan and Dagda sessions the same day, nothing asked of them now.
4. ~~Have a fresh agent answer the real question independently.~~ Done
   2026-11-20: `2026-11-20-second-reader.md`, its differences in the
   plan's "The second reader". Two corrections taken: shared choice holds
   only while interest fits the advert, and order when a publisher moves
   between nodes is a fourth unknown.
5. ~~Take all of it to the maintainer.~~ the maintainer, 2026-11-20, on being asked whether
   to close "single-node gaps" first: "If you're asking about single-node
   specific code, you've missed the whole point, Again." Then: "I think
   realistically you'll have to go for a rewrite from scratch. Store Lir's
   existing contract, because it's used by other systems, and start from
   scratch."
6. **The rewrite**, on `feature/rewrite` (local, from `e809114`). Done
   there: the contract stored ([`docs/contract.md`](../docs/contract.md),
   headers verbatim under `docs/contract-v1/`); every source but the
   version files removed; a skeleton that builds `lir` and `lir_tests`
   (2 tests). The design, drafted 2026-11-20:
   [`2026-11-20-rewrite-design.md`](2026-11-20-rewrite-design.md), with
   The maintainer. Then code, tests first, every MQTT statement tested on Dannan's
   simulation harness, one node being only a cluster of one. There is no single-node Lir.

**State (2026-11-20).** `develop` holds the rewrite's start and its MQTT
codec, squashed from `feature/rewrite`:
- the contract, stored (`docs/contract.md`);
- `lir::mqtt` whole: data representations, topics, properties, every
  control packet of 5.0, 3.1.1 and 3.1;
- the conformance gate, generated from the tests' names.

Its gate on node-d:
- `tools/ci.sh` check, release, client, clang, ASan and UBSan, and TSan:
  113 tests, no sanitizer report;
- 9 of them against a real client, mosquitto at all three versions;
- coverage 99.9% of lines (the one line left is a closing brace gcov
  counts for allocation failure) and 100% of functions.

Dannan is pinned at `0.9.2`. Nothing of Dagda or Lugh is linked yet. The
build nodes have `mosquitto-clients` on node-d only; the others need it before
Lir's tests run there.

The old code is in the history at `e809114`.

**The standing cluster** runs `4762186` on Dannan 0.9.2 on all four build nodes,
majority 4 of 4, and carries the claim board. It runs the code under audit.
It stays as it is: nothing is added to it and no new behaviour is offered
from it. How to check, upgrade and configure it is
`deploy/test-cluster/README.md`. Every restart needs the maintainer's approval, each
time. `/root/lir-work` is gone on every build node since the wipe of 2026-11-19, so
a build there starts from a fresh clone.

## How work is done

- Before a design or a question to the maintainer, name the Dannan or Dagda noun each
  part is built on. A missing noun means wait and tell that project. Never
  offer the maintainer an option that is Lir's own mechanism. Say "I don't know" when
  that is the answer.
- No builds or test runs on the workstation. They run on the build nodes only, in
  `/root/lir-work`, at `-j2` and `nice 19` (node-d `-j1`: it has 4 GB).
  Claim cores on the board first and release them when the job ends. When
  no build node has free cores, run `claim-board wait` in the background.
- A hold on the build nodes (another project's priority) means start nothing there.
- No wasted runs. Sanitizers and the fuzz targets run at the release gate.
  A test is repeated only for a stated reason.
- Never keep a node list (the maintainer, 2026-11-15).
- A problem in Dannan, Dagda or Lugh goes to that project's session. Lir
  does not investigate or patch their code (the maintainer, 2026-11-19).
- The maintainer, 2026-11-20, to the stack: features and milestones are built on
  branches and land on `develop` as one squash commit each, on
  completion. Every build and test runs on the cluster. Sanitizers run
  only when a feature or milestone is complete, before its merge, or to
  diagnose a bug nothing else has found. Test once and be done; no
  repeated runs and no mutants unless a change is small or high-risk. No
  flaky test is tolerated: a test that fails at random is investigated.
  The repository is public, so its GitHub CI is kept working, with all
  relevant badges. No cluster core is ever held idle, for any reason:
  copy evidence off and release. Coverage is kept at about 100% of all
  code, measured (`tools/ci.sh coverage`) before each merge.
- Releases (the maintainer, 2026-11-20): once current work is done, a version is cut
  as follows:
  1. The docs are rationalised; the README is about installation and use.
  2. Debian packages are built.
  3. The full tests run, sanitizers and fuzzing included.
  4. The version is tagged `x.y.z`, merged to `main`, and pushed.

  Only minor or patch versions are bumped (the maintainer): the major stays 0 until
  he says.

  The release badge is kept. Lir's first, `0.1.0`, is the codec alone (the
  maintainer, 2026-11-20: "Lir should have a release with tags"), and its
  README and notes say the broker is not yet functional; the release in
  which the broker serves clients across nodes (step 2 of the design, with
  its stated limits) is the next minor, `0.2.0`. The fuzz targets (`lir/fuzz/`,
  `tools/ci.sh fuzz`) and the packages (`docs/packaging.md`,
  `tools/ci.sh deb`) exist. Still owed as the pieces arrive:
  `liblir-client0.1` with `lir::client` (tell Louhi the names and the
  commit), and the broker's `lir` package (node binary, config.yaml
  conffile, secrets.yaml from postinst, unit installed disabled, `lir`
  system user, dashboard under /usr/share/lir) when it serves clients.
- The stack negotiates through the coordinating session, which
  keeps the record (`../coordination/TODO/2026-11-20-cluster-stack-placement.md`).
  Before anything goes to the maintainer, ask whether the goals already answer it:
  no duplication; the deepest shared dependency owns what two or more
  need; no special role or node; failures never the user's problem. Only
  what the group cannot settle goes to the maintainer (the maintainer, 2026-11-20).
- Report findings before fixing.

## The converged plan (2026-11-20)

The stack's placement converged through the coordinating session (`../coordination/TODO/2026-11-20-cluster-stack-placement.md`,
"The converged plan", "Lir"); Lir agreed, adding sessions on D1/D2 and
in-flight and queues on D9, and Dannan's backoff in `lir::client`. For Lir:
- **Dannan's next minor:** the simulation harness exported as
  `dannan::testing` (`DANNAN_BUILD_TESTING_EXPORT`), and a per-peer answer
  bound that settles the SUBACK hold (L2).
- **Dagda:** D6, the register map, first, then D5, its leases. For the
  retained stamp, Dannan adds `HlcClock::admit`.
- **Lugh:** scope parsing and checks in `lugh_client`, anonymous mode
  included; Lir's needs sent to it.
- **The maintainer answered (2026-11-20, in Dannan's session):** Q1, HTTP and JSON
  move into Dannan, on Boost.JSON; Q2, the register map orders by (hlc
  stamp, node id); Q3, Dannan's DNS responder gives plain clients an
  address that survives a node; Q4, retirement stays an operator command.
  Dannan holds until the maintainer says go.
- **Dagda's D6 review (2026-11-20):** retained values need no index of
  Lir's: D6 opens capable("lir") once D1 exists, with get and list read
  locally; a register may name objects, and the head's reachability frees
  a superseded one; `RegisterMap::now()` is the expiry clock. A QoS 1 or 2
  retained PUBACK waits on D2's replicated commit.

## What waits, and on whom

Nothing here is started before the audit's findings have been to the maintainer.

- **Dagda:** slices 1 to 4 of its three-systems design, each tagged on
  Lir's Dannan, with contract v2 frozen at slice 4. Slice 0 is done (the
  log passes the PUBACK mark on a build node; `COMPLETED.md`, "Archived from
  ACTIVE.md, 2026-11-19"). Slice 1 is in Dagda `0.3.1`. No longer paused:
  The maintainer ruled on 2026-11-20 that Macha's cutover is not Dagda's concern
  (Macha builds on Dagda `0.3.2`), so Dagda's queue is open, Louhi's asks
  first, then Lir's. Dagda will answer unknowns 2 and 3 (the plan).
  Relayed by Louhi, 2026-11-20: The maintainer ruled that Dagda's release after
  `0.3.2` brings a durable `AcceptorStore` for Dannan's agreement, for
  every hosted system; Context v2 reads (one key, a prefix with paging);
  and a caught-up signal per space. Step 5's design is
  [`2026-11-13-step5-design.md`](2026-11-13-step5-design.md); it builds on
  the interim's shape and is re-read against the premise in the audit.
- **Dannan 0.9.3** (after the Macha cutover): the hybrid clock's ceiling is
  written only for stamps issued (the maintainer, 2026-11-19), so an idle node writes
  nothing. Lir changes nothing for it: repin, gate, then a rolling upgrade
  once the maintainer approves.
- **Dannan 0.10.0 (tagged without the stream carrier):** on repinning,
  `tools/ci.sh asan` and `tsan` set `DANNAN_SANITIZE=address` and `thread`
  beside the global flags (every stack library a sanitizer build links is
  built from source and instrumented, the maintainer, 2026-11-20; checked
  with nm), and the deb step installs Dannan's packages with Dannan's
  `tools/ci.sh install-debs <tag>` instead of building them itself.
  Before repinning, read 0.10.0's changelog and release notes against
  Lir's code: a new API that replaces a workaround, a helper that replaces
  code of Lir's own, a fixed bug Lir worked around. The repin commit says
  what changed and what was kept, and why (the maintainer, 2026-11-20).
- **Dannan 0.11.0:** the stream carrier (item 17's `inject`,
  `StreamDirectory`), which step 2's connections are tested on.
- **Dannan, later:** the `DatagramListener`, for the MQTT-SN DTLS gateway
  (then `edge::server_context` and `verified_certificate(SSL*)` go), and the
  peer-witnessed high-water mark for the clock.
- **Dannan, no date:** the atomic commit across names, which guarded
  publish needs; the small-message bench.
- **The maintainer:** whether to adopt Dannan's hybrid logical clock (`Context::hlc()`)
  for retained ordering, which reopens the 2026-11-04 ordering decision.
  Lir recommends it: the later write wins after a heal, and expiry gets an
  absolute time.
- **Lugh:** its first release with `lugh_client`, for the plugin's repin and
  `tools/ci.sh lugh`. Its tag `0.1.0` (2026-11-04) is older than
  `lugh_client` and is not that release.
- **OASIS:** an approved MQTT-SN 2.0 Committee Specification.
- **Owed to others:** `lir::client` is used by Louhi, Lugh's
  principal-events publisher and Mimir's Lugh plugin, at pin `987a5f4` or
  later. QoS 2, aliases and user properties come on request. The rewrite
  removed it; it returns written new as `liblir-client0.1`, not restored.
  Louhi's first requirements (2026-11-20; its publisher stays on
  `e40dd2e`, opt-in, until then): construct on an executor with a config,
  Dannan's time source and a scoped logger; the config carries the broker
  endpoints (from Dannan's roster), optional TLS trust roots, the client
  id, MQTT 5, and a will (an empty retained QoS 1 message that clears the
  node's status topic); a state callback with the reason for each change;
  start, and stop with a callback once everything is closed; publish at
  QoS 1 with retain, its outcome (ok, why) to a callback. Louhi asks for a
  token source called on each connect in place of a fixed password (a
  Lugh access token), reconnection with Dannan's backoff, and every
  callback posted to the caller's executor. Louhi's test broker uses
  `lir::mqtt`'s packet API, unchanged since `e40dd2e`.

Not taken: Dependabot's moves of `actions/checkout` and
`codecov/codecov-action` to v7. Dannan, the reference for the stack's CI,
is on v4 and v5.

**Watch: two Dannan defects that 0.9.x is believed to fix.** If either
comes back, tell Dannan at once (`COMPLETED.md`, "Archived from ACTIVE.md,
2026-11-19").
- The ledger: a TSan report in `RetainedMemoryLedger`, or two `~NodeRecord`
  freeing one map node.
- The join gap: `SlowGossipClusterTest` timing out in its fixture.

**Waiting on the maintainer:** nothing open. The findings of steps 1 to 4 go to him
at step 5.

## Decisions

Not reopened without asking the maintainer.

- (2026-11-20, the maintainer) Lir is rewritten from scratch. Its contract with other
  systems is stored and kept (`docs/contract.md`); nothing else of the old
  code binds the rewrite, and its tests go with it (the gate count falls
  from 361 to the skeleton's 2 by this ruling, not as a finding). There is
  no single-node Lir: one node is a cluster of one, and no work, test or
  question is split by "one node" and "many". Those of 2026-11-04 that set up the
interim cluster are marked: they are Lir's own mechanisms, and what
becomes of them goes to the maintainer with the plan's findings.

- (2026-11-20, the maintainer) Lir is built on Dannan, not merely using it. Dannan has
  no special nodes and neither has Lir. The only real question is what full
  MQTT requires that Dannan does not already provide, and how that is built
  on top of Dannan. "I don't know" is an acceptable answer.

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
- (2026-11-16, the maintainer) Step 5's payloads over 64 KiB are never shared between
  messages: each object starts with its message's dot, so its id is unique.
  No reference count in Lir and nothing new from Dagda.
- (2026-11-16, the maintainer) Lir holds SUBACK until every other node holds the new
  subscription, on Dannan's advert push. The wait has a deadline (1 s). On
  a miss the SUBACK goes anyway, counted (`subscribe_missed`) and logged.
- (2026-11-16, the maintainer) A fix of Dannan's is taken at its tag, never by pinning
  a Dannan develop commit (said of the ledger fix and 0.9.0).
- (2026-11-15, the maintainer, to Dannan, relayed by Dannan) Anything that can use
  consensus or agreement waits for Dannan's agreement primitive: no
  consensus, lease standing in for one, or workaround of Lir's own
  meanwhile. Lir's soft per-shard leases (who resumes a departed origin's
  entries) are a different guarantee and stand.
- (2026-11-15, the maintainer) Lir commits and pushes `develop` on its own; tags,
  releases and any other publishing wait for his word.
- (2026-11-15, the maintainer, relayed by Kvasir) Acknowledgement levels are named
  across the stack: ephemeral, persisted, decided, replicated, in that
  order; a setting for it is `ack_level`.
- (2026-11-15, the maintainer) The repository is public.
- (2026-11-15, the maintainer) Lir keeps no node list: Dannan's roster and adverts are
  the one list of Lir's nodes.
- (2026-11-14, the maintainer) No builds or test runs on the workstation; the build nodes only. Nobody
  schedules the build nodes: claims on the board, first come, first served. Hosted
  CI runs on each push. Split joins are a stated limit of Dannan's
  partition reading, not prevented.
- (2026-11-14, the maintainer) The Lugh plugin is optional, and Lir builds without
  Lugh.
- (2026-11-13, the maintainer) Dannan and Dagda serve Macha, Lugh and Lir as
  first-class users. From the designs that followed:
  - Dagda: the log is a noun (Lir's inflight log, session queues, history);
    built-in register-map and counter-map merges, retained as a
    register-map space with expiry per key and a keys-changed notice; head
    spaces on `replicas(N, scheme, capability)` with per-space k; leases on
    head spaces aligned with each shard's replicas; node-local spaces;
    adverts aggregated; one segment stream per node shared by its logs.
    Contract v1 frozen at Dagda `0.1.4`, v2 at slice 4. Any node may append
    to a log; a dot's origin must be the appender; Lir restricts retiring
    to the entry's origin, or the shard's lease holder once the origin has
    left.
  - A stated limit: a log keeps retired dots for 10 minutes, Lir's QoS 2
    dedup window, so a split longer than that can deliver a QoS 2 message
    twice.
  - PUBACK at QoS 1 with k = 2: p99 under 50 ms over the whole path.
  - Dannan: an ABI promise from `0.6.0` (a hosted system pins one Dagda tag
    and takes its Dannan minor); the partition reading (an even split is a
    minority); the shared listener.
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
  - Authentication is pluggable (`contract::Authenticator`). Token
    verification stays Lugh's (`lugh_client`).
  - HTTP is each hosted system's own.
  - Message types from 512, registered freely (Lir: `0x4C00` to `0x4C03`).
  - One process per node: `lir-node` is a node and advertises `lir`.
  - Process-level tests are GoogleTest only, no Python.
- (2026-11-04, the maintainer, asked in turn)
  - Fan-out direct on Dannan's channels; per-shard leases only where one
    writer matters.
  - *Interim, Lir's own mechanisms:* sessions mirrored to configurable
    standbys until Dagda (default one); retained on every node and PUBACK
    on acceptance until step 5.
  - Retained ordered by the Lamport counter, then the node id.
  - Load: up to about 10,000 messages a second, mostly under 1 KiB, 64 KiB
    inline.
  - Header tagline "Lir - The Message Broker". Namespaces `mqtt`, `subs`,
    `shard`, `broker`, `edge`, `cluster` (and `system`, `http`, `auth`,
    `rules`, `sn`, `client`, `contract`).
  - The seven design sections (`2026-11-04-lir-sections.md`): WebSockets,
    MQTT-SN 1.2 and 2.0, the rules engine, opt-in message history, Server
    Reference. The first release, `0.1.0`, follows step 5.
- (2026-11-03 night, the maintainer) Lir is its own project and agent, with Dannan's
  styles, conventions and layout. Macha is not modified until the maintainer says so.
- From the stack: rules 1 to 4; dependencies point down; hosted systems
  never address one another; the async shape; versioned contracts.

## The documents

- The premise and the audit: [`2026-11-19-premise-and-audit.md`](2026-11-19-premise-and-audit.md).
  Read it first.
- The design, the maintainer's: [`2026-11-03-lir-plan.md`](2026-11-03-lir-plan.md),
  with what later decisions changed listed at its end, and
  [`2026-11-04-lir-sections.md`](2026-11-04-lir-sections.md).
- Step 5's design: [`2026-11-13-step5-design.md`](2026-11-13-step5-design.md).
- The stack: `../dagda/TODO/2026-11-03-stack.md`; Dagda's design for its
  three owners: `../dagda/TODO/2026-11-13-three-systems-design.md`
  ("Ruled"); Dannan's: `../dannan/TODO/2026-11-13-stack-needs.md`.
- The standing cluster and the claim board: `deploy/test-cluster/README.md`.
