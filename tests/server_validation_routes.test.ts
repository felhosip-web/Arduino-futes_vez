import assert from 'node:assert/strict';
import http from 'node:http';
import test from 'node:test';
import express from 'express';
import { createHeatingRouter } from '../server/heatingRoutes.ts';
import type { HeatingMqttBridge } from '../server/mqttBridge.ts';
import type { CmdAckResult, StoredDeviceState } from '../server/types.ts';
import {
  DEFAULT_ALARM_CONFIG,
  DEFAULT_HEARTBEAT_CONFIG,
  DEFAULT_PUMP_CONFIG,
  DEFAULT_SAFETY_CONFIG,
  computeOneWireCrc8,
  validateDs18b20RomAddress,
} from '../server/validation.ts';

function makeValidRomString(serial1: number, serial2: number, serial6: number): string {
  const prefix = [0x28, serial1 & 0xff, serial2 & 0xff, 0x11, 0x22, 0x33, serial6 & 0xff];
  const crc = computeOneWireCrc8(prefix);
  return [...prefix, crc].map((b) => b.toString(16).toUpperCase().padStart(2, '0')).join(':');
}

function createBaseDeviceState(overrides?: Partial<StoredDeviceState>): StoredDeviceState {
  const baseStatus = {
    online: true,
    currentTemp: 21,
    targetTemp: 21.5,
    mode: 'auto' as const,
    heatingOn: false,
    manualHeating: false,
    pumpState: 'varakozas' as const,
    pumpOn: false,
    relayFeedbackOn: false,
    relayFault: false,
    serviceMode: false,
    nrf24Ok: true,
    nrf24_ok: true,
    nanoCounter: 1,
    kazan_status: [0, 0, 0, 0, 0, 0, 0, 0],
    error: null,
    homersekletek: {
      eloremeno: 30,
      visszatero: 25,
      kazan: 40,
      kulteri: 5,
      szoba: 21,
    },
    kimenetek: {
      szivattyu: false,
      led_zold: false,
      led_piros: false,
      tartalek1: false,
      tartalek2: false,
      tartalek3: false,
      tartalek4: false,
      tartalek5: false,
    },
    pumpConfig: { ...DEFAULT_PUMP_CONFIG },
    alarmConfig: { ...DEFAULT_ALARM_CONFIG },
    safety: { ...DEFAULT_SAFETY_CONFIG },
    alarmState: { active: false, reason: 'none' as const },
    heartbeat: { ...DEFAULT_HEARTBEAT_CONFIG },
    sensors: {
      eloremeno: { address: makeValidRomString(0x01, 0x01, 0x01), ok: true },
      visszatero: { address: makeValidRomString(0x02, 0x02, 0x02), ok: true },
      kazan: { address: makeValidRomString(0x03, 0x03, 0x03), ok: true },
      kulteri: { address: makeValidRomString(0x04, 0x04, 0x04), ok: true },
    },
  };

  return {
    id: 'haz-futes',
    lastStatus: baseStatus,
    status: baseStatus,
    lastCommand: null,
    lastCommandAt: null,
    lastStatusAt: Date.now(),
    lastTelemetryAt: Date.now(),
    lastUpdatedAt: Date.now(),
    alarmConfig: { ...DEFAULT_ALARM_CONFIG },
    alarmState: { active: false, reason: 'none' },
    heartbeat: { ...DEFAULT_HEARTBEAT_CONFIG },
    ...overrides,
  };
}

function createMockBridge(deviceState: StoredDeviceState | undefined) {
  const publishedActions: Array<{ deviceId: string; payload: Record<string, unknown> }> = [];
  const logs: Array<{ level: string; code: string; message: string }> = [];

  const mockBridge = {
    getDeviceState: (id: string) => (id === deviceState?.id ? deviceState : undefined),
    generateCmdId: () => ({ cmdId: 'cmd_test_1', cmdSeq: 42 }),
    createCommandAckWaiter: (cmdId: string) => {
      const ack: CmdAckResult = {
        cmdId,
        ok: true,
        acked: true,
        executedAt: new Date().toISOString(),
      };
      return {
        promise: Promise.resolve(ack),
        cancel: () => {},
      };
    },
    publishAction: async (deviceId: string, payload: Record<string, unknown>) => {
      publishedActions.push({ deviceId, payload });
    },
    logBackendEvent: (level: string, code: string, message: string) => {
      logs.push({ level, code, message });
    },
  } as unknown as HeatingMqttBridge;

  return { mockBridge, publishedActions, logs };
}

test('validateDs18b20RomAddress ellenőrzi a formátumot, a 0x28 family code-ot és a CRC8-at', () => {
  const valid = makeValidRomString(0xaa, 0xbb, 0x07);
  const okRes = validateDs18b20RomAddress(` ${valid.toLowerCase()} `);
  assert.equal(okRes.ok, true);
  if (okRes.ok) {
    assert.equal(okRes.value, valid);
  }

  // Hibás family code (0x10 DS18S20), még érvényes CRC8 mellett is elutasítva
  const wrongFamilyPrefix = [0x10, 0xaa, 0xbb, 0x11, 0x22, 0x33, 0x07];
  const wrongFamilyCrc = computeOneWireCrc8(wrongFamilyPrefix);
  const wrongFamilyAddr = [...wrongFamilyPrefix, wrongFamilyCrc]
    .map((b) => b.toString(16).toUpperCase().padStart(2, '0'))
    .join(':');
  const familyFail = validateDs18b20RomAddress(wrongFamilyAddr);
  assert.equal(familyFail.ok, false);

  // Rossz CRC8
  const badCrcBytes = valid.split(':');
  const flipped = (Number.parseInt(badCrcBytes[7], 16) ^ 0x01)
    .toString(16)
    .toUpperCase()
    .padStart(2, '0');
  badCrcBytes[7] = flipped;
  const crcFail = validateDs18b20RomAddress(badCrcBytes.join(':'));
  assert.equal(crcFail.ok, false);

  // Nullás / hibás formátum
  assert.equal(validateDs18b20RomAddress('00:00:00:00:00:00:00:00').ok, false);
  assert.equal(validateDs18b20RomAddress('28:AA:BB:CC').ok, false);
  assert.equal(validateDs18b20RomAddress(null).ok, false);
});

test('POST /sensor/learn elutasítja a rossz CRC8-as és nem-0x28 címeket, kezeli az allowReassign ütközést', async () => {
  const state = createBaseDeviceState();
  const { mockBridge, publishedActions } = createMockBridge(state);

  const app = express();
  app.use(express.json());
  app.use('/api/heating', createHeatingRouter(mockBridge, { host: '127.0.0.1' }));

  const server = http.createServer(app);
  await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', () => resolve()));
  const addressInfo = server.address();
  assert(addressInfo && typeof addressInfo === 'object');
  const baseUrl = `http://127.0.0.1:${addressInfo.port}`;

  try {
    // 1. Rossz CRC8 -> 400, semmit nem küld MQTT-re
    const validNew = makeValidRomString(0x55, 0x66, 0x09);
    const badCrc = `${validNew.slice(0, -2)}00`;
    const actualCrc = validNew.slice(-2);
    const guaranteedBadCrc = actualCrc === '00' ? `${validNew.slice(0, -2)}FF` : badCrc;

    const badCrcRes = await fetch(`${baseUrl}/api/heating/haz-futes/sensor/learn`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ role: 'eloremeno', address: guaranteedBadCrc }),
    });
    assert.equal(badCrcRes.status, 400);
    assert.equal(publishedActions.length, 0);

    // 2. Rossz family code (0x10 érvényes CRC8-al) -> 400
    const wfPrefix = [0x10, 0x55, 0x66, 0x11, 0x22, 0x33, 0x09];
    const wfAddr = [...wfPrefix, computeOneWireCrc8(wfPrefix)]
      .map((b) => b.toString(16).toUpperCase().padStart(2, '0'))
      .join(':');
    const wfRes = await fetch(`${baseUrl}/api/heating/haz-futes/sensor/learn`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ role: 'eloremeno', address: wfAddr }),
    });
    assert.equal(wfRes.status, 400);
    assert.equal(publishedActions.length, 0);

    // 3. Már kazan-hoz rendelt cím allowReassign nélkül -> 409 ADDRESS_ALREADY_USED
    const kazanAddr = state.lastStatus!.sensors!.kazan.address!;
    const dupRes = await fetch(`${baseUrl}/api/heating/haz-futes/sensor/learn`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ role: 'eloremeno', address: kazanAddr }),
    });
    assert.equal(dupRes.status, 409);
    const dupBody = (await dupRes.json()) as { error: string; oldRole: string };
    assert.equal(dupBody.error, 'ADDRESS_ALREADY_USED');
    assert.equal(dupBody.oldRole, 'kazan');
    assert.equal(publishedActions.length, 0);

    // 4. allowReassign: true -> továbbítja az MQTT parancsot és visszaadja reassignedFrom-ot
    const reassignRes = await fetch(`${baseUrl}/api/heating/haz-futes/sensor/learn`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({
        role: 'eloremeno',
        address: kazanAddr.toLowerCase(),
        allowReassign: true,
      }),
    });
    assert.equal(reassignRes.status, 200);
    const reassignBody = (await reassignRes.json()) as {
      ok: boolean;
      address: string;
      reassignedFrom: string;
    };
    assert.equal(reassignBody.ok, true);
    assert.equal(reassignBody.address, kazanAddr);
    assert.equal(reassignBody.reassignedFrom, 'kazan');
    assert.equal(publishedActions.length, 1);
    assert.equal(publishedActions[0].payload.address, kazanAddr);
  } finally {
    await new Promise<void>((resolve, reject) =>
      server.close((err) => (err ? reject(err) : resolve()))
    );
  }
});
