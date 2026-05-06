# Testing concepts

A reference covering three topics that came up while building the test infrastructure for this project: Unity vs Zephyr Twister, what a "smoke test" is (with a deep dive into ours), and what HIL means.

---

## 1. Unity vs Zephyr Twister

They're at different layers of the stack — comparing them directly is a bit like asking "what's the difference between `assert.h` and `cmake`". Let me clarify both, then show how they actually differ.

### What each one is

**Unity** ([ThrowTheSwitch/Unity](https://github.com/ThrowTheSwitch/Unity)) is a **unit-test framework** — a small C library (~1 file) you link into a test binary. It gives you:
- `TEST_ASSERT_*` macros (the equivalent of `zassert_*` or `ASSERT_*`)
- A simple test runner (`UNITY_BEGIN()` / `UNITY_END()`)
- Test fixtures (`setUp` / `tearDown` per test)

That's it. Unity is **the assertion library + a tiny runner**, nothing more.

**Twister** is **Zephyr's test orchestrator / build matrix runner** — a Python tool that lives in the Zephyr tree at `scripts/twister`. It:
- Discovers tests (anything with a `testcase.yaml`)
- Builds each test for one or more boards (could be hundreds)
- Runs the resulting binaries on simulators, real hardware, or QEMU
- Filters by tags, platforms, architectures, modules
- Parallelizes the build matrix
- Emits XML/JSON reports for CI

Twister doesn't ship its own assertion library. It runs **ztest** (Zephyr's framework — the equivalent of Unity, baked into the kernel tree).

### So the real comparison is two-axis

| Layer | Zephyr world | Throw The Switch world |
|---|---|---|
| **Assertion framework** (per-test) | ztest | **Unity** |
| **Mock generator** | (ztest_mock, limited) | CMock |
| **Build orchestrator** (multi-test, multi-target) | **Twister** | Ceedling |

Unity ↔ ztest is one comparison. Twister ↔ Ceedling is the other. People often blur "Unity-style testing" to mean "Unity + CMock + Ceedling" together, which is the full ThrowTheSwitch stack.

### Side-by-side: writing one test

**Unity:**
```c
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

void test_addition(void) {
    TEST_ASSERT_EQUAL_INT(4, 2 + 2);
    TEST_ASSERT_EQUAL_STRING("foo", get_name());
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_addition);
    return UNITY_END();
}
```

**ztest** (what runs *under* Twister):
```c
#include <zephyr/ztest.h>

ZTEST_SUITE(my_suite, NULL, NULL, NULL, NULL, NULL);

ZTEST(my_suite, test_addition) {
    zassert_equal(4, 2 + 2);
    zassert_str_equal("foo", get_name());
}
```

Same shape, slightly different macros. Both produce a binary that returns 0 on success, non-zero on failure.

### Side-by-side: running many tests

**Ceedling** (the Unity orchestrator):
```sh
ceedling test:all                      # discover + build + run every test
ceedling test:test_my_module           # one test
```

`project.yml` declares your modules, where headers live, what mocks to generate. Output is colorized PASS/FAIL across all tests.

**Twister:**
```sh
west twister -p native_sim -T tests/                         # all tests on native_sim
west twister -p nrf52dk/nrf52832 --tag bluetooth -T tests/   # filter by tag, real-board target
west twister -p native_sim -p nrf52dk/nrf52832 -T tests/     # run the matrix
```

`testcase.yaml` per test declares supported platforms, tags, harness, timeout. Output includes per-board PASS/FAIL plus a JUnit XML for CI.

### Where they differ in spirit

| Dimension | Unity (+ Ceedling/CMock) | Zephyr ztest (+ Twister) |
|---|---|---|
| **Scope** | Pure host-side unit tests of C modules | Whole-Zephyr-app builds across N boards |
| **Test execution** | Single native binary | Per-board binary (could be QEMU, real chip, or `native_sim`) |
| **Mock generation** | CMock auto-generates mocks from your headers (huge productivity win) | Mostly hand-rolled stubs; ztest_mock is rudimentary |
| **Hardware/RTOS dependencies** | Discouraged — modules under test should be HAL-mockable | First-class — you can test real Zephyr threads, drivers, BT host, etc. |
| **CI matrix** | Single platform, fast | Cross-board matrix, slow but comprehensive |
| **Coupling to project** | Framework lives in your repo (or as submodule) | Framework lives in Zephyr tree |
| **Typical test runtime** | Sub-millisecond per test | Sub-second per test on `native_sim`, seconds on QEMU, longer on hardware |
| **Where bugs end up** | Logic / algorithm bugs | Driver init order, BLE state-machine bugs, settings persistence, RTOS race conditions |

### When you'd use which

**Reach for Unity (+ Ceedling, + CMock)** when:
- You're testing pure-C modules (parsers, protocol encoders, state machines, math)
- You want sub-second feedback on every save
- The code under test has clean dependency boundaries you can mock
- You're not on Zephyr (FreeRTOS, bare-metal, etc.) — Unity is the de-facto choice there

**Reach for ztest + Twister** when:
- You're already on Zephyr and want to use the testing infrastructure that comes with it
- You need to test things that genuinely require the kernel (threads, work queues, settings, BT host)
- You care about the cross-board matrix — same test on `native_sim` + real hardware + QEMU
- You're integrating with Zephyr CI (the upstream tree itself runs everything through Twister)

### How this relates to *our* setup

What we built for this project is a **third path** — neither ztest+Twister nor Unity+Ceedling, but something simpler:

- A 100-line custom harness (`tests/unit/test_harness.{h,c}`) that's basically a tiny Unity clone — `TEST(name)` macro, `ASSERT_*` macros, auto-registration via `__attribute__((constructor))`.
- A bash wrapper (`scripts/run_tests.sh`) that's basically a tiny Ceedling clone — discovers our 3 suites, builds each with `cc`, runs them, summarizes.
- Zephyr-API shims (`tests/integration/persistence/fakes/`) for the integration test, since the real Zephyr settings subsystem can't compile on macOS in NCS v3.3.

Why we didn't just use Unity directly: same end result with one less external dependency (Unity isn't in the NCS toolchain, so we'd be vendoring or pulling it). The custom harness is small enough that maintaining it isn't a tax.

Why we didn't just use ztest+Twister: NCS v3.3 hard-blocks both `native_sim` and `unit_testing` boards on macOS, as we hit firsthand.

If we were on Linux-only, ztest+Twister would be the natural choice — fully integrated with the rest of the NCS toolchain, no third path needed.

---

## 2. The smoke test

### What "smoke test" means

The term comes from electronics manufacturing: power on the board for the first time, see if smoke comes out. If it doesn't, the device passes — you haven't proven it's *correct*, just that it's not catastrophically broken.

In software the meaning is the same: a **fast, end-to-end "is the basic thing alive?" check** that exercises real surfaces (real network, real DB, real BLE radio) but doesn't try to be exhaustive. You run it after every firmware flash, after every release build, after every infrastructure change.

For embedded BLE specifically, a smoke test answers: "does the chip boot, advertise, accept a connection, pair, expose its GATT service, and respond to one round-trip?" If yes → ship it for further testing. If no → don't bother running anything else, find out what's broken.

### What our `tests/hil/smoke.py` does

It's a Python script that runs on your laptop (any OS with BLE — macOS, Linux+BlueZ, Windows 10+) and talks to a flashed nRF52 DK over the air via the [bleak](https://github.com/hbldh/bleak) library. It walks the entire wire-contract checklist that the React frontend would perform on a phone, but does it from a script so you can rerun it without tapping at a UI.

Seven steps, each gated by a timeout. Any failure raises `FailedStep` with a clear message naming the step.

#### Step 1 — Scan by name prefix

```python
def matcher(dev, adv) -> bool:
    name = adv.local_name or dev.name or ""
    return name.startswith(prefix)
dev = await BleakScanner.find_device_by_filter(matcher, timeout=10s)
```

The OS BLE stack does the actual radio scanning; we install a Python predicate that returns `True` for the first device whose advertised local name begins with `"CoolingDock"`. This mirrors what the frontend's BLE plugin (`@capacitor-community/bluetooth-le`) does: name-prefix filter, not service-UUID filter. If our `bt_set_name(...)` ever produces a name not starting with `"CoolingDock"`, this scan returns nothing — that's the first thing the smoke test would catch.

If 10 s elapses without a match: `FailedStep("no device advertising with prefix...")`.

#### Step 2 — Connect & implicit pairing

```python
async with BleakClient(dev) as client:
```

`BleakClient` opens a GATT connection. We don't explicitly pair anywhere — that's intentional. The frontend doesn't pair explicitly either. The pairing happens automatically when we hit our first encrypted operation in step 3, because the firmware declares all its characteristics with `BT_GATT_PERM_*_ENCRYPT`. The host BLE stack sees "you need an encrypted link to read this" and triggers just-works pairing transparently. On macOS / iOS / Android you might see a system prompt the very first time; subsequent connects use the stored bond.

#### Step 3 — Read DevInfo + PrpConf

```python
raw = await client.read_gatt_char(DEVINFO_UUID)
devinfo = parse_json(raw, "DevInfo")
expect_keys(devinfo, ("cmd", "name", "fw", "uid", "board"), "DevInfo")
```

These two reads each return a JSON string. We:
1. Trigger the read (which forces the just-works pairing if not already bonded).
2. Decode UTF-8 → `json.loads`. Anything that's not valid JSON → `FailedStep`.
3. Assert all expected keys are present (`cmd`, `name`, `fw`, `uid`, `board` for DevInfo; `cmd`, `sets` for PrpConf).
4. Assert `cmd` field has the right magic value (`"GetDeviceInfo"` etc.).
5. For PrpConf, also assert `sets` is a non-empty list and `sets[0]` has `index`, `type`, `mode`.

This catches: schema regressions, characteristic UUID renames, JSON encoder bugs, missing fields, encryption-permission misconfig (the read would fail entirely with an "insufficient authentication" GATT error if perms were wrong).

#### Step 4 — Subscribe to Status

```python
statuses: asyncio.Queue[dict] = asyncio.Queue()
def on_notify(_handle: int, data: bytearray) -> None:
    statuses.put_nowait(parse_json(bytes(data), "status notify"))
await client.start_notify(STATUS_UUID, on_notify)
```

GATT notifications work as: client subscribes by writing the CCC descriptor; firmware then pushes data without being asked. Our firmware's `notify_thread` pushes a status JSON every 500 ms while a peer is subscribed. Bleak's `start_notify` writes the CCC for us and registers a callback.

The callback runs in bleak's internal thread context. We don't do any work there — just `parse_json` and `put_nowait` into an `asyncio.Queue`. The main flow `await`s on `statuses.get()` in the right places.

We then wait up to 3 s for the **first** status frame:

```python
first = await asyncio.wait_for(statuses.get(), STATUS_TIMEOUT_S)
```

If nothing arrives in 3 s → the firmware's notify thread isn't running, or CCC writes are silently failing. Catches a whole class of "BLE connected but firmware quiet" bugs.

The first frame is also schema-checked: must have `cmd`, `ms`, `sensors`, `prf` keys, and `cmd` must equal `"GetStatusData"`. The smoke test logs the master state and current PWM percent at this point.

#### Step 5 — Send a SetPeripheralConfig write

```python
cmd = json.dumps({
    "cmd": "SetPeripheralConfig",
    "index": 0,
    "mode": "sensor",
    "thr1": 24,
    "thr2": 31,
    "retfmt": "json",
}).encode("utf-8")

while not statuses.empty():
    statuses.get_nowait()                          # drain pre-existing status frames

await client.write_gatt_char(CMD_UUID, cmd, response=False)
```

The pre-write queue drain is important: we want to inspect status frames that arrive *after* the write, not whatever was already queued from before. Best-effort — we don't synchronize precisely; the timeout in step 6 forgives any race.

`response=False` does an ATT Write Without Response (faster, no ack) which is what the frontend uses. The firmware's `write_cmd` callback runs the brace-counting accumulator and queues the message to `cmd_q`.

The thresholds we pick (24/31) are intentionally distinctive — they shouldn't match any persisted defaults, so the verification in step 7 actually proves the change happened.

#### Step 6 — Wait for UpdateRet

This is the trickiest part. The firmware's response to a mutating command (`UpdateRet`) comes back **on the same Status notify channel** as periodic status updates — that's the wire contract. So we're watching a stream of frames where most are `cmd: "GetStatusData"` and the one we care about is `cmd: "UpdateRet"`. We loop with a deadline:

```python
deadline = now + 5s
while now < deadline:
    msg = await asyncio.wait_for(statuses.get(), remaining)
    if msg.get("cmd") == "UpdateRet":
        update_ret = msg
        break
    # else periodic status — keep waiting
```

If 5 s elapses without an `UpdateRet`: command was rejected silently, dispatch thread is stuck, msgq full, or the write never made it. We then check `update_ret.code != 0` — that's the firmware reporting a parse/validation error (e.g., `URET_BAD_REQ=3` if we accidentally sent malformed JSON).

#### Step 7 — Read PrpConf and verify the change stuck

```python
raw = await client.read_gatt_char(PRPCONF_UUID)
prpconf2 = parse_json(raw, "PrpConf (post-set)")
s = prpconf2["sets"][0]
if s.get("mode") != "sensor" or s.get("thr1") != 24 or s.get("thr2") != 31:
    raise FailedStep(...)
```

Final and most important assertion: read the persisted config back and check it actually contains what we sent. This proves the full pipeline:

```
ATT write → write_cmd callback → cmd_q → ble_cmd thread →
cmd_interpreter → handle_set_peripheral → sys_data_set_prf →
settings_save_one (NVS) → ... later read_prpconf → 
sys_data_get_prf → jsf_prpconf → ATT read response → bleak → us
```

If anywhere in that chain something silently drops the change, this assertion catches it.

### What the smoke test deliberately does NOT do

- **Not exhaustive.** It sends ONE command type with ONE set of args. It doesn't try every mode (POWER/FIXED/SENSOR/CYCLE/SCHEDULE), every error path, every edge case. Those are unit tests' job.
- **Not stress.** No throughput measurements, no MTU edge cases, no rapid back-to-back writes, no disconnect/reconnect loops.
- **Not security.** Doesn't validate that pairing is actually LE Secure Connections vs Legacy, doesn't check key strengths, doesn't try unauthenticated reads.
- **Not OTA.** Doesn't exercise `Restart` or `RestoreFactory` because they reboot the chip and break the connection mid-test.
- **No multi-device.** One peer, one connection at a time.

It's deliberately scoped to **"the BLE wire contract works end-to-end against real silicon"**. That's what makes it a smoke test and not an acceptance test.

### When you'd run it

After `west flash`, before considering a build "ready". Specifically:

```sh
west build -b nrf52dk/nrf52832 -p always .
west flash
cd tests/hil && python smoke.py
```

If it passes, you've proven the basic happy path against actual hardware. If it fails, the failure message tells you which of the seven steps broke, and you debug with that as a starting point.

### Common failure modes and what they mean

| Failure | What's likely wrong |
|---|---|
| `no device advertising with prefix...` | Firmware not running, advertising name doesn't start with `CoolingDock`, BONDED_ONLY mode + no prior bond, board not powered |
| `connect failed` | Pairing process broken, link layer issue, reset between scan and connect |
| `DevInfo: not valid JSON` | `jsf_devinfo` regression, encryption issue producing garbled bytes, MTU truncation |
| `no status notification within 3s` | `notify_thread` not running, CCC write failing, `attr_status_value` index wrong |
| `no UpdateRet received within 5s` | `cmd_q` full, `ble_cmd` thread not spawned, JSON brace-counter broken on the write side, `cmd_interpreter` returning `n <= 0` |
| `post-set PrpConf doesn't reflect change` | Persistence broken: `sys_data_set_prf` not committing, `settings_save_one` returning error silently, or the read path not reflecting in-memory state |

So if you ever see "post-set PrpConf doesn't reflect change", you know to look in the persistence layer specifically — not anywhere upstream of it. That's the value of structuring the smoke test as a sequence of distinct steps.

---

## 3. HIL — Hardware-in-the-Loop

**HIL = Hardware-in-the-Loop.** A test where the actual physical chip (or board, or full product) is part of the test loop — not simulated, not emulated, not mocked. Real silicon, running real firmware, talking to a test harness over its real interfaces (USB, UART, BLE, CAN, ethernet, GPIO, whatever).

The opposite is **SIL** (software-in-the-loop) where the firmware runs in a simulator/emulator on the test host, and **MIL** (model-in-the-loop) where you don't even have firmware yet — you just have a behavioral model.

### The progression

```
unit tests (host)        → fastest, pure logic, can mock everything
                ↓
SIL  / native_sim / QEMU → real firmware code, simulated peripherals
                ↓
HIL                      → real firmware on real chip, real peripherals
                ↓
field telemetry          → real firmware in real customers' hands
```

Each step gets slower, costs more, and finds different bugs. Each step is also irreplaceable — there are bug classes that only show up at one specific level. HIL is the last step before "shipping to customers" where you can still iterate quickly.

### What HIL catches that nothing else can

- **Timing bugs that depend on real silicon clocks.** A `k_msleep(100)` in firmware vs simulation behaves differently when interrupts, DMA transfers, and BLE radio events compete for the CPU.
- **Hardware peripheral quirks.** Errata in the SoC, sensor I2C edge cases, PWM peripheral behavior at boundaries — only the actual chip runs into these.
- **Power / electrical issues.** Glitches at startup, brown-out behavior, current draw at specific PWM duty cycles.
- **Real RF behavior.** BLE pairing on real radios, signal strength, interference, antenna effects — completely absent from any simulator.
- **Bond storage on real flash.** Wear leveling, settings backend behavior across power cycles. NVS in simulation uses RAM; real flash has retention/erase characteristics.
- **Cross-stack integration.** macOS BLE host stack ↔ Nordic SoftDevice Controller — only the real handshake exposes wire-protocol mismatches.

### What our `tests/hil/smoke.py` is — concretely

It's the simplest tier of HIL: **scripted host-side automation talking to a real chip over the chip's primary interface (BLE)**. Setup:

```
                 BLE radio (real RF)
  ┌─────────────────────────┐    ┌──────────────────────────┐
  │  macOS / Linux laptop   │ ◄──│  nRF52 DK + flashed FW   │
  │  python tests/hil/      │    │  CoolingDockNRF       │
  │    smoke.py             │    │  advertising             │
  │  (bleak)                │ ──►│                          │
  └─────────────────────────┘    └──────────────────────────┘
            │                                │
            └─────────── USB ────────────────┘
              (only for flashing the FW;
               not used during the test)
```

The laptop's role: act as the test driver. The DK's role: be the device under test. Bleak does what a phone would do during normal operation, except deterministically and scripted.

### HIL setups, scaled up

Real industrial HIL setups range from tiny to massive:

| Tier | Example | Equipment |
|---|---|---|
| **Bench HIL** (what we have) | Run smoke.py against a DK plugged into your laptop | DK + USB cable + laptop |
| **Scripted HIL** | Multiple boards in a rack, each with a unique test fixture, runs nightly | Boards + relays + serial-over-IP + power-cycler boxes |
| **Closed-loop HIL** | Firmware drives motors; encoders feed back to the test harness which validates trajectories | Motors, sensors, signal generators, real-time scope |
| **Full-system HIL** | Tesla Autopilot HIL | Full vehicle ECU stack on a bench, simulated wheel sensors, GPS, lidar, camera streams |
| **Vehicle-on-rollers HIL** | Automotive durability testing | Whole car on a chassis dyno, ECU under test, simulated road conditions |

A car company spends millions on HIL labs. We're spending one nRF52 DK and a USB cable. Same idea, vastly different price tag.

### Why "smoke" + HIL go together

You generally don't run *exhaustive* tests on HIL — they're slow and the test rig is a shared resource. You run **smoke** (does it boot, does the happy path work?) and **regression for known bugs** (this CVE used to crash; verify it doesn't anymore).

Exhaustive testing happens at lower tiers (unit tests on host, SIL in `native_sim`/QEMU). HIL is your "is reality consistent with our model of reality?" check.

### In our project's context

| Tier | Where bugs are caught |
|---|---|
| Unit (host, cc) | JSON parser, control hysteresis math |
| Integration (host, shimmed Zephyr) | cmd dispatch + persistence pipeline |
| **HIL smoke (real DK + laptop BLE)** | **BLE stack actually paired, advertised, exchanged, persisted-to-flash on real silicon** |
| Field (a phone running the React app) | Real-world UX bugs, edge cases nobody anticipated |

If your unit tests pass but HIL fails: something specific to the chip is wrong (drivers, BT stack, real flash persistence). If HIL passes but the React app sees errors: something specific to the OS BLE stack on the user's phone is involved. Each tier narrows the search.

That's why every commercial embedded product has at least one HIL test even if it's just a tech intern manually running the device through a checklist before each release. We've automated ours so it runs as a single command.
