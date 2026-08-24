import { useEffect, useRef, useState, useCallback } from 'react';
import { WS_URL } from '../config';

const MAX_EVENTS = 120;

/**
 * Live telemetry over the backend WebSocket. READ-ONLY: this hook only receives.
 *
 * Returns:
 *   telemetry : latest { ts, seq, control, status, link } or null
 *   conn      : { base_link_up, stm_status_fresh }  (backend-derived link state)
 *   events    : most-recent-last array of { ts, type, detail, raw_durum }
 *   wsConnected : is the browser<->backend socket currently open
 *
 * Note the distinct link concepts surfaced to the UI (never conflated):
 *   status.elrs_link_up   – RC/ELRS link, as the STM reports it (field)
 *   status.jetson_link_up – STM's view of the STM<->Jetson link (durum bit)
 *   conn.stm_status_fresh – is the STM STATUS stream currently arriving
 *   conn.base_link_up     – gateway<->Base Station link (backend-derived)
 *   wsConnected           – this browser's socket to the backend
 */
export function useTelemetry() {
  const [telemetry, setTelemetry] = useState(null);
  const [conn, setConn] = useState({ base_link_up: false, stm_status_fresh: false });
  const [events, setEvents] = useState([]);
  const [wsConnected, setWsConnected] = useState(false);
  const wsRef = useRef(null);
  const retryRef = useRef(null);

  const pushEvent = useCallback((e) => {
    setEvents((prev) => {
      const next = [...prev, e];
      return next.length > MAX_EVENTS ? next.slice(next.length - MAX_EVENTS) : next;
    });
  }, []);

  useEffect(() => {
    let closed = false;

    const connect = () => {
      const ws = new WebSocket(WS_URL);
      wsRef.current = ws;

      ws.onopen = () => setWsConnected(true);

      ws.onmessage = (ev) => {
        let msg;
        try { msg = JSON.parse(ev.data); } catch { return; }
        switch (msg.t) {
          case 'snapshot':
            if (msg.telemetry) setTelemetry(msg.telemetry);
            if (msg.conn) setConn(msg.conn);
            if (Array.isArray(msg.events)) setEvents(msg.events.slice(-MAX_EVENTS));
            break;
          case 'telemetry': {
            setTelemetry({
              ts: msg.ts, seq: msg.seq,
              control: msg.control, status: msg.status, link: msg.link,
            });
            if (msg.conn) setConn(msg.conn);
            break;
          }
          case 'event':
            pushEvent({ ts: msg.ts, type: msg.type, detail: msg.detail, raw_durum: msg.raw_durum });
            break;
          case 'conn':
            if (msg.conn) setConn(msg.conn);
            break;
          default:
            break;
        }
      };

      ws.onclose = () => {
        setWsConnected(false);
        // The browser link dropping does NOT mean the vehicle links changed;
        // reflect only that we can no longer observe fresh state.
        setConn((prev) => ({ ...prev, base_link_up: false, stm_status_fresh: false }));
        if (!closed) retryRef.current = setTimeout(connect, 1000);
      };

      ws.onerror = () => ws.close();
    };

    connect();

    return () => {
      closed = true;
      if (retryRef.current) clearTimeout(retryRef.current);
      if (wsRef.current) wsRef.current.close();
    };
  }, [pushEvent]);

  return { telemetry, conn, events, wsConnected };
}
