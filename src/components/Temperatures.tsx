import { ArrowDown, ArrowUp, Equal } from 'lucide-react';
import type { HeatingTemperatures, TrendStats } from '../types/heating';

interface TemperaturesProps {
  temperatures: HeatingTemperatures;
  trend?: TrendStats;
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

function renderEloremenoTrend(trend: TrendStats) {
  const dir = trend.eloremeno;
  const delta = typeof trend.delta === 'number' && Number.isFinite(trend.delta) ? trend.delta : 0;

  if (dir === 'up') {
    const sign = delta > 0 ? '+' : '';
    return (
      <div
        className="mt-1.5 inline-flex items-center gap-1.5 text-xs font-semibold text-orange-400 tabular-nums"
        aria-label={`Trend felfelé ${sign}${delta.toFixed(1)}°C 10 perc alatt`}
      >
        <ArrowUp className="h-3.5 w-3.5 shrink-0" aria-hidden="true" />
        <span>
          {sign}
          {delta.toFixed(1)}°C / 10 perc
        </span>
      </div>
    );
  }

  if (dir === 'down') {
    const sign = delta > 0 ? '+' : '';
    return (
      <div
        className="mt-1.5 inline-flex items-center gap-1.5 text-xs font-semibold text-sky-400 tabular-nums"
        aria-label={`Trend lefelé ${sign}${delta.toFixed(1)}°C 10 perc alatt`}
      >
        <ArrowDown className="h-3.5 w-3.5 shrink-0" aria-hidden="true" />
        <span>
          {sign}
          {delta.toFixed(1)}°C / 10 perc
        </span>
      </div>
    );
  }

  const flatLabel =
    Math.abs(delta) < 0.05
      ? '±0.0°C'
      : `${delta > 0 ? '+' : ''}${delta.toFixed(1)}°C / 10 perc`;

  return (
    <div
      className="mt-1.5 inline-flex items-center gap-1.5 text-xs font-medium text-slate-400 tabular-nums"
      aria-label={`Trend stabil ${flatLabel}`}
    >
      <Equal className="h-3.5 w-3.5 shrink-0" aria-hidden="true" />
      <span>{flatLabel}</span>
    </div>
  );
}

export function Temperatures({ temperatures, trend }: TemperaturesProps) {
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

              {item.key === 'eloremeno' && trend && !error && renderEloremenoTrend(trend)}
            </div>
          );
        })}
      </div>
    </section>
  );
}
