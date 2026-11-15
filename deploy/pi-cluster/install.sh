#!/bin/sh
#
# Lir - The Message Broker
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

# Installs, or upgrades, the standing Lir cluster on the Pi build servers,
# run from a checkout on the Mac. The first argument is where a Linux build
# lives; the rest name the nodes, each as NAME=ADDRESS. Every node bootstraps
# from the others.
#
#   deploy/pi-cluster/install.sh root@10.44.1.202:/root/lir-work/build \
#     corvus-jupiter=10.44.1.169 corvus-saturn=10.44.1.170 corvus-neptune=10.44.1.202 \
#     corvus-uranus=10.44.1.228
#
# Every node is named each time, so each bootstraps from all the others;
# LIR_ONLY=<name> installs and restarts that one node alone, for a rolling
# upgrade with a check between nodes.
#
# The cluster key is made once and kept in build/pi-cluster/cluster.key in
# this checkout, never in the repository. Each node runs as tom from
# /home/tom/lir-cluster under the lir-node systemd unit.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
stage="$repo/build/pi-cluster"
build=$1
shift

rm -rf "$stage/bin" "$stage/lib"
mkdir -p "$stage/bin" "$stage/lib"
rsync -a "$build/lir-node" "$stage/bin/"
rsync -a --include='liblir.so*' --include='liblir_mqtt.so*' --exclude='*' "$build/lir/" "$stage/lib/"
rsync -a --include='libdannan.so*' --exclude='*' "$build/_deps/dannan-build/dannan/" "$stage/lib/"
[ -s "$stage/bin/lir-node" ] || { echo "no lir-node in $build" >&2; exit 1; }

if [ ! -s "$stage/cluster.key" ]; then
  (umask 077 && openssl rand -base64 48 > "$stage/cluster.key")
fi

addresses=""
for node in "$@"; do addresses="$addresses ${node#*=}"; done

for node in "$@"; do
  name=${node%%=*}
  address=${node#*=}
  [ -n "${LIR_ONLY:-}" ] && [ "$LIR_ONLY" != "$name" ] && continue
  bootstrap=""
  for other in $addresses; do
    [ "$other" = "$address" ] && continue
    bootstrap="${bootstrap:+$bootstrap, }$other:7537"
  done
  sed -e "s/@NAME@/$name/" -e "s/@ADDRESS@/$address/" -e "s/@BOOTSTRAP@/$bootstrap/" "$here/node.yaml.in" > "$stage/node-$name.yaml"

  echo "== $name ($address)"
  ssh root@"$address" 'install -d -o tom -g tom -m 0750 /home/tom/lir-cluster /home/tom/lir-cluster/bin /home/tom/lir-cluster/lib /home/tom/lir-cluster/state'
  rsync -a --delete "$stage/bin/" root@"$address":/home/tom/lir-cluster/bin/
  rsync -a --delete "$stage/lib/" root@"$address":/home/tom/lir-cluster/lib/
  rsync -a "$stage/cluster.key" root@"$address":/home/tom/lir-cluster/cluster.key
  rsync -a "$stage/node-$name.yaml" root@"$address":/home/tom/lir-cluster/node.yaml
  rsync -a "$here/lir-node.service" root@"$address":/etc/systemd/system/lir-node.service
  ssh root@"$address" 'chown -R tom:tom /home/tom/lir-cluster && chmod 0600 /home/tom/lir-cluster/cluster.key &&
    systemctl daemon-reload && systemctl enable lir-node >/dev/null 2>&1 && systemctl restart lir-node &&
    sleep 2 && systemctl is-active lir-node'
done
