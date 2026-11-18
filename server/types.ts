export type HeatingMode = 'auto' | 'manual' | 'off';

export type PumpMode = 'auto' | 'manual_on' | 'manual_off';

export type PumpStartSensor = 'kazan' | 'eloremeno' | 'max';

export type PumpState = 'varakozas' | 'futes';

export type PumpControlMode = 'SERVICE' | 'AUTO' | 'MANUAL';

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

export interface AlarmConfig {
  alarmTemp: number;
  alarmEnabled: boolean;
}

export interface SafetyConfig {
  readonly safetyOverheatTemp: number;
}

export type AlarmReason = 'none' | 'overheat' | 'sensor_fail' | 'nrf_fail';

export interface AlarmState {
  active: boolean;
  reason: AlarmReason;
}

export const VALID_HEARTBEAT_INTERVALS = [5, 10, 15, 20, 25, 30] as const;

export interface HeartbeatConfig {
  intervalSec: number;
}

export interface HeatingCommand {
  targetTemp: number;
  mode: HeatingMode;
  manualHeating: boolean;
  kimenetek: HeatingOutputs;
  pumpConfig: PumpConfig;
  alarmConfig: AlarmConfig;
  heartbeat: HeartbeatConfig;
}

export interface HeatingTemperatures {
  eloremeno: number | null;
  visszatero: number | null;
  kazan: number | null;
  kulteri: number | null;
}

export interface EspTimeInfo {
  espNtpSynced: boolean;
  espTime: string;
  espUptimeSec: number;
}

export interface TimeSyncInfo {
  ntpSynced: boolean;
  espTime: string;
  nanoSynced: boolean;
  nanoTime: string | null;
  nanoStale: boolean;
  lastSync: string | null;
  nextSync: string | null;
  syncSuccess: boolean;
  millisOverflowInDays: number;
  pingAgeSec: number | null;
}

export type TrendDirection = 'up' | 'down' | 'flat';

export interface TrendStats {
  eloremeno: TrendDirection;
  delta: number;
  max: number | null;
  min: number | null;
  avg: number | null;
  maxAt?: string | null;
  minAt?: string | null;
  since: string | null;
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
  pumpControl?: PumpControlMode;
  serviceMode?: boolean;
  alarmConfig: AlarmConfig;
  safety?: SafetyConfig;
  alarmState: AlarmState;
  heartbeat: HeartbeatConfig;
  nrf24_ok: boolean;
  error: string | null;
  manualHeating?: boolean;
  nanoCounter?: number;
  time?: EspTimeInfo;
  sync?: TimeSyncInfo;
  trend?: TrendStats;
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
  alarmConfig?: AlarmConfig | null;
  alarmState?: AlarmState | null;
  heartbeat?: HeartbeatConfig | null;
}

export interface HeatingUpdateEvent {
  id: string;
  type: 'status' | 'telemetry';
  topic: string;
  receivedAt: string;
  data: HeatingStatusPayload;
  state: StoredDeviceState;
}
