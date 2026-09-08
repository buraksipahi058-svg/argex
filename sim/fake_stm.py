#!/usr/bin/env python3
"""
Fake vehicle controller — emits valid STATUS + HEARTBEAT frames so the whole
Base Station pipeline can be exercised WITHOUT the vehicle.

It builds frames exactly like the ESP32 firmware (ika_esp32 controller.cpp
`Proto_SendFrame` / `Proto_SendStatus` / `Proto_SendHeartbeat`), reusing the
FROZEN protocol's CRC and constants imported from needtocheck/jetson_parser.py
(never modified). Frames are sent over UDP to the gateway's `udp` source
(default 127.0.0.1:9000).

    python sim/fake_stm.py                 # default dynamic scenario, 20/10 Hz
    python sim/fake_stm.py --host 127.0.0.1 --port 9000

Scenario timeline (loops ~30 s) deterministically triggers each event type:
    mode changes (DRIVE/LASER/AUTO), autonomy engage/disengage, laser on/off,
    failsafe pulse, ELRS link drop (field), Jetson link drop (durum), CRC blips,
    motor arm/disarm, and a latched hardware-fault pulse.
Stop it (Ctrl-C) to test "STM telemetry stale"; stop the gateway to test
"base link lost".
"""
from __future__ import annotations

import argparse
import importlib.util
import math
import socket
import struct
import time
from pathlib import Path

# --- import frozen protocol constants + CRC (unmodified) --------------------
_FROZEN = Path(__file__).resolve().parent.parent / "needtocheck" / "jetson_parser.py"
_spec = importlib.util.spec_from_file_location("frozen_jetson_parser_sim", _FROZEN)
_p = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_p)

HDR0, HDR1, VERSION = _p.HDR0, _p.HDR1, _p.VERSION
TYPE_STATUS, TYPE_HEARTBEAT = _p.TYPE_STATUS, _p.TYPE_HEARTBEAT
TYPE_IMU = 0x04   # ESP32-only frame the frozen parser predates (see stm_reader._ImuTap)
HB_KAYNAK_STM = _p.HB_KAYNAK_STM
ST_JETSON_LINK, ST_CMD_TIMEOUT = _p.ST_JETSON_LINK, _p.ST_CMD_TIMEOUT
ST_AUTO_EN, ST_FAILSAFE, ST_CRC_ERR = _p.ST_AUTO_EN, _p.ST_FAILSAFE, _p.ST_CRC_ERR
crc16 = _p.crc16_ccitt

# Two durum bits the ESP32 firmware adds that the frozen parser predates; the
# gateway decodes them straight from the raw byte (jetson/mapper.py).
ST_ARMED = 0x20
ST_HW_ERROR = 0x40

# aktifMod values (controller.cpp VehicleMode).
MODE_DRIVE, MODE_LASER, MODE_AUTO = 0, 1, 2

# The ESP32 caps manual drive with CH6 (30 / 60 / 100 %); the sim holds the
# low gear, which is what the first field tests run at.
MAX_GUC = 30


class FrameBuilder:
    """Mirrors haberlesme.cpp frame construction. SEQ shared across types."""

    def __init__(self) -> None:
        self._seq = 0

    def _frame(self, type_: int, payload: bytes) -> bytes:
        body = bytes([VERSION, type_, len(payload), self._seq]) + payload
        self._seq = (self._seq + 1) & 0xFF
        crc = crc16(body)
        return bytes([HDR0, HDR1]) + body + struct.pack("<H", crc)

    def status(self, sol, sag, pan, tilt, lazer, mod, elrs, durum) -> bytes:
        payload = struct.pack("<bbBBBBBB", sol, sag, pan, tilt, lazer, mod, elrs, durum)
        return self._frame(TYPE_STATUS, payload)

    def heartbeat(self, uptime_ms) -> bytes:
        payload = struct.pack("<BI", HB_KAYNAK_STM, uptime_ms & 0xFFFFFFFF)
        return self._frame(TYPE_HEARTBEAT, payload)

    def imu(self, pitch_cdeg, yaw_cdeg, cal) -> bytes:
        payload = struct.pack("<hHB", pitch_cdeg, yaw_cdeg, cal)
        return self._frame(TYPE_IMU, payload)


def scenario(t: float) -> dict:
    """Return the STM's reported state at elapsed time t (seconds)."""
    phase = t % 30.0

    if 8.0 <= phase < 16.0:
        mode = MODE_LASER                             # CH5 down: turret + laser
    elif 3.0 <= phase < 7.0:
        mode = MODE_AUTO                              # CH8 down: Jetson drives
    else:
        mode = MODE_DRIVE                             # manual drive

    auto = mode == MODE_AUTO and 4.0 <= phase < 6.0   # AUTO_REQ accepted window
    failsafe = 20.0 <= phase < 22.0                   # failsafe pulse
    hw_fault = 28.0 <= phase < 29.0                   # latched controller fault
    elrs = not (25.0 <= phase < 27.0)                 # ELRS (RC) link drop (field)
    jlink = not (12.0 <= phase < 13.0)                # controller's view of Jetson link
    crc_blip = (int(phase) % 10 == 9) and (t * 5 % 1 < 0.2)  # occasional CRC blip

    # The firmware disarms the motors on every mode change and while RC is gone;
    # in turret mode ARM:0 is the normal, correct reading.
    armed = (mode in (MODE_DRIVE, MODE_AUTO)) and elrs and not failsafe and not hw_fault

    pan = int(90 + 40 * math.sin(t * 0.6))
    tilt = int(90 + 20 * math.sin(t * 0.4))

    if mode == MODE_DRIVE and armed:
        base = int(80 * math.sin(t * 0.8))
        turn = int(30 * math.sin(t * 1.3))
        sol = max(-100, min(100, base + turn)) * MAX_GUC // 100
        sag = max(-100, min(100, base - turn)) * MAX_GUC // 100
    elif mode == MODE_AUTO and auto:
        # Autonomy has no CH6 ceiling on the vehicle; the sim stays gentle.
        sol = sag = int(25 * math.sin(t * 0.5))
    else:                                             # turret / failsafe -> motors 0
        sol = sag = 0

    lazer = 1 if (mode == MODE_LASER and math.sin(t * 2.0) > 0 and not failsafe) else 0

    durum = ST_JETSON_LINK if jlink else 0
    if auto and not failsafe:
        durum |= ST_AUTO_EN
    if failsafe:
        durum |= ST_FAILSAFE
    if crc_blip:
        durum |= ST_CRC_ERR
    if armed:
        durum |= ST_ARMED
    if hw_fault:
        # The firmware raises FAILSAFE together with the fault latch.
        durum |= ST_HW_ERROR | ST_FAILSAFE

    # BNO055 attitude (TYPE_IMU): pitch wobbles, yaw sweeps as a heading; the
    # calibration bits climb to fully-calibrated after the first few seconds.
    imu_pitch = int(round(15.0 * math.sin(t * 0.5) * 100))     # +-15 deg -> deg*100
    imu_yaw = int(round(((t * 20.0) % 360.0) * 100))           # slow heading sweep
    sys_cal = 3 if phase >= 5.0 else 1
    imu_cal = (sys_cal << 6) | (3 << 4) | (3 << 2) | sys_cal

    return dict(sol=sol, sag=sag, pan=pan, tilt=tilt,
                lazer=lazer, mod=mode, elrs=1 if elrs else 0, durum=durum,
                imu_pitch=imu_pitch, imu_yaw=imu_yaw, imu_cal=imu_cal)


def main() -> None:
    ap = argparse.ArgumentParser(description="Fake vehicle telemetry emitter (UDP)")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=9000)
    ap.add_argument("--status-hz", type=float, default=20.0)
    ap.add_argument("--hb-hz", type=float, default=10.0)
    ap.add_argument("--imu-hz", type=float, default=20.0)
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dst = (args.host, args.port)
    fb = FrameBuilder()

    status_dt = 1.0 / args.status_hz
    hb_dt = 1.0 / args.hb_hz
    imu_dt = 1.0 / args.imu_hz if args.imu_hz > 0 else None
    t0 = time.time()
    next_status = t0
    next_hb = t0
    next_imu = t0

    print(f"fake_stm -> udp {args.host}:{args.port}  STATUS {args.status_hz}Hz  HB {args.hb_hz}Hz  IMU {args.imu_hz}Hz")
    try:
        while True:
            now = time.time()
            if now >= next_status:
                next_status += status_dt
                s = scenario(now - t0)
                sock.sendto(
                    fb.status(s["sol"], s["sag"], s["pan"], s["tilt"],
                              s["lazer"], s["mod"], s["elrs"], s["durum"]),
                    dst,
                )
            if now >= next_hb:
                next_hb += hb_dt
                sock.sendto(fb.heartbeat(int((now - t0) * 1000)), dst)
            if imu_dt is not None and now >= next_imu:
                next_imu += imu_dt
                s = scenario(now - t0)
                sock.sendto(fb.imu(s["imu_pitch"], s["imu_yaw"], s["imu_cal"]), dst)
            time.sleep(0.002)
    except KeyboardInterrupt:
        print("\nfake_stm stopped")


if __name__ == "__main__":
    main()
