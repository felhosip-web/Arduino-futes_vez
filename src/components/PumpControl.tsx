import { useEffect, useRef, useState } from 'react';
import { AlertTriangle, Flame, PauseCircle } from 'lucide-react';
import type {
  AlarmConfig,
  PumpConfig,
  PumpMode,
  PumpStartSensor,
  PumpState,
} from '../types/heating';

interface PumpControlProps {
  pumpConfig: PumpConfig;
  pumpState: PumpState;
  alarmConfig: AlarmConfig;
  kazanTemp: number | null;
  sending: boolean;
  onSendPumpConfig: (config: PumpConfig) => Promise<void>;
}

const PUMP_MODE_OPTIONS: ReadonlyArray<{ value: PumpMode; label: string }> = [
  { value: 'auto', label: 'AUTO' },
  { value: 'manual_on', label: 'KÉZI BE' },
  { value: 'manual_off', label: 'KÉZI KI' },
];

const PUMP_SENSOR_OPTIONS: ReadonlyArray<{ value: PumpStartSensor; label: string }> = [
  { value: 'kazan', label: 'Kazán' },
  { value: 'eloremeno', label: 'Előremenő' },
  { value: 'max', label: 'Melegebbik' },
];

function roundHalf(val: number): number {
  return Math.round(val * 2) / 2;
}

function hasHysteresisError(startTemp: number, stopTemp: number): boolean {
  return Math.round(stopTemp * 10) >= Math.round((startTemp - 5) * 10);
}

export function PumpControl({
  pumpConfig,
  pumpState,
  alarmConfig,
  kazanTemp,
  sending,
  onSendPumpConfig,
}: PumpControlProps) {
  const [draftConfig, setDraftConfig] = useState<PumpConfig>(pumpConfig);
  const debounceTimerRef = useRef<number | null>(null);
  const isSlidingRef = useRef<boolean>(false);

  const hysteresisInvalid = hasHysteresisError(
    draftConfig.pumpStartTemp,
    draftConfig.pumpStopTemp
  );

  // ESP státusz szinkronizálása (slider húzás alatt nem rántjuk el)
  useEffect(() => {
    if (!isSlidingRef.current) {
      setDraftConfig(pumpConfig);
    }
  }, [
    pumpConfig.pumpMode,
    pumpConfig.pumpStartTemp,
    pumpConfig.pumpStopTemp,
    pumpConfig.pumpStartSensor,
  ]);

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

  const commitImmediate = (next: PumpConfig) => {
    clearPendingDebounce();
    isSlidingRef.current = false;
    setDraftConfig(next);
    if (!hasHysteresisError(next.pumpStartTemp, next.pumpStopTemp)) {
      void onSendPumpConfig(next);
    }
  };

  const scheduleSliderCommit = (next: PumpConfig) => {
    setDraftConfig(next);
    clearPendingDebounce();
    if (hasHysteresisError(next.pumpStartTemp, next.pumpStopTemp)) {
      return;
    }
    debounceTimerRef.current = window.setTimeout(() => {
      debounceTimerRef.current = null;
      isSlidingRef.current = false;
      void onSendPumpConfig(next);
    }, 500);
  };

  const isOverheat =
    alarmConfig.alarmEnabled &&
    kazanTemp !== null &&
    kazanTemp !== undefined &&
    !Number.isNaN(kazanTemp) &&
    kazanTemp > -120 &&
    kazanTemp >= alarmConfig.overheatTemp;

  const isPumpRunning = pumpState === 'futes';

  return (
    <section
      aria-labelledby="pump-control-title"
      className="rounded-xl border border-slate-800 bg-slate-900 p-5 sm:p-6"
    >
      {/* Felső sor: cím + állapot badge */}
      <div className="flex flex-wrap items-center justify-between gap-3 border-b border-slate-800/80 pb-4">
        <h2 id="pump-control-title" className="text-lg font-semibold text-slate-100">
          Kazánvédelem / Szivattyú
        </h2>

        <div
          className={`inline-flex items-center gap-2 rounded-lg border px-3 py-1.5 text-xs font-semibold whitespace-nowrap shrink-0 ${
            isPumpRunning
              ? 'border-orange-500/40 bg-orange-500/15 text-orange-300'
              : 'border-slate-700 bg-slate-800 text-slate-300'
          }`}
        >
          {isPumpRunning ? (
            <Flame className="h-3.5 w-3.5 text-orange-400" aria-hidden="true" />
          ) : (
            <PauseCircle className="h-3.5 w-3.5 text-slate-400" aria-hidden="true" />
          )}
          <span>
            {isPumpRunning ? 'FŰTÉS - Szivattyú BE' : 'VÁRAKOZÁS - Szivattyú KI'}
          </span>
        </div>
      </div>

      {/* Figyelmeztető sávok */}
      {isOverheat && (
        <div
          role="alert"
          className="mt-4 flex items-center gap-2.5 rounded-lg border border-red-500 bg-red-950/80 px-4 py-3 text-sm font-semibold text-red-100 animate-pulse"
        >
          <AlertTriangle className="h-5 w-5 shrink-0 text-red-400" aria-hidden="true" />
          <span>TÚLMELEGEDÉS! Szivattyú kényszerítve BE</span>
        </div>
      )}

      {draftConfig.pumpMode === 'manual_off' && (
        <div
          role="alert"
          className="mt-4 flex items-center gap-2.5 rounded-lg border border-red-500/60 bg-red-950/60 px-4 py-3 text-sm font-medium text-red-200"
        >
          <AlertTriangle className="h-5 w-5 shrink-0 text-red-400" aria-hidden="true" />
          <span>
            Veszély! A kazán túlmelegedhet, 10 perc múlva automatikusan visszaáll AUTO-ra
          </span>
        </div>
      )}

      {/* Mód választó: AUTO / KÉZI BE / KÉZI KI */}
      <div className="mt-5 flex flex-col gap-3 sm:flex-row sm:items-center sm:justify-between">
        <div className="text-sm font-medium text-slate-300">Szivattyú mód</div>
        <div className="grid grid-cols-3 gap-1.5 rounded-lg border border-slate-800 bg-slate-950 p-1">
          {PUMP_MODE_OPTIONS.map((opt) => {
            const active = draftConfig.pumpMode === opt.value;
            return (
              <button
                key={opt.value}
                type="button"
                onClick={() =>
                  commitImmediate({
                    ...draftConfig,
                    pumpMode: opt.value,
                  })
                }
                disabled={sending}
                className={`min-h-[44px] rounded-md px-3 py-2 text-xs font-semibold sm:px-4 sm:text-sm whitespace-nowrap shrink-0 transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-orange-500 ${
                  active
                    ? opt.value === 'manual_off'
                      ? 'bg-red-500 text-white'
                      : 'bg-orange-500 text-slate-950'
                    : 'text-slate-300 hover:bg-slate-800 hover:text-slate-100'
                }`}
              >
                {opt.label}
              </button>
            );
          })}
        </div>
      </div>

      {/* 2 slider: indítás & leállítás */}
      <div className="mt-6 grid grid-cols-1 gap-6 border-t border-slate-800/80 pt-5 md:grid-cols-2">
        <div>
          <div className="flex items-baseline justify-between">
            <label
              htmlFor="pump-start-slider"
              className="text-sm font-medium text-slate-300"
            >
              Szivattyú indítás
            </label>
            <span className="tabular-nums text-lg font-semibold text-orange-400">
              {draftConfig.pumpStartTemp.toFixed(1)} °C
            </span>
          </div>
          <input
            id="pump-start-slider"
            type="range"
            min={30}
            max={50}
            step={0.5}
            value={draftConfig.pumpStartTemp}
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
                pumpStartTemp: roundHalf(parseFloat(e.target.value)),
              })
            }
            className="mt-3 w-full cursor-pointer"
          />
          <div className="mt-1.5 flex justify-between text-xs tabular-nums text-slate-500">
            <span>30.0 °C</span>
            <span>40.0 °C</span>
            <span>50.0 °C</span>
          </div>
        </div>

        <div>
          <div className="flex items-baseline justify-between">
            <label
              htmlFor="pump-stop-slider"
              className="text-sm font-medium text-slate-300"
            >
              Szivattyú leállítás
            </label>
            <span
              className={`tabular-nums text-lg font-semibold ${
                hysteresisInvalid ? 'text-red-400' : 'text-sky-400'
              }`}
            >
              {draftConfig.pumpStopTemp.toFixed(1)} °C
            </span>
          </div>
          <input
            id="pump-stop-slider"
            type="range"
            min={20}
            max={40}
            step={0.5}
            value={draftConfig.pumpStopTemp}
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
                pumpStopTemp: roundHalf(parseFloat(e.target.value)),
              })
            }
            className="mt-3 w-full cursor-pointer"
          />
          <div className="mt-1.5 flex justify-between text-xs tabular-nums text-slate-500">
            <span>20.0 °C</span>
            <span>30.0 °C</span>
            <span>40.0 °C</span>
          </div>
        </div>
      </div>

      {hysteresisInvalid && (
        <div
          role="alert"
          className="mt-4 rounded-lg border border-red-500/50 bg-red-950/50 px-4 py-2.5 text-sm font-medium text-red-300"
        >
          A leállításnak legalább 5°C-kal alacsonyabbnak kell lennie az indításnál!
        </div>
      )}

      {/* Indító szenzor választó */}
      <div className="mt-5 flex flex-col gap-3 border-t border-slate-800/80 pt-5 sm:flex-row sm:items-center sm:justify-between">
        <div>
          <label
            htmlFor="pump-sensor-select"
            className="block text-sm font-medium text-slate-300"
          >
            Indító szenzor
          </label>
          <p className="mt-1 text-xs text-slate-400">
            BE, ha a kiválasztott szenzor eléri az indítást. KI, ha az előremenő lehűl a
            leállítási értékre.
          </p>
        </div>

        <select
          id="pump-sensor-select"
          value={draftConfig.pumpStartSensor}
          onChange={(e) =>
            commitImmediate({
              ...draftConfig,
              pumpStartSensor: e.target.value as PumpStartSensor,
            })
          }
          disabled={sending}
          className="min-h-[44px] w-full rounded-lg border border-slate-700 bg-slate-950 px-3.5 py-2 text-sm font-medium text-slate-100 sm:w-52 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-orange-500"
        >
          {PUMP_SENSOR_OPTIONS.map((opt) => (
            <option key={opt.value} value={opt.value}>
              {opt.label}
            </option>
          ))}
        </select>
      </div>
    </section>
  );
}
