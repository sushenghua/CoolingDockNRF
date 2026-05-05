#!/usr/bin/env bash
# Build + run every native_sim test with coverage instrumentation,
# then merge the .gcda files into an lcov report and (if genhtml is on
# PATH) render an HTML browse-able tree.
#
# Output:
#   coverage/coverage.info    raw lcov data
#   coverage/html/            browseable report (open index.html)
#
# Sanitizers + coverage flags are already wired into each test's
# CMakeLists.txt — this just orchestrates build, run, and lcov.
#
# We invoke `west build` per-test instead of `west twister` because
# NCS v3.3.0's twister has a hardcoded "native_sim requires Linux"
# guard that strips macOS hosts.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

export ZEPHYR_TOOLCHAIN_VARIANT=host

OUT="coverage"
rm -rf "$OUT"
mkdir -p "$OUT"

# Build + run all tests; `run_tests.sh` writes .gcda files into each
# build/test_*/ directory as a side effect of executing the binaries.
"$PROJECT_ROOT/scripts/run_tests.sh"

echo
echo "==> capturing coverage"
lcov --capture \
     --directory "$PROJECT_ROOT/build" \
     --output-file "$OUT/coverage.raw" \
     --rc geninfo_unexecuted_blocks=1 \
     --ignore-errors gcov,source,unused

# Strip everything outside our src/ tree — we don't care about Zephyr
# kernel/driver coverage in our reports.
lcov --extract "$OUT/coverage.raw" "*/src/*" \
     --output-file "$OUT/coverage.info" \
     --ignore-errors unused

lcov --summary "$OUT/coverage.info"

if command -v genhtml >/dev/null 2>&1; then
    echo "==> rendering HTML"
    genhtml "$OUT/coverage.info" \
            --output-directory "$OUT/html" \
            --title "CoolingDock-NRF coverage" \
            --legend --show-details
    echo "open $OUT/html/index.html"
else
    echo "(genhtml not on PATH — install lcov to get the HTML report)"
fi
