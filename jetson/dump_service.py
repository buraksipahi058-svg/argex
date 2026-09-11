#!/usr/bin/env python3
"""
Kayit -> USB dokum servisi (JETSON'da calisir).

Base station'daki "USB'YE AT" dugmesi buraya POST atar; servis TEK ve SABIT bir
komutu calistirir:  bash <UGV_DUMP_SCRIPT> <UGV_DUMP_DEST>
Istemciden komut, parametre veya yol ALINMAZ — bu bir uzaktan kabuk degil, tek
isli bir dugme. Hedef klasor ve script yalniz Jetson'daki ortam degiskenleriyle
belirlenir.

Calistirma (root olarak; script sonunda `sudo umount` var):
    python3 /root/argex/jetson/dump_service.py
Ortam degiskenleri:
    UGV_DUMP_SCRIPT  varsayilan /root/argex/scripts/dump_recordings.sh
    UGV_DUMP_DEST    varsayilan /media/ugv-usb   (USB'nin mount noktasi)
    UGV_DUMP_PORT    varsayilan 8099
    UGV_DUMP_HOST    varsayilan 0.0.0.0

Uclar:
    GET  /health  -> {ok, dest, script}
    GET  /status  -> {state: idle|running|done|failed, rc, lines[], ...}
    POST /dump    -> islemi baslatir (zaten calisiyorsa 409)

NOT: kimlik dogrulamasi yok — arac agi kapali bir LAN oldugu icin yeterli, ama
servis o aga acik demektir. Disari acik bir agda calistirma.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import threading
import time
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# Script ciktisi renkli (ANSI) — panelde ham kacis dizisi gorunmesin diye temizlenir.
_ANSI = re.compile(r"\[[0-9;]*[A-Za-z]")

SCRIPT = os.environ.get("UGV_DUMP_SCRIPT", "/root/argex/scripts/dump_recordings.sh")
DEST = os.environ.get("UGV_DUMP_DEST", "/media/ugv-usb")
PORT = int(os.environ.get("UGV_DUMP_PORT", "8099"))
HOST = os.environ.get("UGV_DUMP_HOST", "0.0.0.0")

_lock = threading.Lock()
_job = {
    "state": "idle",      # idle | running | done | failed
    "rc": None,
    "started_ms": None,
    "finished_ms": None,
    "lines": deque(maxlen=200),
}


def _now_ms() -> int:
    return int(time.time() * 1000)


def _snapshot() -> dict:
    with _lock:
        return {
            "state": _job["state"],
            "rc": _job["rc"],
            "started_ms": _job["started_ms"],
            "finished_ms": _job["finished_ms"],
            "dest": DEST,
            "script": SCRIPT,
            "lines": list(_job["lines"]),
        }


def _run() -> None:
    """Scripti calistirir, ciktisini satir satir toplar. Tek seferde tek is."""
    rc = 1
    try:
        proc = subprocess.Popen(
            ["bash", SCRIPT, DEST],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",                         # locale ne olursa olsun cokme
            bufsize=1,
        )
        for line in proc.stdout:                      # type: ignore[union-attr]
            line = _ANSI.sub("", line).rstrip()
            if line:
                with _lock:
                    _job["lines"].append(line)
        rc = proc.wait()
    except Exception as exc:                          # script yok, bash yok, vb.
        with _lock:
            _job["lines"].append(f"HATA: {exc}")
    finally:
        with _lock:
            _job["rc"] = rc
            _job["state"] = "done" if rc == 0 else "failed"
            _job["finished_ms"] = _now_ms()


class Handler(BaseHTTPRequestHandler):
    server_version = "ugv-dump/1.0"

    def _send(self, code: int, payload: dict) -> None:
        body = json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_OPTIONS(self) -> None:                     # noqa: N802 (http.server API)
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "*")
        self.end_headers()

    def do_GET(self) -> None:                         # noqa: N802
        path = self.path.split("?", 1)[0]
        if path == "/status":
            self._send(200, _snapshot())
        elif path == "/health":
            self._send(200, {"ok": True, "dest": DEST, "script": SCRIPT})
        else:
            self._send(404, {"error": "yok"})

    def do_POST(self) -> None:                        # noqa: N802
        if self.path.split("?", 1)[0] != "/dump":
            self._send(404, {"error": "yok"})
            return
        with _lock:
            if _job["state"] == "running":
                self._send(409, {"error": "zaten calisiyor"})
                return
            _job.update(state="running", rc=None, started_ms=_now_ms(), finished_ms=None)
            _job["lines"].clear()
        threading.Thread(target=_run, daemon=True).start()
        self._send(202, {"ok": True, "dest": DEST})

    def log_message(self, fmt: str, *args) -> None:   # sessiz: erisim logu istemiyoruz
        pass


def main() -> None:
    print(f">> USB dokum servisi: http://{HOST}:{PORT}  script={SCRIPT}  hedef={DEST}", flush=True)
    ThreadingHTTPServer((HOST, PORT), Handler).serve_forever()


if __name__ == "__main__":
    main()
