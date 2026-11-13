# Step 5: Lir on Dagda (design draft, 2026-11-13)

A draft for Tom. Nothing here is built, and no code is written against an
unfrozen Dagda tag until Tom approves this. It maps Lir's interim state onto
the nouns Tom ruled on 2026-11-13 (`ACTIVE.md`, Decisions;
`../dagda/TODO/2026-11-13-three-systems-design.md`, "Ruled"). It also lists
the tests to write first, and the questions it raises for Tom and for Dagda.

## Where Lir is (the interim, `lir/cluster/cluster.h:16-37`)

- A publish is delivered on its own node, then carried in one hop to the
  nodes with matching subscribers: QoS 0 on `lir.notify`, QoS 1/2 on
  `lir.seq`, at least once, in order per origin, deduplicated by sequence.
  There is no shard primary in the path (Tom's answers 1: fan-out direct).
- PUBACK is sent on acceptance at the publishing node. Nothing is durable.
- Retained values are held on every node, versioned by `RetainedVersion`
  (Lamport counter, then node id; `lir/broker/broker.h:56-60`), spread on
  `lir.retained`, and fetched whole by a node that joins.
- A session lives on the node its client is connected to, as a
  `SessionImage` (`broker.h:110-119`: subscriptions, outbound inflight,
  queued deliveries, inbound QoS 2 packet ids, the next packet id). It is
  mirrored in memory to `session_standbys` nodes (default 1) every 100 ms,
  and taken over by `lir.session_take` when the client connects elsewhere.
- Shared groups are published on `lir.groups`; the publishing node gives each
  message to one member node in turn.

Step 5 keeps that shape (direct fan-out, sessions served where the client is)
and makes it durable. Fan-out does not move to a shard primary.

## The mapping

| State | Today | Step 5 | Dagda noun |
|---|---|---|---|
| A QoS 1/2 publish, until every target node has taken it | Nowhere: lost if the publishing node dies before fan-out | One entry in the shard's inflight log, appended by the publishing node at Replicated before PUBACK | Log per shard, `replicas(3)`, k = 2, stream key the topic |
| A delivery a session still owes its client | `SessionImage.queued` and `.outbound`, in memory and on standbys | Online and acking: nothing new is written (below, "Two rates"). Offline, or past the receive maximum: an entry in the shard's session-queue log, stream key the client id | Log per session shard, `replicas(3)`, k = 2, count bound per stream 10,000 |
| Session state proper: subscriptions, expiry, will, version, next packet id, inbound QoS 2 ids, takeover counter | `SessionImage`, in memory and on standbys | A key in the session shard's head space, Lir's merge (higher takeover counter wins; equal, union) | Head space per session shard, `replicas(3)` |
| Retained values | Every node, in memory | A key in the topic shard's register-map space: last writer by (Lamport, node id), expiry per key, tombstones for the window | Register-map space per topic shard, `replicas(3)` |
| Which retained topics exist (for wildcard subscribes) | Implicit: every node has every value | An in-memory index of retained topics on every node, fed by `Heads::watch_keys` on every retained space; values fetched from holders at SUBSCRIBE | Keys-changed notice (no storage) |
| The lease per shard | None | `lir/shard/<n>`: who resumes the shard's owed entries when a publishing node is gone | Lease space per shard, placed with the shard's scheme |
| The per-origin sequence | In memory | A reserved block, durable in a node-local space (below) | Node-local space |
| Payloads over 64 KiB | Inline | Objects at Replicated, claimed; the log entry carries the id; deleted by `Objects::remove` on the final retire | Objects, Claims |
| Message history | Not built | A log per opted-in topic filter, bounded by count and age | Log |
| Wildcard and shared-group interest | Channels and memory | Unchanged | (none) |

Shards: topics by `shard_of(topic)` (`lir/shard/shard.h:34`); sessions by
`SHA-256("lir-sess:" || client id) mod S` (plan 103). `S` defaults to 256,
which gives about 1,280 spaces and logs: inflight log, queue log, session
space, retained space and lease space per shard. Dagda's slice 0 measures
whether a node carries that. If it does not, the first cut is `S` = 64. The
second is the lease records kept in the session space.

## The QoS 1/2 path

1. **Accept.** Edge E takes a PUBLISH, assigns the dot `(E, seq)` from its
   reserved block, and appends the entry to the topic shard's inflight log at
   Replicated. The entry holds the message, its expiry, the target nodes
   known now, and the publishing client and packet id. For QoS 2, the
   client's packet id is recorded against the dot in the same append.
2. **PUBACK or PUBREC** when the append's evidence names k replicas. Target:
   p99 under 50 ms over the whole path (Tom, 2026-11-13).
3. **Fan out** on `lir.seq` to the target nodes, as today, in parallel with
   the append for QoS 1. For QoS 2, fan-out waits for the append, so that a
   resend after E's death finds the dot recorded and is not delivered twice.
4. **A target node S takes it** for each matching session. For a connected
   session under its receive maximum, S delivers and acks E only when the
   client has acknowledged (PUBACK, or PUBCOMP for QoS 2). For an offline
   session, or one at its receive maximum, S appends the delivery to the
   session-queue log at Replicated and acks E once it is durable. The queue
   entry is under S's own dot `(S, seq)`, since a log refuses an entry whose
   dot is not the appender's (Dagda, 2026-11-13); the publisher's dot rides
   in its bytes for the per-session dedup. S appends in the order it
   receives, which `lir.seq` keeps per origin, so per-origin order holds. Shared
   groups take one member, as today.
5. **Ack and retire.** E records each node's ack as `Logs::ack` with S's
   node id as the stage bytes. When every target node has acked, E retires
   the entry, which leaves a tombstone for 10 minutes.
6. **E is gone** (Dannan's roster says it left): the holder of
   `lir/shard/<n>` scans the shard's inflight log (`Logs::scan`) and fans out
   every live entry again to the nodes not yet acked. Each subscriber node
   deduplicates by dot per session, as it does today by sequence. A target
   node that left is removed from what is owed by an ack written for it by
   the lease holder.

**Two rates.** Point 4 of the path keeps the second hot path off the disk. A
session that is online and acking holds its deliveries only in memory,
because the inflight entry stays live until the client acks, and point 6
re-delivers from it. Only a session that cannot take a message now writes to the queue log.
So the log's append rate is the publish rate plus the rate of messages to
offline or saturated sessions, not the delivery rate. The cost is that an
inflight entry lives for one client round trip, not one network hop.

## Sessions

- **Connect at node N:** read the client's key from the session space
  (forwarded if N is not a holder), raise the takeover counter, commit at
  Replicated, then send `lir.session_take` so the old node closes the
  connection. The old node needs to hand over nothing: the state is in
  Dagda. What the old node held only in memory (online deliveries,
  point 4 of the QoS path) is still live in the inflight logs, and is redelivered to N.
- **Queued messages** for the session are read from the queue log's stream
  for its client id, in order per origin (R4), and retired as the client
  acks.
- **Session standbys go.** `session_standbys`, `mirror_every` and
  `lir.session_mirror` are deleted when step 5 lands; a test that relied on
  them is rewritten against Dagda, never deleted.
- **Expiry and wills:** the session key carries its expiry and any delayed
  will. Of the session space's holders, the lease holder of the session
  shard fires expired wills and drops expired sessions. Commits at the low
  rate.
- **A split:** both sides may connect the same client. On heal the higher
  takeover counter wins (node id breaks ties), the other is sent DISCONNECT
  0x8E, and subscriptions and inflight are unioned (sections 2). Duplicates
  are possible; loss is not.

## Retained

- A retained PUBLISH commits the topic's key in the shard's register-map
  space at Replicated (QoS 1/2) or Persisted (QoS 0). The value is the
  message, versioned by `RetainedVersion` as today. An empty payload is a
  tombstone, and the MQTT 5 message expiry is the key's expiry.
- Every node follows every retained space's changed keys (`watch_keys`) into
  an in-memory index of topics. A SUBSCRIBE matches the filter against that
  index, and reads the values from the holders with forwarded paged
  `entries`, grouped by shard. Retained messages go to the client before
  live ones, as today.
- `$lir/rules/` and `$lir/mqttsn/topics/` are retained messages, so they
  move with retained (rules: every node holds every rule, sections 7).

## The sequence counter

A dot is `(origin, seq)`, with seq monotonic across restarts. Committing
every seq would put a commit on every publish. Instead, a node reserves a
block (default 65,536) by committing its upper bound to its node-local
space, and uses seq from memory until the block is spent. After a restart it
starts above the last bound committed. Gaps are allowed; reuse is not. The
node-local space's no-rollback rule (Dagda D3) refuses a restored older
file, so a block is never handed out twice.

## On a minority side

Dannan's partition reading (Tom, 2026-11-13: an even split is a minority)
decides, on a side that cannot reach k:
- `degrade`, the default: append at Persisted, PUBACK, count the message as
  under-replicated in `$lir/nodes/<id>`, and let Dagda's repair re-replicate
  on heal.
- `refuse`: wait to the deadline, then refuse with 0x97.

The same applies to session commits (connect, subscribe) and retained
commits.

## Tests to write first

From the design, before any code. Each is named as it will appear in
`lir_tests`, and runs on Dagda's installed in-process cluster with stepped
time (Dagda D13).

- `OnDagda.AQoS1PublishIsAckedOnlyOnceTwoReplicasHoldIt`
- `OnDagda.APublishingNodeThatDiesBeforeFanOutLosesNoAckedMessage`
- `OnDagda.ALeaseHolderResumesWhatADeadNodeOwed`
- `OnDagda.AQoS2MessageIsDeliveredOnceAcrossTheOriginsDeath`
- `OnDagda.AQoS2MessageIsDeliveredOnceAcrossASplitShorterThanTheWindow`
- `OnDagda.ASplitLongerThanTheWindowMayDuplicateQoS2AndNeverLoses` (the stated limit)
- `OnDagda.AnOfflineSessionsQueueSurvivesItsNodesDeath`
- `OnDagda.AnOnlineSessionWritesNoQueueEntriesWhileItAcks`
- `OnDagda.ASessionResumesAtAnyNodeWithItsSubscriptionsAndQueue`
- `OnDagda.ASessionConnectedOnBothSidesOfASplitKeepsTheHigherTakeover`
- `OnDagda.ARetainedValueSurvivesEveryNodeRestarting`
- `OnDagda.AWildcardSubscribeGetsRetainedMatchesFromEveryShard`
- `OnDagda.ARetainedValueExpiresWithItsMessageExpiry`
- `OnDagda.AClearedRetainedValueDoesNotReturnAfterAHeal`
- `OnDagda.SeqNeverRepeatsAcrossARestart`
- `OnDagda.AMinoritySideDegradesToPersistedByDefault`
- `OnDagda.AMinoritySideSetToRefuseRefusesWith0x97`
- `OnDagda.APayloadOver64KiBIsAnObjectDeletedOnItsFinalAck`
- `OnDagda.OrderPerTopicAndOriginHoldsAcrossAFailover`

Step 4's cluster tests stay. Where one names an interim behaviour (standbys,
PUBACK on acceptance), it is rewritten to the durable behaviour and its count
kept.

## Questions

Answered by Dagda, 2026-11-13 (its design's log section and measurement):
1. Any node may append to a log. The appender sends straight to the log's k
   replicas, which group-commit with their tick's other appends; the
   evidence names each replica that synced. The PUBACK path is one round
   plus the replicas' sync.
2. `Logs::scan` reads only live entries, from an in-memory index on each
   replica. A lease change moving a third of the shards, with each scan
   timed, is in the measurement.
3. One `watch_keys` subscription covers a family by name prefix
   (`lir/retained/`), on Dannan's prefix-subscribed channels.
4. The dot is the owner's, taken as given and ordered by (R4); a repeated dot
   is the same entry. A replica refuses an entry whose dot origin is not the
   appending node, hence the queue entry's own dot (QoS path, point 4).
   Acks and retires pass from any node, subject only to the scope check;
   an ack is identified by its writer, the dot it names and its stage
   (Dagda, 2026-11-13). Who may retire is Lir's rule, kept on Lir's side:
   only the entry's origin, or the shard's lease holder when the origin has
   left the roster.

Answered by Tom, 2026-11-13:
5. Point 4 of the QoS path: an online session acks from memory, and an
   inflight entry lives one client round trip. Accepted.
6. The sequence block: 65,536 a reservation, gaps allowed, never reused.
   Accepted.
7. Retained: an index of topic names on every node, with values read from
   the holders at SUBSCRIBE; no value cache. Accepted.
