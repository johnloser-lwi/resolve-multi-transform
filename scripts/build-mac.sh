#!/usr/bin/env bash
# Build MultiTransform.ofx.bundle on macOS: one universal binary for Apple
# Silicon and Intel, then run the three test suites.
#
#   scripts/build-mac.sh            Release
#   scripts/build-mac.sh Debug
#
# Needs: Xcode command line tools (xcode-select --install) and CMake 3.24+
# (brew install cmake). The bundle lands in build-mac/<Config>/.

set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
config="${1:-Release}"
build="$repo/build-mac"

command -v cmake >/dev/null || { echo "cmake not found -- brew install cmake" >&2; exit 1; }
xcode-select -p >/dev/null 2>&1 || { echo "Xcode command line tools missing -- xcode-select --install" >&2; exit 1; }

cmake -S "$repo" -B "$build" -DCMAKE_BUILD_TYPE="$config"
cmake --build "$build" --config "$config" -j "$(sysctl -n hw.ncpu)"

echo
echo "=== Tests ==="
# test_render_parity runs every CPU-vs-GPU check against the Metal path: it is
# the first and most important thing to pass on a new Mac.
status=0
for t in test_anim_engine test_preset test_render_parity; do
    exe="$build/$t"
    [ -x "$exe" ] || exe="$build/$config/$t"
    echo "--- $t"
    out="$("$exe" 2>&1)" || status=1
    printf '%s\n' "$out" | grep -E "FAIL|checks,|PASSED|PARITY|failure" || printf '%s\n' "$out" | tail -3
done

echo
echo "Bundle: $build/$config/MultiTransform.ofx.bundle"
[ "$status" -eq 0 ] || { echo "SOME TESTS FAILED -- see above" >&2; exit 1; }
