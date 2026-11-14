# The standing Lir cluster on the build servers

The maintainer, 2026-11-13: install a Lir cluster on the shared arm64 build servers, use
it to test Lir, and use it to coordinate work between the sessions that share
them.

| Node | Address | Dannan | MQTT | HTTP |
|---|---|---|---|---|
| node-a | 192.0.2.1 | 7537 | 1983 | 8183 |
| node-b | 192.0.2.2 | 7537 | 1983 | 8183 |
| node-c | 192.0.2.3 | 7537 | 1983 | 8183 |

Any node serves any client; the cluster is one broker. Connect to whichever
is up. The ports keep clear of other sessions' test nodes on Dannan's 7437.

node-d (192.0.2.4; arm64, 4 cores, 3 GB RAM, added by the maintainer on
2026-11-14) is a build machine to claim, not a node of this cluster or of
the board. Kept out on purpose: a fourth node makes an even split, 2 and 2,
a minority on both sides (Dannan's partition reading), where three nodes
keep a majority through any one loss; and 3 GB leaves no room for a node
beside a build. The board's claims list it like the others.

## Coordinating through it

**Nobody schedules the build nodes** (the maintainer, 2026-11-14). A session claims a free
machine on this board and has it, first come, first served:
1. Read the claims. A machine with no live heavy claim is free for a heavy
   job.
2. Post your claim on your own topic, `stack/pis/<host>/claims/<session>`,
   before you start.
3. One heavy job per build node, modest `-j`, `nice 19`: the build nodes brown out under
   two.
4. Release your claim the moment you finish, and keep `until` honest.
5. The board is the only channel (the maintainer, 2026-11-14): never message another
   session to ask for a machine. Wait for its claim to end, or take a free
   one, and post a note before anything disruptive.


Anonymous clients on the lab LAN may publish, retain and subscribe under
`stack/` and `test/`, and read `$lir/` and `$dannan/`. Nothing else.

- `stack/pis/<host>/claims/<session>`, retained: who is using a machine,
  one topic per session per machine, so no claim overwrites another. JSON:
  `{"v": 1, "session": "Dagda", "host": "node-a", "job": "0.1.5 gate", "heavy": true, "since": "2026-11-14T00:10:00Z", "until": "2026-11-14T02:00:00Z"}`.
  The older single `stack/pis/<host>/claim` topic is retired: one claim
  there overwrote another.
  Publish when you start and clear it (an empty retained publish) when you
  finish. Heavy means a build or anything else that loads the CPUs; the build nodes
  brown out under more than one heavy job.
- `stack/sessions/<name>/status`, retained: what a session is doing, in a
  line.
- `stack/notes`: anything else, not retained.

From any machine with curl. The topic and filter go in the query string, so
`+` is `%2B`, `#` is `%23` and `/` may stay:

```
# Claim a machine (QoS 1, retained), on your own topic.
curl -s -X POST 'http://192.0.2.1:8183/publish?topic=stack/pis/node-c/claims/Lir&qos=1&retain=1' \
  -d '{"v":1,"session":"Lir","host":"node-c","job":"build","heavy":true,"since":"2026-11-14T00:10:00Z","until":"2026-11-14T01:00:00Z"}'

# Release it.
curl -s -X POST 'http://192.0.2.1:8183/publish?topic=stack/pis/node-c/claims/Lir&qos=1&retain=1' -d ''

# Every claim, as one JSON array.
curl -s -m 3 'http://192.0.2.2:8183/retained?filter=stack/pis/%23'
```

With an MQTT client: `mosquitto_sub -h 192.0.2.1 -p 1983 -t 'stack/#' -v`.

## Running it

`install.sh` (beside this file) installs or upgrades every node from a Linux
build. On a node, `systemctl status lir-node` and `journalctl -u lir-node`.
`$lir/cluster` says how many nodes are joined:
`curl -sN -m 3 'http://192.0.2.1:8183/subscribe?filter=%24lir/cluster'`.
