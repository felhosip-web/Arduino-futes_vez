import { OUTPUT_KEYS } from './types.ts';
import type {
  AlarmConfig,
  HeatingCommand,
  HeatingMode,
  HeatingOutputs,
  OutputKey,
  PumpConfig,
  PumpMode,
  PumpStartSensor,
  StoredDeviceState,
} from './types.ts';

const VALID_MODES: readonly HeatingMode[] = ['auto', 'manual', 'off'];
const VALID_PUMP_MODES: readonly PumpMode[] = ['auto', 'manual_on', 'manual_off'];
const VALID_PUMP_SENSORS: readonly PumpStartSensor[] = ['kazan', 'eloremeno', 'max'];
const PUMP_CONFIG_KEYS = ['pumpMode', 'pumpStartTemp', 'pumpStopTemp', 'pumpStartSensor'] as const;
const ALARM_CONFIG_KEYS = ['overheatTemp', 'alarmEnabled'] as const;

export const DEFAULT_PUMP_CONFIG: PumpConfig = {
  pumpMode: 'auto',
  pumpStartTemp: 40.0,
  pumpStopTemp: 30.0,
  pumpStartSensor: 'kazan',
};

export const DEFAULT_ALARM_CONFIG: AlarmConfig = {
  overheatTemp: 85.0,
  alarmEnabled: true,
};

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

export interface FreshCommandBase {
  targetTemp?: number;
  mode?: HeatingMode;
  manualHeating?: boolean;
  kimenetek?: HeatingOutputs;
  pumpConfig?: PumpConfig;
  alarmConfig?: AlarmConfig;
}

export function resolveFreshBase(state?: StoredDeviceState): FreshCommandBase {
  if (!state) return {};

  const lastCmdMs = state.lastCommandAt ?? 0;
  const lastStatusMs = state.lastStatusAt ?? 0;
  const useLastCommand = state.lastCommand !== null && lastCmdMs > lastStatusMs;

  if (useLastCommand && state.lastCommand) {
    return {
      targetTemp: state.lastCommand.targetTemp,
      mode: state.lastCommand.mode,
      manualHeating: state.lastCommand.manualHeating,
      kimenetek: state.lastCommand.kimenetek,
      pumpConfig: state.lastCommand.pumpConfig,
      alarmConfig: state.lastCommand.alarmConfig,
    };
  }

  const src = state.lastStatus ?? state.status;
  return {
    targetTemp: src?.targetTemp,
    mode: src?.mode,
    manualHeating: src?.manualHeating ?? state.lastCommand?.manualHeating ?? false,
    kimenetek: src?.kimenetek,
    pumpConfig: src?.pumpConfig,
    alarmConfig: src?.alarmConfig,
  };
}

export function validatePumpConfig(
  rawPumpConfig: unknown,
  fallbackPumpConfig?: Partial<PumpConfig>
): ValidationResult<PumpConfig> | ValidationError {
  const base: PumpConfig = {
    pumpMode: fallbackPumpConfig?.pumpMode ?? DEFAULT_PUMP_CONFIG.pumpMode,
    pumpStartTemp: fallbackPumpConfig?.pumpStartTemp ?? DEFAULT_PUMP_CONFIG.pumpStartTemp,
    pumpStopTemp: fallbackPumpConfig?.pumpStopTemp ?? DEFAULT_PUMP_CONFIG.pumpStopTemp,
    pumpStartSensor: fallbackPumpConfig?.pumpStartSensor ?? DEFAULT_PUMP_CONFIG.pumpStartSensor,
  };

  if (rawPumpConfig === undefined) {
    return validateResolvedPumpConfig(base);
  }

  if (!rawPumpConfig || typeof rawPumpConfig !== 'object' || Array.isArray(rawPumpConfig)) {
    return { ok: false, error: 'A pumpConfig mezőnek érvényes JSON objektumnak kell lennie.' };
  }

  const raw = rawPumpConfig as Record<string, unknown>;
  for (const key of Object.keys(raw)) {
    if (!(PUMP_CONFIG_KEYS as readonly string[]).includes(key)) {
      return { ok: false, error: `Ismeretlen pumpConfig kulcs: "${key}".` };
    }
  }

  let pumpMode: PumpMode;
  if (raw.pumpMode === undefined) {
    pumpMode = base.pumpMode;
  } else if (
    typeof raw.pumpMode !== 'string' ||
    !(VALID_PUMP_MODES as readonly string[]).includes(raw.pumpMode)
  ) {
    return {
      ok: false,
      error: 'A pumpConfig.pumpMode csak "auto", "manual_on" vagy "manual_off" lehet.',
    };
  } else {
    pumpMode = raw.pumpMode as PumpMode;
  }

  let pumpStartTemp: number;
  if (raw.pumpStartTemp === undefined) {
    pumpStartTemp = base.pumpStartTemp;
  } else if (typeof raw.pumpStartTemp !== 'number' || !Number.isFinite(raw.pumpStartTemp)) {
    return { ok: false, error: 'A pumpConfig.pumpStartTemp mezőnek véges számnak kell lennie.' };
  } else {
    pumpStartTemp = Math.round(raw.pumpStartTemp * 10) / 10;
  }

  let pumpStopTemp: number;
  if (raw.pumpStopTemp === undefined) {
    pumpStopTemp = base.pumpStopTemp;
  } else if (typeof raw.pumpStopTemp !== 'number' || !Number.isFinite(raw.pumpStopTemp)) {
    return { ok: false, error: 'A pumpConfig.pumpStopTemp mezőnek véges számnak kell lennie.' };
  } else {
    pumpStopTemp = Math.round(raw.pumpStopTemp * 10) / 10;
  }

  let pumpStartSensor: PumpStartSensor;
  if (raw.pumpStartSensor === undefined) {
    pumpStartSensor = base.pumpStartSensor;
  } else if (
    typeof raw.pumpStartSensor !== 'string' ||
    !(VALID_PUMP_SENSORS as readonly string[]).includes(raw.pumpStartSensor)
  ) {
    return {
      ok: false,
      error: 'A pumpConfig.pumpStartSensor csak "kazan", "eloremeno" vagy "max" lehet.',
    };
  } else {
    pumpStartSensor = raw.pumpStartSensor as PumpStartSensor;
  }

  return validateResolvedPumpConfig({
    pumpMode,
    pumpStartTemp,
    pumpStopTemp,
    pumpStartSensor,
  });
}

function validateResolvedPumpConfig(cfg: PumpConfig): ValidationResult<PumpConfig> | ValidationError {
  if (!(VALID_PUMP_MODES as readonly string[]).includes(cfg.pumpMode)) {
    return {
      ok: false,
      error: 'A pumpConfig.pumpMode csak "auto", "manual_on" vagy "manual_off" lehet.',
    };
  }

  if (typeof cfg.pumpStartTemp !== 'number' || !Number.isFinite(cfg.pumpStartTemp)) {
    return { ok: false, error: 'A pumpConfig.pumpStartTemp mezőnek véges számnak kell lennie.' };
  }
  if (cfg.pumpStartTemp < 30 || cfg.pumpStartTemp > 50) {
    return { ok: false, error: 'A pumpConfig.pumpStartTemp értéke 30 és 50 °C között lehet.' };
  }

  if (typeof cfg.pumpStopTemp !== 'number' || !Number.isFinite(cfg.pumpStopTemp)) {
    return { ok: false, error: 'A pumpConfig.pumpStopTemp mezőnek véges számnak kell lennie.' };
  }
  if (cfg.pumpStopTemp < 20 || cfg.pumpStopTemp > 40) {
    return { ok: false, error: 'A pumpConfig.pumpStopTemp értéke 20 és 40 °C között lehet.' };
  }

  if (!(VALID_PUMP_SENSORS as readonly string[]).includes(cfg.pumpStartSensor)) {
    return {
      ok: false,
      error: 'A pumpConfig.pumpStartSensor csak "kazan", "eloremeno" vagy "max" lehet.',
    };
  }

  // Kötelező hiszterézis: pumpStopTemp < pumpStartTemp - 5 (tizedfokra pontos összehasonlítás)
  if (Math.round(cfg.pumpStopTemp * 10) >= Math.round((cfg.pumpStartTemp - 5) * 10)) {
    return {
      ok: false,
      error: 'A pumpConfig.pumpStopTemp értékének kisebbnek kell lennie, mint pumpStartTemp - 5 °C.',
    };
  }

  return { ok: true, value: cfg };
}

export function validateAlarmConfig(
  rawAlarmConfig: unknown,
  fallbackAlarmConfig?: Partial<AlarmConfig>
): ValidationResult<AlarmConfig> | ValidationError {
  const base: AlarmConfig = {
    overheatTemp: fallbackAlarmConfig?.overheatTemp ?? DEFAULT_ALARM_CONFIG.overheatTemp,
    alarmEnabled: fallbackAlarmConfig?.alarmEnabled ?? DEFAULT_ALARM_CONFIG.alarmEnabled,
  };

  if (rawAlarmConfig === undefined) {
    return validateResolvedAlarmConfig(base);
  }

  if (!rawAlarmConfig || typeof rawAlarmConfig !== 'object' || Array.isArray(rawAlarmConfig)) {
    return { ok: false, error: 'Az alarmConfig mezőnek érvényes JSON objektumnak kell lennie.' };
  }

  const raw = rawAlarmConfig as Record<string, unknown>;
  for (const key of Object.keys(raw)) {
    if (!(ALARM_CONFIG_KEYS as readonly string[]).includes(key)) {
      return { ok: false, error: `Ismeretlen alarmConfig kulcs: "${key}".` };
    }
  }

  let overheatTemp: number;
  if (raw.overheatTemp === undefined) {
    overheatTemp = base.overheatTemp;
  } else if (typeof raw.overheatTemp !== 'number' || !Number.isFinite(raw.overheatTemp)) {
    return { ok: false, error: 'Az alarmConfig.overheatTemp mezőnek véges számnak kell lennie.' };
  } else {
    overheatTemp = Math.round(raw.overheatTemp * 10) / 10;
  }

  let alarmEnabled: boolean;
  if (raw.alarmEnabled === undefined) {
    alarmEnabled = base.alarmEnabled;
  } else if (typeof raw.alarmEnabled !== 'boolean') {
    return { ok: false, error: 'Az alarmConfig.alarmEnabled mező csak boolean lehet.' };
  } else {
    alarmEnabled = raw.alarmEnabled;
  }

  return validateResolvedAlarmConfig({
    overheatTemp,
    alarmEnabled,
  });
}

function validateResolvedAlarmConfig(cfg: AlarmConfig): ValidationResult<AlarmConfig> | ValidationError {
  if (typeof cfg.overheatTemp !== 'number' || !Number.isFinite(cfg.overheatTemp)) {
    return { ok: false, error: 'Az alarmConfig.overheatTemp mezőnek véges számnak kell lennie.' };
  }
  if (cfg.overheatTemp < 70 || cfg.overheatTemp > 90) {
    return { ok: false, error: 'Az alarmConfig.overheatTemp értéke 70 és 90 °C között lehet.' };
  }

  if (typeof cfg.alarmEnabled !== 'boolean') {
    return { ok: false, error: 'Az alarmConfig.alarmEnabled mező csak boolean lehet.' };
  }

  return { ok: true, value: cfg };
}

export function validateHeatingCommand(
  body: unknown,
  currentState?: StoredDeviceState
): ValidationResult<HeatingCommand> | ValidationError {
  if (!body || typeof body !== 'object' || Array.isArray(body)) {
    return { ok: false, error: 'A kérés törzsének érvényes JSON objektumnak kell lennie.' };
  }

  const raw = body as Record<string, unknown>;
  const freshBase = resolveFreshBase(currentState);

  let targetTemp: number;
  if (raw.targetTemp === undefined) {
    if (typeof freshBase.targetTemp === 'number' && Number.isFinite(freshBase.targetTemp)) {
      targetTemp = freshBase.targetTemp;
    } else {
      return { ok: false, error: 'A targetTemp mezőnek véges számnak kell lennie.' };
    }
  } else {
    if (typeof raw.targetTemp !== 'number' || !Number.isFinite(raw.targetTemp)) {
      return { ok: false, error: 'A targetTemp mezőnek véges számnak kell lennie.' };
    }
    if (raw.targetTemp < 10 || raw.targetTemp > 30) {
      return { ok: false, error: 'A targetTemp értéke 10 és 30 °C között lehet.' };
    }
    targetTemp = Math.round(raw.targetTemp * 10) / 10;
  }

  let mode: HeatingMode;
  if (raw.mode === undefined) {
    if (freshBase.mode && (VALID_MODES as readonly string[]).includes(freshBase.mode)) {
      mode = freshBase.mode;
    } else {
      return { ok: false, error: 'A mode csak "auto", "manual" vagy "off" lehet.' };
    }
  } else {
    if (typeof raw.mode !== 'string' || !(VALID_MODES as readonly string[]).includes(raw.mode)) {
      return { ok: false, error: 'A mode csak "auto", "manual" vagy "off" lehet.' };
    }
    mode = raw.mode as HeatingMode;
  }

  let manualHeating: boolean;
  if (raw.manualHeating === undefined) {
    manualHeating = freshBase.manualHeating ?? false;
  } else {
    if (typeof raw.manualHeating !== 'boolean') {
      return { ok: false, error: 'A manualHeating mező csak boolean lehet.' };
    }
    manualHeating = raw.manualHeating;
  }

  if (
    raw.kimenetek !== undefined &&
    (!raw.kimenetek || typeof raw.kimenetek !== 'object' || Array.isArray(raw.kimenetek))
  ) {
    return { ok: false, error: 'A kimenetek mezőnek objektumnak kell lennie.' };
  }

  const rawKimenetek = (raw.kimenetek ?? {}) as Record<string, unknown>;
  for (const key of Object.keys(rawKimenetek)) {
    if (!isOutputKey(key)) {
      return { ok: false, error: `Ismeretlen kimenet kulcs: "${key}".` };
    }
  }

  const baseOutputs = freshBase.kimenetek;
  const normalizedOutputs = {} as HeatingOutputs;
  for (const key of OUTPUT_KEYS) {
    const hasKey = Object.prototype.hasOwnProperty.call(rawKimenetek, key);
    const val = rawKimenetek[key];
    if (!hasKey || val === undefined) {
      normalizedOutputs[key] = typeof baseOutputs?.[key] === 'boolean' ? baseOutputs[key] : false;
      continue;
    }
    if (typeof val !== 'boolean') {
      return { ok: false, error: `A kimenetek.${key} mező csak boolean lehet.` };
    }
    normalizedOutputs[key] = val;
  }

  const pumpValidation = validatePumpConfig(raw.pumpConfig, freshBase.pumpConfig);
  if (!pumpValidation.ok) {
    return pumpValidation;
  }

  const alarmValidation = validateAlarmConfig(raw.alarmConfig, freshBase.alarmConfig);
  if (!alarmValidation.ok) {
    return alarmValidation;
  }

  return {
    ok: true,
    value: {
      targetTemp,
      mode,
      manualHeating,
      kimenetek: normalizedOutputs,
      pumpConfig: pumpValidation.value,
      alarmConfig: alarmValidation.value,
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

export function hasValidEspStatus(state: StoredDeviceState | undefined): state is StoredDeviceState {
  const s = state?.lastStatus;
  if (!state || state.lastStatusAt === null || !s) {
    return false;
  }

  if (typeof s.targetTemp !== 'number' || !Number.isFinite(s.targetTemp)) {
    return false;
  }

  if (typeof s.mode !== 'string' || !(VALID_MODES as readonly string[]).includes(s.mode)) {
    return false;
  }

  if (!s.kimenetek || typeof s.kimenetek !== 'object') {
    return false;
  }

  return OUTPUT_KEYS.every((key) => typeof s.kimenetek[key] === 'boolean');
}

export function buildSingleOutputCommand(
  state: StoredDeviceState,
  toggle: SingleOutputBody
): HeatingCommand {
  const fresh = resolveFreshBase(state);
  const latest = state.lastStatus ?? state.status;

  return {
    targetTemp: fresh.targetTemp ?? latest.targetTemp,
    mode: fresh.mode ?? latest.mode,
    manualHeating: fresh.manualHeating ?? false,
    kimenetek: {
      ...(fresh.kimenetek ?? latest.kimenetek),
      [toggle.nev]: toggle.ertek,
    },
    pumpConfig: {
      ...(fresh.pumpConfig ?? latest.pumpConfig ?? DEFAULT_PUMP_CONFIG),
    },
    alarmConfig: {
      ...(fresh.alarmConfig ?? latest.alarmConfig ?? DEFAULT_ALARM_CONFIG),
    },
  };
}

export function buildPumpUpdateCommand(
  state: StoredDeviceState,
  body: unknown
): ValidationResult<HeatingCommand> | ValidationError {
  if (!body || typeof body !== 'object' || Array.isArray(body)) {
    return { ok: false, error: 'A kérés törzsének JSON objektumnak kell lennie.' };
  }

  const raw = body as Record<string, unknown>;
  if (!raw.pumpConfig || typeof raw.pumpConfig !== 'object' || Array.isArray(raw.pumpConfig)) {
    return { ok: false, error: 'A pumpConfig objektum kötelező.' };
  }

  const fresh = resolveFreshBase(state);
  const latest = state.lastStatus ?? state.status;

  const pumpValidation = validatePumpConfig(
    raw.pumpConfig,
    fresh.pumpConfig ?? latest.pumpConfig ?? DEFAULT_PUMP_CONFIG
  );
  if (!pumpValidation.ok) {
    return pumpValidation;
  }

  return {
    ok: true,
    value: {
      targetTemp: fresh.targetTemp ?? latest.targetTemp,
      mode: fresh.mode ?? latest.mode,
      manualHeating: fresh.manualHeating ?? false,
      kimenetek: {
        ...(fresh.kimenetek ?? latest.kimenetek),
      },
      pumpConfig: pumpValidation.value,
      alarmConfig: {
        ...(fresh.alarmConfig ?? latest.alarmConfig ?? DEFAULT_ALARM_CONFIG),
      },
    },
  };
}

export function buildAlarmUpdateCommand(
  state: StoredDeviceState,
  body: unknown
): ValidationResult<HeatingCommand> | ValidationError {
  if (!body || typeof body !== 'object' || Array.isArray(body)) {
    return { ok: false, error: 'A kérés törzsének JSON objektumnak kell lennie.' };
  }

  const raw = body as Record<string, unknown>;
  if (!raw.alarmConfig || typeof raw.alarmConfig !== 'object' || Array.isArray(raw.alarmConfig)) {
    return { ok: false, error: 'Az alarmConfig objektum kötelező.' };
  }

  const fresh = resolveFreshBase(state);
  const latest = state.lastStatus ?? state.status;

  const alarmValidation = validateAlarmConfig(
    raw.alarmConfig,
    fresh.alarmConfig ?? latest.alarmConfig ?? DEFAULT_ALARM_CONFIG
  );
  if (!alarmValidation.ok) {
    return alarmValidation;
  }

  return {
    ok: true,
    value: {
      targetTemp: fresh.targetTemp ?? latest.targetTemp,
      mode: fresh.mode ?? latest.mode,
      manualHeating: fresh.manualHeating ?? false,
      kimenetek: {
        ...(fresh.kimenetek ?? latest.kimenetek),
      },
      pumpConfig: {
        ...(fresh.pumpConfig ?? latest.pumpConfig ?? DEFAULT_PUMP_CONFIG),
      },
      alarmConfig: alarmValidation.value,
    },
  };
}
