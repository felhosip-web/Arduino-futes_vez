export type HeatingMode = 'auto' | 'manual' | 'off';

export type PumpMode = 'auto' | 'manual_on' | 'manual_off';

export type PumpStartSensor = 'kazan' | 'eloremeno' | 'max';

export type PumpState = 'varakozas' | 'futes';

export const OUTPUT_KEYS = [
  'szivattyu',
  'led_zold',
  'led_piros',
  'tartalek1',
  'tartalek2',
  'tartalek3',
  'tartalek4',
  'tartalek5',
] as const;

export type OutputKey = (typeof OUTPUT_KEYS)[number];

export type HeatingOutputs = Record<OutputKey, boolean>;

export interface PumpConfig {
  pumpMode: PumpMode;
  pumpStartTemp: number;
  pumpStopTemp: number;
  pumpStartSensor: PumpStartSensor;
}

export interface HeatingCommand {
  targetTemp: number;
  mode: HeatingMode;
  manualHeating: boolean;
  kimenetek: HeatingOutputs;
  pumpConfig: PumpConfig;
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
  nrf24_ok: boolean;
  error: string | null;
  manualHeating?: boolean;
  nanoCounter?: number;
}

export interface StoredDeviceState {
  id: string;
  lastStatus: HeatingStatusPayload | null;
  status: HeatingStatusPayload;
  lastCommand: HeatingCommand | null;
  lastCommandAt: number | null;
  lastStatusAt: number | null;
  lastTelemetryAt: number | null;
  lastUpdatedAt: number;
}

export interface HeatingUpdateEvent {
  id: string;
  type: 'status' | 'telemetry';
  topic: string;
  receivedAt: string;
  data: HeatingStatusPayload;
  state: StoredDeviceState;
}
