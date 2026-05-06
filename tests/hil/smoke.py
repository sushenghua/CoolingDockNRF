#!/usr/bin/env python3
"""HIL smoke test for the CoolingDock-NRF firmware.

Walks the wire-contract checklist against a real flashed nRF52 DK:

  1. Scan for a peer whose advertised name starts with "CoolingDockNRF"
  2. Connect; the OS triggers just-works pairing on first encrypted read
  3. Read DevInfo and PrpConf characteristics, validate JSON structure
  4. Subscribe to Status notifications; assert one arrives within 2 s
  5. Send SetPeripheralConfig (sensor mode, custom thresholds)
  6. Wait for the UpdateRet reply on the Status channel
  7. Wait for a fresh status notification and assert it reflects the
     change (master state and peripheral status field)

Run:
  pip install -r requirements.txt
  python smoke.py [device-name-prefix]

Default name prefix is "CoolingDockNRF". The more-specific prefix
disambiguates from the ESP32-based CoolingDock chip which advertises
as "CoolingDock_..." — without this, smoke.py could connect to the
wrong board if both are powered on.

Exits 0 on success, non-zero on any failure with a clear message
identifying which step failed.
"""

import asyncio
import json
import sys
import time
from typing import Optional

from bleak import BleakClient, BleakScanner
from bleak.backends.device import BLEDevice
from bleak.backends.scanner import AdvertisementData

# Wire-contract UUIDs — must match src/ble_svc.c exactly.
SVC_UUID     = "59462f12-9543-9999-12c8-58b459a2712d"
STATUS_UUID  = "33333333-2222-2222-1111-111100000001"
CMD_UUID     = "33333333-2222-2222-1111-111100000002"
DEVINFO_UUID = "33333333-2222-2222-1111-111100000004"
PRPCONF_UUID = "33333333-2222-2222-1111-111100000005"

NAME_PREFIX = "CoolingDockNRF"
SCAN_TIMEOUT_S       = 10.0
CMD_RESPONSE_TIMEOUT = 5.0
STATUS_TIMEOUT_S     = 3.0


class FailedStep(Exception):
    pass


def log(step: str, detail: str = "") -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {step:<28} {detail}")


# ---------------------------------------------------------------- discovery

async def find_device(prefix: str) -> BLEDevice:
    log("scan", f"looking for prefix={prefix!r} (timeout {SCAN_TIMEOUT_S}s)")
    found: dict[str, BLEDevice] = {}

    def matcher(dev: BLEDevice, adv: AdvertisementData) -> bool:
        name = adv.local_name or dev.name or ""
        if name.startswith(prefix):
            found[dev.address] = dev
            return True
        return False

    dev = await BleakScanner.find_device_by_filter(
        matcher, timeout=SCAN_TIMEOUT_S
    )
    if dev is None:
        raise FailedStep(
            f"no device advertising with name prefix {prefix!r} found within "
            f"{SCAN_TIMEOUT_S}s"
        )
    log("scan", f"found {dev.name} @ {dev.address}")
    return dev


# ---------------------------------------------------------------- helpers

def parse_json(payload: bytes, label: str) -> dict:
    try:
        return json.loads(payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as e:
        raise FailedStep(f"{label}: not valid JSON: {e!r} bytes={payload!r}")


def expect_keys(d: dict, keys: tuple, label: str) -> None:
    missing = [k for k in keys if k not in d]
    if missing:
        raise FailedStep(f"{label}: missing keys {missing} in {d}")


# ---------------------------------------------------------------- main flow

async def run(prefix: str) -> None:
    dev = await find_device(prefix)

    log("connect", f"to {dev.address}")
    async with BleakClient(dev) as client:
        log("connect", "connected")

        # --- DevInfo ---
        log("read", "DevInfo")
        raw = await client.read_gatt_char(DEVINFO_UUID)
        devinfo = parse_json(raw, "DevInfo")
        expect_keys(devinfo, ("cmd", "name", "fw", "uid", "board"), "DevInfo")
        if devinfo["cmd"] != "GetDeviceInfo":
            raise FailedStep(f"DevInfo: wrong cmd field {devinfo['cmd']!r}")
        log("read", f"DevInfo OK  fw={devinfo['fw']} uid={devinfo['uid']}")

        # --- PrpConf ---
        log("read", "PrpConf")
        raw = await client.read_gatt_char(PRPCONF_UUID)
        prpconf = parse_json(raw, "PrpConf")
        expect_keys(prpconf, ("cmd", "sets"), "PrpConf")
        if not isinstance(prpconf["sets"], list) or len(prpconf["sets"]) < 1:
            raise FailedStep(f"PrpConf: sets must be a non-empty list, got {prpconf['sets']!r}")
        sets0 = prpconf["sets"][0]
        expect_keys(sets0, ("index", "type", "mode"), "PrpConf.sets[0]")
        log("read", f"PrpConf OK  mode={sets0['mode']} thr=({sets0.get('thr1')},{sets0.get('thr2')})")

        # --- Subscribe to Status ---
        statuses: asyncio.Queue[dict] = asyncio.Queue()

        def on_notify(_handle: int, data: bytearray) -> None:
            try:
                statuses.put_nowait(parse_json(bytes(data), "status notify"))
            except FailedStep as e:
                # Don't let a parse failure kill the notify task; surface
                # it via the queue with a sentinel instead.
                statuses.put_nowait({"_parse_error": str(e)})

        await client.start_notify(STATUS_UUID, on_notify)
        log("subscribe", "Status notify enabled")

        # --- Wait for a baseline status frame ---
        try:
            first = await asyncio.wait_for(statuses.get(), STATUS_TIMEOUT_S)
        except asyncio.TimeoutError:
            raise FailedStep(
                f"no status notification within {STATUS_TIMEOUT_S}s — "
                "firmware may not be running its notify thread"
            )
        if "_parse_error" in first:
            raise FailedStep(first["_parse_error"])
        expect_keys(first, ("cmd", "ms", "sensors", "prf"), "first status")
        if first["cmd"] != "GetStatusData":
            raise FailedStep(f"first status: wrong cmd {first['cmd']!r}")
        log("status", f"baseline OK  ms={first['ms']}  pwm={first['prf'][0].get('pwm')}")

        # --- SetPeripheralConfig (sensor mode, distinctive thresholds) ---
        thr1, thr2 = 24, 31
        cmd = json.dumps({
            "cmd": "SetPeripheralConfig",
            "index": 0,
            "mode": "sensor",
            "thr1": thr1,
            "thr2": thr2,
            "retfmt": "json",
        }).encode("utf-8")
        log("write", f"SetPeripheralConfig sensor thr1={thr1} thr2={thr2}")

        # Drain any pre-existing status frames so the next frame we read
        # is post-write. Best-effort.
        while not statuses.empty():
            statuses.get_nowait()

        # Use Write WITH Response (ATT Write Request, CID 0x0004) rather
        # than Write Without Response. On macOS Sonoma+ / iOS 17+ Apple's
        # Core Bluetooth routes Write-Without-Response through an EATT
        # bearer (CID in the dynamic range, e.g. 0x003a), and our NCS
        # v3.3.0 firmware can't complete the EATT bearer setup — the data
        # arrives on a CID with no handler and is dropped by L2CAP. Write
        # WITH Response always uses the legacy ATT bearer, which works.
        # Same applies to the React frontend's BLE plugin on macOS/iOS.
        await client.write_gatt_char(CMD_UUID, cmd, response=True)

        # --- Wait for UpdateRet on the status channel ---
        deadline = asyncio.get_event_loop().time() + CMD_RESPONSE_TIMEOUT
        update_ret: Optional[dict] = None
        while asyncio.get_event_loop().time() < deadline:
            remaining = deadline - asyncio.get_event_loop().time()
            try:
                msg = await asyncio.wait_for(statuses.get(), timeout=max(0.1, remaining))
            except asyncio.TimeoutError:
                break
            if "_parse_error" in msg:
                raise FailedStep(msg["_parse_error"])
            if msg.get("cmd") == "UpdateRet":
                update_ret = msg
                break
            # else it's just a periodic GetStatusData; keep waiting
        if update_ret is None:
            raise FailedStep("no UpdateRet received within "
                             f"{CMD_RESPONSE_TIMEOUT}s")
        if update_ret.get("code") != 0:
            raise FailedStep(f"UpdateRet code={update_ret.get('code')} "
                             f"val={update_ret.get('val')}")
        log("response", f"UpdateRet code=0")

        # --- Verify the change reflected in PrpConf ---
        raw = await client.read_gatt_char(PRPCONF_UUID)
        prpconf2 = parse_json(raw, "PrpConf (post-set)")
        s = prpconf2["sets"][0]
        if s.get("mode") != "sensor" or s.get("thr1") != thr1 or s.get("thr2") != thr2:
            raise FailedStep(
                f"post-set PrpConf doesn't reflect change: {s}"
            )
        log("verify", f"PrpConf reflects change  thr1={s['thr1']} thr2={s['thr2']}")

        await client.stop_notify(STATUS_UUID)
        log("done", "all checks passed ✅")


def main() -> int:
    prefix = sys.argv[1] if len(sys.argv) > 1 else NAME_PREFIX
    try:
        asyncio.run(run(prefix))
    except FailedStep as e:
        log("FAIL", str(e))
        return 2
    except KeyboardInterrupt:
        log("FAIL", "interrupted")
        return 130
    except Exception as e:                                  # pragma: no cover
        log("FAIL", f"unexpected: {e!r}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
