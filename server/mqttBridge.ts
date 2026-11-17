import mqtt from 'mqtt';
import type { MqttClient } from 'mqtt';
import type { Server as SocketIOServer } from 'socket.io';
import { LogsStore, normalizeLogLevel } from './logsStore.ts';
import type { LogLevel } from './logsStore.ts';
import { VALID_HEARTBEAT_INTERVALS } from './types.ts';
import type {
  AlarmConfig,
  HeatingCommand,
  HeatingStatusPayload,
  HeatingUpdateEvent,
  PumpMode,
  PumpStartSensor,
  StoredDeviceState,
} from './types.ts';
import { SAFETY_OVERHEAT_TEMP } from './validation.ts';

const VALID_PUMP_MODES: readonly PumpMode[] = ['auto', 'manual_on', 'manual_off'];
const VALID_PUMP_SENSORS: readonly PumpStartSensor[] = ['kazan', 'eloremeno', 'max'];

function mergeDefined<T extends object>(
  base: T | undefined,
  patch: Partial<T> | undefined
): T | undefined {
  if (!patch) return base;
  const cleanPatch: Partial<T> = {};
  for (const [key, value] of Object.entries(patch)) {
    if (value !== undefined) {
      (cleanPatch as Record<string, unknown>)[key] = value;
    }
  }
  return base ? { ...base, ...cleanPatch } : (cleanPatch as T);
}

function normalizeIncomingAlarmConfig(raw: unknown): Partial<AlarmConfig> | undefined {
  if (!raw || typeof raw !== 'object' || Array.isArray(raw)) return undefined;
  const ac = raw as Record<string, unknown>;
  const out: Partial<AlarmConfig> = {};
  if (typeof ac.alarmTemp === 'number' && Number.isFinite(ac.alarmTemp)) {
    out.alarmTemp = ac.alarmTemp;
  } else if (typeof ac.overheatTemp === 'number' && Number.isFinite(ac.overheatTemp)) {
    out.alarmTemp = ac.overheatTemp;
  }
  if (typeof ac.alarmEnabled === 'boolean') {
    out.alarmEnabled = ac.alarmEnabled;
  }
  return out;
}

export function mergeHeatingStatus(
  prevStatus: HeatingStatusPayload | null | undefined,
  parsed: HeatingStatusPayload
): HeatingStatusPayload {
  const normalizedAlarm = normalizeIncomingAlarmConfig(parsed.alarmConfig) as AlarmConfig | undefined;

  if (!prevStatus) {
    return {
      ...parsed,
      ...(normalizedAlarm ? { alarmConfig: normalizedAlarm } : {}),
      safety: { safetyOverheatTemp: SAFETY_OVERHEAT_TEMP },
    };
  }

  const cleanTop: Partial<HeatingStatusPayload> = {};
  for (const [key, value] of Object.entries(parsed)) {
    if (value !== undefined && key !== 'safety') {
      (cleanTop as Record<string, unknown>)[key] = value;
    }
  }

  return {
    ...prevStatus,
    ...cleanTop,
    homersekletek: mergeDefined(prevStatus.homersekletek, parsed.homersekletek)!,
    kimenetek: mergeDefined(prevStatus.kimenetek, parsed.kimenetek)!,
    pumpConfig: mergeDefined(prevStatus.pumpConfig, parsed.pumpConfig)!,
    alarmConfig: mergeDefined(prevStatus.alarmConfig, normalizedAlarm)!,
    safety: { safetyOverheatTemp: SAFETY_OVERHEAT_TEMP },
    alarmState: mergeDefined(prevStatus.alarmState, parsed.alarmState)!,
    heartbeat: mergeDefined(prevStatus.heartbeat, parsed.heartbeat)!,
  };
}

export function isFullEspPayload(parsed: Partial<HeatingStatusPayload>): boolean {
  if (
    typeof parsed.targetTemp !== 'number' ||
    !Number.isFinite(parsed.targetTemp) ||
    typeof parsed.mode !== 'string' ||
    !parsed.kimenetek ||
    typeof parsed.kimenetek !== 'object' ||
    Array.isArray(parsed.kimenetek)
  ) {
    return false;
  }

  const pc = parsed.pumpConfig;
  if (!pc || typeof pc !== 'object' || Array.isArray(pc)) {
    return false;
  }

  const pumpValid =
    typeof pc.pumpMode === 'string' &&
    (VALID_PUMP_MODES as readonly string[]).includes(pc.pumpMode) &&
    typeof pc.pumpStartTemp === 'number' &&
    Number.isFinite(pc.pumpStartTemp) &&
    typeof pc.pumpStopTemp === 'number' &&
    Number.isFinite(pc.pumpStopTemp) &&
    typeof pc.pumpStartSensor === 'string' &&
    (VALID_PUMP_SENSORS as readonly string[]).includes(pc.pumpStartSensor);

  if (!pumpValid) {
    return false;
  }

  const ac = parsed.alarmConfig as (Partial<AlarmConfig> & { overheatTemp?: unknown }) | undefined;
  if (!ac || typeof ac !== 'object' || Array.isArray(ac)) {
    return false;
  }

  const rawAlarmTemp = ac.alarmTemp ?? ac.overheatTemp;
  const alarmValid =
    typeof rawAlarmTemp === 'number' &&
    Number.isFinite(rawAlarmTemp) &&
    typeof ac.alarmEnabled === 'boolean';

  if (!alarmValid) {
    return false;
  }

  const hb = parsed.heartbeat;
  if (!hb || typeof hb !== 'object' || Array.isArray(hb)) {
    return false;
  }

  return (
    typeof hb.intervalSec === 'number' &&
    Number.isFinite(hb.intervalSec) &&
    (VALID_HEARTBEAT_INTERVALS as readonly number[]).includes(hb.intervalSec)
  );
}

export class HeatingMqttBridge {
  private client: MqttClient | null = null;
  private readonly io: SocketIOServer;
  private readonly logsStore: LogsStore;
  private readonly states = new Map<string, StoredDeviceState>();
  private readonly lastCommands = new Map<string, { command: HeatingCommand; sentAt: number }>();
  private readonly onlineStates = new Map<string, boolean>();
  private connected = false;

  constructor(io: SocketIOServer, logsStore: LogsStore = new LogsStore()) {
    this.io = io;
    this.logsStore = logsStore;
  }

  public getLogsStore(): LogsStore {
    return this.logsStore;
  }

  public logBackendEvent(level: LogLevel, code: string, message: string): void {
    this.logsStore.add(level, code, message, 'backend');
  }

  public connect(): void {
    const mqttHost = process.env.MQTT_HOST?.trim();
    const mqttPort = Number(process.env.MQTT_PORT || 8883);
    const brokerUrl =
      process.env.MQTT_BROKER_URL?.trim() ||
      (mqttHost ? `mqtts://${mqttHost}:${mqttPort}` : '');
    const username = process.env.MQTT_USER?.trim() || process.env.MQTT_USERNAME?.trim();
    const password = process.env.MQTT_PASS?.trim() || process.env.MQTT_PASSWORD?.trim();
    const clientId = process.env.MQTT_CLIENT_ID?.trim() || 'backend-futes';

    if (!brokerUrl || brokerUrl.includes('your-cluster-id')) {
      console.warn('[MQTT] MQTT_HOST / MQTT_BROKER_URL nincs beállítva (.env) — az MQTT híd várakozik.');
      return;
    }

    console.log(`[MQTT] Csatlakozás HiveMQ Cloud-hoz (${brokerUrl}), clientId=${clientId}...`);

    this.client = mqtt.connect(brokerUrl, {
      clientId,
      username,
      password,
      protocol: 'mqtts',
      rejectUnauthorized: true,
      clean: true,
      reconnectPeriod: 5000,
      connectTimeout: 10000,
    });

    this.client.on('connect', () => {
      const wasConnected = this.connected;
      this.connected = true;
      console.log('[MQTT] Sikeresen csatlakozva a HiveMQ Cloud brokerhez.');
      if (!wasConnected) {
        this.logBackendEvent('INFO', 'MQTT_CONNECT', 'HiveMQ Cloud broker csatlakozva');
      }
      this.client?.subscribe(['futes/+/status', 'futes/+/telemetry', 'futes/+/log'], { qos: 1 }, (err) => {
        if (err) {
          console.error('[MQTT] Feliratkozási hiba:', err.message);
        } else {
          console.log('[MQTT] Feliratkozva: futes/+/status, futes/+/telemetry, futes/+/log');
        }
      });
    });

    this.client.on('reconnect', () => {
      console.log('[MQTT] Újracsatlakozás folyamatban...');
    });

    this.client.on('close', () => {
      const wasConnected = this.connected;
      this.connected = false;
      if (wasConnected) {
        this.logBackendEvent('WARN', 'MQTT_DISCONNECT', 'HiveMQ Cloud kapcsolat megszakadt');
      }
    });

    this.client.on('error', (err) => {
      console.error('[MQTT] Hiba:', err.message);
      this.logBackendEvent('ERROR', 'MQTT_DISCONNECT', `MQTT hiba: ${err.message}`);
    });

    this.client.on('message', (topic, payloadBuffer) => {
      this.handleIncomingMessage(topic, payloadBuffer);
    });
  }

  private handleIncomingMessage(topic: string, payloadBuffer: Buffer): void {
    const logMatch = /^futes\/([^/]+)\/log$/.exec(topic);
    if (logMatch) {
      this.handleIncomingLogMessage(logMatch[1], payloadBuffer);
      return;
    }

    const match = /^futes\/([^/]+)\/(status|telemetry)$/.exec(topic);
    if (!match) return;

    const deviceId = match[1];
    const messageType = match[2] as 'status' | 'telemetry';

    let parsed: HeatingStatusPayload;
    try {
      parsed = JSON.parse(payloadBuffer.toString('utf8')) as HeatingStatusPayload;
    } catch {
      console.warn(`[MQTT] Érvénytelen JSON a topic-on (${topic})`);
      return;
    }

    const nowMs = Date.now();
    const prev = this.states.get(deviceId);
    const fullPayload = isFullEspPayload(parsed);
    const savedCmd = this.lastCommands.get(deviceId);
    const mergedStatus = mergeHeatingStatus(prev?.lastStatus, parsed);

    // Az ESP32-től érkező status/telemetry a mérvadó (a backend soha nem írja felül lokális parancsból)
    const nextState: StoredDeviceState = {
      id: deviceId,
      lastStatus: mergedStatus,
      status: mergedStatus,
      lastCommand: savedCmd?.command ?? prev?.lastCommand ?? null,
      lastCommandAt: savedCmd?.sentAt ?? prev?.lastCommandAt ?? null,
      lastStatusAt: fullPayload ? nowMs : (prev?.lastStatusAt ?? null),
      lastTelemetryAt: messageType === 'telemetry' ? nowMs : (prev?.lastTelemetryAt ?? null),
      lastUpdatedAt: nowMs,
      alarmConfig: mergedStatus.alarmConfig ?? prev?.alarmConfig ?? null,
      alarmState: mergedStatus.alarmState ?? prev?.alarmState ?? null,
      heartbeat: mergedStatus.heartbeat ?? prev?.heartbeat ?? null,
    };

    this.states.set(deviceId, nextState);

    if (messageType === 'status' && typeof parsed.online === 'boolean') {
      const prevOnline = this.onlineStates.get(deviceId);
      if (parsed.online && prevOnline !== true) {
        this.logBackendEvent('INFO', 'ESP_ONLINE', `${deviceId} online`);
      } else if (!parsed.online && prevOnline !== false) {
        const reason = typeof parsed.error === 'string' && parsed.error ? parsed.error : 'offline';
        this.logBackendEvent('WARN', 'ESP_OFFLINE', `${deviceId}: ${reason}`);
      }
      this.onlineStates.set(deviceId, parsed.online);
    }

    const eventPayload: HeatingUpdateEvent = {
      id: deviceId,
      type: messageType,
      topic,
      receivedAt: new Date(nowMs).toISOString(),
      data: parsed,
      state: nextState,
    };

    this.io.emit('heating-update', eventPayload);
  }

  private handleIncomingLogMessage(deviceId: string, payloadBuffer: Buffer): void {
    let parsed: Record<string, unknown>;
    try {
      const raw = JSON.parse(payloadBuffer.toString('utf8')) as unknown;
      if (!raw || typeof raw !== 'object' || Array.isArray(raw)) return;
      parsed = raw as Record<string, unknown>;
    } catch {
      console.warn(`[MQTT] Érvénytelen log JSON (futes/${deviceId}/log)`);
      return;
    }

    const level = normalizeLogLevel(parsed.level);
    if (level !== 'ERROR') {
      return;
    }

    const code = typeof parsed.code === 'string' && parsed.code.trim() ? parsed.code.trim() : 'ESP_ERROR';
    const message = typeof parsed.message === 'string' ? parsed.message : '';
    const timestamp = typeof parsed.timestamp === 'string' ? parsed.timestamp : undefined;

    this.logsStore.add('ERROR', code, message, 'esp', timestamp);
  }

  public publishAction(deviceId: string, actionPayload: Record<string, unknown>): Promise<void> {
    return new Promise((resolve, reject) => {
      if (!this.client || !this.connected) {
        reject(new Error('Az MQTT kliens jelenleg nincs csatlakozva a HiveMQ Cloud brokerhez.'));
        return;
      }

      const topic = `futes/${deviceId}/cmd`;
      const payload = JSON.stringify(actionPayload);

      this.client.publish(topic, payload, { qos: 1, retain: false }, (err) => {
        if (err) {
          reject(err);
          return;
        }
        resolve();
      });
    });
  }

  public publishCommand(deviceId: string, command: HeatingCommand): Promise<void> {
    return new Promise((resolve, reject) => {
      if (!this.client || !this.connected) {
        reject(new Error('Az MQTT kliens jelenleg nincs csatlakozva a HiveMQ Cloud brokerhez.'));
        return;
      }

      const topic = `futes/${deviceId}/cmd`;
      const payload = JSON.stringify(command);

      // Fontos: a backend CSAK továbbítja a parancsot MQTT-re, nem módosítja a tárolt ESP státuszt.
      this.client.publish(topic, payload, { qos: 1, retain: false }, (err) => {
        if (err) {
          reject(err);
          return;
        }
        const sentAt = Date.now();
        this.lastCommands.set(deviceId, { command, sentAt });
        const existing = this.states.get(deviceId);
        if (existing) {
          existing.lastCommand = command;
          existing.lastCommandAt = sentAt;
        }
        resolve();
      });
    });
  }

  public getDeviceState(deviceId: string): StoredDeviceState | undefined {
    return this.states.get(deviceId);
  }

  public getAllStates(): StoredDeviceState[] {
    return Array.from(this.states.values());
  }

  public isConnected(): boolean {
    return this.connected;
  }

  public disconnect(): void {
    this.client?.end(true);
    this.connected = false;
  }
}
