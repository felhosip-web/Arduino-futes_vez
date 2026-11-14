import { Router } from 'express';
import type { HeatingMqttBridge } from './mqttBridge.ts';
import type { HeatingCommand } from './types.ts';
import { validateHeatingCommand, validateSingleOutputBody } from './validation.ts';

export function createHeatingRouter(bridge: HeatingMqttBridge): Router {
  const router = Router();

  // Összes ismert eszköz állapota
  router.get('/', (_req, res) => {
    res.json({
      mqttConnected: bridge.isConnected(),
      devices: bridge.getAllStates(),
    });
  });

  // GET /api/heating/:id - utolsó ESP állapot lekérdezése
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

  // POST /api/heating/:id/command - teljes CMD JSON validálása és továbbítása MQTT-re
  router.post('/:id/command', async (req, res) => {
    const { id } = req.params;
    const validation = validateHeatingCommand(req.body);

    if (!validation.ok) {
      res.status(400).json({ error: validation.error });
      return;
    }

    try {
      await bridge.publishCommand(id, validation.value);
      res.json({
        ok: true,
        topic: `futes/${id}/cmd`,
        command: validation.value,
      });
    } catch (err) {
      const message = err instanceof Error ? err.message : 'MQTT küldési hiba.';
      res.status(503).json({ error: message });
    }
  });

  // POST /api/heating/:id/kimenet - egyetlen kimenet bit gyors kapcsolása az utolsó ESP állapot alapján
  router.post('/:id/kimenet', async (req, res) => {
    const { id } = req.params;
    const validation = validateSingleOutputBody(req.body);

    if (!validation.ok) {
      res.status(400).json({ error: validation.error });
      return;
    }

    const current = bridge.getDeviceState(id);
    if (!current) {
      res.status(409).json({
        error: `Nincs még utolsó ESP állapot az eszközhöz (${id}), így a targetTemp/mode felülírása nélkül nem módosítható egyetlen bit.`,
      });
      return;
    }

    const { nev, ertek } = validation.value;
    const latest = current.status;

    const fullCommand: HeatingCommand = {
      targetTemp: latest.targetTemp,
      mode: latest.mode,
      manualHeating: latest.mode === 'manual' ? latest.heatingOn : false,
      kimenetek: {
        ...latest.kimenetek,
        [nev]: ertek,
      },
    };

    try {
      await bridge.publishCommand(id, fullCommand);
      res.json({
        ok: true,
        topic: `futes/${id}/cmd`,
        changed: { nev, ertek },
        command: fullCommand,
      });
    } catch (err) {
      const message = err instanceof Error ? err.message : 'MQTT küldési hiba.';
      res.status(503).json({ error: message });
    }
  });

  return router;
}
