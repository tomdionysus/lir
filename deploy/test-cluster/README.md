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

## Coordinating through it

Anonymous clients on the lab LAN may publish, retain and subscribe under
`stack/` and `test/`, and read `$lir/` and `$dannan/`. Nothing else.

- `stack/pis/<host>/claim`, retained: who is using a machine. JSON:
  `{"session": "Dagda", "job": "0.1.5 gate", "heavy": true, "since": "2026-11-14T00:10Z", "until": "about 02:00Z"}`.
  Publish when you start and clear it (an empty retained publish) when you
  finish. Heavy means a build or anything else that loads the CPUs; the build nodes
  brown out under more than one heavy job.
- `stack/sessions/<name>/status`, retained: what a session is doing, in a
  line.
- `stack/notes`: anything else, not retained.

From any machine with curl. The topic and filter go in the query string, so
`+` is `%2B`, `#` is `%23` and `/` may stay:

```
# Claim a machine (QoS 1, retained).
curl -s -X POST 'http://192.0.2.1:8183/publish?topic=stack/pis/node-c/claim&qos=1&retain=1' \
  -d '{"session":"Lir","job":"build","heavy":true,"since":"2026-11-14T00:10Z"}'

# Release it.
curl -s -X POST 'http://192.0.2.1:8183/publish?topic=stack/pis/node-c/claim&qos=1&retain=1' -d ''

# See every claim: retained messages arrive first, then live ones. -m ends it.
curl -sN -m 3 'http://192.0.2.2:8183/subscribe?filter=stack/pis/%2B/claim'
```

With an MQTT client: `mosquitto_sub -h 192.0.2.1 -p 1983 -t 'stack/#' -v`.

## Running it

`install.sh` (beside this file) installs or upgrades every node from a Linux
build. On a node, `systemctl status lir-node` and `journalctl -u lir-node`.
`$lir/cluster` says how many nodes are joined:
`curl -sN -m 3 'http://192.0.2.1:8183/subscribe?filter=%24lir/cluster'`.
