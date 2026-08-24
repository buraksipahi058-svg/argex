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
from pathlib import Path
from typing import AsyncIterator, Dict

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
                for pkt in self._proto.feed(chunk):
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
                    for pkt in self._proto.feed(chunk):
                        yield pkt
                else:
                    await asyncio.sleep(0)  # yield control
        finally:
            ser.close()

    @staticmethod
    def _read_available(ser) -> bytes:
        n = ser.in_waiting or 1
        return ser.read(n)
