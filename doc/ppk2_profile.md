# Power profiling with the PPK2

Practical guide for measuring this firmware's current consumption on the nRF52 DK using the Nordic **Power Profiler Kit II (PPK2)**. Covers hardware wiring (specific to PCA10040), software setup, and per-region attribution via GPIO markers.

---

## What the PPK2 is

A USB-stick from Nordic that measures current from ~200 nA to 1 A at 100 ksps. Two operating modes:

| Mode | Who powers the DUT | When to use |
|---|---|---|
| **Source** | PPK2 supplies VDD and measures the current it sources | When you want to vary VDD across measurements (e.g. simulate battery drain) |
| **Ampere** | Some other supply powers the DUT; PPK2 sits in series like an ammeter | The default for "just measure how much my firmware uses" |

For everyday firmware power profiling on this project, **Ampere mode at the DK's P22 current-measurement header** is the canonical setup.

---

## Hardware setup for nRF52 DK (PCA10040)

### One-time board modification

The PCA10040 ships with **solder bridge SB9** shorting the two pins of the `P22` current-measurement header. With SB9 intact, current flows from the on-board 3.3 V regulator straight to VDD_nRF — no way to insert an ammeter.

**Cut SB9** on the back of the DK with a sharp knife or scalpel. After this, current must flow through whatever you place across P22 to reach VDD_nRF.

```
Before SB9 cut:                 After SB9 cut:
───────[ SB9 ]──────             ────[gap]────
                                       ↑
                              insert PPK2 (or any
                              ammeter) here via P22
```

The cut is one-way unless you re-solder a bridge across the SB9 pads later. To reprogram the chip while SB9 is cut and PPK2 is wired in: disconnect any external supply, briefly add a jumper across P22 to short it, flash, then remove the jumper.

> Note: PCA10040 has **no SW6/SW9/SW10 toggles** — those switches exist on the newer PCA10100 (nRF52833 DK), not on this board. PCA10040 uses solder-bridge modifications instead of switches for power-source selection.

### Wiring the PPK2

| PPK2 P1 pin | Wire color (Nordic kit) | DK side |
|---|---|---|
| `GND` | Black | any DK GND (P20 has one near P22) |
| `VIN` | Brown | `P22` pin **closer to the LDO/USB** (upstream — current source) |
| `VOUT` | Red | `P22` pin **closer to the chip** (downstream — current destination) |

> **Polarity matters in Ampere mode.** PPK2 has reverse-current protection on its sense path. If VIN and VOUT are swapped:
> - The chip still gets *some* power (leakage through the protection diodes), so BLE may appear to work
> - The current chart reads sub-µA (clamped to ~nA range)
> - It looks like the meter is broken or SB9 wasn't cut
>
> Confirmed by experiment on this project: with wires swapped, the chart showed ~1.5 nA average with a 770 nA peak. Swapped them → instantly read 1.45 mA average. So if readings look impossibly small while the chip is alive, **swap the wires first** before chasing other theories.

### Plugging everything in

- DK J-Link USB cable → your Mac (powers the DK normally; the J-Link side is independent of P22, so flashing + serial VCOM still work)
- PPK2 USB → your Mac
- DK power switch (top-left, `Power OFF/ON`) → ON

Both USB connections matter. The J-Link USB powers the DK's regulator; PPK2 USB powers the PPK2 + carries data. Without the J-Link USB, the DK has no upstream power supply for PPK2 to meter.

### Sanity check

In the Power Profiler app, briefly press the **IF BOOT/RESET** button on the DK. The current trace should:

1. Drop briefly toward 0 (chip in reset)
2. Spike during BT controller init (~5 mA spike)
3. Settle to the steady-state idle pattern (BLE adv events every ~100 ms)

If the trace doesn't change when you reset the chip, P22 isn't actually inserted (SB9 is still shorting it, or PPK2 isn't really on P22).

---

## Software setup

Install the **Power Profiler** app via nRF Connect for Desktop (already installed if you set up the toolchain via Nordic's instructions):

1. Open nRF Connect for Desktop
2. Click **Apps** → install **Power Profiler**

Or download standalone: <https://www.nordicsemi.com/Products/Development-hardware/Power-Profiler-Kit-2>

Launch the Power Profiler app:

| Setting | Value |
|---|---|
| Device | PPK2 (auto-detected at top) |
| Mode | **Ampere** |
| Voltage | n/a in Ampere mode |

Click **Enable** → **Start**. The live current chart begins.

### Source-mode alternative (no SB9 cut required)

If you don't want to modify the board, you can use the External-supply header `P21` in Source Mode:

| PPK2 pin | DK pin |
|---|---|
| `VOUT` | `P21` external supply `+` |
| `GND` | `P21` external supply `-` |

| Setting | Value |
|---|---|
| Mode | **Source** |
| Voltage | 3.0 V (or 3.3 V) |

Power Profiler now both supplies and measures. Slightly less precise than P22 Ampere mode, but no soldering required. Use this for one-off checks before committing to the SB9 cut.

---

## What the readings mean

Rough breakdown for our firmware in OPEN advertising mode, no client connected, no fan running:

| Component | Active draw | Active duration | Avg contribution |
|---|---|---|---|
| nRF52 CPU (running threads) | ~3 mA | few ms / sample cycle | ~30–60 µA |
| BLE radio: advertising (~100 ms interval) | ~10 mA peaks | ~1 ms every 100 ms | ~100 µA |
| BLE radio: connected + 500 ms notifications | ~7 mA peaks | ~2–3 ms per conn event | **300–500 µA** |
| SHT3x measurement (single-shot, high-rep) | ~600 µA | ~15 ms per sample | ~18 µA at 500 ms cadence |
| SHT3x idle | ~0.6 µA | continuous | <1 µA |
| Fan PWM peripheral (just the SoC) | ~50 µA | continuous when on | ~50 µA |

Idle (advertising only) average: **~80–150 µA**. Connected + notifying: **~300–500 µA**. The fan motor itself dominates everything when running, but that's external to the SoC.

---

## Per-region power attribution

Direct line-by-line attribution doesn't really exist (current is instantaneous, code is sequential). What you *can* do is **per-region** measurement: bracket a code region with GPIO toggles, wire those GPIOs to the PPK2's digital channels, and read off the average current and duration of that region from the chart.

### Mental model

The PPK2 captures two synchronized streams:

- **Current** — what the chip is drawing (100 ksps)
- **Digital channels D0–D7** — 8 logic inputs sampled at the same rate

If a GPIO goes high right before a code region and low right after, the digital trace shows a square wave aligned exactly with the current trace. Drop two markers on the chart bracketing one rising-to-falling edge → the app shows you:

- **Δt** — duration of the region
- **Avg current** — mean current draw during the region
- **Charge** (avg × Δt) — total energy cost in coulombs

Multiply charge by VDD to get joules.

### Wiring the GPIOs to PPK2

The PPK2 has a 10-pin logic-port ribbon connector. The included ribbon cable has labeled wires:

| PPK2 logic pin | Wire color | Suggested DK GPIO |
|---|---|---|
| D0 | Brown | LED1 (`P0.17`) |
| D1 | Red | LED2 (`P0.18`) |
| D2 | Orange | LED3 (`P0.19`) |
| D3 | Yellow | LED4 (`P0.20`) |
| D4–D7 | Green / Blue / Purple / Grey | unused GPIOs / additional regions |
| GND | Black | DK GND |

Use the LEDs as the easy default — they're already on the DK and exposed in devicetree as `led0`–`led3`. If you need more than four regions, route additional GPIOs (e.g. `P0.03`, `P0.04` on the Arduino headers) to D4–D7.

### Code pattern

```c
#include <zephyr/init.h>
#include <zephyr/drivers/gpio.h>

static const struct gpio_dt_spec mark0 = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec mark1 = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
static const struct gpio_dt_spec mark2 = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);

static int mark_init(void)
{
    gpio_pin_configure_dt(&mark0, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&mark1, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&mark2, GPIO_OUTPUT_INACTIVE);
    return 0;
}
SYS_INIT(mark_init, APPLICATION, 99);

#define MARK_HI(m)  gpio_pin_set_dt(&(m), 1)
#define MARK_LO(m)  gpio_pin_set_dt(&(m), 0)
```

Wrap regions to measure. Example targeting `notify_thread` in `ble_svc.c`:

```c
while (1) {
    k_msleep(NOTIFY_PERIOD_MS);
    if (!atomic_get(&is_connected)) continue;

    MARK_HI(mark0);                                          /* status JSON build */
    ssize_t n = cmd_interpreter_dispatch("…", …, buf, sizeof(buf));
    MARK_LO(mark0);

    MARK_HI(mark1);                                          /* GATT notify */
    (void)bt_gatt_notify(NULL, attr_status_value, buf, n);
    MARK_LO(mark1);
}
```

And in the sensor thread:

```c
MARK_HI(mark2);                                              /* SHT3x sample */
sensor_sample_fetch(sht);
sensor_channel_get(sht, SENSOR_CHAN_AMBIENT_TEMP, &t);
sensor_channel_get(sht, SENSOR_CHAN_HUMIDITY,    &h);
MARK_LO(mark2);
```

### Reading the chart

After flashing the instrumented firmware:

1. The Power Profiler app shows current at top, 8 digital traces underneath.
2. The traces toggle in sync with your `MARK_HI`/`MARK_LO` calls.
3. Click + drag (or left-click + right-click) to drop two markers bracketing a single rising-to-falling edge of D0.
4. The status panel shows Δt, average current, and charge for that region.

So for example: `bt_gatt_notify` might show Δt ≈ 1.5 ms, avg ≈ 6 mA → charge ≈ 9 µC per notify. At 500 ms cadence, that's an average of 18 µA contributed by notifications alone — a number you can quote and compare against future code changes.

---

## Useful patterns

### Loop the region for stability

A single instance of a fast region is hard to read precisely. Run the code in a loop and let markers span hundreds of cycles:

```c
for (int i = 0; i < 1000; i++) {
    MARK_HI(mark0);
    do_thing();
    MARK_LO(mark0);
}
```

The "average between markers" reading then has tiny error.

### Differential measurement

To answer "how much does feature X cost?" — measure twice:

1. Capture average current with the feature **disabled** (compile out, comment out, or runtime flag).
2. Capture again with it **enabled**.
3. Subtract.

The difference (scaled by VDD) is what feature X costs in power.

### Synchronize with serial logs

Have the firmware emit a log line at `MARK_HI`. The serial timestamp and the PPK2 timestamp can be correlated visually — useful for understanding what was happening in the firmware around an unexpected current spike.

```c
MARK_HI(mark0);
LOG_INF("region X start");
do_thing();
MARK_LO(mark0);
LOG_INF("region X end");
```

---

## What this can't tell you

- **Per-instruction power.** PPK2 samples at 100 ksps, the chip runs at 64 MHz. You only see regions ≥ ~10 µs.
- **Sleep-mode efficacy.** To validate System OFF / System ON, the firmware has to actually enter that mode (Zephyr does this automatically with `CONFIG_PM=y` when no thread is runnable). PPK2 shows whether the chip *actually* sleeps (current drops to single µA) or whether something is keeping it awake (residual mA).
- **Causation.** If a region's current is high, the *why* is up to you to figure out from the code. PPK2 measures, doesn't explain.

---

## Useful baseline measurements to capture

Every time you make a power-relevant change, measure these and compare to the previous values:

| Scenario | Capture |
|---|---|
| Idle, advertising only (OPEN mode, no peer) | 10 s average — your fundamental baseline |
| Connected, peer subscribed to status notifications | 10 s average — your "active use" baseline |
| One BLE adv event isolated | Δt + charge — useful for tuning advertising interval |
| One status notify isolated | Δt + charge — biggest single repeating cost |
| One SHT3x sample isolated | Δt + charge — usually negligible relative to BLE |
| One control-loop iteration | Δt + charge — shows fan PWM impact |

Save the captures as CSV (`File` → `Save data`). Name them descriptively (`baseline-adv-only-FW0.1.0.csv`) so a year from now you can compare against current firmware quantitatively.

---

## What it's good for in this firmware

Most relevant questions that `MARK_*` instrumentation can answer:

- **How much does each `bt_gatt_notify` cost?** Wrap the call. Read avg × Δt.
- **Is single-shot SHT3x more or less expensive than periodic?** Compile both, mark the sample, compare.
- **Is I2C or BLE the bigger drain?** Mark each separately.
- **Does the brace-counter loop in `write_cmd` take measurable time?** Mark it.
- **Does encrypted GATT cost more than plain ATT?** Toggle the encryption permissions, compare.

For a wall-powered fan controller, almost none of these matter — the fan motor dominates all firmware costs by orders of magnitude. For a battery-powered variant, this is exactly the workflow you'd run on every code change.

---

## Bring-up gotchas (encountered on this project)

Things that bit us during the first PPK2 setup. Surface them here so future-you doesn't waste an afternoon on the same dead-ends.

### `Lock Y-axis` makes a working capture look empty

The Power Profiler app's `Lock Y-axis` toggle (top-left) pins the chart's Y range. If it's locked at e.g. 0–500 nA but the firmware is drawing 1.4 mA, the trace lives entirely above the visible area — chart appears blank, even though the WINDOW stats panel correctly shows mA-range averages. **Fix**: toggle `Lock Y-axis` off so the chart auto-scales. Re-enable later once you've zoomed to a meaningful fixed range (e.g. 0–15 mA).

### Polarity-swapped wires give clamped readings, not negative

PPK2's reverse-current protection clamps to ~nA when VIN/VOUT are wired backwards. This presents identically to "PPK2 not in circuit" — the chip still gets *some* power (through the protection diodes) so BLE may even work, but the meter sees nothing. The first thing to try when readings look impossibly small is **swap the wires**.

Confirmed on this project: wires swapped → 1.5 nA average, 770 nA peak. Wires correct → 1.45 mA average, 11 mA peak. Three orders of magnitude difference from a wire-order mistake.

### Partial SB9 cut

Cutting SB9 looks easy but a hair-thin copper strip can remain even after a confident-feeling slice. Symptom: chip runs but PPK2 reads ~0. Verify with a multimeter in continuity mode — probe both sides of SB9 with the DK unpowered. Beep / < 5 Ω = trace still continuous, re-cut deeper until you can see beige FR4 substrate where copper used to be.

### "PPK2 connecting…" hangs

Common on macOS. Usually one of:
- Another process holding the J-Link interface (`pkill -9 JLinkExe nrfjprog`)
- PPK2 firmware update prompt hidden behind another window
- macOS USB permission gate not yet approved (System Settings → Privacy & Security)
- Bad USB cable (try a different one)

Restart the Power Profiler app fully (`killall "nRF Connect for Desktop"`) and replug the PPK2 cable.

---

## Worked example: first capture interpretation

This is what the firmware looked like in the *first* good capture, before any power optimization:

**Window**: 1.229 s, advertising only (no client connected)
**Y-axis**: 0–12 mA (auto-scaled)

```
        Current
   12 mA┤ ▲       ▲    ▲       ▲       ▲      ▲     ▲     ▲     ▲     ▲   ← BLE adv events,
   10 mA┤ █       █    █       █       █      █     █     █     █     █     ~10–11 mA peaks
    8 mA┤ █       █    █       █       █      █     █     █     █     █     every ~40 ms
    6 mA┤ █       █    █       █       █      █     █     █     █     █
    4 mA┤ █       █    █       █       █      █     █     █     █     █
    2 mA┤▌█▌▌▌▌▌▌▌█▌▌▌▌█▌▌▌▌▌▌▌█▌▌▌▌▌▌▌█▌▌▌▌▌▌█▌▌▌▌▌█▌▌▌▌▌█▌▌▌▌▌█▌▌▌▌▌█▌  ← noisy "fuzz"
    0  ┤▁██▁▁▁▁▁▁▁██▁▁▁██▁▁▁▁▁▁▁██▁▁▁▁██▁▁▁▁▁██▁▁▁▁██▁▁▁██▁▁▁██▁▁▁▁██▁▁     baseline at 0.5–2 mA
```

**Stats:**
- Average: **1.45 mA** ← dominated by baseline, not radio spikes
- Max: 11.01 mA ← single-sample peak during a TX
- Charge over 1.229 s: 1.78 mC

**Decomposition:**

```
Radio events:   ~10 mA × ~0.5 ms × 25 events/s ≈   125 µA contribution (~9%)
Baseline:       ~1.3 mA × ~98% of the time      ≈ 1300 µA contribution (~91%)
                                                  ─────────────────────
Total:                                          ≈ 1450 µA = 1.45 mA  ✓
```

**Diagnosis:** The radio cost is normal. **The baseline is wrong**. For a properly power-managed nRF52, the baseline between events should be ~5–20 µA, not 1–2 mA. The chip is staying in System ON Active mode between adv events instead of dropping into a deeper sleep state.

### Root cause — `CONFIG_PM=y` not in `prj.conf`

Zephyr's automatic CPU sleep needs the Power Management subsystem (`CONFIG_PM=y`). Without it, the kernel idle thread executes a basic `WFI` that only stops the CPU clock — peripheral clocks and most of the SoC stay on, drawing ~1–3 mA continuously. With `CONFIG_PM=y`, the idle thread coordinates with the SoC's PM peripheral to enter System ON Idle / Constant Latency / Low Power, dropping current to single µA.

**Verify the current state:**

```sh
grep -E "^CONFIG_PM" build/nrf52dk/zephyr/.config
```

**If not enabled, add to `prj.conf`:**

```
# Power management — let the kernel put the SoC into deep sleep
# between events. Without this, idle current is ~1.5 mA; with it,
# ~5 µA. Massive impact on battery-powered use cases.
CONFIG_PM=y
CONFIG_PM_DEVICE=y
```

Rebuild + reflash, re-capture. Expected change:

| Metric | Before `CONFIG_PM=y` | After |
|---|---|---|
| Average current (advertising only) | 1.45 mA | **80–150 µA** |
| Peak (radio TX) | 11 mA | 11 mA (unchanged) |
| Baseline between events | 1–2 mA | **5–20 µA** |
| Battery life on a 240 mAh CR2477 | ~7 days | **~3 months** |

Save both captures as CSV via `File → Save data`. Naming convention: `before_pm.csv`, `after_pm.csv`. Quote the numbers in the commit message.

### Other contributors to "high baseline"

If `CONFIG_PM=y` is already on and baseline is still elevated:

- **`CONFIG_SERIAL=y` + UART log backend** — keeps UART peripheral active continuously, ~500 µA. Test by setting `CONFIG_LOG_BACKEND_UART=n` temporarily.
- **PWM peripheral always-on** — even at 0 % duty, ~50 µA. Negligible but real.
- **A thread polling without `k_msleep`** — CPU never goes idle. Audit thread loops for `while (1) { check(); }` patterns.
- **Sensor driver in periodic-trigger mode** — keeps the SoC's GPIO interrupt latched. Single-shot mode (already on for SHT3x) avoids this.

---

## Sources

- [Power Profiler Kit II — official product page](https://www.nordicsemi.com/Products/Development-hardware/Power-Profiler-Kit-2)
- [Power Profiler Kit II — User Guide (Nordic docs)](https://docs.nordicsemi.com/bundle/ug_ppk2/page/UG/ppk/PPK_user_guide_Intro.html)
- [Preparing the nRF52 DK for current measurements (Nordic infocenter)](https://infocenter.nordicsemi.com/topic/ug_nrf52832_dk/UG/dk/prepare_board.html)
- [Current measurement guide: PCA10040 (Nordic DevZone)](https://devzone.nordicsemi.com/nordic/b/archives/posts/current-measurement-guide-measuring-current-with-p)
- [NRF52832 DK PCA10040 current measurement (DevZone)](https://devzone.nordicsemi.com/f/nordic-q-a/78131/nrf52832-dk---pca10040-current-measurement)
