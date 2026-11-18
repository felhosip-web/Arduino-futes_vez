import { useEffect, useState } from 'react';
import { AlertTriangle, Clock, Wrench } from 'lucide-react';

interface ServiceModeModalProps {
  active: boolean;
  startedAtMs: number | null;
}

function formatElapsedDuration(totalSeconds: number): string {
  const safeSec = Math.max(0, Math.floor(totalSeconds));
  const hours = Math.floor(safeSec / 3600);
  const minutes = Math.floor((safeSec % 3600) / 60);
  const seconds = safeSec % 60;

  if (hours > 0) {
    return `${hours} óra ${minutes} perc ${seconds} mp`;
  }
  return `${minutes} perc ${seconds} mp`;
}

export function ServiceModeModal({ active, startedAtMs }: ServiceModeModalProps) {
  const [nowMs, setNowMs] = useState<number>(() => Date.now());

  useEffect(() => {
    if (!active) {
      return;
    }
    setNowMs(Date.now());
    const timer = window.setInterval(() => {
      setNowMs(Date.now());
    }, 1000);
    return () => {
      window.clearInterval(timer);
    };
  }, [active]);

  if (!active) {
    return null;
  }

  const effectiveStart = startedAtMs ?? nowMs;
  const elapsedSec = Math.max(0, Math.floor((nowMs - effectiveStart) / 1000));

  return (
    <div
      role="alertdialog"
      aria-modal="true"
      aria-labelledby="service-mode-modal-title"
      aria-describedby="service-mode-modal-desc"
      className="fixed inset-0 z-50 flex items-center justify-center bg-slate-950/85 p-4 backdrop-blur-sm"
    >
      <div className="w-full max-w-lg rounded-2xl border-2 border-orange-500 bg-slate-900 p-6 shadow-2xl ring-2 ring-red-500/40 sm:p-7">
        {/* Fejléc: villogó figyelmeztető ikon + cím (NINCS bezáró X gomb) */}
        <div className="flex items-start gap-4 border-b border-slate-800 pb-4">
          <div className="flex h-12 w-12 shrink-0 items-center justify-center rounded-xl border border-orange-500/70 bg-gradient-to-br from-orange-500/25 to-red-600/25 text-orange-400 animate-pulse">
            <AlertTriangle className="h-6 w-6 text-orange-400" aria-hidden="true" />
          </div>

          <div className="min-w-0 flex-1">
            <div className="inline-flex items-center gap-1.5 text-xs font-semibold uppercase tracking-wider text-orange-400">
              <Wrench className="h-3.5 w-3.5" aria-hidden="true" />
              <span>Hardveres felülbírálás aktív</span>
            </div>
            <h2
              id="service-mode-modal-title"
              className="mt-1 text-xl font-bold tracking-tight text-slate-50 sm:text-2xl"
            >
              SZERVIZ ÜZEMMÓD - SLAVE
            </h2>
          </div>
        </div>

        {/* Tartalom */}
        <p
          id="service-mode-modal-desc"
          className="mt-4 text-sm leading-relaxed text-slate-200 sm:text-base"
        >
          A kazánnál lévő szerviz kapcsoló BE van kapcsolva. A szivattyú közvetlenül a Nano
          által van bekapcsolva, az ESP nem vezérli. A vezérlők le vannak tiltva, amíg a
          szerviz üzemmód aktív.
        </p>

        {/* Állapot + élő időtartam számláló */}
        <div className="mt-5 rounded-xl border border-orange-500/40 bg-slate-950/80 px-4 py-3.5">
          <div className="flex flex-wrap items-center justify-between gap-2 text-xs sm:text-sm">
            <span className="inline-flex items-center gap-2 font-medium text-orange-300">
              <Clock className="h-4 w-4 text-orange-400" aria-hidden="true" />
              <span>
                Szervizben:{' '}
                <strong className="tabular-nums font-bold text-slate-50">
                  {formatElapsedDuration(elapsedSec)}
                </strong>{' '}
                óta
              </span>
            </span>

            <span className="inline-flex items-center gap-1.5 rounded-md border border-emerald-500/40 bg-emerald-950/60 px-2.5 py-1 text-xs font-semibold text-emerald-300">
              <span className="h-2 w-2 rounded-full bg-emerald-400" aria-hidden="true" />
              Szivattyú: BE (Nano)
            </span>
          </div>
        </div>

        {/* Lábléc */}
        <div className="mt-5 border-t border-slate-800 pt-4 text-xs font-medium text-slate-300 sm:text-sm">
          Szerviz vége: kapcsold KI a kapcsolót a kazánnál (Nano D6)
        </div>
      </div>
    </div>
  );
}
