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
//
// FRONT: geri görüş kamerası gibi YEŞİL araç-genişliği kılavuzu var (araç 108 cm).
// Köşeler kutuya göre oran [x, y] (sol-üst = 0,0 ; sağ-alt = 1,1). En pratik yol:
// panelde ÖN kameranın sağ üstündeki "KALİBRE" butonuna bas, köşeleri sürükleyip
// araç kenarlarına oturt, alttaki sayıları buraya yapıştır. Renk/opaklık/kalınlık
// da buradan ayarlanır (yeşilin tonu, çizgi yönü/açısı köşe konumlarıyla değişir).
export const CAMERAS = [
  {
    key: 'front', label: 'ÖN', path: 'cam_front',
    guides: {
      enabled: true,
      color: '#22c55e',   // yeşil tonu (istediğin gibi değiştir)
      opacity: 0.85,      // 0–1
      lineWidth: 3,       // piksel
      nearLeft:  [0.30, 1.00], nearRight: [0.70, 1.00],  // alt (araca yakın) kenarlar
      farLeft:   [0.43, 0.52], farRight:  [0.57, 0.52],  // üst (uzak) kenarlar
      bands:     [1.00, 0.78, 0.58],                     // mesafe çizgileri (y oranı)
    },
  },
  { key: 'rear',   label: 'ARKA',  path: 'cam_rear'   },
  { key: 'turret', label: 'TARET', path: 'cam_turret' },
];
