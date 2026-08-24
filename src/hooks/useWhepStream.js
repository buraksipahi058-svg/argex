import { useEffect, useRef, useState } from 'react';
import { WHEP_BASE } from '../config';

/**
 * Minimal WHEP (WebRTC-HTTP Egress Protocol) client for one camera served by
 * MediaMTX. Video is a SEPARATE data plane (RTP/UDP -> MediaMTX -> WebRTC); it
 * never travels over the telemetry WebSocket.
 *
 * Returns { videoRef, state } where state is 'connecting' | 'live' | 'offline'.
 * If MediaMTX / the camera is not up, the tile shows 'offline' and retries.
 */
export function useWhepStream(cameraPath) {
  const videoRef = useRef(null);
  const [state, setState] = useState('connecting');

  useEffect(() => {
    let pc = null;
    let retry = null;
    let cancelled = false;

    const stop = () => {
      if (pc) { try { pc.close(); } catch { /* noop */ } pc = null; }
    };

    const start = async () => {
      stop();
      setState('connecting');
      try {
        pc = new RTCPeerConnection({ iceServers: [] });
        pc.addTransceiver('video', { direction: 'recvonly' });

        pc.ontrack = (e) => {
          if (videoRef.current) videoRef.current.srcObject = e.streams[0];
        };
        pc.onconnectionstatechange = () => {
          if (!pc) return;
          if (pc.connectionState === 'connected') setState('live');
          else if (['failed', 'disconnected', 'closed'].includes(pc.connectionState)) {
            setState('offline');
            if (!cancelled) retry = setTimeout(start, 3000);
          }
        };

        const offer = await pc.createOffer();
        await pc.setLocalDescription(offer);

        const res = await fetch(`${WHEP_BASE}/${cameraPath}/whep`, {
          method: 'POST',
          headers: { 'Content-Type': 'application/sdp' },
          body: offer.sdp,
        });
        if (!res.ok) throw new Error(`WHEP ${res.status}`);
        const answer = await res.text();
        await pc.setRemoteDescription({ type: 'answer', sdp: answer });
      } catch {
        setState('offline');
        if (!cancelled) retry = setTimeout(start, 3000);
      }
    };

    start();
    return () => {
      cancelled = true;
      if (retry) clearTimeout(retry);
      stop();
    };
  }, [cameraPath]);

  return { videoRef, state };
}
