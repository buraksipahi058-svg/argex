"""
Observe the FROZEN STM32->Jetson STATUS/HEARTBEAT stream and yield decoded
packets.

IMPORTANT:
  * This module DECODES only. It reuses the unmodified reference parser at
    needtocheck/jetson_parser.py (imported from its frozen location, never
    edited or copied). We use only Protocol.feed() -- the receive/decode path.
    We never call build_command / build_heartbeat, and never write to the STM.
  * The Base Station read-only guarantee lives at the application layer. This
    observer does not participate in vehicle control and does not disable the
    existing Jetson->STM heartbeat/autonomy traffic that the firmware relies on.

Sources:
  * "serial" : read-only open of a UART carrying the STM stream (real vehicle).
  * "udp"    : raw STM protocol bytes over UDP (sim/fake_stm, or an on-Jetson
               IPC tap where the existing process re-publishes frames it reads).
"""
from __future__ import annotations

import asyncio
import importlib.util
import struct
from pathlib import Path
from typing import AsyncIterator, Dict, Iterator

from .config import SourceConfig

# --- Import the frozen reference parser from its original location ----------
_FROZEN_PARSER = Path(__file__).resolve().parent.parent / "needtocheck" / "jetson_parser.py"


def _load_frozen_parser():
    spec = importlib.util.spec_from_file_location("frozen_jetson_parser", _FROZEN_PARSER)
    if spec is None or spec.loader is None:
        raise ImportError(f"Cannot load frozen parser at {_FROZEN_PARSER}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


frozen = _load_frozen_parser()

# Re-export the frozen protocol constants so the rest of the gateway references
# the single source of truth rather than duplicating magic numbers.
Protocol = frozen.Protocol
TYPE_STATUS = frozen.TYPE_STATUS
TYPE_HEARTBEAT = frozen.TYPE_HEARTBEAT
TYPE_COMMAND = frozen.TYPE_COMMAND
HB_KAYNAK_STM = frozen.HB_KAYNAK_STM
HB_KAYNAK_JETSON = frozen.HB_KAYNAK_JETSON

# TYPE_IMU (0x04) is an ESP32-only addition the frozen parser predates. That
# parser yields the frame (type/seq) but DROPS its payload, and it must not be
# edited. So we recover pitch/yaw here from the same read-only byte stream with
# a tiny second framer, mirroring how mapper.py decodes the ESP32-only durum
# bits from the raw byte. Framing/CRC/constants are still the frozen ones.
TYPE_IMU = 0x04


class _ImuTap:
    """Decode ONLY TYPE_IMU (0x04) frames from the raw STM stream.

    Runs in parallel with the frozen Protocol on the same bytes (its own
    buffer), so it never touches the frozen decoder or its SEQ-loss accounting
    (the frozen parser already counts IMU frames in the shared SEQ sequence).
    """

    def __init__(self) -> None:
        self._buf = bytearray()

    def feed(self, chunk: bytes) -> Iterator[Dict]:
        b = self._buf
        b.extend(chunk)
        while True:
            while len(b) >= 2 and not (b[0] == frozen.HDR0 and b[1] == frozen.HDR1):
                del b[0]
            if len(b) < 6:
                return
            length = b[4]
            if length > frozen.MAX_PAYLOAD:   # bogus len -> resync
                del b[0]
                continue
            total = 6 + length + 2
            if len(b) < total:
                return
            frame = bytes(b[:total])
            del b[:total]

            type_, ln = frame[3], frame[4]
            payload = frame[6:6 + ln]
            crc_rx = struct.unpack("<H", frame[6 + ln:8 + ln])[0]
            if frozen.crc16_ccitt(frame[2:6 + ln]) != crc_rx:
                continue                       # CRC fail -> drop (frozen parser does the same)
            if type_ == TYPE_IMU and ln >= 5:
                pitch_c, yaw_c, cal = struct.unpack("<hHB", payload[:5])
                yield {
                    "type": TYPE_IMU,
                    "seq": frame[5],
                    "version": frame[2],
                    "imu": {
                        "pitch_deg": pitch_c / 100.0,
                        "yaw_deg": yaw_c / 100.0,
                        "cal_sys": (cal >> 6) & 3,
                        "cal_gyro": (cal >> 4) & 3,
                        "cal_accel": (cal >> 2) & 3,
                        "cal_mag": cal & 3,
                    },
                }


class _UdpProtocol(asyncio.DatagramProtocol):
    def __init__(self, queue: "asyncio.Queue[bytes]") -> None:
        self._queue = queue

    def datagram_received(self, data: bytes, addr) -> None:
        self._queue.put_nowait(data)


class StmReader:
    """Yields decoded packet dicts (as produced by the frozen Protocol.feed)."""

    def __init__(self, cfg: SourceConfig) -> None:
        self._cfg = cfg
        self._proto = Protocol()  # frozen decoder (framing + CRC + SEQ loss)
        self._imu = _ImuTap()     # non-frozen: recovers TYPE_IMU payloads

    def _decode(self, chunk: bytes) -> Iterator[Dict]:
        # Frozen parser: STATUS / HEARTBEAT (and unknown-type stubs it drops).
        for pkt in self._proto.feed(chunk):
            yield pkt
        # Parallel tap: TYPE_IMU payloads the frozen parser does not surface.
        for pkt in self._imu.feed(chunk):
            yield pkt

    @property
    def packets_lost(self) -> int:
        """Cumulative SEQ-gap loss counted by the frozen parser."""
        return self._proto.lost

    async def packets(self) -> AsyncIterator[Dict]:
        if self._cfg.type == "serial":
            async for pkt in self._serial_packets():
                yield pkt
        elif self._cfg.type == "udp":
            async for pkt in self._udp_packets():
                yield pkt
        else:
            raise ValueError(f"Unknown source type: {self._cfg.type!r}")

    # --- UDP source (sim / IPC tap) -----------------------------------------
    async def _udp_packets(self) -> AsyncIterator[Dict]:
        loop = asyncio.get_running_loop()
        queue: "asyncio.Queue[bytes]" = asyncio.Queue()
        transport, _ = await loop.create_datagram_endpoint(
            lambda: _UdpProtocol(queue),
            local_addr=(self._cfg.udp_host, self._cfg.udp_port),
        )
        try:
            while True:
                chunk = await queue.get()
                for pkt in self._decode(chunk):
                    yield pkt
        finally:
            transport.close()

    # --- Serial source (real hardware) --------------------------------------
    async def _serial_packets(self) -> AsyncIterator[Dict]:
        import serial  # pyserial; imported lazily so udp-only dev needs no port

        loop = asyncio.get_running_loop()
        # Read-only: open the port for input only. We never write.
        ser = serial.Serial(self._cfg.serial_port, self._cfg.serial_baud, timeout=0.05)
        try:
            while True:
                # Blocking read runs in the default executor to avoid stalling
                # the event loop. We ask for whatever is buffered (>=1 byte).
                chunk = await loop.run_in_executor(None, self._read_available, ser)
                if chunk:
                    for pkt in self._decode(chunk):
                        yield pkt
                else:
                    await asyncio.sleep(0)  # yield control
        finally:
            ser.close()

    @staticmethod
    def _read_available(ser) -> bytes:
        n = ser.in_waiting or 1
        return ser.read(n)
