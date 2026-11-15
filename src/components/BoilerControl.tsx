import { Lock } from 'lucide-react';
import {
  OUTPUT_ITEMS,
  type HeatingOutputs,
  type OutputKey,
  type PumpMode,
} from '../types/heating';

interface BoilerControlProps {
  outputs: HeatingOutputs;
  pumpMode: PumpMode;
  pendingKey: OutputKey | null;
  onToggleOutput: (key: OutputKey, nextVal: boolean) => Promise<void>;
}

export function BoilerControl({
  outputs,
  pumpMode,
  pendingKey,
  onToggleOutput,
}: BoilerControlProps) {
  const isPumpAuto = pumpMode === 'auto';

  return (
    <section
      aria-labelledby="boiler-control-title"
      className="rounded-xl border border-slate-800 bg-slate-900 p-5 sm:p-6"
    >
      <div className="border-b border-slate-800/80 pb-4">
        <h2 id="boiler-control-title" className="text-lg font-semibold text-slate-100">
          Kazán vezérlés (ESP → Nano)
        </h2>
        <p className="mt-0.5 text-xs text-slate-400">
          Kimeneti relék és állapotjelző LED-ek kapcsolása
        </p>
      </div>

      <div className="mt-4 grid grid-cols-1 gap-3 sm:grid-cols-2 lg:grid-cols-4">
        {OUTPUT_ITEMS.map((item) => {
          const isOn = Boolean(outputs?.[item.key]);
          const isPumpRow = item.key === 'szivattyu';
          const isLockedByAuto = isPumpRow && isPumpAuto;
          const isPending = pendingKey === item.key;
          const disabled = isLockedByAuto || isPending;

          return (
            <button
              key={item.key}
              type="button"
              role="switch"
              aria-checked={isOn}
              disabled={disabled}
              onClick={() => {
                if (!disabled) {
                  void onToggleOutput(item.key, !isOn);
                }
              }}
              className={`flex min-h-[60px] w-full items-center justify-between gap-3 rounded-lg border px-4 py-3 text-left transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-orange-500 ${
                isLockedByAuto
                  ? 'cursor-not-allowed border-slate-800 bg-slate-950/60 opacity-75'
                  : isOn
                    ? 'border-emerald-500/40 bg-emerald-950/30 hover:bg-emerald-950/45'
                    : 'border-slate-800 bg-slate-950/60 hover:border-slate-700 hover:bg-slate-800/70'
              } ${isPending ? 'opacity-60' : ''}`}
            >
              <div className="min-w-0">
                <div className="flex items-center gap-1.5 text-sm font-medium text-slate-100">
                  <span className="truncate">{item.label}</span>
                  {isLockedByAuto && (
                    <Lock className="h-3.5 w-3.5 shrink-0 text-slate-400" aria-hidden="true" />
                  )}
                </div>
                <div className="mt-0.5 text-xs text-slate-400">
                  {isLockedByAuto
                    ? 'AUTO - Kazánvédelem vezérli'
                    : isOn
                      ? 'Bekapcsolva'
                      : 'Kikapcsolva'}
                </div>
              </div>

              <span
                aria-hidden="true"
                className={`relative inline-flex h-6 w-11 shrink-0 items-center rounded-full transition-colors ${
                  isOn ? 'bg-emerald-500' : 'bg-slate-700'
                }`}
              >
                <span
                  className={`inline-block h-4 w-4 transform rounded-full bg-white transition-transform ${
                    isOn ? 'translate-x-6' : 'translate-x-1'
                  }`}
                />
              </span>
            </button>
          );
        })}
      </div>
    </section>
  );
}
