# CoolingDock on Zephyr / nRF52

Zephyr / nRF Connect SDK firmware for the **Nordic nRF52832 DK** that re-implements the local-control feature set of the original ESP32-C3 **CoolingDock** product — wire-compatible with the existing Capacitor / React frontend (same UUIDs, JSON shapes, MTU), so the app runs against this firmware unmodified.

Network features (WiFi, HTTP, MQTT, OTA) are out of scope for this port. Only BLE control + local sensor sampling + fan PWM are implemented.

[![Bench setup: nRF52 DK + SHT3x + PPK2 + oscilloscope](doc/assets/nrf_board_ppk2_oscope.jpg)](doc/assets/nrf_board_ppk2_oscope.jpg)

## What this repo demonstrates

- **Zephyr / nRF Connect SDK port of an ESP32 product** — kernel API, devicetree, Kconfig, west, with explicit init ordering across `bt_enable`, the settings subsystem, and the I2C driver.
- **BLE peripheral** — LE Secure Connections, encrypted GATT characteristics, persistent bonds via Zephyr settings / NVS, and a pairing-window state machine (120 s open ↔ bonded-only via Filter Accept List, with a button-held-5 s recovery from soft-brick).
- **Multi-tier test pyramid** — host-based unit tests, integration tests with kernel / NVS / `nrfx` fakes, and a Python hardware-in-loop smoke test.
- **Power profiling with the Nordic PPK2** — diagnostic walkthrough in [`doc/ppk2_profile.md`](doc/ppk2_profile.md). Baseline idle current dropped from 1.45 mA to 290–570 µA depending on BLE state (≈ −74 % at the typical post-pairing State 3 reading of 381 µA), after identifying the UART driver as the dominant idle-current source.

## Quick start

Build, flash, monitor:

```sh
west build -b nrf52dk/nrf52832 -p always .
west flash
nrfutil device monitor                           # 115200 8N1, J-Link VCOM
```

Power-profile build (silent serial, lower baseline — see [`doc/ppk2_profile.md`](doc/ppk2_profile.md) for the rationale):

```sh
west build -b nrf52dk/nrf52832 -p always . -- -DEXTRA_CONF_FILE=power_profile.conf
west flash
```

## Run tests

Host-based unit + integration tests:

```sh
./scripts/run_tests.sh
```

Coverage and static-analysis variants:

```sh
./scripts/run_coverage.sh
./scripts/run_static_analysis.sh
```

Hardware-in-loop smoke test (firmware flashed, board paired):

```sh
python -m venv .venv && source .venv/bin/activate
pip install -r tests/hil/requirements.txt
python tests/hil/smoke.py
```

See [`tests/README.md`](tests/README.md) for the rationale behind each tier and the trade-offs that led to a `cc`-based harness instead of full Zephyr / twister on macOS.

## Source layout

| Path | Contents |
|---|---|
| `src/` | Application threads (`sensor`, `control`, `ble_svc`), `cmd_interpreter`, `json_io`, `sys_data` |
| `boards/nrf52dk_nrf52832.overlay` | Hardware wiring (I2C, PWM, GPIO) |
| `prj.conf` | Kconfig — Bluetooth, sensor, PWM, NVS, settings |
| `power_profile.conf` | Build overlay for PPK2 measurement (drops the UART driver) |
| `tests/unit/` | Host-runnable unit tests for `json_io` and `control_logic` |
| `tests/integration/persistence/` | Settings / NVS lifecycle test with kernel, reboot, and `nrfx` fakes |
| `tests/hil/` | Python BLE smoke test that drives the real device |
| `scripts/` | Test, coverage, and static-analysis runners |
| `doc/` | Architecture walkthrough, testing concepts, PPK2 setup guide |

## Documentation

- [`CLAUDE.md`](CLAUDE.md) — full project architecture, build / flash commands, BLE wire contract, pairing-window state machine, bring-up notes
- [`doc/learn.md`](doc/learn.md) — codebase walkthrough across six layers (big picture → build → modules → BLE protocol → boot → end-to-end flow)
- [`doc/learn_test.md`](doc/learn_test.md) — testing concepts (Unity vs Twister, smoke tests, HIL)
- [`doc/ppk2_profile.md`](doc/ppk2_profile.md) — Power Profiler Kit II setup and the diagnostic loop that landed the ≈ 74 % baseline-current reduction, plus measured per-state averages with screenshots

## Hardware

- Nordic **nRF52 DK** (PCA10040, nRF52832 — Cortex-M4 @ 64 MHz, BLE 5)
- **SHT3x** temperature / humidity sensor on Arduino A4/A5 (I2C0)
- PWM fan + separate GPIO power gate on Arduino D2/D3
- (Optional) **Nordic PPK2** — Power Profiler Kit II — for current measurements, wired in Ampere mode at the DK's P22 header (SB9 cut)

### Setup gallery

PPK2 inserted in series at P22 for current measurement (Ampere mode), with the SHT3x sensor on the I2C header:

[![nRF52 DK + PPK2](doc/assets/nrf_board_ppk2.jpg)](doc/assets/nrf_board_ppk2.jpg)

Adding an oscilloscope tap so PWM transitions and BLE radio bursts can be correlated with the PPK2 current trace:

[![nRF52 DK + PPK2 + scope](doc/assets/nrf_board_ppk2_oscope.jpg)](doc/assets/nrf_board_ppk2_oscope.jpg)

Short clip showing the **closed-loop sensor → control → fan path live**: pinching the SHT3x sensor between two fingers warms it (body heat). The sensor thread picks up the higher reading, the control loop (sensor mode, hysteresis with linear ramp from `thr1` to `thr2`) bumps up the target PWM duty, and the oscilloscope shows the PWM waveform's duty cycle widening in real time. Release the sensor and the temperature falls back, PWM duty drops with it. The PPK2 trace alongside shows the BLE radio events superimposed on the slowly-changing background current. Click play to stream:

<!-- Video source is a GitHub user-attachment URL rather than the
     in-repo file at doc/assets/nrf_board_ppk2_oscope.mp4 — github.com's
     Content-Security-Policy excludes raw.githubusercontent.com from
     media-src, so an in-repo <video> source won't play on the repo
     home page. The user-attachments domain IS on the allow-list. The
     in-repo copy is kept for versioning + offline access. -->
<video controls preload="none" width="800" poster="doc/assets/nrf_board_ppk2_oscope.jpg">
  <source src="https://github.com/user-attachments/assets/cb884f7a-21bd-4922-9ae0-8e61d6cf00b9" type="video/mp4">
  Your browser does not support inline video. <a href="doc/assets/nrf_board_ppk2_oscope.mp4">Download the video</a> instead.
</video>

The video and screenshots are tracked via Git LFS — see [`.gitattributes`](.gitattributes). If the video doesn't play on a deployed GitHub Pages site, verify the deploy workflow checks out LFS (`actions/checkout@v4` with `lfs: true`).

## License

[MIT](LICENSE).
