#!/usr/bin/env bash
# Run clang-tidy across our app sources.
#
# Requires a previous successful `west build` so that
# build/<image>/compile_commands.json exists. clang-tidy ships with the
# nRF Connect toolchain — make sure you're inside the `ncs` shell or
# have it on PATH.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

# Locate the per-image compile_commands.json (sysbuild puts it under
# build/<image>/, single-image builds under build/).
CDB=""
for candidate in build/nrf52dk/compile_commands.json build/compile_commands.json; do
    if [[ -f "$candidate" ]]; then
        CDB="$candidate"; break
    fi
done
if [[ -z "$CDB" ]]; then
    echo "no compile_commands.json found — run 'west build -b nrf52dk/nrf52832 -p always .' first" >&2
    exit 1
fi
echo "using compile DB: $CDB"

# Lint only OUR app sources, not Zephyr's tree.
SOURCES=(src/main.c src/sys_data.c src/sensor.c src/fan.c
         src/control.c src/control_logic.c src/json_io.c
         src/cmd_interpreter.c src/ble_svc.c)

# A small, opinionated checks bundle: bugprone, performance, portability,
# readability — but not `clang-analyzer-*` (slow) and not the full
# `cert-*` because Zephyr macros trip several false positives.
CHECKS='-*,bugprone-*,performance-*,portability-*,readability-*,
        -readability-magic-numbers,
        -readability-identifier-length,
        -bugprone-easily-swappable-parameters,
        -readability-function-cognitive-complexity'

clang-tidy -p "$(dirname "$CDB")" --checks="$CHECKS" "${SOURCES[@]}"
