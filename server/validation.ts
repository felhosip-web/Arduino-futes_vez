import { OUTPUT_KEYS } from './types.ts';
import type { HeatingCommand, HeatingMode, HeatingOutputs, OutputKey } from './types.ts';

const VALID_MODES: readonly HeatingMode[] = ['auto', 'manual', 'off'];

export interface ValidationResult<T> {
  ok: true;
  value: T;
}

export interface ValidationError {
  ok: false;
  error: string;
}

export function isOutputKey(key: unknown): key is OutputKey {
  return typeof key === 'string' && (OUTPUT_KEYS as readonly string[]).includes(key);
}

export function validateHeatingCommand(body: unknown): ValidationResult<HeatingCommand> | ValidationError {
  if (!body || typeof body !== 'object' || Array.isArray(body)) {
    return { ok: false, error: 'A kérés törzsének érvényes JSON objektumnak kell lennie.' };
  }

  const raw = body as Record<string, unknown>;

  if (typeof raw.targetTemp !== 'number' || !Number.isFinite(raw.targetTemp)) {
    return { ok: false, error: 'A targetTemp mezőnek véges számnak kell lennie.' };
  }
  if (raw.targetTemp < 10 || raw.targetTemp > 30) {
    return { ok: false, error: 'A targetTemp értéke 10 és 30 °C között lehet.' };
  }

  if (typeof raw.mode !== 'string' || !(VALID_MODES as readonly string[]).includes(raw.mode)) {
    return { ok: false, error: 'A mode csak "auto", "manual" vagy "off" lehet.' };
  }

  if (typeof raw.manualHeating !== 'boolean') {
    return { ok: false, error: 'A manualHeating mező csak boolean lehet.' };
  }

  if (!raw.kimenetek || typeof raw.kimenetek !== 'object' || Array.isArray(raw.kimenetek)) {
    return { ok: false, error: 'A kimenetek mezőnek objektumnak kell lennie.' };
  }

  const rawKimenetek = raw.kimenetek as Record<string, unknown>;
  for (const key of Object.keys(rawKimenetek)) {
    if (!isOutputKey(key)) {
      return { ok: false, error: `Ismeretlen kimenet kulcs: "${key}".` };
    }
  }

  const normalizedOutputs = {} as HeatingOutputs;
  for (const key of OUTPUT_KEYS) {
    const val = rawKimenetek[key];
    if (typeof val !== 'boolean') {
      return { ok: false, error: `A kimenetek.${key} mező csak boolean lehet.` };
    }
    normalizedOutputs[key] = val;
  }

  return {
    ok: true,
    value: {
      targetTemp: Math.round(raw.targetTemp * 10) / 10,
      mode: raw.mode as HeatingMode,
      manualHeating: raw.manualHeating,
      kimenetek: normalizedOutputs,
    },
  };
}

export interface SingleOutputBody {
  nev: OutputKey;
  ertek: boolean;
}

export function validateSingleOutputBody(body: unknown): ValidationResult<SingleOutputBody> | ValidationError {
  if (!body || typeof body !== 'object' || Array.isArray(body)) {
    return { ok: false, error: 'A kérés törzsének JSON objektumnak kell lennie.' };
  }

  const raw = body as Record<string, unknown>;

  if (!isOutputKey(raw.nev)) {
    return {
      ok: false,
      error: `Érvénytelen kimenet név. Engedélyezett értékek: ${OUTPUT_KEYS.join(', ')}.`,
    };
  }

  if (typeof raw.ertek !== 'boolean') {
    return { ok: false, error: 'Az ertek mező csak boolean lehet.' };
  }

  return {
    ok: true,
    value: {
      nev: raw.nev,
      ertek: raw.ertek,
    },
  };
}
