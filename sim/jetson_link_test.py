#!/usr/bin/env python3
"""
Jetson<->ESP32 cift yonlu bring-up testi (SERI HAT, gercek donanim).

TEK KOMUTLA IKI YONU BIRDEN dogrular:
    1) Ilk ~3 sn SADECE dinler.
       STATUS akiyorsa  -> ESP32 -> Jetson yonu CALISIYOR (20 Hz telemetri).
       jetson_link=False olmali (ESP32 henuz bizi duymuyor).
    2) Sonra COMMAND + HEARTBEAT gonderir (10 Hz).
       jetson_link 500 ms icinde True olmali -> Jetson -> ESP32 yonu CALISIYOR.
       (Bu biti ESP32'nin KENDISI set eder; bizden geleni duydugunun kanitidir.)

Arac hareket etmez: sol=sag=0, AUTO_REQ gonderilmez.

Kablolama (CAPRAZ, 3.3V, seviye cevirici GEREKMEZ) -- ika_esp32 config.h:
    USB-TTL GND -> ESP32 GND
    USB-TTL TX  -> ESP32 GPIO21   (JETSON_RX_PIN)
    USB-TTL RX  <- ESP32 GPIO22   (JETSON_TX_PIN)
    USB-TTL VCC/3V3/5V -> BAGLANMAZ

ESP32'nin kendi USB portu ayri bir cihazdir (flash + Seri Monitor); protokol
oradan akmaz, bu testi USB-TTL'in portuna baglayin.

Kullanim:
    python3 sim/jetson_link_test.py /dev/ttyUSB0   # Jetson, USB-TTL
    python  sim/jetson_link_test.py COM22          # Windows, USB-TTL

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

# ESP32'nin donmus parser'da adi gecmeyen iki durum biti (controller.cpp).
ST_ARMED = 0x20
ST_HW_ERROR = 0x40
MODE_NAMES = {0: "DRIVE", 1: "LASER", 2: "AUTO"}


def main() -> None:
    ap = argparse.ArgumentParser(description="Jetson->ESP32 RX bring-up testi")
    ap.add_argument("port", help="seri port (/dev/ttyUSB0, COM22)")
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
                    print(f"[{now - t0:5.1f}s] ESP32'den STATUS YOK (+{rate}/s)  [{phase}]  "
                          f"-> GPIO22->USB-TTL RX / GND / baud / firmware kontrol")
                else:
                    link = last_status["jetson_link"]
                    mark = "   <== BASARILI (RX calisiyor)" if (sending and link) else ""
                    durum = last_status["durum"]
                    print(f"[{now - t0:5.1f}s] STATUS +{rate}/s  "
                          f"jetson_link={link!s:5}  "
                          f"mod={MODE_NAMES.get(last_status['aktif_mod'], '?'):6s} "
                          f"arm={bool(durum & ST_ARMED)!s:5} "
                          f"hw={bool(durum & ST_HW_ERROR)!s:5} "
                          f"durum=0x{durum:02X}  "
                          f"kayip={proto.lost}  [{phase}]{mark}")

            time.sleep(0.005)
    except KeyboardInterrupt:
        print("\n[i] durduruldu.")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
