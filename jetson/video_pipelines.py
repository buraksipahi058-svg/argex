"""
Mode-aware camera supervisor (vehicle-side, ffmpeg -> MediaMTX RTSP push).

USB cameras on the Jetson are software-encoded (low-latency x264) and pushed
over RTSP to the on-Jetson MediaMTX, which bridges them to WebRTC/WHEP for the
Base Station. This video plane is SEPARATE from the QUIC/Protobuf telemetry.

Only the cameras for the CURRENT vehicle mode stream at any time:
    DRIVE  -> front + rear
    LASER  -> turret
The mode comes from the STM `aktifMod` telemetry (CH5 on the transmitter). The
gateway calls `request_mode()` on every STATUS and the supervisor switches
camera sets automatically (stop the old set first, then start the new one).

Why one set at a time:
  * Wi-Fi at ~100 m cannot carry 3x1 Mbps; a single mode's set fits.
  * The shared USB bus runs out of isochronous bandwidth with three cameras
    streaming at once (VIDIOC_STREAMON -> "No space left on device"); capping at
    two simultaneous avoids it. Switching stops-before-starting for the same
    reason (no transient third stream).

NVENC (nvv4l2h264enc) and GStreamer rtspclientsink are unavailable on this unit,
so we encode on the CPU with x264 via ffmpeg.

Standalone (testing, without the gateway):
    python -m jetson.video_pipelines --print [config.yaml]        # print commands
    python -m jetson.video_pipelines --mode=drive [config.yaml]   # run a set
"""
from __future__ import annotations

import asyncio
import logging
import sys
import time
from typing import Dict, List, Optional

from .config import CameraConfig, VideoConfig, load_config

log = logging.getLogger("jetson.video")


def _now_ms() -> int:
    return int(time.time() * 1000)


def build_ffmpeg_cmd(cam: CameraConfig, video: VideoConfig) -> List[str]:
    """v4l2 capture -> low-latency x264 -> RTSP push to MediaMTX (path cam_<name>).

    Mirrors the hand-tuned bring-up command: CBR ~bitrate_kbps with a tight VBV
    (bufsize = bitrate/2) for low latency, 1-second GOP, ultrafast/zerolatency.
    """
    bitrate = f"{cam.bitrate_kbps}k"
    bufsize = f"{max(cam.bitrate_kbps // 2, 1)}k"
    rtsp_url = f"rtsp://{video.base_host}:8554/cam_{cam.name}"
    return [
        "ffmpeg", "-hide_banner", "-nostdin", "-loglevel", "warning",
        "-f", "v4l2",
        "-input_format", cam.input_format,
        "-framerate", str(cam.fps),
        "-video_size", f"{cam.width}x{cam.height}",
        "-fflags", "nobuffer",
        "-i", cam.device,
        "-c:v", "libx264",
        "-preset", "ultrafast",
        "-tune", "zerolatency",
        "-pix_fmt", "yuv420p",
        "-b:v", bitrate, "-maxrate", bitrate, "-bufsize", bufsize,
        "-g", str(cam.fps),
        "-f", "rtsp", "-rtsp_transport", "tcp", rtsp_url,
    ]


def _mode_of(cam: CameraConfig) -> str:
    return (cam.mode or "drive").lower()


def _norm_mode(mode: str) -> str:
    """Only two modes exist here; anything not 'laser' falls back to 'drive'."""
    return "laser" if str(mode).lower() == "laser" else "drive"


class CameraSupervisor:
    """Starts/stops per-camera ffmpeg pushers to match the active vehicle mode.

    Async so start/stop never block the gateway event loop. `request_mode()` is
    cheap (just records intent); `tick()` (called ~2 Hz) applies the debounced
    mode change and restarts any pusher that died (USB glitch, or MediaMTX not
    up yet at startup).
    """

    def __init__(self, video: VideoConfig, debounce_ms: int = 700, stale_ms: int = 1500) -> None:
        self._video = video
        self._debounce_ms = debounce_ms
        self._stale_ms = stale_ms

        self._cams: Dict[str, CameraConfig] = {c.name: c for c in video.cameras}
        self._procs: Dict[str, asyncio.subprocess.Process] = {}

        self._active_mode: Optional[str] = None
        self._requested_mode = "drive"
        self._requested_at = 0
        self._last_request_ms = 0

    # ---- called from the reader task on every STATUS ----------------------
    def request_mode(self, mode: str, now_ms: int) -> None:
        mode = _norm_mode(mode)
        self._last_request_ms = now_ms
        if mode != self._requested_mode:
            self._requested_mode = mode
            self._requested_at = now_ms

    def _cams_for(self, mode: str) -> List[str]:
        return [n for n, c in self._cams.items() if _mode_of(c) == mode]

    # ---- periodic: apply debounced mode + restart dead pushers ------------
    async def tick(self, now_ms: int) -> None:
        desired = self._requested_mode
        # Telemetry gone stale -> fall back to the safe default (drive view).
        if now_ms - self._last_request_ms > self._stale_ms:
            desired = "drive"
            if desired != self._requested_mode:
                self._requested_mode = desired
                self._requested_at = now_ms

        if desired != self._active_mode and (now_ms - self._requested_at) >= self._debounce_ms:
            await self.set_mode(desired)

        await self._restart_dead()

    async def set_mode(self, mode: str) -> None:
        mode = _norm_mode(mode)
        want = set(self._cams_for(mode))
        # STOP unwanted cameras FIRST (release the USB bus before adding any new
        # stream), THEN start the wanted set.
        for name in list(self._procs.keys()):
            if name not in want:
                await self._stop(name)
        for name in want:
            if name not in self._procs:
                await self._start(name)
        if mode != self._active_mode:
            log.info("video: mode -> %s (cameras: %s)", mode, ", ".join(sorted(want)) or "none")
        self._active_mode = mode

    async def _start(self, name: str) -> None:
        cam = self._cams[name]
        cmd = build_ffmpeg_cmd(cam, self._video)
        try:
            proc = await asyncio.create_subprocess_exec(
                *cmd,
                stdin=asyncio.subprocess.DEVNULL,
                stdout=asyncio.subprocess.DEVNULL,
                stderr=asyncio.subprocess.DEVNULL,
            )
        except FileNotFoundError:
            log.error("video: ffmpeg not found on PATH; cannot start cam_%s", name)
            return
        self._procs[name] = proc
        log.info("video: started cam_%s (%s @ %s)", name, cam.device, cam.input_format)

    async def _stop(self, name: str) -> None:
        proc = self._procs.pop(name, None)
        if proc is None:
            return
        if proc.returncode is None:
            try:
                proc.terminate()
                await asyncio.wait_for(proc.wait(), timeout=2.0)
            except asyncio.TimeoutError:
                proc.kill()
                await proc.wait()
            except ProcessLookupError:
                pass
        log.info("video: stopped cam_%s", name)

    async def _restart_dead(self) -> None:
        for name in self._cams_for(self._active_mode or ""):
            proc = self._procs.get(name)
            if proc is not None and proc.returncode is not None:
                log.warning("video: cam_%s exited (rc=%s); restarting", name, proc.returncode)
                self._procs.pop(name, None)
                await self._start(name)

    async def stop_all(self) -> None:
        for name in list(self._procs.keys()):
            await self._stop(name)
        self._active_mode = None


# ==========================================================================
#  Standalone entry point (manual testing without the gateway)
# ==========================================================================
def main() -> None:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    cfg = load_config(args[0] if args else None)

    if "--print" in sys.argv:
        for cam in cfg.video.cameras:
            print(f"# cam_{cam.name} [{_mode_of(cam)}]:")
            print(" ".join(build_ffmpeg_cmd(cam, cfg.video)) + "\n")
        return

    if not cfg.video.enabled:
        print("video.enabled is false in config; use --print to preview commands.")
        return

    mode = "drive"
    for a in sys.argv:
        if a.startswith("--mode="):
            mode = a.split("=", 1)[1]

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(levelname)s %(message)s")

    async def _run() -> None:
        sup = CameraSupervisor(cfg.video)
        try:
            while True:
                now = _now_ms()
                sup.request_mode(mode, now)   # hold the chosen mode in standalone
                await sup.tick(now)
                await asyncio.sleep(0.5)
        finally:
            await sup.stop_all()

    try:
        asyncio.run(_run())
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
