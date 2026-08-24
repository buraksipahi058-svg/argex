"""
Camera video plane (SEPARATE from telemetry). Three cameras on the Jetson are
encoded to H.264/H.265 and streamed low-latency over RTP to the Base Station's
WebRTC bridge (MediaMTX). This never touches the QUIC/Protobuf telemetry plane.

    python -m jetson.video_pipelines [config.yaml]        # launch all cameras
    python -m jetson.video_pipelines --print [config.yaml]# just print pipelines

Default transport is RTSP push (rtspclientsink) to MediaMTX, which carries RTP
under the hood and is the simplest to bridge to WebRTC. A raw RTP/UDP variant is
also provided (see build_udp_pipeline) for setups that prefer udpsink + an SDP.

Encoders:
  * Jetson (preferred): nvv4l2h264enc / nvv4l2h265enc (NVENC, hardware).
  * Fallback (no NVENC): x264enc / x265enc (software) — set encoder accordingly.
"""
from __future__ import annotations

import subprocess
import sys
from typing import List

from .config import CameraConfig, VideoConfig, load_config


def _encoder_chain(encoder: str, bitrate_kbps: int) -> str:
    """GStreamer encode + payload chain for the chosen codec."""
    if encoder == "h265":
        return (
            f"nvv4l2h265enc insert-sps-pps=true idrinterval=30 "
            f"bitrate={bitrate_kbps * 1000} ! h265parse ! rtph265pay config-interval=1 pt=96"
        )
    # default h264
    return (
        f"nvv4l2h264enc insert-sps-pps=true idrinterval=30 "
        f"bitrate={bitrate_kbps * 1000} ! h264parse ! rtph264pay config-interval=1 pt=96"
    )


def build_rtsp_pipeline(cam: CameraConfig, video: VideoConfig) -> str:
    """v4l2 camera -> NVENC -> RTP -> RTSP publish to MediaMTX."""
    caps = f"video/x-raw,width={cam.width},height={cam.height},framerate={cam.fps}/1"
    enc = _encoder_chain(video.encoder, cam.bitrate_kbps)
    rtsp_url = f"rtsp://{video.base_host}:8554/cam_{cam.name}"
    return (
        f"v4l2src device={cam.device} io-mode=2 ! {caps} ! "
        f"nvvidconv ! video/x-raw(memory:NVMM),format=NV12 ! "
        f"{enc} ! rtspclientsink location={rtsp_url} latency=0"
    )


def build_udp_pipeline(cam: CameraConfig, video: VideoConfig) -> str:
    """v4l2 camera -> NVENC -> RTP/UDP (requires an SDP on the receiver side)."""
    caps = f"video/x-raw,width={cam.width},height={cam.height},framerate={cam.fps}/1"
    enc = _encoder_chain(video.encoder, cam.bitrate_kbps)
    return (
        f"v4l2src device={cam.device} io-mode=2 ! {caps} ! "
        f"nvvidconv ! video/x-raw(memory:NVMM),format=NV12 ! "
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
