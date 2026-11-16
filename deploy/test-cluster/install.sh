#!/bin/sh
#
# Lir - The Message Broker
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

# Installs, or upgrades, the standing Lir cluster on the build servers,
# run from a checkout on the workstation. The first argument is where a Linux build
# lives; the rest name the nodes, each as NAME=ADDRESS. Every node bootstraps
# from the others.
#
#   deploy/test-cluster/install.sh root@192.0.2.3:/root/lir-work/build \
#     node-a=192.0.2.1 node-b=192.0.2.2 node-c=192.0.2.3 \
#     node-d=192.0.2.4
#
# Every node is named each time, so each bootstraps from all the others;
# LIR_ONLY=<name> installs and restarts that one node alone, for a rolling
# upgrade with a check between nodes. LIR_CONFIG_ONLY=1, with "-" for the
# build, installs only node.yaml and restarts: a configuration change that
# keeps the binaries each node has.
#
# The cluster key is made once and kept in build/test-cluster/cluster.key in
# this checkout, never in the repository. Each node runs as lir from
# /opt/lir-cluster under the lir-node systemd unit.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
stage="$repo/build/test-cluster"
build=$1
shift

config_only=${LIR_CONFIG_ONLY:-}
if [ -z "$config_only" ]; then
  rm -rf "$stage/bin" "$stage/lib"
  mkdir -p "$stage/bin" "$stage/lib"
  rsync -a "$build/lir-node" "$stage/bin/"
  rsync -a --include='liblir.so*' --include='liblir_mqtt.so*' --exclude='*' "$build/lir/" "$stage/lib/"
  rsync -a --include='libdannan.so*' --exclude='*' "$build/_deps/dannan-build/dannan/" "$stage/lib/"
  [ -s "$stage/bin/lir-node" ] || { echo "no lir-node in $build" >&2; exit 1; }
fi

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
  ssh root@"$address" 'install -d -o lir -g lir -m 0750 /opt/lir-cluster /opt/lir-cluster/bin /opt/lir-cluster/lib /opt/lir-cluster/state'
  if [ -z "$config_only" ]; then
    rsync -a --delete "$stage/bin/" root@"$address":/opt/lir-cluster/bin/
    rsync -a --delete "$stage/lib/" root@"$address":/opt/lir-cluster/lib/
  fi
  rsync -a "$stage/cluster.key" root@"$address":/opt/lir-cluster/cluster.key
  rsync -a "$stage/node-$name.yaml" root@"$address":/opt/lir-cluster/node.yaml
  rsync -a "$here/lir-node.service" root@"$address":/etc/systemd/system/lir-node.service
  ssh root@"$address" 'chown -R lir:lir /opt/lir-cluster && chmod 0600 /opt/lir-cluster/cluster.key &&
    systemctl daemon-reload && systemctl enable lir-node >/dev/null 2>&1 && systemctl restart lir-node &&
    sleep 2 && systemctl is-active lir-node'
done
