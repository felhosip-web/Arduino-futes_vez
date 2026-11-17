import { useCallback, useEffect, useRef, useState } from 'react';
import {
  AlertTriangle,
  ChevronLeft,
  ChevronRight,
  Download,
  RefreshCw,
  ScrollText,
  Trash2,
} from 'lucide-react';
import { API_URL, DEVICE_ID } from '../config';
import type {
  LogEntry,
  LogLevel,
  LogLevelFilter,
  LogsQueryResponse,
} from '../types/logs';

interface LogsViewProps {
  refreshSignal?: number;
}

const LEVEL_OPTIONS: ReadonlyArray<{ value: LogLevelFilter; label: string }> = [
  { value: 'ALL', label: 'MIND' },
  { value: 'INFO', label: 'INFO' },
  { value: 'WARN', label: 'WARN' },
  { value: 'ERROR', label: 'ERROR' },
];

const DEFAULT_LIMIT = 20;
const AUTO_REFRESH_MS = 10_000;

function getLevelBadgeClass(level: LogLevel): string {
  switch (level) {
    case 'ERROR':
      return 'border-red-500 bg-red-950/80 text-red-200 animate-pulse';
    case 'WARN':
      return 'border-orange-500/60 bg-orange-950/60 text-orange-300';
    case 'INFO':
    default:
      return 'border-slate-600 bg-slate-800 text-slate-300';
  }
}

export function LogsView({ refreshSignal = 0 }: LogsViewProps) {
  const [logs, setLogs] = useState<LogEntry[]>([]);
  const [levelFilter, setLevelFilter] = useState<LogLevelFilter>('ALL');
  const [page, setPage] = useState<number>(1);
  const [total, setTotal] = useState<number>(0);
  const [pages, setPages] = useState<number>(1);
  const [loading, setLoading] = useState<boolean>(true);
  const [refreshing, setRefreshing] = useState<boolean>(false);
  const [exporting, setExporting] = useState<boolean>(false);
  const [clearing, setClearing] = useState<boolean>(false);
  const [showClearModal, setShowClearModal] = useState<boolean>(false);
  const [errorMsg, setErrorMsg] = useState<string | null>(null);
  const [deletedNotice, setDeletedNotice] = useState<boolean>(false);
  const [manualTick, setManualTick] = useState<number>(0);

  const deletedTimerRef = useRef<number | null>(null);
  const hasLoadedOnceRef = useRef<boolean>(false);

  useEffect(() => {
    return () => {
      if (deletedTimerRef.current !== null) {
        window.clearTimeout(deletedTimerRef.current);
      }
    };
  }, []);

  useEffect(() => {
    if (!showClearModal) return;
    const handleKeyDown = (e: KeyboardEvent) => {
      if (e.key === 'Escape' && !clearing) {
        setShowClearModal(false);
      }
    };
    window.addEventListener('keydown', handleKeyDown);
    return () => {
      window.removeEventListener('keydown', handleKeyDown);
    };
  }, [showClearModal, clearing]);

  const fetchLogs = useCallback(
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
          page: String(page),
          limit: String(DEFAULT_LIMIT),
        });
        if (levelFilter !== 'ALL') {
          params.set('level', levelFilter);
        }

        const res = await fetch(
          `${API_URL}/api/heating/${DEVICE_ID}/logs?${params.toString()}`,
          { signal: options?.signal }
        );

        if (!res.ok) {
          const body = (await res.json().catch(() => null)) as { error?: string } | null;
          if (res.status === 401) {
            throw new Error(body?.error || 'Nincs jogosultság a naplók lekérdezéséhez (401).');
          }
          throw new Error(body?.error || `Napló lekérdezési hiba (HTTP ${res.status})`);
        }

        const data = (await res.json()) as LogsQueryResponse;
        hasLoadedOnceRef.current = true;
        setLogs(Array.isArray(data.logs) ? data.logs : []);
        setTotal(typeof data.total === 'number' ? data.total : 0);
        const safePages = typeof data.pages === 'number' && data.pages >= 1 ? data.pages : 1;
        setPages(safePages);
      } catch (err) {
        if (err instanceof DOMException && err.name === 'AbortError') {
          return;
        }
        const message =
          err instanceof Error ? err.message : 'Nem sikerült betölteni a naplókat.';
        setErrorMsg(message);
      } finally {
        if (!options?.signal?.aborted) {
          setLoading(false);
          setRefreshing(false);
        }
      }
    },
    [levelFilter, page]
  );

  // Kezdeti betöltés, lapozás, szűrés vagy kézi frissítés
  useEffect(() => {
    const controller = new AbortController();
    void fetchLogs({ signal: controller.signal });
    return () => {
      controller.abort();
    };
  }, [fetchLogs, manualTick, refreshSignal]);

  // 10 másodpercenkénti automatikus frissítés az aktuális oldal megtartásával
  useEffect(() => {
    const controller = new AbortController();
    const timer = window.setInterval(() => {
      void fetchLogs({ signal: controller.signal, silent: true });
    }, AUTO_REFRESH_MS);

    return () => {
      window.clearInterval(timer);
      controller.abort();
    };
  }, [fetchLogs]);

  const handleSelectLevel = (nextLevel: LogLevelFilter) => {
    if (nextLevel === levelFilter) return;
    setPage(1);
    setLevelFilter(nextLevel);
  };

  const handleRefresh = () => {
    setManualTick((prev) => prev + 1);
  };

  const handleExport = async () => {
    setExporting(true);
    setErrorMsg(null);
    try {
      const res = await fetch(`${API_URL}/api/heating/${DEVICE_ID}/logs/export`);
      if (!res.ok) {
        const body = (await res.json().catch(() => null)) as { error?: string } | null;
        throw new Error(body?.error || `Exportálási hiba (HTTP ${res.status})`);
      }
      const text = await res.text();
      const blob = new Blob([text], { type: 'text/plain;charset=utf-8' });
      const downloadUrl = URL.createObjectURL(blob);
      const anchor = document.createElement('a');
      anchor.href = downloadUrl;
      anchor.download = `heating-logs-${DEVICE_ID}.txt`;
      document.body.appendChild(anchor);
      anchor.click();
      anchor.remove();
      URL.revokeObjectURL(downloadUrl);
    } catch (err) {
      const message =
        err instanceof Error ? err.message : 'Nem sikerült letölteni a naplófájlt.';
      setErrorMsg(message);
    } finally {
      setExporting(false);
    }
  };

  const handleConfirmClear = async () => {
    setClearing(true);
    setErrorMsg(null);
    try {
      const res = await fetch(`${API_URL}/api/heating/${DEVICE_ID}/logs`, {
        method: 'DELETE',
      });
      if (!res.ok) {
        const body = (await res.json().catch(() => null)) as { error?: string } | null;
        throw new Error(body?.error || `Napló törlési hiba (HTTP ${res.status})`);
      }
      setShowClearModal(false);
      setPage(1);
      setDeletedNotice(true);
      if (deletedTimerRef.current !== null) {
        window.clearTimeout(deletedTimerRef.current);
      }
      deletedTimerRef.current = window.setTimeout(() => {
        deletedTimerRef.current = null;
        setDeletedNotice(false);
      }, 2000);
      setManualTick((prev) => prev + 1);
    } catch (err) {
      const message =
        err instanceof Error ? err.message : 'Nem sikerült törölni a naplókat.';
      setErrorMsg(message);
      setShowClearModal(false);
    } finally {
      setClearing(false);
    }
  };

  const hasPrev = page > 1;
  const hasNext = page < pages;

  return (
    <section
      aria-labelledby="logs-view-title"
      className="rounded-xl border border-slate-800 bg-slate-900 p-5 sm:p-6"
    >
      {/* Fejléc + Export + Frissítés + Piros Napló törlése gomb */}
      <div className="flex flex-wrap items-center justify-between gap-4 border-b border-slate-800/80 pb-4">
        <div className="flex items-start gap-3">
          <ScrollText className="mt-0.5 h-5 w-5 shrink-0 text-orange-400" aria-hidden="true" />
          <div>
            <h2 id="logs-view-title" className="text-lg font-semibold text-slate-100">
              Esemény és hiba napló
            </h2>
            <p className="mt-0.5 text-xs text-slate-400 tabular-nums">
              Automatikus frissítés 10 mp-enként · {total} bejegyzés
            </p>
          </div>
        </div>

        <div className="flex flex-wrap items-center gap-2">
          <button
            type="button"
            onClick={handleRefresh}
            disabled={refreshing}
            className="inline-flex min-h-[44px] items-center gap-2 rounded-lg border border-slate-700 bg-slate-800 px-3.5 py-2 text-xs font-semibold text-slate-200 whitespace-nowrap shrink-0 transition-colors hover:bg-slate-700 disabled:opacity-60 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-orange-500 sm:text-sm"
          >
            <RefreshCw
              className={`h-4 w-4 ${refreshing ? 'animate-spin' : ''}`}
              aria-hidden="true"
            />
            <span>Frissítés</span>
          </button>

          <button
            type="button"
            onClick={() => void handleExport()}
            disabled={exporting}
            className="inline-flex min-h-[44px] items-center gap-2 rounded-lg border border-slate-700 bg-slate-800 px-3.5 py-2 text-xs font-semibold text-slate-200 whitespace-nowrap shrink-0 transition-colors hover:bg-slate-700 disabled:opacity-60 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-orange-500 sm:text-sm"
          >
            <Download className="h-4 w-4" aria-hidden="true" />
            <span>{exporting ? 'Letöltés...' : 'Letöltés .txt'}</span>
          </button>

          <button
            type="button"
            onClick={() => setShowClearModal(true)}
            disabled={clearing}
            className="inline-flex min-h-[44px] items-center gap-2 rounded-lg border border-red-500 bg-red-600 px-3.5 py-2 text-xs font-semibold text-white whitespace-nowrap shrink-0 transition-colors hover:bg-red-500 disabled:opacity-60 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-red-400 sm:text-sm"
          >
            <Trash2 className="h-4 w-4" aria-hidden="true" />
            <span>Napló törlése</span>
          </button>
        </div>
      </div>

      {/* Szűrő gombok: MIND | INFO | WARN | ERROR */}
      <div className="mt-4 flex flex-wrap items-center justify-between gap-3 border-b border-slate-800/80 pb-4">
        <div
          role="group"
          aria-label="Naplószint szűrő"
          className="flex flex-wrap items-center gap-2"
        >
          {LEVEL_OPTIONS.map((opt) => {
            const active = levelFilter === opt.value;
            return (
              <button
                key={opt.value}
                type="button"
                aria-pressed={active}
                onClick={() => handleSelectLevel(opt.value)}
                className={`min-h-[44px] rounded-lg border px-4 py-2 text-xs font-semibold whitespace-nowrap shrink-0 transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-orange-500 sm:text-sm ${
                  active
                    ? 'border-orange-500 bg-orange-500 text-slate-950'
                    : 'border-slate-700 bg-slate-800 text-slate-300 hover:bg-slate-700 hover:text-slate-100'
                }`}
              >
                {opt.label}
              </button>
            );
          })}
        </div>
      </div>

      {/* Napló törölve visszajelzés (2 mp-ig) */}
      {deletedNotice && (
        <div
          role="status"
          className="mt-4 rounded-lg border border-emerald-500/50 bg-emerald-950/50 px-4 py-3 text-sm font-medium text-emerald-200"
        >
          Napló törölve
        </div>
      )}

      {/* Hibajelzés */}
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
            onClick={handleRefresh}
            className="min-h-[40px] rounded-md border border-red-400/40 bg-red-900/50 px-3 py-1.5 text-xs font-semibold text-red-100 whitespace-nowrap shrink-0 transition-colors hover:bg-red-900"
          >
            Újrapróbálás
          </button>
        </div>
      )}

      {/* Naplólista */}
      <div className="mt-4">
        {loading && logs.length === 0 ? (
          <div className="space-y-2" aria-busy="true" aria-label="Naplók betöltése">
            {Array.from({ length: 6 }, (_, idx) => (
              <div
                key={idx}
                className="h-10 animate-pulse rounded-md border border-slate-800/70 bg-slate-950/70"
              />
            ))}
          </div>
        ) : logs.length === 0 ? (
          <div className="rounded-lg border border-slate-800/80 bg-slate-950/60 px-6 py-10 text-center">
            <p className="text-sm font-medium text-slate-200">
              Nincs naplóbejegyzés - minden rendben.
            </p>
            {levelFilter !== 'ALL' && (
              <button
                type="button"
                onClick={() => handleSelectLevel('ALL')}
                className="mt-4 inline-flex min-h-[44px] items-center rounded-lg bg-orange-500 px-4 py-2 text-xs font-semibold text-slate-950 whitespace-nowrap shrink-0 transition-colors hover:bg-orange-400 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-orange-500"
              >
                Szűrő visszaállítása (MIND)
              </button>
            )}
          </div>
        ) : (
          <ul
            aria-label="Naplóbejegyzések"
            className="divide-y divide-slate-800/80 rounded-lg border border-slate-800/80 bg-slate-950/60"
          >
            {logs.map((entry) => (
              <li
                key={`${entry.id}-${entry.timestamp}`}
                className="flex flex-wrap items-baseline gap-2 px-3.5 py-2.5 font-mono text-xs leading-relaxed transition-colors hover:bg-slate-800/35 sm:text-sm"
              >
                <span className="tabular-nums text-slate-400 whitespace-nowrap">
                  [{entry.timestamp}]
                </span>
                <span
                  className={`inline-block rounded border px-1.5 py-0.5 text-[11px] font-semibold leading-none whitespace-nowrap ${getLevelBadgeClass(
                    entry.level
                  )}`}
                >
                  [{entry.level}]
                </span>
                <span className="text-slate-100 break-words">
                  <strong className="font-semibold text-slate-50">{entry.code}</strong>
                  {' - '}
                  <span className="text-slate-300">{entry.message || '—'}</span>
                </span>
              </li>
            ))}
          </ul>
        )}
      </div>

      {/* Lapozás: Előző / Következő gombok + "1 / 7. oldal (123 bejegyzés)" */}
      <div className="mt-4 flex flex-wrap items-center justify-between gap-3 border-t border-slate-800/80 pt-4">
        <div className="text-xs font-medium tabular-nums text-slate-300 sm:text-sm">
          {page} / {pages}. oldal ({total} bejegyzés)
        </div>

        <div className="flex items-center gap-2">
          <button
            type="button"
            onClick={() => setPage((p) => Math.max(1, p - 1))}
            disabled={!hasPrev || loading}
            className="inline-flex min-h-[44px] items-center gap-1.5 rounded-lg border border-slate-700 bg-slate-800 px-3.5 py-2 text-xs font-semibold text-slate-200 whitespace-nowrap shrink-0 transition-colors hover:bg-slate-700 disabled:cursor-not-allowed disabled:opacity-45 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-orange-500 sm:text-sm"
          >
            <ChevronLeft className="h-4 w-4" aria-hidden="true" />
            <span>Előző</span>
          </button>

          <button
            type="button"
            onClick={() => setPage((p) => Math.min(pages, p + 1))}
            disabled={!hasNext || loading}
            className="inline-flex min-h-[44px] items-center gap-1.5 rounded-lg border border-slate-700 bg-slate-800 px-3.5 py-2 text-xs font-semibold text-slate-200 whitespace-nowrap shrink-0 transition-colors hover:bg-slate-700 disabled:cursor-not-allowed disabled:opacity-45 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-orange-500 sm:text-sm"
          >
            <span>Következő</span>
            <ChevronRight className="h-4 w-4" aria-hidden="true" />
          </button>
        </div>
      </div>

      {/* Megerősítő felugró ablak törléshez */}
      {showClearModal && (
        <div
          className="fixed inset-0 z-50 flex items-center justify-center bg-slate-950/80 p-4 backdrop-blur-xs"
          onClick={() => {
            if (!clearing) setShowClearModal(false);
          }}
        >
          <div
            role="dialog"
            aria-modal="true"
            aria-labelledby="clear-logs-dialog-title"
            className="w-full max-w-md rounded-xl border border-slate-700 bg-slate-900 p-6 shadow-xl"
            onClick={(e) => e.stopPropagation()}
          >
            <div className="flex items-start gap-3">
              <AlertTriangle className="mt-0.5 h-5 w-5 shrink-0 text-red-400" aria-hidden="true" />
              <div>
                <h3 id="clear-logs-dialog-title" className="text-base font-semibold text-slate-100">
                  Napló törlése
                </h3>
                <p className="mt-2 text-sm leading-relaxed text-slate-300">
                  Biztosan törlöd az összes naplóbejegyzést? Ez nem vonható vissza.
                </p>
              </div>
            </div>

            <div className="mt-6 flex items-center justify-end gap-3">
              <button
                type="button"
                onClick={() => setShowClearModal(false)}
                disabled={clearing}
                className="min-h-[44px] rounded-lg border border-slate-700 bg-slate-800 px-4 py-2 text-sm font-semibold text-slate-200 whitespace-nowrap shrink-0 transition-colors hover:bg-slate-700 disabled:opacity-60 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-orange-500"
              >
                Mégse
              </button>
              <button
                type="button"
                onClick={() => void handleConfirmClear()}
                disabled={clearing}
                className="min-h-[44px] rounded-lg bg-red-600 px-5 py-2 text-sm font-semibold text-white whitespace-nowrap shrink-0 transition-colors hover:bg-red-500 disabled:opacity-60 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-red-400"
              >
                {clearing ? 'Törlés...' : 'IGEN'}
              </button>
            </div>
          </div>
        </div>
      )}
    </section>
  );
}
