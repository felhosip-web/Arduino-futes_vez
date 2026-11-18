import { useEffect, useState } from 'react';
import {
  AlertTriangle,
  Bell,
  BellOff,
  BellRing,
  CheckCircle2,
  EyeOff,
  Flame,
  Radio,
  Send,
  Thermometer,
  WifiOff,
  Wrench,
} from 'lucide-react';
import {
  CRITICAL_ALERT_ITEMS,
  type CriticalAlertKey,
  type NotificationPrefs,
} from '../types/notifications';

type BrowserPermissionState = NotificationPermission | 'unsupported';

interface PushNotificationsProps {
  prefs: NotificationPrefs;
  onChangePrefs: (next: NotificationPrefs) => void;
  activeAlerts?: Partial<Record<CriticalAlertKey, boolean>>;
}

function readPermissionState(): BrowserPermissionState {
  if (typeof window === 'undefined' || !('Notification' in window)) {
    return 'unsupported';
  }
  return window.Notification.permission;
}

function getAlertIcon(key: CriticalAlertKey) {
  switch (key) {
    case 'overheat':
      return Flame;
    case 'pumpFault':
      return AlertTriangle;
    case 'sensorFail':
      return Thermometer;
    case 'nrfFail':
      return Radio;
    case 'serviceMode':
      return Wrench;
    case 'espOffline':
      return WifiOff;
  }
}

export function PushNotifications({
  prefs,
  onChangePrefs,
  activeAlerts,
}: PushNotificationsProps) {
  const [permission, setPermission] = useState<BrowserPermissionState>(() =>
    readPermissionState()
  );
  const [requesting, setRequesting] = useState<boolean>(false);
  const [feedback, setFeedback] = useState<{
    type: 'success' | 'error';
    text: string;
  } | null>(null);

  // Böngésző engedély állapot szinkronizálása (pl. ha a felhasználó a címsorban módosítja)
  useEffect(() => {
    setPermission(readPermissionState());

    if (typeof navigator === 'undefined' || !('permissions' in navigator)) {
      return;
    }

    let statusObj: PermissionStatus | null = null;
    let active = true;
    const handleChange = () => {
      if (active) {
        setPermission(readPermissionState());
      }
    };

    navigator.permissions
      .query({ name: 'notifications' as PermissionName })
      .then((res) => {
        if (!active) return;
        statusObj = res;
        res.addEventListener('change', handleChange);
      })
      .catch(() => {
        // Nem minden böngésző támogatja a permissions.query('notifications') hívást
      });

    return () => {
      active = false;
      statusObj?.removeEventListener('change', handleChange);
    };
  }, []);

  const handleRequestPermission = async () => {
    if (permission === 'unsupported') {
      setFeedback({
        type: 'error',
        text: 'Ez a böngésző vagy beágyazott nézet nem támogatja a Notification API-t.',
      });
      return;
    }

    setRequesting(true);
    setFeedback(null);
    try {
      const result = await window.Notification.requestPermission();
      setPermission(result);
      if (result === 'granted') {
        onChangePrefs({
          ...prefs,
          enabled: true,
        });
        setFeedback({
          type: 'success',
          text: 'Böngésző értesítések engedélyezve.',
        });
      } else if (result === 'denied') {
        onChangePrefs({
          ...prefs,
          enabled: false,
        });
        setFeedback({
          type: 'error',
          text: 'A böngésző letiltotta az értesítéseket. Engedélyezd a webhely beállításaiban.',
        });
      } else {
        setFeedback({
          type: 'error',
          text: 'Az értesítési engedély kérése el lett halasztva.',
        });
      }
    } catch {
      setFeedback({
        type: 'error',
        text: 'Nem sikerült lekérni az értesítési engedélyt ebben a környezetben.',
      });
    } finally {
      setRequesting(false);
    }
  };

  const handleToggleMaster = async () => {
    setFeedback(null);
    if (!prefs.enabled) {
      if (permission === 'default') {
        await handleRequestPermission();
        return;
      }
      if (permission === 'denied' || permission === 'unsupported') {
        setFeedback({
          type: 'error',
          text:
            permission === 'denied'
              ? 'Előbb engedélyezd az értesítéseket a böngésző webhely-beállításaiban.'
              : 'A böngésző nem támogatja az asztali értesítéseket.',
        });
        return;
      }
    }

    onChangePrefs({
      ...prefs,
      enabled: !prefs.enabled,
    });
  };

  const handleToggleAlert = (key: CriticalAlertKey) => {
    setFeedback(null);
    onChangePrefs({
      ...prefs,
      alerts: {
        ...prefs.alerts,
        [key]: !prefs.alerts[key],
      },
    });
  };

  const handleSetAllAlerts = (value: boolean) => {
    setFeedback(null);
    onChangePrefs({
      ...prefs,
      alerts: {
        overheat: value,
        pumpFault: value,
        sensorFail: value,
        nrfFail: value,
        serviceMode: value,
        espOffline: value,
      },
    });
  };

  const handleSendTest = () => {
    setFeedback(null);
    if (permission !== 'granted') {
      setFeedback({
        type: 'error',
        text: 'Teszt értesítéshez előbb engedélyezd a böngésző értesítéseket.',
      });
      return;
    }

    try {
      const notification = new window.Notification('Fűtésvezérlés — Teszt értesítés', {
        body: 'A kritikus kazánriasztások böngésző szintű jelzése megfelelően működik.',
        tag: 'futes-test-notification',
      });
      notification.onclick = () => {
        window.focus();
        notification.close();
      };
      setFeedback({
        type: 'success',
        text: 'Teszt értesítés elküldve a böngészőnek.',
      });
    } catch {
      setFeedback({
        type: 'error',
        text: 'A böngésző megakadályozta az értesítés megjelenítését.',
      });
    }
  };

  const enabledCount = CRITICAL_ALERT_ITEMS.filter((item) => prefs.alerts[item.key]).length;
  const masterOn = prefs.enabled && permission === 'granted';

  return (
    <section
      aria-labelledby="push-notifications-title"
      className="rounded-xl border border-slate-800 bg-slate-900 p-5 sm:p-6"
    >
      {/* Fejléc: cím + jogosultság állapot + főkapcsoló */}
      <div className="flex flex-wrap items-center justify-between gap-4 border-b border-slate-800/80 pb-4">
        <div className="flex items-start gap-3">
          <div
            className={`mt-0.5 flex h-9 w-9 shrink-0 items-center justify-center rounded-lg border ${
              masterOn
                ? 'border-orange-500/40 bg-orange-500/15 text-orange-400'
                : 'border-slate-700 bg-slate-800 text-slate-400'
            }`}
          >
            {masterOn ? (
              <BellRing className="h-4.5 w-4.5" aria-hidden="true" />
            ) : (
              <BellOff className="h-4.5 w-4.5" aria-hidden="true" />
            )}
          </div>
          <div>
            <h2 id="push-notifications-title" className="text-lg font-semibold text-slate-100">
              Böngésző értesítések (Push)
            </h2>
            <p className="mt-0.5 text-xs text-slate-400">
              Azonnali rendszerértesítések kritikus kazánriasztásoknál · {enabledCount}/
              {CRITICAL_ALERT_ITEMS.length} esemény aktív
            </p>
          </div>
        </div>

        <div className="flex flex-wrap items-center gap-3">
          {/* Engedély állapotjelző */}
          <div className="inline-flex items-center gap-2 text-xs font-medium">
            <span
              className={`inline-block h-2.5 w-2.5 rounded-full ${
                permission === 'granted'
                  ? 'bg-emerald-500'
                  : permission === 'denied' || permission === 'unsupported'
                    ? 'bg-red-500'
                    : 'bg-amber-400'
              }`}
              aria-hidden="true"
            />
            <span
              className={
                permission === 'granted'
                  ? 'text-emerald-300'
                  : permission === 'denied' || permission === 'unsupported'
                    ? 'text-red-300'
                    : 'text-amber-300'
              }
            >
              {permission === 'granted'
                ? 'Engedélyezve'
                : permission === 'denied'
                  ? 'Tiltva a böngészőben'
                  : permission === 'unsupported'
                    ? 'Nem támogatott'
                    : 'Engedély szükséges'}
            </span>
          </div>

          {/* Főkapcsoló */}
          <button
            type="button"
            role="switch"
            aria-checked={masterOn}
            aria-label="Böngésző értesítések főkapcsoló"
            disabled={requesting || permission === 'unsupported'}
            onClick={() => void handleToggleMaster()}
            className={`inline-flex min-h-[44px] items-center gap-2.5 rounded-lg border px-3.5 py-2 text-xs font-semibold whitespace-nowrap shrink-0 transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-orange-500 disabled:cursor-not-allowed disabled:opacity-50 ${
              masterOn
                ? 'border-orange-500/50 bg-orange-500/15 text-orange-200 hover:bg-orange-500/25'
                : 'border-slate-700 bg-slate-800 text-slate-300 hover:bg-slate-700'
            }`}
          >
            <span
              aria-hidden="true"
              className={`relative inline-flex h-5 w-9 shrink-0 items-center rounded-full transition-colors ${
                masterOn ? 'bg-orange-500' : 'bg-slate-600'
              }`}
            >
              <span
                className={`inline-block h-3.5 w-3.5 transform rounded-full bg-white transition-transform ${
                  masterOn ? 'translate-x-4.5' : 'translate-x-1'
                }`}
              />
            </span>
            <span>{masterOn ? 'BE' : 'KI'}</span>
          </button>
        </div>
      </div>

      {/* Jogosultságkérő vagy tiltás tájékoztató sáv */}
      {permission === 'default' && (
        <div className="mt-4 flex flex-wrap items-center justify-between gap-3 rounded-lg border border-amber-500/40 bg-amber-950/35 px-4 py-3 text-sm text-amber-100">
          <div className="flex items-center gap-2.5">
            <Bell className="h-4 w-4 shrink-0 text-amber-400" aria-hidden="true" />
            <span>
              A böngésző még nem kapott engedélyt az értesítések megjelenítésére.
            </span>
          </div>
          <button
            type="button"
            disabled={requesting}
            onClick={() => void handleRequestPermission()}
            className="inline-flex min-h-[40px] items-center gap-2 rounded-lg bg-orange-500 px-3.5 py-2 text-xs font-semibold text-slate-950 whitespace-nowrap shrink-0 transition-colors hover:bg-orange-400 disabled:opacity-60 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-orange-500"
          >
            <BellRing className="h-3.5 w-3.5" aria-hidden="true" />
            <span>{requesting ? 'Engedély kérése...' : 'Értesítések engedélyezése'}</span>
          </button>
        </div>
      )}

      {permission === 'denied' && (
        <div
          role="alert"
          className="mt-4 flex items-start gap-2.5 rounded-lg border border-red-500/50 bg-red-950/50 px-4 py-3 text-xs leading-relaxed text-red-200 sm:text-sm"
        >
          <AlertTriangle className="mt-0.5 h-4 w-4 shrink-0 text-red-400" aria-hidden="true" />
          <span>
            Az értesítések le vannak tiltva ehhez az oldalhoz. Kattints a böngésző címsora
            melletti lakat/információ ikonra, állítsd az <strong>Értesítések</strong> jogot{' '}
            <strong>Engedélyezés</strong>-re, majd töltsd újra az oldalt.
          </span>
        </div>
      )}

      {permission === 'unsupported' && (
        <div
          role="alert"
          className="mt-4 flex items-start gap-2.5 rounded-lg border border-slate-700 bg-slate-950/70 px-4 py-3 text-xs text-slate-300 sm:text-sm"
        >
          <AlertTriangle className="mt-0.5 h-4 w-4 shrink-0 text-amber-400" aria-hidden="true" />
          <span>
            Ez a böngésző vagy beágyazott nézet nem támogatja a böngésző szintű Notification
            API-t.
          </span>
        </div>
      )}

      {/* Visszajelző üzenet */}
      {feedback && (
        <div
          role="status"
          className={`mt-4 flex items-center gap-2.5 rounded-lg border px-4 py-2.5 text-xs font-medium sm:text-sm ${
            feedback.type === 'success'
              ? 'border-emerald-500/40 bg-emerald-950/40 text-emerald-200'
              : 'border-red-500/40 bg-red-950/40 text-red-200'
          }`}
        >
          {feedback.type === 'success' ? (
            <CheckCircle2 className="h-4 w-4 shrink-0 text-emerald-400" aria-hidden="true" />
          ) : (
            <AlertTriangle className="h-4 w-4 shrink-0 text-red-400" aria-hidden="true" />
          )}
          <span>{feedback.text}</span>
        </div>
      )}

      {/* Viselkedési opció + gyors gombok */}
      <div className="mt-5 flex flex-wrap items-center justify-between gap-3 border-t border-slate-800/80 pt-4">
        <button
          type="button"
          role="switch"
          aria-checked={prefs.onlyWhenHidden}
          disabled={!masterOn}
          onClick={() =>
            onChangePrefs({
              ...prefs,
              onlyWhenHidden: !prefs.onlyWhenHidden,
            })
          }
          className="inline-flex min-h-[40px] items-center gap-2.5 rounded-lg border border-slate-800 bg-slate-950/70 px-3 py-2 text-xs font-medium text-slate-300 transition-colors hover:border-slate-700 hover:text-slate-100 disabled:cursor-not-allowed disabled:opacity-50 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-orange-500"
        >
          <EyeOff className="h-3.5 w-3.5 text-slate-400" aria-hidden="true" />
          <span>Csak háttérben lévő fül esetén</span>
          <span
            className={`ml-1 rounded px-1.5 py-0.5 text-[11px] font-semibold ${
              prefs.onlyWhenHidden
                ? 'bg-orange-500/20 text-orange-300'
                : 'bg-slate-800 text-slate-400'
            }`}
          >
            {prefs.onlyWhenHidden ? 'BE' : 'KI'}
          </span>
        </button>

        <div className="flex flex-wrap items-center gap-2">
          <button
            type="button"
            disabled={!masterOn}
            onClick={() => handleSetAllAlerts(true)}
            className="min-h-[40px] rounded-lg border border-slate-700 bg-slate-800 px-3 py-2 text-xs font-medium text-slate-200 whitespace-nowrap shrink-0 transition-colors hover:bg-slate-700 disabled:cursor-not-allowed disabled:opacity-50 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-orange-500"
          >
            Mind BE
          </button>
          <button
            type="button"
            disabled={!masterOn}
            onClick={() => handleSetAllAlerts(false)}
            className="min-h-[40px] rounded-lg border border-slate-700 bg-slate-800 px-3 py-2 text-xs font-medium text-slate-300 whitespace-nowrap shrink-0 transition-colors hover:bg-slate-700 disabled:cursor-not-allowed disabled:opacity-50 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-orange-500"
          >
            Mind KI
          </button>
          <button
            type="button"
            disabled={permission !== 'granted'}
            onClick={handleSendTest}
            className="inline-flex min-h-[40px] items-center gap-1.5 rounded-lg border border-orange-500/40 bg-orange-500/15 px-3.5 py-2 text-xs font-semibold text-orange-200 whitespace-nowrap shrink-0 transition-colors hover:bg-orange-500/25 disabled:cursor-not-allowed disabled:opacity-50 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-orange-500"
          >
            <Send className="h-3.5 w-3.5" aria-hidden="true" />
            <span>Teszt értesítés</span>
          </button>
        </div>
      </div>

      {/* Esemény-specifikus kapcsolók */}
      <div className="mt-4 grid grid-cols-1 gap-3 sm:grid-cols-2">
        {CRITICAL_ALERT_ITEMS.map((item) => {
          const Icon = getAlertIcon(item.key);
          const isChecked = prefs.alerts[item.key];
          const isCurrentlyTriggered = Boolean(activeAlerts?.[item.key]);
          const isInteractive = masterOn;

          return (
            <button
              key={item.key}
              type="button"
              role="switch"
              aria-checked={isChecked}
              disabled={!isInteractive}
              onClick={() => handleToggleAlert(item.key)}
              className={`flex min-h-[76px] w-full items-start justify-between gap-3 rounded-lg border px-4 py-3 text-left transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-orange-500 ${
                !isInteractive
                  ? 'cursor-not-allowed border-slate-800 bg-slate-950/50 opacity-60'
                  : isChecked
                    ? item.severity === 'critical'
                      ? 'border-red-500/40 bg-red-950/20 hover:bg-red-950/35'
                      : 'border-amber-500/40 bg-amber-950/20 hover:bg-amber-950/35'
                    : 'border-slate-800 bg-slate-950/60 hover:border-slate-700 hover:bg-slate-800/60'
              }`}
            >
              <div className="min-w-0 flex-1">
                <div className="flex flex-wrap items-center gap-2">
                  <Icon
                    className={`h-4 w-4 shrink-0 ${
                      isChecked
                        ? item.severity === 'critical'
                          ? 'text-red-400'
                          : 'text-amber-400'
                        : 'text-slate-500'
                    }`}
                    aria-hidden="true"
                  />
                  <span className="text-sm font-medium text-slate-100">{item.label}</span>
                  {isCurrentlyTriggered && (
                    <span className="rounded border border-red-500/50 bg-red-950/80 px-1.5 py-0.5 text-[10px] font-bold uppercase tracking-wider text-red-200">
                      AKTÍV
                    </span>
                  )}
                </div>
                <p className="mt-1 text-xs leading-relaxed text-slate-400">{item.description}</p>
              </div>

              <span
                aria-hidden="true"
                className={`mt-0.5 relative inline-flex h-5 w-9 shrink-0 items-center rounded-full transition-colors ${
                  isChecked
                    ? item.severity === 'critical'
                      ? 'bg-red-500'
                      : 'bg-amber-500'
                    : 'bg-slate-700'
                }`}
              >
                <span
                  className={`inline-block h-3.5 w-3.5 transform rounded-full bg-white transition-transform ${
                    isChecked ? 'translate-x-4.5' : 'translate-x-1'
                  }`}
                />
              </span>
            </button>
          );
        })}
      </div>
    </section>
  );
}
