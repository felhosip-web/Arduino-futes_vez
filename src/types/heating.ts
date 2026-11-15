export type HeatingMode = 'auto' | 'manual' | 'off';

export type PumpMode = 'auto' | 'manual_on' | 'manual_off';

export type PumpStartSensor = 'kazan' | 'eloremeno' | 'max';

export type PumpState = 'varakozas' | 'futes';

export type OutputKey =
  | 'szivattyu'
  | 'led_zold'
  | 'led_piros'
  | 'tartalek1'
  | 'tartalek2'
  | 'tartalek3'
  | 'tartalek4'
  | 'tartalek5';

export type HeatingOutputs = Record<OutputKey, boolean>;

export interface PumpConfig {
  pumpMode: PumpMode;
  pumpStartTemp: number;
  pumpStopTemp: number;
  pumpStartSensor: PumpStartSensor;
}

export interface AlarmConfig {
  overheatTemp: number;
  alarmEnabled: boolean;
}

export type AlarmReason = 'none' | 'overheat' | 'sensor_fail' | 'nrf_fail';

export interface AlarmState {
  active: boolean;
  reason: AlarmReason;
}

export interface HeatingTemperatures {
  eloremeno: number | null;
  visszatero: number | null;
  kazan: number | null;
  kulteri: number | null;
}

export interface HeatingStatusPayload {
  currentTemp: number;
  targetTemp: number;
  heatingOn: boolean;
  mode: HeatingMode;
  online: boolean;
  homersekletek: HeatingTemperatures;
  kimenetek: HeatingOutputs;
  kazan_status: number[];
  pumpConfig: PumpConfig;
  pumpState: PumpState;
  alarmConfig: AlarmConfig;
  alarmState: AlarmState;
  nrf24_ok: boolean;
  error: string | null;
  manualHeating?: boolean;
  nanoCounter?: number;
}

export interface HeatingCommandPayload {
  targetTemp: number;
  mode: HeatingMode;
  manualHeating: boolean;
  kimenetek: HeatingOutputs;
  pumpConfig: PumpConfig;
  alarmConfig: AlarmConfig;
}

export interface StoredDeviceState {
  id: string;
  lastStatus: HeatingStatusPayload | null;
  status?: HeatingStatusPayload | null;
  lastCommand: HeatingCommandPayload | null;
  lastCommandAt: number | null;
  lastStatusAt: number | null;
  lastTelemetryAt: number | null;
  lastUpdatedAt: number;
  alarmConfig?: AlarmConfig | null;
  alarmState?: AlarmState | null;
}

export interface HeatingUpdateEvent {
  id: string;
  type: 'status' | 'telemetry';
  topic: string;
  receivedAt: string;
  data: Partial<HeatingStatusPayload>;
  state: StoredDeviceState;
}

export const DEFAULT_OUTPUTS: HeatingOutputs = {
  szivattyu: false,
  led_zold: false,
  led_piros: false,
  tartalek1: false,
  tartalek2: false,
  tartalek3: false,
  tartalek4: false,
  tartalek5: false,
};

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

export const DEFAULT_ALARM_STATE: AlarmState = {
  active: false,
  reason: 'none',
};

export const OUTPUT_ITEMS: ReadonlyArray<{ key: OutputKey; label: string; bit: number }> = [
  { key: 'szivattyu', label: 'Szivattyú', bit: 0 },
  { key: 'led_zold', label: 'Zöld LED', bit: 1 },
  { key: 'led_piros', label: 'Piros LED', bit: 2 },
  { key: 'tartalek1', label: 'Tartalék 1', bit: 3 },
  { key: 'tartalek2', label: 'Tartalék 2', bit: 4 },
  { key: 'tartalek3', label: 'Tartalék 3', bit: 5 },
  { key: 'tartalek4', label: 'Tartalék 4', bit: 6 },
  { key: 'tartalek5', label: 'Tartalék 5', bit: 7 },
];
