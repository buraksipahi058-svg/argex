#!/usr/bin/env python3
"""
STM32 <-> Jetson binary UART protokolu - Jetson tarafi referansi.
Frame: AA 55 | VERSION | TYPE | LENGTH | SEQ | PAYLOAD | CRC_L CRC_H
CRC-16/CCITT-FALSE, kapsam VERSION..PAYLOAD. Cok baytli alanlar little-endian.

Kullanim (ornek):
    import serial
    ser = serial.Serial("/dev/ttyTHS1", 115200, timeout=0)   # Jetson UART pinine gore
    proto = Protocol()
    # Gonderme:
    ser.write(proto.build_command(sol=30, sag=30, pan=90, tilt=90,
                                  lazer=0, mod=None, auto_request=True))
    # Alma (loop icinde surekli cagir):
    for pkt in proto.feed(ser.read(256)):
        if pkt["type"] == TYPE_STATUS:
            print(pkt["status"])
"""

import struct

HDR0, HDR1        = 0xAA, 0x55
VERSION           = 0x01
TYPE_STATUS       = 0x01
TYPE_COMMAND      = 0x02
TYPE_HEARTBEAT    = 0x03

HB_KAYNAK_STM     = 0x00
HB_KAYNAK_JETSON  = 0x01

MOD_KOMUT_DEGISTIRME = 0xFF

# COMMAND bayrak bitleri
CMD_FLAG_AUTO_REQ = 0x01

# STATUS durum bitleri
ST_JETSON_LINK = 0x01
ST_CMD_TIMEOUT = 0x02
ST_AUTO_EN     = 0x04
ST_FAILSAFE    = 0x08
ST_CRC_ERR     = 0x10

MAX_PAYLOAD = 32


def crc16_ccitt(data: bytes) -> int:
    """CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, xorout 0x0000)."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


class Protocol:
    def __init__(self):
        self._buf = bytearray()
        self._tx_seq = 0
        self._rx_last_seq = None
        self.lost = 0            # tespit edilen paket kaybi

    # ---------------- GONDERME ----------------
    def _frame(self, type_: int, payload: bytes) -> bytes:
        assert len(payload) <= MAX_PAYLOAD
        head = bytes([VERSION, type_, len(payload), self._tx_seq])
        self._tx_seq = (self._tx_seq + 1) & 0xFF
        body = head + payload
        crc = crc16_ccitt(body)
        return bytes([HDR0, HDR1]) + body + struct.pack("<H", crc)

    def build_command(self, sol=0, sag=0, pan=90, tilt=90,
                      lazer=0, mod=None, auto_request=False) -> bytes:
        """COMMAND paketi. mod=None -> STM modu degistirme."""
        sol = max(-100, min(100, int(sol)))
        sag = max(-100, min(100, int(sag)))
        pan = max(0, min(180, int(pan)))
        tilt = max(0, min(180, int(tilt)))
        modv = MOD_KOMUT_DEGISTIRME if mod is None else int(mod)
        flags = CMD_FLAG_AUTO_REQ if auto_request else 0
        payload = struct.pack("<bbBBBBB", sol, sag, pan, tilt,
                              1 if lazer else 0, modv, flags)
        return self._frame(TYPE_COMMAND, payload)

    def build_heartbeat(self, uptime_ms=0) -> bytes:
        payload = struct.pack("<BI", HB_KAYNAK_JETSON, uptime_ms & 0xFFFFFFFF)
        return self._frame(TYPE_HEARTBEAT, payload)

    # ---------------- ALMA (streaming) ----------------
    def feed(self, chunk: bytes):
        """Gelen baytlari ekle, tam+CRC-gecerli paketleri uret (generator)."""
        self._buf.extend(chunk)
        while True:
            pkt = self._try_extract()
            if pkt is None:
                break
            yield pkt

    def _try_extract(self):
        b = self._buf
        # senkron ara
        while len(b) >= 2 and not (b[0] == HDR0 and b[1] == HDR1):
            del b[0]
        if len(b) < 6:
            return None
        length = b[4]
        if length > MAX_PAYLOAD:          # sacma len -> senkron kaydir
            del b[0]
            return None
        total = 6 + length + 2
        if len(b) < total:
            return None
        frame = bytes(b[:total])
        del b[:total]

        ver, type_, ln, seq = frame[2], frame[3], frame[4], frame[5]
        payload = frame[6:6 + ln]
        crc_rx = struct.unpack("<H", frame[6 + ln:8 + ln])[0]
        if crc16_ccitt(frame[2:6 + ln]) != crc_rx:
            return None                   # CRC hatasi -> paketi at

        # SEQ kayip takibi
        if self._rx_last_seq is not None:
            delta = (seq - self._rx_last_seq) & 0xFF
            if delta > 1:
                self.lost += delta - 1
        self._rx_last_seq = seq

        pkt = {"type": type_, "seq": seq, "version": ver}
        if type_ == TYPE_STATUS and ln >= 8:
            sol, sag, pan, tilt, lazer, mod, elrs, durum = \
                struct.unpack("<bbBBBBBB", payload[:8])
            pkt["status"] = {
                "sol_motor": sol, "sag_motor": sag,
                "pan": pan, "tilt": tilt,
                "lazer": lazer, "aktif_mod": mod,
                "elrs_link": elrs, "durum": durum,
                "jetson_link": bool(durum & ST_JETSON_LINK),
                "cmd_timeout": bool(durum & ST_CMD_TIMEOUT),
                "auto_enabled": bool(durum & ST_AUTO_EN),
                "failsafe": bool(durum & ST_FAILSAFE),
                "crc_err": bool(durum & ST_CRC_ERR),
            }
        elif type_ == TYPE_HEARTBEAT and ln >= 5:
            kaynak, uptime = struct.unpack("<BI", payload[:5])
            pkt["heartbeat"] = {"kaynak": kaynak, "uptime_ms": uptime}
        return pkt


if __name__ == "__main__":
    # Basit round-trip / CRC dogrulama
    p = Protocol()
    cmd = p.build_command(sol=30, sag=-30, pan=100, tilt=80,
                          lazer=1, mod=1, auto_request=True)
    print("COMMAND:", cmd.hex(" "))
    assert crc16_ccitt(b"123456789") == 0x29B1, "CRC-16/CCITT-FALSE dogrulamasi"
    print("CRC self-test OK")
