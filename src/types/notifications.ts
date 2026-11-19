import { DEFAULT_SAFETY_CONFIG, type HeatingStatusPayload } from './heating';

export type CriticalAlertKey =
  | 'overheat'
  | 'pumpFault'
  | 'sensorFail'
  | 'nrfFail'
  | 'loopFault'
  | 'serviceMode'
  | 'espOffline';

export interface NotificationPrefs {
  enabled: boolean;
  onlyWhenHidden: boolean;
  alerts: Record<CriticalAlertKey, boolean>;
}

export interface CriticalAlertItemMeta {
  key: CriticalAlertKey;
  label: string;
  description: string;
  severity: 'critical' | 'warning';
}

export interface EvaluatedCriticalAlert {
  active: boolean;
  title: string;
  body: string;
  requireInteraction: boolean;
}

export const NOTIFICATION_PREFS_STORAGE_KEY = 'heating_push_notification_prefs_v1';

export const DEFAULT_NOTIFICATION_PREFS: NotificationPrefs = {
  enabled: false,
  onlyWhenHidden: false,
  alerts: {
    overheat: true,
    pumpFault: true,
    sensorFail: true,
    nrfFail: true,
    loopFault: true,
    serviceMode: true,
    espOffline: true,
  },
};

export const CRITICAL_ALERT_ITEMS: ReadonlyArray<CriticalAlertItemMeta> = [
  {
    key: 'overheat',
    label: 'Túlmelegedés riasztás',
    description: 'Kazán elérte a riasztási küszöböt vagy a 95 °C-os biztonsági határt.',
    severity: 'critical',
  },
  {
    key: 'pumpFault',
    label: 'Szivattyúhiba',
    description: 'Fűtési igény vagy túlmelegedés közben a szivattyú relé nem kapcsolt BE.',
    severity: 'critical',
  },
  {
    key: 'sensorFail',
    label: 'Szenzorhiba (-127 °C)',
    description: 'Kazán, előremenő vagy visszatérő DS18B20 hőmérő olvasási hibája.',
    severity: 'critical',
  },
  {
    key: 'nrfFail',
    label: 'Rádió (NRF24) hiba',
    description: 'Megszakadt a Master–Slave kapcsolat (a kazán fail-safe módba lép).',
    severity: 'critical',
  },
  {
    key: 'loopFault',
    label: 'Loop ciklusidő hiba',
    description: 'Az ESP32-C3 vagy az Arduino Nano főciklusa tartósan lelassult.',
    severity: 'critical',
  },
  {
    key: 'serviceMode',
    label: 'Hardveres szerviz mód',
    description: 'A kazánnál lévő fizikai szerviz kapcsoló (Nano D6) BE lett kapcsolva.',
    severity: 'warning',
  },
  {
    key: 'espOffline',
    label: 'Gateway offline',
    description: 'Az ESP32-C3 WiFi/MQTT kapcsolata megszakadt a felhővel.',
    severity: 'warning',
  },
];

export function loadNotificationPrefs(): NotificationPrefs {
  if (typeof window === 'undefined') {
    return {
      ...DEFAULT_NOTIFICATION_PREFS,
      alerts: { ...DEFAULT_NOTIFICATION_PREFS.alerts },
    };
  }

  try {
    const raw = window.localStorage.getItem(NOTIFICATION_PREFS_STORAGE_KEY);
    if (!raw) {
      return {
        ...DEFAULT_NOTIFICATION_PREFS,
        alerts: { ...DEFAULT_NOTIFICATION_PREFS.alerts },
      };
    }

    const parsed = JSON.parse(raw) as Partial<NotificationPrefs> | null;
    if (!parsed || typeof parsed !== 'object') {
      return {
        ...DEFAULT_NOTIFICATION_PREFS,
        alerts: { ...DEFAULT_NOTIFICATION_PREFS.alerts },
      };
    }

    const rawAlerts = (parsed.alerts ?? {}) as Partial<Record<CriticalAlertKey, unknown>>;
    return {
      enabled: typeof parsed.enabled === 'boolean' ? parsed.enabled : DEFAULT_NOTIFICATION_PREFS.enabled,
      onlyWhenHidden:
        typeof parsed.onlyWhenHidden === 'boolean'
          ? parsed.onlyWhenHidden
          : DEFAULT_NOTIFICATION_PREFS.onlyWhenHidden,
      alerts: {
        overheat:
          typeof rawAlerts.overheat === 'boolean'
            ? rawAlerts.overheat
            : DEFAULT_NOTIFICATION_PREFS.alerts.overheat,
        pumpFault:
          typeof rawAlerts.pumpFault === 'boolean'
            ? rawAlerts.pumpFault
            : DEFAULT_NOTIFICATION_PREFS.alerts.pumpFault,
        sensorFail:
          typeof rawAlerts.sensorFail === 'boolean'
            ? rawAlerts.sensorFail
            : DEFAULT_NOTIFICATION_PREFS.alerts.sensorFail,
        nrfFail:
          typeof rawAlerts.nrfFail === 'boolean'
            ? rawAlerts.nrfFail
            : DEFAULT_NOTIFICATION_PREFS.alerts.nrfFail,
        loopFault:
          typeof rawAlerts.loopFault === 'boolean'
            ? rawAlerts.loopFault
            : DEFAULT_NOTIFICATION_PREFS.alerts.loopFault,
        serviceMode:
          typeof rawAlerts.serviceMode === 'boolean'
            ? rawAlerts.serviceMode
            : DEFAULT_NOTIFICATION_PREFS.alerts.serviceMode,
        espOffline:
          typeof rawAlerts.espOffline === 'boolean'
            ? rawAlerts.espOffline
            : DEFAULT_NOTIFICATION_PREFS.alerts.espOffline,
      },
    };
  } catch {
    return {
      ...DEFAULT_NOTIFICATION_PREFS,
      alerts: { ...DEFAULT_NOTIFICATION_PREFS.alerts },
    };
  }
}

export function saveNotificationPrefs(prefs: NotificationPrefs): void {
  if (typeof window === 'undefined') return;
  try {
    window.localStorage.setItem(NOTIFICATION_PREFS_STORAGE_KEY, JSON.stringify(prefs));
  } catch {
    // localStorage kvóta vagy privát mód hiba esetén csendben figyelmen kívül hagyjuk
  }
}

function isValidTemp(val: number | null | undefined): val is number {
  return typeof val === 'number' && Number.isFinite(val) && val > -120;
}

export function evaluateCriticalAlerts(
  status: HeatingStatusPayload
): Record<CriticalAlertKey, EvaluatedCriticalAlert> {
  const kazanTemp = status.homersekletek?.kazan ?? null;
  const eloreTemp = status.homersekletek?.eloremeno ?? null;
  const visszaTemp = status.homersekletek?.visszatero ?? null;
  const safetyLimit = status.safety?.safetyOverheatTemp ?? DEFAULT_SAFETY_CONFIG.safetyOverheatTemp;
  const alarmLimit = status.alarmConfig?.alarmTemp ?? 85.0;

  const safetyOverheat = isValidTemp(kazanTemp) && kazanTemp >= safetyLimit;
  const alarmOverheat =
    (status.alarmState?.active && status.alarmState.reason === 'overheat') ||
    (isValidTemp(kazanTemp) && Boolean(status.alarmConfig?.alarmEnabled) && kazanTemp >= alarmLimit);

  const overheatActive = safetyOverheat || alarmOverheat;
  const overheatBody = isValidTemp(kazanTemp)
    ? `Kazán hőmérséklet: ${kazanTemp.toFixed(1)} °C (${
        safetyOverheat ? `elérte a ${safetyLimit.toFixed(0)} °C biztonsági határt` : `riasztási határ: ${alarmLimit.toFixed(1)} °C`
      }).`
    : 'A kazán elérte a túlmelegedési riasztási határt.';

  const isManualOff = status.pumpConfig?.pumpMode === 'manual_off';
  const pumpRelayOn = Boolean(status.kimenetek?.szivattyu);
  const hasRelayFault = Boolean(status.relayFault);
  const commandedOn = status.pumpOn ?? pumpRelayOn;
  const feedbackOn = status.relayFeedbackOn;
  const pumpFaultActive =
    hasRelayFault ||
    (Boolean(status.nrf24_ok) &&
      !isManualOff &&
      !pumpRelayOn &&
      (status.pumpState === 'futes' ||
        overheatActive ||
        status.pumpConfig?.pumpMode === 'manual_on'));
  const pumpFaultBody = hasRelayFault
    ? `Relé visszacsatolási hiba (Nano D7): Kért ${commandedOn ? 'BE' : 'KI'}, de a fizikai relé ${
        feedbackOn ? 'BE' : 'KI'
      } állapotban van!`
    : 'A szivattyúnak működnie kellene (fűtési igény vagy túlmelegedés miatt), de a relé KI állapotban van!';

  const brokenSensors: string[] = [];
  if (status.nrf24_ok) {
    if (!isValidTemp(kazanTemp)) brokenSensors.push('Kazán');
    if (!isValidTemp(eloreTemp)) brokenSensors.push('Előremenő');
    if (!isValidTemp(visszaTemp)) brokenSensors.push('Visszatérő');
  }
  const sensorFailActive =
    (status.alarmState?.active && status.alarmState.reason === 'sensor_fail') ||
    brokenSensors.length > 0;
  const sensorFailBody =
    brokenSensors.length > 0
      ? `Szenzorhiba (-127 °C): ${brokenSensors.join(', ')} nem olvasható.`
      : 'Kazán oldali hőmérséklet-szenzor hiba (-127 °C).';

  const nrfFailActive =
    status.nrf24_ok === false ||
    (status.alarmState?.active && status.alarmState.reason === 'nrf_fail');

  const espLoopFault = Boolean(status.loopFault);
  const nanoLoopFault = Boolean(status.nanoLoopFault);
  const loopFaultActive = espLoopFault || nanoLoopFault;
  const loopSource =
    espLoopFault && nanoLoopFault
      ? 'ESP32 és Nano'
      : espLoopFault
        ? 'ESP32'
        : 'Nano';
  const maxMsSuffix =
    typeof status.loopMaxMs === 'number' && Number.isFinite(status.loopMaxMs)
      ? ` (max ${status.loopMaxMs} ms)`
      : '';

  const serviceModeActive = Boolean(status.serviceMode);
  const espOfflineActive = status.online === false;

  return {
    overheat: {
      active: overheatActive,
      title: safetyOverheat ? 'KRITIKUS: Kazán túlmelegedés!' : 'Figyelmeztetés: Kazán túlmelegedés',
      body: overheatBody,
      requireInteraction: true,
    },
    pumpFault: {
      active: pumpFaultActive,
      title: 'KRITIKUS: Szivattyúhiba észlelve!',
      body: pumpFaultBody,
      requireInteraction: true,
    },
    sensorFail: {
      active: sensorFailActive,
      title: 'Szenzorhiba a kazánnál',
      body: sensorFailBody,
      requireInteraction: true,
    },
    nrfFail: {
      active: nrfFailActive,
      title: 'NRF24 rádiós kapcsolat megszakadt',
      body: 'Nincs válasz a kazánoldali Nano egységtől — a szivattyú fail-safe módban BE van kapcsolva.',
      requireInteraction: true,
    },
    loopFault: {
      active: loopFaultActive,
      title: 'Loop hiba - ESP/Nano lelassult',
      body: `${loopSource} főciklus túlterhelés / lassulás${maxMsSuffix}. Ellenőrizd a rendszernaplót.`,
      requireInteraction: true,
    },
    serviceMode: {
      active: serviceModeActive,
      title: 'Hardveres szerviz üzemmód aktív',
      body: 'A kazánnál lévő fizikai szerviz kapcsoló (Nano D6) BE lett kapcsolva. A szivattyút közvetlenül a Nano vezérli.',
      requireInteraction: false,
    },
    espOffline: {
      active: espOfflineActive,
      title: 'ESP32-C3 gateway offline',
      body: 'Az ESP32-C3 szobai vezérlő kapcsolata megszakadt az MQTT brokerrel.',
      requireInteraction: false,
    },
  };
}
