import { useCallback, useEffect, useRef, useState } from 'react';
import {
  Activity,
  Antenna,
  RefreshCw,
  ShieldAlert,
  ShieldCheck,
  Wifi,
  WifiOff,
} from 'lucide-react';
import { API_URL } from '../config';
import type { MqttBrokerStats, MqttSignalQuality } from '../types/heating';

interface HeaderProps {
  socketConnected: boolean;
  lastUpdatedAt: number | null;
  heartbeatIntervalSec: number;
  alarmActive: boolean;
  isRefreshing: boolean;
  onRefresh: () => void;
  mqttStats?: MqttBrokerStats | null;
}

const QUALITY_STYLES: Record<
  MqttSignalQuality,
  {
    label: string;
    textClass: string;
    iconClass: string;
    borderClass: string;
    bgClass: string;
    barClass: string;
  }
> = {
  excellent: {
    label: 'Kiváló',
    textClass: 'text-emerald-300',
    iconClass: 'text-emerald-400',
    borderClass: 'border-emerald-500/40',
    bgClass: 'bg-emerald-950/30',
    barClass: 'bg-emerald-400',
  },
  good: {
    label: 'Jó',
    textClass: 'text-lime-300',
    iconClass: 'text-lime-400',
    borderClass: 'border-lime-500/40',
    bgClass: 'bg-lime-950/20',
    barClass: 'bg-lime-400',
  },
  fair: {
    label: 'Közepes',
    textClass: 'text-amber-300',
    iconClass: 'text-amber-400',
    borderClass: 'border-amber-500/50',
    bgClass: 'bg-amber-950/30',
    barClass: 'bg-amber-400',
  },
  poor: {
    label: 'Gyenge',
    textClass: 'text-red-300',
    iconClass: 'text-red-400',
    borderClass: 'border-red-500/50',
    bgClass: 'bg-red-950/40',
    barClass: 'bg-red-400',
  },
  offline: {
    label: 'Offline',
    textClass: 'text-slate-400',
    iconClass: 'text-slate-500',
    borderClass: 'border-slate-700',
    bgClass: 'bg-slate-900',
    barClass: 'bg-slate-600',
  },
};

function deriveMqttQualityFromStats(stats: MqttBrokerStats | null): {
  quality: MqttSignalQuality;
  signalLevel: 0 | 1 | 2 | 3 | 4;
  reachable: boolean;
} {
  if (!stats || !stats.connected) {
    return { quality: 'offline', signalLevel: 0, reachable: false };
  }

  const latency = stats.latencyMs;
  const reliability = stats.reliabilityPct;

  if (
    (reliability !== null && reliability < 80) ||
    (latency !== null && latency >= 600)
  ) {
    return { quality: 'poor', signalLevel: 1, reachable: true };
  }
  if (
    (reliability !== null && reliability < 95) ||
    (latency !== null && latency >= 250)
  ) {
    return { quality: 'fair', signalLevel: 2, reachable: true };
  }
  if (latency !== null && latency >= 100) {
    return { quality: 'good', signalLevel: 3, reachable: true };
  }
  return { quality: 'excellent', signalLevel: 4, reachable: true };
}

function formatTimestamp(ms: number | null): string {
  if (!ms) return 'Nincs adat';
  return new Date(ms).toLocaleTimeString('hu-HU', {
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
  });
}

function SignalBarsIcon({
  level,
  activeBarClass,
}: {
  level: 0 | 1 | 2 | 3 | 4;
  activeBarClass: string;
}) {
  const heights = ['h-1.5', 'h-2.5', 'h-3.5', 'h-4'];
  return (
    <span className="inline-flex h-4 items-end gap-0.5" aria-hidden="true">
      {heights.map((heightClass, idx) => {
        const active = idx < level;
        return (
          <span
            key={heightClass}
            className={`w-1 rounded-[1px] transition-colors ${heightClass} ${
              active ? activeBarClass : 'bg-slate-700/80'
            }`}
          />
        );
      })}
    </span>
  );
}

function MqttConnectionIcon({
  reachable,
  configured,
  signalLevel,
  iconClass,
  barClass,
}: {
  reachable: boolean;
  configured: boolean;
  signalLevel: 0 | 1 | 2 | 3 | 4;
  iconClass: string;
  barClass: string;
}) {
  if (!reachable) {
    return (
      <span className="inline-flex items-center gap-1.5" aria-hidden="true">
        <WifiOff className={`h-4 w-4 ${configured ? 'text-red-400' : 'text-slate-500'}`} />
        <SignalBarsIcon level={0} activeBarClass={barClass} />
      </span>
    );
  }
  return (
    <span className="inline-flex items-center gap-1.5" aria-hidden="true">
      <Antenna className={`h-4 w-4 ${iconClass}`} />
      <SignalBarsIcon level={signalLevel} activeBarClass={barClass} />
    </span>
  );
}

export function Header({
  socketConnected,
  lastUpdatedAt,
  heartbeatIntervalSec,
  alarmActive,
  isRefreshing,
  onRefresh,
  mqttStats,
}: HeaderProps) {
  const [fetchedStats, setFetchedStats] = useState<MqttBrokerStats | null>(null);
  const [fetchingStats, setFetchingStats] = useState<boolean>(false);
  const [detailsOpen, setDetailsOpen] = useState<boolean>(false);
  const popoverRef = useRef<HTMLDivElement | null>(null);

  const fetchMqttStats = useCallback(async () => {
    setFetchingStats(true);
    try {
      const res = await fetch(`${API_URL}/api/mqtt/stats`);
      if (!res.ok) return;
      const data = (await res.json()) as MqttBrokerStats;
      setFetchedStats(data);
    } catch {
      // Hálózati hiba esetén megtartjuk az utolsó ismert MQTT statisztikát
    } finally {
      setFetchingStats(false);
    }
  }, []);

  useEffect(() => {
    if (mqttStats) {
      setFetchedStats(mqttStats);
    }
  }, [mqttStats]);

  useEffect(() => {
    void fetchMqttStats();
    const timer = window.setInterval(() => {
      void fetchMqttStats();
    }, 15_000);
    return () => window.clearInterval(timer);
  }, [fetchMqttStats]);

  useEffect(() => {
    if (!detailsOpen) return;
    const handlePointerDown = (event: MouseEvent) => {
      if (popoverRef.current && !popoverRef.current.contains(event.target as Node)) {
        setDetailsOpen(false);
      }
    };
    const handleKeyDown = (event: KeyboardEvent) => {
      if (event.key === 'Escape') {
        setDetailsOpen(false);
      }
    };
    window.addEventListener('mousedown', handlePointerDown);
    window.addEventListener('keydown', handleKeyDown);
    return () => {
      window.removeEventListener('mousedown', handlePointerDown);
      window.removeEventListener('keydown', handleKeyDown);
    };
  }, [detailsOpen]);

  const stats = mqttStats ?? fetchedStats ?? null;
  const { quality, signalLevel, reachable } = deriveMqttQualityFromStats(stats);
  const isConfigured = Boolean(stats?.configured);
  const isUnconfiguredOffline = stats !== null && !stats.configured;
  const style =
    quality === 'offline' && isConfigured
      ? {
          ...QUALITY_STYLES.offline,
          textClass: 'text-red-300',
          iconClass: 'text-red-400',
          borderClass: 'border-red-500/50',
          bgClass: 'bg-red-950/30',
        }
      : QUALITY_STYLES[quality];

  const reliabilityPct = stats?.reliabilityPct ?? null;
  const reliabilityHealthy = reliabilityPct === null || reliabilityPct >= 95;
  const latencyLabel = !stats
    ? 'MQTT...'
    : !stats.configured
      ? 'Nincs broker'
      : !reachable
        ? 'Nem elérhető'
        : stats.latencyMs !== null
          ? `${stats.latencyMs} ms`
          : 'Elérhető';

  return (
    <header className="flex flex-wrap items-center justify-between gap-4 border-b border-slate-800 pb-4">
      <div>
        <h1 className="text-xl font-bold tracking-tight text-slate-50 sm:text-2xl">
          Fűtésvezérlés
        </h1>
        <div className="mt-1 flex flex-wrap items-center gap-2 text-xs text-slate-400">
          <span className="inline-flex items-center gap-1.5">
            {socketConnected ? (
              <Wifi className="h-3.5 w-3.5 text-emerald-400" aria-hidden="true" />
            ) : (
              <WifiOff className="h-3.5 w-3.5 text-slate-500" aria-hidden="true" />
            )}
            <span>{socketConnected ? 'Élő kapcsolat' : 'Újracsatlakozás...'}</span>
          </span>
          <span aria-hidden="true">·</span>
          <span className="tabular-nums">Frissítve: {formatTimestamp(lastUpdatedAt)}</span>
          <span aria-hidden="true">·</span>
          <span className="tabular-nums">Ping: {heartbeatIntervalSec}s</span>
        </div>
      </div>

      <div className="flex flex-wrap items-center gap-3">
        {alarmActive && (
          <div
            role="alert"
            className="inline-flex min-h-[44px] items-center gap-2 rounded-lg border border-red-500 bg-red-950/80 px-3.5 py-2 text-xs font-bold text-red-100 whitespace-nowrap shrink-0 animate-pulse"
          >
            <span className="inline-block h-2.5 w-2.5 rounded-full bg-red-500" aria-hidden="true" />
            <span>RIASZTÁS</span>
          </div>
        )}

        {/* MQTT Broker jelerősség, késleltetés és elérés jelző (mqttStats alapján) */}
        <div className="relative" ref={popoverRef}>
          <button
            type="button"
            onClick={() => setDetailsOpen((prev) => !prev)}
            aria-expanded={detailsOpen}
            aria-label="MQTT kapcsolat minősége és késleltetése"
            title={
              stats
                ? `MQTT ${style.label} · Elérés: ${reachable ? 'Elérhető' : 'Nem elérhető'} · Késleltetés: ${latencyLabel} · Megbízhatóság: ${
                    reliabilityPct !== null ? `${reliabilityPct}%` : '—'
                  }`
                : 'MQTT statisztikák betöltése...'
            }
            className={`inline-flex min-h-[44px] items-center gap-2.5 rounded-lg border px-3 py-1.5 text-left transition-colors hover:bg-slate-800/80 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-orange-500 ${style.borderClass} ${style.bgClass}`}
          >
            <MqttConnectionIcon
              reachable={reachable}
              configured={isConfigured}
              signalLevel={signalLevel}
              iconClass={style.iconClass}
              barClass={style.barClass}
            />
            <div className="leading-tight">
              <div className="flex items-center gap-1.5 text-xs font-semibold whitespace-nowrap">
                <span className="text-slate-300">MQTT</span>
                <span className={`font-mono tabular-nums ${style.textClass}`}>{latencyLabel}</span>
              </div>
              <div className="mt-0.5 flex items-center gap-1 text-[11px] text-slate-400 tabular-nums whitespace-nowrap">
                {reliabilityHealthy ? (
                  <ShieldCheck
                    className={`h-3 w-3 ${
                      isUnconfiguredOffline || !reachable
                        ? 'text-slate-500'
                        : 'text-emerald-400'
                    }`}
                    aria-hidden="true"
                  />
                ) : (
                  <ShieldAlert className="h-3 w-3 text-amber-400" aria-hidden="true" />
                )}
                <span>{reliabilityPct !== null ? `${reliabilityPct}% megbíz.` : 'Nincs adat'}</span>
              </div>
            </div>
          </button>

          {detailsOpen && (
            <div
              role="dialog"
              aria-label="MQTT kapcsolat részletei"
              className="absolute right-0 z-40 mt-2 w-72 rounded-xl border border-slate-700 bg-slate-900 p-4 shadow-xl sm:w-80"
            >
              <div className="flex items-center justify-between gap-2 border-b border-slate-800 pb-2.5">
                <div className="flex items-center gap-2">
                  <MqttConnectionIcon
                    reachable={reachable}
                    configured={isConfigured}
                    signalLevel={signalLevel}
                    iconClass={style.iconClass}
                    barClass={style.barClass}
                  />
                  <div>
                    <p className="text-xs font-semibold uppercase tracking-wide text-slate-300">
                      MQTT Broker
                    </p>
                    <p className={`text-xs font-semibold ${style.textClass}`}>
                      {!stats?.configured
                        ? 'Broker nincs beállítva (.env)'
                        : reachable
                          ? `Elérhető · ${style.label}`
                          : 'Nem elérhető'}
                    </p>
                  </div>
                </div>
                <button
                  type="button"
                  onClick={() => void fetchMqttStats()}
                  disabled={fetchingStats}
                  className="inline-flex h-8 w-8 items-center justify-center rounded-md border border-slate-700 bg-slate-950 text-slate-300 transition-colors hover:bg-slate-800 disabled:opacity-60"
                  title="MQTT statisztika lekérdezése"
                >
                  <RefreshCw
                    className={`h-3.5 w-3.5 ${fetchingStats ? 'animate-spin' : ''}`}
                    aria-hidden="true"
                  />
                </button>
              </div>

              <div className="mt-3 grid grid-cols-2 gap-2 text-xs">
                <div className="rounded-lg border border-slate-800 bg-slate-950/70 p-2.5">
                  <span className="text-[11px] text-slate-400">Aktuális késleltetés</span>
                  <p className={`mt-0.5 font-mono text-sm font-semibold tabular-nums ${style.textClass}`}>
                    {stats?.latencyMs !== null && stats?.latencyMs !== undefined
                      ? `${stats.latencyMs} ms`
                      : '—'}
                  </p>
                </div>
                <div className="rounded-lg border border-slate-800 bg-slate-950/70 p-2.5">
                  <span className="text-[11px] text-slate-400">Átlag (Min / Max)</span>
                  <p className="mt-0.5 font-mono text-sm font-semibold text-slate-200 tabular-nums">
                    {stats?.avgLatencyMs !== null && stats?.avgLatencyMs !== undefined
                      ? `${stats.avgLatencyMs} ms`
                      : '—'}
                  </p>
                  {stats?.minLatencyMs !== null &&
                    stats?.minLatencyMs !== undefined &&
                    stats?.maxLatencyMs !== null &&
                    stats?.maxLatencyMs !== undefined && (
                      <p className="text-[10px] text-slate-500 tabular-nums">
                        {stats.minLatencyMs}–{stats.maxLatencyMs} ms
                      </p>
                    )}
                </div>
              </div>

              <div className="mt-3 rounded-lg border border-slate-800 bg-slate-950/70 p-2.5">
                <div className="flex items-center justify-between text-xs">
                  <span className="inline-flex items-center gap-1.5 text-slate-300">
                    <Activity className="h-3.5 w-3.5 text-slate-400" aria-hidden="true" />
                    Kézbesítési megbízhatóság
                  </span>
                  <span className={`font-mono font-semibold tabular-nums ${style.textClass}`}>
                    {reliabilityPct !== null ? `${reliabilityPct}%` : '—'}
                  </span>
                </div>
                <div className="mt-1.5 h-1.5 w-full overflow-hidden rounded-full bg-slate-800">
                  <div
                    className={`h-full transition-all ${
                      reliabilityPct === null
                        ? 'w-0 bg-slate-600'
                        : reliabilityPct >= 95
                          ? 'bg-emerald-400'
                          : reliabilityPct >= 80
                            ? 'bg-amber-400'
                            : 'bg-red-400'
                    }`}
                    style={{ width: `${reliabilityPct ?? 0}%` }}
                  />
                </div>
                <div className="mt-2 grid grid-cols-2 gap-x-3 gap-y-1 text-[11px] text-slate-400 tabular-nums">
                  <span>
                    QoS 1 PUBACK: {stats?.pubAckedCount ?? 0}/{stats?.publishedCount ?? 0}
                  </span>
                  <span>Sikertelen küldés: {stats?.pubFailedCount ?? 0}</span>
                  <span>CMD_ACK OK: {stats?.cmdAckSuccessCount ?? 0}</span>
                  <span>CMD_ACK hiba: {stats?.cmdAckFailCount ?? 0}</span>
                </div>
              </div>

              <div className="mt-2.5 flex items-center justify-between text-[11px] text-slate-500 tabular-nums">
                <span>Újracsatlakozások: {stats?.reconnectCount ?? 0}</span>
                <span>Mérve: {formatTimestamp(stats?.lastMeasuredAt ?? null)}</span>
              </div>

              {stats?.lastError && (
                <p className="mt-2 rounded border border-red-500/40 bg-red-950/40 px-2.5 py-1.5 text-[11px] text-red-200">
                  {stats.lastError}
                </p>
              )}
            </div>
          )}
        </div>

        <button
          type="button"
          onClick={() => {
            void fetchMqttStats();
            onRefresh();
          }}
          disabled={isRefreshing}
          className="inline-flex min-h-[44px] items-center gap-2 rounded-lg border border-slate-700 bg-slate-900 px-4 py-2 text-sm font-medium text-slate-200 whitespace-nowrap shrink-0 transition-colors hover:bg-slate-800 disabled:opacity-60 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-orange-500"
        >
          <RefreshCw
            className={`h-4 w-4 ${isRefreshing ? 'animate-spin' : ''}`}
            aria-hidden="true"
          />
          <span>Frissítés</span>
        </button>
      </div>
    </header>
  );
}
