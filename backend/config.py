"""Typed configuration loader for the Base Station backend (reads config.yaml)."""
from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

import yaml

DEFAULT_CONFIG = Path(__file__).resolve().parent / "config.yaml"


@dataclass
class QuicConfig:
    host: str = "0.0.0.0"
    port: int = 4433
    cert_file: str = "backend/dev-cert.pem"
    key_file: str = "backend/dev-key.pem"


@dataclass
class WsConfig:
    host: str = "0.0.0.0"
    port: int = 8080


@dataclass
class StorageConfig:
    db_path: str = "backend/telemetry.db"
    persist_frame_rate_hz: float = 5.0
    retention_seconds: int = 3600


@dataclass
class LinksConfig:
    base_link_timeout_ms: int = 2500


@dataclass
class BackendConfig:
    quic: QuicConfig = field(default_factory=QuicConfig)
    ws: WsConfig = field(default_factory=WsConfig)
    storage: StorageConfig = field(default_factory=StorageConfig)
    links: LinksConfig = field(default_factory=LinksConfig)


def load_config(path: Path | str | None = None) -> BackendConfig:
    path = Path(path) if path else DEFAULT_CONFIG
    raw = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
    return BackendConfig(
        quic=QuicConfig(**(raw.get("quic", {}) or {})),
        ws=WsConfig(**(raw.get("ws", {}) or {})),
        storage=StorageConfig(**(raw.get("storage", {}) or {})),
        links=LinksConfig(**(raw.get("links", {}) or {})),
    )
