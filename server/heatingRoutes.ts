import { timingSafeEqual } from 'node:crypto';
import { Router, type NextFunction, type Request, type Response } from 'express';
import {
  DEFAULT_RESOLUTION_FOR_RANGE,
  normalizeHistoryRange,
  normalizeHistoryResolution,
  normalizeHistorySensor,
} from './historyStore.ts';
import type { HistoryRange, HistoryResolution, HistorySensorFilter } from './historyStore.ts';
import { normalizeLogLevel } from './logsStore.ts';
import type { LogLevel } from './logsStore.ts';
import type { HeatingMqttBridge } from './mqttBridge.ts';
import {
  buildAlarmUpdateCommand,
  buildHeartbeatUpdateCommand,
  buildPumpUpdateCommand,
  buildSingleOutputCommand,
  hasValidEspStatus,
  validateHeatingCommand,
  validateSingleOutputBody,
} from './validation.ts';

export interface HeatingRouterOptions {
  host?: string;
  apiKey?: string;
}

const deviceLocks = new Map<string, Promise<void>>();

async function withDeviceLock<T>(deviceId: string, fn: () => Promise<T>): Promise<T> {
  const prev = deviceLocks.get(deviceId) ?? Promise.resolve();
  let release!: () => void;
  const current = new Promise<void>((resolve) => {
    release = resolve;
  });
  const tail = prev.then(() => current);
  deviceLocks.set(deviceId, tail);

  await prev;
  try {
    return await fn();
  } finally {
    release();
    if (deviceLocks.get(deviceId) === tail) {
      deviceLocks.delete(deviceId);
    }
  }
}

function isValidBearerToken(authHeader: string | undefined, expectedKey: string): boolean {
  if (!authHeader || !expectedKey) return false;
  const match = /^Bearer\s+(.+)$/i.exec(authHeader.trim());
  if (!match) return false;
  const providedBuf = Buffer.from(match[1].trim(), 'utf8');
  const expectedBuf = Buffer.from(expectedKey, 'utf8');
  if (providedBuf.length !== expectedBuf.length) return false;
  return timingSafeEqual(providedBuf, expectedBuf);
}

function getClientIp(req: Request): string {
  return req.ip || req.socket?.remoteAddress || 'unknown';
}

export function createHeatingRouter(
  bridge: HeatingMqttBridge,
  options: HeatingRouterOptions = {}
): Router {
  const router = Router();
  const host = (options.host ?? process.env.HOST ?? '127.0.0.1').trim() || '127.0.0.1';
  const apiKey = (options.apiKey ?? process.env.HEATING_API_KEY ?? '').trim();
  const isLocalhost = host === '127.0.0.1' || host === 'localhost' || host === '::1';

  const requireBearerAuth = (req: Request, res: Response, next: NextFunction): void => {
    if (!apiKey && isLocalhost) {
      next();
      return;
    }
    if (!isValidBearerToken(req.headers.authorization, apiKey)) {
      bridge.logBackendEvent(
        'WARN',
        'API_401',
        `Unauthorized ${req.method} ${req.originalUrl} IP=${getClientIp(req)}`
      );
      res.status(401).json({ error: 'Unauthorized' });
      return;
    }
    next();
  };

  const requireGetAuth = (req: Request, res: Response, next: NextFunction): void => {
    if (isLocalhost) {
      next();
      return;
    }
    if (!isValidBearerToken(req.headers.authorization, apiKey)) {
      bridge.logBackendEvent(
        'WARN',
        'API_401',
        `Unauthorized ${req.method} ${req.originalUrl} IP=${getClientIp(req)}`
      );
      res.status(401).json({ error: 'Unauthorized' });
      return;
    }
    next();
  };

  // Minden POST /api/heating/:id/* végpont védelme Bearer tokennel
  router.post('/:id/*', requireBearerAuth);

  // GET /api/heating/:id/logs/export - szöveges (.txt) napló letöltés (Bearer auth-os)
  router.get('/:id/logs/export', requireBearerAuth, (req, res) => {
    const { id } = req.params;
    const rawLevel = typeof req.query.level === 'string' ? req.query.level.trim() : '';
    let levelFilter: LogLevel | undefined;
    if (rawLevel.length > 0) {
      const parsedLevel = normalizeLogLevel(rawLevel);
      if (!parsedLevel) {
        res.status(400).json({ error: 'Érvénytelen level szűrő. Engedélyezett: INFO, WARN, ERROR.' });
        return;
      }
      levelFilter = parsedLevel;
    }

    const safeId = id.replace(/[^a-zA-Z0-9_-]/g, '_') || 'device';
    const text = bridge.getLogsStore().exportText(levelFilter);
    res.setHeader('Content-Type', 'text/plain; charset=utf-8');
    res.setHeader('Content-Disposition', `attachment; filename="heating-logs-${safeId}.txt"`);
    res.send(text);
  });

  // GET /api/heating/:id/logs?page=1&limit=20&level=ERROR (Bearer auth-os)
  router.get('/:id/logs', requireBearerAuth, (req, res) => {
    const pageRaw = typeof req.query.page === 'string' ? Number(req.query.page) : 1;
    const limitRaw = typeof req.query.limit === 'string' ? Number(req.query.limit) : 20;
    const rawLevel = typeof req.query.level === 'string' ? req.query.level.trim() : '';

    let levelFilter: LogLevel | undefined;
    if (rawLevel.length > 0) {
      const parsedLevel = normalizeLogLevel(rawLevel);
      if (!parsedLevel) {
        res.status(400).json({ error: 'Érvénytelen level szűrő. Engedélyezett: INFO, WARN, ERROR.' });
        return;
      }
      levelFilter = parsedLevel;
    }

    const result = bridge.getLogsStore().query({
      page: Number.isFinite(pageRaw) ? pageRaw : 1,
      limit: Number.isFinite(limitRaw) ? limitRaw : 20,
      level: levelFilter,
    });

    res.json(result);
  });

  // DELETE /api/heating/:id/logs - backend + ESP naplópuffer kiürítése (Bearer auth-os)
  router.delete('/:id/logs', requireBearerAuth, async (req, res) => {
    const { id } = req.params;
    const deleted = bridge.getLogsStore().clear();

    if (bridge.isConnected()) {
      try {
        await bridge.publishAction(id, { action: 'clearLogs' });
      } catch (err) {
        console.warn('[MQTT] ESP clearLogs parancs küldése sikertelen:', err);
      }
    }

    bridge.logBackendEvent('INFO', 'LOGS_CLEARED_BY_USER', `Naplo uritve (${id})`);

    res.json({ ok: true, deleted });
  });

  // POST /api/heating/:id/sync - azonnali ESP-Nano időszinkronizáció indítása (Bearer auth-os)
  router.post('/:id/sync', async (req, res) => {
    const { id } = req.params;
    const body = req.body as Record<string, unknown> | undefined;
    if (body && typeof body === 'object' && !Array.isArray(body) && body.action !== undefined) {
      if (body.action !== 'forceSync') {
        res.status(400).json({ error: 'Érvénytelen action. Engedélyezett: forceSync.' });
        return;
      }
    }

    try {
      await bridge.publishAction(id, { action: 'forceSync' });
      bridge.logBackendEvent('INFO', 'SYNC_FORCED_BY_USER', `Idoszinkron inditva (${id})`);
      res.json({
        ok: true,
        topic: `futes/${id}/cmd`,
        action: 'forceSync',
      });
    } catch (err) {
      const message = err instanceof Error ? err.message : 'MQTT küldési hiba.';
      res.status(503).json({ error: message });
    }
  });

  // POST /api/heating/:id/trend/reset - előremenő max/min/átlag statisztika nullázása (Bearer auth-os)
  router.post('/:id/trend/reset', async (req, res) => {
    const { id } = req.params;

    try {
      await bridge.publishAction(id, { action: 'resetTrend' });
      bridge.logBackendEvent('INFO', 'TREND_RESET_BY_USER', `Eloremeno statisztika nullazva (${id})`);
      res.json({ ok: true });
    } catch (err) {
      const message = err instanceof Error ? err.message : 'MQTT küldési hiba.';
      res.status(503).json({ error: message });
    }
  });

  // GET /api/heating/:id/history?range=1h|6h|24h|7d&resolution=1m|5m|15m|1h&sensor=all|... (Bearer auth-os)
  router.get('/:id/history', requireBearerAuth, (req, res) => {
    const { id } = req.params;
    const rawRange = typeof req.query.range === 'string' ? req.query.range.trim() : '';
    const rawResolution =
      typeof req.query.resolution === 'string' ? req.query.resolution.trim() : '';
    const rawSensor = typeof req.query.sensor === 'string' ? req.query.sensor.trim() : '';

    let range: HistoryRange = '24h';
    if (rawRange.length > 0) {
      const parsed = normalizeHistoryRange(rawRange);
      if (!parsed) {
        res.status(400).json({ error: 'Érvénytelen range. Engedélyezett: 1h, 6h, 24h, 7d.' });
        return;
      }
      range = parsed;
    }

    let resolution: HistoryResolution = DEFAULT_RESOLUTION_FOR_RANGE[range];
    if (rawResolution.length > 0) {
      const parsed = normalizeHistoryResolution(rawResolution);
      if (!parsed) {
        res.status(400).json({ error: 'Érvénytelen resolution. Engedélyezett: 1m, 5m, 15m, 1h.' });
        return;
      }
      resolution = parsed;
    }

    let sensor: HistorySensorFilter = 'all';
    if (rawSensor.length > 0) {
      const parsed = normalizeHistorySensor(rawSensor);
      if (!parsed) {
        res.status(400).json({
          error:
            'Érvénytelen sensor. Engedélyezett: all, eloremeno, visszatero, kazan, kulteri, szoba.',
        });
        return;
      }
      sensor = parsed;
    }

    const result = bridge.getHistoryStore(id).query({
      range,
      resolution,
      sensor,
    });

    res.json(result);
  });

  // Összes ismert eszköz állapota
  router.get('/', requireGetAuth, (_req, res) => {
    res.json({
      mqttConnected: bridge.isConnected(),
      devices: bridge.getAllStates(),
    });
  });

  const handleGetStatus = (req: Request, res: Response): void => {
    const { id } = req.params;
    const state = bridge.getDeviceState(id);

    if (!state) {
      res.status(404).json({
        error: `Még nem érkezett állapot az eszköztől (${id}).`,
      });
      return;
    }

    res.json(state);
  };

  // GET /api/heating/:id és /api/heating/:id/status - utolsó ismert ESP állapot lekérdezése
  router.get('/:id', requireGetAuth, handleGetStatus);
  router.get('/:id/status', requireGetAuth, handleGetStatus);

  // POST /api/heating/:id/command - teljes vagy részleges CMD JSON validálása és továbbítása MQTT-re + CMD_ACK
  router.post('/:id/command', async (req, res) => {
    const { id } = req.params;

    try {
      const validationOptions = {
        onWarn: (code: string, message: string) => bridge.logBackendEvent('WARN', code, message),
      };

      const result = await withDeviceLock(id, async () => {
        const current = bridge.getDeviceState(id);
        const validation = validateHeatingCommand(req.body, current, validationOptions);
        if (!validation.ok) {
          return { ok: false as const, error: validation.error };
        }
        const meta = bridge.generateCmdId();
        const waiter = bridge.createCommandAckWaiter(meta.cmdId, 3000);
        try {
          await bridge.publishCommand(id, validation.value, meta);
        } catch (err) {
          waiter.cancel();
          throw err;
        }
        return {
          ok: true as const,
          command: validation.value,
          cmdId: meta.cmdId,
          ackPromise: waiter.promise,
        };
      });

      if (!result.ok) {
        res.status(400).json({ error: result.error });
        return;
      }

      const ack = await result.ackPromise;
      if (!ack.acked) {
        res.status(504).json({
          ok: false,
          acked: false,
          cmdId: ack.cmdId,
          reason: ack.reason || 'ACK_TIMEOUT',
          ...(ack.pumpOn !== undefined ? { pumpOn: ack.pumpOn } : {}),
        });
        return;
      }

      bridge.logBackendEvent('INFO', 'API_CMD', `POST /command OK (cmdId=${ack.cmdId})`);

      res.json({
        ok: true,
        acked: true,
        cmdId: ack.cmdId,
        ...(ack.pumpOn !== undefined ? { pumpOn: ack.pumpOn } : {}),
        ...(ack.executedAt !== undefined ? { executedAt: ack.executedAt } : {}),
        topic: `futes/${id}/cmd`,
        command: result.command,
      });
    } catch (err) {
      const message = err instanceof Error ? err.message : 'MQTT küldési hiba.';
      res.status(503).json({ error: message });
    }
  });

  // POST /api/heating/:id/kimenet - egyetlen kimenet bit gyors kapcsolása sorosított eszköz-mutexszel + CMD_ACK
  router.post('/:id/kimenet', async (req, res) => {
    const { id } = req.params;
    const validation = validateSingleOutputBody(req.body);

    if (!validation.ok) {
      res.status(400).json({ error: validation.error });
      return;
    }

    try {
      const result = await withDeviceLock(id, async () => {
        const current = bridge.getDeviceState(id);
        if (!hasValidEspStatus(current)) {
          return { conflict: true as const };
        }

        const fullCommand = buildSingleOutputCommand(current, validation.value);
        const meta = bridge.generateCmdId();
        const waiter = bridge.createCommandAckWaiter(meta.cmdId, 3000);
        try {
          await bridge.publishCommand(id, fullCommand, meta);
        } catch (err) {
          waiter.cancel();
          throw err;
        }
        return {
          conflict: false as const,
          fullCommand,
          cmdId: meta.cmdId,
          ackPromise: waiter.promise,
        };
      });

      if (result.conflict) {
        res.status(409).json({
          error: 'Nincs még ESP állapot',
        });
        return;
      }

      const ack = await result.ackPromise;
      if (!ack.acked) {
        res.status(504).json({
          ok: false,
          acked: false,
          cmdId: ack.cmdId,
          reason: ack.reason || 'ACK_TIMEOUT',
          ...(ack.pumpOn !== undefined ? { pumpOn: ack.pumpOn } : {}),
        });
        return;
      }

      res.json({
        ok: true,
        acked: true,
        cmdId: ack.cmdId,
        ...(ack.pumpOn !== undefined ? { pumpOn: ack.pumpOn } : {}),
        ...(ack.executedAt !== undefined ? { executedAt: ack.executedAt } : {}),
        topic: `futes/${id}/cmd`,
        changed: validation.value,
        command: result.fullCommand,
      });
    } catch (err) {
      const message = err instanceof Error ? err.message : 'MQTT küldési hiba.';
      res.status(503).json({ error: message });
    }
  });

  // POST /api/heating/:id/pump - csak a pumpConfig módosítása + CMD_ACK
  router.post('/:id/pump', async (req, res) => {
    const { id } = req.params;

    if (
      !req.body ||
      typeof req.body !== 'object' ||
      Array.isArray(req.body) ||
      !(req.body as Record<string, unknown>).pumpConfig ||
      typeof (req.body as Record<string, unknown>).pumpConfig !== 'object' ||
      Array.isArray((req.body as Record<string, unknown>).pumpConfig)
    ) {
      res.status(400).json({ error: 'A kérés törzsének tartalmaznia kell egy érvényes pumpConfig objektumot.' });
      return;
    }

    try {
      const validationOptions = {
        onWarn: (code: string, message: string) => bridge.logBackendEvent('WARN', code, message),
      };

      const result = await withDeviceLock(id, async () => {
        const current = bridge.getDeviceState(id);
        if (!hasValidEspStatus(current)) {
          return { status: 409 as const, error: 'Nincs még ESP állapot' };
        }

        const built = buildPumpUpdateCommand(current, req.body, validationOptions);
        if (!built.ok) {
          return { status: 400 as const, error: built.error };
        }

        const meta = bridge.generateCmdId();
        const waiter = bridge.createCommandAckWaiter(meta.cmdId, 3000);
        try {
          await bridge.publishCommand(id, built.value, meta);
        } catch (err) {
          waiter.cancel();
          throw err;
        }
        return {
          status: 200 as const,
          command: built.value,
          cmdId: meta.cmdId,
          ackPromise: waiter.promise,
        };
      });

      if (result.status !== 200) {
        res.status(result.status).json({ error: result.error });
        return;
      }

      const ack = await result.ackPromise;
      if (!ack.acked) {
        res.status(504).json({
          ok: false,
          acked: false,
          cmdId: ack.cmdId,
          reason: ack.reason || 'ACK_TIMEOUT',
          ...(ack.pumpOn !== undefined ? { pumpOn: ack.pumpOn } : {}),
        });
        return;
      }

      res.json({
        ok: true,
        acked: true,
        cmdId: ack.cmdId,
        ...(ack.pumpOn !== undefined ? { pumpOn: ack.pumpOn } : {}),
        ...(ack.executedAt !== undefined ? { executedAt: ack.executedAt } : {}),
        topic: `futes/${id}/cmd`,
        pumpConfig: result.command.pumpConfig,
        command: result.command,
      });
    } catch (err) {
      const message = err instanceof Error ? err.message : 'MQTT küldési hiba.';
      res.status(503).json({ error: message });
    }
  });

  // POST /api/heating/:id/alarm - csak az alarmConfig módosítása + CMD_ACK
  router.post('/:id/alarm', async (req, res) => {
    const { id } = req.params;

    if (
      !req.body ||
      typeof req.body !== 'object' ||
      Array.isArray(req.body) ||
      !(req.body as Record<string, unknown>).alarmConfig ||
      typeof (req.body as Record<string, unknown>).alarmConfig !== 'object' ||
      Array.isArray((req.body as Record<string, unknown>).alarmConfig)
    ) {
      res.status(400).json({ error: 'A kérés törzsének tartalmaznia kell egy érvényes alarmConfig objektumot.' });
      return;
    }

    try {
      const validationOptions = {
        onWarn: (code: string, message: string) => bridge.logBackendEvent('WARN', code, message),
      };

      const result = await withDeviceLock(id, async () => {
        const current = bridge.getDeviceState(id);
        if (!hasValidEspStatus(current)) {
          return { status: 409 as const, error: 'Nincs még ESP állapot' };
        }

        const built = buildAlarmUpdateCommand(current, req.body, validationOptions);
        if (!built.ok) {
          return { status: 400 as const, error: built.error };
        }

        const meta = bridge.generateCmdId();
        const waiter = bridge.createCommandAckWaiter(meta.cmdId, 3000);
        try {
          await bridge.publishCommand(id, built.value, meta);
        } catch (err) {
          waiter.cancel();
          throw err;
        }
        return {
          status: 200 as const,
          command: built.value,
          cmdId: meta.cmdId,
          ackPromise: waiter.promise,
        };
      });

      if (result.status !== 200) {
        res.status(result.status).json({ error: result.error });
        return;
      }

      const ack = await result.ackPromise;
      if (!ack.acked) {
        res.status(504).json({
          ok: false,
          acked: false,
          cmdId: ack.cmdId,
          reason: ack.reason || 'ACK_TIMEOUT',
          ...(ack.pumpOn !== undefined ? { pumpOn: ack.pumpOn } : {}),
        });
        return;
      }

      res.json({
        ok: true,
        acked: true,
        cmdId: ack.cmdId,
        ...(ack.pumpOn !== undefined ? { pumpOn: ack.pumpOn } : {}),
        ...(ack.executedAt !== undefined ? { executedAt: ack.executedAt } : {}),
        topic: `futes/${id}/cmd`,
        alarmConfig: result.command.alarmConfig,
        command: result.command,
      });
    } catch (err) {
      const message = err instanceof Error ? err.message : 'MQTT küldési hiba.';
      res.status(503).json({ error: message });
    }
  });

  // POST /api/heating/:id/heartbeat - csak a heartbeat módosítása + CMD_ACK
  router.post('/:id/heartbeat', async (req, res) => {
    const { id } = req.params;

    if (
      !req.body ||
      typeof req.body !== 'object' ||
      Array.isArray(req.body) ||
      !(req.body as Record<string, unknown>).heartbeat ||
      typeof (req.body as Record<string, unknown>).heartbeat !== 'object' ||
      Array.isArray((req.body as Record<string, unknown>).heartbeat)
    ) {
      res.status(400).json({ error: 'A kérés törzsének tartalmaznia kell egy érvényes heartbeat objektumot.' });
      return;
    }

    try {
      const validationOptions = {
        onWarn: (code: string, message: string) => bridge.logBackendEvent('WARN', code, message),
      };

      const result = await withDeviceLock(id, async () => {
        const current = bridge.getDeviceState(id);
        if (!hasValidEspStatus(current)) {
          return { status: 409 as const, error: 'Nincs még ESP állapot' };
        }

        const built = buildHeartbeatUpdateCommand(current, req.body, validationOptions);
        if (!built.ok) {
          return { status: 400 as const, error: built.error };
        }

        const meta = bridge.generateCmdId();
        const waiter = bridge.createCommandAckWaiter(meta.cmdId, 3000);
        try {
          await bridge.publishCommand(id, built.value, meta);
        } catch (err) {
          waiter.cancel();
          throw err;
        }
        return {
          status: 200 as const,
          command: built.value,
          cmdId: meta.cmdId,
          ackPromise: waiter.promise,
        };
      });

      if (result.status !== 200) {
        res.status(result.status).json({ error: result.error });
        return;
      }

      const ack = await result.ackPromise;
      if (!ack.acked) {
        res.status(504).json({
          ok: false,
          acked: false,
          cmdId: ack.cmdId,
          reason: ack.reason || 'ACK_TIMEOUT',
          ...(ack.pumpOn !== undefined ? { pumpOn: ack.pumpOn } : {}),
        });
        return;
      }

      bridge.logBackendEvent(
        'INFO',
        'API_HEARTBEAT',
        `POST /heartbeat OK (${result.command.heartbeat.intervalSec}s, cmdId=${ack.cmdId})`
      );

      res.json({
        ok: true,
        acked: true,
        cmdId: ack.cmdId,
        ...(ack.pumpOn !== undefined ? { pumpOn: ack.pumpOn } : {}),
        ...(ack.executedAt !== undefined ? { executedAt: ack.executedAt } : {}),
        topic: `futes/${id}/cmd`,
        heartbeat: result.command.heartbeat,
        command: result.command,
      });
    } catch (err) {
      const message = err instanceof Error ? err.message : 'MQTT küldési hiba.';
      res.status(503).json({ error: message });
    }
  });

  return router;
}
