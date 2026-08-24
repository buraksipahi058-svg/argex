"""
Length-delimited framing for protobuf messages sent over a QUIC *stream*.

QUIC datagrams are already message-framed (one datagram == one message), so the
high-frequency TelemetryFrame path needs no delimiting. The reliable stream that
carries Events + GatewayHeartbeats is a byte stream, so each message is prefixed
with a 4-byte big-endian length.
"""
from __future__ import annotations

import struct
from typing import Dict, Iterator

_LEN = struct.Struct(">I")
MAX_MESSAGE_BYTES = 1 << 20  # 1 MiB guard against a desync producing a huge length


def encode_delimited(payload: bytes) -> bytes:
    """Prefix a serialized message with its 4-byte length."""
    return _LEN.pack(len(payload)) + payload


class DelimitedStreamDecoder:
    """
    Accumulates bytes per QUIC stream id and yields complete length-prefixed
    messages. Tolerant of partial reads (QUIC may deliver stream data in chunks).
    """

    def __init__(self) -> None:
        self._buffers: Dict[int, bytearray] = {}

    def feed(self, stream_id: int, data: bytes) -> Iterator[bytes]:
        buf = self._buffers.setdefault(stream_id, bytearray())
        buf.extend(data)
        while len(buf) >= 4:
            (n,) = _LEN.unpack_from(buf, 0)
            if n > MAX_MESSAGE_BYTES:
                # Desync / corruption: drop this stream's buffer rather than
                # allocate unbounded memory.
                buf.clear()
                return
            if len(buf) < 4 + n:
                break
            msg = bytes(buf[4 : 4 + n])
            del buf[: 4 + n]
            yield msg

    def drop(self, stream_id: int) -> None:
        self._buffers.pop(stream_id, None)
