# Tests

Six-tier test setup mirroring industrial embedded practice.

```
tests/
  unit/                          host-side, sub-second
    json_io/                     ztest, native_sim, ASan + UBSan + gcov
    control_logic/               ztest, native_sim, ASan + UBSan + gcov
  integration/
    persistence/                 ztest, native_sim, real settings + NVS
  hil/
    smoke.py                     bleak; runs against a flashed nRF52 DK
    requirements.txt
scripts/
  run_static_analysis.sh         clang-tidy over compile_commands.json
  run_coverage.sh                lcov + genhtml after twister
```

All tier-1/2 tests are run by Zephyr's [twister](https://docs.zephyrproject.org/latest/develop/test/twister.html). Activate the NCS environment first (`ncs` per the project's bash function), then run from the project root.

> **macOS — Zephyr's test framework is unavailable.** NCS v3.3.0 hard-blocks `native_sim` (`zephyr/arch/posix/CMakeLists.txt:3` — `FATAL_ERROR` on Darwin) AND the `unit_testing` board fails to compile any `ZTEST(...)` macro because Zephyr's iterable-sections feature uses ELF section attributes that Mach-O rejects. Both walls are inherent to NCS v3.3.
>
> The unit tests therefore use a tiny custom harness in `tests/unit/test_harness.{h,c}` (header-only `TEST(name)` + `ASSERT_*` macros, auto-registered via `__attribute__((constructor))`) and compile with `cc` directly. ASan + UBSan + gcov are wired in via `scripts/run_tests.sh`. Same testing power, no Zephyr scaffolding.
>
> The integration test still requires Zephyr's settings + NVS subsystems — it stays Zephyr-based and is automatically skipped on macOS, run on Linux/CI.
>
> Run on any host:
>
> ```sh
> ./scripts/run_tests.sh
> ```
>
> The wrapper invokes `west build -b native_sim` per test and runs the resulting executable directly. Linux users *can* still use the twister commands shown below if they prefer — the twister output format gives nicer summaries.

> **Toolchain override (Linux + macOS).** The NCS env defaults to `ZEPHYR_TOOLCHAIN_VARIANT=zephyr` (ARM cross-compiler) but native_sim needs `host`. The wrapper script and the twister commands below all set it.

## Tier 1 — host unit tests

```sh
ZEPHYR_TOOLCHAIN_VARIANT=host west twister -p native_sim --force-toolchain -T tests/unit
```

Builds and runs `json_io` and `control_logic` suites under AddressSanitizer + UndefinedBehaviorSanitizer, with gcov instrumentation. Each suite finishes in well under a second.

To run just one suite:

```sh
ZEPHYR_TOOLCHAIN_VARIANT=host west twister -p native_sim --force-toolchain -T tests/unit/json_io --inline-logs
```

## Tier 2 — native_sim integration

```sh
ZEPHYR_TOOLCHAIN_VARIANT=host west twister -p native_sim --force-toolchain -T tests/integration
```

`tests/integration/persistence/` boots the real `sys_data` + `cmd_interpreter` + `json_io` modules on top of Zephyr's flash simulator. It exercises the end-to-end `BLE write → cmd_interpreter → sys_data → settings → NVS → reload → state recovered` path, plus negative cases (bad mode rejected without mutating state, unknown command, etc.). The hardware-touching modules (`sensor`, `control`, `ble_svc`) are stubbed in `tests/integration/persistence/src/stubs.c` so the test focuses on the persistence pipeline.

## Tier 3 — HIL smoke

Requires a flashed and powered nRF52 DK plus a host with a working BLE adapter (macOS, Linux with BlueZ, or Windows 10+).

```sh
cd tests/hil
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
python smoke.py
```

The script walks the wire-contract checklist: scan by name prefix, connect (OS triggers just-works pairing on first encrypted read), read `DevInfo` + `PrpConf`, subscribe to `Status`, send `SetPeripheralConfig`, verify `UpdateRet` and that the next `PrpConf` read reflects the change.

Pass `python smoke.py SomeOtherPrefix` to scan for a different name prefix.

If the device is in BONDED_ONLY mode (post-120 s window) and the host hasn't paired before, the connection will time out — hold BUTTON3 on the DK for 5 s to reopen the pairing window.

## Static analysis

```sh
west build -b nrf52dk/nrf52832 -p always .   # generates compile_commands.json
./scripts/run_static_analysis.sh
```

Runs `clang-tidy` against just our `src/*.c` files (not Zephyr internals) with `bugprone-*`, `performance-*`, `portability-*`, and `readability-*` checks enabled.

## Coverage

```sh
./scripts/run_coverage.sh
open coverage/html/index.html        # macOS
```

Builds and runs every native_sim test, captures gcov output, runs lcov to produce `coverage/coverage.info` (filtered to `src/*` only — we don't care about Zephyr's own coverage), and renders an HTML browse-able report if `genhtml` is on `PATH` (`brew install lcov`).

## Sanitizers

ASan + UBSan are enabled in every test's `CMakeLists.txt`. Failures abort the process with a backtrace pointing at the offending source line. No extra flags needed.

## What's NOT tested here

- `sensor.c` — depends on real I2C transactions to a wired SHT3x; covered by HIL smoke only.
- `fan.c` — depends on real PWM hardware; covered by HIL smoke only.
- `ble_svc.c` GATT/adv internals — depend on the BT host, controller, and live RF; mocking them comprehensively would amount to re-implementing them. Covered transitively through HIL smoke.
- `main.c` — three function calls in init order; not worth a test.

This split is intentional: the firmware modules with real bugs in their history (`json_io` parser, `control` hysteresis) get exhaustive host-side testing; modules whose correctness can only be verified against silicon get a single end-to-end smoke check on real hardware.
