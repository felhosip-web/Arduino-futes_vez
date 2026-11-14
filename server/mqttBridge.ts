import mqtt from 'mqtt';
import type { MqttClient } from 'mqtt';
import type { Server as SocketIOServer } from 'socket.io';
import type {
  HeatingCommand,
  HeatingStatusPayload,
  HeatingUpdateEvent,
  StoredDeviceState,
} from './types.ts';

export class HeatingMqttBridge {
  private client: MqttClient | null = null;
  private readonly io: SocketIOServer;
  private readonly states = new Map<string, StoredDeviceState>();
  private connected = false;

  constructor(io: SocketIOServer) {
    this.io = io;
  }

  public connect(): void {
    const brokerUrl = process.env.MQTT_BROKER_URL?.trim();
    const username = process.env.MQTT_USERNAME?.trim();
    const password = process.env.MQTT_PASSWORD?.trim();
    const clientId = process.env.MQTT_CLIENT_ID?.trim() || 'backend-futes';

    if (!brokerUrl || brokerUrl.includes('your-cluster-id')) {
      console.warn('[MQTT] MQTT_BROKER_URL nincs beállítva (.env) — az MQTT híd várakozik.');
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
      this.connected = true;
      console.log('[MQTT] Sikeresen csatlakozva a HiveMQ Cloud brokerhez.');
      this.client?.subscribe(['futes/+/status', 'futes/+/telemetry'], { qos: 1 }, (err) => {
        if (err) {
          console.error('[MQTT] Feliratkozási hiba:', err.message);
        } else {
          console.log('[MQTT] Feliratkozva: futes/+/status, futes/+/telemetry');
        }
      });
    });

    this.client.on('reconnect', () => {
      console.log('[MQTT] Újracsatlakozás folyamatban...');
    });

    this.client.on('close', () => {
      this.connected = false;
    });

    this.client.on('error', (err) => {
      console.error('[MQTT] Hiba:', err.message);
    });

    this.client.on('message', (topic, payloadBuffer) => {
      this.handleIncomingMessage(topic, payloadBuffer);
    });
  }

  private handleIncomingMessage(topic: string, payloadBuffer: Buffer): void {
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

    const nowIso = new Date().toISOString();
    const prev = this.states.get(deviceId);

    // Az ESP32-től érkező status/telemetry a mérvadó (soha nem írja felül a backend lokális parancsból)
    const nextState: StoredDeviceState = {
      id: deviceId,
      status: prev ? { ...prev.status, ...parsed } : parsed,
      lastStatusAt: messageType === 'status' ? nowIso : (prev?.lastStatusAt ?? null),
      lastTelemetryAt: messageType === 'telemetry' ? nowIso : (prev?.lastTelemetryAt ?? null),
      lastUpdatedAt: nowIso,
    };

    this.states.set(deviceId, nextState);

    const eventPayload: HeatingUpdateEvent = {
      id: deviceId,
      type: messageType,
      topic,
      receivedAt: nowIso,
      data: parsed,
      state: nextState,
    };

    this.io.emit('heating-update', eventPayload);
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
