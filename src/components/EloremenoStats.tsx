import { useEffect, useRef, useState } from 'react';
import { RotateCcw } from 'lucide-react';
import type { TrendStats } from '../types/heating';

interface EloremenoStatsProps {
  trend?: TrendStats;
  onResetStats: () => Promise<boolean>;
}

function formatTemp(val: number | null | undefined): string {
  if (val === null || val === undefined || Number.isNaN(val)) {
    return '---';
  }
  return `${val.toFixed(1)}°C`;
}

function formatSinceLabel(since: string | null | undefined): string {
  if (!since || !since.trim()) {
    return 'Boot óta';
  }
  const trimmed = since.trim();
  if (/óta$/i.test(trimmed)) {
    return trimmed;
  }
  return `${trimmed} óta`;
}

export function EloremenoStats({ trend, onResetStats }: EloremenoStatsProps) {
  const [confirmPending, setConfirmPending] = useState(false);
  const [resetting, setResetting] = useState(false);
  const [resetNotice, setResetNotice] = useState(false);

  const confirmTimerRef = useRef<number | null>(null);
  const noticeTimerRef = useRef<number | null>(null);

  useEffect(() => {
    return () => {
      if (confirmTimerRef.current !== null) {
        window.clearTimeout(confirmTimerRef.current);
      }
      if (noticeTimerRef.current !== null) {
        window.clearTimeout(noticeTimerRef.current);
      }
    };
  }, []);

  const handleResetClick = async () => {
    if (resetting) return;

    if (!confirmPending) {
      setConfirmPending(true);
      if (confirmTimerRef.current !== null) {
        window.clearTimeout(confirmTimerRef.current);
      }
      confirmTimerRef.current = window.setTimeout(() => {
        setConfirmPending(false);
        confirmTimerRef.current = null;
      }, 5000);
      return;
    }

    if (confirmTimerRef.current !== null) {
      window.clearTimeout(confirmTimerRef.current);
      confirmTimerRef.current = null;
    }
    setConfirmPending(false);
    setResetting(true);

    try {
      const ok = await onResetStats();
      if (ok) {
        setResetNotice(true);
        if (noticeTimerRef.current !== null) {
          window.clearTimeout(noticeTimerRef.current);
        }
        noticeTimerRef.current = window.setTimeout(() => {
          setResetNotice(false);
          noticeTimerRef.current = null;
        }, 2000);
      }
    } finally {
      setResetting(false);
    }
  };

  const maxText = formatTemp(trend?.max);
  const minText = formatTemp(trend?.min);
  const avgText = formatTemp(trend?.avg);
  const maxAtText =
    trend?.max !== null && trend?.max !== undefined && trend?.maxAt ? ` (${trend.maxAt}-kor)` : '';
  const minAtText =
    trend?.min !== null && trend?.min !== undefined && trend?.minAt ? ` (${trend.minAt}-kor)` : '';
  const sinceText = formatSinceLabel(trend?.since);

  return (
    <section
      aria-labelledby="eloremeno-stats-title"
      className="rounded-xl border border-slate-800 bg-slate-900 px-5 py-4 sm:px-6"
    >
      <div className="flex flex-wrap items-center justify-between gap-3">
        <div>
          <h2 id="eloremeno-stats-title" className="text-sm font-semibold text-slate-200">
            Előremenő statisztika
          </h2>
          <p className="mt-0.5 text-xs text-slate-400 tabular-nums">Mérés: {sinceText}</p>
        </div>

        <div className="flex items-center gap-2.5">
          {resetNotice && (
            <span
              role="status"
              className="text-xs font-medium text-emerald-400 whitespace-nowrap"
            >
              Statisztika nullázva
            </span>
          )}

          <button
            type="button"
            onClick={() => void handleResetClick()}
            disabled={resetting}
            className={`inline-flex min-h-[36px] items-center gap-1.5 rounded-lg border px-3 py-1.5 text-xs font-medium whitespace-nowrap shrink-0 transition-colors disabled:opacity-60 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-slate-400 ${
              confirmPending
                ? 'border-slate-500 bg-slate-700 text-slate-100 hover:bg-slate-600'
                : 'border-slate-700 bg-slate-800 text-slate-200 hover:bg-slate-700'
            }`}
          >
            <RotateCcw className={`h-3.5 w-3.5 ${resetting ? 'animate-spin' : ''}`} aria-hidden="true" />
            <span>
              {resetting
                ? 'Nullázás...'
                : confirmPending
                  ? 'Biztos? Kattints újra'
                  : 'Nullázás'}
            </span>
          </button>
        </div>
      </div>

      <div className="mt-3 grid grid-cols-1 gap-3 sm:grid-cols-3">
        <div className="rounded-lg border border-slate-800/80 bg-slate-950/60 px-3.5 py-2.5">
          <div className="text-xs text-slate-400">Max</div>
          <div className="mt-0.5 flex flex-wrap items-baseline gap-1.5 tabular-nums">
            <span className="text-base font-bold text-orange-400">{maxText}</span>
            {maxAtText && <span className="text-xs text-slate-400">{maxAtText}</span>}
          </div>
        </div>

        <div className="rounded-lg border border-slate-800/80 bg-slate-950/60 px-3.5 py-2.5">
          <div className="text-xs text-slate-400">Min</div>
          <div className="mt-0.5 flex flex-wrap items-baseline gap-1.5 tabular-nums">
            <span className="text-base font-bold text-sky-400">{minText}</span>
            {minAtText && <span className="text-xs text-slate-400">{minAtText}</span>}
          </div>
        </div>

        <div className="rounded-lg border border-slate-800/80 bg-slate-950/60 px-3.5 py-2.5">
          <div className="text-xs text-slate-400">Átlag</div>
          <div className="mt-0.5 tabular-nums">
            <span className="text-base font-bold text-slate-100">{avgText}</span>
          </div>
        </div>
      </div>
    </section>
  );
}
