# Codebase walkthrough

A complete tour of the nRF52 CoolingDock firmware in six layers: big picture → build config → application modules → BLE pairing protocol → boot sequence → end-to-end flow of one frontend action.

---

## 1. Big picture

The firmware is a BLE-only port of the ESP32 CoolingDock product. It exposes a fan-control interface to a phone app over BLE, reads an SHT3x temperature/humidity sensor over I2C, drives a fan via hardware PWM + a GPIO power gate, and persists its configuration to internal flash so settings survive reboot.

```
   ┌──────────────────┐           ┌────────────────────────┐
   │  React/Capacitor │ ◄───BLE──►│   ble_svc              │ GATT 4 chars
   │   mobile app     │           │   (notify + cmd thrds) │ encrypted
   └──────────────────┘           └────────┬───────────────┘
                                           │ JSON in/out
                                           ▼
                                  ┌────────────────────────┐
                                  │   cmd_interpreter      │ dispatch on "cmd"
                                  └────┬───────┬────────┬──┘
                                       │       │        │
                          read latest  │       │        │ persist + reboot
                                       ▼       ▼        ▼
                                  ┌────────┐ ┌────────┐ ┌──────────┐
                                  │ sensor │ │ control│ │ sys_data │
                                  │ thread │ │ thread │ │ (NVS)    │
                                  └───┬────┘ └───┬────┘ └────┬─────┘
                                      │ I2C      │ PWM/GPIO  │ flash
                                      ▼          ▼           ▼
                                  ┌─────────┐  ┌────────┐ ┌──────┐
                                  │ SHT3x   │  │  Fan   │ │ NVS  │
                                  │ (ext.)  │  │ + gate │ │ part │
                                  └─────────┘  └────────┘ └──────┘
```

**Five application threads** (all priority 5):

| Thread       | Period / trigger | Job                                                 |
|--------------|------------------|-----------------------------------------------------|
| `sensor`     | 500 ms           | sample SHT3x, store latest reading                  |
| `control`    | 1 s              | compute fan duty for current mode, apply via PWM/GPIO |
| `ble_notify` | 500 ms           | push status JSON to subscribed peer                 |
| `ble_cmd`    | event            | drain msgq, dispatch commands, send response        |
| `main`       | one-shot         | init then exit                                      |

The Bluetooth stack itself spawns 2–3 internal threads (BT RX, BT TX, RPA, …) when `bt_enable()` runs. We don't manage those.

A single `K_WORK` (`adv_kick_work`) on the system workqueue serializes all `bt_le_adv_start`/`stop` operations — every state-machine path that wants to (re)arm advertising posts to that work item rather than calling the host directly. This eliminates a previously-existing race between the BT thread and the system workqueue contending on advertising state.

---

## 2. Build & config layer

### `CMakeLists.txt`

```cmake
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(coolingdock_nrf)

target_sources(app PRIVATE
    src/main.c   src/sys_data.c   src/sensor.c   src/fan.c
    src/control.c   src/json_io.c   src/cmd_interpreter.c   src/ble_svc.c
)
target_include_directories(app PRIVATE src)
```

`find_package(Zephyr)` is the magic line — it pulls in the entire Zephyr build infrastructure (toolchain, devicetree compiler, Kconfig processor, all subsystem libraries). `app` is a predefined target Zephyr links into the final image.

### `prj.conf` — what the binary contains

This file is where the firmware becomes "a Bluetooth peripheral with persistent settings, a sensor subsystem, PWM, GPIO, and reboot capability". Each `CONFIG_*` line either pulls a subsystem in or tunes its size.

Grouped by purpose:

- **Bluetooth host + role**: `BT`, `BT_PERIPHERAL`, `BT_DEVICE_NAME`, `BT_DEVICE_NAME_DYNAMIC` (so `bt_set_name()` is callable at runtime), `BT_DEVICE_NAME_MAX=24`, `BT_MAX_CONN=1`.
- **Larger MTU** (frontend chunks at 200 B): `BT_L2CAP_TX_MTU=247`, `BT_BUF_ACL_*=251`, `BT_CTLR_DATA_LENGTH_MAX=251`.
- **Pairing & bonds**: `BT_SMP`, `BT_SMP_SC_PAIR_ONLY` (force LE Secure Connections), `BT_BONDABLE`, `BT_SETTINGS` (bonds persist via the settings subsystem), `BT_FILTER_ACCEPT_LIST` (used by the bonded-only adv mode), `BT_KEYS_OVERWRITE_OLDEST` (evict oldest bond when the table is full instead of refusing the new pairing).
- **I2C + sensor**: `I2C`, `SENSOR`, `SHT3XD` (the in-tree Sensirion driver).
- **Fan**: `PWM`, `GPIO`.
- **Persistence**: `FLASH`, `FLASH_PAGE_LAYOUT`, `FLASH_MAP`, `NVS`, `SETTINGS`, `SETTINGS_NVS`.
- **Logging**: `LOG`, `LOG_DEFAULT_LEVEL=3` (INFO; DBG calls compile out).
- **Misc**: `REBOOT` (so `sys_reboot()` is linked), heap and stack tuning.

### `boards/nrf52dk_nrf52832.overlay` — what the hardware actually is

Devicetree overlay that customizes the board for *this* application without forking the upstream board file. It does four things:

1. **Disables the board's `button0` and `button1` nodes** — they live on P0.13 and P0.14, which we're reusing as fan PWM and fan-power-gate. Without this, a future Kconfig flip that enables `gpio-keys` would race us for the pins at boot.
2. **Enables `&i2c0`** (it's `disabled` by default on the nRF52 DK board) and adds an `sht3xd@44` child node — that child is what makes the SHT3x driver auto-instantiate.
3. **Enables `&pwm0`** and binds it to a pinctrl group routing channel 0 to `P0.13`. PWM frequency (22 kHz) is encoded in the `pwms = <…>` cell of the consumer node, not the controller.
4. **Adds three consumer nodes / aliases**:
   - `fan-pwm` → a `pwm-leds` child binding into `&pwm0` ch0
   - `fan-power` → a `gpio-leds` child driving `&gpio0 14`
   - `sht3x` → the `sht3xd@44` node above
   - `pairing-btn` → `&button2` (P0.15, BUTTON3 on the DK silkscreen) — used by the pairing-window button described in §4

The application code never references P0.13 / P0.26 / etc. directly — it asks for `DT_ALIAS(...)` and lets the build system resolve.

---

## 3. Application modules

In dependency order — bottom of the stack first.

### `src/sys_data.{h,c}` — persistent configuration

**The data**: a single static struct with three fields the frontend can mutate:

```c
static struct {
    bool            master;        // master power switch
    char            name[24];      // device name
    struct prf_cfg  prf[1];        // peripheral config (just the fan)
} state = { .master = true, .name = "CoolingDockNRF", .prf = { ... defaults ... } };
```

`struct prf_cfg` holds every per-mode field the frontend can send: `mode`, `pwr` (power mode), `thr1_c`/`thr2_c` (sensor mode), `pwm_pct` (fixed/cycle), `con_sec`/`coff_sec` (cycle), `ont_sec`/`offt_sec` (schedule). Even fields irrelevant to the active mode are persisted, so switching modes via the app preserves the previously-set values.

**Persistence model** — Zephyr's settings subsystem with NVS as the backend:

- `SETTINGS_STATIC_HANDLER_DEFINE(app_settings, "app", NULL, sys_set_cb, NULL, NULL)` registers a handler that owns the `app/*` key namespace at link time.
- On `sys_data_init()` → `settings_load_subtree("app")`, the subsystem scans the NVS partition for keys starting with `app/`, and for each one, calls our `sys_set_cb` with the key name and a read-callback to pull the bytes.
- Each setter (`sys_data_set_master`, `sys_data_set_name`, `sys_data_set_prf`) updates the in-memory struct under the mutex, **releases the mutex**, then calls `settings_save_one("app/<key>", &snapshot, sizeof)` — the slow flash write doesn't block other readers.

**Versioned persistence for `prf_cfg`** — to survive future struct-layout changes, `prf_cfg` is wrapped in a small header before being written to flash:

```c
#define PRF_BLOB_MAGIC    0xCD42u
#define PRF_BLOB_VERSION  1u
struct prf_blob { uint16_t magic; uint16_t version; struct prf_cfg cfg; };
```

On load, `sys_set_cb` reads a `prf_blob`, validates the magic and version, and copies `.cfg` out only if both match. A mismatch (corruption, downgrade, layout bump) silently falls back to the static defaults instead of mis-loading garbage.

**Concurrency**: one `K_MUTEX_DEFINE(state_mu)` guards all in-memory state. `sys_data_get_name(buf, cap)` copies into the caller's buffer under the lock so concurrent `SetDeviceName` writes can't tear the read. Only fields the frontend touches are persisted — no WiFi, no MQTT, no PN tokens, none of the original CoolingDock state that has no purpose here.

`sys_data_factory_reset()` deletes all three keys via `settings_delete()`, restores in-memory defaults, and returns. The caller (the `RestoreFactory` command) is responsible for triggering a reboot.

### `src/sensor.{h,c}` — SHT3x sampling thread

The interface is two functions:

```c
int  sensor_get(struct sensor_reading *out);   // -EAGAIN if no fresh sample
void sensor_start(void);                       // spawn the thread
```

Internal storage is a `struct sensor_reading { int16_t temp_centi_c; uint16_t humid_centi_pct; bool valid; }` plus a timestamp `latest_uptime_ms`, both guarded by `latest_mu`. Centi-units are stored to keep enough precision while staying in fixed-point integers — no floats anywhere on this MCU.

`sensor_get` returns `-EAGAIN` if the last good sample is older than `SAMPLE_STALE_MS = 5000`. So if the SHT3x dies mid-run (cable yanked, ESD), the status JSON correctly switches to `"temp":null` after 5 s instead of reporting a stale reading forever.

The sensor device handle is resolved at compile time:

```c
static const struct device *const sht = DEVICE_DT_GET(DT_ALIAS(sht3x));
```

`DT_ALIAS(sht3x)` is the alias we added in the overlay; `DEVICE_DT_GET` expands to a static pointer to the auto-generated `struct device` for that node. **No runtime lookup, no allocation.** The driver itself lives in Zephyr (`drivers/sensor/sensirion/sht3xd/sht3xd.c`) and was registered with the kernel during pre-main init.

The thread loop is the canonical Zephyr sensor pattern:

```c
sensor_sample_fetch(sht);                                    // trigger I2C measurement
sensor_channel_get(sht, SENSOR_CHAN_AMBIENT_TEMP, &t);       // read cached result
sensor_channel_get(sht, SENSOR_CHAN_HUMIDITY,    &h);
```

`sensor_value` is `{ val1, val2 }` (whole + millionths). Conversion to centi-units: `val1 * 100 + val2 / 10000`. Humidity is clamped to [0, 100 %] in case of sensor noise.

### `src/fan.{h,c}` — PWM + GPIO driver wrapper

Thin layer over Zephyr's PWM and GPIO APIs. Three exported functions: `fan_init`, `fan_set_percent(0..100)`, `fan_power_set(bool)`.

DT specs are resolved at compile time:

```c
static const struct pwm_dt_spec  fan_pwm   = PWM_DT_SPEC_GET(DT_ALIAS(fan_pwm));
static const struct gpio_dt_spec fan_power = GPIO_DT_SPEC_GET(DT_ALIAS(fan_power), gpios);
```

`pwm_dt_spec` carries the device pointer + channel + period (in nanoseconds — derived from `PWM_HZ(22000)` in the overlay) + flags. `fan_set_percent` translates a percentage into a pulse width:

```c
uint64_t pulse = ((uint64_t)fan_pwm.period * pct) / 100U;
pwm_set_pulse_dt(&fan_pwm, (uint32_t)pulse);
```

The 64-bit intermediate avoids overflow when `period × pct` exceeds 32 bits. `cur_pct` is a single `uint8_t` cached for `fan_get_percent()` — atomic on Cortex-M3 so no mutex needed.

GPIO is configured `GPIO_OUTPUT_INACTIVE` (off) at init, then toggled with `gpio_pin_set_dt`.

The "use nrf's way" of PWM is delivered here implicitly: `pwm0` in devicetree is backed by Nordic's `nrfx_pwm` driver, which uses the actual PWM0 hardware peripheral on the SoC — not soft-PWM, not a timer-driven GPIO trick.

### `src/control.{h,c}` — fan-control loop

This is where the closed-loop logic lives. Single thread, 1-second period, one job: read state, compute target duty, apply.

The control flow each iteration:

```c
sys_data_get_prf(0, &cfg);          // current peripheral config
bool valid = (sensor_get(&r) == 0); // latest temp+humid (or staleness -> false)
bool master = sys_data_get_master();// is the device "on" at all?

if (!have_prev_mode || cfg.mode != prev_mode) cycle_elapsed_ms = 0;
if (!master) { fan off, skip mode logic; }
else         apply_cfg(&cfg, r.temp_centi_c, valid, cycle_elapsed_ms);
```

`apply_cfg` switches on `cfg.mode`:

- **POWER**: 100 % if `pwr` set else 0 %
- **FIXED**: `cfg.pwm_pct`
- **SENSOR**: `sensor_mode_pwm()` — three-zone hysteresis with linear ramp:
  - `T ≤ thr1` → 0 %
  - `thr1 < T < thr1+2 °C` → 30 % (dead-band, prevents fan thrash near the threshold)
  - `thr1+2 °C ≤ T < thr2` → linear 30 % → 100 %
  - `T ≥ thr2` → 100 %
- **CYCLE**: `cycle_mode_pwm()` — alternate `cfg.pwm_pct` for `con_sec` then 0 % for `coff_sec`, phase tracked by accumulating `CONTROL_PERIOD_MS` into `cycle_elapsed_ms` and `% (on+off)`. `pwm_pct=0` is honored literally (cycle-on phase commands 0 % too).
- **SCHEDULE**: persisted but returns 0 (no RTC available without WiFi/SNTP).

After computing `target`, the loop drives both the PWM duty and the GPIO power gate (`power_on = target > 0`), then publishes the *actual* commanded duty + a status enum (`STAT_RUNNING`/`STAT_STOPPED`/`STAT_UNKNOWN`) via `atomic_t actual_pwm_a` and `atomic_t status_a`. The BLE thread reads these via `control_get_actual_pwm()` and `control_get_status()` (the latter translates the enum to the JSON string the frontend expects). Atomics make the cross-thread reads well-defined under the C memory model rather than relying on Cortex-M3 single-byte-store atomicity.

### `src/json_io.{h,c}` — hand-rolled JSON

Why hand-rolled instead of `<zephyr/data/json.h>`? The frontend's `SetPeripheralConfig` schema is **conditional on `mode`** — different fields appear depending on whether mode is `power`, `fixed`, `sensor`, etc. Zephyr's descriptor-based encoder/decoder requires a fixed schema where every declared field must be present.

**Formatters** (`jsf_*`) — each returns bytes written or `-ENOMEM`:

- `jsf_status` — produces the status JSON: `{"cmd":"GetStatusData","ms":bool,"sensors":{"s0":{"temp":N.NN,"humid":N.NN}},"prf":[{"index":0,"type":"fan","status":"...","pwm":N}]}`. If the sensor hasn't yielded a fresh sample, temp/humid are `null`. Negative temperatures with magnitude under 1 °C (e.g. -0.50) are printed with an explicit minus prefix; without that, integer truncation would emit `0.50` instead of `-0.50`.
- `jsf_devinfo` — `{"cmd":"GetDeviceInfo","name":"...","fw":"...","uid":"...","board":"..."}`
- `jsf_prpconf` — `{"cmd":"GetPeripheralSetsConfig","sets":[{...all prf_cfg fields...}]}`. Sends every field unconditionally; frontend ignores ones not relevant to the mode.
- `jsf_updateret` — `{"cmd":"UpdateRet","code":N}` or `{...,"val":"..."}`. The reply to mutating commands.

The `EMIT(...)` macro is a snprintf wrapper that keeps a running `pos` and bails out with `-ENOMEM` if the buffer fills up.

**Parser** (`jsp_*`) — three functions: `jsp_get_str`, `jsp_get_int`, `jsp_get_bool`. All use `find_value(json, key)`, which:

1. `strstr`-searches for the key string,
2. requires the match be a quoted KEY: `p[-1] == '"' && p[keylen] == '"'` **AND** the next non-whitespace char must be `:` (without that colon check, `{"name":"name"}` would match the value rather than the key when searching for `"name"`),
3. returns a pointer to the start of the value.

`jsp_get_str` passes through one level of escape (`\"`, `\\`, `\/`, …) by copying the next char verbatim — handles common escapes without a full Unicode parser.

It's not RFC-compliant JSON parsing (no nested objects, no `\uXXXX`) but it correctly covers the entire frontend command schema.

### `src/cmd_interpreter.{h,c}` — command dispatcher

Transport-agnostic. Single entry point:

```c
ssize_t cmd_interpreter_dispatch(const char *json_in, size_t in_len,
                                 char *json_out, size_t out_cap);
```

Pulls the `cmd` field from `json_in`, routes to a per-command handler, writes a JSON response to `json_out`, returns its length. Designed so any future transport (UART shell, USB CDC, network, …) can call this with a complete JSON object and forward the response.

**`UpdateRet` codes** are a small disjoint set so callers can dispatch on them cleanly:

| const | value | meaning |
|---|---|---|
| `URET_OK` | 0 | success |
| `URET_NOOP` | 1 | already in requested state (frontend's "already up-to-date") |
| `URET_FAIL` | 2 | I/O / persistence failure |
| `URET_BAD_REQ` | 3 | malformed args, missing field, validation error |
| `URET_UNSUPP` | 4 | unknown `cmd` |

Eight handlers, grouped:

- **Queries**: `handle_get_status`, `handle_get_devinfo`, `handle_get_prpconf` — pull current state, hand off to the matching `jsf_*` formatter. `handle_get_devinfo` copies the device name into a stack buffer via `sys_data_get_name(buf, cap)`.
- **Mutations**: `handle_set_peripheral`, `handle_set_master`, `handle_set_name` — parse args, validate, call the `sys_data_set_*` setter, return `UpdateRet`.
  - `handle_set_master` only accepts the `on` field (verified against `react_projects/CoolingDock/src/App.tsx`).
  - `handle_set_name` additionally calls `ble_svc_apply_name(name)` so the BLE-advertised name is updated alongside the persisted value.
- **Lifecycle**: `handle_restart`, `handle_factory` — schedule a delayed reboot via `K_WORK_DELAYABLE_DEFINE(reboot_work, ...)` 500 ms out, so the response can be flushed over BLE first.

`handle_set_peripheral` validates `mode`, then per-mode parses `pwr` / `pwm` / `thr1+thr2` / `con+coff+pwm` / `ont+offt` and clamps numeric ranges. `thr2 <= thr1` returns `URET_BAD_REQ`. SCHEDULE persists but logs a warning that nothing will happen.

`cmd_interpreter_init()` runs once from `main()` to format the device UID — read from `NRF_FICR->DEVICEID[1..0]`, the chip's factory-burned 64-bit unique ID — into a hex string for `GetDeviceInfo`.

### `src/ble_svc.{h,c}` — GATT service + adv + threads + pairing-window state machine

The biggest module. Six things happen here:

**1. UUIDs** — Hard-coded to match the React frontend exactly. Service `59462f12-…2d`; four characteristics `…0001` (status), `…0002` (cmd), `…0004` (devinfo), `…0005` (prpconf). No `…0003` — the frontend doesn't subscribe to it; UpdateRet replies are sent back via the STATUS notify channel and demuxed client-side by the `cmd` field.

**2. Advertising data** — The flags TLV is in `ad[]`; the device-name TLV is **not** — both adv-param macros set `BT_LE_ADV_OPT_USE_NAME`, which makes the host inject the current `bt_get_name()` value at adv-build time. So when `SetDeviceName` calls `bt_set_name()`, the next adv packet carries the new name without us rebuilding any structures.

The scan response (`sd[]`) carries the 128-bit service UUID so apps doing service-UUID filtering can also find us.

**3. GATT service definition** — `BT_GATT_SERVICE_DEFINE(coolingdock_svc, ...)`. This is a macro that emits a `struct bt_gatt_service_static` plus a flat array of `struct bt_gatt_attr` and registers them with the GATT server at boot via Zephyr's iterable sections. Attribute layout for our service:

| idx | attribute |
|---|---|
| 0 | primary service decl |
| 1 | status char decl |
| 2 | status value (handled by `read_status`) |
| 3 | status CCC descriptor |
| 4 | cmd char decl |
| 5 | cmd value (handled by `write_cmd`) |
| 6 | devinfo char decl |
| 7 | devinfo value (handled by `read_devinfo`) |
| 8 | prpconf char decl |
| 9 | prpconf value (handled by `read_prpconf`) |

`ATTR_STATUS_VALUE_IDX = 2` is hard-coded in `ble_svc_start()`. If you reorder the service definition, update that index.

All read/write permissions are `BT_GATT_PERM_*_ENCRYPT`, which causes the host stack to demand an encrypted link. That, combined with `CONFIG_BT_SMP=y`, triggers automatic just-works pairing on first read — the phone OS shows a system pair prompt; no app code involved.

**4. Connection lifecycle + command accumulator**:

- `connected_cb` / `disconnected_cb` track `is_connected`/`notify_enabled` atomic flags and call `request_advertising()` (see below) on disconnect.
- `status_ccc_changed` fires when the peer subscribes/unsubscribes; mirrors that into `notify_enabled`.
- `write_cmd` is the BLE command write handler. The frontend chunks writes at 200 B; complete JSON commands can span multiple writes. So `write_cmd` runs a brace-counting state machine over each incoming byte (`acc.depth`, `acc.in_string`, `acc.escape_next`) and only treats the buffer as a complete command when `depth` returns to 0. At that point it copies the buffer into a `struct cmd_msg` and `k_msgq_put`s it to `cmd_q`. The handler returns immediately; **flash writes for command persistence happen on a different thread** so the BT RX context never blocks on slow ops.
- `status_notify` checks the `bt_gatt_notify` return; warns on `-EMSGSIZE` so an oversized status payload is visible in logs rather than silently dropped.

**5. Two threads spawned by `ble_svc_start()`**:

- **`ble_notify`** — wakes every 500 ms, skips if not connected or not subscribed, otherwise asks `cmd_interpreter_dispatch("{\"cmd\":\"GetStatusData\"}", ...)` to format the current status JSON and `bt_gatt_notify`s it via `attr_status_value`. Goes through the dispatcher rather than calling `jsf_status` directly so the status format always matches the response to `GetStatusData` reads/queries.
- **`ble_cmd`** — `k_msgq_get` blocks until a complete command lands, then calls `cmd_interpreter_dispatch` to handle it, then sends the response back over the **status notify channel** (using the same `status_notify` helper).

**6. `ble_svc_apply_name(name)`** — exposed in the header so `cmd_interpreter` can call it after `sys_data_set_name`. Calls `bt_set_name(name)` then `request_advertising()` to roll out the new name in the next adv packet.

### `src/main.c` — boot ordering

Linear init, then return:

```c
settings_subsys_init();      // 1. mount NVS, ready for settings_*()
sys_data_init();             // 2. load app/* keys, fall back to defaults
cmd_interpreter_init();      // 3. cache device UID for GetDeviceInfo
fan_init();                  // 4. configure PWM + GPIO, set fan to 0%
sensor_start();              // 5. spawn sensor sampling thread
control_start();             // 6. spawn fan-control thread
ble_svc_start();             // 7. bt_enable + adv + spawn 2 BLE threads
```

This is "Style B" coordination — explicit ordered spawn from `main()` rather than `K_THREAD_DEFINE`. Necessary because:

- `bt_enable()` reads bond data via the settings subsystem, which must be initialised first.
- The SHT3x driver is auto-initialised at SYS_INIT time (before `main`), but if the sensor thread tried to read state from `sys_data` before `sys_data_init()` ran, it would see in-memory defaults rather than persisted values.

`main` returns after kicking off the threads; the main thread terminates but everything else keeps running.

---

## 4. BLE pairing-window protocol

Two advertising modes, governed by `atomic_t adv_mode_v`:

| Mode | Adv params | Effect |
|---|---|---|
| `ADV_OPEN` | `BT_LE_ADV_OPT_CONN \| BT_LE_ADV_OPT_USE_NAME` | Any client may connect → trigger pairing → bond |
| `ADV_BONDED_ONLY` | adds `BT_LE_ADV_OPT_FILTER_CONN` | Controller drops connection requests from addresses not in the Filter Accept List |

State transitions:

| Event | Transition |
|---|---|
| Boot | mode = OPEN, schedule 120 s timer, request adv |
| `pairing_complete(bonded=true)` | mode = BONDED_ONLY, cancel timer (locked in) |
| 120 s timer fires (no pairing happened) | mode = BONDED_ONLY (FAL may be empty → no one can connect) |
| BUTTON3 (P0.15) held ≥ 5 s | mode = OPEN, reschedule 120 s timer, disconnect any current peer (so the new mode takes effect immediately) |
| Disconnect | request adv in whatever mode is current |

**Key implementation details**:

- `request_advertising()` posts to a single `K_WORK adv_kick_work` on the system workqueue. All callers (boot, disconnect, button hold, timer expiry, pairing complete, name change) go through this one function — no two threads ever call `bt_le_adv_start/stop` simultaneously.
- `start_advertising_now()` (the work handler) repopulates the FAL via `bt_le_filter_accept_list_clear` + `bt_foreach_bond` → `bt_le_filter_accept_list_add` whenever it starts in BONDED_ONLY mode. Bond list is the source of truth; we never cache addresses.
- `enter_open_window()` uses `bt_conn_foreach(BT_CONN_TYPE_LE, disconnect_one, &ctx)` to disconnect any active peer rather than dereferencing a cached `current_conn` from a non-BT thread (which would race with `disconnected_cb` running on the BT thread).
- BUTTON3 long-press detection: `gpio_pin_interrupt_configure_dt(EDGE_BOTH)` on the button GPIO; press schedules a `K_WORK_DELAYABLE` for 5 s, release cancels it. The handler re-reads the pin to defend against bounce, then calls `enter_open_window()`.

**Soft-brick recovery**: if the device boots fresh with no bond and no client pairs within 120 s, advertising stops (FAL is empty). Hold BUTTON3 for 5 s to reopen. If the button is unavailable (overlay alias missing), only `west flash --erase` recovers — the chip-erase wipes settings + bonds along with the firmware.

### Wire contract recap

- Service UUID `59462f12-9543-9999-12c8-58b459a2712d`
- Characteristics (all encrypted via `BT_GATT_PERM_*_ENCRYPT`):
  - `…0001` Status — READ + NOTIFY (status JSON, also carries UpdateRet replies)
  - `…0002` Command — WRITE (chunked JSON in)
  - `…0004` Device Info — READ
  - `…0005` Peripheral Sets — READ
- Device name must start with `CoolingDock` (frontend filters scan by name prefix). The advertised name reflects `bt_get_name()`, which is initialized from `sys_data` at boot and updated by `SetDeviceName`.
- MTU enlarged to 247 (`CONFIG_BT_L2CAP_TX_MTU`) so 200-byte frontend chunks fit in single L2CAP frames.
- LE Secure Connections, just-works, bondable. Bond stored via `CONFIG_BT_SETTINGS=y`; oldest evicted when full (`CONFIG_BT_KEYS_OVERWRITE_OLDEST=y`).

### Status / DevInfo / PrpConf / UpdateRet JSON shapes

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

---

## 5. Boot sequence in execution order

What actually happens from reset:

1. **Reset vector → Zephyr early init** — clocks, NVIC, MPU, vector table.
2. **`SYS_INIT` chain** runs through phases (`EARLY` → `PRE_KERNEL_1` → `PRE_KERNEL_2` → `POST_KERNEL` → `APPLICATION`):
   - `nrfx` HAL initializes the SoC.
   - I2C driver initializes the controller.
   - SHT3x driver registers a `struct device` (lazy — actual sensor talk happens later).
   - PWM driver registers `pwm0`.
   - GPIO driver registers `gpio0`.
   - NVS module attaches the `storage_partition` from devicetree.
   - The settings subsystem registers our static handler `app_settings`.
   - The Bluetooth host module sets up internal data structures (no radio yet).
3. **`BT_GATT_SERVICE_DEFINE` and `BT_CONN_CB_DEFINE` records** were placed into iterable linker sections by their respective macros; the BT subsystem will iterate them when `bt_enable()` runs.
4. **Scheduler starts.** All ready threads (the system workqueue, idle, eventually `main`) enter the run queue. Our application threads don't exist yet — they're created from `main`.
5. **`main` runs**, executing the 7-step init sequence above. Each `*_start()` call spawns a thread that's immediately runnable.
6. **`bt_enable()`** synchronously brings up the Bluetooth host, spawns BT RX/TX worker threads, loads bonds from settings.
7. **`ble_svc_start()`** registers `auth_info_cb`, configures BUTTON3, syncs the host's name from `sys_data`, sets `adv_mode = OPEN`, schedules the 120 s timer, posts the first `adv_kick_work` to start advertising. Phone scans, finds `CoolingDockNRF`, connects, OS triggers just-works pairing transparent to the React app.
8. **Steady state**: sensor samples every 500 ms, control loop reapplies every 1 s, ble_notify pushes status every 500 ms when subscribed, ble_cmd sleeps on `cmd_q` until the user taps something in the app.

---

## 6. End-to-end: user changes fan threshold to 30 °C

To make the layering concrete, here's what happens when the user opens the app, taps "sensor mode", types `thr1=20 thr2=30`, hits Confirm:

1. **App side** — Capacitor's BLE plugin chunks the JSON `{"cmd":"SetPeripheralConfig","index":0,"mode":"sensor","thr1":20,"thr2":30,"retfmt":"json"}` into ≤200-byte writes targeting characteristic `…0002` (cmd).
2. **Stack delivery** — Zephyr's BT controller assembles ATT Write requests, host runs `write_cmd` callback (in BT RX thread context).
3. **Brace counter** in `write_cmd` accumulates bytes; on the closing `}` of the top-level object, posts a `struct cmd_msg` to `cmd_q` and returns to BLE.
4. **`ble_cmd` thread** wakes from `k_msgq_get`, calls `cmd_interpreter_dispatch(msg.buf, msg.len, resp, sizeof(resp))`.
5. **`cmd_interpreter`** parses `cmd`="SetPeripheralConfig", routes to `handle_set_peripheral`, which:
   - Reads `index=0`, validates.
   - Reads `mode="sensor"`, maps to `PRF_MODE_SENSOR`.
   - Reads `thr1=20`, `thr2=30`, validates `thr2 > thr1`.
   - Calls `sys_data_set_prf(0, &cfg)`.
6. **`sys_data_set_prf`**:
   - Locks `state_mu`.
   - Copies `cfg` into `state.prf[0]`, makes a snapshot.
   - **Releases the mutex.**
   - Wraps the snapshot in a `prf_blob` (magic + version + cfg) and calls `settings_save_one("app/p0", &blob, sizeof(blob))` — settings layer asks the NVS backend to write that key. NVS picks a free slot in flash (round-robin within the partition), writes the value + a header, and returns. The slow flash op no longer blocks readers since the mutex was already released.
   - Returns 0.
7. **`handle_set_peripheral`** returns `jsf_updateret(out, cap, URET_OK, NULL)` → `{"cmd":"UpdateRet","code":0}`.
8. **`ble_cmd` thread** sends that response via `bt_gatt_notify(NULL, attr_status_value, resp, n)`.
9. **App side**: status notification handler sees `cmd = "UpdateRet"`, demuxes it as a command reply, marks the request resolved, UI shows "Saved".
10. **One second later** the `control` thread's loop iteration:
    - `sys_data_get_prf(0, &cfg)` returns the new `thr1=20, thr2=30`.
    - `sensor_get(&r)` returns the latest temperature (or `-EAGAIN` if stale > 5 s).
    - `apply_cfg` calls `sensor_mode_pwm()` with the new thresholds → new target PWM percent.
    - `fan_set_percent(target)` and `fan_power_set(target > 0)` apply it to the hardware.
    - `atomic_set(&actual_pwm_a, target)` and `atomic_set(&status_a, STAT_RUNNING/STOPPED)` publish for the BLE side.
11. **Half a second later** the `ble_notify` thread builds a fresh status JSON (with the updated `pwm` field, read via the atomics) and sends it to the app, which animates the gauge.

**On reboot**, `sys_data_init()` calls `settings_load_subtree("app")`, the settings subsystem replays every `app/*` key from NVS, and `sys_set_cb` reads the `prf_blob`, validates magic+version, and copies the cfg into `state.prf[0]`. The thresholds the user set are still 20/30, no app interaction needed. The pairing window reopens (boot always enters `ADV_OPEN`), but the previously-bonded phone can still reconnect during the window using the persisted bond.

That's the whole loop. The architecture is: **devicetree binds hardware → drivers expose generic APIs → application threads pull configured state and physical state, compute, drive → BLE layer translates between flash-backed state + sensor reads and the JSON wire protocol the frontend expects, with all advertising mutations serialized through one work item to keep state coherent across threads.**
