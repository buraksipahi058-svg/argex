import { useState, useEffect, useRef } from 'react';
import { useTelemetry } from './hooks/useTelemetry';
import { useWhepStream } from './hooks/useWhepStream';
import { useLaserSpot } from './hooks/useLaserSpot';
import { useUsbDump } from './hooks/useUsbDump';
import { CAMERAS } from './config';

/* ============================================================
   TEKNOFEST İNSANSIZ KARA ARACI — BASE STATION (READ-ONLY)
   Shows ONLY fields that exist in the ESP32<->Jetson protocol.
   Live data via backend WebSocket; video via WebRTC (WHEP).
   ============================================================ */

// ─── PALETTE ────────────────────────────────────────────────
// Askeri/taktik palet: zeytin-haki zemin, fosfor yesili OK, kehribar uyari,
// isaret fisegi kirmizisi. Kamera overlay'leri zaten koyu oldugu icin uyumlu.
const C = {
  bgPage: '#0f1310', bgCard: '#171c15', bgInner: '#1f251b',
  border: '#39422e', borderStrong: '#5a6644',
  textPrimary: '#dde3cf', textSecondary: '#a6b18c', textMuted: '#79855f', textLabel: '#c0c9a8',
  ok: '#8dbf4a', okBg: '#1c2714',
  warn: '#d9a327', warnBg: '#2c2412',
  fail: '#d1533a', failBg: '#2e150f',
  accent: '#a7bd6b', accentBg: '#232a17',
  modeAuto: '#a7bd6b', modeManual: '#c9b46a', modeLaser: '#d9a327',
};

const MONO = 'JetBrains Mono, ui-monospace, monospace';

const fmtDuration = (ms) => {
  const sec = Math.max(0, Math.floor((ms || 0) / 1000));
  const h = Math.floor(sec / 3600).toString().padStart(2, '0');
  const m = Math.floor((sec % 3600) / 60).toString().padStart(2, '0');
  const s = Math.floor(sec % 60).toString().padStart(2, '0');
  return `${h}:${m}:${s}`;
};

const fmtTs = (ms) => {
  const d = new Date(ms);
  return d.toLocaleTimeString('tr-TR', { hour12: false });
};

// ─── PRIMITIVES (reused from the original mockup) ────────────
const Card = ({ title, tag, children, accent = C.accent, dim = false }) => (
  <section style={{
    background: C.bgCard, border: `1px solid ${C.border}`, borderRadius: 6,
    padding: '8px 10px', display: 'flex', flexDirection: 'column', gap: 6,
    minHeight: 0, overflow: 'hidden', height: '100%',
    boxShadow: '0 1px 2px rgba(0,0,0,0.35)', opacity: dim ? 0.55 : 1,
    transition: 'opacity 0.2s',
  }}>
    <header style={{
      display: 'flex', alignItems: 'center', justifyContent: 'space-between',
      borderBottom: `1px solid ${C.border}`, paddingBottom: 5, flexShrink: 0,
    }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
        <span style={{ width: 3, height: 11, background: accent, display: 'inline-block' }} />
        <h2 style={{ margin: 0, fontSize: 10, letterSpacing: '0.18em', fontWeight: 700, color: C.textPrimary, fontFamily: MONO }}>{title}</h2>
      </div>
      {tag != null && <span style={{ fontFamily: MONO, fontSize: 9, letterSpacing: '0.1em', color: C.textMuted, fontWeight: 600 }}>{tag}</span>}
    </header>
    <div style={{ display: 'flex', flexDirection: 'column', gap: 5, minHeight: 0, flex: 1 }}>{children}</div>
  </section>
);

const Row = ({ label, value, valueColor = C.textPrimary }) => (
  <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
    <span style={{ color: C.textMuted, fontFamily: MONO, fontSize: 10, letterSpacing: '0.06em', textTransform: 'uppercase', fontWeight: 600 }}>{label}</span>
    <span style={{ color: valueColor, fontFamily: MONO, fontWeight: 600, fontSize: 11 }}>{value}</span>
  </div>
);

const Dot = ({ color, pulse = false, size = 8 }) => (
  <span style={{
    width: size, height: size, borderRadius: '50%', background: color,
    boxShadow: `0 0 0 2px ${color}22`, display: 'inline-block', flexShrink: 0,
    animation: pulse ? 'pulse 1.6s ease-in-out infinite' : 'none',
  }} />
);

// Signed motor bar: center = 0, fills right (fwd) or left (rev).
const SignedBar = ({ value, max = 100 }) => {
  const v = Math.max(-max, Math.min(max, value || 0));
  const half = (Math.abs(v) / max) * 50;
  const left = v >= 0 ? 50 : 50 - half;
  const color = v > 0 ? C.ok : v < 0 ? C.warn : C.border;
  return (
    <div style={{ position: 'relative', width: '100%', height: 6, background: C.bgInner, border: `1px solid ${C.border}`, borderRadius: 2, overflow: 'hidden' }}>
      <div style={{ position: 'absolute', left: '50%', top: 0, bottom: 0, width: 1, background: C.borderStrong }} />
      <div style={{ position: 'absolute', left: `${left}%`, top: 0, bottom: 0, width: `${half}%`, background: color }} />
    </div>
  );
};

const Bar = ({ percent, color = C.accent }) => (
  <div style={{ width: '100%', height: 5, background: C.bgInner, border: `1px solid ${C.border}`, borderRadius: 2, overflow: 'hidden' }}>
    <div style={{ width: `${Math.max(0, Math.min(100, percent))}%`, height: '100%', background: color }} />
  </div>
);

// ─── LINK STATE HELPERS ──────────────────────────────────────
const linkColor = (up) => (up ? C.ok : C.fail);

const LinkRow = ({ label, up, meta, unknown = false }) => (
  <div style={{
    display: 'flex', alignItems: 'center', gap: 7, padding: '4px 7px',
    background: unknown ? C.bgInner : up ? C.bgInner : C.failBg,
    border: `1px solid ${unknown ? C.border : up ? C.border : C.fail + '55'}`,
    borderRadius: 3, flex: 1, minHeight: 0,
  }}>
    <Dot color={unknown ? C.textMuted : linkColor(up)} pulse={!unknown && !up} size={7} />
    <div style={{ flex: 1, minWidth: 0 }}>
      <div style={{ fontSize: 10, color: C.textPrimary, fontFamily: MONO, fontWeight: 600, lineHeight: 1.2 }}>{label}</div>
      {meta && <div style={{ fontSize: 8, color: C.textMuted, fontFamily: MONO, letterSpacing: '0.04em', lineHeight: 1.2, fontWeight: 600 }}>{meta}</div>}
    </div>
    <span style={{ fontSize: 9, color: unknown ? C.textMuted : up ? C.ok : C.fail, fontFamily: MONO, letterSpacing: '0.08em', fontWeight: 700 }}>
      {unknown ? '—' : up ? 'UP' : 'DOWN'}
    </span>
  </div>
);

// ─── PANELS ──────────────────────────────────────────────────
const VehicleControlPanel = ({ telemetry, stale }) => {
  const c = telemetry?.control;
  const s = telemetry?.status;
  const im = telemetry?.imu;
  const MODE_COLOR = { LASER: C.modeLaser, DRIVE: C.accent, AUTO: C.modeAuto };
  const modeColor = MODE_COLOR[c?.mode] ?? C.textMuted;
  // aktifMod === AUTO means CH8 handed authority to the Jetson; autonomous_active
  // (durum.AUTO_EN) means the firmware actually accepted it. They can disagree:
  // AUTO + not engaged = the vehicle is waiting on a fresh AUTO_REQ command.
  const opAuto = !!s?.autonomous_active;
  const autoPending = c?.mode === 'AUTO' && !opAuto;
  return (
    <Card title="VEHICLE CONTROL" tag="STATUS" accent={C.accent} dim={stale}>
      <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 6, padding: '5px 7px', background: C.bgInner, border: `1px solid ${C.border}`, borderRadius: 3 }}>
        <div>
          <div style={{ fontSize: 8, color: C.textMuted, letterSpacing: '0.12em', fontWeight: 700 }}>MODE</div>
          <div style={{ fontSize: 13, fontWeight: 700, color: modeColor, fontFamily: MONO }}>{c?.mode ?? '—'}</div>
        </div>
        <div>
          <div style={{ fontSize: 8, color: C.textMuted, letterSpacing: '0.12em', fontWeight: 700 }}>OPERATION</div>
          <div style={{ fontSize: 13, fontWeight: 700, color: opAuto ? C.modeAuto : autoPending ? C.warn : C.modeManual, fontFamily: MONO }}>
            {c ? (opAuto ? 'AUTONOMOUS' : autoPending ? 'AUTO PENDING' : 'MANUAL') : '—'}
          </div>
        </div>
      </div>

      <div>
        <Row label="Sol Motor" value={c ? `${c.left_motor} %` : '—'} valueColor={C.textPrimary} />
        <SignedBar value={c?.left_motor ?? 0} />
      </div>
      <div>
        <Row label="Sağ Motor" value={c ? `${c.right_motor} %` : '—'} valueColor={C.textPrimary} />
        <SignedBar value={c?.right_motor ?? 0} />
      </div>

      <Row label="Pan (yatay)" value={c ? `${c.pan_deg}°` : '—'} />
      <Bar percent={((c?.pan_deg ?? 0) / 180) * 100} color={C.accent} />
      <Row label="Tilt (dikey)" value={c ? `${c.tilt_deg}°` : '—'} />
      <Bar percent={((c?.tilt_deg ?? 0) / 180) * 100} color={C.accent} />

      {/* Attitude from the BNO055 IMU (TYPE_IMU 0x04) — telemetry only. */}
      <Row label="Pitch (IMU)" value={im?.present ? `${im.pitch_deg}°` : '—'} />
      <SignedBar value={im?.present ? Math.max(-100, Math.min(100, im.pitch_deg)) : 0} />
      <Row label="Yaw / Heading" value={im?.present ? `${im.yaw_deg}°` : '—'} />
      <Bar percent={((im?.yaw_deg ?? 0) / 360) * 100} color={C.accent} />
      <div style={{ fontSize: 8, color: C.textMuted, fontFamily: MONO, letterSpacing: '0.06em', fontWeight: 600, marginTop: -2 }}>
        IMU CAL · sys {im?.present ? im.cal_sys : '–'} · gyro {im?.present ? im.cal_gyro : '–'} · acc {im?.present ? im.cal_accel : '–'} · mag {im?.present ? im.cal_mag : '–'}
      </div>

      <div style={{
        display: 'flex', alignItems: 'center', gap: 8, padding: '5px 7px', marginTop: 'auto',
        background: c?.laser_on ? C.failBg : C.bgInner,
        border: `1px solid ${c?.laser_on ? C.fail + '44' : C.border}`, borderRadius: 3,
      }}>
        <Dot color={c?.laser_on ? C.fail : C.textMuted} pulse={!!c?.laser_on} size={7} />
        <span style={{ fontSize: 10, color: c?.laser_on ? C.fail : C.textMuted, fontFamily: MONO, letterSpacing: '0.08em', fontWeight: 700 }}>
          LAZER · {c ? (c.laser_on ? 'ON' : 'OFF') : '—'}
        </span>
      </div>
    </Card>
  );
};

const LinksPanel = ({ telemetry, conn, wsConnected }) => {
  const s = telemetry?.status;
  const l = telemetry?.link;
  const known = wsConnected && conn.base_link_up;  // can we trust vehicle fields?
  return (
    <Card title="LINKS · TELEMETRY" tag={l ? `${l.status_rate_hz.toFixed(0)} Hz` : '—'} accent={C.accent}>
      <div style={{ display: 'flex', flexDirection: 'column', gap: 3, flex: 1 }}>
        {/* Browser <-> Backend */}
        <LinkRow label="Base Station Server" up={wsConnected} meta="browser ↔ backend (WS)" />
        {/* Gateway <-> Backend (backend-derived) */}
        <LinkRow label="Jetson Gateway Link" up={conn.base_link_up} meta="gateway ↔ base (QUIC)" unknown={!wsConnected} />
        {/* STATUS freshness (gateway-derived) */}
        <LinkRow label="ESP32 Telemetry" up={conn.stm_status_fresh} meta={l ? `last STATUS ${fmtTs(l.last_status_unix_ms)}` : 'stream'} unknown={!known} />
        {/* RC/ELRS link — vehicle FIELD */}
        <LinkRow label="ELRS (RC) Link" up={!!s?.elrs_link_up} meta="STATUS.elrsLink" unknown={!known} />
        {/* The controller's own view of the ESP32<->Jetson link — durum bit */}
        <LinkRow label="ESP32 ↔ Jetson (araç görüşü)" up={!!s?.jetson_link_up} meta="durum.JETSON_LINK" unknown={!known} />
      </div>
      <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 4, paddingTop: 4, borderTop: `1px solid ${C.border}` }}>
        <Row label="Paket Kaybı" value={l ? l.packets_lost_total : '—'} valueColor={l?.packets_lost_total ? C.warn : C.textPrimary} />
        <Row label="Seq" value={telemetry ? telemetry.seq : '—'} />
        <Row label="HB Hz" value={l ? l.heartbeat_rate_hz.toFixed(1) : '—'} />
        <Row label="STM Uptime" value={l ? fmtDuration(l.stm_uptime_ms) : '—'} />
      </div>
    </Card>
  );
};

const SafetyPanel = ({ telemetry, stale }) => {
  const s = telemetry?.status;
  const block = (label, active, activeText, inactiveText, goodWhenInactive = true) => {
    const good = goodWhenInactive ? !active : active;
    const color = good ? C.ok : C.fail;
    return (
      <div style={{ display: 'flex', alignItems: 'center', gap: 8, padding: '5px 7px', background: good ? C.okBg : C.failBg, border: `1px solid ${color}44`, borderRadius: 3 }}>
        <Dot color={color} pulse={!good} size={7} />
        <span style={{ fontSize: 9, color: C.textLabel, fontFamily: MONO, letterSpacing: '0.1em', fontWeight: 700 }}>{label}</span>
        <span style={{ marginLeft: 'auto', fontSize: 10, color, fontFamily: MONO, letterSpacing: '0.08em', fontWeight: 700 }}>
          {s ? (active ? activeText : inactiveText) : '—'}
        </span>
      </div>
    );
  };
  // Arming is a state, not a fault: DISARMED is the correct, safe reading in
  // turret mode and right after any mode change, so it gets a neutral row
  // instead of the red/green fault treatment.
  const neutral = (label, active, activeText, inactiveText) => {
    const color = active ? C.accent : C.textMuted;
    return (
      <div style={{ display: 'flex', alignItems: 'center', gap: 8, padding: '5px 7px', background: active ? C.accentBg : C.bgInner, border: `1px solid ${color}44`, borderRadius: 3 }}>
        <Dot color={color} size={7} />
        <span style={{ fontSize: 9, color: C.textLabel, fontFamily: MONO, letterSpacing: '0.1em', fontWeight: 700 }}>{label}</span>
        <span style={{ marginLeft: 'auto', fontSize: 10, color, fontFamily: MONO, letterSpacing: '0.08em', fontWeight: 700 }}>
          {s ? (active ? activeText : inactiveText) : '—'}
        </span>
      </div>
    );
  };
  return (
    <Card title="SAFETY (REPORTED)" tag="VEHICLE" accent={C.fail} dim={stale}>
      {block('FAILSAFE', !!s?.failsafe_active, 'ACTIVE', 'CLEAR')}
      {block('HW FAULT', !!s?.hw_error, 'LATCHED', 'CLEAR')}
      {neutral('MOTOR ARM', !!s?.motor_armed, 'ARMED', 'DISARMED')}
      {block('CMD TIMEOUT', !!s?.cmd_timeout, 'TIMEOUT', 'FRESH')}
      {block('CRC ERROR', !!s?.crc_error_recent, 'RECENT', 'CLEAR')}
      {/* Fills the panel's leftover height; the rows above keep their natural size. */}
      <div style={{ flex: 1, minHeight: 60, marginTop: 2, borderRadius: 3, overflow: 'hidden', border: `1px solid ${C.border}` }}>
        <img src="/fatih.jpeg" alt="" style={{ width: '100%', height: '100%', objectFit: 'cover', display: 'block' }} />
      </div>
      <div style={{ fontSize: 8, color: C.textMuted, fontFamily: MONO, lineHeight: 1.4 }}>
        raw durum: 0x{(s?.raw_durum ?? 0).toString(16).padStart(2, '0').toUpperCase()} · reported by STM; safety is enforced on-vehicle, not here.
      </div>
    </Card>
  );
};

const EVENT_SEV = (type) => {
  if (/LOST|ACTIVATED|SET|CRC_ERROR|STALE/.test(type)) return 'fail';
  if (/RESTORED|CLEARED|DISENGAGED/.test(type)) return 'ok';
  return 'info';
};
const SEV_COLOR = { fail: C.fail, ok: C.ok, info: C.accent };
const SEV_BG = { fail: C.failBg, ok: C.okBg, info: C.accentBg };

const EventsPanel = ({ events }) => {
  const list = [...events].reverse();
  return (
    <Card title="EVENTS" tag={`${events.length}`} accent={C.warn}>
      <div style={{ display: 'flex', flexDirection: 'column', gap: 3, overflowY: 'auto', flex: 1, minHeight: 0 }}>
        {list.length === 0 && (
          <div style={{ padding: 8, background: C.okBg, border: `1px solid ${C.ok}44`, borderRadius: 3, display: 'flex', alignItems: 'center', gap: 8 }}>
            <Dot color={C.ok} size={7} />
            <span style={{ fontFamily: MONO, fontSize: 10, color: C.ok, letterSpacing: '0.08em', fontWeight: 700 }}>NO EVENTS</span>
          </div>
        )}
        {list.map((e, i) => {
          const sev = EVENT_SEV(e.type);
          return (
            <div key={`${e.ts}-${i}`} style={{ padding: '4px 7px', background: SEV_BG[sev], borderLeft: `3px solid ${SEV_COLOR[sev]}`, borderRadius: '2px 3px 3px 2px' }}>
              <div style={{ display: 'flex', justifyContent: 'space-between' }}>
                <span style={{ fontFamily: MONO, fontSize: 9.5, color: SEV_COLOR[sev], letterSpacing: '0.04em', fontWeight: 700 }}>{e.type}</span>
                <span style={{ fontFamily: MONO, fontSize: 8.5, color: C.textMuted, fontWeight: 600 }}>{fmtTs(e.ts)}</span>
              </div>
              {e.detail && <div style={{ fontFamily: MONO, fontSize: 9, color: C.textSecondary, marginTop: 1 }}>{e.detail}</div>}
            </div>
          );
        })}
      </div>
    </Card>
  );
};

// Aracın gideceği koridoru gösteren yeşil kılavuz (geri görüş kamerası gibi).
// Köşeler 0–1 oran; viewBox 0..100'e ölçeklenir, çizgi kalınlığı non-scaling
// olduğu için ekranda sabit piksel kalır.
const GuideOverlay = ({ g, editable, onHandleDown, aspect = 1, zoom = 1 }) => {
  // Katman CSS scale() ile buyuyor: cizgiler ve arti da buyurdu. Geometri
  // goruntuyle birlikte olceklenmeli (hizalama icin), ama KALINLIK ve ARTININ
  // BOYU ekranda sabit kalmali — bu yuzden zoom'a bolüyoruz.
  const lw = (g.lineWidth || 1) / (zoom || 1);
  const P = ([x, y]) => `${x * 100},${y * 100}`;
  const edgeX = (near, far, y) => {
    const denom = far[1] - near[1] || 1;
    const t = (y - near[1]) / denom;                 // near→far interpolasyon
    return near[0] + t * (far[0] - near[0]);
  };
  const line = (p1, p2, key) => (
    <polyline key={key} points={`${P(p1)} ${P(p2)}`} fill="none"
      stroke={g.color} strokeOpacity={g.opacity} strokeWidth={lw}
      strokeLinecap="round" vectorEffect="non-scaling-stroke" />
  );
  return (
    <svg viewBox="0 0 100 100" preserveAspectRatio="none"
      style={{ position: 'absolute', inset: 0, width: '100%', height: '100%', pointerEvents: editable ? 'auto' : 'none' }}>
      {line(g.nearLeft, g.farLeft, 'L')}
      {line(g.nearRight, g.farRight, 'R')}
      {(g.bands || []).map((y, i) => (
        <line key={`b${i}`}
          x1={edgeX(g.nearLeft, g.farLeft, y) * 100} y1={y * 100}
          x2={edgeX(g.nearRight, g.farRight, y) * 100} y2={y * 100}
          stroke={g.color} strokeOpacity={g.opacity} strokeWidth={lw}
          strokeLinecap="round" vectorEffect="non-scaling-stroke" />
      ))}
      {g.crossK != null && (() => {
        const cxF = g.crossX ?? 0.5;
        const cyF = Math.min(1, Math.max(0, 0.5 - g.crossK / (g.dist || 3)));  // paralaks
        const cx = cxF * 100, cy = cyF * 100;
        // viewBox 100x100 karoya GERILEREK oturuyor (preserveAspectRatio=none),
        // yani 1 birim x != 1 birim y. Kilavuz oyle kalibre edildigi icin oyle
        // kaliyor, ama artinin kollari ekranda ESIT olmali: yatay yariciapi
        // en/boy oranina bolüyoruz. Boylece genis karoda da kare bir arti.
        const halfY = (g.crossSize || 0.1) * 100 / 2 / (zoom || 1);
        const halfX = halfY / (aspect || 1);
        const gapY = halfY * 0.32, gapX = halfX * 0.32;
        const tickY = halfY * 0.30, tickX = halfX * 0.30;
        const st = { stroke: g.color, strokeOpacity: g.opacity, strokeWidth: lw, strokeLinecap: 'round', vectorEffect: 'non-scaling-stroke' };
        // Koyu kontur: ayni geometri once siyah ve daha kalin cizilir; acik
        // zeminde (beton, gokyuzu) arti kaybolmasin diye.
        const halo = { stroke: '#000', strokeOpacity: Math.min(1, (g.opacity ?? 1) * 0.55), strokeWidth: lw + 2 / (zoom || 1), strokeLinecap: 'round', vectorEffect: 'non-scaling-stroke' };
        const arms = (p) => (
          <>
            <line x1={cx} y1={cy - halfY} x2={cx} y2={cy - gapY} {...p} />
            <line x1={cx} y1={cy + gapY} x2={cx} y2={cy + halfY} {...p} />
            <line x1={cx - halfX} y1={cy} x2={cx - gapX} y2={cy} {...p} />
            <line x1={cx + halfX} y1={cy} x2={cx + gapX} y2={cy} {...p} />
            {/* yatay kolun uclarindaki cizikler: sapmayi goz karariyla olcmek icin */}
            <line x1={cx - halfX} y1={cy - tickY / 2} x2={cx - halfX} y2={cy + tickY / 2} {...p} />
            <line x1={cx + halfX} y1={cy - tickY / 2} x2={cx + halfX} y2={cy + tickY / 2} {...p} />
            <line x1={cx - tickX / 2} y1={cy - halfY} x2={cx + tickX / 2} y2={cy - halfY} {...p} />
          </>
        );
        return (
          <g>
            {arms(halo)}
            {arms(st)}
            <ellipse cx={cx} cy={cy} rx={0.9 / (aspect || 1) / (zoom || 1)} ry={0.9 / (zoom || 1)} fill="none" {...halo} />
            <ellipse cx={cx} cy={cy} rx={0.9 / (aspect || 1) / (zoom || 1)} ry={0.9 / (zoom || 1)} fill="none" {...st} />
            {editable && (
              <ellipse cx={cx} cy={cy} rx={2.6 / (aspect || 1) / (zoom || 1)} ry={2.6 / (zoom || 1)} fill={g.color} fillOpacity="0.35" stroke="#dde3cf" strokeWidth="1"
                vectorEffect="non-scaling-stroke" style={{ cursor: 'grab' }} onPointerDown={(e) => onHandleDown('crosshair', e)} />
            )}
          </g>
        );
      })()}
      {editable && ['nearLeft', 'nearRight', 'farLeft', 'farRight'].map((k) => (
        <circle key={k} cx={g[k][0] * 100} cy={g[k][1] * 100} r={2 / (zoom || 1)}
          fill="#dde3cf" stroke={g.color} strokeWidth="1" vectorEffect="non-scaling-stroke"
          style={{ cursor: 'grab' }} onPointerDown={(e) => onHandleDown(k, e)} />
      ))}
    </svg>
  );
};

// ATIS GORUSU: sahneyi karartilmis gri olarak, kirmizilik farkini
// (R - max(G,B)) parlak basar. Griye cevirmek TEK BASINA ise yaramaz — renk
// bilgisini siler; burada gri yalnizca ZEMIN, isaret eden sey farkin kendisi.
// Video ile ayni oz cozunurluk + `objectFit: cover`: kirpma birebir ayni.
const ShotView = ({ videoRef, active, fps = 15, gain = 3, dim = 0.3, color = '#ff3b30' }) => {
  const ref = useRef(null);
  useEffect(() => {
    if (!active) return undefined;
    const cv = ref.current;
    const ctx = cv?.getContext('2d', { willReadFrequently: true });
    if (!ctx) return undefined;
    const tint = [parseInt(color.slice(1, 3), 16), parseInt(color.slice(3, 5), 16), parseInt(color.slice(5, 7), 16)];
    const minGap = 1000 / Math.max(1, fps);
    let raf = 0, last = 0;
    const tick = (t) => {
      raf = requestAnimationFrame(tick);
      if (t - last < minGap) return;
      last = t;
      const v = videoRef.current;
      if (!v || document.hidden || !v.videoWidth) return;
      const w = Math.min(480, v.videoWidth);
      const h = Math.max(1, Math.round((v.videoHeight * w) / v.videoWidth));
      if (cv.width !== w || cv.height !== h) { cv.width = w; cv.height = h; }
      let img;
      try {
        ctx.drawImage(v, 0, 0, w, h);
        img = ctx.getImageData(0, 0, w, h);
      } catch { return; }
      const d = img.data;
      for (let i = 0; i < d.length; i += 4) {
        const r = d[i], g = d[i + 1], b = d[i + 2];
        const diff = r - (g > b ? g : b);
        const gray = (r * 0.299 + g * 0.587 + b * 0.114) * dim;
        if (diff > 8) {
          const k = Math.min(1, (diff * gain) / 255);
          d[i]     = gray + (tint[0] - gray) * k;
          d[i + 1] = gray + (tint[1] - gray) * k;
          d[i + 2] = gray + (tint[2] - gray) * k;
        } else {
          d[i] = d[i + 1] = d[i + 2] = gray;
        }
      }
      ctx.putImageData(img, 0, 0);
    };
    raf = requestAnimationFrame(tick);
    return () => cancelAnimationFrame(raf);
  }, [active, fps, gain, dim, color, videoRef]);
  if (!active) return null;
  return <canvas ref={ref} style={{ position: 'absolute', inset: 0, width: '100%', height: '100%', objectFit: 'cover', pointerEvents: 'none' }} />;
};

// Lazer noktasi isareti. Canvas video ile AYNI oz cozunurlukte ve ayni
// `objectFit: cover` stiliyle duruyor — boylece tarayicinin kirpmasi ikisinde
// birebir ayni olur ve kare koordinatlarini elde donusturmeye gerek kalmaz.
const LaserMarker = ({ spot, frame, color }) => {
  const ref = useRef(null);
  useEffect(() => {
    const cv = ref.current;
    if (!cv || !frame) return;
    const ctx = cv.getContext('2d');
    if (!ctx) return;
    ctx.clearRect(0, 0, cv.width, cv.height);
    if (!spot) return;
    const x = spot.x * cv.width, y = spot.y * cv.height;
    const r = Math.max(7, cv.width * 0.035);
    const lw = Math.max(2, cv.width * 0.004);
    // Once beyaz kontur: koyu ve acik zeminde de halka gorunur kalsin.
    ctx.lineWidth = lw * 2.2; ctx.strokeStyle = 'rgba(255,255,255,0.55)';
    ctx.beginPath(); ctx.arc(x, y, r, 0, Math.PI * 2); ctx.stroke();
    ctx.lineWidth = lw; ctx.strokeStyle = color;
    ctx.beginPath(); ctx.arc(x, y, r, 0, Math.PI * 2); ctx.stroke();
    ctx.beginPath();
    for (const [dx, dy] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) {
      ctx.moveTo(x + dx * r * 1.25, y + dy * r * 1.25);
      ctx.lineTo(x + dx * r * 1.9, y + dy * r * 1.9);
    }
    ctx.stroke();
  }, [spot, frame, color]);
  if (!frame) return null;
  return (
    <canvas ref={ref} width={frame.w} height={frame.h}
      style={{ position: 'absolute', inset: 0, width: '100%', height: '100%', objectFit: 'cover', pointerEvents: 'none' }} />
  );
};

const CameraTile = ({ cam, grow = 1, telemetry = null }) => {
  const { videoRef, state } = useWhepStream(cam.path);
  const badge = state === 'live' ? C.ok : state === 'connecting' ? C.warn : C.fail;
  const boxRef = useRef(null);
  const dragRef = useRef(null);
  const hasGuides = !!cam.guides?.enabled;
  const LS_KEY = `guide:${cam.key}`;
  const [edit, setEdit] = useState(false);
  const [g, setG] = useState(() => {
    if (!cam.guides) return cam.guides;
    try {
      const s = localStorage.getItem(LS_KEY);
      return s ? { ...cam.guides, ...JSON.parse(s) } : cam.guides;
    } catch { return cam.guides; }
  });
  // Ayarları tarayıcıda sakla (koda girmeden kalıcı olsun).
  useEffect(() => {
    if (!cam.guides) return;
    try { localStorage.setItem(LS_KEY, JSON.stringify(g)); } catch { /* storage yok/kapalı */ }
  }, [g, cam.guides, LS_KEY]);

  const r2 = (v) => Math.round(v * 100) / 100;
  const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
  const setOpacity = (d) => setG((p) => ({ ...p, opacity: r2(clamp(p.opacity + d, 0.1, 1)) }));
  const setWidth = (d) => setG((p) => ({ ...p, lineWidth: clamp(p.lineWidth + d, 1, 8) }));
  const resetGuides = () => { try { localStorage.removeItem(LS_KEY); } catch { /* ignore */ } setG(cam.guides); };

  // ── LAZER modu ────────────────────────────────────────────
  const hasLaser = !!cam.laser?.enabled;
  const LZ_KEY = `laser:${cam.key}`;
  const [showLz, setShowLz] = useState(false);   // ⚙ ayar seridi acik mi
  const [lz, setLz] = useState(() => {
    if (!cam.laser) return cam.laser;
    try {
      const s = localStorage.getItem(LZ_KEY);
      return s ? { ...cam.laser, ...JSON.parse(s) } : cam.laser;
    } catch { return cam.laser; }
  });
  useEffect(() => {
    if (!cam.laser) return;
    try { localStorage.setItem(LZ_KEY, JSON.stringify(lz)); } catch { /* storage yok/kapali */ }
  }, [lz, cam.laser, LZ_KEY]);
  const laserOn = hasLaser && lz.on;
  const shotOn = hasLaser && !!lz.shot && state === 'live';
  // Lazer kameraya paralel: nokta hep nisan artisinin paralaks hattinda. Arama
  // seridini oradan cikariyoruz (1 m'de en yukarida, uzakta merkeze yakin).
  const roi = hasGuides && g.crossK != null ? (() => {
    const hw = lz?.roiHalfWidth ?? 0.18, m = lz?.roiMargin ?? 0.12;
    const yNear = 0.5 - (g.crossK ?? 0) / 1;     // ~1 m
    const yFar = 0.5 - (g.crossK ?? 0) / 10;     // ~10 m
    return {
      x0: clamp((g.crossX ?? 0.5) - hw, 0, 1), x1: clamp((g.crossX ?? 0.5) + hw, 0, 1),
      y0: clamp(Math.min(yNear, yFar) - m, 0, 1), y1: clamp(Math.max(yNear, yFar) + m, 0, 1),
    };
  })() : null;
  // Arac telemetrisi: lazer KAPALIYKEN arka plan ogrenilir, acilinca cikarilir
  // — sahnedeki sabit kirmizi cisimler (koni, bayrak) boylece yok olur. Taret
  // oynarsa ogrenilen arka plan gecersiz: resetKey pan/tilt ile degisir.
  const tc = cam.key === 'turret' ? telemetry?.control : null;
  const useBg = (lz?.useBackground ?? true) && !!tc;
  const { spot, frame } = useLaserSpot(videoRef, {
    active: laserOn && lz.mark && state === 'live',
    sensitivity: lz?.sensitivity ?? 0.5,
    fps: lz?.fps ?? 15,
    roi,
    learn: useBg && tc?.laser_on === false,
    resetKey: useBg ? `${tc?.pan_deg ?? ''}|${tc?.tilt_deg ?? ''}` : '',
  });
  // Karo en/boy orani: arti kollarinin ekranda esit uzunlukta cikmasi icin.
  const [aspect, setAspect] = useState(1);
  useEffect(() => {
    const el = boxRef.current;
    if (!el || typeof ResizeObserver === 'undefined') return;
    const ro = new ResizeObserver(([e]) => {
      const { width, height } = e.contentRect;
      if (width > 0 && height > 0) setAspect(width / height);
    });
    ro.observe(el);
    return () => ro.disconnect();
  }, []);

  // ── Dijital zoom ──────────────────────────────────────────
  const hasZoom = !!cam.zoom?.enabled;
  const ZM_KEY = `zoom:${cam.key}`;
  const [zoom, setZoom] = useState(() => {
    const init = cam.zoom?.initial ?? 1;
    try {
      const v = parseFloat(localStorage.getItem(ZM_KEY));
      return Number.isFinite(v) ? v : init;
    } catch { return init; }
  });
  useEffect(() => {
    if (!hasZoom) return;
    try { localStorage.setItem(ZM_KEY, String(zoom)); } catch { /* storage yok/kapali */ }
  }, [zoom, hasZoom, ZM_KEY]);
  const bumpZoom = (d) => setZoom((z) => r2(clamp(z + d, cam.zoom?.min ?? 1, cam.zoom?.max ?? 4)));
  const zoomActive = hasZoom ? zoom : 1;
  // Buyutme merkezi = nisan artisi: zoom'da hedef noktasi yerinde kalir.
  const originX = hasGuides ? (g.crossX ?? 0.5) : 0.5;
  const originY = hasGuides ? clamp(0.5 - (g.crossK ?? 0) / (g.dist || 3), 0, 1) : 0.5;
  const zoomOrigin = `${originX * 100}% ${originY * 100}%`;

  const setLzNum = (k, d, lo, hi) => setLz((p) => ({ ...p, [k]: r2(clamp((p[k] ?? 0) + d, lo, hi)) }));
  const resetLaser = () => { try { localStorage.removeItem(LZ_KEY); } catch { /* ignore */ } setLz(cam.laser); setShowLz(false); };
  const onMove = (e) => {
    if (!dragRef.current || !boxRef.current) return;
    const b = boxRef.current.getBoundingClientRect();
    // Zoom acikken imlec BUYUTULMUS kareyi gosterir; kalibrasyon degerleri ise
    // hep ham kare oranidir. scale(z) donusumunun tersini alip geri ceviriyoruz
    // (merkez = zoom origin), boylece yakinlastirilmisken de arti kalibre edilir.
    const z = zoomActive || 1;
    const px = (e.clientX - b.left) / b.width;
    const py = (e.clientY - b.top) / b.height;
    const x = clamp(originX + (px - originX) / z, 0, 1);
    const y = clamp(originY + (py - originY) / z, 0, 1);
    if (dragRef.current === 'crosshair') {
      // Yatay: crossX. Dikey: bu mesafede kalibre → crossK = (0.5 - y) * mesafe.
      setG((prev) => ({ ...prev, crossX: r2(x), crossK: r2((0.5 - y) * (prev.dist || 3)) }));
    } else {
      setG((prev) => ({ ...prev, [dragRef.current]: [r2(x), r2(y)] }));
    }
  };
  const endDrag = () => { dragRef.current = null; };

  return (
    <div ref={boxRef}
      onPointerMove={edit ? onMove : undefined}
      onPointerUp={edit ? endDrag : undefined}
      onPointerLeave={edit ? endDrag : undefined}
      style={{ position: 'relative', flex: `${grow} 1 0`, minWidth: 0, background: 'repeating-linear-gradient(45deg,#232a1b 0,#232a1b 12px,#161b12 12px,#161b12 24px)', border: `1px solid ${C.borderStrong}`, borderRadius: 3, overflow: 'hidden' }}>
      {/* Video, lazer isareti ve kilavuz AYNI katmanda olceklenir — boylece zoom
          hizalamayi bozmaz, ucu birden ayni kirpmayi gorur. */}
      <div style={{
        position: 'absolute', inset: 0,
        transform: zoomActive !== 1 ? `scale(${zoomActive})` : 'none',
        transformOrigin: zoomOrigin,
      }}>
        <video ref={videoRef} autoPlay muted playsInline
          style={{
            width: '100%', height: '100%', objectFit: 'cover',
            display: state === 'live' ? 'block' : 'none',
            filter: laserOn ? `saturate(${lz.saturate}) contrast(${lz.contrast})` : 'none',
          }} />
        {shotOn && (
          <ShotView videoRef={videoRef} active={shotOn} fps={lz.fps ?? 15}
            gain={lz.shotGain ?? 3} dim={lz.shotDim ?? 0.3} color={lz.color} />
        )}
        {laserOn && lz.mark && state === 'live' && (
          <LaserMarker spot={spot} frame={frame} color={lz.color} />
        )}
        {hasGuides && (state === 'live' || edit) && (
          <GuideOverlay g={g} editable={edit} aspect={aspect} zoom={zoomActive}
            onHandleDown={(k, e) => { dragRef.current = k; e.preventDefault(); }} />
        )}
      </div>
      {state !== 'live' && (
        <div style={{ position: 'absolute', inset: 0, display: 'flex', alignItems: 'center', justifyContent: 'center', color: C.textMuted, fontFamily: MONO, fontSize: 9, letterSpacing: '0.15em', fontWeight: 700, textAlign: 'center', padding: 6 }}>
          {state === 'connecting' ? 'CONNECTING…' : 'NO SIGNAL'}
        </div>
      )}
      <div style={{ position: 'absolute', top: 5, left: 6, display: 'flex', alignItems: 'center', gap: 5, fontFamily: MONO, fontSize: 9, color: C.textPrimary, fontWeight: 700, letterSpacing: '0.08em' }}>
        <Dot color={badge} pulse={state === 'connecting'} size={6} />
        {cam.label}
      </div>
      {(hasGuides || hasLaser) && (() => {
        const tab = (active, tint) => ({
          fontSize: 8, fontFamily: MONO, fontWeight: 700, letterSpacing: '0.08em',
          color: active ? C.bgPage : C.textPrimary, background: active ? tint : 'rgba(0,0,0,0.5)',
          border: `1px solid ${tint}`, borderRadius: 3, padding: '2px 5px', cursor: 'pointer', lineHeight: 1.2,
        });
        return (
          <div style={{ position: 'absolute', top: 4, right: 5, display: 'flex', alignItems: 'center', gap: 4, zIndex: 2 }}>
            {hasLaser && (
              <button onClick={() => setLz((p) => ({ ...p, on: !p.on }))} title="lazer görünürlük modu"
                style={tab(laserOn, lz.color)}>LAZER</button>
            )}
            {hasLaser && (
              <button onClick={() => setLz((p) => ({ ...p, shot: !p.shot }))}
                title="atış görüşü: sahne griye/karanlığa düşer, sadece kırmızılık farkı parlar"
                style={tab(!!lz.shot, C.warn)}>ATIŞ</button>
            )}
            {laserOn && (
              <button onClick={() => setShowLz((v) => !v)} title="lazer ayarları"
                style={tab(showLz, lz.color)}>⚙</button>
            )}
            {hasZoom && (
              <span style={{ display: 'flex', alignItems: 'center', gap: 3, background: 'rgba(0,0,0,0.5)', border: `1px solid ${C.borderStrong}`, borderRadius: 3, padding: '1px 3px' }}>
                <button onClick={() => bumpZoom(-(cam.zoom?.step ?? 0.25))} title="uzaklaştır"
                  style={{ ...tab(false, C.borderStrong), border: 'none', background: 'none', padding: '0 3px' }}>−</button>
                <span onClick={() => setZoom(cam.zoom?.min ?? 1)} title="sıfırla (1.0×)"
                  style={{ fontSize: 8, fontFamily: MONO, fontWeight: 700, color: zoom > 1 ? C.accent : C.textPrimary, cursor: 'pointer', minWidth: 22, textAlign: 'center' }}>
                  {zoom.toFixed(1)}×
                </span>
                <button onClick={() => bumpZoom(cam.zoom?.step ?? 0.25)} title="yakınlaştır"
                  style={{ ...tab(false, C.borderStrong), border: 'none', background: 'none', padding: '0 3px' }}>+</button>
              </span>
            )}
            {hasGuides && (
              <button onClick={() => setEdit((v) => !v)}
                style={tab(edit, g.color)}>{edit ? 'BİTİR' : 'AYAR'}</button>
            )}
          </div>
        );
      })()}
      {laserOn && lz.mark && state === 'live' && !showLz && (
        <div style={{ position: 'absolute', bottom: 4, left: 6, fontFamily: MONO, fontSize: 8, fontWeight: 700, letterSpacing: '0.08em', color: spot ? lz.color : C.textMuted, background: 'rgba(0,0,0,0.55)', borderRadius: 3, padding: '2px 4px', pointerEvents: 'none' }}>
          {tc?.laser_on === false && useBg ? 'ARKA PLAN ÖĞRENİLİYOR'
            : spot ? `LAZER ${spot.strength} · ${spot.px}px` : 'LAZER YOK'}
        </div>
      )}
      {hasGuides && edit && (() => {
        const stepBtn = { fontFamily: MONO, fontSize: 10, fontWeight: 700, lineHeight: 1, color: C.textPrimary, background: 'rgba(255,255,255,0.12)', border: `1px solid ${C.borderStrong}`, borderRadius: 2, width: 15, height: 15, cursor: 'pointer', padding: 0 };
        return (
          <div style={{ position: 'absolute', top: 22, left: 4, right: 4, display: 'flex', alignItems: 'center', gap: 4, flexWrap: 'wrap', fontSize: 8, fontFamily: MONO, color: C.textPrimary, background: 'rgba(0,0,0,0.75)', padding: '3px 4px', borderRadius: 3, zIndex: 2 }}>
            <input type="color" value={g.color} onChange={(e) => setG((p) => ({ ...p, color: e.target.value }))} title="renk"
              style={{ width: 18, height: 15, border: 'none', background: 'none', padding: 0, cursor: 'pointer' }} />
            <span>op</span>
            <button style={stepBtn} onClick={() => setOpacity(-0.05)}>−</button>
            <span style={{ minWidth: 20, textAlign: 'center' }}>{g.opacity.toFixed(2)}</span>
            <button style={stepBtn} onClick={() => setOpacity(0.05)}>+</button>
            <span>kal</span>
            <button style={stepBtn} onClick={() => setWidth(-1)}>−</button>
            <span style={{ minWidth: 8, textAlign: 'center' }}>{g.lineWidth}</span>
            <button style={stepBtn} onClick={() => setWidth(1)}>+</button>
            <button onClick={resetGuides} title="başlangıca dön"
              style={{ marginLeft: 'auto', fontFamily: MONO, fontSize: 8, fontWeight: 700, color: C.textPrimary, background: 'rgba(255,255,255,0.12)', border: `1px solid ${C.borderStrong}`, borderRadius: 2, padding: '2px 5px', cursor: 'pointer' }}>Sıfırla</button>
            {g.crossK != null && (
              <div style={{ display: 'flex', alignItems: 'center', gap: 4, width: '100%' }}>
                <span>mesafe</span>
                <input type="range" min="1" max="10" step="0.5" value={g.dist || 3}
                  onChange={(e) => setG((p) => ({ ...p, dist: +e.target.value }))} style={{ flex: 1, minWidth: 60 }} />
                <span style={{ minWidth: 26, textAlign: 'right' }}>{(g.dist || 3)}m</span>
                <button style={stepBtn} onClick={() => setG((p) => ({ ...p, crossSize: r2(clamp((p.crossSize || 0.1) - 0.02, 0.03, 0.4)) }))}>−</button>
                <span title="artı boyutu">✚</span>
                <button style={stepBtn} onClick={() => setG((p) => ({ ...p, crossSize: r2(clamp((p.crossSize || 0.1) + 0.02, 0.03, 0.4)) }))}>+</button>
              </div>
            )}
          </div>
        );
      })()}
      {laserOn && showLz && (() => {
        const stepBtn = { fontFamily: MONO, fontSize: 10, fontWeight: 700, lineHeight: 1, color: C.textPrimary, background: 'rgba(255,255,255,0.12)', border: `1px solid ${C.borderStrong}`, borderRadius: 2, width: 15, height: 15, cursor: 'pointer', padding: 0 };
        const txtBtn = { fontFamily: MONO, fontSize: 8, fontWeight: 700, color: C.textPrimary, background: 'rgba(255,255,255,0.12)', border: `1px solid ${C.borderStrong}`, borderRadius: 2, padding: '2px 5px', cursor: 'pointer' };
        // Etiket + −/+ tek parca kalsin: dar karoda satir ortasindan bolunmesin.
        const ctl = (label, shown, w, key, step, lo, hi, title) => (
          <span key={key} style={{ display: 'flex', alignItems: 'center', gap: 4, whiteSpace: 'nowrap' }}>
            <span title={title}>{label}</span>
            <button style={stepBtn} onClick={() => setLzNum(key, -step, lo, hi)}>−</button>
            <span style={{ minWidth: w, textAlign: 'center' }}>{shown}</span>
            <button style={stepBtn} onClick={() => setLzNum(key, step, lo, hi)}>+</button>
          </span>
        );
        return (
          <div style={{ position: 'absolute', left: 4, right: 4, bottom: 4, display: 'flex', alignItems: 'center', gap: 4, flexWrap: 'wrap', fontSize: 8, fontFamily: MONO, color: C.textPrimary, background: 'rgba(0,0,0,0.78)', padding: '3px 4px', borderRadius: 3, zIndex: 2 }}>
            {ctl('duyar', (lz.sensitivity ?? 0.5).toFixed(2), 20, 'sensitivity', 0.1, 0, 1, 'duyarlılık: 0 = sadece çok belirgin nokta, 1 = soluk noktayı da yakalar (yanılma riski artar). Eşik her karede ışığa göre kendi hesaplanır.')}
            {ctl('doy', lz.saturate.toFixed(1), 18, 'saturate', 0.2, 1, 4, 'doygunluk')}
            {ctl('kon', lz.contrast.toFixed(2), 20, 'contrast', 0.05, 1, 2.5, 'kontrast')}
            <button style={{ ...txtBtn, color: lz.mark ? C.bgPage : C.textPrimary, background: lz.mark ? lz.color : 'rgba(255,255,255,0.12)' }}
              title="noktayı halka ile işaretle (kapatırsan sadece renk açma kalır)"
              onClick={() => setLz((p) => ({ ...p, mark: !p.mark }))}>işaret</button>
            <button style={{ ...txtBtn, marginLeft: 'auto' }} title="başlangıca dön" onClick={resetLaser}>Sıfırla</button>
          </div>
        );
      })()}
    </div>
  );
};

// TARET nisan goruntusu ORTADA durur; uc karo esit genislikte. Taret karosunu
// buyutmek 640x480 akisi gererek bulaniklastirdigi icin esit birakildi —
// yakinlastirmak icin karodaki dijital zoom var.
// Kayitlari USB'ye dokme dugmesi. Jetson'daki dump_service.py'ye POST atar; o da
// scripts/dump_recordings.sh'i calistirir (kopyala -> dogrula -> sync -> unmount).
// Yanlislikla basmayi onlemek icin iki asamali: ilk tik "EMIN?", ikinci tik baslatir.
const UsbDumpControl = () => {
  const { job, error, start } = useUsbDump();
  const [armed, setArmed] = useState(false);
  useEffect(() => {
    if (!armed) return undefined;
    const t = setTimeout(() => setArmed(false), 4000);   // 4 sn sonra kendini kurar
    return () => clearTimeout(t);
  }, [armed]);

  const running = job?.state === 'running';
  const last = job?.lines?.length ? job.lines[job.lines.length - 1] : null;
  const tone = error ? C.textMuted
    : running ? C.warn
    : job?.state === 'done' ? C.ok
    : job?.state === 'failed' ? C.fail
    : C.textPrimary;

  const label = error ? 'USB SERVİSİ YOK'
    : running ? 'AKTARILIYOR…'
    : armed ? 'EMİN?'
    : "USB'YE AT";

  const onClick = () => {
    if (error || running) return;
    if (!armed) { setArmed(true); return; }
    setArmed(false);
    start();
  };

  return (
    <span style={{ display: 'flex', alignItems: 'center', gap: 6, maxWidth: 340 }}>
      {last && (
        <span title={(job?.lines || []).join('\n')}
          style={{ fontFamily: MONO, fontSize: 8, color: tone, fontWeight: 600, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', maxWidth: 200 }}>
          {last}
        </span>
      )}
      <button onClick={onClick} disabled={!!error || running}
        title={error ? `${error} — Jetson'da dump_service.py çalışıyor mu?` : `kayıtları ${job?.dest || 'USB'} diskine kopyala, doğrula ve diski çıkarmaya hazırla`}
        style={{
          fontFamily: MONO, fontSize: 8, fontWeight: 700, letterSpacing: '0.1em',
          color: armed ? C.bgPage : tone,
          background: armed ? C.warn : 'rgba(0,0,0,0.35)',
          border: `1px solid ${armed ? C.warn : tone}`, borderRadius: 3,
          padding: '2px 6px', cursor: error || running ? 'default' : 'pointer',
          opacity: error ? 0.6 : 1,
        }}>
        {label}
      </button>
    </span>
  );
};

const CamerasPanel = ({ telemetry }) => {
  const main = CAMERAS.find((c) => c.key === 'turret') ?? CAMERAS[0];
  const side = CAMERAS.filter((c) => c !== main);
  const ordered = [side[0], main, ...side.slice(1)].filter(Boolean);
  return (
    <Card title="LIVE FEED · CAMERAS" tag={<UsbDumpControl />} accent={C.accent}>
      <div style={{ display: 'flex', gap: 5, flex: 1, minHeight: 0 }}>
        {ordered.map((cam) => <CameraTile key={cam.key} cam={cam} telemetry={telemetry} />)}
      </div>
    </Card>
  );
};

// ─── HEADER ──────────────────────────────────────────────────
const Header = ({ telemetry, conn, wsConnected, clock }) => {
  const s = telemetry?.status;
  let overall;
  if (!wsConnected) overall = { label: 'SERVER OFFLINE', color: C.fail, bg: C.failBg };
  else if (!conn.base_link_up) overall = { label: 'BASE LINK LOST', color: C.fail, bg: C.failBg };
  else if (s?.hw_error) overall = { label: 'HW FAULT', color: C.fail, bg: C.failBg };
  else if (s?.failsafe_active) overall = { label: 'FAILSAFE', color: C.fail, bg: C.failBg };
  else if (!conn.stm_status_fresh) overall = { label: 'TELEMETRY STALE', color: C.warn, bg: C.warnBg };
  else if (s && !s.elrs_link_up) overall = { label: 'RC LINK LOST', color: C.warn, bg: C.warnBg };
  else overall = { label: 'NOMINAL', color: C.ok, bg: C.okBg };

  return (
    <header style={{ display: 'flex', alignItems: 'center', gap: 14, padding: '8px 14px', background: C.bgCard, borderBottom: `1px solid ${C.borderStrong}`, flexShrink: 0 }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 10 }}>
        <div style={{ width: 28, height: 28, border: `1.5px solid ${C.accent}`, background: C.accentBg, display: 'flex', alignItems: 'center', justifyContent: 'center', color: C.accent, fontFamily: MONO, fontSize: 13, fontWeight: 700, borderRadius: 4 }}>⌬</div>
        <div>
          <div style={{ fontFamily: MONO, fontSize: 13, fontWeight: 700, color: C.textPrimary, letterSpacing: '0.15em', lineHeight: 1.2 }}>BASE STATION</div>
          <div style={{ fontFamily: MONO, fontSize: 8, color: C.textMuted, letterSpacing: '0.18em', lineHeight: 1.2, fontWeight: 600 }}>TEKNOFEST · İNSANSIZ KARA ARACI · READ-ONLY</div>
        </div>
      </div>
      <div style={{ height: 24, width: 1, background: C.border }} />
      <div style={{ display: 'flex', alignItems: 'center', gap: 8, padding: '4px 10px', background: overall.bg, border: `1px solid ${overall.color}55`, borderRadius: 3 }}>
        <Dot color={overall.color} pulse={overall.label !== 'NOMINAL'} />
        <span style={{ fontFamily: MONO, fontSize: 11, color: overall.color, letterSpacing: '0.14em', fontWeight: 700 }}>MISSION · {overall.label}</span>
      </div>
      <div style={{ marginLeft: 'auto', display: 'flex', alignItems: 'center', gap: 14 }}>
        <span style={{ fontFamily: MONO, fontSize: 10, color: C.textSecondary, letterSpacing: '0.1em', fontWeight: 600 }}>UGV-01 · TELEM v1</span>
        <span style={{ fontFamily: MONO, fontSize: 13, color: C.accent, letterSpacing: '0.06em', fontVariantNumeric: 'tabular-nums', fontWeight: 700 }}>{clock}</span>
      </div>
    </header>
  );
};

// ─── APP ─────────────────────────────────────────────────────
export default function BaseStation() {
  const { telemetry, conn, events, wsConnected } = useTelemetry();
  const [clock, setClock] = useState('');

  useEffect(() => {
    const tick = () => setClock(new Date().toLocaleTimeString('tr-TR', { hour12: false }) + ' TRT');
    tick();
    const id = setInterval(tick, 1000);
    return () => clearInterval(id);
  }, []);

  const stale = !wsConnected || !conn.base_link_up || !conn.stm_status_fresh;

  return (
    <div style={{ height: '100vh', width: '100vw', background: C.bgPage, color: C.textPrimary, fontFamily: 'Inter, system-ui, sans-serif', display: 'flex', flexDirection: 'column', overflow: 'hidden' }}>
      <style>{`
        @import url('https://fonts.googleapis.com/css2?family=JetBrains+Mono:wght@400;500;600;700&family=Inter:wght@400;500;600;700&display=swap');
        * { box-sizing: border-box; }
        @keyframes pulse { 0%,100% { opacity: 1; } 50% { opacity: 0.4; } }
        .grid-main {
          display: grid; gap: 8px; padding: 8px; flex: 1; min-height: 0;
          grid-template-columns: 230px 1fr 300px;
          grid-template-rows: 1.15fr 1fr;
          grid-template-areas: "control camera links" "safety bottom links";
        }
        .area-control { grid-area: control; min-height: 0; }
        .area-safety  { grid-area: safety;  min-height: 0; }
        .area-camera  { grid-area: camera;  min-height: 0; }
        .area-links   { grid-area: links;   min-height: 0; }
        .area-bottom  { grid-area: bottom; min-height: 0; }
        @media (max-width: 1100px) {
          .grid-main { grid-template-columns: 1fr 1fr; grid-template-rows: auto auto auto auto;
            grid-template-areas: "camera camera" "control links" "safety safety" "bottom bottom"; }
        }
      `}</style>

      <Header telemetry={telemetry} conn={conn} wsConnected={wsConnected} clock={clock} />

      <div className="grid-main">
        <div className="area-control"><VehicleControlPanel telemetry={telemetry} stale={stale} /></div>
        <div className="area-camera"><CamerasPanel telemetry={telemetry} /></div>
        <div className="area-safety"><SafetyPanel telemetry={telemetry} stale={stale} /></div>
        <div className="area-links"><LinksPanel telemetry={telemetry} conn={conn} wsConnected={wsConnected} /></div>
        <div className="area-bottom"><EventsPanel events={events} /></div>
      </div>

      <footer style={{ padding: '5px 14px', borderTop: `1px solid ${C.border}`, background: C.bgCard, display: 'flex', justifyContent: 'space-between', fontFamily: MONO, fontSize: 9, color: C.textMuted, letterSpacing: '0.08em', flexShrink: 0, fontWeight: 600 }}>
        <span>BASE STATION · READ-ONLY OBSERVER {stale ? '· DATA STALE' : ''}</span>
        <span>ESP32 → USB-TTL/UART → JETSON → QUIC/PROTOBUF → BASE → WS/WEBRTC</span>
      </footer>
    </div>
  );
}
