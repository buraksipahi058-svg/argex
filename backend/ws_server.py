"""
Frontend-facing server: live telemetry over WebSocket (JSON) + a small
read-only REST surface on the same port. The browser never needs protobuf.

  WebSocket:  ws://<host>:<port>/ws     -> snapshot, then telemetry/event/conn msgs
  REST (GET):
    /health                    -> {"status":"ok", ...}
    /api/state                 -> latest snapshot (same shape as WS snapshot)
    /api/history/frames?limit= -> recent persisted frames (verification/replay)
    /api/history/events?limit= -> recent persisted events

Everything here is read-only; there is no control endpoint of any kind.
"""
from __future__ import annotations

import asyncio
import json
from typing import Optional
from urllib.parse import parse_qs, urlsplit

from websockets.asyncio.server import serve
from websockets.datastructures import Headers
from websockets.exceptions import ConnectionClosed
from websockets.http11 import Response

from .config import WsConfig
from .ingest import StateHub
from .store import Store


def _json_response(status: int, obj) -> Response:
    body = json.dumps(obj).encode("utf-8")
    headers = Headers()
    headers["Content-Type"] = "application/json"
    headers["Content-Length"] = str(len(body))
    headers["Access-Control-Allow-Origin"] = "*"
    return Response(status, "OK", headers, body)


class WsServer:
    def __init__(self, cfg: WsConfig, hub: StateHub, store: Store) -> None:
        self._cfg = cfg
        self._hub = hub
        self._store = store
        self._server = None

    async def start(self) -> None:
        self._server = await serve(
            self._handle_ws,
            self._cfg.host,
            self._cfg.port,
            process_request=self._process_request,
        )

    async def stop(self) -> None:
        if self._server is not None:
            self._server.close()
            await self._server.wait_closed()

    # ---- REST (short-circuits the WS handshake for non-/ws paths) ----------
    async def _process_request(self, connection, request) -> Optional[Response]:
        path = urlsplit(request.path)
        route = path.path
        if route == "/ws":
            return None  # continue with the WebSocket handshake

        query = parse_qs(path.query)
        limit = int(query.get("limit", ["200"])[0])

        if route == "/health":
            return _json_response(200, {"status": "ok"})
        if route == "/api/state":
            return _json_response(200, self._hub.snapshot())
        if route == "/api/history/frames":
            return _json_response(200, await self._store.recent_frames(min(limit, 5000)))
        if route == "/api/history/events":
            return _json_response(200, await self._store.recent_events(min(limit, 5000)))
        return _json_response(404, {"error": "not found", "path": route})

    # ---- WebSocket -------------------------------------------------------
    async def _handle_ws(self, websocket) -> None:
        q = self._hub.add_client()
        try:
            while True:
                msg = await q.get()
                await websocket.send(msg)
        except (ConnectionClosed, asyncio.CancelledError):
            pass
        finally:
            self._hub.remove_client(q)
