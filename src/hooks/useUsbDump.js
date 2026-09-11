import { useCallback, useEffect, useRef, useState } from 'react';
import { DUMP_BASE } from '../config';

/**
 * Jetson'daki USB dokum servisine (jetson/dump_service.py) bakan kucuk istemci.
 * Servis TEK isi yapar: kayitlari takili USB'ye kopyalayip diski unmount eder.
 * Buradan komut GITMEZ — sadece "basla" ve "durum" sorulur.
 *
 * Doner: { job, error, start } — job.state: idle | running | done | failed.
 */
const POLL_RUNNING_MS = 1500;
const POLL_IDLE_MS = 10000;

export function useUsbDump() {
  const [job, setJob] = useState(null);      // servis yanitı; null = ulasilamiyor
  const [error, setError] = useState(null);
  const jobRef = useRef(null);

  const poll = useCallback(async () => {
    try {
      const res = await fetch(`${DUMP_BASE}/status`);
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const j = await res.json();
      jobRef.current = j;
      setJob(j);
      setError(null);
    } catch {
      jobRef.current = null;
      setJob(null);
      setError('servise ulaşılamıyor');
    }
  }, []);

  useEffect(() => {
    let timer = 0;
    let stopped = false;
    const loop = async () => {
      if (stopped) return;
      await poll();
      if (stopped) return;
      const gap = jobRef.current?.state === 'running' ? POLL_RUNNING_MS : POLL_IDLE_MS;
      timer = setTimeout(loop, gap);
    };
    loop();
    return () => { stopped = true; clearTimeout(timer); };
  }, [poll]);

  const start = useCallback(async () => {
    setError(null);
    try {
      const res = await fetch(`${DUMP_BASE}/dump`, { method: 'POST' });
      const body = await res.json().catch(() => ({}));
      if (!res.ok) { setError(body.error || `HTTP ${res.status}`); return false; }
      const optimistic = { state: 'running', lines: [], rc: null, started_ms: Date.now() };
      jobRef.current = optimistic;
      setJob(optimistic);
      poll();                                  // gercek durumu hemen tazele
      return true;
    } catch {
      setError('servise ulaşılamıyor');
      return false;
    }
  }, [poll]);

  return { job, error, start };
}
