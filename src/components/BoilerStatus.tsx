interface BoilerStatusProps {
  kazanStatus: number[];
  nrf24Ok: boolean;
}

export function BoilerStatus({ kazanStatus, nrf24Ok }: BoilerStatusProps) {
  const bits = Array.from({ length: 8 }, (_, idx) => (kazanStatus?.[idx] ? 1 : 0));

  return (
    <section
      aria-labelledby="boiler-status-title"
      className={`rounded-xl border border-slate-800 bg-slate-900 p-5 sm:p-6 transition-opacity ${
        nrf24Ok ? 'opacity-100' : 'opacity-55'
      }`}
    >
      <div className="flex flex-wrap items-center justify-between gap-3 border-b border-slate-800/80 pb-4">
        <div>
          <h2 id="boiler-status-title" className="text-lg font-semibold text-slate-100">
            Kazán állapota (Nano → ESP)
          </h2>
          <p className="mt-0.5 text-xs text-slate-400">
            Nano visszajelző státusz bitek (Bit 0–7)
          </p>
        </div>

        {!nrf24Ok && (
          <span className="text-xs font-semibold text-red-400">
            Nincs kapcsolat a kazánnal
          </span>
        )}
      </div>

      {!nrf24Ok && (
        <div className="mt-4 rounded-lg border border-slate-800 bg-slate-950/70 px-4 py-3 text-sm font-medium text-slate-300">
          Nincs kapcsolat a kazánnal
        </div>
      )}

      <div className="mt-4 grid grid-cols-4 gap-3 sm:grid-cols-8">
        {bits.map((bitVal, idx) => {
          const active = nrf24Ok && bitVal === 1;
          return (
            <div
              key={idx}
              className="flex flex-col items-center justify-center rounded-lg border border-slate-800/80 bg-slate-950/60 px-2 py-3"
            >
              <span
                className={`h-3.5 w-3.5 rounded-full ${
                  active ? 'bg-emerald-500' : 'bg-slate-600'
                }`}
                aria-hidden="true"
              />
              <span className="mt-2 tabular-nums text-xs font-semibold text-slate-200">
                Bit {idx}
              </span>
              <span className="mt-0.5 text-[11px] text-slate-400">Tartalék</span>
            </div>
          );
        })}
      </div>
    </section>
  );
}
