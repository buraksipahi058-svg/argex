"""
SQLite persistence for telemetry frames + events (aiosqlite).

The store is a passive sink: it records what the vehicle reported. It never
feeds anything back to the vehicle. DB writes for high-rate frames are decimated
(persist_frame_rate_hz) so the file does not grow unbounded at 20 Hz; the live
WebSocket feed is unaffected and stays full-rate.
"""
from __future__ import annotations

import time
from pathlib import Path
from typing import Any, Dict, List, Optional

import aiosqlite

from .config import StorageConfig

_SCHEMA = """
CREATE TABLE IF NOT EXISTS telemetry_frames (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    ts_ms INTEGER NOT NULL,
    seq INTEGER,
    left_motor INTEGER, right_motor INTEGER,
    pan INTEGER, tilt INTEGER,
    laser INTEGER, mode INTEGER,
    elrs_link INTEGER, jetson_link INTEGER,
    cmd_timeout INTEGER, auto_active INTEGER,
    failsafe INTEGER, crc_err INTEGER,
    packets_lost INTEGER, status_rate_hz REAL,
    stm_uptime_ms INTEGER, stm_status_fresh INTEGER
);
CREATE INDEX IF NOT EXISTS idx_frames_ts ON telemetry_frames(ts_ms);

CREATE TABLE IF NOT EXISTS events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    ts_ms INTEGER NOT NULL,
    type INTEGER, type_name TEXT,
    detail TEXT, raw_durum INTEGER
);
CREATE INDEX IF NOT EXISTS idx_events_ts ON events(ts_ms);
"""


class Store:
    def __init__(self, cfg: StorageConfig) -> None:
        self._cfg = cfg
        self._db: Optional[aiosqlite.Connection] = None
        self._persist_dt_ms = (1000.0 / cfg.persist_frame_rate_hz) if cfg.persist_frame_rate_hz > 0 else 0.0
        self._last_persist_ms = 0.0

    async def open(self) -> None:
        Path(self._cfg.db_path).parent.mkdir(parents=True, exist_ok=True)
        self._db = await aiosqlite.connect(self._cfg.db_path)
        self._db.row_factory = aiosqlite.Row
        await self._db.executescript(_SCHEMA)
        await self._db.commit()

    async def close(self) -> None:
        if self._db:
            await self._db.commit()
            await self._db.close()
            self._db = None

    async def maybe_persist_frame(self, f: Dict[str, Any]) -> None:
        """Insert a frame subject to decimation (persist_frame_rate_hz)."""
        now = f["ts_ms"]
        if self._persist_dt_ms and (now - self._last_persist_ms) < self._persist_dt_ms:
            return
        self._last_persist_ms = now
        assert self._db is not None
        await self._db.execute(
            """INSERT INTO telemetry_frames
               (ts_ms, seq, left_motor, right_motor, pan, tilt, laser, mode,
                elrs_link, jetson_link, cmd_timeout, auto_active, failsafe, crc_err,
                packets_lost, status_rate_hz, stm_uptime_ms, stm_status_fresh)
               VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)""",
            (
                f["ts_ms"], f["seq"],
                f["left_motor"], f["right_motor"], f["pan"], f["tilt"],
                int(f["laser"]), f["mode"],
                int(f["elrs_link"]), int(f["jetson_link"]),
                int(f["cmd_timeout"]), int(f["auto_active"]),
                int(f["failsafe"]), int(f["crc_err"]),
                f["packets_lost"], f["status_rate_hz"],
                f["stm_uptime_ms"], int(f["stm_status_fresh"]),
            ),
        )
        await self._db.commit()

    async def insert_event(self, e: Dict[str, Any]) -> None:
        assert self._db is not None
        await self._db.execute(
            "INSERT INTO events (ts_ms, type, type_name, detail, raw_durum) VALUES (?,?,?,?,?)",
            (e["ts_ms"], e["type"], e["type_name"], e.get("detail", ""), e.get("raw_durum", 0)),
        )
        await self._db.commit()

    async def recent_frames(self, limit: int = 200) -> List[Dict[str, Any]]:
        assert self._db is not None
        cur = await self._db.execute(
            "SELECT * FROM telemetry_frames ORDER BY ts_ms DESC LIMIT ?", (limit,)
        )
        rows = await cur.fetchall()
        return [dict(r) for r in reversed(rows)]

    async def recent_events(self, limit: int = 100) -> List[Dict[str, Any]]:
        assert self._db is not None
        cur = await self._db.execute(
            "SELECT * FROM events ORDER BY ts_ms DESC LIMIT ?", (limit,)
        )
        rows = await cur.fetchall()
        return [dict(r) for r in reversed(rows)]

    async def prune(self) -> None:
        if self._cfg.retention_seconds <= 0 or self._db is None:
            return
        cutoff = int(time.time() * 1000) - self._cfg.retention_seconds * 1000
        await self._db.execute("DELETE FROM telemetry_frames WHERE ts_ms < ?", (cutoff,))
        await self._db.execute("DELETE FROM events WHERE ts_ms < ?", (cutoff,))
        await self._db.commit()
