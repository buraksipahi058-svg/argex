#!/usr/bin/env python3
"""
Jetson<->STM RX bring-up testi (SERI HAT, gercek donanim).

STM'e COMMAND + HEARTBEAT gonderir ve STM'in geri yolladigi STATUS
telemetrisindeki `jetson_link` bitini 1 Hz canli izler:
    - Once ~3 sn SADECE dinler  -> jetson_link=False olmali (STM bizi duymuyor)
    - Sonra gondermeye baslar    -> jetson_link 500 ms icinde True olmali
Boylece Jetson->STM yolu (PC7 = USART6_RX) + COMMAND parser dogrulanir.

Kablolama (CAPRAZ, 3.3V):
    Jetson/adapter GND -> STM GND
    Jetson/adapter TX  -> STM PC7   (USART6_RX)   <-- bu yeni yon; takili mi?
    Jetson/adapter RX  -> STM PC6   (USART6_TX)

Kullanim:
    python3 sim/jetson_link_test.py /dev/ttyTHS1   # Jetson dahili UART
    python  sim/jetson_link_test.py COM7           # Windows USB-TTL

Gereksinim:  pip install pyserial  (Jetson: apt install python3-serial)
"""
from __future__ import annotations

import argparse
import importlib.util
import time
from pathlib import Path

try:
    import serial  # pyserial
except ImportError:  # pragma: no cover
    raise SystemExit("pyserial gerekli:  apt install python3-serial")

# --- frozen protokol (needtocheck/jetson_parser.py) --------------------------
_FROZEN = Path(__file__).resolve().parent.parent / "needtocheck" / "jetson_parser.py"
_spec = importlib.util.spec_from_file_location("frozen_jetson_parser_link", _FROZEN)
_p = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_p)
Protocol = _p.Protocol
TYPE_STATUS = _p.TYPE_STATUS


def main() -> None:
    ap = argparse.ArgumentParser(description="Jetson->STM RX bring-up testi")
    ap.add_argument("port", help="seri port (/dev/ttyTHS1, COM7, /dev/ttyUSB0)")
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
    t_report = t0
    sending = False
    status_total = 0
    status_prev = 0
    last_status = None

    try:
        while True:
            now = time.time()

            # 1) STM'den gelen STATUS'lari coz (en sonuncuyu tut)
            for pkt in proto.feed(ser.read(256)):
                if pkt["type"] == TYPE_STATUS:
                    status_total += 1
                    last_status = pkt["status"]

            # 2) Dinleme bitince COMMAND + HEARTBEAT gonder (10 Hz)
            if not sending and (now - t0) >= args.listen:
                sending = True
                print(">>> Artik COMMAND + HEARTBEAT gonderiliyor (10 Hz). "
                      "jetson_link True olmali.\n")
            if sending and (now - t_send) >= 0.1:
                t_send = now
                ser.write(proto.build_heartbeat(uptime_ms=int((now - t0) * 1000)))
                ser.write(proto.build_command(sol=0, sag=0, pan=90, tilt=90))

            # 3) 1 Hz canli rapor
            if now - t_report >= 1.0:
                t_report = now
                rate = status_total - status_prev
                status_prev = status_total
                phase = "GONDERIYOR" if sending else "dinliyor"
                if last_status is None:
                    print(f"[{now - t0:5.1f}s] STM'den STATUS YOK (+{rate}/s)  [{phase}]  "
                          f"-> PC6->Jetson RX / GND / baud / firmware kontrol")
                else:
                    link = last_status["jetson_link"]
                    mark = "   <== BASARILI (RX calisiyor)" if (sending and link) else ""
                    print(f"[{now - t0:5.1f}s] STATUS +{rate}/s  "
                          f"jetson_link={link!s:5}  "
                          f"durum=0x{last_status['durum']:02X}  "
                          f"kayip={proto.lost}  [{phase}]{mark}")

            time.sleep(0.005)
    except KeyboardInterrupt:
        print("\n[i] durduruldu.")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
