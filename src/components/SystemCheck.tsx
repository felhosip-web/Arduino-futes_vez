import { useEffect, useRef, useState } from 'react';
import {
  Activity,
  AlertTriangle,
  CheckCircle2,
  Clock,
  Cpu,
  Gauge,
  Radio,
  RefreshCw,
  Wrench,
} from 'lucide-react';
import { API_URL, DEVICE_ID } from '../config';
import type { HeatingStatusPayload } from '../types/heating';

interface SystemCheckProps {
  status: HeatingStatusPayload | null;
  lastUpdatedAt: number | null;
  onSyncTriggered?: () => void;
}

function formatUpdatedAt(ms: number | null): string {
  if (!ms) return 'Nincs adat';
  return new Date(ms).toLocaleTimeString('hu-HU', {
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
  });
}

export function SystemCheck({ status, lastUpdatedAt, onSyncTriggered }: SystemCheckProps) {
  const [syncing, setSyncing] = useState<boolean>(false);
  const [syncFeedback, setSyncFeedback] = useState<string | null>(null);
  const [syncError, setSyncError] = useState<string | null>(null);
  const feedbackTimerRef = useRef<number | null>(null);

  useEffect(() => {
    return () => {
      if (feedbackTimerRef.current !== null) {
        window.clearTimeout(feedbackTimerRef.current);
      }
    };
  }, []);

  const sync = status?.sync ?? null;
  const espTimeInfo = status?.time ?? null;

  const ntpOk = Boolean(sync?.ntpSynced ?? espTimeInfo?.espNtpSynced);
  const espTimeDisplay = sync?.espTime ?? espTimeInfo?.espTime ?? 'Nincs adat';
  const nanoSynced = Boolean(sync?.nanoSynced);
  const nanoStale = Boolean(sync?.nanoStale);
  const nanoOk = nanoSynced && !nanoStale && (sync?.syncSuccess ?? true);
  const radioOk = Boolean(status?.nrf24_ok);
  const overflowDays =
    typeof sync?.millisOverflowInDays === 'number' ? sync.millisOverflowInDays : null;
  const overflowWarning = overflowDays !== null && overflowDays <= 7;

  const serviceModeActive = Boolean(status?.serviceMode);
  const espLoopFault = Boolean(status?.loopFault);
  const nanoLoopFault = Boolean(status?.nanoLoopFault);
  const anyLoopFault = espLoopFault || nanoLoopFault;
  const loopMaxMs =
    typeof status?.loopMaxMs === 'number' && Number.isFinite(status.loopMaxMs)
      ? status.loopMaxMs
      : null;
  const loopLastMs =
    typeof status?.loopLastMs === 'number' && Number.isFinite(status.loopLastMs)
      ? status.loopLastMs
      : null;

  const hasSyncData = Boolean(status && sync);
  const allOk = hasSyncData && ntpOk && nanoOk && radioOk && !overflowWarning && !anyLoopFault;

  const handleForceSync = async () => {
    setSyncing(true);
    setSyncError(null);
    setSyncFeedback(null);

    try {
      const res = await fetch(`${API_URL}/api/heating/${DEVICE_ID}/sync`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ action: 'forceSync' }),
      });

      if (!res.ok) {
        const body = (await res.json().catch(() => null)) as { error?: string } | null;
        throw new Error(body?.error || `Szinkronizálási hiba (HTTP ${res.status})`);
      }

      setSyncFeedback('Időszinkronizálási parancs elküldve az ESP32-nek.');
      onSyncTriggered?.();
      if (feedbackTimerRef.current !== null) {
        window.clearTimeout(feedbackTimerRef.current);
      }
      feedbackTimerRef.current = window.setTimeout(() => {
        feedbackTimerRef.current = null;
        setSyncFeedback(null);
      }, 3000);
    } catch (err) {
      const message =
        err instanceof Error ? err.message : 'Nem sikerült elindítani a szinkronizálást.';
      setSyncError(message);
    } finally {
      setSyncing(false);
    }
  };

  return (
    <section
      aria-labelledby="system-check-title"
      className="rounded-xl border border-slate-800 bg-slate-900 p-5 sm:p-6"
    >
      {/* Fejléc */}
      <div className="flex flex-wrap items-center justify-between gap-4 border-b border-slate-800/80 pb-4">
        <div className="flex items-start gap-3">
          <Activity className="mt-0.5 h-5 w-5 shrink-0 text-orange-400" aria-hidden="true" />
          <div>
            <div className="flex flex-wrap items-center gap-2.5">
              <h2 id="system-check-title" className="text-lg font-semibold text-slate-100">
                Rendszer ellenőrzés
              </h2>
              {!hasSyncData ? (
                <span className="inline-flex items-center rounded border border-slate-700 bg-slate-800 px-2 py-0.5 text-xs font-semibold text-slate-300 whitespace-nowrap">
                  VÁRAKOZÁS
                </span>
              ) : allOk ? (
                <span className="inline-flex items-center gap-1.5 rounded border border-emerald-500/50 bg-emerald-950/60 px-2.5 py-0.5 text-xs font-semibold text-emerald-300 whitespace-nowrap">
                  <CheckCircle2 className="h-3.5 w-3.5" aria-hidden="true" />
                  <span>MINDEN OK</span>
                </span>
              ) : (
                <span className="inline-flex items-center gap-1.5 rounded border border-red-500 bg-red-950/80 px-2.5 py-0.5 text-xs font-bold text-red-200 whitespace-nowrap animate-pulse">
                  <AlertTriangle className="h-3.5 w-3.5" aria-hidden="true" />
                  <span>HIBA - lásd lent</span>
                </span>
              )}
            </div>
            <p className="mt-0.5 text-xs text-slate-400 tabular-nums">
              Automatikus frissítés 30 mp-enként · Utolsó állapot: {formatUpdatedAt(lastUpdatedAt)}
            </p>
          </div>
        </div>

        <button
          type="button"
          onClick={() => void handleForceSync()}
          disabled={syncing}
          className="inline-flex min-h-[44px] items-center gap-2 rounded-lg bg-orange-500 px-4 py-2 text-xs font-semibold text-slate-950 whitespace-nowrap shrink-0 transition-colors hover:bg-orange-400 disabled:opacity-60 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-orange-500 sm:text-sm"
        >
          <RefreshCw
            className={`h-4 w-4 ${syncing ? 'animate-spin' : ''}`}
            aria-hidden="true"
          />
          <span>{syncing ? 'Küldés...' : 'Szinkronizálás most'}</span>
        </button>
      </div>

      {syncFeedback && (
        <div
          role="status"
          className="mt-4 rounded-lg border border-emerald-500/50 bg-emerald-950/50 px-4 py-2.5 text-xs font-medium text-emerald-200 sm:text-sm"
        >
          {syncFeedback}
        </div>
      )}

      {syncError && (
        <div
          role="alert"
          className="mt-4 flex items-center gap-2.5 rounded-lg border border-red-500/50 bg-red-950/60 px-4 py-2.5 text-xs text-red-200 sm:text-sm"
        >
          <AlertTriangle className="h-4 w-4 shrink-0 text-red-400" aria-hidden="true" />
          <span>{syncError}</span>
        </div>
      )}

      {/* 4 ellenőrző sor */}
      {!hasSyncData ? (
        <div className="mt-4 rounded-lg border border-slate-800/80 bg-slate-950/60 px-5 py-6 text-center text-sm text-slate-400">
          Várakozás az ESP32-C3 szinkronizációs adataira (futes/{DEVICE_ID}/status)...
        </div>
      ) : (
        <div className="mt-4 grid gap-3 sm:grid-cols-2">
          {/* 1. ESP NTP */}
          <div className="rounded-lg border border-slate-800/90 bg-slate-950/60 p-4">
            <div className="flex items-center justify-between gap-2">
              <span className="inline-flex items-center gap-2 text-xs font-semibold uppercase tracking-wide text-slate-400">
                <Clock className="h-4 w-4 text-slate-400" aria-hidden="true" />
                ESP NTP
              </span>
              <span
                className={`rounded border px-2 py-0.5 text-[11px] font-semibold whitespace-nowrap ${
                  ntpOk
                    ? 'border-emerald-500/40 bg-emerald-950/60 text-emerald-300'
                    : 'border-red-500/60 bg-red-950/70 text-red-300'
                }`}
              >
                {ntpOk ? 'SZINKRONBAN' : 'NINCS NTP'}
              </span>
            </div>
            <p className="mt-2 font-mono text-sm font-medium text-slate-100 tabular-nums">
              {espTimeDisplay}
            </p>
            <p className="mt-1 text-xs text-slate-400 tabular-nums">
              {espTimeInfo ? `Üzemidő: ${espTimeInfo.espUptimeSec} mp` : 'Europe/Budapest (NTP)'}
            </p>
          </div>

          {/* 2. Nano szinkron */}
          <div className="rounded-lg border border-slate-800/90 bg-slate-950/60 p-4">
            <div className="flex items-center justify-between gap-2">
              <span className="inline-flex items-center gap-2 text-xs font-semibold uppercase tracking-wide text-slate-400">
                <Cpu className="h-4 w-4 text-slate-400" aria-hidden="true" />
                Nano szinkron
              </span>
              <span
                className={`rounded border px-2 py-0.5 text-[11px] font-semibold whitespace-nowrap ${
                  nanoOk
                    ? 'border-emerald-500/40 bg-emerald-950/60 text-emerald-300'
                    : nanoStale
                      ? 'border-orange-500/60 bg-orange-950/60 text-orange-300'
                      : 'border-red-500/60 bg-red-950/70 text-red-300'
                }`}
              >
                {nanoOk ? 'OK' : nanoStale ? 'ELAVULT (>13h)' : 'NINCS'}
              </span>
            </div>
            <p className="mt-2 font-mono text-sm font-medium text-slate-100 tabular-nums">
              {sync?.nanoTime ?? 'Nincs szinkronizálva'}
            </p>
            <div className="mt-1 space-y-0.5 text-xs text-slate-400 tabular-nums">
              <p>Utolsó: {sync?.lastSync ?? '—'}</p>
              <p>Következő: {sync?.nextSync ?? '—'}</p>
            </div>
          </div>

          {/* 3. Rádió */}
          <div className="rounded-lg border border-slate-800/90 bg-slate-950/60 p-4">
            <div className="flex items-center justify-between gap-2">
              <span className="inline-flex items-center gap-2 text-xs font-semibold uppercase tracking-wide text-slate-400">
                <Radio className="h-4 w-4 text-slate-400" aria-hidden="true" />
                Rádió (NRF24)
              </span>
              <span
                className={`rounded border px-2 py-0.5 text-[11px] font-semibold whitespace-nowrap ${
                  radioOk
                    ? 'border-emerald-500/40 bg-emerald-950/60 text-emerald-300'
                    : 'border-red-500/60 bg-red-950/70 text-red-300'
                }`}
              >
                {radioOk ? 'OK' : 'HIBA'}
              </span>
            </div>
            <p className="mt-2 font-mono text-sm font-medium text-slate-100 tabular-nums">
              {sync?.pingAgeSec !== null && sync?.pingAgeSec !== undefined
                ? `Utolsó ping: ${sync.pingAgeSec} mp`
                : 'Nincs ping adat'}
            </p>
            <p className="mt-1 text-xs text-slate-400">
              {radioOk ? 'Master–Slave kapcsolat aktív' : '3 ping kimaradt vagy NRF hiba'}
            </p>
          </div>

          {/* 4. Millis túlcsordulás */}
          <div className="rounded-lg border border-slate-800/90 bg-slate-950/60 p-4">
            <div className="flex items-center justify-between gap-2">
              <span className="inline-flex items-center gap-2 text-xs font-semibold uppercase tracking-wide text-slate-400">
                <Activity className="h-4 w-4 text-slate-400" aria-hidden="true" />
                Millis túlcsordulás
              </span>
              <span
                className={`rounded border px-2 py-0.5 text-[11px] font-semibold whitespace-nowrap ${
                  overflowWarning
                    ? 'border-orange-500/60 bg-orange-950/60 text-orange-300'
                    : 'border-emerald-500/40 bg-emerald-950/60 text-emerald-300'
                }`}
              >
                {overflowWarning ? 'HAMAROSAN' : 'BIZTONSÁGOS'}
              </span>
            </div>
            <p
              className={`mt-2 font-mono text-sm font-semibold tabular-nums ${
                overflowWarning ? 'text-orange-300' : 'text-emerald-300'
              }`}
            >
              {overflowDays !== null ? `${overflowDays} nap múlva` : '—'}
            </p>
            <p className="mt-1 text-xs text-slate-400">
              32-bit uint32_t kivonás (49 napos körbefordulás védett)
            </p>
          </div>

          {/* 5. Szerviz kapcsoló (Nano D6) */}
          <div className="rounded-lg border border-slate-800/90 bg-slate-950/60 p-4">
            <div className="flex items-center justify-between gap-2">
              <span className="inline-flex items-center gap-2 text-xs font-semibold uppercase tracking-wide text-slate-400">
                <Wrench className="h-4 w-4 text-slate-400" aria-hidden="true" />
                Szerviz kapcsoló (D6)
              </span>
              <span
                className={`rounded border px-2 py-0.5 text-[11px] font-semibold whitespace-nowrap ${
                  serviceModeActive
                    ? 'border-amber-500/60 bg-amber-950/70 text-amber-300'
                    : 'border-emerald-500/40 bg-emerald-950/60 text-emerald-300'
                }`}
              >
                {serviceModeActive ? 'BE (SLAVE)' : 'KI'}
              </span>
            </div>
            <p className="mt-2 font-mono text-sm font-medium text-slate-100">
              {serviceModeActive ? 'Kézi kazánoldali vezérlés' : 'Normál automata vezérlés'}
            </p>
            <p className="mt-1 text-xs text-slate-400">
              {serviceModeActive
                ? 'A szivattyú D6-ról kényszerítve BE van kapcsolva'
                : 'ESP32 / webes vezérlés aktív'}
            </p>
          </div>

          {/* 6. Loop monitor */}
          <div className="rounded-lg border border-slate-800/90 bg-slate-950/60 p-4">
            <div className="flex items-center justify-between gap-2">
              <span className="inline-flex items-center gap-2 text-xs font-semibold uppercase tracking-wide text-slate-400">
                <Gauge className="h-4 w-4 text-slate-400" aria-hidden="true" />
                Loop monitor
              </span>
              <span
                className={`rounded border px-2 py-0.5 text-[11px] font-semibold whitespace-nowrap ${
                  anyLoopFault
                    ? 'border-red-500 bg-red-950/80 text-red-200 animate-pulse'
                    : 'border-emerald-500/40 bg-emerald-950/60 text-emerald-300'
                }`}
              >
                {anyLoopFault ? `LASSÚ (max ${loopMaxMs ?? '?'}ms)` : 'Loop: OK'}
              </span>
            </div>
            <p
              className={`mt-2 font-mono text-sm font-semibold tabular-nums ${
                anyLoopFault ? 'text-red-300' : 'text-slate-100'
              }`}
            >
              {anyLoopFault
                ? `${espLoopFault ? 'ESP32 ' : ''}${nanoLoopFault ? 'Nano ' : ''}lassulás észlelve`
                : loopLastMs !== null
                  ? `Utolsó: ${loopLastMs} ms`
                  : 'Nincs mérés'}
            </p>
            <p className="mt-1 text-xs text-slate-400 tabular-nums">
              {loopMaxMs !== null ? `ESP max: ${loopMaxMs} ms` : 'ESP max: —'} · Nano:{' '}
              {nanoLoopFault ? 'LASSÚ' : 'OK'}
            </p>
          </div>
        </div>
      )}
    </section>
  );
}
