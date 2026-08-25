"""
Camera video plane (SEPARATE from telemetry). Three cameras on the Jetson are
encoded to H.264/H.265 and streamed low-latency over RTP to the Base Station's
WebRTC bridge (MediaMTX). This never touches the QUIC/Protobuf telemetry plane.

    python -m jetson.video_pipelines [config.yaml]        # launch all cameras
    python -m jetson.video_pipelines --print [config.yaml]# just print pipelines

Default transport is RTSP push (rtspclientsink) to MediaMTX, which carries RTP
under the hood and is the simplest to bridge to WebRTC. A raw RTP/UDP variant is
also provided (see build_udp_pipeline) for setups that prefer udpsink + an SDP.

Encoders (config `video.encoder`):
  * Jetson NVENC (hardware, preferred): "h264" | "h265"  (nvv4l2h264enc/h265enc)
  * Software (CPU, no NVENC needed):     "x264" | "x265" | "openh264"
    Use software when the nvv4l2* GStreamer plugin is unavailable; x264 is the
    best default (tune=zerolatency, speed-preset=ultrafast).
"""
from __future__ import annotations

import subprocess
import sys
from typing import List

from .config import CameraConfig, VideoConfig, load_config


# Software encoders run on CPU (no NVENC / no NVMM memory). Use these when the
# Jetson's nvv4l2* GStreamer plugin is unavailable. x264 is the best default.
_SOFTWARE_ENCODERS = {"x264", "x265", "openh264"}


def _convert_chain(encoder: str) -> str:
    """Colorspace/scale stage feeding the encoder.

    NVENC needs NV12 in NVMM (GPU) memory via nvvidconv; software encoders take
    plain system-memory frames via videoconvert.
    """
    if encoder in _SOFTWARE_ENCODERS:
        return "videoconvert"
    return "nvvidconv ! video/x-raw(memory:NVMM),format=NV12"


def _encoder_chain(encoder: str, bitrate_kbps: int) -> str:
    """GStreamer encode + payload chain for the chosen codec.

    Hardware (Jetson NVENC): h264 | h265
    Software (CPU):          x264 | x265 | openh264
    """
    # --- software (CPU) ---
    if encoder == "x264":
        return (
            f"x264enc tune=zerolatency speed-preset=ultrafast key-int-max=30 "
            f"bitrate={bitrate_kbps} ! h264parse ! rtph264pay config-interval=1 pt=96"
        )
    if encoder == "x265":
        return (
            f"x265enc tune=zerolatency speed-preset=ultrafast key-int-max=30 "
            f"bitrate={bitrate_kbps} ! h265parse ! rtph265pay config-interval=1 pt=96"
        )
    if encoder == "openh264":
        return (
            f"openh264enc bitrate={bitrate_kbps * 1000} gop-size=30 complexity=low "
            f"! h264parse ! rtph264pay config-interval=1 pt=96"
        )
    # --- hardware (Jetson NVENC) ---
    if encoder == "h265":
        return (
            f"nvv4l2h265enc insert-sps-pps=true idrinterval=30 "
            f"bitrate={bitrate_kbps * 1000} ! h265parse ! rtph265pay config-interval=1 pt=96"
        )
    # default h264 (NVENC)
    return (
        f"nvv4l2h264enc insert-sps-pps=true idrinterval=30 "
        f"bitrate={bitrate_kbps * 1000} ! h264parse ! rtph264pay config-interval=1 pt=96"
    )


def build_rtsp_pipeline(cam: CameraConfig, video: VideoConfig) -> str:
    """v4l2 camera -> (NVENC|software) -> RTP -> RTSP publish to MediaMTX."""
    caps = f"video/x-raw,width={cam.width},height={cam.height},framerate={cam.fps}/1"
    convert = _convert_chain(video.encoder)
    enc = _encoder_chain(video.encoder, cam.bitrate_kbps)
    rtsp_url = f"rtsp://{video.base_host}:8554/cam_{cam.name}"
    return (
        f"v4l2src device={cam.device} io-mode=2 ! {caps} ! "
        f"{convert} ! "
        f"{enc} ! rtspclientsink location={rtsp_url} latency=0"
    )


def build_udp_pipeline(cam: CameraConfig, video: VideoConfig) -> str:
    """v4l2 camera -> (NVENC|software) -> RTP/UDP (requires an SDP on the receiver side)."""
    caps = f"video/x-raw,width={cam.width},height={cam.height},framerate={cam.fps}/1"
    convert = _convert_chain(video.encoder)
    enc = _encoder_chain(video.encoder, cam.bitrate_kbps)
    return (
        f"v4l2src device={cam.device} io-mode=2 ! {caps} ! "
        f"{convert} ! "
        f"{enc} ! udpsink host={video.base_host} port={cam.rtp_port} sync=false async=false"
    )


def launch_all(video: VideoConfig, transport: str = "rtsp") -> List[subprocess.Popen]:
    build = build_rtsp_pipeline if transport == "rtsp" else build_udp_pipeline
    procs = []
    for cam in video.cameras:
        pipeline = build(cam, video)
        print(f"[video] cam_{cam.name}: gst-launch-1.0 {pipeline}")
        procs.append(subprocess.Popen(["gst-launch-1.0", "-e", *pipeline.split()]))
    return procs


def main() -> None:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    print_only = "--print" in sys.argv
    cfg = load_config(args[0] if args else None)
    if not cfg.video.enabled and not print_only:
        print("video.enabled is false in config; use --print to preview pipelines.")
        return
    if print_only:
        for cam in cfg.video.cameras:
            print(f"\n# cam_{cam.name} (RTSP push):\ngst-launch-1.0 -e {build_rtsp_pipeline(cam, cfg.video)}")
        return

    procs = launch_all(cfg.video)
    try:
        for p in procs:
            p.wait()
    except KeyboardInterrupt:
        for p in procs:
            p.terminate()


if __name__ == "__main__":
    main()
