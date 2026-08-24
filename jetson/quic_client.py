"""
QUIC client: pushes telemetry from the Jetson gateway to the Base Station.

Two-plane mapping onto QUIC (matches the high-frequency / event-driven split):
  * TelemetryFrame  -> QUIC DATAGRAM  (unreliable, latest-wins, low latency)
                       (falls back to the reliable stream if datagrams disabled)
  * Event + GatewayHeartbeat -> a single client-initiated UNIDIRECTIONAL stream,
                       length-delimited (ordered, guaranteed).

This side only SENDS telemetry. It never carries a command message (there is no
such message in the schema). The Base Station cannot actuate the vehicle through
this channel.
"""
from __future__ import annotations

import ssl
from typing import Optional

from aioquic.asyncio import connect
from aioquic.asyncio.protocol import QuicConnectionProtocol
from aioquic.quic.configuration import QuicConfiguration

from common.proto_io import encode_delimited
from common.quic_common import ALPN, MAX_DATAGRAM_FRAME_SIZE
from gen import telemetry_pb2 as pb
from .config import QuicConfig


class _ClientProtocol(QuicConnectionProtocol):
    """Adds datagram/stream send helpers. Runs on the event loop thread."""

    def send_datagram(self, data: bytes) -> None:
        self._quic.send_datagram_frame(data)
        self.transmit()

    def open_uni_stream(self) -> int:
        return self._quic.get_next_available_stream_id(is_unidirectional=True)

    def send_stream(self, stream_id: int, data: bytes) -> None:
        self._quic.send_stream_data(stream_id, data, end_stream=False)
        self.transmit()


class QuicTelemetryClient:
    def __init__(self, cfg: QuicConfig) -> None:
        self._cfg = cfg
        self._cm = None
        self._proto: Optional[_ClientProtocol] = None
        self._uni_stream: Optional[int] = None

    async def start(self) -> None:
        config = QuicConfiguration(
            is_client=True,
            alpn_protocols=ALPN,
            max_datagram_frame_size=MAX_DATAGRAM_FRAME_SIZE,
        )
        if not self._cfg.verify:
            # DEV: accept the backend's self-signed certificate.
            config.verify_mode = ssl.CERT_NONE

        self._cm = connect(
            self._cfg.host,
            self._cfg.port,
            configuration=config,
            create_protocol=_ClientProtocol,
        )
        self._proto = await self._cm.__aenter__()
        self._uni_stream = self._proto.open_uni_stream()

    async def stop(self) -> None:
        if self._cm is not None:
            await self._cm.__aexit__(None, None, None)
            self._cm = None
            self._proto = None

    # ---- send paths --------------------------------------------------------
    def send_frame(self, frame: pb.TelemetryFrame) -> None:
        env = pb.TelemetryEnvelope(frame=frame)
        data = env.SerializeToString()
        assert self._proto is not None
        if self._cfg.use_datagrams_for_frames:
            self._proto.send_datagram(data)
        else:
            self._proto.send_stream(self._uni_stream, encode_delimited(data))

    def send_event(self, event: pb.Event) -> None:
        self._send_reliable(pb.TelemetryEnvelope(event=event))

    def send_gateway_heartbeat(self, hb: pb.GatewayHeartbeat) -> None:
        self._send_reliable(pb.TelemetryEnvelope(heartbeat=hb))

    def _send_reliable(self, env: pb.TelemetryEnvelope) -> None:
        assert self._proto is not None and self._uni_stream is not None
        self._proto.send_stream(self._uni_stream, encode_delimited(env.SerializeToString()))
