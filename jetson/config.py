"""Typed configuration loader for the Jetson gateway (reads config.yaml)."""
from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import List

import yaml

DEFAULT_CONFIG = Path(__file__).resolve().parent / "config.yaml"


@dataclass
class SourceConfig:
    type: str = "udp"                 # "serial" | "udp"
    serial_port: str = "/dev/ttyTHS1"
    serial_baud: int = 115200
    udp_host: str = "127.0.0.1"
    udp_port: int = 9000


@dataclass
class QuicConfig:
    host: str = "127.0.0.1"
    port: int = 4433
    use_datagrams_for_frames: bool = True
    verify: bool = False


@dataclass
class TelemetryConfig:
    frame_rate_hz: float = 20.0
    gateway_heartbeat_hz: float = 1.0
    status_stale_ms: int = 300
    heartbeat_stale_ms: int = 600
    event_debounce_ms: int = 150


@dataclass
class CameraConfig:
    name: str
    device: str
    input_format: str = "yuyv422"     # v4l2 giris formati: "yuyv422" | "mjpeg"
    width: int = 640
    height: int = 480
    fps: int = 30
    bitrate_kbps: int = 1000
    mode: str = "drive"               # bu kamera hangi arac modunda yayinlanir: "drive" | "laser"
    rtp_port: int = 5000


@dataclass
class VideoConfig:
    enabled: bool = False
    base_host: str = "127.0.0.1"
    encoder: str = "h264"
    cameras: List[CameraConfig] = field(default_factory=list)


@dataclass
class GatewayConfig:
    source: SourceConfig = field(default_factory=SourceConfig)
    quic: QuicConfig = field(default_factory=QuicConfig)
    telemetry: TelemetryConfig = field(default_factory=TelemetryConfig)
    video: VideoConfig = field(default_factory=VideoConfig)


def load_config(path: Path | str | None = None) -> GatewayConfig:
    path = Path(path) if path else DEFAULT_CONFIG
    raw = yaml.safe_load(path.read_text(encoding="utf-8")) or {}

    cams = [CameraConfig(**c) for c in (raw.get("video", {}) or {}).get("cameras", [])]
    video_raw = dict(raw.get("video", {}) or {})
    video_raw.pop("cameras", None)

    return GatewayConfig(
        source=SourceConfig(**(raw.get("source", {}) or {})),
        quic=QuicConfig(**(raw.get("quic", {}) or {})),
        telemetry=TelemetryConfig(**(raw.get("telemetry", {}) or {})),
        video=VideoConfig(cameras=cams, **video_raw),
    )
