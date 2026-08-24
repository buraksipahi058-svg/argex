#!/usr/bin/env python3
"""
Generate Python protobuf stubs from proto/telemetry.proto into ./gen.

Cross-platform: uses the protoc bundled with grpcio-tools, so no system protoc
is required.

    python scripts/gen_proto.py

Both the Jetson gateway and the Base Station backend import the result as:

    from gen import telemetry_pb2
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PROTO_DIR = ROOT / "proto"
OUT_DIR = ROOT / "gen"


def main() -> int:
    OUT_DIR.mkdir(exist_ok=True)
    (OUT_DIR / "__init__.py").write_text(
        '"""Generated protobuf stubs. Do not edit by hand; run scripts/gen_proto.py."""\n',
        encoding="utf-8",
    )
    protos = sorted(str(p) for p in PROTO_DIR.glob("*.proto"))
    if not protos:
        print(f"No .proto files found in {PROTO_DIR}", file=sys.stderr)
        return 1

    cmd = [
        sys.executable, "-m", "grpc_tools.protoc",
        f"-I{PROTO_DIR}",
        f"--python_out={OUT_DIR}",
        *protos,
    ]
    print("Running:", " ".join(cmd))
    result = subprocess.run(cmd)
    if result.returncode == 0:
        print(f"OK -> generated stubs in {OUT_DIR}")
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
