#!/bin/sh
#
# Lir - The Message Broker
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#
# Builds the Debian packages (liblir-mqtt<major.minor>, liblir-dev and debug
# symbols) on Debian trixie from a git checkout, natively for the machine's
# architecture, against the installed libdannan-dev. The packages are written
# to build-deb/ (docs/packaging.md).
#
#   sudo apt install dpkg-dev && sudo apt build-dep ./
#   tools/build-deb.sh [dpkg-buildpackage options]
#
# The version comes from `git describe`: the tag 0.1.0 gives 0.1.0, and a build
# three commits later 0.1.0+git3.gabc1234, which sorts after 0.1.0 and before
# 0.1.1; where no tag is reachable (develop), the tree's version and the
# commit, 0.1.0+git.gabc1234. Uncommitted changes add .dirty.
# DEB_BUILD_OPTIONS=nocheck skips the tests. debian/control names the
# library's package for the CMake version's major.minor, the SONAME. As
# Dannan's tools/build-deb.sh has it.

set -eu

top=$(git rev-parse --show-toplevel)
cd "$top"

project=$(sed -nE 's/^project\(lir VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' CMakeLists.txt)

# Release tags sit on main's release merges, which develop never merges back: a
# build where none is reachable is the tree's version and the commit.
if describe=$(git describe --tags --match '[0-9]*.[0-9]*.[0-9]*' --dirty 2>/dev/null); then
  version=$(echo "$describe" | sed -E 's/-([0-9]+)-g([0-9a-f]+)/+git\1.g\2/; s/-dirty$/.dirty/')
else
  describe=$(git describe --always --dirty)
  version="$project+git.g$(echo "$describe" | sed -E 's/-dirty$/.dirty/')"
fi
library=$(sed -n 's/^Package: \(liblir-mqtt[0-9].*\)/\1/p' debian/control)
if [ "$library" != "liblir-mqtt${project%.*}" ]; then
  echo "build-deb.sh: debian/control names $library and CMakeLists.txt says $project" >&2
  exit 1
fi
# A build of a tag is a release, and has to be the version the tree says.
# Between tags the tree may already carry the next one.
case "$version" in
  *+git*) ;;
  "$project" | "$project".dirty) ;;
  *)
    echo "build-deb.sh: the tag says $describe and CMakeLists.txt says $project" >&2
    exit 1
    ;;
esac

# One job per CPU unless the caller says otherwise: on a shared build node, say
# parallel=<the cores claimed>.
case " ${DEB_BUILD_OPTIONS:-} " in
  *" parallel="*) ;;
  *) export DEB_BUILD_OPTIONS="${DEB_BUILD_OPTIONS:+$DEB_BUILD_OPTIONS }parallel=$(nproc)" ;;
esac

cat >debian/changelog <<END
lir ($version) trixie; urgency=medium

  * Built from $describe.

 -- tomdionysus <tomdionysus@users.noreply.github.com>  $(date -R)
END

dpkg-buildpackage -us -uc -b "$@"

mkdir -p build-deb
for f in ../liblir*_"$version"_*.deb ../lir_"$version"_*.buildinfo ../lir_"$version"_*.changes; do
  if [ -e "$f" ]; then mv "$f" build-deb/; fi
done
ls -l build-deb/*"$version"*
