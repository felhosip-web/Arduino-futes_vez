import { useEffect, useState } from 'react';
import { AlertTriangle, Radio } from 'lucide-react';
import { HEARTBEAT_INTERVALS } from '../config';
import type { CommandAckState, HeartbeatConfig } from '../types/heating';
import { CommandAckBanner } from './CommandAckBanner';

interface HeartbeatControlProps {
  heartbeat: HeartbeatConfig;
  nrf24Ok: boolean;
  lastUpdatedAt: number | null;
  counterValue?: number;
  sending: boolean;
  ackState?: CommandAckState;
  onSendHeartbeat: (config: HeartbeatConfig) => Promise<void>;
}

const STALE_COUNTER_THRESHOLD_MS = 10_000;

export function HeartbeatControl({
  heartbeat,
  nrf24Ok,
  lastUpdatedAt,
  counterValue,
  sending,
  ackState,
  onSendHeartbeat,
}: HeartbeatControlProps) {
  const [draftInterval, setDraftInterval] = useState<number>(heartbeat.intervalSec);
  const [nowMs, setNowMs] = useState<number>(() => Date.now());
  const [counterTracker, setCounterTracker] = useState<{
    value: number | undefined;
    changedAt: number | null;
  }>(() => ({
    value: counterValue,
    changedAt: counterValue !== undefined ? Date.now() : null,
  }));

  // ESP státuszból érkező intervallum szinkronizálása
  useEffect(() => {
    setDraftInterval(heartbeat.intervalSec);
  }, [heartbeat.intervalSec]);

  // 1 másodperces élő számláló frissítés az utolsó válasz és a csomagszámláló figyeléséhez
  useEffect(() => {
    const timer = window.setInterval(() => {
      setNowMs(Date.now());
    }, 1000);
    return () => {
      window.clearInterval(timer);
    };
  }, []);

  // Csomagszámláló változásának figyelése
  useEffect(() => {
    if (counterValue === undefined) {
      return;
    }
    const now = Date.now();
    setCounterTracker((prev) => {
      if (prev.value === counterValue) {
        return prev;
      }
      return { value: counterValue, changedAt: now };
    });
  }, [counterValue]);

  const elapsedSec =
    lastUpdatedAt !== null ? Math.max(0, Math.floor((nowMs - lastUpdatedAt) / 1000)) : 0;

  const hasCounter = typeof counterValue === 'number' && Number.isFinite(counterValue);
  const isCounterStale =
    hasCounter &&
    counterTracker.changedAt !== null &&
    nowMs - counterTracker.changedAt >= STALE_COUNTER_THRESHOLD_MS;

  const handleSelectInterval = (sec: number) => {
    setDraftInterval(sec);
    void onSendHeartbeat({ intervalSec: sec });
  };

  return (
    <section
      aria-labelledby="heartbeat-control-title"
      className={`rounded-xl border bg-slate-900 p-5 sm:p-6 ${
        !nrf24Ok ? 'border-red-500 animate-pulse' : 'border-slate-800'
      }`}
    >
      {/* Felső sor: cím + kapcsolat állapot */}
      <div className="flex flex-wrap items-center justify-between gap-3 border-b border-slate-800/80 pb-4">
        <div className="flex items-center gap-2.5">
          <Radio
            className={`h-5 w-5 shrink-0 ${nrf24Ok ? 'text-emerald-400' : 'text-red-400'}`}
            aria-hidden="true"
          />
          <h2 id="heartbeat-control-title" className="text-lg font-semibold text-slate-100">
            Rádió kapcsolat (NRF24) - Életjel
          </h2>
        </div>

        {nrf24Ok ? (
          <div
            role="status"
            className="inline-flex items-center gap-2 text-xs font-semibold text-emerald-300 whitespace-nowrap shrink-0"
          >
            <span
              className="inline-block h-2.5 w-2.5 rounded-full bg-emerald-500"
              aria-hidden="true"
            />
            <span className="tabular-nums">Kapcsolat OK - Utolsó válasz: {elapsedSec} mp-e</span>
          </div>
        ) : (
          <div
            role="alert"
            className="inline-flex items-center gap-2 rounded-lg border border-red-500 bg-red-950/80 px-3 py-1.5 text-xs font-semibold text-red-100 animate-pulse"
          >
            <AlertTriangle className="h-3.5 w-3.5 shrink-0 text-red-400" aria-hidden="true" />
            <span>NINCS KAPCSOLAT - A kazán fail-safe-ben van (szivattyú BE)</span>
          </div>
        )}
      </div>

      {/* Magyarázó szöveg */}
      <p className="mt-4 text-sm leading-relaxed text-slate-400">
        A szobai egység (master) ennyi másodpercenként kérdezi le a kazánt (slave). A slave csak
        válaszol, önállóan nem ad. Ha 3 egymás utáni lekérdezésre nincs válasz, a kazán automatikusan
        bekapcsolja a szivattyút.
      </p>

      {/* Ping intervallum választó (6 gomb egy sorban) */}
      <div className="mt-5 border-t border-slate-800/80 pt-5">
        <div className="mb-3 flex flex-wrap items-center justify-between gap-2">
          <span className="text-sm font-medium text-slate-300">Ping intervallum</span>
          <span className="tabular-nums text-xs text-slate-400">
            Aktív periódus: {draftInterval} mp
          </span>
        </div>

        <div className="grid grid-cols-6 gap-2" role="group" aria-label="Ping intervallum választó">
          {HEARTBEAT_INTERVALS.map((sec) => {
            const isSelected = draftInterval === sec;
            return (
              <button
                key={sec}
                type="button"
                disabled={sending}
                aria-pressed={isSelected}
                onClick={() => handleSelectInterval(sec)}
                className={`min-h-[44px] rounded-lg border px-2 py-2 text-xs font-semibold tabular-nums whitespace-nowrap shrink-0 transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-orange-500 disabled:opacity-60 sm:text-sm ${
                  isSelected
                    ? 'border-orange-500 bg-orange-500 text-slate-950'
                    : 'border-slate-700 bg-slate-800 text-slate-300 hover:bg-slate-700 hover:text-slate-100'
                }`}
              >
                {sec}s
              </button>
            );
          })}
        </div>
      </div>

      {/* 30 mp-es figyelmeztetés */}
      {draftInterval === 30 && (
        <div
          role="alert"
          className="mt-4 flex items-center gap-2.5 rounded-lg border border-amber-500/50 bg-amber-950/40 px-4 py-3 text-sm font-medium text-amber-200"
        >
          <AlertTriangle className="h-4 w-4 shrink-0 text-amber-400" aria-hidden="true" />
          <span>Figyelem: 30 mp-es pingnél a fail-safe csak 92 mp után lép életbe (3*30+2).</span>
        </div>
      )}

      {/* Utolsó csomagszám (ha jön a STATUS-ban) + 10 mp-es elakadás figyelmeztetés */}
      {hasCounter && (
        <div className="mt-4 flex flex-wrap items-center justify-between gap-3 border-t border-slate-800/80 pt-4">
          <div className="font-mono text-sm tabular-nums text-slate-300">
            Utolsó csomagszám: <span className="font-semibold text-slate-100">#{counterValue}</span>
          </div>

          {isCounterStale && (
            <div
              role="alert"
              className="flex items-center gap-2 rounded-lg border border-amber-500/50 bg-amber-950/50 px-3 py-1.5 text-xs font-semibold text-amber-200"
            >
              <AlertTriangle className="h-3.5 w-3.5 shrink-0 text-amber-400" aria-hidden="true" />
              <span>A számláló nem forog - ellenőrizd a rádiót!</span>
            </div>
          )}
        </div>
      )}

      <CommandAckBanner ackState={ackState} />
    </section>
  );
}
