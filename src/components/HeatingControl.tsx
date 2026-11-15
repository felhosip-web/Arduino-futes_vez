import { useEffect, useRef, useState } from 'react';
import { Flame, Minus, Plus, Snowflake } from 'lucide-react';
import type { HeatingMode, HeatingStatusPayload } from '../types/heating';

interface HeatingControlProps {
  status: HeatingStatusPayload;
  sending: boolean;
  onSendRoomCommand: (patch: {
    targetTemp: number;
    mode: HeatingMode;
    manualHeating?: boolean;
  }) => Promise<void>;
}

const MODE_OPTIONS: ReadonlyArray<{ value: HeatingMode; label: string }> = [
  { value: 'auto', label: 'AUTO' },
  { value: 'manual', label: 'KÉZI' },
  { value: 'off', label: 'KI' },
];

const TARGET_MIN = 16;
const TARGET_MAX = 26;
const TARGET_STEP = 0.5;

function clampTarget(value: number): number {
  const clamped = Math.min(TARGET_MAX, Math.max(TARGET_MIN, value));
  return Math.round(clamped * 2) / 2;
}

export function HeatingControl({
  status,
  sending,
  onSendRoomCommand,
}: HeatingControlProps) {
  const [draftTargetTemp, setDraftTargetTemp] = useState<number>(status.targetTemp);
  const debounceTimerRef = useRef<number | null>(null);
  const isSlidingRef = useRef<boolean>(false);

  // Az ESP-től jövő status mindig elsőbbséget élvez (encoder felülírhatja)
  useEffect(() => {
    if (!isSlidingRef.current) {
      setDraftTargetTemp(status.targetTemp);
    }
  }, [status.targetTemp]);

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

  const handleStepTemp = (delta: number) => {
    clearPendingDebounce();
    isSlidingRef.current = false;
    const nextTemp = clampTarget(draftTargetTemp + delta);
    setDraftTargetTemp(nextTemp);
    void onSendRoomCommand({
      targetTemp: nextTemp,
      mode: status.mode,
      manualHeating: status.manualHeating,
    });
  };

  const handleSliderChange = (rawValue: number) => {
    const nextTemp = clampTarget(rawValue);
    setDraftTargetTemp(nextTemp);
    clearPendingDebounce();
    debounceTimerRef.current = window.setTimeout(() => {
      debounceTimerRef.current = null;
      isSlidingRef.current = false;
      void onSendRoomCommand({
        targetTemp: nextTemp,
        mode: status.mode,
        manualHeating: status.manualHeating,
      });
    }, 500);
  };

  const handleModeChange = (nextMode: HeatingMode) => {
    clearPendingDebounce();
    isSlidingRef.current = false;
    void onSendRoomCommand({
      targetTemp: draftTargetTemp,
      mode: nextMode,
      manualHeating:
        nextMode === 'manual' ? (status.manualHeating ?? status.heatingOn) : false,
    });
  };

  const handleManualHeatingToggle = () => {
    clearPendingDebounce();
    isSlidingRef.current = false;
    void onSendRoomCommand({
      targetTemp: draftTargetTemp,
      mode: 'manual',
      manualHeating: !(status.manualHeating ?? status.heatingOn),
    });
  };

  return (
    <section
      aria-labelledby="heating-control-title"
      className="rounded-xl border border-slate-800 bg-slate-900 p-5 sm:p-6"
    >
      <div className="flex flex-wrap items-center justify-between gap-3 border-b border-slate-800/80 pb-4">
        <div>
          <h2 id="heating-control-title" className="text-lg font-semibold text-slate-100">
            Fűtés szabályzás
          </h2>
          <p className="mt-0.5 text-xs text-slate-400">
            Az ESP32-n lévő tekerőgombbal is állítható
          </p>
        </div>

        <div className="flex flex-wrap items-center gap-4">
          <div className="flex items-center gap-2 text-xs font-medium">
            <span
              className={`inline-block h-2.5 w-2.5 rounded-full ${
                status.nrf24_ok ? 'bg-emerald-500' : 'bg-red-500'
              }`}
              aria-hidden="true"
            />
            <span className={status.nrf24_ok ? 'text-emerald-300' : 'text-red-300'}>
              {status.nrf24_ok ? 'NRF24 kapcsolat OK' : 'NRF24 kapcsolat hiba'}
            </span>
          </div>

          <div
            className={`inline-flex items-center gap-1.5 rounded-lg border px-3 py-1.5 text-xs font-semibold whitespace-nowrap shrink-0 ${
              status.heatingOn
                ? 'border-orange-500/40 bg-orange-500/15 text-orange-300'
                : 'border-slate-700 bg-slate-800 text-slate-300'
            }`}
          >
            {status.heatingOn ? (
              <Flame className="h-3.5 w-3.5 text-orange-400" aria-hidden="true" />
            ) : (
              <Snowflake className="h-3.5 w-3.5 text-slate-400" aria-hidden="true" />
            )}
            <span>{status.heatingOn ? 'FŰT' : 'NEM FŰT'}</span>
          </div>
        </div>
      </div>

      {/* Nagy számok: Szobahő és Célhő */}
      <div className="mt-5 grid grid-cols-1 gap-6 sm:grid-cols-2">
        <div className="border-b border-slate-800/70 pb-4 sm:border-b-0 sm:border-r sm:pb-0 sm:pr-6">
          <div className="text-xs font-medium text-slate-400">Szobahőmérséklet (LM35)</div>
          <div className="mt-1.5 flex items-baseline gap-1.5">
            <span className="tabular-nums text-4xl font-bold tracking-tight text-slate-50 sm:text-5xl">
              {Number.isFinite(status.currentTemp) ? status.currentTemp.toFixed(1) : '--.-'}
            </span>
            <span className="text-xl font-medium text-slate-400">°C</span>
          </div>
        </div>

        <div className="flex flex-col justify-between">
          <div className="flex items-baseline justify-between gap-3">
            <div>
              <div className="text-xs font-medium text-slate-400">Célhőmérséklet</div>
              <div className="mt-1.5 flex items-baseline gap-1.5">
                <span className="tabular-nums text-4xl font-bold tracking-tight text-orange-400 sm:text-5xl">
                  {draftTargetTemp.toFixed(1)}
                </span>
                <span className="text-xl font-medium text-orange-300/80">°C</span>
              </div>
            </div>

            <div className="flex items-center gap-2">
              <button
                type="button"
                onClick={() => handleStepTemp(-TARGET_STEP)}
                disabled={draftTargetTemp <= TARGET_MIN}
                aria-label="Célhőmérséklet csökkentése 0.5 fokkal"
                className="inline-flex h-11 w-11 items-center justify-center rounded-lg border border-slate-700 bg-slate-800 text-slate-100 transition-colors hover:bg-slate-700 active:scale-95 disabled:cursor-not-allowed disabled:opacity-40 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-orange-500"
              >
                <Minus className="h-5 w-5" />
              </button>
              <button
                type="button"
                onClick={() => handleStepTemp(TARGET_STEP)}
                disabled={draftTargetTemp >= TARGET_MAX}
                aria-label="Célhőmérséklet növelése 0.5 fokkal"
                className="inline-flex h-11 w-11 items-center justify-center rounded-lg border border-slate-700 bg-slate-800 text-slate-100 transition-colors hover:bg-slate-700 active:scale-95 disabled:cursor-not-allowed disabled:opacity-40 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-orange-500"
              >
                <Plus className="h-5 w-5" />
              </button>
            </div>
          </div>

          <div className="mt-4">
            <label htmlFor="target-temp-slider" className="sr-only">
              Célhőmérséklet csúszka
            </label>
            <input
              id="target-temp-slider"
              type="range"
              min={TARGET_MIN}
              max={TARGET_MAX}
              step={TARGET_STEP}
              value={draftTargetTemp}
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
              onChange={(e) => handleSliderChange(parseFloat(e.target.value))}
              className="w-full cursor-pointer"
            />
            <div className="mt-1.5 flex justify-between text-xs tabular-nums text-slate-500">
              <span>16.0 °C</span>
              <span>21.0 °C</span>
              <span>26.0 °C</span>
            </div>
          </div>
        </div>
      </div>

      {/* Mód választó */}
      <div className="mt-5 flex flex-col gap-3 border-t border-slate-800/80 pt-4 sm:flex-row sm:items-center sm:justify-between">
        <div className="text-xs font-medium text-slate-400">Üzemmód</div>

        <div className="flex flex-wrap items-center gap-2">
          <div className="grid grid-cols-3 gap-1.5 rounded-lg border border-slate-800 bg-slate-950 p-1">
            {MODE_OPTIONS.map((opt) => {
              const active = status.mode === opt.value;
              return (
                <button
                  key={opt.value}
                  type="button"
                  onClick={() => handleModeChange(opt.value)}
                  disabled={sending}
                  className={`min-h-[44px] rounded-md px-4 py-2 text-sm font-semibold whitespace-nowrap shrink-0 transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-orange-500 ${
                    active
                      ? 'bg-orange-500 text-slate-950'
                      : 'text-slate-300 hover:bg-slate-800 hover:text-slate-100'
                  }`}
                >
                  {opt.label}
                </button>
              );
            })}
          </div>

          {status.mode === 'manual' && (
            <button
              type="button"
              onClick={handleManualHeatingToggle}
              disabled={sending}
              className={`min-h-[44px] rounded-lg border px-4 py-2 text-xs font-semibold whitespace-nowrap shrink-0 transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-orange-500 ${
                status.manualHeating ?? status.heatingOn
                  ? 'border-orange-500/50 bg-orange-500/20 text-orange-200 hover:bg-orange-500/30'
                  : 'border-slate-700 bg-slate-800 text-slate-300 hover:bg-slate-700'
              }`}
            >
              {status.manualHeating ?? status.heatingOn
                ? 'Kézi jelzés: BE'
                : 'Kézi jelzés: KI'}
            </button>
          )}
        </div>
      </div>
    </section>
  );
}
