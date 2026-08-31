#!/usr/bin/env python3
"""
Jetson<->STM RX bring-up testi (SERI HAT, gercek donanim).

STM'e COMMAND + HEARTBEAT gonderir ve STM'in geri yolladigi STATUS
telemetrisindeki `jetson_link` bitini izler:
    - Once ~3 sn SADECE dinler  -> jetson_link=False olmali (STM bizi duymuyor)
    - Sonra gondermeye baslar    -> jetson_link 500 ms icinde True olmali
Boylece Jetson->STM yolu (PC7 = USART6_RX) + COMMAND parser dogrulanir.

Kablolama (USB-TTL 3.3V <-> STM, CAPRAZ):
    adapter GND -> STM GND
    adapter TX  -> STM PC7   (USART6_RX)
    adapter RX  -> STM PC6   (USART6_TX)

Kullanim:
    python sim/jetson_link_test.py COM7           # Windows USB-TTL
    python sim/jetson_link_test.py /dev/ttyUSB0   # Linux/PC USB-TTL
    python sim/jetson_link_test.py /dev/ttyTHS1   # Jetson dahili UART

Gereksinim:  pip install pyserial
"""
from __future__ import annotations

import argparse
import importlib.util
import time
from pathlib import Path

try:
    import serial  # pyserial
except ImportError:  # pragma: no cover
    raise SystemExit("pyserial gerekli:  pip install pyserial")

# --- frozen protokol (needtocheck/jetson_parser.py) --------------------------
_FROZEN = Path(__file__).resolve().parent.parent / "needtocheck" / "jetson_parser.py"
_spec = importlib.util.spec_from_file_location("frozen_jetson_parser_link", _FROZEN)
_p = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_p)
Protocol = _p.Protocol
TYPE_STATUS = _p.TYPE_STATUS


def main() -> None:
    ap = argparse.ArgumentParser(description="Jetson->STM RX bring-up testi")
    ap.add_argument("port", help="seri port (COM7, /dev/ttyUSB0, /dev/ttyTHS1)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--listen", type=float, default=3.0,
                    help="gondermeden once yalniz dinleme suresi (sn)")
    args = ap.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=0)
    proto = Protocol()
    print(f"[i] {args.port} @{args.baud} acildi.")
    print(f"[i] Ilk {args.listen:.0f} sn SADECE dinleniyor "
          f"(jetson_link=False bekleniyor)...\n")

    t0 = time.time()
    t_send = 0.0
    sending = False
    last_link = None
    status_seen = 0

    try:
        while True:
            now = time.time()

            # 1) STM'den gelen telemetriyi coz, jetson_link degisimini bildir
            for pkt in proto.feed(ser.read(256)):
                if pkt["type"] == TYPE_STATUS:
                    status_seen += 1
                    s = pkt["status"]
                    if s["jetson_link"] != last_link:
                        print(f"[{now - t0:6.2f}s] STATUS  "
                              f"jetson_link={s['jetson_link']!s:5}  "
                              f"failsafe={s['failsafe']!s:5}  "
                              f"durum=0x{s['durum']:02X}  "
                              f"(STATUS x{status_seen}, kayip={proto.lost})")
                        last_link = s["jetson_link"]

            # 2) Dinleme bitince COMMAND+HEARTBEAT gondermeye basla (10 Hz)
            if not sending and (now - t0) >= args.listen:
                sending = True
                if status_seen == 0:
                    print("[!] Hic STATUS gelmedi! Kontrol: PC6->adapter RX yonu, "
                          "GND ortak mi, baud 115200 mi, firmware flash'landi mi.\n")
                print(">>> COMMAND + HEARTBEAT gonderiliyor (10 Hz)... "
                      "jetson_link True olmali\n")
            if sending and (now - t_send) >= 0.1:
                t_send = now
                ser.write(proto.build_heartbeat(uptime_ms=int((now - t0) * 1000)))
                ser.write(proto.build_command(sol=0, sag=0, pan=90, tilt=90))

            time.sleep(0.005)
    except KeyboardInterrupt:
        print("\n[i] durduruldu.")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
