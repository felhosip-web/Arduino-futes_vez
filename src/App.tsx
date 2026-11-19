import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { io } from 'socket.io-client';
import {
  AlertTriangle,
  LayoutDashboard,
  LineChart,
  RefreshCw,
  ScrollText,
  Wifi,
  WifiOff,
} from 'lucide-react';
import { API_URL, DEVICE_ID } from './config';
import {
  DEFAULT_ALARM_CONFIG,
  DEFAULT_ALARM_STATE,
  DEFAULT_HEARTBEAT_CONFIG,
  DEFAULT_OUTPUTS,
  DEFAULT_PUMP_CONFIG,
  DEFAULT_SAFETY_CONFIG,
  type AlarmConfig,
  type CommandAckState,
  type CommandApiResponse,
  type HeartbeatConfig,
  type HeatingCommandPayload,
  type HeatingMode,
  type HeatingStatusPayload,
  type HeatingUpdateEvent,
  type OutputKey,
  type PumpConfig,
  type StoredDeviceState,
} from './types/heating';
import { HeatingControl } from './components/HeatingControl';
import { Temperatures } from './components/Temperatures';
import { EloremenoStats } from './components/EloremenoStats';
import { PumpControl } from './components/PumpControl';
import { AlarmControl } from './components/AlarmControl';
import { HeartbeatControl } from './components/HeartbeatControl';
import { BoilerControl } from './components/BoilerControl';
import { BoilerStatus } from './components/BoilerStatus';
import { SystemCheck } from './components/SystemCheck';
import { LogsView } from './components/LogsView';
import { ChartsView } from './components/ChartsView';
import { ServiceModeModal } from './components/ServiceModeModal';
import { PushNotifications } from './components/PushNotifications';
import { SensorsPanel } from './components/SensorsPanel';
import {
  CRITICAL_ALERT_ITEMS,
  evaluateCriticalAlerts,
  loadNotificationPrefs,
  saveNotificationPrefs,
  type CriticalAlertKey,
  type NotificationPrefs,
} from './types/notifications';

const ALERT_NOTIFICATION_COOLDOWN_MS = 60_000;
const ACK_BANNER_DURATION_MS = 2000;

type ActiveView = 'dashboard' | 'logs' | 'charts';
type AckSectionKey = 'room' | 'pump' | 'output' | 'alarm' | 'heartbeat';

const IDLE_ACK_STATE: CommandAckState = { status: 'idle' };

const INITIAL_ACK_STATES: Record<AckSectionKey, CommandAckState> = {
  room: IDLE_ACK_STATE,
  pump: IDLE_ACK_STATE,
  output: IDLE_ACK_STATE,
  alarm: IDLE_ACK_STATE,
  heartbeat: IDLE_ACK_STATE,
};

function isUsableStatus(
  candidate: Partial<HeatingStatusPayload> | null | undefined
): candidate is HeatingStatusPayload {
  return Boolean(
    candidate &&
      typeof candidate.targetTemp === 'number' &&
      Number.isFinite(candidate.targetTemp) &&
      typeof candidate.mode === 'string' &&
      candidate.kimenetek &&
      typeof candidate.kimenetek === 'object'
  );
}

function formatTimestamp(ms: number | null): string {
  if (!ms) return 'Nincs adat';
  return new Date(ms).toLocaleTimeString('hu-HU', {
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
  });
}

export default function App() {
  const [activeView, setActiveView] = useState<ActiveView>('dashboard');
  const [logsRefreshSignal, setLogsRefreshSignal] = useState<number>(0);
  const [chartsRefreshSignal, setChartsRefreshSignal] = useState<number>(0);
  const [status, setStatus] = useState<HeatingStatusPayload | null>(null);
  const [lastUpdatedAt, setLastUpdatedAt] = useState<number | null>(null);
  const [socketConnected, setSocketConnected] = useState<boolean>(false);
  const [loadingInitial, setLoadingInitial] = useState<boolean>(true);
  const [isRefreshing, setIsRefreshing] = useState<boolean>(false);
  const [sendingCommand, setSendingCommand] = useState<boolean>(false);
  const [sendingPump, setSendingPump] = useState<boolean>(false);
  const [sendingAlarm, setSendingAlarm] = useState<boolean>(false);
  const [sendingHeartbeat, setSendingHeartbeat] = useState<boolean>(false);
  const [pendingOutputKey, setPendingOutputKey] = useState<OutputKey | null>(null);
  const [waitingForEsp, setWaitingForEsp] = useState<boolean>(false);
  const [errorMsg, setErrorMsg] = useState<string | null>(null);
  const [serviceStartedAtMs, setServiceStartedAtMs] = useState<number | null>(null);
  const [notificationPrefs, setNotificationPrefs] = useState<NotificationPrefs>(() =>
    loadNotificationPrefs()
  );
  const [ackStates, setAckStates] = useState<Record<AckSectionKey, CommandAckState>>(
    INITIAL_ACK_STATES
  );

  const hasAlertBaselineRef = useRef<boolean>(false);
  const prevAlertActiveRef = useRef<Partial<Record<CriticalAlertKey, boolean>>>({});
  const lastNotifiedAtRef = useRef<Partial<Record<CriticalAlertKey, number>>>({});
  const ackTimersRef = useRef<Partial<Record<AckSectionKey, number>>>({});

  const setSectionAck = useCallback(
    (section: AckSectionKey, nextState: CommandAckState, autoClearMs?: number) => {
      const prevTimer = ackTimersRef.current[section];
      if (prevTimer !== undefined) {
        window.clearTimeout(prevTimer);
        delete ackTimersRef.current[section];
      }
      setAckStates((prev) => ({ ...prev, [section]: nextState }));
      if (autoClearMs && autoClearMs > 0) {
        ackTimersRef.current[section] = window.setTimeout(() => {
          delete ackTimersRef.current[section];
          setAckStates((prev) => ({ ...prev, [section]: IDLE_ACK_STATE }));
        }, autoClearMs);
      }
    },
    []
  );

  useEffect(() => {
    const timers = ackTimersRef.current;
    return () => {
      for (const timerId of Object.values(timers)) {
        if (timerId !== undefined) {
          window.clearTimeout(timerId);
        }
      }
    };
  }, []);

  const handleChangeNotificationPrefs = useCallback((next: NotificationPrefs) => {
    setNotificationPrefs(next);
    saveNotificationPrefs(next);
  }, []);

  const evaluatedAlerts = useMemo(
    () => (status ? evaluateCriticalAlerts(status) : null),
    [status]
  );

  const activeAlertsMap = useMemo(() => {
    if (!evaluatedAlerts) return undefined;
    return {
      overheat: evaluatedAlerts.overheat.active,
      pumpFault: evaluatedAlerts.pumpFault.active,
      sensorFail: evaluatedAlerts.sensorFail.active,
      nrfFail: evaluatedAlerts.nrfFail.active,
      loopFault: evaluatedAlerts.loopFault.active,
      serviceMode: evaluatedAlerts.serviceMode.active,
      espOffline: evaluatedAlerts.espOffline.active,
    };
  }, [evaluatedAlerts]);

  useEffect(() => {
    if (!evaluatedAlerts) return;

    if (!hasAlertBaselineRef.current) {
      for (const item of CRITICAL_ALERT_ITEMS) {
        prevAlertActiveRef.current[item.key] = evaluatedAlerts[item.key].active;
      }
      hasAlertBaselineRef.current = true;
      return;
    }

    const canDispatch =
      notificationPrefs.enabled &&
      typeof window !== 'undefined' &&
      'Notification' in window &&
      window.Notification.permission === 'granted' &&
      (!notificationPrefs.onlyWhenHidden || document.hidden);

    const now = Date.now();

    for (const item of CRITICAL_ALERT_ITEMS) {
      const key = item.key;
      const curr = evaluatedAlerts[key];
      const wasActive = Boolean(prevAlertActiveRef.current[key]);
      prevAlertActiveRef.current[key] = curr.active;

      const isRisingEdge = curr.active && !wasActive;
      if (!isRisingEdge || !canDispatch || !notificationPrefs.alerts[key]) {
        continue;
      }

      const lastSent = lastNotifiedAtRef.current[key] ?? 0;
      if (now - lastSent < ALERT_NOTIFICATION_COOLDOWN_MS) {
        continue;
      }

      try {
        const notification = new window.Notification(curr.title, {
          body: curr.body,
          tag: `futes-alert-${key}`,
          requireInteraction: curr.requireInteraction,
        });
        notification.onclick = () => {
          window.focus();
          notification.close();
        };
        lastNotifiedAtRef.current[key] = now;
      } catch {
        // Beágyazott környezetben a Notification példányosítása sikertelen lehet
      }
    }
  }, [evaluatedAlerts, notificationPrefs]);

  useEffect(() => {
    if (status?.serviceMode) {
      setServiceStartedAtMs((prev) => prev ?? Date.now());
    } else {
      setServiceStartedAtMs(null);
    }
  }, [status?.serviceMode]);

  const applyDeviceState = useCallback((deviceState: StoredDeviceState | null | undefined) => {
    const candidate = deviceState?.lastStatus ?? deviceState?.status ?? null;
    if (isUsableStatus(candidate)) {
      setStatus({
        ...candidate,
        pumpConfig: candidate.pumpConfig ?? DEFAULT_PUMP_CONFIG,
        pumpState: candidate.pumpState ?? (candidate.kimenetek.szivattyu ? 'futes' : 'varakozas'),
        alarmConfig: candidate.alarmConfig ?? DEFAULT_ALARM_CONFIG,
        safety: candidate.safety ?? DEFAULT_SAFETY_CONFIG,
        alarmState: candidate.alarmState ?? DEFAULT_ALARM_STATE,
        heartbeat: candidate.heartbeat ?? DEFAULT_HEARTBEAT_CONFIG,
      });
      setLastUpdatedAt(deviceState?.lastUpdatedAt ?? Date.now());
      setWaitingForEsp(false);
    } else {
      setWaitingForEsp(true);
    }
  }, []);

  const fetchInitialState = useCallback(async () => {
    setIsRefreshing(true);
    try {
      const res = await fetch(`${API_URL}/api/heating/${DEVICE_ID}`);
      if (res.status === 404 || res.status === 409) {
        setStatus(null);
        setWaitingForEsp(true);
        setErrorMsg(null);
        return;
      }
      if (!res.ok) {
        const body = (await res.json().catch(() => null)) as { error?: string } | null;
        throw new Error(body?.error || `HTTP ${res.status}`);
      }
      const data = (await res.json()) as StoredDeviceState;
      applyDeviceState(data);
      setErrorMsg(null);
    } catch (err) {
      const message =
        err instanceof Error ? err.message : 'Nem sikerült lekérdezni a backendet.';
      setErrorMsg(message);
    } finally {
      setLoadingInitial(false);
      setIsRefreshing(false);
    }
  }, [applyDeviceState]);

  useEffect(() => {
    void fetchInitialState();

    const socket = io(API_URL || undefined, {
      transports: ['websocket', 'polling'],
      reconnectionDelay: 2000,
    });

    socket.on('connect', () => {
      setSocketConnected(true);
    });

    socket.on('disconnect', () => {
      setSocketConnected(false);
    });

    // MINDIG a state.lastStatus-t használjuk (teljes, mély merge-elt állapot), SOHA a nyers data-t!
    socket.on('heating-update', (event: HeatingUpdateEvent) => {
      if (!event || event.id !== DEVICE_ID || !event.state) return;
      applyDeviceState(event.state);
      setLoadingInitial(false);
      setErrorMsg(null);
    });

    const pollTimer = window.setInterval(() => {
      void fetchInitialState();
    }, 30_000);

    return () => {
      window.clearInterval(pollTimer);
      socket.disconnect();
    };
  }, [applyDeviceState, fetchInitialState]);

  const processAckResponse = useCallback(
    async (
      section: AckSectionKey,
      res: Response,
      fallbackError: string
    ): Promise<boolean> => {
      if (res.status === 409) {
        setWaitingForEsp(true);
        setSectionAck(section, IDLE_ACK_STATE);
        return false;
      }

      const body = (await res.json().catch(() => null)) as CommandApiResponse | null;

      if (res.ok && body?.acked === true) {
        setSectionAck(
          section,
          { status: 'acked', cmdId: body.cmdId },
          ACK_BANNER_DURATION_MS
        );
        return true;
      }

      const reason = body?.reason;
      const errMsg =
        reason === 'SERVICE_MODE'
          ? 'Nincs visszaigazolás a Nano-tól - szerviz módban van'
          : reason === 'RELAY_MISMATCH'
            ? 'Relé hiba: a fizikai visszacsatolás (Nano D7) nem követte a parancsot'
            : reason === 'NRF_TIMEOUT' || reason === 'ACK_TIMEOUT'
              ? 'Nincs visszaigazolás a Nano-tól - ellenőrizd a rádiót'
              : body?.error || `${fallbackError} (HTTP ${res.status})`;

      setSectionAck(section, {
        status: 'error',
        cmdId: body?.cmdId,
        reason,
        message: errMsg,
      });
      setErrorMsg(errMsg);
      return false;
    },
    [setSectionAck]
  );

  const handleSendRoomCommand = useCallback(
    async (patch: { targetTemp: number; mode: HeatingMode; manualHeating?: boolean }) => {
      if (!status) return;
      setSendingCommand(true);
      setSectionAck('room', { status: 'pending' });
      setErrorMsg(null);

      const payload: HeatingCommandPayload = {
        targetTemp: patch.targetTemp,
        mode: patch.mode,
        manualHeating: patch.manualHeating ?? status.manualHeating ?? false,
        kimenetek: status.kimenetek ?? DEFAULT_OUTPUTS,
        pumpConfig: status.pumpConfig ?? DEFAULT_PUMP_CONFIG,
        alarmConfig: status.alarmConfig ?? DEFAULT_ALARM_CONFIG,
        heartbeat: status.heartbeat ?? DEFAULT_HEARTBEAT_CONFIG,
      };

      try {
        const res = await fetch(`${API_URL}/api/heating/${DEVICE_ID}/command`, {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify(payload),
        });
        await processAckResponse('room', res, 'Parancs küldési hiba');
      } catch (err) {
        const message =
          err instanceof Error ? err.message : 'Hiba a parancs küldése közben.';
        setSectionAck('room', { status: 'error', message });
        setErrorMsg(message);
      } finally {
        setSendingCommand(false);
      }
    },
    [processAckResponse, setSectionAck, status]
  );

  const handleSendPumpConfig = useCallback(
    async (pumpConfig: PumpConfig) => {
      setSendingPump(true);
      setSectionAck('pump', { status: 'pending' });
      setErrorMsg(null);
      try {
        const res = await fetch(`${API_URL}/api/heating/${DEVICE_ID}/pump`, {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ pumpConfig }),
        });
        await processAckResponse('pump', res, 'Szivattyú beállítási hiba');
      } catch (err) {
        const message =
          err instanceof Error ? err.message : 'Hiba a szivattyú beállításakor.';
        setSectionAck('pump', { status: 'error', message });
        setErrorMsg(message);
      } finally {
        setSendingPump(false);
      }
    },
    [processAckResponse, setSectionAck]
  );

  const handleSendAlarmConfig = useCallback(
    async (alarmConfig: AlarmConfig) => {
      setSendingAlarm(true);
      setSectionAck('alarm', { status: 'pending' });
      setErrorMsg(null);
      try {
        const res = await fetch(`${API_URL}/api/heating/${DEVICE_ID}/alarm`, {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ alarmConfig }),
        });
        await processAckResponse('alarm', res, 'Riasztás beállítási hiba');
      } catch (err) {
        const message =
          err instanceof Error ? err.message : 'Hiba a riasztás beállításakor.';
        setSectionAck('alarm', { status: 'error', message });
        setErrorMsg(message);
      } finally {
        setSendingAlarm(false);
      }
    },
    [processAckResponse, setSectionAck]
  );

  const handleSendHeartbeat = useCallback(
    async (heartbeat: HeartbeatConfig) => {
      setSendingHeartbeat(true);
      setSectionAck('heartbeat', { status: 'pending' });
      setErrorMsg(null);
      try {
        const res = await fetch(`${API_URL}/api/heating/${DEVICE_ID}/heartbeat`, {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ heartbeat }),
        });
        await processAckResponse('heartbeat', res, 'Életjel beállítási hiba');
      } catch (err) {
        const message =
          err instanceof Error ? err.message : 'Hiba az életjel intervallum beállításakor.';
        setSectionAck('heartbeat', { status: 'error', message });
        setErrorMsg(message);
      } finally {
        setSendingHeartbeat(false);
      }
    },
    [processAckResponse, setSectionAck]
  );

  const handleToggleOutput = useCallback(
    async (nev: OutputKey, ertek: boolean) => {
      setPendingOutputKey(nev);
      setSectionAck('output', { status: 'pending' });
      setErrorMsg(null);
      try {
        const res = await fetch(`${API_URL}/api/heating/${DEVICE_ID}/kimenet`, {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ nev, ertek }),
        });
        await processAckResponse('output', res, 'Kimenet kapcsolási hiba');
      } catch (err) {
        const message =
          err instanceof Error ? err.message : 'Hiba a kimenet kapcsolása közben.';
        setSectionAck('output', { status: 'error', message });
        setErrorMsg(message);
      } finally {
        setPendingOutputKey(null);
      }
    },
    [processAckResponse, setSectionAck]
  );

  const handleResetTrendStats = useCallback(async (): Promise<boolean> => {
    setErrorMsg(null);
    try {
      const res = await fetch(`${API_URL}/api/heating/${DEVICE_ID}/trend/reset`, {
        method: 'POST',
      });
      if (!res.ok) {
        const body = (await res.json().catch(() => null)) as { error?: string } | null;
        throw new Error(body?.error || `Statisztika nullázási hiba (HTTP ${res.status})`);
      }
      setLogsRefreshSignal((prev) => prev + 1);
      await fetchInitialState();
      return true;
    } catch (err) {
      const message =
        err instanceof Error ? err.message : 'Hiba a statisztika nullázása közben.';
      setErrorMsg(message);
      return false;
    }
  }, [fetchInitialState]);

  return (
    <div className="min-h-screen bg-slate-950 text-slate-100">
      <div className="mx-auto max-w-5xl px-4 py-6 sm:px-6 lg:px-8">
        {/* Fejléc */}
        <header className="flex flex-wrap items-center justify-between gap-4 border-b border-slate-800 pb-4">
          <div>
            <h1 className="text-xl font-bold tracking-tight text-slate-50 sm:text-2xl">
              Fűtésvezérlés
            </h1>
            <div className="mt-1 flex flex-wrap items-center gap-2 text-xs text-slate-400">
              <span className="inline-flex items-center gap-1.5">
                {socketConnected ? (
                  <Wifi className="h-3.5 w-3.5 text-emerald-400" aria-hidden="true" />
                ) : (
                  <WifiOff className="h-3.5 w-3.5 text-slate-500" aria-hidden="true" />
                )}
                <span>{socketConnected ? 'Élő kapcsolat' : 'Újracsatlakozás...'}</span>
              </span>
              <span aria-hidden="true">·</span>
              <span className="tabular-nums">Frissítve: {formatTimestamp(lastUpdatedAt)}</span>
              <span aria-hidden="true">·</span>
              <span className="tabular-nums">
                Ping: {status?.heartbeat?.intervalSec ?? DEFAULT_HEARTBEAT_CONFIG.intervalSec}s
              </span>
            </div>
          </div>

          <div className="flex items-center gap-3">
            {status?.alarmState?.active && (
              <div
                role="alert"
                className="inline-flex min-h-[44px] items-center gap-2 rounded-lg border border-red-500 bg-red-950/80 px-3.5 py-2 text-xs font-bold text-red-100 whitespace-nowrap shrink-0 animate-pulse"
              >
                <span className="inline-block h-2.5 w-2.5 rounded-full bg-red-500" aria-hidden="true" />
                <span>RIASZTÁS</span>
              </div>
            )}

            <button
              type="button"
              onClick={() => {
                void fetchInitialState();
                if (activeView === 'logs') {
                  setLogsRefreshSignal((prev) => prev + 1);
                } else if (activeView === 'charts') {
                  setChartsRefreshSignal((prev) => prev + 1);
                }
              }}
              disabled={isRefreshing}
              className="inline-flex min-h-[44px] items-center gap-2 rounded-lg border border-slate-700 bg-slate-900 px-4 py-2 text-sm font-medium text-slate-200 whitespace-nowrap shrink-0 transition-colors hover:bg-slate-800 disabled:opacity-60 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-orange-500"
            >
              <RefreshCw
                className={`h-4 w-4 ${isRefreshing ? 'animate-spin' : ''}`}
                aria-hidden="true"
              />
              <span>Frissítés</span>
            </button>
          </div>
        </header>

        {/* Nézet választó */}
        <nav aria-label="Nézetek" className="mt-4 flex items-center justify-between gap-4">
          <div
            role="tablist"
            aria-label="Fő nézetek"
            className="inline-flex items-center gap-1 rounded-lg border border-slate-800 bg-slate-900 p-1"
          >
            <button
              type="button"
              role="tab"
              aria-selected={activeView === 'dashboard'}
              onClick={() => setActiveView('dashboard')}
              className={`inline-flex min-h-[44px] items-center gap-2 rounded-md px-4 py-2 text-sm font-semibold whitespace-nowrap shrink-0 transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-orange-500 ${
                activeView === 'dashboard'
                  ? 'bg-orange-500 text-slate-950'
                  : 'text-slate-300 hover:bg-slate-800 hover:text-slate-100'
              }`}
            >
              <LayoutDashboard className="h-4 w-4" aria-hidden="true" />
              <span>Vezérlőpult</span>
            </button>

            <button
              type="button"
              role="tab"
              aria-selected={activeView === 'logs'}
              onClick={() => setActiveView('logs')}
              className={`inline-flex min-h-[44px] items-center gap-2 rounded-md px-4 py-2 text-sm font-semibold whitespace-nowrap shrink-0 transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-orange-500 ${
                activeView === 'logs'
                  ? 'bg-orange-500 text-slate-950'
                  : 'text-slate-300 hover:bg-slate-800 hover:text-slate-100'
              }`}
            >
              <ScrollText className="h-4 w-4" aria-hidden="true" />
              <span>Napló</span>
            </button>

            <button
              type="button"
              role="tab"
              aria-selected={activeView === 'charts'}
              onClick={() => setActiveView('charts')}
              className={`inline-flex min-h-[44px] items-center gap-2 rounded-md px-4 py-2 text-sm font-semibold whitespace-nowrap shrink-0 transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-orange-500 ${
                activeView === 'charts'
                  ? 'bg-orange-500 text-slate-950'
                  : 'text-slate-300 hover:bg-slate-800 hover:text-slate-100'
              }`}
            >
              <LineChart className="h-4 w-4" aria-hidden="true" />
              <span>Grafikonok</span>
            </button>
          </div>
        </nav>

        {/* Hibák és figyelmeztetések */}
        {errorMsg && (
          <div
            role="alert"
            className="mt-4 flex items-center gap-2.5 rounded-lg border border-red-500/50 bg-red-950/60 px-4 py-3 text-sm text-red-200"
          >
            <AlertTriangle className="h-4 w-4 shrink-0 text-red-400" aria-hidden="true" />
            <span>{errorMsg}</span>
          </div>
        )}

        {status && status.online === false && (
          <div
            role="status"
            className="mt-4 flex items-center gap-2.5 rounded-lg border border-amber-500/40 bg-amber-950/40 px-4 py-3 text-sm text-amber-200"
          >
            <AlertTriangle className="h-4 w-4 shrink-0 text-amber-400" aria-hidden="true" />
            <span>Az ESP32-C3 gateway jelenleg offline — az utolsó ismert állapot látható.</span>
          </div>
        )}

        {status?.error && (
          <div
            role="alert"
            className="mt-4 flex items-center gap-2.5 rounded-lg border border-red-500/50 bg-red-950/60 px-4 py-3 text-sm text-red-200"
          >
            <AlertTriangle className="h-4 w-4 shrink-0 text-red-400" aria-hidden="true" />
            <span>Rendszerhiba: {status.error}</span>
          </div>
        )}

        {/* Tartalom */}
        {activeView === 'logs' ? (
          <main className="mt-6 space-y-6">
            <SystemCheck
              status={status}
              lastUpdatedAt={lastUpdatedAt}
              onSyncTriggered={() => setLogsRefreshSignal((prev) => prev + 1)}
            />
            <LogsView refreshSignal={logsRefreshSignal} />
          </main>
        ) : activeView === 'charts' ? (
          <main className="mt-6 space-y-6">
            <ChartsView liveStatus={status} refreshSignal={chartsRefreshSignal} />
          </main>
        ) : loadingInitial ? (
          <div className="mt-6 rounded-xl border border-slate-800 bg-slate-900 p-8 text-center text-sm text-slate-400">
            Állapot betöltése...
          </div>
        ) : !status || waitingForEsp ? (
          <div className="mt-6 rounded-xl border border-slate-800 bg-slate-900 p-8 text-center">
            <h2 className="text-lg font-semibold text-slate-100">Várakozás az ESP-re...</h2>
            <p className="mt-2 text-sm text-slate-400">
              Még nem érkezett teljes állapotüzenet az ESP32-C3 gateway-től (
              <span className="font-mono text-slate-300">futes/{DEVICE_ID}/status</span>).
            </p>
            <button
              type="button"
              onClick={() => void fetchInitialState()}
              className="mt-5 inline-flex min-h-[44px] items-center gap-2 rounded-lg bg-orange-500 px-5 py-2.5 text-sm font-semibold text-slate-950 transition-colors hover:bg-orange-400 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-orange-500"
            >
              Újrapróbálás
            </button>
          </div>
        ) : (
          <main className="mt-6 space-y-6">
            <HeatingControl
              status={status}
              sending={sendingCommand}
              disabled={Boolean(status.serviceMode)}
              ackState={ackStates.room}
              onSendRoomCommand={handleSendRoomCommand}
            />

            <Temperatures temperatures={status.homersekletek} trend={status.trend} />

            <EloremenoStats trend={status.trend} onResetStats={handleResetTrendStats} />

            <PumpControl
              pumpConfig={status.pumpConfig ?? DEFAULT_PUMP_CONFIG}
              pumpState={status.pumpState ?? 'varakozas'}
              commandedPumpOn={status.pumpOn ?? Boolean(status.kimenetek?.szivattyu)}
              relayFeedbackOn={status.relayFeedbackOn}
              relayFault={status.relayFault}
              safety={status.safety ?? DEFAULT_SAFETY_CONFIG}
              kazanTemp={status.homersekletek?.kazan ?? null}
              sending={sendingPump}
              disabled={Boolean(status.serviceMode)}
              ackState={ackStates.pump}
              onSendPumpConfig={handleSendPumpConfig}
            />

            <AlarmControl
              alarmConfig={status.alarmConfig ?? DEFAULT_ALARM_CONFIG}
              alarmState={status.alarmState ?? DEFAULT_ALARM_STATE}
              safety={status.safety ?? DEFAULT_SAFETY_CONFIG}
              kazanTemp={status.homersekletek?.kazan ?? null}
              sending={sendingAlarm}
              ackState={ackStates.alarm}
              onSendAlarmConfig={handleSendAlarmConfig}
            />

            <PushNotifications
              prefs={notificationPrefs}
              onChangePrefs={handleChangeNotificationPrefs}
              activeAlerts={activeAlertsMap}
            />

            <HeartbeatControl
              heartbeat={status.heartbeat ?? DEFAULT_HEARTBEAT_CONFIG}
              nrf24Ok={Boolean(status.nrf24_ok)}
              lastUpdatedAt={lastUpdatedAt}
              counterValue={status.nanoCounter ?? status.seq ?? status.counter}
              sending={sendingHeartbeat}
              ackState={ackStates.heartbeat}
              onSendHeartbeat={handleSendHeartbeat}
            />

            <BoilerControl
              outputs={status.kimenetek ?? DEFAULT_OUTPUTS}
              pumpMode={status.pumpConfig?.pumpMode ?? 'auto'}
              pendingKey={pendingOutputKey}
              disabled={Boolean(status.serviceMode)}
              ackState={ackStates.output}
              onToggleOutput={handleToggleOutput}
            />

            <BoilerStatus
              kazanStatus={status.kazan_status ?? [0, 0, 0, 0, 0, 0, 0, 0]}
              nrf24Ok={Boolean(status.nrf24_ok)}
            />

            <SensorsPanel status={status} onSensorLearned={fetchInitialState} />
          </main>
        )}
      </div>

      <ServiceModeModal
        active={Boolean(status?.serviceMode)}
        startedAtMs={serviceStartedAtMs}
      />
    </div>
  );
}
