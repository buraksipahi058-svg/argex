import { useState, useEffect } from 'react';
import { useTelemetry } from './hooks/useTelemetry';
import { useWhepStream } from './hooks/useWhepStream';
import { CAMERAS } from './config';

/* ============================================================
   TEKNOFEST İNSANSIZ KARA ARACI — BASE STATION (READ-ONLY)
   Shows ONLY fields that exist in the STM32<->Jetson protocol.
   Live data via backend WebSocket; video via WebRTC (WHEP).
   ============================================================ */

// ─── PALETTE (light theme, unchanged design language) ───────
const C = {
  bgPage: '#f1f5f9', bgCard: '#ffffff', bgInner: '#f8fafc',
  border: '#cbd5e1', borderStrong: '#94a3b8',
  textPrimary: '#0f172a', textSecondary: '#475569', textMuted: '#64748b', textLabel: '#334155',
  ok: '#059669', okBg: '#d1fae5',
  warn: '#b45309', warnBg: '#fef3c7',
  fail: '#b91c1c', failBg: '#fee2e2',
  accent: '#0369a1', accentBg: '#e0f2fe',
  modeAuto: '#0369a1', modeManual: '#6d28d9', modeLaser: '#b45309',
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
    boxShadow: '0 1px 2px rgba(15,23,42,0.04)', opacity: dim ? 0.55 : 1,
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
  const modeColor = c?.mode === 'LASER' ? C.modeLaser : c?.mode === 'DRIVE' ? C.accent : C.textMuted;
  const opAuto = !!s?.autonomous_active;
  return (
    <Card title="VEHICLE CONTROL" tag="STATUS" accent={C.accent} dim={stale}>
      <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 6, padding: '5px 7px', background: C.bgInner, border: `1px solid ${C.border}`, borderRadius: 3 }}>
        <div>
          <div style={{ fontSize: 8, color: C.textMuted, letterSpacing: '0.12em', fontWeight: 700 }}>MODE</div>
          <div style={{ fontSize: 13, fontWeight: 700, color: modeColor, fontFamily: MONO }}>{c?.mode ?? '—'}</div>
        </div>
        <div>
          <div style={{ fontSize: 8, color: C.textMuted, letterSpacing: '0.12em', fontWeight: 700 }}>OPERATION</div>
          <div style={{ fontSize: 13, fontWeight: 700, color: opAuto ? C.modeAuto : C.modeManual, fontFamily: MONO }}>
            {c ? (opAuto ? 'AUTONOMOUS' : 'MANUAL') : '—'}
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
        <LinkRow label="STM Telemetry" up={conn.stm_status_fresh} meta={l ? `last STATUS ${fmtTs(l.last_status_unix_ms)}` : 'stream'} unknown={!known} />
        {/* RC/ELRS link — vehicle FIELD */}
        <LinkRow label="ELRS (RC) Link" up={!!s?.elrs_link_up} meta="STATUS.elrsLink" unknown={!known} />
        {/* STM's view of STM<->Jetson link — durum bit */}
        <LinkRow label="STM ↔ Jetson (STM view)" up={!!s?.jetson_link_up} meta="durum.JETSON_LINK" unknown={!known} />
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
  return (
    <Card title="SAFETY (REPORTED)" tag="STM" accent={C.fail} dim={stale}>
      {block('FAILSAFE', !!s?.failsafe_active, 'ACTIVE', 'CLEAR')}
      {block('CMD TIMEOUT', !!s?.cmd_timeout, 'TIMEOUT', 'FRESH')}
      {block('CRC ERROR', !!s?.crc_error_recent, 'RECENT', 'CLEAR')}
      <div style={{ marginTop: 'auto', fontSize: 8, color: C.textMuted, fontFamily: MONO, lineHeight: 1.4 }}>
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

const CameraTile = ({ cam }) => {
  const { videoRef, state } = useWhepStream(cam.path);
  const badge = state === 'live' ? C.ok : state === 'connecting' ? C.warn : C.fail;
  return (
    <div style={{ position: 'relative', flex: 1, minWidth: 0, background: 'repeating-linear-gradient(45deg,#1e293b 0,#1e293b 12px,#0f172a 12px,#0f172a 24px)', border: `1px solid ${C.borderStrong}`, borderRadius: 3, overflow: 'hidden' }}>
      <video ref={videoRef} autoPlay muted playsInline style={{ width: '100%', height: '100%', objectFit: 'cover', display: state === 'live' ? 'block' : 'none' }} />
      {state !== 'live' && (
        <div style={{ position: 'absolute', inset: 0, display: 'flex', alignItems: 'center', justifyContent: 'center', color: '#64748b', fontFamily: MONO, fontSize: 9, letterSpacing: '0.15em', fontWeight: 700, textAlign: 'center', padding: 6 }}>
          {state === 'connecting' ? 'CONNECTING…' : 'NO SIGNAL'}
        </div>
      )}
      <div style={{ position: 'absolute', top: 5, left: 6, display: 'flex', alignItems: 'center', gap: 5, fontFamily: MONO, fontSize: 9, color: '#e2e8f0', fontWeight: 700, letterSpacing: '0.08em' }}>
        <Dot color={badge} pulse={state === 'connecting'} size={6} />
        {cam.label}
      </div>
    </div>
  );
};

const CamerasPanel = () => (
  <Card title="LIVE FEED · CAMERAS" tag="RTP→WebRTC" accent={C.accent}>
    <div style={{ display: 'flex', gap: 5, flex: 1, minHeight: 0 }}>
      {CAMERAS.map((cam) => <CameraTile key={cam.key} cam={cam} />)}
    </div>
  </Card>
);

// ─── HEADER ──────────────────────────────────────────────────
const Header = ({ telemetry, conn, wsConnected, clock }) => {
  const s = telemetry?.status;
  let overall;
  if (!wsConnected) overall = { label: 'SERVER OFFLINE', color: C.fail, bg: C.failBg };
  else if (!conn.base_link_up) overall = { label: 'BASE LINK LOST', color: C.fail, bg: C.failBg };
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
          grid-template-rows: 1.5fr 1fr;
          grid-template-areas: "control camera links" "control bottom links";
        }
        .area-control { grid-area: control; min-height: 0; }
        .area-camera  { grid-area: camera;  min-height: 0; }
        .area-links   { grid-area: links;   min-height: 0; }
        .area-bottom  { grid-area: bottom; display: grid; grid-template-columns: 1fr 1.2fr; gap: 8px; min-height: 0; }
        @media (max-width: 1100px) {
          .grid-main { grid-template-columns: 1fr 1fr; grid-template-rows: auto auto auto;
            grid-template-areas: "camera camera" "control links" "bottom bottom"; }
        }
      `}</style>

      <Header telemetry={telemetry} conn={conn} wsConnected={wsConnected} clock={clock} />

      <div className="grid-main">
        <div className="area-control"><VehicleControlPanel telemetry={telemetry} stale={stale} /></div>
        <div className="area-camera"><CamerasPanel /></div>
        <div className="area-links"><LinksPanel telemetry={telemetry} conn={conn} wsConnected={wsConnected} /></div>
        <div className="area-bottom">
          <div><SafetyPanel telemetry={telemetry} stale={stale} /></div>
          <div><EventsPanel events={events} /></div>
        </div>
      </div>

      <footer style={{ padding: '5px 14px', borderTop: `1px solid ${C.border}`, background: C.bgCard, display: 'flex', justifyContent: 'space-between', fontFamily: MONO, fontSize: 9, color: C.textMuted, letterSpacing: '0.08em', flexShrink: 0, fontWeight: 600 }}>
        <span>BASE STATION · READ-ONLY OBSERVER {stale ? '· DATA STALE' : ''}</span>
        <span>STM32 → UART → JETSON → QUIC/PROTOBUF → BASE → WS/WEBRTC</span>
      </footer>
    </div>
  );
}
