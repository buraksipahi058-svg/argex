#!/usr/bin/env python3
"""
Quick verification client: connect to the Base Station WebSocket, print the
first N messages, then exit. Used to confirm the live telemetry feed.

    python scripts/ws_probe.py [ws://127.0.0.1:8080/ws] [count]
"""
import asyncio
import sys

from websockets.asyncio.client import connect


async def main() -> None:
    url = sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:8080/ws"
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 15
    async with connect(url) as ws:
        for _ in range(count):
            msg = await asyncio.wait_for(ws.recv(), timeout=5.0)
            print(msg[:300])


if __name__ == "__main__":
    asyncio.run(main())
