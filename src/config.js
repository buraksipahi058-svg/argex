// Base Station frontend endpoints. The frontend is READ-ONLY: it only reads the
// backend's telemetry WebSocket and the WebRTC (WHEP) video bridge.

const HOST = (typeof window !== 'undefined' && window.location.hostname) || '127.0.0.1';

// Live telemetry (JSON) from the backend WebSocket/REST server (backend/config.yaml ws.port).
export const WS_URL = `ws://${HOST}:8080/ws`;
export const REST_BASE = `http://${HOST}:8080`;

// MediaMTX WebRTC (WHEP) bridge — default WHEP port is 8889.
// The camera/MediaMTX host can differ from the telemetry host (e.g. during
// bring-up MediaMTX runs on the Jetson). Override with VITE_CAMERA_HOST.
const CAMERA_HOST = import.meta.env.VITE_CAMERA_HOST || HOST;
export const WHEP_BASE = `http://${CAMERA_HOST}:8889`;

// The three vehicle cameras (names must match mediamtx/mediamtx.yml paths).
export const CAMERAS = [
  { key: 'front',  label: 'ÖN',    path: 'cam_front'  },
  { key: 'rear',   label: 'ARKA',  path: 'cam_rear'   },
  { key: 'turret', label: 'TARET', path: 'cam_turret' },
];
