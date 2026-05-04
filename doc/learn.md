# Codebase walkthrough

A complete tour of the nRF52 CoolingDock firmware in five layers: big picture → build config → application modules → boot sequence → end-to-end flow of one frontend action.

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

- **Bluetooth host + role**: `BT`, `BT_PERIPHERAL`, `BT_DEVICE_NAME`, `BT_MAX_CONN=1`
- **Larger MTU** (frontend chunks at 200 B): `BT_L2CAP_TX_MTU=247`, `BT_BUF_ACL_*=251`, `BT_CTLR_DATA_LENGTH_MAX=251`
- **Pairing & bonds**: `BT_SMP`, `BT_SMP_SC_PAIR_ONLY` (force LE Secure Connections), `BT_BONDABLE`, `BT_SETTINGS` (bonds persist via the settings subsystem)
- **I2C + sensor**: `I2C`, `SENSOR`, `SHT3XD` (the in-tree Sensirion driver)
- **Fan**: `PWM`, `GPIO`
- **Persistence**: `FLASH`, `FLASH_PAGE_LAYOUT`, `FLASH_MAP`, `NVS`, `SETTINGS`, `SETTINGS_NVS`
- **Logging**: `LOG`, `LOG_DEFAULT_LEVEL=3` (INFO; DBG calls compile out)
- **Misc**: `REBOOT` (so `sys_reboot()` is linked), heap and stack tuning

### `boards/nrf52dk_nrf52832.overlay` — what the hardware actually is

Devicetree overlay that customizes the board for *this* application without forking the upstream board file. It does three things:

1. **Enables `&i2c0`** (it's `disabled` by default on the nRF52 DK board) and adds an `sht3xd@44` child node — that child is what makes the SHT3x driver auto-instantiate.
2. **Enables `&pwm0`** and binds it to a pinctrl group routing channel 0 to `P0.13`. PWM frequency (22 kHz) is encoded in the `pwms = <…>` cell of the consumer node, not the controller.
3. **Adds two consumer nodes at the root** (`fan_pwms` using `pwm-leds` binding, `fan_power_gpios` using `gpio-leds` binding) and **registers three aliases**: `sht3x`, `fan-pwm`, `fan-power`. The application code never references P0.13 / P0.26 / etc. directly — it asks for `DT_ALIAS(fan_pwm)` and `DT_ALIAS(sht3x)`.

This indirection is the whole point of devicetree: change the wiring later by editing only this file.

---

## 3. Application modules

I'll go in dependency order — bottom of the stack first.

### `src/sys_data.{h,c}` — persistent configuration

**The data**: a single static struct with three fields the frontend can mutate:

```c
static struct {
    bool            master;        // master power switch
    char            name[24];      // device name
    struct prf_cfg  prf[1];        // peripheral config (just the fan)
} state = { .master = true, .name = "CoolingDock_NRF52", .prf = { ... defaults ... } };
```

`struct prf_cfg` holds every per-mode field the frontend can send: `mode`, `pwr` (power mode), `thr1_c`/`thr2_c` (sensor mode), `pwm_pct` (fixed/cycle), `con_sec`/`coff_sec` (cycle), `ont_sec`/`offt_sec` (schedule). Even fields irrelevant to the active mode are persisted, so switching modes via the app preserves the previously-set values.

**Persistence model** — Zephyr's settings subsystem with NVS as the backend:

- `SETTINGS_STATIC_HANDLER_DEFINE(app_settings, "app", NULL, sys_set_cb, NULL, NULL)` registers a handler that owns the `app/*` key namespace at link time.
- On `sys_data_init()` → `settings_load_subtree("app")`, the subsystem scans the NVS partition for keys starting with `app/`, and for each one, calls our `sys_set_cb` with the key name and a read-callback to pull the bytes.
- Each setter (`sys_data_set_master`, `sys_data_set_name`, `sys_data_set_prf`) updates the in-memory struct under the mutex *and* calls `settings_save_one("app/<key>", &val, sizeof)` to immediately persist the change.

**Concurrency**: one `K_MUTEX_DEFINE(state_mu)` guards reads and writes. Only fields the frontend touches are persisted — no WiFi, no MQTT, no PN tokens, none of the original CoolingDock state that has no purpose here.

`sys_data_factory_reset()` deletes all three keys via `settings_delete()`, restores in-memory defaults, and returns. The caller (the `RestoreFactory` command) is responsible for triggering a reboot.

### `src/sensor.{h,c}` — SHT3x sampling thread

The interface is two functions:

```c
int  sensor_get(struct sensor_reading *out);   // -EAGAIN if no sample yet
void sensor_start(void);                       // spawn the thread
```

Internal storage is a `struct sensor_reading { int16_t temp_centi_c; uint16_t humid_centi_pct; bool valid; }` guarded by `latest_mu`. Centi-units are stored to keep enough precision while staying in fixed-point integers — no floats anywhere on this MCU.

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

`sensor_value` is `{ val1, val2 }` (whole + millionths). Conversion to centi-units: `val1 * 100 + val2 / 10000`. We clamp humidity to [0, 100 %] in case of sensor noise and store under the mutex.

`sensor_start()` uses dynamic spawn (`k_thread_create`) rather than `K_THREAD_DEFINE` so `main()` can decide when to start it.

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

The 64-bit intermediate avoids overflow when `period × pct` exceeds 32 bits (it can — period at 22 kHz is ~45,454 ns, times 100 = ~4.5 M, fine, but the pattern is defensive). `cur_pct` is a single `uint8_t` cached for `fan_get_percent()` — atomic on Cortex-M3 so no mutex needed.

GPIO is configured `GPIO_OUTPUT_INACTIVE` (off) at init, then toggled with `gpio_pin_set_dt`.

The "use nrf's way" of PWM is delivered here implicitly: `pwm0` in devicetree is backed by Nordic's `nrfx_pwm` driver, which uses the actual PWM0 hardware peripheral on the SoC — not soft-PWM, not a timer-driven GPIO trick.

### `src/control.{h,c}` — fan-control loop

This is where the closed-loop logic lives. Single thread, 1-second period, one job: read state, compute target duty, apply.

The control flow each iteration:

```c
sys_data_get_prf(0, &cfg);          // current peripheral config
bool valid = (sensor_get(&r) == 0); // latest temp+humid
bool master = sys_data_get_master();// is the device "on" at all?

if (cfg.mode != prev_mode) cycle_elapsed_ms = 0;   // reset cycle phase
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
- **CYCLE**: `cycle_mode_pwm()` — alternate `cfg.pwm_pct` for `con_sec` then 0 % for `coff_sec`, phase tracked by accumulating `CONTROL_PERIOD_MS` into `cycle_elapsed_ms` and `% (on+off)`.
- **SCHEDULE**: persisted but returns 0 (no RTC available without WiFi/SNTP).

After computing `target`, the loop drives both the PWM duty and the GPIO power gate (`power_on = target > 0`), then exposes the *actual* commanded duty + a status string ("running"/"stopped") via `control_get_actual_pwm()` and `control_get_status()` — the BLE status JSON reports these so the frontend sees what's *really* happening rather than what's *configured*.

### `src/json_io.{h,c}` — hand-rolled JSON

Why hand-rolled instead of `<zephyr/data/json.h>`? The frontend's `SetPeripheralConfig` schema is **conditional on `mode`** — different fields appear depending on whether mode is `power`, `fixed`, `sensor`, etc. Zephyr's descriptor-based encoder/decoder requires a fixed schema where every declared field must be present. Trying to express conditional fields would mean either declaring everything as optional (loses validation) or maintaining five separate descriptors per command. snprintf is just simpler here.

**Formatters** (`jsf_*`) — each returns bytes written or `-ENOMEM`:

- `jsf_status` — produces the status JSON: `{"cmd":"GetStatusData","ms":bool,"sensors":{"s0":{"temp":N.NN,"humid":N.NN}},"prf":[{"index":0,"type":"fan","status":"...","pwm":N}]}`. If the sensor hasn't yielded a sample yet, temp/humid are `null` (valid JSON, frontend handles).
- `jsf_devinfo` — `{"cmd":"GetDeviceInfo","name":"...","fw":"...","uid":"...","board":"..."}`
- `jsf_prpconf` — `{"cmd":"GetPeripheralSetsConfig","sets":[{...all prf_cfg fields...}]}`. Sends every field unconditionally; frontend ignores ones not relevant to the mode.
- `jsf_updateret` — `{"cmd":"UpdateRet","code":N}` or `{...,"val":"..."}`. The reply to mutating commands.

The `EMIT(...)` macro is a snprintf wrapper that keeps a running `pos` and bails out with `-ENOMEM` if the buffer fills up — saves 6× the boilerplate.

**Parser** (`jsp_*`) — three functions: `jsp_get_str`, `jsp_get_int`, `jsp_get_bool`. All use `find_value(json, key)`, which:

1. `strstr`-searches for the key string,
2. requires the match be a **quoted JSON key** (preceded by `"`, followed by `"` then optional space then `:`),
3. returns a pointer to the start of the value.

That second check (`p[-1] == '"' && p[keylen] == '"'`) prevents matches inside string *values* — so `{"name":"con","con":5}` won't confuse a search for `"con"`. It's not RFC-compliant JSON parsing (no nested objects, no unicode escapes, no number formats beyond decimal int) but it covers the entire frontend command schema.

### `src/cmd_interpreter.{h,c}` — command dispatcher

Transport-agnostic. Single entry point:

```c
ssize_t cmd_interpreter_dispatch(const char *json_in, size_t in_len,
                                 char *json_out, size_t out_cap);
```

Pulls the `cmd` field from `json_in`, routes to a per-command handler, writes a JSON response to `json_out`, returns its length. Designed so any future transport (UART shell, USB CDC, network, …) can call this with a complete JSON object and forward the response.

Eight handlers, grouped:

- **Queries**: `handle_get_status`, `handle_get_devinfo`, `handle_get_prpconf` — pull current state, hand off to the matching `jsf_*` formatter.
- **Mutations**: `handle_set_peripheral`, `handle_set_master`, `handle_set_name` — parse args, validate, call the `sys_data_set_*` setter, return `UpdateRet`.
- **Lifecycle**: `handle_restart`, `handle_factory` — schedule a delayed reboot via `K_WORK_DELAYABLE_DEFINE(reboot_work, ...)` 500 ms out, so the response can be flushed over BLE first.

`handle_set_peripheral` has the most logic — it validates `mode`, then per-mode parses `pwr` / `pwm` / `thr1+thr2` / `con+coff+pwm` / `ont+offt` and clamps numeric ranges. `thr2 <= thr1` returns an error UpdateRet. SCHEDULE persists but logs a warning that nothing will happen.

`cmd_interpreter_init()` runs once from `main()` to format the device UID — read from `NRF_FICR->DEVICEID[1..0]`, the chip's factory-burned 64-bit unique ID — into a hex string for `GetDeviceInfo`.

### `src/ble_svc.{h,c}` — GATT service + adv + threads

The biggest module. Five things happen here:

**1. UUIDs** — Hard-coded to match the React frontend exactly. Service `59462f12-…2d`; four characteristics `…0001` (status), `…0002` (cmd), `…0004` (devinfo), `…0005` (prpconf). No `…0003` because the frontend doesn't subscribe to it; UpdateRet replies are sent back via the STATUS notify channel and demuxed client-side by the `cmd` field.

**2. Advertising data** — Two arrays:
- `ad[]` (advertising packet, 31 B max): flags + complete local name `CoolingDock_NRF52`. The frontend filters scan results by name prefix `CoolingDock`, so the name must lead with that.
- `sd[]` (scan response, 31 B): the 128-bit service UUID, so apps doing service-UUID filtering can also find us.

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

`ATTR_STATUS_VALUE_IDX = 2` is hard-coded in the `ble_svc_start()` setup. If you reorder the service definition, update that index.

All read/write permissions are `BT_GATT_PERM_*_ENCRYPT`, which causes the host stack to demand an encrypted link. That, combined with `CONFIG_BT_SMP=y`, triggers automatic just-works pairing on first read — the phone OS shows a system pair prompt; no app code involved.

**4. Connection lifecycle + command accumulator**:

- `connected_cb` / `disconnected_cb` track `is_connected`/`notify_enabled` atomic flags and re-arm advertising on disconnect (BLE auto-stops advertising when a peer connects).
- `status_ccc_changed` fires when the peer subscribes/unsubscribes; mirrors that into `notify_enabled`.
- `write_cmd` is the BLE command write handler. The frontend chunks writes at 200 B; complete JSON commands can span multiple writes. So `write_cmd` runs a brace-counting state machine over each incoming byte (`acc.depth`, `acc.in_string`, `acc.escape_next`) and only treats the buffer as a complete command when `depth` returns to 0. At that point it copies the buffer into a `struct cmd_msg` and `k_msgq_put`s it to `cmd_q`. The handler returns immediately; **flash writes for command persistence happen on a different thread** so the BT RX context never blocks on slow ops.

**5. Two threads spawned by `ble_svc_start()`**:

- **`ble_notify`** — wakes every 500 ms, skips if not connected or not subscribed, otherwise asks `cmd_interpreter_dispatch("{\"cmd\":\"GetStatusData\"}", ...)` to format the current status JSON and `bt_gatt_notify`s it via `attr_status_value`. Goes through the dispatcher rather than calling `jsf_status` directly so the status format always matches the response to `GetStatusData` reads/queries.
- **`ble_cmd`** — `k_msgq_get` blocks until a complete command lands, then calls `cmd_interpreter_dispatch` to handle it, then sends the response back over the **status notify channel** (using the same `status_notify` helper). The frontend demuxes status updates and command replies by reading the `cmd` field of each JSON message.

`ble_svc_start()` is the orchestrator: `bt_enable(NULL)` → load bonds via `settings_load()` → resolve `attr_status_value` → start advertising → spawn the two threads.

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

## 4. Boot sequence in execution order

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
7. **Advertising starts.** Phone scans, finds `CoolingDock_NRF52`, connects, and the OS triggers just-works pairing (transparent to the React app).
8. **Steady state**: sensor samples every 500 ms, control loop reapplies every 1 s, ble_notify pushes status every 500 ms when subscribed, ble_cmd sleeps on `cmd_q` until the user taps something in the app.

---

## 5. End-to-end: user changes fan threshold to 30 °C

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
   - Copies `cfg` into `state.prf[0]`.
   - Calls `settings_save_one("app/p0", &state.prf[0], sizeof(cfg))` — settings layer asks the NVS backend to write that key. NVS picks a free slot in flash (round-robin within the partition), writes the value + a header, and returns. **This is the slow op** that justified the worker thread.
   - Unlocks the mutex, returns 0.
7. **`handle_set_peripheral`** returns `jsf_updateret(out, cap, 0, NULL)` → `{"cmd":"UpdateRet","code":0}`.
8. **`ble_cmd` thread** sends that response via `bt_gatt_notify(NULL, attr_status_value, resp, n)`.
9. **App side**: status notification handler sees `cmd = "UpdateRet"`, demuxes it as a command reply, marks the request resolved, UI shows "Saved".
10. **One second later** the `control` thread's loop iteration:
    - `sys_data_get_prf(0, &cfg)` returns the new `thr1=20, thr2=30`.
    - `sensor_get(&r)` returns the latest temperature.
    - `apply_cfg` calls `sensor_mode_pwm()` with the new thresholds → new target PWM percent.
    - `fan_set_percent(target)` and `fan_power_set(target > 0)` apply it to the hardware.
11. **Half a second later** the `ble_notify` thread builds a fresh status JSON (with the updated `pwm` field) and sends it to the app, which animates the gauge.

**On reboot**, `sys_data_init()` calls `settings_load_subtree("app")`, the settings subsystem replays every `app/*` key from NVS, and `sys_set_cb` deserializes `app/p0` back into `state.prf[0]`. The thresholds the user set are still 20/30, no app interaction needed.

That's the whole loop. The architecture is: **devicetree binds hardware → drivers expose generic APIs → application threads pull configured state and physical state, compute, drive → BLE layer translates between flash-backed state + sensor reads and the JSON wire protocol the frontend expects.**
