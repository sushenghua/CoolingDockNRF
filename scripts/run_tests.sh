#!/usr/bin/env bash
# Build + run unit tests directly with the host C compiler. Bypasses
# Zephyr/twister entirely because NCS v3.3.0 hard-blocks native_sim
# and unit_testing builds on macOS:
#   - native_sim: zephyr/arch/posix/CMakeLists.txt fails fatal on Darwin
#   - unit_testing: ZTEST macros use ELF section attrs that Mach-O rejects
#
# Tests use the lightweight harness in tests/unit/test_harness.{h,c}
# instead of ztest. Same assertions, same ASan/UBSan/coverage, no
# Zephyr scaffolding.
#
# The persistence integration test still requires Zephyr (it links
# against the real settings + NVS subsystems) and is skipped on macOS.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

CC=${CC:-cc}
COMMON_FLAGS=(
	-O0 -g
	-Wall -Wextra
	-Itests/unit -Isrc
	-fsanitize=address,undefined
	-fno-sanitize-recover=all
	--coverage
)

mkdir -p build
failures=()
ran=0

# unit:  <name> <test source> [extra source ...]
run_unit() {
	local name=$1
	shift

	local out_dir="build/test_${name}"
	rm -rf "$out_dir"
	mkdir -p "$out_dir"

	echo
	echo "════════════════════════════════════════════════════════════"
	echo "  build  unit/${name}"
	echo "════════════════════════════════════════════════════════════"

	# Compile each TU separately into the build dir so .gcno files
	# land next to .o files (lcov groups them per-directory).
	local objs=()
	local sources=("$@" tests/unit/test_harness.c)
	for src in "${sources[@]}"; do
		local obj="$out_dir/$(basename "$src" .c).o"
		"$CC" "${COMMON_FLAGS[@]}" -c "$src" -o "$obj"
		objs+=("$obj")
	done

	"$CC" "${COMMON_FLAGS[@]}" -o "$out_dir/run_test" "${objs[@]}"

	echo
	echo "  run    unit/${name}"
	echo "────────────────────────────────────────────────────────────"
	if "$out_dir/run_test"; then
		echo "  ✅ unit/${name}"
	else
		echo "  ❌ unit/${name}"
		failures+=("unit/${name}")
	fi
	ran=$((ran + 1))
}

run_unit json_io       tests/unit/json_io/src/test_json_io.c        src/json_io.c
run_unit control_logic tests/unit/control_logic/src/test_control_logic.c src/control_logic.c

# Integration test for the cmd → sys_data → settings pipeline.
# Compiled against shimmed Zephyr APIs in
# tests/integration/persistence/fakes/ so it builds on macOS too.
run_integration() {
	local out_dir="build/test_persistence"
	rm -rf "$out_dir"; mkdir -p "$out_dir"

	echo
	echo "════════════════════════════════════════════════════════════"
	echo "  build  integration/persistence"
	echo "════════════════════════════════════════════════════════════"

	local fakes_inc="tests/integration/persistence/fakes"
	local int_flags=(
		-O0 -g -Wall
		-Wno-unused-function   # reboot_work_handler is unreachable with our shim
		-I"$fakes_inc"
		-Itests/unit
		-Isrc
		"-DCONFIG_BT_DEVICE_NAME=\"CoolingDockNRF\""
		"-DCONFIG_BOARD=\"native_test_host\""
		-fsanitize=address,undefined -fno-sanitize-recover=all
		--coverage
	)

	local sources=(
		tests/integration/persistence/src/test_persistence.c
		tests/integration/persistence/src/stubs.c
		tests/integration/persistence/fakes/fakes.c
		src/sys_data.c
		src/json_io.c
		src/cmd_interpreter.c
		tests/unit/test_harness.c
	)
	local objs=()
	for src in "${sources[@]}"; do
		local obj="$out_dir/$(basename "$src" .c).o"
		"$CC" "${int_flags[@]}" -c "$src" -o "$obj"
		objs+=("$obj")
	done
	"$CC" "${int_flags[@]}" -lpthread -o "$out_dir/run_test" "${objs[@]}"

	echo
	echo "  run    integration/persistence"
	echo "────────────────────────────────────────────────────────────"
	if "$out_dir/run_test"; then
		echo "  ✅ integration/persistence"
	else
		echo "  ❌ integration/persistence"
		failures+=("integration/persistence")
	fi
	ran=$((ran + 1))
}

run_integration

echo
echo "════════════════════════════════════════════════════════════"
if [[ ${#failures[@]} -eq 0 ]]; then
	echo "  ✅  $ran suites passed"
	exit 0
else
	echo "  ❌  ${#failures[@]} of $ran suites failed:"
	printf '       - %s\n' "${failures[@]}"
	exit 1
fi
