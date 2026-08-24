"""
Ingest + live state hub for the Base Station.

Responsibilities:
  * decode TelemetryEnvelope protobuf from the QUIC plane,
  * keep the latest in-memory state for new WebSocket clients,
  * persist frames (decimated) and events to SQLite,
  * broadcast JSON to connected frontends,
  * derive the BASE-LINK state (gateway<->base) — which is DISTINCT from the
    vehicle-reported ELRS/Jetson links and from "STM telemetry stale". When the
    base link drops, we mark values not-fresh rather than fabricating vehicle
    state transitions.

Read-only: this module only consumes. Nothing here can actuate the vehicle.
"""
from __future__ import annotations

import asyncio
import json
import time
from collections import deque
from typing import Any, Deque, Dict, Optional, Set

from gen import telemetry_pb2 as pb
from .store import Store

EVENTS_KEPT = 200


def now_ms() -> int:
    return int(time.time() * 1000)


def _mode_name(v: int) -> str:
    return {pb.ACTIVE_MODE_DRIVE: "DRIVE", pb.ACTIVE_MODE_LASER: "LASER"}.get(v, "UNKNOWN")


def _event_name(v: int) -> str:
    return pb.EventType.Name(v).removeprefix("EVENT_")


def frame_to_dict(fr: pb.TelemetryFrame) -> Dict[str, Any]:
    c, s, l = fr.control, fr.status, fr.link
    return {
        "ts": fr.timestamp_unix_ms,
        "seq": fr.seq,
        "control": {
            "left_motor": c.left_motor,
            "right_motor": c.right_motor,
            "pan_deg": c.pan_deg,
            "tilt_deg": c.tilt_deg,
            "laser_on": c.laser_on,
            "mode": _mode_name(c.mode),
        },
        "status": {
            "elrs_link_up": s.elrs_link_up,
            "jetson_link_up": s.jetson_link_up,
            "cmd_timeout": s.cmd_timeout,
            "autonomous_active": s.autonomous_active,
            "failsafe_active": s.failsafe_active,
            "crc_error_recent": s.crc_error_recent,
            "raw_durum": s.raw_durum,
        },
        "link": {
            "packets_lost_total": l.packets_lost_total,
            "crc_errors_total": l.crc_errors_total,
            "status_rate_hz": round(l.status_rate_hz, 2),
            "heartbeat_rate_hz": round(l.heartbeat_rate_hz, 2),
            "stm_uptime_ms": l.stm_uptime_ms,
            "stm_status_fresh": l.stm_status_fresh,
            "last_status_unix_ms": l.last_status_unix_ms,
            "last_heartbeat_unix_ms": l.last_heartbeat_unix_ms,
        },
    }


def _frame_db_row(d: Dict[str, Any]) -> Dict[str, Any]:
    c, s, l = d["control"], d["status"], d["link"]
    return {
        "ts_ms": d["ts"], "seq": d["seq"],
        "left_motor": c["left_motor"], "right_motor": c["right_motor"],
        "pan": c["pan_deg"], "tilt": c["tilt_deg"],
        "laser": c["laser_on"], "mode": {"DRIVE": 1, "LASER": 2}.get(c["mode"], 0),
        "elrs_link": s["elrs_link_up"], "jetson_link": s["jetson_link_up"],
        "cmd_timeout": s["cmd_timeout"], "auto_active": s["autonomous_active"],
        "failsafe": s["failsafe_active"], "crc_err": s["crc_error_recent"],
        "packets_lost": l["packets_lost_total"], "status_rate_hz": l["status_rate_hz"],
        "stm_uptime_ms": l["stm_uptime_ms"], "stm_status_fresh": l["stm_status_fresh"],
    }


class StateHub:
    def __init__(self, store: Store, base_link_timeout_ms: int) -> None:
        self._store = store
        self._timeout_ms = base_link_timeout_ms

        self._clients: Set[asyncio.Queue] = set()
        self._latest: Optional[Dict[str, Any]] = None
        self._events: Deque[Dict[str, Any]] = deque(maxlen=EVENTS_KEPT)
        self._conn = {"base_link_up": False, "stm_status_fresh": False}
        self._base_link_established = False
        self._last_gateway_ms = 0

    # ---- WebSocket client registry ----------------------------------------
    def add_client(self) -> asyncio.Queue:
        q: asyncio.Queue = asyncio.Queue(maxsize=64)
        self._clients.add(q)
        q.put_nowait(json.dumps(self.snapshot()))
        return q

    def remove_client(self, q: asyncio.Queue) -> None:
        self._clients.discard(q)

    def snapshot(self) -> Dict[str, Any]:
        return {
            "t": "snapshot",
            "telemetry": self._latest,
            "conn": dict(self._conn),
            "events": list(self._events),
        }

    def _broadcast(self, obj: Dict[str, Any]) -> None:
        text = json.dumps(obj)
        for q in list(self._clients):
            try:
                q.put_nowait(text)
            except asyncio.QueueFull:
                # Slow client: drop this message (telemetry is latest-wins).
                pass

    # ---- ingestion --------------------------------------------------------
    async def on_envelope(self, data: bytes) -> None:
        env = pb.TelemetryEnvelope()
        try:
            env.ParseFromString(data)
        except Exception:
            return  # ignore malformed frame
        self._last_gateway_ms = now_ms()
        await self._ensure_base_link_up()

        kind = env.WhichOneof("payload")
        if kind == "frame":
            await self._on_frame(env.frame)
        elif kind == "event":
            await self._on_event(env.event)
        elif kind == "heartbeat":
            await self._on_heartbeat(env.heartbeat)

    async def _on_frame(self, fr: pb.TelemetryFrame) -> None:
        d = frame_to_dict(fr)
        self._latest = d
        fresh = bool(d["link"]["stm_status_fresh"])
        if fresh != self._conn["stm_status_fresh"]:
            self._conn["stm_status_fresh"] = fresh
            self._broadcast({"t": "conn", "conn": dict(self._conn)})
        await self._store.maybe_persist_frame(_frame_db_row(d))
        self._broadcast({"t": "telemetry", **d, "conn": dict(self._conn)})

    async def _on_event(self, ev: pb.Event) -> None:
        e = {
            "ts": ev.timestamp_unix_ms,
            "type": _event_name(ev.type),
            "detail": ev.detail,
            "raw_durum": ev.raw_durum,
        }
        self._events.append(e)
        await self._store.insert_event(
            {"ts_ms": ev.timestamp_unix_ms, "type": ev.type, "type_name": e["type"],
             "detail": ev.detail, "raw_durum": ev.raw_durum}
        )
        self._broadcast({"t": "event", **e})

    async def _on_heartbeat(self, hb: pb.GatewayHeartbeat) -> None:
        stm_fresh = bool(hb.stm_alive)
        if stm_fresh != self._conn["stm_status_fresh"]:
            self._conn["stm_status_fresh"] = stm_fresh
            self._broadcast({"t": "conn", "conn": dict(self._conn)})

    # ---- base-link derivation (backend-only) ------------------------------
    async def _ensure_base_link_up(self) -> None:
        if self._conn["base_link_up"]:
            return
        self._conn["base_link_up"] = True
        self._broadcast({"t": "conn", "conn": dict(self._conn)})
        if self._base_link_established:
            await self._emit_backend_event(pb.EVENT_BASE_LINK_RESTORED, "gateway link restored")
        self._base_link_established = True

    async def check_base_link(self) -> None:
        """Periodic watchdog: mark base link down if the gateway went quiet."""
        if not self._conn["base_link_up"]:
            return
        if now_ms() - self._last_gateway_ms > self._timeout_ms:
            self._conn["base_link_up"] = False
            self._conn["stm_status_fresh"] = False  # cannot be fresh if link is down
            self._broadcast({"t": "conn", "conn": dict(self._conn)})
            await self._emit_backend_event(pb.EVENT_BASE_LINK_LOST, "no gateway traffic")

    async def _emit_backend_event(self, etype: int, detail: str) -> None:
        ts = now_ms()
        e = {"ts": ts, "type": _event_name(etype), "detail": detail, "raw_durum": 0}
        self._events.append(e)
        await self._store.insert_event(
            {"ts_ms": ts, "type": etype, "type_name": e["type"], "detail": detail, "raw_durum": 0}
        )
        self._broadcast({"t": "event", **e})
