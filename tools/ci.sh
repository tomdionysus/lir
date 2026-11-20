#!/bin/sh
#
# Lir - The Message Broker
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#
# One CI step on Debian 13, as the nodes have it, in GitHub's runner, a local
# container or a Pi; .github/workflows/ci.yml runs each as its own job.
#
#   tools/ci.sh check      the header block on every source, and clang-format
#   tools/ci.sh release    g++ release build, warnings as errors, and the tests
#   tools/ci.sh client     only lir::mqtt and lir::client (LIR_BUILD_SERVER off), as a consumer builds them
#   tools/ci.sh clang      the same with clang++ (and -Wthread-safety)
#   tools/ci.sh asan       AddressSanitizer and UndefinedBehaviorSanitizer
#   tools/ci.sh tsan       ThreadSanitizer
#   tools/ci.sh coverage   a coverage build's tests; lcov report build-ci/coverage.info
#   tools/ci.sh fuzz       each libFuzzer target (lir/fuzz/) from its seeds, under ASan and UBSan,
#                          LIR_FUZZ_SECONDS each (default 60); a finding lands in build-ci/fuzz/findings/
#
# Builds go under build-ci/ (LIR_CI_OUT to move it), one job per CPU
# (LIR_CI_JOBS to set it: 2 on the shared Pis). Dannan is fetched at the
# pinned tag from the sibling checkout when there is one, else from its public
# repository (LIR_DANNAN_REPOSITORY to name another). Needs g++ cmake
# ninja-build libboost-dev libssl-dev libyaml-cpp-dev libgtest-dev openssl
# mosquitto-clients, and for its step clang, clang-format or lcov.

set -eu

top=$(cd "$(dirname "$0")/.." && pwd)
out=${LIR_CI_OUT:-$top/build-ci}
jobs=${LIR_CI_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}
step=${1:-}
if [ -z "${LIR_DANNAN_REPOSITORY:-}" ]; then
  if [ -d "$top/../dannan/.git" ]; then
    LIR_DANNAN_REPOSITORY=$top/../dannan
  else
    LIR_DANNAN_REPOSITORY=https://github.com/tomdionysus/dannan.git
  fi
fi

# Warnings are errors in every build but the sanitizers': instrumented, g++
# warns inside libstdc++ and Asio, not in Lir. The release and clang steps hold
# the warning gate.
configure() {
  dir="$1"
  shift
  cmake -S "$top" -B "$out/$dir" -G Ninja -DLIR_BUILD_TESTING=ON -DLIR_WARNINGS_AS_ERRORS=ON -DLIR_DANNAN_REPOSITORY="$LIR_DANNAN_REPOSITORY" "$@"
  cmake --build "$out/$dir" -j "$jobs"
}

case "$step" in
  check)
    cd "$top"
    missing=0
    for f in $(git ls-files '*.h' '*.cpp' '*.h.in'); do
      if ! head -3 "$f" | grep -qx '// Lir - The Message Broker'; then
        echo "no header block: $f"
        missing=1
      fi
    done
    [ "$missing" = 0 ]
    git ls-files '*.h' '*.cpp' | xargs clang-format --dry-run -Werror
    ;;
  release)
    configure release -DCMAKE_BUILD_TYPE=Release
    "$out/release/lir_tests"
    ;;
  client)
    configure client -DCMAKE_BUILD_TYPE=Release -DLIR_BUILD_SERVER=OFF
    ;;
  clang)
    CXX=clang++ configure clang -DCMAKE_BUILD_TYPE=Release
    "$out/clang/lir_tests"
    ;;
  asan)
    flags="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined"
    configure asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLIR_WARNINGS_AS_ERRORS=OFF -DCMAKE_CXX_FLAGS="$flags" -DCMAKE_EXE_LINKER_FLAGS="$flags" -DCMAKE_SHARED_LINKER_FLAGS="$flags"
    ASAN_OPTIONS=detect_stack_use_after_return=1 UBSAN_OPTIONS=print_stacktrace=1 "$out/asan/lir_tests"
    ;;
  tsan)
    flags="-fsanitize=thread -fno-omit-frame-pointer"
    configure tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLIR_WARNINGS_AS_ERRORS=OFF -DCMAKE_CXX_FLAGS="$flags" -DCMAKE_EXE_LINKER_FLAGS="$flags" -DCMAKE_SHARED_LINKER_FLAGS="$flags"
    TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1" "$out/tsan/lir_tests"
    ;;
  coverage)
    configure coverage -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS=--coverage -DCMAKE_EXE_LINKER_FLAGS=--coverage -DCMAKE_SHARED_LINKER_FLAGS=--coverage
    find "$out/coverage" -name '*.gcda' -delete
    (cd "$out/coverage" && ./lir_tests)
    ignore=inconsistent,inconsistent,mismatch,negative,unused
    lcov --quiet --capture --directory "$out/coverage" --ignore-errors "$ignore" --output-file "$out/all.info"
    lcov --quiet --extract "$out/all.info" "$top/lir/*" --ignore-errors "$ignore" --output-file "$out/library.info"
    lcov --quiet --remove "$out/library.info" "$top/lir/test/*" --ignore-errors "$ignore" --output-file "$out/coverage.info"
    rm "$out/all.info" "$out/library.info"
    lcov --summary "$out/coverage.info" --ignore-errors "$ignore"
    ;;
  fuzz)
    # Only lir::mqtt: the parsers of what clients send.
    CXX=clang++ configure fuzz -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLIR_BUILD_TESTING=OFF -DLIR_BUILD_SERVER=OFF -DLIR_BUILD_FUZZING=ON
    mkdir -p "$out/fuzz/findings"
    # The targets side by side, one process each: libFuzzer's -fork mode
    # leaks in its own driver, which would hide a leak in Lir's code.
    pids=""
    for target in packet properties topic; do
      # The seeds are read, never written: what the fuzzer finds goes to a
      # corpus of its own in the build tree.
      mkdir -p "$out/fuzz/corpus/$target"
      "$out/fuzz/lir_fuzz_$target" -max_total_time="${LIR_FUZZ_SECONDS:-60}" -max_len=4096 -timeout=10 \
        -artifact_prefix="$out/fuzz/findings/$target-" "$out/fuzz/corpus/$target" "$top/lir/fuzz/corpus/$target" \
        > "$out/fuzz/$target.log" 2>&1 &
      pids="$pids $!"
    done
    failed=0
    for pid in $pids; do
      wait "$pid" || failed=1
    done
    for target in packet properties topic; do
      echo "== $target"
      tail -n 3 "$out/fuzz/$target.log"
    done
    if [ "$failed" != 0 ]; then
      grep -h -A 30 '^==[0-9]*==ERROR\|deadly signal\|uncaught exception' "$out"/fuzz/*.log || true
      exit 1
    fi
    ;;
  *)
    echo "usage: tools/ci.sh check|release|client|clang|asan|tsan|coverage|fuzz" >&2
    exit 2
    ;;
esac
