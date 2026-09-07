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
// TARET: araç-genişliği/nişan kılavuzu (yeşil). Bunlar sadece BAŞLANGIÇ değerleri;
// asıl ayarı panelde TARET karosundaki "AYAR" butonundan yaparsın (renk, saydamlık,
// kalınlık + köşeleri sürükleme). Seçtiklerin tarayıcıda saklanır (localStorage),
// reload'da kalır. "Sıfırla" bu başlangıç değerlerine döner.
// Köşeler kutuya göre oran [x, y] (sol-üst = 0,0 ; sağ-alt = 1,1). Araç 108 cm.
export const CAMERAS = [
  { key: 'front',  label: 'ÖN',    path: 'cam_front'  },
  { key: 'rear',   label: 'ARKA',  path: 'cam_rear'   },
  {
    key: 'turret', label: 'TARET', path: 'cam_turret',
    guides: {
      enabled: true,
      color: '#22c55e',   // yeşil tonu
      opacity: 0.85,      // 0–1
      lineWidth: 3,       // piksel
      nearLeft:  [0.30, 1.00], nearRight: [0.70, 1.00],  // alt (yakın) kenarlar
      farLeft:   [0.43, 0.52], farRight:  [0.57, 0.52],  // üst (uzak) kenarlar
      bands:     [1.00, 0.78, 0.58],                     // mesafe çizgileri (y oranı)
      // Nişan artısı (crosshair). Lazer kameradan 8.2 cm YUKARIDA olduğu için
      // lazer noktası merkezin biraz ÜSTünde görünür → varsayılan y < 0.5.
      // AYAR modunda artıyı sürükleyip lazerin gerçekten vurduğu yere oturt.
      crosshair: [0.50, 0.42],
      crossSize: 0.10,      // artının boyutu (kutu oranı)
    },
  },
];
