# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Zephyr/NCS firmware on the Nordic nRF52832 DK that re-implements the local-control feature set of the original **CoolingDock** product (ESP32-C3, ESP-IDF) and is **wire-compatible with its Capacitor/React frontend** — same UUIDs, JSON shapes, and MTU, so the existing app runs against this firmware unmodified. Network features (WiFi, HTTP, MQTT, OTA) are out of scope for this port; only BLE control + local sensor + fan loop are implemented.

## Environment
- nRF Connect SDK installed by VS Code extension at `/opt/nordic/ncs`
- SDK version: v3.3.0 (check `ls /opt/nordic/ncs`)
- Toolchain: `/opt/nordic/ncs/toolchains/<hash>`

## Build / flash / monitor

```sh
west build -b nrf52dk/nrf52832 -p always .
west flash
nrfutil device monitor                         # 115200 8N1, J-Link VCOM
```

Single-test rebuild without pristine: drop `-p always`. Clean: `rm -rf build`.

## Hardware wiring (board overlay)

`boards/nrf52dk_nrf52832.overlay` wires:

| Function          | nRF Pin | Header     | Notes                  |
|-------------------|---------|------------|------------------------|
| SHT3x SCL         | P0.27   | Arduino A5 | I2C0, 100 kHz          |
| SHT3x SDA         | P0.26   | Arduino A4 | addr 0x45 (ADDR pin high; tie ADDR to GND for 0x44) |
| Fan PWM           | P0.13   | Arduino D2 | `&pwm0` ch0 @ 22 kHz   |
| Fan power gate    | P0.14   | Arduino D3 | active-high GPIO out   |

The SHT3x must be physically wired; `device_is_ready()` will fail and the sensor thread will exit if not present. Fan PWM uses the nRF hardware PWM peripheral via Zephyr's `pwm_dt_spec` API (under the hood: `nrfx_pwm`).

## Architecture

Five application threads dynamically spawned from `main()` (Style B — explicit ordering, since `bt_enable()` and the SHT3x driver have init dependencies on `settings_subsys_init()` and the I2C driver respectively):

| Thread       | File         | Period   | Job |
|--------------|--------------|----------|-----|
| `sensor`     | `sensor.c`   | 500 ms   | SHT3x temp+humidity sample loop |
| `control`    | `control.c`  | 1 s      | per-mode fan duty calc + PWM/GPIO drive |
| `ble_notify` | `ble_svc.c`  | 500 ms   | pushes status JSON via STATUS notify when subscribed |
| `ble_cmd`    | `ble_svc.c`  | event    | drains `cmd_q` msgq, dispatches to `cmd_interpreter`, replies via STATUS notify |
| (BT host)    | (Zephyr)     | event    | spawned internally by `bt_enable()` |

`main()` runs once and returns; its thread terminates but the others keep running.

### Module boundaries

- `sys_data` — minimal persistent state (`master_switch`, `device_name`, `prf[1]` config). Stored under settings keys `app/ms`, `app/name`, `app/p0` via Zephyr's settings subsystem (NVS backend). Per-key save on each setter; defaults baked into the static initializer so threads can read state safely before `sys_data_init()` runs. Only fields the frontend actually sends/reads are persisted — no WiFi/MQTT/alert/token state.
- `sensor` — Zephyr generic sensor API (`sensor_sample_fetch` / `sensor_channel_get`) over `DT_ALIAS(sht3x)`. Latest reading guarded by mutex; `sensor_get(struct sensor_reading *)` is the only accessor.
- `fan` — thin PWM/GPIO wrapper. `fan_set_percent(0..100)` translates to nanoseconds against the DT-declared period; `fan_power_set(bool)` toggles the gate GPIO.
- `control` — pulls `prf_cfg` + sensor reading + master flag every 1 s, computes target duty per mode (POWER/FIXED/SENSOR/CYCLE/SCHEDULE), drives the fan. Master-off forces fan off regardless of mode. SCHEDULE persists but does nothing without an RTC.
- `json_io` — hand-rolled snprintf formatters (`jsf_*`) and key-lookup parser (`jsp_*`). Replaces Zephyr's descriptor-based `<zephyr/data/json.h>` because the frontend's command schema has conditional-by-mode fields that don't fit a fixed descriptor.
- `cmd_interpreter` — transport-agnostic dispatcher. `cmd_interpreter_dispatch(json_in, len, out, cap)` switches on the `cmd` field and produces a response JSON. Same entry point will work for any future transport (UART shell, etc.).
- `ble_svc` — GATT service definition, advertising, connection lifecycle, status notify thread, command worker thread. Command writes are accumulated by a brace-counting state machine until a complete top-level JSON object lands, then handed off via `cmd_q` msgq so the BT RX context never blocks on flash writes.

### Wire contract (must match frontend exactly)

- Service UUID `59462f12-9543-9999-12c8-58b459a2712d`
- Characteristics (all encrypted via `BT_GATT_PERM_*_ENCRYPT`):
  - `…0001` Status — READ + NOTIFY (status JSON, also carries UpdateRet replies)
  - `…0002` Command — WRITE (chunked JSON in)
  - `…0004` Device Info — READ
  - `…0005` Peripheral Sets — READ
- **No `…0003` characteristic.** The frontend never subscribes to it; UpdateRet replies are sent over the STATUS notify channel and demuxed client-side by the `cmd` field.
- Device name is `CoolingDockNRF`. Starts with `CoolingDock` so it still satisfies the React frontend's name-prefix scan filter, but the `NRF` suffix disambiguates from the original ESP32-based CoolingDock board if both are powered on simultaneously. The HIL smoke test (`tests/hil/smoke.py`) scans for the more specific `CoolingDockNRF` prefix to avoid accidentally connecting to the ESP32 unit.
- MTU enlarged to 247 (`CONFIG_BT_L2CAP_TX_MTU`) so 200-byte frontend chunks fit in single L2CAP frames.
- LE Secure Connections, just-works, bondable. Bond stored via `CONFIG_BT_SETTINGS=y`. The frontend never calls a pair API; the OS BLE stack triggers pairing automatically on first encrypted read.

### Pairing window protocol (`ble_svc.c`)

Two advertising modes: `ADV_OPEN` (any client) and `ADV_BONDED_ONLY` (Filter Accept List restricts incoming connections to previously-bonded peers). State machine:

| Event | Transition |
|---|---|
| Boot | mode = OPEN, schedule 120 s timer, start adv |
| `pairing_complete(bonded=true)` | mode = BONDED_ONLY, cancel timer (locked in) |
| 120 s timer fires (no pairing happened) | mode = BONDED_ONLY (FAL may be empty → no one can connect) |
| BUTTON3 (P0.15) held ≥ 5 s | mode = OPEN, reschedule 120 s timer, re-arm adv |
| Disconnect | re-arm adv in whatever mode is current |

Implementation pieces in `ble_svc.c`:
- `adv_mode_v` (`atomic_t`) — the OPEN / BONDED_ONLY flag
- `pairing_window_work` — `k_work_delayable`, fires after 120 s
- `auth_info_cb.pairing_complete` — Zephyr's bond-completion callback
- `fal_repopulate()` — `bt_le_filter_accept_list_clear` + `bt_foreach_bond` → `bt_le_filter_accept_list_add`
- `pairing_btn` GPIO with edge-both interrupt + a `btn_hold_work` deferred 5 s; cancelled if button released early
- `start_advertising()` — single helper; checks current mode, repopulates FAL if needed, starts with `ADV_OPEN_PARAM` or `ADV_BONDED_ONLY_PARAM`

**Soft-brick recovery**: if the device boots fresh with no bond and no client pairs within 120 s, advertising stops (FAL is empty). Hold BUTTON3 for 5 s to reopen the window. If the button is unavailable (e.g., overlay alias missing), only a chip-erase + reflash recovers — `west flash --erase` does both.

### SHT3x driver mode + soft-reset wrapper (`sensor.c`)

Two interrelated SHT3x quirks specific to NCS v3.3 / Zephyr 4.x's `sht3xd` driver:

1. **Driver default measurement mode.** Zephyr defaults to **periodic** mode at MPS=1 (one measurement per second). Each `sensor_sample_fetch` issues `FETCH_DATA` (0xE000) and reads 6 bytes from the chip's measurement register. Per the SHT3x datasheet, *if no measurement is ready when the read header is sent, the chip NACKs the read* — Zephyr surfaces that NACK as `-EIO` (`-5`). With our 500 ms sample period vs the chip's 1 Hz measurement rate, this would naively give ~50 % failure; in practice we hit 100 % because the periodic-mode init is fragile (see below). **Fix**: `CONFIG_SHT3XD_SINGLE_SHOT_MODE=y` in `prj.conf`. Each sample becomes a self-contained `cmd → wait 15 ms → read` cycle with no persistent chip state to break — the same pattern ESP-IDF's reference SHT3x driver uses by default.

2. **Driver init doesn't soft-reset the chip.** The Zephyr driver sends `START_PERIODIC_MEASUREMENT` (or nothing for single-shot) and reports init success without confirming the chip's state. If the chip carried over state from a previous boot (different mode, half-issued command, alert-pending) the new "init" runs against stale chip state. **Fix**: `sensor.c` registers a `SYS_INIT` at `POST_KERNEL` priority 80 that issues the `0x30A2` soft-reset over I2C and waits 2 ms before the Zephyr `sht3xd` driver init runs (priority 90). Cost: one extra I2C write at boot. Benefit: the chip is always in a known state regardless of which mode the driver picks. If the chip is unreachable (wrong address, not wired), the soft-reset write fails silently and init proceeds — the sensor thread will surface the issue clearly when it later finds `device_is_ready()` returning false.

**Diagnosing a future SHT3x failure** — symptoms map to causes:

| Symptom | Likely cause |
|---|---|
| `<err> sensor: SHT3x not ready` at boot | Chip absent, address wrong, or `device_is_ready` failing — wiring problem |
| Boot OK but `<wrn> sensor: sht3x read failed: -5` repeatedly | Driver running against bad chip state OR mode mismatch — try toggling `CONFIG_SHT3XD_SINGLE_SHOT_MODE` |
| Soft-reset wrapper logs `chip absent?` at boot | The address in the overlay (`reg = <0x44>` or `<0x45>`) doesn't match the wired ADDR-pin level (low → 0x44, high → 0x45) |

It is **not** a hardware/pull-up problem. The nRF52 SoC's internal weak pull-ups are sufficient for this physical setup, same as on ESP32. Don't add external resistors before exhausting software-side hypotheses.

### Apple Core Bluetooth + EATT compatibility (macOS Sonoma+ / iOS 17+)

When the peer is a modern Apple device, Core Bluetooth tries to route **Write-Without-Response** through an Enhanced ATT (EATT) bearer on a dynamically-allocated L2CAP channel (e.g. CID 0x003a). NCS v3.3.0's Zephyr (4.3.99) cannot complete the EATT bearer setup against Apple's request — the L2CAP Credit-Connection negotiation hangs in a state where Apple thinks it has the bearer and starts sending data, but our host has no handler bound. Symptoms in the firmware log:

```
<wrn> bt_att: No ATT channel for MTU 140
<wrn> bt_l2cap: Ignoring data for unknown channel ID 0x003a
```

Adding `CONFIG_BT_EATT=y` (plus the usual EATT companions: `CONFIG_BT_GATT_CLIENT`, `CONFIG_BT_L2CAP_DYNAMIC_CHANNEL`, `CONFIG_BT_GATT_AUTO_UPDATE_MTU`, larger `CONFIG_BT_BUF_ACL_RX_COUNT_EXTRA`, `CONFIG_BT_ATT_TX_COUNT`) does NOT fix this in NCS v3.3 — the negotiation still fails.

**Workaround**: use **Write WITH Response** (ATT Write Request, CID 0x0004 — the legacy ATT bearer). Apple routes that through the legacy channel regardless of EATT state. Reads, notifications, and writes-with-response all work fine.

This affects:
- `tests/hil/smoke.py` — uses `response=True` (already applied)
- The React frontend (`react_projects/CoolingDock`) — `@capacitor-community/bluetooth-le` defaults to write-without-response. To make the iOS/macOS app work, change frontend BLE writes to `writeWithResponse: true` in the plugin call.

The issue does NOT affect Android phones (Android's Bluedroid stack doesn't aggressively use EATT in the same way).

### Status / DevInfo / PrpConf JSON shapes

Status (every 500 ms):
```json
{"cmd":"GetStatusData","ms":true,"sensors":{"s0":{"temp":42.50,"humid":65.30}},
 "prf":[{"index":0,"type":"fan","status":"running","pwm":75}]}
```

DeviceInfo (read on connect):
```json
{"cmd":"GetDeviceInfo","name":"CoolingDockNRF","fw":"0.1.0",
 "uid":"<16 hex from NRF_FICR DEVICEID>","board":"nrf52dk_nrf52832"}
```

PeripheralSetsConfig (read on connect):
```json
{"cmd":"GetPeripheralSetsConfig","sets":[{"index":0,"type":"fan","mode":"sensor",
 "pwr":false,"thr1":25,"thr2":32,"pwm":50,"con":10,"coff":10,"ont":0,"offt":0}]}
```

UpdateRet (response to mutating commands, sent via STATUS notify):
```json
{"cmd":"UpdateRet","code":0}
```

### Supported commands (subset of CoolingDock)

| cmd | Args | Effect |
|---|---|---|
| `GetStatusData` | — | Returns current status JSON |
| `GetDeviceInfo` | — | Returns device info JSON |
| `GetPeripheralSetsConfig` | — | Returns persisted prf cfg |
| `SetPeripheralConfig` | `index, mode` + per-mode args | Persists prf[idx] + UpdateRet |
| `SetMasterControl` | `on \| enable \| ms` (bool) | Persists master flag |
| `SetDeviceName` | `name` (string) | Persists device name |
| `Restart` | — | Schedules cold reboot in 500 ms |
| `RestoreFactory` | — | Wipes app/* settings, then reboots |

WiFi/MQTT/alert/token commands from the original CoolingDock are intentionally not implemented.

## IDE diagnostics note

The clang LSP outside the Zephyr build environment cannot resolve `zephyr/*.h` and flags every Zephyr macro/type as unknown. These are spurious — the real build runs through `west`/Zephyr CMake which sets the correct sysroot. Trust `west build` output, not the IDE squiggles.
