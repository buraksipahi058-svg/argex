#!/usr/bin/env python3
"""
Fake STM32 — emits valid STATUS + HEARTBEAT frames so the whole Base Station
pipeline can be exercised WITHOUT the vehicle.

It builds frames exactly like the firmware (haberlesme.cpp `frameGonder` /
`telemetriGonder` / `heartbeatGonder`), reusing the FROZEN protocol's CRC and
constants imported from needtocheck/jetson_parser.py (never modified). Frames
are sent over UDP to the gateway's `udp` source (default 127.0.0.1:9000).

    python sim/fake_stm.py                 # default dynamic scenario, 20/10 Hz
    python sim/fake_stm.py --host 127.0.0.1 --port 9000

Scenario timeline (loops ~30 s) deterministically triggers each event type:
    mode changes, autonomy engage/disengage, laser on/off, failsafe pulse,
    ELRS link drop (field), STM<->Jetson link drop (durum), CRC blips.
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
HB_KAYNAK_STM = _p.HB_KAYNAK_STM
ST_JETSON_LINK, ST_CMD_TIMEOUT = _p.ST_JETSON_LINK, _p.ST_CMD_TIMEOUT
ST_AUTO_EN, ST_FAILSAFE, ST_CRC_ERR = _p.ST_AUTO_EN, _p.ST_FAILSAFE, _p.ST_CRC_ERR
crc16 = _p.crc16_ccitt

MAX_GUC = 50  # firmware caps applied motor output at 50 %


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


def scenario(t: float) -> dict:
    """Return the STM's reported state at elapsed time t (seconds)."""
    phase = t % 30.0

    mode = 1 if 8.0 <= phase < 16.0 else 0            # DRIVE / LASER window
    auto = 4.0 <= phase < 6.0                         # autonomy engaged window
    failsafe = 20.0 <= phase < 22.0                   # failsafe pulse
    elrs = not (25.0 <= phase < 27.0)                 # ELRS (RC) link drop (field)
    jlink = not (12.0 <= phase < 13.0)                # STM's view of Jetson link drop
    crc_blip = (int(phase) % 10 == 9) and (t * 5 % 1 < 0.2)  # occasional CRC blip

    pan = int(90 + 40 * math.sin(t * 0.6))
    tilt = int(90 + 20 * math.sin(t * 0.4))

    if mode == 0 and not failsafe:                    # DRIVE
        base = int(80 * math.sin(t * 0.8))
        turn = int(30 * math.sin(t * 1.3))
        sol = max(-100, min(100, base + turn)) * MAX_GUC // 100
        sag = max(-100, min(100, base - turn)) * MAX_GUC // 100
    else:                                             # LASER or failsafe -> motors 0
        sol = sag = 0

    lazer = 1 if (mode == 1 and math.sin(t * 2.0) > 0 and not failsafe) else 0

    durum = ST_JETSON_LINK if jlink else 0
    if auto and not failsafe:
        durum |= ST_AUTO_EN
    if failsafe:
        durum |= ST_FAILSAFE
    if crc_blip:
        durum |= ST_CRC_ERR

    return dict(sol=sol, sag=sag, pan=pan, tilt=tilt,
                lazer=lazer, mod=mode, elrs=1 if elrs else 0, durum=durum)


def main() -> None:
    ap = argparse.ArgumentParser(description="Fake STM32 telemetry emitter (UDP)")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=9000)
    ap.add_argument("--status-hz", type=float, default=20.0)
    ap.add_argument("--hb-hz", type=float, default=10.0)
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dst = (args.host, args.port)
    fb = FrameBuilder()

    status_dt = 1.0 / args.status_hz
    hb_dt = 1.0 / args.hb_hz
    t0 = time.time()
    next_status = t0
    next_hb = t0

    print(f"fake_stm -> udp {args.host}:{args.port}  STATUS {args.status_hz}Hz  HB {args.hb_hz}Hz")
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
            time.sleep(0.002)
    except KeyboardInterrupt:
        print("\nfake_stm stopped")


if __name__ == "__main__":
    main()
