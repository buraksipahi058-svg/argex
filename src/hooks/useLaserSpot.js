import { useEffect, useRef, useState } from 'react';

/**
 * Kirmizi lazer noktasi bulucu (yalniz tarayicida calisir — Jetson'a maliyeti YOK).
 *
 * Olcut: her piksel icin "kirmizilik" = R - max(G, B). Parlakliktan bagimsizdir,
 * beyaz/gri yuzeyler 0'a yakin kalir. Guneste bu fark kuculdugu ve sahnedeki
 * kirmizi cisimler (koni, bayrak, mont) ayni olcutu yukselttigi icin ham esik
 * tek basina yetmiyor; bu yuzden dort kademe var:
 *
 *   1) ROI — lazer kameraya PARALEL oldugu icin nokta her zaman nisan artisinin
 *      paralaks hatti uzerindedir. Tum kare yerine o dar seridi tariyoruz;
 *      serit disindaki kirmizi cisimler daha bakilmadan elenir.
 *   2) OTOMATIK ESIK — elle "esik" ayarlamak yerine ROI'nin kirmizilik
 *      dagiliminin ortalama + k*std'si. k tek bir "duyarlilik" kaydiraciyla
 *      gelir; isik degisince esik kendini tasir.
 *   3) BLOB KISITI — lazer kucuk ve izoledir. Esigi asan piksel sayisi ustten
 *      sinirli: genis kirmizi yuzey aday olamaz.
 *   4) SUREKLILIK — bir aday, ard arda iki karede ayni civarda gorulmeden
 *      isaretlenmez; tek karelik parlamalar (yansima, gurultu) elenir.
 *
 * ARKA PLAN FARKI: `learn` true iken (arac telemetrisinde lazer KAPALI ve taret
 * sabit) kirmizilik haritasi ogrenilir; lazer acilinca bu harita cikarilir.
 * Sahnedeki sabit kirmizi cisimler boylece tamamen yok olur. `resetKey`
 * degisince (pan/tilt oynadi) ogrenilen arka plan atilir — artik gecerli degil.
 *
 * Donen koordinatlar KARE oranidir (0..1) — video'nun kendi cercevesine gore,
 * ekrandaki kirpilmis kutuya gore DEGIL.
 */
const DETECT_W = 320;      // tespit cozunurlugu (genislik); yukseklik orantili
const SMOOTH = 0.45;       // EMA katsayisi (1 = yumusatma yok)
const HOLD_MS = 400;       // kaybettikten sonra isareti bu kadar tut (titremesin)
const CONFIRM_R = 0.06;    // iki karedeki aday bu yaricap icindeyse "ayni nokta"
const BG_EMA = 0.25;       // arka plan ogrenme hizi

// duyarlilik 0..1 -> esik katsayilari. Dusuk duyarlilik = daha cok std ve daha
// yuksek mutlak taban (yalniz cok belirgin nokta); yuksek = daha atak.
const kOf = (s) => 7 - 5 * Math.min(1, Math.max(0, s));      // 7σ .. 2σ
const floorOf = (s) => 45 - 35 * Math.min(1, Math.max(0, s)); // 45 .. 10

export function useLaserSpot(videoRef, {
  active = false,
  sensitivity = 0.5,
  fps = 15,
  roi = null,          // { x0, y0, x1, y1 } kare orani; null = tum kare
  learn = false,       // true: nokta arama, arka plani ogren (lazer kapali an)
  resetKey = '',       // degisince arka plan + surekliligi sifirla
  maxBlobFrac = 0.02,  // ROI'nin bu kadarindan buyuk leke lazer degildir
} = {}) {
  const [spot, setSpot] = useState(null);      // { x, y, strength, px } | null
  const [frame, setFrame] = useState(null);    // { w, h } — video'nun oz cozunurlugu
  const emaRef = useRef(null);
  const seenRef = useRef(0);
  const emitRef = useRef(null);
  const candRef = useRef(null);                // onaylanmayi bekleyen aday
  const bgRef = useRef(null);                  // { w, h, key, data: Float32Array }

  // Sik degisen (her karede okunan) girdiler ref'te: effect yeniden kurulmasin.
  const optRef = useRef({ sensitivity, roi, learn, resetKey, maxBlobFrac });
  optRef.current = { sensitivity, roi, learn, resetKey, maxBlobFrac };

  useEffect(() => {
    emaRef.current = null;
    seenRef.current = 0;
    emitRef.current = null;
    candRef.current = null;
    if (!active) return undefined;

    const canvas = document.createElement('canvas');
    const ctx = canvas.getContext('2d', { willReadFrequently: true });
    if (!ctx) return undefined;
    const minGap = 1000 / Math.max(1, fps);
    let raf = 0;
    let last = 0;
    let red = null;   // kirmizilik haritasi (yeniden kullanilan tampon)

    const tick = (t) => {
      raf = requestAnimationFrame(tick);
      if (t - last < minGap) return;
      last = t;

      const video = videoRef.current;
      if (!video || document.hidden) return;
      const vw = video.videoWidth, vh = video.videoHeight;
      if (!vw || !vh) return;
      const o = optRef.current;

      const w = Math.min(DETECT_W, vw);
      const h = Math.max(1, Math.round((vh * w) / vw));
      if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; }
      setFrame((p) => (p && p.w === vw && p.h === vh ? p : { w: vw, h: vh }));

      let data;
      try {
        ctx.drawImage(video, 0, 0, w, h);
        data = ctx.getImageData(0, 0, w, h).data;
      } catch { return; }   // kare henuz yok / kaynak okunamiyor

      if (!red || red.length !== w * h) red = new Float32Array(w * h);
      for (let i = 0, j = 0; j < red.length; i += 4, j++) {
        const g = data[i + 1], b = data[i + 2];
        red[j] = data[i] - (g > b ? g : b);
      }

      // ── arka plan: ogren ya da cikar ───────────────────────────────
      const bg = bgRef.current;
      const bgOk = bg && bg.w === w && bg.h === h && bg.key === o.resetKey;
      if (o.learn) {
        // Lazer KAPALIYKEN sahnenin kendi kirmiziligi: ustune EMA ile yaz.
        if (bgOk) {
          for (let j = 0; j < red.length; j++) bg.data[j] += (red[j] - bg.data[j]) * BG_EMA;
        } else {
          bgRef.current = { w, h, key: o.resetKey, data: Float32Array.from(red) };
        }
        // Ogrenme aninda isaret yok — lazer kapali.
        candRef.current = null;
        if (emitRef.current) { emitRef.current = null; emaRef.current = null; setSpot(null); }
        return;
      }
      if (bgOk) {
        for (let j = 0; j < red.length; j++) {
          const d = red[j] - bg.data[j];
          red[j] = d > 0 ? d : 0;
        }
      } else if (bg) {
        bgRef.current = null;   // pan/tilt oynadi: eski arka plan gecersiz
      }

      // ── ROI (nisan artisinin paralaks seridi) ──────────────────────
      const rx0 = Math.max(0, Math.round((o.roi?.x0 ?? 0) * w));
      const rx1 = Math.min(w - 1, Math.round((o.roi?.x1 ?? 1) * w));
      const ry0 = Math.max(0, Math.round((o.roi?.y0 ?? 0) * h));
      const ry1 = Math.min(h - 1, Math.round((o.roi?.y1 ?? 1) * h));
      if (rx1 <= rx0 || ry1 <= ry0) return;

      // ── tepe + dagilim (ortalama/std) tek gecliste ─────────────────
      let best = -1, bestIdx = 0, sum = 0, sum2 = 0, count = 0;
      for (let y = ry0; y <= ry1; y++) {
        const row = y * w;
        for (let x = rx0; x <= rx1; x++) {
          const v = red[row + x];
          sum += v; sum2 += v * v; count++;
          if (v > best) { best = v; bestIdx = row + x; }
        }
      }
      if (!count) return;
      const mean = sum / count;
      const std = Math.sqrt(Math.max(0, sum2 / count - mean * mean));
      const cut = Math.max(floorOf(o.sensitivity), mean + kOf(o.sensitivity) * std);

      const now = t;
      const lose = () => {
        candRef.current = null;
        if (now - seenRef.current > HOLD_MS) {
          emaRef.current = null;
          if (emitRef.current) { emitRef.current = null; setSpot(null); }
        }
      };
      if (best < cut) { lose(); return; }

      // ── tepe cevresinde agirlikli merkez + leke buyuklugu ──────────
      const px = bestIdx % w, py = (bestIdx / w) | 0;
      const rad = Math.max(6, Math.round(w * 0.06));
      const inner = Math.max(cut, best * 0.6);
      const x0 = Math.max(rx0, px - rad), x1 = Math.min(rx1, px + rad);
      const y0 = Math.max(ry0, py - rad), y1 = Math.min(ry1, py + rad);
      let sx = 0, sy = 0, sw = 0, n = 0;
      for (let y = y0; y <= y1; y++) {
        const row = y * w;
        for (let x = x0; x <= x1; x++) {
          const v = red[row + x];
          if (v >= inner) { sx += x * v; sy += y * v; sw += v; n++; }
        }
      }
      if (!sw) { lose(); return; }

      // Genis kirmizi yuzey (koni, bayrak, mont): lazer degil.
      if (n > Math.max(12, count * o.maxBlobFrac)) { lose(); return; }

      const nx = sx / sw / w, ny = sy / sw / h;

      // ── SUREKLILIK: ayni civarda ikinci kare gormeden isaretleme ───
      const cand = candRef.current;
      const locked = !!emitRef.current && (now - seenRef.current) <= HOLD_MS;
      if (!locked) {
        if (!cand || Math.hypot(cand.x - nx, cand.y - ny) > CONFIRM_R) {
          candRef.current = { x: nx, y: ny };
          return;   // ilk gorus: bir kare daha bekle
        }
      }
      candRef.current = { x: nx, y: ny };

      const prev = emaRef.current;
      const ema = prev
        ? { x: prev.x + (nx - prev.x) * SMOOTH, y: prev.y + (ny - prev.y) * SMOOTH }
        : { x: nx, y: ny };
      emaRef.current = ema;
      seenRef.current = now;

      // Kuantalayip yalniz gercekten degistiyse render tetikle.
      const out = {
        x: Math.round(ema.x * 1000) / 1000,
        y: Math.round(ema.y * 1000) / 1000,
        strength: Math.round(best / 5) * 5,
        px: n,
      };
      const e = emitRef.current;
      if (!e || e.x !== out.x || e.y !== out.y || e.strength !== out.strength) {
        emitRef.current = out;
        setSpot(out);
      }
    };

    raf = requestAnimationFrame(tick);
    return () => {
      cancelAnimationFrame(raf);
      emaRef.current = null;
      emitRef.current = null;
      candRef.current = null;
      setSpot(null);
    };
  }, [active, fps, videoRef]);

  return { spot, frame };
}
