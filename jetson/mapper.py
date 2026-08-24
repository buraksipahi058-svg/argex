"""
Map decoded STM STATUS/HEARTBEAT packets -> Base Station protobuf telemetry.

This is where the "structured vehicle data" is produced. It:
  * copies real STATUS/HEARTBEAT fields 1:1 into the schema (no invented fields),
  * derives link health deterministically (rates, SEQ loss, freshness),
  * detects events by diffing consecutive STATUS packets, keeping the distinct
    link/failure concepts strictly separate:
        - STATUS.elrsLink            -> ELRS (RC) link events
        - STATUS.durum & JETSON_LINK -> STM's view of the STM<->Jetson link
        - STATUS stopped arriving    -> "STM telemetry stale" (transport), NOT
                                        an ELRS or failsafe transition.
  * never fabricates a failsafe/link transition when telemetry merely goes stale;
    last-known values are retained and flagged not-fresh instead.
"""
from __future__ import annotations

import time
from typing import Dict, List, Optional

from gen import telemetry_pb2 as pb
from .config import TelemetryConfig


def now_ms() -> int:
    return int(time.time() * 1000)


_ACTIVE_MODE = {0: pb.ACTIVE_MODE_DRIVE, 1: pb.ACTIVE_MODE_LASER}
_MODE_NAME = {0: "DRIVE", 1: "LASER"}


class _Ewma:
    """Exponentially-weighted mean of inter-arrival intervals -> rate in Hz."""

    def __init__(self, alpha: float = 0.2) -> None:
        self._alpha = alpha
        self._interval_ms: Optional[float] = None
        self._last_ms: Optional[int] = None

    def tick(self, t_ms: int) -> None:
        if self._last_ms is not None:
            dt = t_ms - self._last_ms
            if dt > 0:
                self._interval_ms = (
                    dt if self._interval_ms is None
                    else (1 - self._alpha) * self._interval_ms + self._alpha * dt
                )
        self._last_ms = t_ms

    def rate_hz(self) -> float:
        if not self._interval_ms:
            return 0.0
        return 1000.0 / self._interval_ms


class TelemetryMapper:
    def __init__(self, cfg: TelemetryConfig) -> None:
        self._cfg = cfg

        self._prev_status: Optional[Dict] = None
        self._latest_control = pb.VehicleControlState()
        self._latest_status = pb.SystemStatus()
        self._have_status = False

        self._status_rate = _Ewma()
        self._hb_rate = _Ewma()

        self._last_status_ms = 0
        self._last_heartbeat_ms = 0
        self._last_seq = 0
        self._stm_uptime_ms = 0
        self._packets_lost = 0
        self._crc_errors = 0  # see note in LinkHealth: not surfaced by frozen parser

        self._status_fresh = False  # staleness edge tracking
        self._debounce: Dict[int, int] = {}

    # ---- ingestion ---------------------------------------------------------
    def on_status(self, s: Dict, seq: int, now: int, packets_lost: int) -> List[pb.Event]:
        self._status_rate.tick(now)
        self._last_status_ms = now
        self._last_seq = seq
        self._packets_lost = packets_lost

        self._latest_control = pb.VehicleControlState(
            left_motor=int(s["sol_motor"]),
            right_motor=int(s["sag_motor"]),
            pan_deg=int(s["pan"]),
            tilt_deg=int(s["tilt"]),
            laser_on=bool(s["lazer"]),
            mode=_ACTIVE_MODE.get(int(s["aktif_mod"]), pb.ACTIVE_MODE_UNSPECIFIED),
        )
        self._latest_status = pb.SystemStatus(
            elrs_link_up=bool(s["elrs_link"]),
            jetson_link_up=bool(s["jetson_link"]),
            cmd_timeout=bool(s["cmd_timeout"]),
            autonomous_active=bool(s["auto_enabled"]),
            failsafe_active=bool(s["failsafe"]),
            crc_error_recent=bool(s["crc_err"]),
            raw_durum=int(s["durum"]),
        )
        self._have_status = True

        events = self._detect_events(s, now)

        # Recovering from stale is handled here too: a fresh STATUS means the
        # transport recovered.
        if not self._status_fresh:
            self._status_fresh = True
            if self._prev_status is not None:  # not the very first packet
                events.append(self._event(pb.EVENT_STM_TELEMETRY_RESTORED, now, "STATUS resumed"))

        self._prev_status = s
        return events

    def on_heartbeat(self, hb: Dict, now: int) -> None:
        # Only the STM-sourced heartbeat carries the vehicle uptime we display.
        self._hb_rate.tick(now)
        self._last_heartbeat_ms = now
        from .stm_reader import HB_KAYNAK_STM

        if int(hb["kaynak"]) == HB_KAYNAK_STM:
            self._stm_uptime_ms = int(hb["uptime_ms"])

    # ---- periodic tick (time-based staleness, no fabricated vehicle state) --
    def tick(self, now: int) -> List[pb.Event]:
        events: List[pb.Event] = []
        if self._have_status:
            fresh = (now - self._last_status_ms) <= self._cfg.status_stale_ms
            if self._status_fresh and not fresh:
                self._status_fresh = False
                events.append(
                    self._event(
                        pb.EVENT_STM_TELEMETRY_STALE,
                        now,
                        f"no STATUS for {now - self._last_status_ms} ms",
                    )
                )
        return events

    # ---- snapshots ---------------------------------------------------------
    def snapshot_frame(self, now: int, seq: int | None = None) -> pb.TelemetryFrame:
        return pb.TelemetryFrame(
            timestamp_unix_ms=now,
            seq=self._last_seq if seq is None else seq,
            control=self._latest_control,
            status=self._latest_status,
            link=self._link_health(now),
        )

    def gateway_heartbeat(self, now: int) -> pb.GatewayHeartbeat:
        stm_alive = self._have_status and (now - self._last_status_ms) <= self._cfg.status_stale_ms
        return pb.GatewayHeartbeat(
            timestamp_unix_ms=now,
            stm_uptime_ms=self._stm_uptime_ms,
            stm_alive=stm_alive,
        )

    @property
    def has_status(self) -> bool:
        return self._have_status

    # ---- internals ---------------------------------------------------------
    def _link_health(self, now: int) -> pb.LinkHealth:
        return pb.LinkHealth(
            packets_lost_total=self._packets_lost,
            # NOTE: the frozen reference parser (needtocheck/jetson_parser.py)
            # silently drops CRC-failed frames and does not expose a count, so we
            # cannot derive a Jetson-side CRC-error total without modifying it
            # (forbidden). The authoritative CRC signal we DO surface is the STM's
            # own STATUS.durum & CRC_ERR bit (SystemStatus.crc_error_recent).
            crc_errors_total=self._crc_errors,
            status_rate_hz=self._status_rate.rate_hz(),
            heartbeat_rate_hz=self._hb_rate.rate_hz(),
            stm_uptime_ms=self._stm_uptime_ms,
            stm_status_fresh=self._have_status and (now - self._last_status_ms) <= self._cfg.status_stale_ms,
            last_status_unix_ms=self._last_status_ms,
            last_heartbeat_unix_ms=self._last_heartbeat_ms,
        )

    def _detect_events(self, s: Dict, now: int) -> List[pb.Event]:
        prev = self._prev_status
        events: List[pb.Event] = []
        if prev is None:
            return events

        # Mode
        if s["aktif_mod"] != prev["aktif_mod"]:
            events.append(self._event(
                pb.EVENT_MODE_CHANGED, now,
                f"{_MODE_NAME.get(prev['aktif_mod'], '?')} -> {_MODE_NAME.get(s['aktif_mod'], '?')}",
                raw_durum=s["durum"],
            ))

        # Autonomy (durum.AUTO_EN)
        self._edge(events, now, prev["auto_enabled"], s["auto_enabled"],
                   pb.EVENT_AUTONOMOUS_ENGAGED, pb.EVENT_AUTONOMOUS_DISENGAGED, s["durum"])
        # Failsafe (durum.FAILSAFE)
        self._edge(events, now, prev["failsafe"], s["failsafe"],
                   pb.EVENT_FAILSAFE_ACTIVATED, pb.EVENT_FAILSAFE_CLEARED, s["durum"])
        # Laser (STATUS.lazer)
        self._edge(events, now, prev["lazer"], s["lazer"],
                   pb.EVENT_LASER_ON, pb.EVENT_LASER_OFF, s["durum"])
        # ELRS link -- from the STATUS.elrsLink FIELD only
        self._edge(events, now, prev["elrs_link"], s["elrs_link"],
                   pb.EVENT_ELRS_LINK_RESTORED, pb.EVENT_ELRS_LINK_LOST, s["durum"])
        # STM's view of STM<->Jetson link (durum.JETSON_LINK)
        self._edge(events, now, prev["jetson_link"], s["jetson_link"],
                   pb.EVENT_STM_JETSON_LINK_RESTORED, pb.EVENT_STM_JETSON_LINK_LOST, s["durum"])
        # Command timeout (durum.CMD_TIMEOUT)
        self._edge(events, now, prev["cmd_timeout"], s["cmd_timeout"],
                   pb.EVENT_CMD_TIMEOUT_SET, pb.EVENT_CMD_TIMEOUT_CLEARED, s["durum"])

        # CRC error (durum.CRC_ERR) -- rising edge only, debounced
        if s["crc_err"] and not prev["crc_err"]:
            self._emit_debounced(events, now, pb.EVENT_CRC_ERROR, "STM reported recent CRC error", s["durum"])

        return events

    def _edge(self, events, now, prev_val, cur_val, on_rise, on_fall, durum) -> None:
        if bool(cur_val) == bool(prev_val):
            return
        etype = on_rise if cur_val else on_fall
        self._emit_debounced(events, now, etype, "", durum)

    def _emit_debounced(self, events, now, etype, detail, durum) -> None:
        last = self._debounce.get(etype, -10**12)
        if now - last < self._cfg.event_debounce_ms:
            return
        self._debounce[etype] = now
        events.append(self._event(etype, now, detail, raw_durum=durum))

    @staticmethod
    def _event(etype, now, detail="", raw_durum=0) -> pb.Event:
        return pb.Event(timestamp_unix_ms=now, type=etype, detail=detail, raw_durum=int(raw_durum))
