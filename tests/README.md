# Tests

A six-tier test setup.

```
tests/
  unit/                                host-side, sub-second
    test_harness.{h,c}                 lightweight TEST + ASSERT_* macros
    json_io/src/test_json_io.c         24 cases
    control_logic/src/test_control_logic.c  11 cases
  integration/
    persistence/
      src/{test_persistence,stubs}.c   8 cases — full cmd → sys_data → settings path
      fakes/                           Zephyr API shims (settings, kernel, log, FICR)
  hil/
    smoke.py                           bleak; runs against a flashed nRF52 DK
    requirements.txt
scripts/
  run_tests.sh                         build + run all unit + integration suites
  run_static_analysis.sh               clang-tidy over compile_commands.json
  run_coverage.sh                      lcov + genhtml after run_tests.sh
```

> **Why we don't use Zephyr's `twister` for tiers 1-2.** NCS v3.3.0 hard-blocks `native_sim` on macOS at the arch level (`zephyr/arch/posix/CMakeLists.txt:3` raises `FATAL_ERROR` on Darwin), and its `unit_testing` board can't compile any `ZTEST(...)` macro because Zephyr's iterable-sections feature uses ELF section attributes that Mach-O rejects. Both blocks are in NCS v3.3 itself.
>
> The workaround:
>
> - **Unit tests** use a tiny custom harness (`tests/unit/test_harness.{h,c}`): header-only `TEST(name)` + `ASSERT_*` macros, auto-registered via `__attribute__((constructor))`. Compiled with `cc` directly, no Zephyr.
> - **Integration test** compiles the *real* `sys_data` + `cmd_interpreter` + `json_io` production code against thin Zephyr-API shims in `tests/integration/persistence/fakes/` (in-memory settings backend, pthread mutexes, no-op work queue, fixed FICR). The production source files are untouched; only the slice of Zephyr API they consume is faked.
>
> This covers the same ground as ztest and twister without the Zephyr scaffolding, and it runs anywhere `cc` does.

## Tier 1 + 2 — unit + integration (host)

```sh
./scripts/run_tests.sh
```

Builds and runs all three suites under **AddressSanitizer + UndefinedBehaviorSanitizer + gcov**. Each suite is its own native binary in `build/test_<name>/run_test`. Per-test PASS/FAIL, summary at the end.

| Suite | Cases | What it covers |
|---|---|---|
| `unit/json_io` | 24 | All 7 functions in `src/json_io.c`; regression tests for the negative-temp sign, key-vs-value parser confusion, and escape passthrough bugs |
| `unit/control_logic` | 11 | All four hysteresis bands + cycle phase logic in `src/control_logic.c` |
| `integration/persistence` | 8 | End-to-end `BLE write → cmd_interpreter → sys_data → settings → reload → state survives` against the real production code |

Total wall time on a modest Mac: ~3 s.

## Tier 3 — HIL smoke (real hardware)

Requires a flashed and powered nRF52 DK plus a host with a working BLE adapter (macOS, Linux with BlueZ, or Windows 10+).

```sh
cd tests/hil
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
python smoke.py
```

Walks the wire-contract checklist: scan by name prefix → connect (OS triggers just-works pairing on first encrypted read) → read `DevInfo` + `PrpConf` → subscribe to `Status` → send `SetPeripheralConfig` → verify `UpdateRet` and that the next `PrpConf` read reflects the change.

Pass `python smoke.py SomeOtherPrefix` to scan for a different name prefix.

If the device is in BONDED_ONLY mode (post-120 s window) and the host hasn't paired before, the connection will time out. Hold BUTTON3 on the DK for 5 s to reopen the pairing window.

## Tier 4 — static analysis

```sh
west build -b nrf52dk/nrf52832 -p always .   # generates build/nrf52dk/compile_commands.json
./scripts/run_static_analysis.sh
```

Runs `clang-tidy` against just `src/*.c` (not Zephyr internals) with `bugprone-*`, `performance-*`, `portability-*`, and `readability-*` checks enabled. The build itself doesn't need hardware; only `west flash` does.

## Tier 5 — sanitizers

ASan + UBSan are baked into the compile + link flags inside `scripts/run_tests.sh`:

```
-fsanitize=address,undefined -fno-sanitize-recover=all
```

Failures abort the test process with a backtrace pointing at the offending source line. No separate command is needed: every `./scripts/run_tests.sh` run is a sanitizer run.

## Tier 6 — coverage

```sh
brew install lcov            # if not installed
./scripts/run_tests.sh       # produces .gcda files alongside .o
./scripts/run_coverage.sh    # captures, filters to src/*, renders HTML
open coverage/html/index.html
```

Filtered to `src/*` only, since Zephyr and harness coverage aren't interesting here.

## What's NOT tested here

- `sensor.c` — depends on real I2C transactions to a wired SHT3x; covered by HIL smoke only.
- `fan.c` — depends on real PWM hardware; covered by HIL smoke only.
- `ble_svc.c` GATT/adv internals — depend on the BT host, controller, and live RF; mocking them comprehensively would amount to re-implementing them. Covered transitively through HIL smoke.
- `main.c` — three function calls in init order; not worth a test.

This split is intentional: the firmware modules with real bugs in their history (`json_io` parser, `control` hysteresis) get exhaustive host-side testing; modules whose correctness can only be verified against silicon get a single end-to-end smoke check on real hardware.

## Linux / CI alternative path

The `tests/integration/persistence/` directory still includes `CMakeLists.txt`, `prj.conf`, and `testcase.yaml` so the same source can be run through Zephyr's twister + ztest on Linux:

```sh
ZEPHYR_TOOLCHAIN_VARIANT=host west twister -p native_sim --force-toolchain -T tests/integration
```

The Linux path uses Zephyr's real `settings_nvs` backend on top of the flash simulator (rather than the in-memory shim), so it's a slightly stronger integration check. Useful as a CI sanity net even when local dev runs the host path.
