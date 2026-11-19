import { AlertTriangle, CheckCircle2, Loader2 } from 'lucide-react';
import type { CommandAckState } from '../types/heating';

interface CommandAckBannerProps {
  ackState?: CommandAckState;
}

function getErrorText(ackState: CommandAckState): string {
  if (ackState.reason === 'SERVICE_MODE') {
    return 'Nincs visszaigazolás a Nano-tól - szerviz módban van';
  }
  if (ackState.reason === 'RELAY_MISMATCH') {
    return 'Relé hiba: a fizikai visszacsatolás (Nano D7) nem követte a parancsot';
  }
  if (ackState.reason === 'NRF_TIMEOUT' || ackState.reason === 'ACK_TIMEOUT') {
    return 'Nincs visszaigazolás a Nano-tól - ellenőrizd a rádiót';
  }
  if (ackState.message) {
    return ackState.message;
  }
  return 'Nincs visszaigazolás a Nano-tól - ellenőrizd a rádiót / szerviz módban van';
}

export function CommandAckBanner({ ackState }: CommandAckBannerProps) {
  if (!ackState || ackState.status === 'idle') {
    return null;
  }

  if (ackState.status === 'pending') {
    return (
      <div
        role="status"
        aria-live="polite"
        className="mt-4 flex items-center gap-2 rounded-lg border border-amber-500/40 bg-amber-950/40 px-3.5 py-2.5 text-xs font-medium text-amber-200"
      >
        <Loader2 className="h-4 w-4 shrink-0 animate-spin text-amber-400" aria-hidden="true" />
        <span>Folyamatban... (várakozás a Nano visszaigazolására)</span>
      </div>
    );
  }

  if (ackState.status === 'acked') {
    return (
      <div
        role="status"
        aria-live="polite"
        className="mt-4 flex items-center gap-2 rounded-lg border border-emerald-500/40 bg-emerald-950/40 px-3.5 py-2.5 text-xs font-semibold text-emerald-200"
      >
        <CheckCircle2 className="h-4 w-4 shrink-0 text-emerald-400" aria-hidden="true" />
        <span>Végrehajtva - Nano visszaigazolta</span>
      </div>
    );
  }

  return (
    <div
      role="alert"
      className="mt-4 flex items-center gap-2 rounded-lg border border-red-500/50 bg-red-950/60 px-3.5 py-2.5 text-xs font-semibold text-red-200"
    >
      <AlertTriangle className="h-4 w-4 shrink-0 text-red-400" aria-hidden="true" />
      <span>{getErrorText(ackState)}</span>
    </div>
  );
}
