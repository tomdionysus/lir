# Lir - Active Work

Last updated: 2026-11-19. `COMPLETED.md` is the record. This file was
rewritten on 2026-11-19 around the audit; what it carried before is in the
history at `5a6769f` and in `COMPLETED.md`'s archives.

## The goal

Tom, 2026-11-03 night: "It must expose a MQTT interface and support all MQTT
functionality. But, it needs to be distributed, multimaster, clustered, using
Dannan as the network layer."

- All of MQTT 3.1, 3.1.1 and 5.0. A feature left out is a finding.
- Any node accepts any client operation. Both sides of a split serve, and
  merge after.
- Dannan is the network layer. What Lir persists lives in Dagda.

## Built on Dannan

The plan is [`2026-11-19-premise-and-audit.md`](2026-11-19-premise-and-audit.md).
Read it before anything else. In short (Tom, 2026-11-19 and 20):

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
made of four workarounds and put them to Tom as choices. Tom withdrew it:
"The questions you're asking me mean you haven't understood what Lir is
supposed to do." The fault is older than that design: the cluster that runs
today is Lir's own distributed system beside Dannan. Tom's "only real
question": what does full MQTT require that Dannan does not already
provide, and how is that built on top of Dannan? The plan holds Lir's first
answer, unchecked, and three things it does not know.

**Do this, in this order.** Build and delete nothing until step 6.
1. Read Dannan's and Dagda's contracts whole, and correct the plan's table
   against them.
2. Write the conformance gate: every normative statement of MQTT 5.0 and
   3.1.1 by its identifier, with the test that proves it or "none". No
   such list exists today, so how much of MQTT Lir supports is not
   measured.
3. Map each cross-node need to its noun, or state the need to the Dannan
   or Dagda session.
4. Have a fresh agent answer the real question independently.
5. Take all of it to Tom.
6. Only then code, tests first, on a clean branch.

**State.** `develop` is pushed and nothing is unmerged. No code changed on
2026-11-19. Dannan is pinned at `0.9.2` and Lugh at `dbb0573` (plugin only).
The last gate was on corvus-saturn at `e839b4a`, 2026-11-17: `tools/ci.sh
release` 361 passed, none skipped; hosted CI fully green (run 44200063109).
`tools/ci.sh lugh` was 343 at the 0.8.0 repin. These counts say the code
does what its tests say. They do not say the code is right.

**The standing cluster** runs `e839b4a` on Dannan 0.9.2 on all four Pis,
majority 4 of 4, and carries the claim board. It runs the code under audit.
It stays as it is: nothing is added to it and no new behaviour is offered
from it. How to check, upgrade and configure it is
`deploy/pi-cluster/README.md`. Every restart needs Tom's approval, each
time. `/root/lir-work` is gone on every Pi since the wipe of 2026-11-19, so
a build there starts from a fresh clone.

## How work is done

- Before a design or a question to Tom, name the Dannan or Dagda noun each
  part is built on. A missing noun means wait and tell that project. Never
  offer Tom an option that is Lir's own mechanism. Say "I don't know" when
  that is the answer.
- No builds or test runs on the Mac. They run on the Pis only, in
  `/root/lir-work`, at `-j2` and `nice 19` (uranus `-j1`: it has 4 GB).
  Claim cores on the board first and release them when the job ends. When
  no Pi has free cores, run `claim-board wait` in the background.
- A hold on the Pis (another project's priority) means start nothing there.
- No wasted runs. Sanitizers and the fuzz targets run at the release gate.
  A test is repeated only for a stated reason.
- Never keep a node list (Tom, 2026-11-15).
- A problem in Dannan, Dagda or Lugh goes to that project's session. Lir
  does not investigate or patch their code (Tom, 2026-11-19).
- Report findings before fixing.

## What waits, and on whom

Nothing here is started before the audit's findings have been to Tom.

- **Dagda:** slices 1 to 4 of its three-systems design, each tagged on
  Lir's Dannan, with contract v2 frozen at slice 4. Slice 0 is done (the
  log passes the PUBACK mark on a Pi; `COMPLETED.md`, "Archived from
  ACTIVE.md, 2026-11-19"). Slice 1 is on Dagda's develop, untagged. Paused
  by Tom until Macha runs on Dagda. Step 5's design is
  [`2026-11-13-step5-design.md`](2026-11-13-step5-design.md); it builds on
  the interim's shape and is re-read against the premise in the audit.
- **Dannan 0.9.3** (after the Macha cutover): the hybrid clock's ceiling is
  written only for stamps issued (Tom, 2026-11-19), so an idle node writes
  nothing. Lir changes nothing for it: repin, gate, then a rolling upgrade
  once Tom approves.
- **Dannan 0.10.0:** the `DatagramListener`, for the MQTT-SN DTLS gateway
  (then `edge::server_context` and `verified_certificate(SSL*)` go), and the
  peer-witnessed high-water mark for the clock.
- **Dannan, no date:** the atomic commit across names, which guarded
  publish needs; the small-message bench.
- **Tom:** whether to adopt Dannan's hybrid logical clock (`Context::hlc()`)
  for retained ordering, which reopens the 2026-11-04 ordering decision.
  Lir recommends it: the later write wins after a heal, and expiry gets an
  absolute time.
- **Lugh:** its first release with `lugh_client`, for the plugin's repin and
  `tools/ci.sh lugh`. Its tag `0.1.0` (2026-11-04) is older than
  `lugh_client` and is not that release.
- **OASIS:** an approved MQTT-SN 2.0 Committee Specification.
- **Owed to others:** `lir::client` is used by Louhi, Lugh's
  principal-events publisher and Mimir's Lugh plugin, at pin `6b23268` or
  later. QoS 2, aliases and user properties come on request.

Not taken: Dependabot's moves of `actions/checkout` and
`codecov/codecov-action` to v7. Dannan, the reference for the stack's CI,
is on v4 and v5.

**Watch: two Dannan defects that 0.9.x is believed to fix.** If either
comes back, tell Dannan at once (`COMPLETED.md`, "Archived from ACTIVE.md,
2026-11-19").
- The ledger: a TSan report in `RetainedMemoryLedger`, or two `~NodeRecord`
  freeing one map node.
- The join gap: `SlowGossipClusterTest` timing out in its fixture.

**Waiting on Tom:** nothing open. The findings of steps 1 to 4 go to him
at step 5.

## Decisions

Not reopened without asking Tom. Those of 2026-11-04 that set up the
interim cluster are marked: they are Lir's own mechanisms, and what
becomes of them goes to Tom with the plan's findings.

- (2026-11-20, Tom) Lir is built on Dannan, not merely using it. Dannan has
  no special nodes and neither has Lir. The only real question is what full
  MQTT requires that Dannan does not already provide, and how that is built
  on top of Dannan. "I don't know" is an acceptable answer.

- (2026-11-19, Tom, on the first guarded-publish design) Lir builds no
  workaround for what Dannan or Dagda has not shipped, and does not offer
  one to Tom as a choice.
  - Guarded publish waits for Dannan's atomic commit across names, with a
    register per topic. A whole tree in one register is not done.
  - Its acceptor state waits for Dagda. No memory store and no file of
    Lir's own.
  - "Leaderless does not mean 'group of immovable leaders'." No fixed or
    operator-moved replica set: placement is Dannan's and follows the
    roster.
  - The cluster is never restarted as a whole to introduce a feature.
- (2026-11-19, Tom) Dannan's code is Dannan's.
- (2026-11-16, Tom) Step 5's payloads over 64 KiB are never shared between
  messages: each object starts with its message's dot, so its id is unique.
  No reference count in Lir and nothing new from Dagda.
- (2026-11-16, Tom) Lir holds SUBACK until every other node holds the new
  subscription, on Dannan's advert push. The wait has a deadline (1 s). On
  a miss the SUBACK goes anyway, counted (`subscribe_missed`) and logged.
- (2026-11-16, Tom) A fix of Dannan's is taken at its tag, never by pinning
  a Dannan develop commit (said of the ledger fix and 0.9.0).
- (2026-11-15, Tom, to Dannan, relayed by Dannan) Anything that can use
  consensus or agreement waits for Dannan's agreement primitive: no
  consensus, lease standing in for one, or workaround of Lir's own
  meanwhile. Lir's soft per-shard leases (who resumes a departed origin's
  entries) are a different guarantee and stand.
- (2026-11-15, Tom) Lir commits and pushes `develop` on its own; tags,
  releases and any other publishing wait for his word.
- (2026-11-15, Tom, relayed by Kvasir) Acknowledgement levels are named
  across the stack: ephemeral, persisted, decided, replicated, in that
  order; a setting for it is `ack_level`.
- (2026-11-15, Tom) The repository is public.
- (2026-11-15, Tom) Lir keeps no node list: Dannan's roster and adverts are
  the one list of Lir's nodes.
- (2026-11-14, Tom) No builds or test runs on the Mac; the Pis only. Nobody
  schedules the Pis: claims on the board, first come, first served. Hosted
  CI runs on each push. Split joins are a stated limit of Dannan's
  partition reading, not prevented.
- (2026-11-14, Tom) The Lugh plugin is optional, and Lir builds without
  Lugh.
- (2026-11-13, Tom) Dannan and Dagda serve Macha, Lugh and Lir as
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
- (2026-11-04, Tom; stack document "Decided")
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
- (2026-11-04, Tom, asked in turn)
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
- (2026-11-03 night, Tom) Lir is its own project and agent, with Dannan's
  styles, conventions and layout. Macha is not modified until Tom says so.
- From the stack: rules 1 to 4; dependencies point down; hosted systems
  never address one another; the async shape; versioned contracts.

## The documents

- The premise and the audit: [`2026-11-19-premise-and-audit.md`](2026-11-19-premise-and-audit.md).
  Read it first.
- The design, Tom's: [`2026-11-03-lir-plan.md`](2026-11-03-lir-plan.md),
  with what later decisions changed listed at its end, and
  [`2026-11-04-lir-sections.md`](2026-11-04-lir-sections.md).
- Step 5's design: [`2026-11-13-step5-design.md`](2026-11-13-step5-design.md).
- The stack: `../dagda/TODO/2026-11-03-stack.md`; Dagda's design for its
  three owners: `../dagda/TODO/2026-11-13-three-systems-design.md`
  ("Ruled"); Dannan's: `../dannan/TODO/2026-11-13-stack-needs.md`.
- The standing cluster and the claim board: `deploy/pi-cluster/README.md`.
