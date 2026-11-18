import { useCallback, useEffect, useRef, useState } from 'react';
import { AlertTriangle, LineChart as LineChartIcon, RefreshCw } from 'lucide-react';
import {
  CartesianGrid,
  Line,
  LineChart,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis,
} from 'recharts';
import { API_URL, DEVICE_ID } from '../config';
import type { HeatingStatusPayload } from '../types/heating';
import {
  DEFAULT_RESOLUTION_BY_RANGE,
  RANGE_LABELS,
  RESOLUTION_LABELS,
  SENSOR_SERIES,
  type HistoryQueryResponse,
  type HistoryRange,
  type HistoryResolution,
  type HistorySample,
  type HistorySensorKey,
} from '../types/history';
import { formatChartAxisTime, formatChartTooltipTime, TempChart } from './TempChart';

interface ChartsViewProps {
  liveStatus?: HeatingStatusPayload | null;
  refreshSignal?: number;
}

const RANGE_OPTIONS: readonly HistoryRange[] = ['1h', '6h', '24h', '7d'];
const RESOLUTION_OPTIONS: readonly HistoryResolution[] = ['1m', '5m', '15m', '1h'];
const AUTO_REFRESH_MS = 60_000;

const ADJECTIVE_RESOLUTION_LABELS: Record<HistoryResolution, string> = {
  '1m': '1 perces',
  '5m': '5 perces',
  '15m': '15 perces',
  '1h': '1 órás',
};

function getLiveSensorValue(
  status: HeatingStatusPayload | null | undefined,
  key: HistorySensorKey
): number | null {
  if (!status) return null;
  if (key === 'szoba') {
    return typeof status.currentTemp === 'number' ? status.currentTemp : null;
  }
  const val = status.homersekletek?.[key];
  return typeof val === 'number' ? val : null;
}

interface CombinedTooltipProps {
  active?: boolean;
  payload?: ReadonlyArray<{
    dataKey?: unknown;
    value?: unknown;
    color?: string;
    name?: string | number;
  }>;
  label?: string | number;
  visibleSensors: Record<HistorySensorKey, boolean>;
}

function CombinedSensorsTooltip({
  active,
  payload,
  label,
  visibleSensors,
}: CombinedTooltipProps) {
  if (!active || !payload) return null;

  const valueByKey = new Map<HistorySensorKey, number | null>();
  for (const item of payload) {
    const key = String(item.dataKey) as HistorySensorKey;
    valueByKey.set(
      key,
      typeof item.value === 'number' && Number.isFinite(item.value) ? item.value : null
    );
  }

  return (
    <div className="min-w-[200px] rounded-lg border border-slate-700 bg-slate-950/95 px-3.5 py-2.5 text-xs shadow-lg">
      <div className="border-b border-slate-800 pb-1.5 font-mono text-slate-300 tabular-nums">
        {typeof label === 'string' ? formatChartTooltipTime(label) : String(label ?? '')}
      </div>
      <div className="mt-2 space-y-1">
        {SENSOR_SERIES.filter((s) => visibleSensors[s.key]).map((series) => {
          const val = valueByKey.get(series.key);
          return (
            <div
              key={series.key}
              className="flex items-center justify-between gap-4 tabular-nums"
            >
              <span className="flex items-center gap-2 text-slate-300">
                <span
                  className="inline-block h-2 w-2 shrink-0 rounded-full"
                  style={{ backgroundColor: series.color }}
                  aria-hidden="true"
                />
                <span>{series.label}</span>
              </span>
              <span className="font-semibold text-slate-50">
                {val !== null && val !== undefined ? `${val.toFixed(1)} °C` : '---'}
              </span>
            </div>
          );
        })}
      </div>
    </div>
  );
}

export function ChartsView({ liveStatus, refreshSignal = 0 }: ChartsViewProps) {
  const [range, setRange] = useState<HistoryRange>('24h');
  const [resolution, setResolution] = useState<HistoryResolution>('15m');
  const [effectiveResolution, setEffectiveResolution] = useState<HistoryResolution>('15m');
  const [resolutionClamped, setResolutionClamped] = useState<boolean>(false);
  const [samples, setSamples] = useState<HistorySample[]>([]);
  const [loading, setLoading] = useState<boolean>(true);
  const [refreshing, setRefreshing] = useState<boolean>(false);
  const [errorMsg, setErrorMsg] = useState<string | null>(null);
  const [manualTick, setManualTick] = useState<number>(0);

  const [visibleSensors, setVisibleSensors] = useState<Record<HistorySensorKey, boolean>>({
    eloremeno: true,
    visszatero: true,
    kazan: true,
    kulteri: true,
    szoba: true,
  });

  const hasLoadedOnceRef = useRef<boolean>(false);

  const fetchHistory = useCallback(
    async (options?: { signal?: AbortSignal; silent?: boolean }) => {
      const silent = Boolean(options?.silent);
      if (!silent && !hasLoadedOnceRef.current) {
        setLoading(true);
      } else {
        setRefreshing(true);
      }
      setErrorMsg(null);

      try {
        const params = new URLSearchParams({
          range,
          resolution,
          sensor: 'all',
        });

        const res = await fetch(
          `${API_URL}/api/heating/${DEVICE_ID}/history?${params.toString()}`,
          { signal: options?.signal }
        );

        if (!res.ok) {
          const body = (await res.json().catch(() => null)) as { error?: string } | null;
          if (res.status === 401) {
            throw new Error(
              body?.error || 'Nincs jogosultság az előzmények lekérdezéséhez (401).'
            );
          }
          throw new Error(body?.error || `Idősor lekérdezési hiba (HTTP ${res.status})`);
        }

        const data = (await res.json()) as HistoryQueryResponse;
        hasLoadedOnceRef.current = true;
        setSamples(Array.isArray(data.samples) ? data.samples : []);
        setEffectiveResolution(data.resolution ?? resolution);
        setResolutionClamped(Boolean(data.resolutionClamped));
      } catch (err) {
        if (err instanceof DOMException && err.name === 'AbortError') {
          return;
        }
        const message =
          err instanceof Error ? err.message : 'Nem sikerült betölteni a grafikon adatokat.';
        setErrorMsg(message);
      } finally {
        if (!options?.signal?.aborted) {
          setLoading(false);
          setRefreshing(false);
        }
      }
    },
    [range, resolution]
  );

  // Kezdeti betöltés, időtartam/felbontás váltás és kézi frissítés
  useEffect(() => {
    const controller = new AbortController();
    void fetchHistory({ signal: controller.signal });
    return () => {
      controller.abort();
    };
  }, [fetchHistory, manualTick, refreshSignal]);

  // 60 másodpercenkénti automatikus csendes frissítés (villogás elkerülése)
  useEffect(() => {
    const controller = new AbortController();
    const timer = window.setInterval(() => {
      void fetchHistory({ signal: controller.signal, silent: true });
    }, AUTO_REFRESH_MS);

    return () => {
      window.clearInterval(timer);
      controller.abort();
    };
  }, [fetchHistory]);

  const handleSelectRange = (nextRange: HistoryRange) => {
    if (nextRange === range) return;
    setRange(nextRange);
    setResolution(DEFAULT_RESOLUTION_BY_RANGE[nextRange]);
  };

  const handleToggleSensor = (key: HistorySensorKey) => {
    setVisibleSensors((prev) => ({
      ...prev,
      [key]: !prev[key],
    }));
  };

  const anyCombinedVisible = SENSOR_SERIES.some((s) => visibleSensors[s.key]);
  const hasCombinedValidPoints = samples.some((sample) =>
    SENSOR_SERIES.some((s) => {
      if (!visibleSensors[s.key]) return false;
      const val = sample[s.key];
      return typeof val === 'number' && Number.isFinite(val) && val > -120;
    })
  );

  return (
    <div className="space-y-6">
      {/* Vezérlősáv */}
      <section
        aria-labelledby="charts-view-title"
        className="rounded-xl border border-slate-800 bg-slate-900 p-5 sm:p-6"
      >
        <div className="flex flex-wrap items-center justify-between gap-4 border-b border-slate-800/80 pb-4">
          <div className="flex items-start gap-3">
            <LineChartIcon className="mt-0.5 h-5 w-5 shrink-0 text-orange-400" aria-hidden="true" />
            <div>
              <h2 id="charts-view-title" className="text-lg font-semibold text-slate-100">
                Hőmérséklet grafikonok
              </h2>
              <p className="mt-0.5 text-xs text-slate-400 tabular-nums">
                60 másodperces mintavétel (7 napos puffer) · {samples.length} pont megjelenítve
              </p>
            </div>
          </div>

          <button
            type="button"
            onClick={() => setManualTick((t) => t + 1)}
            disabled={refreshing}
            className="inline-flex min-h-[44px] items-center gap-2 rounded-lg border border-slate-700 bg-slate-800 px-3.5 py-2 text-xs font-semibold text-slate-200 whitespace-nowrap shrink-0 transition-colors hover:bg-slate-700 disabled:opacity-60 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-orange-500 sm:text-sm"
          >
            <RefreshCw
              className={`h-4 w-4 ${refreshing ? 'animate-spin' : ''}`}
              aria-hidden="true"
            />
            <span>Frissítés</span>
          </button>
        </div>

        <div className="mt-4 flex flex-col gap-4 lg:flex-row lg:items-center lg:justify-between">
          <div>
            <div className="mb-1.5 text-xs font-medium text-slate-400">Időtartam</div>
            <div role="group" aria-label="Időtartam választó" className="flex flex-wrap gap-2">
              {RANGE_OPTIONS.map((opt) => {
                const active = range === opt;
                return (
                  <button
                    key={opt}
                    type="button"
                    aria-pressed={active}
                    onClick={() => handleSelectRange(opt)}
                    className={`min-h-[40px] rounded-lg border px-3.5 py-1.5 text-xs font-semibold whitespace-nowrap shrink-0 transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-orange-500 sm:text-sm ${
                      active
                        ? 'border-orange-500 bg-orange-500 text-slate-950'
                        : 'border-slate-700 bg-slate-800 text-slate-300 hover:bg-slate-700 hover:text-slate-100'
                    }`}
                  >
                    {RANGE_LABELS[opt]}
                  </button>
                );
              })}
            </div>
          </div>

          <div>
            <div className="mb-1.5 text-xs font-medium text-slate-400">Felbontás</div>
            <div role="group" aria-label="Felbontás választó" className="flex flex-wrap gap-2">
              {RESOLUTION_OPTIONS.map((opt) => {
                const active = resolution === opt;
                return (
                  <button
                    key={opt}
                    type="button"
                    aria-pressed={active}
                    onClick={() => setResolution(opt)}
                    className={`min-h-[40px] rounded-lg border px-3.5 py-1.5 text-xs font-semibold whitespace-nowrap shrink-0 transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-orange-500 sm:text-sm ${
                      active
                        ? 'border-orange-500 bg-orange-500 text-slate-950'
                        : 'border-slate-700 bg-slate-800 text-slate-300 hover:bg-slate-700 hover:text-slate-100'
                    }`}
                  >
                    {RESOLUTION_LABELS[opt]}
                  </button>
                );
              })}
            </div>
          </div>
        </div>

        {resolutionClamped && (
          <div
            role="status"
            className="mt-4 flex items-center gap-2.5 rounded-lg border border-amber-500/40 bg-amber-950/40 px-4 py-2.5 text-xs text-amber-200 sm:text-sm"
          >
            <AlertTriangle className="h-4 w-4 shrink-0 text-amber-400" aria-hidden="true" />
            <span>
              {RANGE_LABELS[range]} + {RESOLUTION_LABELS[resolution]} túl sok pont,{' '}
              {ADJECTIVE_RESOLUTION_LABELS[effectiveResolution]} átlagolás van használatban.
            </span>
          </div>
        )}

        {errorMsg && (
          <div
            role="alert"
            className="mt-4 flex flex-wrap items-center justify-between gap-3 rounded-lg border border-red-500/50 bg-red-950/60 px-4 py-3 text-sm text-red-200"
          >
            <div className="flex items-center gap-2.5">
              <AlertTriangle className="h-4 w-4 shrink-0 text-red-400" aria-hidden="true" />
              <span>{errorMsg}</span>
            </div>
            <button
              type="button"
              onClick={() => setManualTick((t) => t + 1)}
              className="min-h-[40px] rounded-md border border-red-400/40 bg-red-900/50 px-3 py-1.5 text-xs font-semibold text-red-100 whitespace-nowrap shrink-0 transition-colors hover:bg-red-900"
            >
              Újrapróbálás
            </button>
          </div>
        )}
      </section>

      {/* Betöltés / Üres állapot */}
      {loading && samples.length === 0 ? (
        <div className="rounded-xl border border-slate-800 bg-slate-900 p-8 text-center text-sm text-slate-400">
          Grafikon adatok betöltése...
        </div>
      ) : samples.length === 0 && !errorMsg ? (
        <div className="rounded-xl border border-slate-800 bg-slate-900 p-8 text-center">
          <h3 className="text-base font-semibold text-slate-100">
            Még nincs adat - várd meg az első mintát
          </h3>
          <p className="mt-1.5 text-xs text-slate-400">
            A backend minden 60. másodpercben rögzít egy idősor-mintát az ESP32-től érkező állapotüzenetekből.
          </p>
        </div>
      ) : (
        <>
          {/* Kombinált grafikon: Összes érzékelő együtt */}
          <section
            aria-labelledby="combined-chart-title"
            className="rounded-xl border border-slate-800 bg-slate-900 p-5 sm:p-6"
          >
            <div className="border-b border-slate-800/80 pb-3.5">
              <h3 id="combined-chart-title" className="text-base font-semibold text-slate-100">
                Összes érzékelő együtt
              </h3>
              <p className="mt-0.5 text-xs text-slate-400">
                Kattints az alsó jelmagyarázatban egy érzékelőre a vonal ki/be kapcsolásához
              </p>
            </div>

            <div className="mt-4 h-72 w-full sm:h-80">
              {!anyCombinedVisible ? (
                <div className="flex h-full items-center justify-center rounded-lg border border-dashed border-slate-800 text-xs text-slate-400">
                  Minden érzékelő ki van kapcsolva — válassz legalább egyet az alábbi jelmagyarázatból
                </div>
              ) : !hasCombinedValidPoints ? (
                <div className="flex h-full items-center justify-center rounded-lg border border-dashed border-slate-800 text-xs text-slate-400">
                  Nincs adat erre az időszakra
                </div>
              ) : (
                <ResponsiveContainer width="100%" height="100%">
                  <LineChart data={samples} margin={{ top: 10, right: 14, left: -8, bottom: 0 }}>
                    <CartesianGrid stroke="#1e293b" strokeDasharray="3 3" vertical={false} />
                    <XAxis
                      dataKey="timestamp"
                      tickFormatter={(val) =>
                        typeof val === 'string' ? formatChartAxisTime(val, range) : ''
                      }
                      stroke="#475569"
                      tick={{ fill: '#94a3b8', fontSize: 11 }}
                      minTickGap={32}
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
                        <CombinedSensorsTooltip
                          active={tooltipProps.active}
                          payload={tooltipProps.payload}
                          label={tooltipProps.label}
                          visibleSensors={visibleSensors}
                        />
                      )}
                    />
                    {SENSOR_SERIES.map((series) =>
                      visibleSensors[series.key] ? (
                        <Line
                          key={series.key}
                          type="monotone"
                          dataKey={series.key}
                          name={series.label}
                          stroke={series.color}
                          strokeWidth={2}
                          dot={false}
                          activeDot={{
                            r: 4,
                            fill: series.color,
                            stroke: '#0f172a',
                            strokeWidth: 2,
                          }}
                          connectNulls={false}
                          isAnimationActive={false}
                        />
                      ) : null
                    )}
                  </LineChart>
                </ResponsiveContainer>
              )}
            </div>

            {/* Kattintható legenda a grafikon alatt */}
            <div
              role="group"
              aria-label="Érzékelő vonalak ki- és bekapcsolása"
              className="mt-4 flex flex-wrap items-center justify-center gap-2 border-t border-slate-800/80 pt-3.5"
            >
              {SENSOR_SERIES.map((series) => {
                const visible = visibleSensors[series.key];
                return (
                  <button
                    key={series.key}
                    type="button"
                    aria-pressed={visible}
                    onClick={() => handleToggleSensor(series.key)}
                    className={`inline-flex min-h-[36px] items-center gap-2 rounded-lg border px-3 py-1.5 text-xs font-medium whitespace-nowrap shrink-0 transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-orange-500 ${
                      visible
                        ? 'border-slate-700 bg-slate-800/90 text-slate-100 hover:bg-slate-700'
                        : 'border-slate-800 bg-slate-950/60 text-slate-500 line-through hover:text-slate-300'
                    }`}
                  >
                    <span
                      className="inline-block h-1.5 w-4 rounded-full"
                      style={{
                        backgroundColor: series.color,
                        opacity: visible ? 1 : 0.3,
                      }}
                      aria-hidden="true"
                    />
                    <span>{series.label}</span>
                  </button>
                );
              })}
            </div>
          </section>

          {/* 5 külön grafikon */}
          <div className="grid grid-cols-1 gap-6 lg:grid-cols-2">
            {SENSOR_SERIES.map((series, index) => (
              <div
                key={series.key}
                className={index === SENSOR_SERIES.length - 1 ? 'lg:col-span-2' : ''}
              >
                <TempChart
                  title={series.label}
                  subtitle={series.subtitle}
                  sensorKey={series.key}
                  color={series.color}
                  samples={samples}
                  range={range}
                  liveValue={getLiveSensorValue(liveStatus, series.key)}
                />
              </div>
            ))}
          </div>
        </>
      )}
    </div>
  );
}
