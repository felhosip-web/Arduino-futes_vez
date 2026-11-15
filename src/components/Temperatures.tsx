import type { HeatingTemperatures } from '../types/heating';

interface TemperaturesProps {
  temperatures: HeatingTemperatures;
}

const TEMP_ITEMS: ReadonlyArray<{
  key: keyof HeatingTemperatures;
  label: string;
  subtitle: string;
}> = [
  { key: 'eloremeno', label: 'Előremenő', subtitle: 'Kazánból kilépő víz' },
  { key: 'visszatero', label: 'Visszatérő', subtitle: 'Fűtési kör visszatérő' },
  { key: 'kazan', label: 'Kazán', subtitle: 'Kazántest hőfok' },
  { key: 'kulteri', label: 'Kültéri', subtitle: 'Külső környezet' },
];

function isSensorError(val: number | null | undefined): boolean {
  return val === null || val === undefined || Number.isNaN(val) || val <= -120;
}

export function Temperatures({ temperatures }: TemperaturesProps) {
  return (
    <section
      aria-labelledby="temperatures-title"
      className="rounded-xl border border-slate-800 bg-slate-900 p-5 sm:p-6"
    >
      <div className="border-b border-slate-800/80 pb-4">
        <h2 id="temperatures-title" className="text-lg font-semibold text-slate-100">
          Hőmérsékletek
        </h2>
        <p className="mt-0.5 text-xs text-slate-400">
          Arduino Nano DS18B20 szenzorok értékei
        </p>
      </div>

      <div className="mt-4 grid grid-cols-2 gap-4 lg:grid-cols-4">
        {TEMP_ITEMS.map((item) => {
          const val = temperatures?.[item.key];
          const error = isSensorError(val);

          return (
            <div
              key={item.key}
              className="rounded-lg border border-slate-800/90 bg-slate-950/60 p-4"
            >
              <div className="text-sm font-medium text-slate-300">{item.label}</div>
              <div className="mt-0.5 text-xs text-slate-500">{item.subtitle}</div>

              <div className="mt-3 flex items-baseline gap-1">
                <span
                  className={`tabular-nums text-2xl font-bold sm:text-3xl ${
                    error ? 'text-red-400' : 'text-slate-50'
                  }`}
                >
                  {error ? '---' : (val as number).toFixed(1)}
                </span>
                {!error && <span className="text-sm font-medium text-slate-400">°C</span>}
              </div>

              {error && (
                <div className="mt-1 text-xs font-medium text-red-400">Szenzor hiba</div>
              )}
            </div>
          );
        })}
      </div>
    </section>
  );
}
