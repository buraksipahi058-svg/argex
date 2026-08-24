"""
QUIC server for the Base Station telemetry plane. RECEIVE-ONLY at the
application layer: it decodes datagrams (high-rate frames) and the reliable
stream (events + gateway heartbeats), and hands raw envelope bytes to a
callback. It never sends an application message back to the gateway, and the
schema has no command message — so the Base Station cannot actuate the vehicle.
"""
from __future__ import annotations

from functools import partial
from pathlib import Path
from typing import Callable

from aioquic.asyncio import serve
from aioquic.asyncio.protocol import QuicConnectionProtocol
from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.events import DatagramFrameReceived, QuicEvent, StreamDataReceived

from common.proto_io import DelimitedStreamDecoder
from common.quic_common import ALPN, MAX_DATAGRAM_FRAME_SIZE, ensure_dev_certificate
from .config import QuicConfig

EnvelopeCallback = Callable[[bytes], None]


class _ServerProtocol(QuicConnectionProtocol):
    def __init__(self, *args, on_envelope: EnvelopeCallback, **kwargs) -> None:
        super().__init__(*args, **kwargs)
        self._on_envelope = on_envelope
        self._decoder = DelimitedStreamDecoder()

    def quic_event_received(self, event: QuicEvent) -> None:
        if isinstance(event, DatagramFrameReceived):
            # One datagram == one TelemetryEnvelope (high-frequency frame).
            self._on_envelope(event.data)
        elif isinstance(event, StreamDataReceived):
            # Reliable stream: length-delimited envelopes (events + heartbeats).
            for msg in self._decoder.feed(event.stream_id, event.data):
                self._on_envelope(msg)
        # No response is ever generated: read-only.


async def start_quic_server(cfg: QuicConfig, on_envelope: EnvelopeCallback):
    ensure_dev_certificate(Path(cfg.cert_file), Path(cfg.key_file))
    configuration = QuicConfiguration(
        is_client=False,
        alpn_protocols=ALPN,
        max_datagram_frame_size=MAX_DATAGRAM_FRAME_SIZE,
    )
    configuration.load_cert_chain(cfg.cert_file, cfg.key_file)

    return await serve(
        cfg.host,
        cfg.port,
        configuration=configuration,
        create_protocol=partial(_ServerProtocol, on_envelope=on_envelope),
    )
