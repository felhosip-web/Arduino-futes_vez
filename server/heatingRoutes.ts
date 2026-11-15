import { Router } from 'express';
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

export function createHeatingRouter(bridge: HeatingMqttBridge): Router {
  const router = Router();

  // Összes ismert eszköz állapota
  router.get('/', (_req, res) => {
    res.json({
      mqttConnected: bridge.isConnected(),
      devices: bridge.getAllStates(),
    });
  });

  // GET /api/heating/:id - utolsó ismert ESP állapot lekérdezése
  router.get('/:id', (req, res) => {
    const { id } = req.params;
    const state = bridge.getDeviceState(id);

    if (!state) {
      res.status(404).json({
        error: `Még nem érkezett állapot az eszköztől (${id}).`,
      });
      return;
    }

    res.json(state);
  });

  // POST /api/heating/:id/command - teljes vagy részleges CMD JSON validálása és továbbítása MQTT-re
  router.post('/:id/command', async (req, res) => {
    const { id } = req.params;

    try {
      const result = await withDeviceLock(id, async () => {
        const current = bridge.getDeviceState(id);
        const validation = validateHeatingCommand(req.body, current);
        if (!validation.ok) {
          return { ok: false as const, error: validation.error };
        }
        await bridge.publishCommand(id, validation.value);
        return { ok: true as const, command: validation.value };
      });

      if (!result.ok) {
        res.status(400).json({ error: result.error });
        return;
      }

      res.json({
        ok: true,
        topic: `futes/${id}/cmd`,
        command: result.command,
      });
    } catch (err) {
      const message = err instanceof Error ? err.message : 'MQTT küldési hiba.';
      res.status(503).json({ error: message });
    }
  });

  // POST /api/heating/:id/kimenet - egyetlen kimenet bit gyors kapcsolása sorosított eszköz-mutexszel
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
        await bridge.publishCommand(id, fullCommand);
        return { conflict: false as const, fullCommand };
      });

      if (result.conflict) {
        res.status(409).json({
          error: 'Nincs még ESP állapot',
        });
        return;
      }

      res.json({
        ok: true,
        topic: `futes/${id}/cmd`,
        changed: validation.value,
        command: result.fullCommand,
      });
    } catch (err) {
      const message = err instanceof Error ? err.message : 'MQTT küldési hiba.';
      res.status(503).json({ error: message });
    }
  });

  // POST /api/heating/:id/pump - csak a pumpConfig módosítása, a többi mező a frissebb forrásból
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
      const result = await withDeviceLock(id, async () => {
        const current = bridge.getDeviceState(id);
        if (!hasValidEspStatus(current)) {
          return { status: 409 as const, error: 'Nincs még ESP állapot' };
        }

        const built = buildPumpUpdateCommand(current, req.body);
        if (!built.ok) {
          return { status: 400 as const, error: built.error };
        }

        await bridge.publishCommand(id, built.value);
        return { status: 200 as const, command: built.value };
      });

      if (result.status !== 200) {
        res.status(result.status).json({ error: result.error });
        return;
      }

      res.json({
        ok: true,
        topic: `futes/${id}/cmd`,
        pumpConfig: result.command.pumpConfig,
        command: result.command,
      });
    } catch (err) {
      const message = err instanceof Error ? err.message : 'MQTT küldési hiba.';
      res.status(503).json({ error: message });
    }
  });

  // POST /api/heating/:id/alarm - csak az alarmConfig módosítása, a többi mező a frissebb forrásból
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
      const result = await withDeviceLock(id, async () => {
        const current = bridge.getDeviceState(id);
        if (!hasValidEspStatus(current)) {
          return { status: 409 as const, error: 'Nincs még ESP állapot' };
        }

        const built = buildAlarmUpdateCommand(current, req.body);
        if (!built.ok) {
          return { status: 400 as const, error: built.error };
        }

        await bridge.publishCommand(id, built.value);
        return { status: 200 as const, command: built.value };
      });

      if (result.status !== 200) {
        res.status(result.status).json({ error: result.error });
        return;
      }

      res.json({
        ok: true,
        topic: `futes/${id}/cmd`,
        alarmConfig: result.command.alarmConfig,
        command: result.command,
      });
    } catch (err) {
      const message = err instanceof Error ? err.message : 'MQTT küldési hiba.';
      res.status(503).json({ error: message });
    }
  });

  // POST /api/heating/:id/heartbeat - csak a heartbeat módosítása, a többi mező a frissebb forrásból
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
      const result = await withDeviceLock(id, async () => {
        const current = bridge.getDeviceState(id);
        if (!hasValidEspStatus(current)) {
          return { status: 409 as const, error: 'Nincs még ESP állapot' };
        }

        const built = buildHeartbeatUpdateCommand(current, req.body);
        if (!built.ok) {
          return { status: 400 as const, error: built.error };
        }

        await bridge.publishCommand(id, built.value);
        return { status: 200 as const, command: built.value };
      });

      if (result.status !== 200) {
        res.status(result.status).json({ error: result.error });
        return;
      }

      res.json({
        ok: true,
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
