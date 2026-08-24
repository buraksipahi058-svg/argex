"""
Base Station backend — main entry point.

    python -m backend.main [path/to/config.yaml]

Wires together (all read-only w.r.t. the vehicle):
    QUIC server (receive telemetry) -> ingest queue -> StateHub
        -> SQLite (persist)  +  WebSocket/REST (serve frontend)

The backend is NOT part of the vehicle safety loop: if it stops, the vehicle
continues under the existing STM32/Jetson logic.
"""
from __future__ import annotations

import asyncio
import logging
import sys

from .config import load_config
from .ingest import StateHub
from .quic_server import start_quic_server
from .store import Store
from .ws_server import WsServer

log = logging.getLogger("backend.main")


async def _ingest_consumer(queue: "asyncio.Queue[bytes]", hub: StateHub) -> None:
    while True:
        data = await queue.get()
        await hub.on_envelope(data)


async def _base_link_watchdog(hub: StateHub) -> None:
    while True:
        await asyncio.sleep(0.5)
        await hub.check_base_link()


async def _retention_task(store: Store) -> None:
    while True:
        await asyncio.sleep(60)
        await store.prune()


async def run(config_path: str | None = None) -> None:
    cfg = load_config(config_path)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(levelname)s %(message)s")

    store = Store(cfg.storage)
    await store.open()

    hub = StateHub(store, cfg.links.base_link_timeout_ms)

    # QUIC callback runs on the event loop thread; hand bytes to an async queue
    # so decoding/persistence happens outside the transport callback.
    ingest_queue: "asyncio.Queue[bytes]" = asyncio.Queue(maxsize=10000)

    def on_envelope(data: bytes) -> None:
        try:
            ingest_queue.put_nowait(data)
        except asyncio.QueueFull:
            pass  # backpressure: drop (telemetry is latest-wins)

    quic_server = await start_quic_server(cfg.quic, on_envelope)
    log.info("QUIC telemetry server listening on %s:%d", cfg.quic.host, cfg.quic.port)

    ws = WsServer(cfg.ws, hub, store)
    await ws.start()
    log.info("WebSocket/REST server listening on %s:%d (ws path /ws)", cfg.ws.host, cfg.ws.port)

    try:
        await asyncio.gather(
            _ingest_consumer(ingest_queue, hub),
            _base_link_watchdog(hub),
            _retention_task(store),
        )
    finally:
        quic_server.close()
        await ws.stop()
        await store.close()


def main() -> None:
    config_path = sys.argv[1] if len(sys.argv) > 1 else None
    try:
        asyncio.run(run(config_path))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
