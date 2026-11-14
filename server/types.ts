export type HeatingMode = 'auto' | 'manual' | 'off';

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

export interface HeatingCommand {
  targetTemp: number;
  mode: HeatingMode;
  manualHeating: boolean;
  kimenetek: HeatingOutputs;
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
  nrf24_ok: boolean;
  error: string | null;
  nanoCounter?: number;
}

export interface StoredDeviceState {
  id: string;
  status: HeatingStatusPayload;
  lastStatusAt: string | null;
  lastTelemetryAt: string | null;
  lastUpdatedAt: string;
}

export interface HeatingUpdateEvent {
  id: string;
  type: 'status' | 'telemetry';
  topic: string;
  receivedAt: string;
  data: HeatingStatusPayload;
  state: StoredDeviceState;
}
