export type HeatingMode = 'auto' | 'manual' | 'off';

export type PumpMode = 'auto' | 'manual_on' | 'manual_off';

export type PumpStartSensor = 'kazan' | 'eloremeno' | 'max';

export type PumpState = 'varakozas' | 'futes';

export type PumpControlMode = 'SERVICE' | 'AUTO' | 'MANUAL';

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
  alarmTemp: number;
  alarmEnabled: boolean;
}

export interface SafetyConfig {
  readonly safetyOverheatTemp: number;
}

export type AlarmReason = 'none' | 'overheat' | 'sensor_fail' | 'nrf_fail' | 'pump_fault';

export interface AlarmState {
  active: boolean;
  reason: AlarmReason;
}

export interface HeartbeatConfig {
  intervalSec: number;
}

export interface HeatingTemperatures {
  eloremeno: number | null;
  visszatero: number | null;
  kazan: number | null;
  kulteri: number | null;
}

export type SensorRoleKey = 'eloremeno' | 'visszatero' | 'kazan' | 'kulteri';

export interface SensorRoleInfo {
  address: string | null;
  ok: boolean;
}

export type SensorStatusMap = Record<SensorRoleKey, SensorRoleInfo>;

export interface SensorScanItem {
  address: string;
  temp: number | null;
}

export interface SensorScanApiResponse {
  ok?: boolean;
  acked?: boolean;
  cmdId?: string;
  executedAt?: string;
  found?: SensorScanItem[];
  reason?: string;
  error?: string;
}

export interface SensorLearnApiResponse {
  ok?: boolean;
  acked?: boolean;
  cmdId?: string;
  executedAt?: string;
  role?: SensorRoleKey;
  address?: string;
  oldRole?: SensorRoleKey;
  reassignedFrom?: SensorRoleKey;
  reason?: string;
  error?: string;
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
  pumpOn?: boolean;
  relayFeedbackOn?: boolean;
  relayFault?: boolean;
  pumpControl?: PumpControlMode;
  serviceMode?: boolean;
  alarmConfig: AlarmConfig;
  safety?: SafetyConfig;
  alarmState: AlarmState;
  heartbeat: HeartbeatConfig;
  nrf24_ok: boolean;
  loopFault?: boolean;
  nanoLoopFault?: boolean;
  loopMaxMs?: number;
  loopLastMs?: number;
  dmaWaitUs?: number;
  error: string | null;
  manualHeating?: boolean;
  nanoCounter?: number;
  seq?: number;
  counter?: number;
  time?: EspTimeInfo;
  sync?: TimeSyncInfo;
  trend?: TrendStats;
  sensors?: SensorStatusMap;
  sensorUnknown?: boolean;
}

export interface HeatingCommandPayload {
  targetTemp: number;
  mode: HeatingMode;
  manualHeating: boolean;
  kimenetek: HeatingOutputs;
  pumpConfig: PumpConfig;
  alarmConfig: AlarmConfig;
  heartbeat: HeartbeatConfig;
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
  heartbeat?: HeartbeatConfig | null;
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
  alarmTemp: 85.0,
  alarmEnabled: true,
};

export const DEFAULT_SAFETY_CONFIG: SafetyConfig = {
  safetyOverheatTemp: 95.0,
};

export const DEFAULT_ALARM_STATE: AlarmState = {
  active: false,
  reason: 'none',
};

export const DEFAULT_HEARTBEAT_CONFIG: HeartbeatConfig = {
  intervalSec: 5,
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

export type CommandAckStatus = 'idle' | 'pending' | 'acked' | 'error';

export interface CommandAckState {
  status: CommandAckStatus;
  cmdId?: string;
  reason?: string;
  message?: string;
}

export interface CommandApiResponse {
  ok?: boolean;
  acked?: boolean;
  cmdId?: string;
  pumpOn?: boolean;
  executedAt?: string;
  reason?: string;
  error?: string;
}

export type MqttSignalQuality = 'excellent' | 'good' | 'fair' | 'poor' | 'offline';

export interface MqttBrokerStats {
  connected: boolean;
  configured: boolean;
  quality: MqttSignalQuality;
  signalLevel: 0 | 1 | 2 | 3 | 4;
  latencyMs: number | null;
  avgLatencyMs: number | null;
  minLatencyMs: number | null;
  maxLatencyMs: number | null;
  reliabilityPct: number | null;
  publishedCount: number;
  pubAckedCount: number;
  pubFailedCount: number;
  cmdAckSuccessCount: number;
  cmdAckFailCount: number;
  reconnectCount: number;
  lastMeasuredAt: number | null;
  lastMessageAt: number | null;
  lastError: string | null;
}
