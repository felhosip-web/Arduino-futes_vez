import 'dotenv/config';
import http from 'node:http';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import express from 'express';
import cors from 'cors';
import { Server as SocketIOServer } from 'socket.io';
import { HeatingMqttBridge } from './server/mqttBridge.ts';
import { createHeatingRouter } from './server/heatingRoutes.ts';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

const PORT = Number(process.env.PORT || 3000);
const CORS_ORIGINS = (process.env.CORS_ORIGIN || 'http://localhost:5173')
  .split(',')
  .map((o) => o.trim())
  .filter(Boolean);

if (process.env.APP_URL && !CORS_ORIGINS.includes(process.env.APP_URL)) {
  CORS_ORIGINS.push(process.env.APP_URL);
}

async function startServer(): Promise<void> {
  const app = express();
  const httpServer = http.createServer(app);

  app.use(
    cors({
      origin: CORS_ORIGINS,
      methods: ['GET', 'POST', 'OPTIONS'],
      credentials: true,
    })
  );
  app.use(express.json());

  const io = new SocketIOServer(httpServer, {
    cors: {
      origin: CORS_ORIGINS,
      methods: ['GET', 'POST'],
      credentials: true,
    },
  });

  const mqttBridge = new HeatingMqttBridge(io);
  mqttBridge.connect();

  io.on('connection', (socket) => {
    // Új kliens csatlakozásakor átküldjük az ismert eszközök aktuális ESP státuszát
    for (const state of mqttBridge.getAllStates()) {
      const statusData = state.lastStatus ?? state.status;
      if (!statusData) continue;
      socket.emit('heating-update', {
        id: state.id,
        type: 'status',
        topic: `futes/${state.id}/status`,
        receivedAt: new Date(state.lastUpdatedAt).toISOString(),
        data: statusData,
        state,
      });
    }
  });

  app.get('/api/health', (_req, res) => {
    res.json({
      ok: true,
      mqttConnected: mqttBridge.isConnected(),
    });
  });

  app.use('/api/heating', createHeatingRouter(mqttBridge));

  // Fejlesztői környezetben Vite middleware, élesben statikus dist kiszolgálás
  if (process.env.NODE_ENV !== 'production') {
    const { createServer: createViteServer } = await import('vite');
    const vite = await createViteServer({
      server: { middlewareMode: true },
      appType: 'spa',
    });
    app.use(vite.middlewares);
  } else {
    const distPath = path.join(__dirname, 'dist');
    app.use(express.static(distPath));
    app.get('*', (_req, res) => {
      res.sendFile(path.join(distPath, 'index.html'));
    });
  }

  httpServer.listen(PORT, '0.0.0.0', () => {
    console.log(`[SERVER] Express + Socket.IO fut a http://0.0.0.0:${PORT} címen`);
  });

  const shutdown = () => {
    mqttBridge.disconnect();
    httpServer.close(() => process.exit(0));
  };
  process.on('SIGINT', shutdown);
  process.on('SIGTERM', shutdown);
}

startServer().catch((err) => {
  console.error('[SERVER] Indítási hiba:', err);
  process.exit(1);
});
