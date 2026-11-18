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

const NGINX_PORT = process.env.NGINX_PORT?.trim();
const DEFAULT_PORT = Number(process.env.DEFAULT_APP_PORT || 3000);
const RAW_PORT = process.env.PORT?.trim();
const PARSED_PORT = RAW_PORT ? Number(RAW_PORT) : NaN;
const PORT =
  RAW_PORT && RAW_PORT !== NGINX_PORT && Number.isFinite(PARSED_PORT) && PARSED_PORT > 0
    ? PARSED_PORT
    : DEFAULT_PORT;
const RAW_HOST = process.env.HOST?.trim() || '127.0.0.1';
const HEATING_API_KEY = process.env.HEATING_API_KEY?.trim() || '';
const IS_SANDBOX_DEV_PREVIEW =
  process.env.NODE_ENV !== 'production' && Boolean(process.env.CONTROL_PLANE_PORT);

// AI Studio dev sandbox injects HOST=0.0.0.0 without .env; fall back to 127.0.0.1 there,
// while strictly requiring HEATING_API_KEY for non-localhost bind everywhere else.
const HOST =
  RAW_HOST === '0.0.0.0' && !HEATING_API_KEY && IS_SANDBOX_DEV_PREVIEW ? '127.0.0.1' : RAW_HOST;
const IS_LOCALHOST = HOST === '127.0.0.1' || HOST === 'localhost' || HOST === '::1';

if (HOST === '0.0.0.0' && !HEATING_API_KEY) {
  console.error('HEATING_API_KEY kötelező, ha HOST=0.0.0.0');
  process.exit(1);
}

if (!IS_LOCALHOST && !HEATING_API_KEY) {
  console.error('HEATING_API_KEY kötelező, ha HOST=0.0.0.0');
  process.exit(1);
}

if (!HEATING_API_KEY && IS_LOCALHOST) {
  console.warn('FIGYELEM: HEATING_API_KEY nincs beállítva, csak localhost');
}

const rawFrontendOrigins = process.env.FRONTEND_ORIGIN || 'http://localhost:3000';
const FRONTEND_ORIGINS = rawFrontendOrigins
  .split(',')
  .map((o) => o.trim())
  .filter((o) => Boolean(o) && o !== '*');

if (FRONTEND_ORIGINS.length === 0) {
  FRONTEND_ORIGINS.push('http://localhost:3000');
}

async function startServer(): Promise<void> {
  const app = express();
  const httpServer = http.createServer(app);

  app.use(
    cors({
      origin: FRONTEND_ORIGINS,
      methods: ['GET', 'POST', 'DELETE', 'OPTIONS'],
      allowedHeaders: ['Content-Type', 'Authorization'],
      credentials: true,
    })
  );
  app.use(express.json());

  const io = new SocketIOServer(httpServer, {
    cors: {
      origin: FRONTEND_ORIGINS,
      methods: ['GET', 'POST'],
      allowedHeaders: ['Content-Type', 'Authorization'],
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

  app.use(
    '/api/heating',
    createHeatingRouter(mqttBridge, {
      host: HOST,
      apiKey: HEATING_API_KEY,
    })
  );

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

  httpServer.listen(PORT, HOST, () => {
    mqttBridge.logBackendEvent('INFO', 'SERVER_START', `Listening on ${HOST}:${PORT}`);
    if (HOST === '0.0.0.0') {
      console.warn(
        `FIGYELEM: Listening on 0.0.0.0:${PORT} - külső hálózatról is elérhető, auth kötelező!`
      );
    } else {
      console.log(`Listening on ${HOST}:${PORT} (localhost only)`);
    }
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
