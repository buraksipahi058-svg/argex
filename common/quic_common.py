"""
Shared QUIC configuration for the telemetry data plane.

Design notes:
  * ALPN "ugv-telemetry" identifies this application protocol.
  * Datagrams are enabled (max_datagram_frame_size) so the Jetson gateway can
    push high-frequency TelemetryFrames unreliably (latest-wins, low latency).
  * The Base Station is READ-ONLY at the application layer: the server never
    sends application messages. (Transport-level ACKs are not application data.)
  * For a field deployment, replace the self-signed dev certificate with a real
    one and enable verification on the client. The dev helper below only exists
    so the pipeline is runnable end-to-end without a PKI.
"""
from __future__ import annotations

import datetime
import ssl
from pathlib import Path

ALPN = ["ugv-telemetry"]
MAX_DATAGRAM_FRAME_SIZE = 65536


def ensure_dev_certificate(cert_path: Path, key_path: Path, common_name: str = "ugv-base-station") -> None:
    """
    Generate a self-signed certificate + key at the given paths if they do not
    already exist. DEV ONLY. Uses `cryptography`, which is already a dependency
    of aioquic.
    """
    cert_path = Path(cert_path)
    key_path = Path(key_path)
    if cert_path.exists() and key_path.exists():
        return

    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.x509.oid import NameOID

    cert_path.parent.mkdir(parents=True, exist_ok=True)

    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    subject = issuer = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, common_name)])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(issuer)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(days=1))
        .not_valid_after(now + datetime.timedelta(days=3650))
        .add_extension(
            x509.SubjectAlternativeName([x509.DNSName("localhost"), x509.DNSName(common_name)]),
            critical=False,
        )
        .sign(key, hashes.SHA256())
    )

    key_path.write_bytes(
        key.private_bytes(
            encoding=serialization.Encoding.PEM,
            format=serialization.PrivateFormat.TraditionalOpenSSL,
            encryption_algorithm=serialization.NoEncryption(),
        )
    )
    cert_path.write_bytes(cert.public_bytes(serialization.Encoding.PEM))
