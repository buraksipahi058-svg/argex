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

// Jetson'daki USB dökum servisi (jetson/dump_service.py) — "USB'YE AT" düğmesi
// buraya POST atar. MediaMTX ile aynı makinede olduğu için CAMERA_HOST'u
// paylaşır; ayrıysa VITE_DUMP_HOST / VITE_DUMP_PORT ile ayarla.
const DUMP_HOST = import.meta.env.VITE_DUMP_HOST || CAMERA_HOST;
export const DUMP_BASE = `http://${DUMP_HOST}:${import.meta.env.VITE_DUMP_PORT || 8099}`;

// The three vehicle cameras (names must match mediamtx/mediamtx.yml paths).
//
// TARET: araç-genişliği/nişan kılavuzu (yeşil). Bunlar sadece BAŞLANGIÇ değerleri;
// asıl ayarı panelde TARET karosundaki "AYAR" butonundan yaparsın (renk, saydamlık,
// kalınlık + köşeleri sürükleme). Seçtiklerin tarayıcıda saklanır (localStorage),
// reload'da kalır. "Sıfırla" bu başlangıç değerlerine döner.
// Köşeler kutuya göre oran [x, y] (sol-üst = 0,0 ; sağ-alt = 1,1). Araç 108 cm.
// ÖN/ARKA için park (geri görüş) kılavuzu: araç genişliğini gösteren yeşil
// çizgiler + mesafe bantları. TARET'teki nişan artısı BURADA YOK — artı yalnız
// `crossK` tanımlıysa çizilir, bu kameralarda tanımlı değil.
const PARK_GUIDES = {
  enabled: true,
  color: '#22c55e',
  opacity: 0.85,
  lineWidth: 3,
  nearLeft:  [0.30, 1.00], nearRight: [0.70, 1.00],  // alt (yakın) kenarlar
  farLeft:   [0.43, 0.52], farRight:  [0.57, 0.52],  // üst (uzak) kenarlar
  bands:     [1.00, 0.78, 0.58],                     // mesafe çizgileri (y oranı)
};

export const CAMERAS = [
  // Kılavuzu her karodaki "AYAR" ile sürükleyerek o kameranın görüş açısına
  // oturtursun; ayar tarayıcıda kamera bazında saklanır (guide:front / guide:rear).
  { key: 'front',  label: 'ÖN',    path: 'cam_front',  guides: { ...PARK_GUIDES } },
  { key: 'rear',   label: 'ARKA',  path: 'cam_rear',   guides: { ...PARK_GUIDES } },
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
      // Nişan artısı (crosshair). Lazer kameraya PARALEL ve 8.2 cm YUKARIDA →
      // lazer noktası merkezin üstünde görünür ve KAYMASI MESAFEYLE değişir
      // (yakında çok yukarı, uzakta merkeze yakın). Bu yüzden dikey yeri
      // paralaks modeliyle mesafeden hesaplanır:  y = 0.5 - crossK / mesafe.
      // AYAR panelindeki "mesafe" kaydırıcısı 1–10 m; artıyı bir kez o mesafedeki
      // lazer noktasına sürükleyince crossK kalibre olur ve TÜM mesafeler oturur.
      crossX: 0.50,         // yatay konum (paralel lazer → merkez)
      crossK: 0.10,         // paralaks sabiti (kalibrasyonla ayarlanır)
      dist: 3,              // AYAR'daki mesafe (m)
      crossSize: 0.10,      // artının boyutu (kutu oranı)
    },
    // Dijital zoom (yakinlastirma). TAMAMEN TARAYICIDA: kare buyutulup kenarlar
    // kirpilir — Jetson'a, encoder'a, banda dokunmaz. Yeni detay KAZANDIRMAZ,
    // sadece mevcut pikselleri buyutur. Nisan noktasi (crosshair) sabit kalsin
    // diye buyutme merkezi crosshair'dir; kilavuz ve lazer isareti goruntuyle
    // birlikte olceklenir, boylece kalibrasyon bozulmaz. AYAR modunda zoom
    // otomatik 1.0'a doner (surukleme koordinatlari sapmasin diye).
    zoom: { enabled: true, initial: 1, min: 1, max: 4, step: 0.25 },

    // LAZER modu: kırmızı nokta bazı zeminlerde/pozlamada zor seçiliyor. Bu mod
    // (a) görüntüyü doygunluk/kontrast ile açar, (b) kareyi tarayıp lazer
    // noktasını halka ile işaretler. Hepsi TARAYICIDA olur — Jetson'a, bant
    // genişliğine, kayda hiç dokunmaz. Karodaki "LAZER" düğmesiyle açılır,
    // yanındaki ⚙ ile ayarlanır; seçimler localStorage'da kalır.
    laser: {
      enabled: true,
      on: false,          // başlangıçta kapalı
      // Eşik ARTIK ELLE AYARLANMIYOR: eşik, arama bölgesindeki kırmızılık
      // dağılımından (ortalama + k·std) her karede kendi hesaplanıyor. Tek
      // ayar "duyarlılık": 0 = sadece çok belirgin nokta (yanlış işaret yok),
      // 1 = atak (soluk noktayı da yakalar, yanılma riski artar).
      sensitivity: 0.5,
      // Arama bölgesi: lazer kameraya paralel olduğu için nokta hep nişan
      // artısının paralaks hattındadır. Şerit yarı-genişliği (kare oranı) —
      // büyütürsen daha geniş tarar, sahnedeki kırmızılara daha açık olur.
      roiHalfWidth: 0.18,
      roiMargin: 0.12,    // şeridin üst/alt payı
      useBackground: true, // lazer KAPALIYKEN arka planı öğren, açılınca çıkar
      saturate: 2.2,      // video CSS filtresi — doygunluk
      contrast: 1.25,     // video CSS filtresi — kontrast
      mark: true,         // bulunan noktayı halka ile işaretle
      color: '#ff3b30',   // işaret rengi (kılavuzun yeşilinden ayrışsın)
      fps: 15,            // tespit hızı (kare/sn) — düşür = daha az CPU
      // ATIŞ GÖRÜŞÜ: sahneyi karartıp griye çeker, SADECE kırmızılık farkını
      // parlak basar. Tespit tutmasa bile göz noktayı bulur. Atıştan hemen
      // önce aç, sonra kapat — sürüş için uygun bir görüntü değildir.
      shot: false,
      shotGain: 3.0,      // kırmızılık farkının parlaklık kazancı
      shotDim: 0.30,      // arka planın (gri) kısılma oranı
    },
  },
];
