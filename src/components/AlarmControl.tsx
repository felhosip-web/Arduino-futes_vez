import { useEffect, useRef, useState } from 'react';
import { AlertTriangle, Bell, BellOff, ShieldCheck } from 'lucide-react';
import type { AlarmConfig, AlarmReason, AlarmState } from '../types/heating';

interface AlarmControlProps {
  alarmConfig: AlarmConfig;
  alarmState: AlarmState;
  kazanTemp: number | null;
  sending: boolean;
  onSendAlarmConfig: (config: AlarmConfig) => Promise<void>;
}

const OVERHEAT_MIN = 70;
const OVERHEAT_MAX = 90;
const OVERHEAT_STEP = 0.5;

function clampOverheatTemp(val: number): number {
  const clamped = Math.min(OVERHEAT_MAX, Math.max(OVERHEAT_MIN, val));
  return Math.round(clamped * 2) / 2;
}

function getAlarmBadgeText(reason: AlarmReason): string {
  switch (reason) {
    case 'overheat':
      return 'RIASZTÁS: TÚLMELEGEDÉS';
    case 'sensor_fail':
      return 'RIASZTÁS: SZENZOR HIBA';
    case 'nrf_fail':
      return 'RIASZTÁS: NRF HIBA';
    default:
      return 'RIASZTÁS';
  }
}

export function AlarmControl({
  alarmConfig,
  alarmState,
  kazanTemp,
  sending,
  onSendAlarmConfig,
}: AlarmControlProps) {
  const [draftConfig, setDraftConfig] = useState<AlarmConfig>(alarmConfig);
  const debounceTimerRef = useRef<number | null>(null);
  const isSlidingRef = useRef<boolean>(false);

  // ESP státusz szinkronizálása (slider húzás alatt nem rántjuk el)
  useEffect(() => {
    if (!isSlidingRef.current) {
      setDraftConfig(alarmConfig);
    }
  }, [alarmConfig.overheatTemp, alarmConfig.alarmEnabled]);

  useEffect(() => {
    return () => {
      if (debounceTimerRef.current !== null) {
        window.clearTimeout(debounceTimerRef.current);
      }
    };
  }, []);

  const clearPendingDebounce = () => {
    if (debounceTimerRef.current !== null) {
      window.clearTimeout(debounceTimerRef.current);
      debounceTimerRef.current = null;
    }
  };

  const commitImmediate = (next: AlarmConfig) => {
    clearPendingDebounce();
    isSlidingRef.current = false;
    setDraftConfig(next);
    void onSendAlarmConfig(next);
  };

  const scheduleSliderCommit = (next: AlarmConfig) => {
    setDraftConfig(next);
    clearPendingDebounce();
    debounceTimerRef.current = window.setTimeout(() => {
      debounceTimerRef.current = null;
      isSlidingRef.current = false;
      void onSendAlarmConfig(next);
    }, 500);
  };

  const isKazanOverheat =
    kazanTemp !== null &&
    kazanTemp !== undefined &&
    !Number.isNaN(kazanTemp) &&
    kazanTemp > -120 &&
    kazanTemp >= alarmConfig.overheatTemp;

  return (
    <section
      aria-labelledby="alarm-control-title"
      className={`rounded-xl border bg-slate-900 p-5 sm:p-6 ${
        isKazanOverheat ? 'border-red-500 animate-pulse' : 'border-slate-800'
      }`}
    >
      {/* Felső sor: cím + állapot badge */}
      <div className="flex flex-wrap items-center justify-between gap-3 border-b border-slate-800/80 pb-4">
        <h2 id="alarm-control-title" className="text-lg font-semibold text-slate-100">
          Túlmelegedés riasztás
        </h2>

        {alarmState.active ? (
          <div
            role="status"
            className="inline-flex items-center gap-2 rounded-lg border border-red-500 bg-red-950/80 px-3 py-1.5 text-xs font-semibold text-red-100 whitespace-nowrap shrink-0 animate-pulse"
          >
            <AlertTriangle className="h-3.5 w-3.5 text-red-400" aria-hidden="true" />
            <span>{getAlarmBadgeText(alarmState.reason)}</span>
          </div>
        ) : (
          <div className="inline-flex items-center gap-2 rounded-lg border border-slate-700 bg-slate-800 px-3 py-1.5 text-xs font-semibold text-slate-300 whitespace-nowrap shrink-0">
            <ShieldCheck className="h-3.5 w-3.5 text-slate-400" aria-hidden="true" />
            <span>Nincs riasztás</span>
          </div>
        )}
      </div>

      {/* Magyarázó szöveg */}
      <p className="mt-4 text-sm leading-relaxed text-slate-400">
        Ha a kazán eléri a beállított hőmérsékletet, a piros LED villog a kazánnál (Nano) és a
        szobában (ESP32) is, és a szivattyú kényszerítve BEkapcsol.
      </p>

      {/* Figyelmeztetés kikapcsolt riasztásnál */}
      {!draftConfig.alarmEnabled && (
        <div
          role="alert"
          className="mt-4 flex items-center gap-2.5 rounded-lg border border-amber-500/50 bg-amber-950/40 px-4 py-3 text-sm font-medium text-amber-200"
        >
          <BellOff className="h-4 w-4 shrink-0 text-amber-400" aria-hidden="true" />
          <span>Riasztás kikapcsolva - a villogó LED nem fog jelezni!</span>
        </div>
      )}

      {/* Engedélyezés toggle + Hőmérséklet slider */}
      <div className="mt-5 grid grid-cols-1 gap-6 border-t border-slate-800/80 pt-5 md:grid-cols-2 md:items-center">
        <div>
          <div className="flex items-baseline justify-between gap-3">
            <label
              htmlFor="alarm-overheat-slider"
              className={`text-sm font-medium ${
                draftConfig.alarmEnabled ? 'text-slate-300' : 'text-slate-500'
              }`}
            >
              Riasztási hőmérséklet
            </label>
            <span
              className={`tabular-nums text-2xl font-bold ${
                draftConfig.alarmEnabled ? 'text-red-400' : 'text-slate-500'
              }`}
            >
              {draftConfig.overheatTemp.toFixed(1)}°C
            </span>
          </div>

          <input
            id="alarm-overheat-slider"
            type="range"
            min={OVERHEAT_MIN}
            max={OVERHEAT_MAX}
            step={OVERHEAT_STEP}
            disabled={!draftConfig.alarmEnabled}
            value={draftConfig.overheatTemp}
            onPointerDown={() => {
              isSlidingRef.current = true;
            }}
            onPointerUp={() => {
              isSlidingRef.current = false;
            }}
            onTouchStart={() => {
              isSlidingRef.current = true;
            }}
            onTouchEnd={() => {
              isSlidingRef.current = false;
            }}
            onChange={(e) =>
              scheduleSliderCommit({
                ...draftConfig,
                overheatTemp: clampOverheatTemp(parseFloat(e.target.value)),
              })
            }
            className={`mt-3 w-full ${
              draftConfig.alarmEnabled ? 'cursor-pointer' : 'cursor-not-allowed opacity-45'
            }`}
          />
          <div className="mt-1.5 flex justify-between text-xs tabular-nums text-slate-500">
            <span>70.0°C</span>
            <span>80.0°C</span>
            <span>90.0°C</span>
          </div>
        </div>

        <div className="flex items-center justify-between gap-4 rounded-lg border border-slate-800 bg-slate-950/70 px-4 py-3.5">
          <div className="min-w-0">
            <div className="flex items-center gap-2 text-sm font-medium text-slate-200">
              {draftConfig.alarmEnabled ? (
                <Bell className="h-4 w-4 shrink-0 text-red-400" aria-hidden="true" />
              ) : (
                <BellOff className="h-4 w-4 shrink-0 text-slate-500" aria-hidden="true" />
              )}
              <span>Riasztás engedélyezve</span>
            </div>
            <div className="mt-0.5 text-xs text-slate-400">
              {draftConfig.alarmEnabled ? 'Bekapcsolva (villogó LED aktív)' : 'Kikapcsolva'}
            </div>
          </div>

          <button
            type="button"
            role="switch"
            aria-checked={draftConfig.alarmEnabled}
            aria-label="Riasztás engedélyezve"
            disabled={sending}
            onClick={() =>
              commitImmediate({
                ...draftConfig,
                alarmEnabled: !draftConfig.alarmEnabled,
              })
            }
            className={`inline-flex min-h-[44px] items-center gap-2.5 rounded-lg border px-3.5 py-2 text-xs font-semibold whitespace-nowrap shrink-0 transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-orange-500 disabled:opacity-60 ${
              draftConfig.alarmEnabled
                ? 'border-red-500/40 bg-red-500/15 text-red-200 hover:bg-red-500/25'
                : 'border-slate-700 bg-slate-800 text-slate-300 hover:bg-slate-700'
            }`}
          >
            <span
              aria-hidden="true"
              className={`relative inline-flex h-5 w-9 shrink-0 items-center rounded-full transition-colors ${
                draftConfig.alarmEnabled ? 'bg-red-500' : 'bg-slate-600'
              }`}
            >
              <span
                className={`inline-block h-3.5 w-3.5 transform rounded-full bg-white transition-transform ${
                  draftConfig.alarmEnabled ? 'translate-x-4.5' : 'translate-x-1'
                }`}
              />
            </span>
            <span>{draftConfig.alarmEnabled ? 'BE' : 'KI'}</span>
          </button>
        </div>
      </div>
    </section>
  );
}
