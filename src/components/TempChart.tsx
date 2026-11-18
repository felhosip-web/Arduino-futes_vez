import { useMemo } from 'react';
import {
  CartesianGrid,
  Line,
  LineChart,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis,
} from 'recharts';
import type { HistoryRange, HistorySample, HistorySensorKey } from '../types/history';

interface TempChartProps {
  title: string;
  subtitle: string;
  sensorKey: HistorySensorKey;
  color: string;
  samples: HistorySample[];
  range: HistoryRange;
  liveValue?: number | null;
  connectNulls?: boolean;
}

export function formatChartAxisTime(iso: string, range: HistoryRange): string {
  const date = new Date(iso);
  if (Number.isNaN(date.getTime())) return iso;
  const pad = (n: number) => String(n).padStart(2, '0');
  const hhmm = `${pad(date.getHours())}:${pad(date.getMinutes())}`;
  if (range === '7d') {
    return `${pad(date.getMonth() + 1)}.${pad(date.getDate())} ${hhmm}`;
  }
  return hhmm;
}

export function formatChartTooltipTime(iso: string): string {
  const date = new Date(iso);
  if (Number.isNaN(date.getTime())) return iso;
  const pad = (n: number) => String(n).padStart(2, '0');
  return `${date.getFullYear()}.${pad(date.getMonth() + 1)}.${pad(date.getDate())} ${pad(date.getHours())}:${pad(date.getMinutes())}`;
}

function isValidTemp(val: number | null | undefined): val is number {
  return typeof val === 'number' && Number.isFinite(val) && val > -120;
}

interface CustomTooltipProps {
  active?: boolean;
  payload?: ReadonlyArray<{
    value?: unknown;
  }>;
  label?: string | number;
  title: string;
  color: string;
}

function SingleSensorTooltip({ active, payload, label, title, color }: CustomTooltipProps) {
  if (!active || label === undefined || label === null || label === '') return null;
  const rawVal = payload?.[0]?.value;
  const numericVal = typeof rawVal === 'number' ? rawVal : null;
  const hasVal = isValidTemp(numericVal);

  return (
    <div className="rounded-lg border border-slate-700 bg-slate-950/95 px-3 py-2 text-xs shadow-lg">
      <div className="font-mono text-slate-400 tabular-nums">
        {typeof label === 'string' ? formatChartTooltipTime(label) : String(label)}
      </div>
      <div className="mt-1 flex items-center gap-2 tabular-nums">
        <span
          className="inline-block h-2 w-2 shrink-0 rounded-full"
          style={{ backgroundColor: hasVal ? color : '#64748b' }}
          aria-hidden="true"
        />
        <span className="text-slate-300">{title}:</span>
        <span className={`font-semibold ${hasVal ? 'text-slate-50' : 'text-slate-400'}`}>
          {hasVal ? `${numericVal.toFixed(1)} °C` : 'Nincs adat'}
        </span>
      </div>
    </div>
  );
}

export function TempChart({
  title,
  subtitle,
  sensorKey,
  color,
  samples,
  range,
  liveValue,
  connectNulls,
}: TempChartProps) {
  const shouldConnectNulls =
    connectNulls ?? (sensorKey === 'kulteri' || sensorKey === 'szoba');
  const chartData = useMemo(
    () =>
      samples.map((s) => {
        const val = s[sensorKey];
        return {
          ...s,
          [sensorKey]: isValidTemp(val) ? val : null,
        };
      }),
    [samples, sensorKey]
  );

  const stats = useMemo(() => {
    let min = Infinity;
    let max = -Infinity;
    let sum = 0;
    let count = 0;
    let lastValid: number | null = null;

    for (const s of chartData) {
      const val = s[sensorKey];
      if (isValidTemp(val)) {
        if (val < min) min = val;
        if (val > max) max = val;
        sum += val;
        count++;
        lastValid = val;
      }
    }

    return {
      count,
      min: count > 0 ? min : null,
      max: count > 0 ? max : null,
      avg: count > 0 ? sum / count : null,
      lastValid,
    };
  }, [chartData, sensorKey]);

  const currentDisplay = isValidTemp(liveValue) ? liveValue : stats.lastValid;

  return (
    <section className="rounded-xl border border-slate-800 bg-slate-900 p-5">
      <div className="flex flex-wrap items-start justify-between gap-3 border-b border-slate-800/80 pb-3.5">
        <div>
          <h3 className="text-base font-semibold text-slate-100">{title}</h3>
          <p className="mt-0.5 text-xs text-slate-400">{subtitle}</p>
        </div>

        <div className="text-right tabular-nums">
          <div className="flex items-baseline justify-end gap-1">
            <span
              className="text-2xl font-bold"
              style={{ color: currentDisplay !== null ? color : '#94a3b8' }}
            >
              {currentDisplay !== null ? currentDisplay.toFixed(1) : '---'}
            </span>
            {currentDisplay !== null && (
              <span className="text-xs font-medium text-slate-400">°C</span>
            )}
          </div>
        </div>
      </div>

      <div className="mt-2.5 flex flex-wrap items-center gap-x-3 gap-y-1 text-xs text-slate-400 tabular-nums">
        <span>
          Max:{' '}
          <strong className="font-semibold text-slate-200">
            {stats.max !== null ? `${stats.max.toFixed(1)}°C` : '---'}
          </strong>
        </span>
        <span aria-hidden="true">·</span>
        <span>
          Min:{' '}
          <strong className="font-semibold text-slate-200">
            {stats.min !== null ? `${stats.min.toFixed(1)}°C` : '---'}
          </strong>
        </span>
        <span aria-hidden="true">·</span>
        <span>
          Átlag:{' '}
          <strong className="font-semibold text-slate-200">
            {stats.avg !== null ? `${stats.avg.toFixed(1)}°C` : '---'}
          </strong>
        </span>
      </div>

      <div className="mt-4 h-56 w-full">
        {stats.count === 0 ? (
          <div className="flex h-full items-center justify-center rounded-lg border border-dashed border-slate-800 text-xs text-slate-400">
            Nincs adat erre az időszakra
          </div>
        ) : (
          <ResponsiveContainer width="100%" height="100%">
            <LineChart data={chartData} margin={{ top: 8, right: 12, left: -10, bottom: 0 }}>
              <CartesianGrid stroke="#1e293b" strokeDasharray="3 3" vertical={false} />
              <XAxis
                dataKey="timestamp"
                tickFormatter={(val) =>
                  typeof val === 'string' ? formatChartAxisTime(val, range) : ''
                }
                stroke="#475569"
                tick={{ fill: '#94a3b8', fontSize: 11 }}
                minTickGap={28}
              />
              <YAxis
                stroke="#475569"
                tick={{ fill: '#94a3b8', fontSize: 11 }}
                tickFormatter={(val) => `${val}°`}
                domain={['auto', 'auto']}
                width={46}
              />
              <Tooltip
                content={(tooltipProps) => (
                  <SingleSensorTooltip
                    active={tooltipProps.active}
                    payload={tooltipProps.payload}
                    label={tooltipProps.label}
                    title={title}
                    color={color}
                  />
                )}
              />
              <Line
                type="monotone"
                dataKey={sensorKey}
                name={title}
                stroke={color}
                strokeWidth={2}
                dot={false}
                activeDot={{ r: 4, fill: color, stroke: '#0f172a', strokeWidth: 2 }}
                connectNulls={shouldConnectNulls}
                isAnimationActive={false}
              />
            </LineChart>
          </ResponsiveContainer>
        )}
      </div>
    </section>
  );
}
