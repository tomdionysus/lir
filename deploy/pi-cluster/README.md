# The standing Lir cluster on the Pi build servers

Tom, 2026-11-13: install a Lir cluster on the shared Pi 5 build servers, use
it to test Lir, and use it to coordinate work between the sessions that share
them.

| Node | Address | Dannan | MQTT | HTTP |
|---|---|---|---|---|
| corvus-jupiter | 10.44.1.169 | 7537 | 1983 | 8183 |
| corvus-saturn | 10.44.1.170 | 7537 | 1983 | 8183 |
| corvus-neptune | 10.44.1.202 | 7537 | 1983 | 8183 |

Any node serves any client; the cluster is one broker. Connect to whichever
is up. The ports keep clear of other sessions' test nodes on Dannan's 7437.

corvus-uranus (10.44.1.228; arm64, 4 cores, 3 GB RAM, added by Tom on
2026-11-14) is a build machine to claim, not a node of this cluster or of
the board. Kept out on purpose: a fourth node makes an even split, 2 and 2,
a minority on both sides (Dannan's partition reading), where three nodes
keep a majority through any one loss; and 3 GB leaves no room for a node
beside a build. The board's claims list it like the others.

## Coordinating through it

A session claims cores on a Pi on this board (claim board 0.2.0,
2026-11-14), first come, first served, with the claim-board plugin's
tools rather than by hand:
1. Read the claims. Each Pi hands out 4 cores.
2. Claim the cores the job needs (its `-j`), `all` for a full build, 0 for
   light work, before you start. Low-priority work takes 1 or 2 under
   `nice 19` and shares the machine.
3. When no machine has the cores, wait in the queue
   (`claim-board wait <host...|--any> --cores <n|all> --job "..." --for 2h`,
   in the background; it exits once the claim is yours). The queue is run
   by `claim-board-scheduler.service` beside `lir-node` on jupiter, saturn
   and neptune; it only reads and writes the board.
4. Release your claim the moment you finish, and keep `until` honest.
5. A session may also ask a machine's holder to make room (Tom,
   2026-11-14, reversing the earlier board-only rule). Post a note before
   anything disruptive.


Anonymous clients on the lab LAN may publish, retain and subscribe under
`stack/` and `test/`, and read `$lir/` and `$dannan/`. Nothing else.

- `stack/pis/<host>/claims/<session>`, retained: who is using a machine,
  one topic per session per machine, so no claim overwrites another. JSON,
  version 2:
  `{"v": 2, "session": "Dagda", "host": "corvus-jupiter", "job": "0.1.5 gate", "cores": 2, "since": "2026-11-14T00:10:00Z", "until": "2026-11-14T02:00:00Z", "note": ""}`,
  `cores` a count or `"all"`. Version 1 (`"heavy": true|false`) is still
  read. The older single `stack/pis/<host>/claim` topic is retired.
  Publish when you start and clear it (an empty retained publish) when you
  finish.
- `stack/waits/<session>`, retained: the queue, a session waiting for
  cores.
- `stack/scheduler/<node>`, retained: each scheduler's heartbeat.
- `stack/sessions/<name>/status`, retained: what a session is doing, in a
  line.
- `stack/notes`: anything else, not retained.

From any machine with curl. The topic and filter go in the query string, so
`+` is `%2B`, `#` is `%23` and `/` may stay:

```
# Claim a machine (QoS 1, retained), on your own topic.
curl -s -X POST 'http://10.44.1.169:8183/publish?topic=stack/pis/corvus-neptune/claims/Lir&qos=1&retain=1' \
  -d '{"v":2,"session":"Lir","host":"corvus-neptune","job":"build","cores":2,"since":"2026-11-14T00:10:00Z","until":"2026-11-14T01:00:00Z","note":""}'

# Release it.
curl -s -X POST 'http://10.44.1.169:8183/publish?topic=stack/pis/corvus-neptune/claims/Lir&qos=1&retain=1' -d ''

# Every claim, as one JSON array.
curl -s -m 3 'http://10.44.1.170:8183/retained?filter=stack/pis/%23'
```

With an MQTT client: `mosquitto_sub -h 10.44.1.169 -p 1983 -t 'stack/#' -v`.

## Running it

`install.sh` (beside this file) installs or upgrades every node from a Linux
build. On a node, `systemctl status lir-node` and `journalctl -u lir-node`.
`$lir/cluster` says how many nodes are joined:
`curl -sN -m 3 'http://10.44.1.169:8183/subscribe?filter=%24lir/cluster'`.
