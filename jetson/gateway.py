"""
Jetson telemetry gateway (vehicle-side observer) — main entry point.

    python -m jetson.gateway [path/to/config.yaml]

Pipeline:
    STM STATUS/HEARTBEAT (frozen protocol)
        -> StmReader (frozen parser)
        -> TelemetryMapper (structured proto + derived link health + events)
        -> QuicTelemetryClient (datagrams for frames, reliable stream for events)
        -> Base Station backend

Read-only: consumes STATUS/HEARTBEAT, emits telemetry only, never COMMAND. It
also does not manage the existing Jetson->STM heartbeat/autonomy traffic; that
remains the responsibility of the (separate) vehicle autonomy process.

Video is handled out-of-band by video_pipelines.py (RTP/UDP), never here.
"""
from __future__ import annotations

import asyncio
import logging
import sys
from pathlib import Path

from .config import load_config, GatewayConfig
from .mapper import TelemetryMapper, now_ms
from .quic_client import QuicTelemetryClient
from .stm_reader import StmReader, TYPE_STATUS, TYPE_HEARTBEAT

log = logging.getLogger("jetson.gateway")


async def _reader_task(reader: StmReader, mapper: TelemetryMapper, client: QuicTelemetryClient) -> None:
    """Consume decoded STM packets; update the mapper; emit events immediately."""
    async for pkt in reader.packets():
        now = now_ms()
        if pkt["type"] == TYPE_STATUS and "status" in pkt:
            events = mapper.on_status(pkt["status"], pkt["seq"], now, reader.packets_lost)
            for ev in events:
                client.send_event(ev)
        elif pkt["type"] == TYPE_HEARTBEAT and "heartbeat" in pkt:
            mapper.on_heartbeat(pkt["heartbeat"], now)
        # COMMAND or unknown types are ignored: the Base Station never consumes
        # or produces control traffic.


async def _publisher_task(cfg: GatewayConfig, mapper: TelemetryMapper, client: QuicTelemetryClient) -> None:
    """Rate-limited publisher: high-frequency frames, heartbeat, staleness ticks."""
    tcfg = cfg.telemetry
    frame_dt = 1.0 / tcfg.frame_rate_hz if tcfg.frame_rate_hz > 0 else 0.05
    hb_dt = 1.0 / tcfg.gateway_heartbeat_hz if tcfg.gateway_heartbeat_hz > 0 else 1.0

    next_frame = 0.0
    next_hb = 0.0
    tick_dt = 0.02  # 50 Hz control loop for timers/staleness

    while True:
        loop_t = asyncio.get_event_loop().time()
        now = now_ms()

        # Event: time-based STM telemetry staleness (distinct from ELRS/failsafe).
        for ev in mapper.tick(now):
            client.send_event(ev)

        if mapper.has_status and loop_t >= next_frame:
            next_frame = loop_t + frame_dt
            client.send_frame(mapper.snapshot_frame(now))

        if loop_t >= next_hb:
            next_hb = loop_t + hb_dt
            client.send_gateway_heartbeat(mapper.gateway_heartbeat(now))

        await asyncio.sleep(tick_dt)


async def run(config_path: str | None = None) -> None:
    cfg = load_config(config_path)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(levelname)s %(message)s")

    reader = StmReader(cfg.source)
    mapper = TelemetryMapper(cfg.telemetry)
    client = QuicTelemetryClient(cfg.quic)

    log.info("connecting to Base Station QUIC %s:%d", cfg.quic.host, cfg.quic.port)
    await client.start()
    log.info("connected; observing STM via %s source", cfg.source.type)

    try:
        await asyncio.gather(
            _reader_task(reader, mapper, client),
            _publisher_task(cfg, mapper, client),
        )
    finally:
        await client.stop()


def main() -> None:
    config_path = sys.argv[1] if len(sys.argv) > 1 else None
    try:
        asyncio.run(run(config_path))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
