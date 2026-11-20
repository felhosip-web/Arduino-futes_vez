import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import {
  AlertTriangle,
  CheckCircle2,
  Flame,
  Hand,
  Loader2,
  RefreshCw,
  Save,
  Thermometer,
  Wrench,
} from 'lucide-react';
import { API_URL, DEVICE_ID } from '../config';
import type {
  HeatingStatusPayload,
  SensorLearnApiResponse,
  SensorRoleKey,
  SensorScanApiResponse,
  SensorScanItem,
} from '../types/heating';

interface SensorsPanelProps {
  status: HeatingStatusPayload;
  onSensorLearned?: () => Promise<void> | void;
}

type TeachingMode = 'manual' | 'heat';

const SENSOR_ROLES: ReadonlyArray<{ key: SensorRoleKey; label: string; hint: string }> = [
  { key: 'eloremeno', label: 'Előremenő', hint: 'Fűtési előremenő vízcső' },
  { key: 'visszatero', label: 'Visszatérő', hint: 'Fűtési visszatérő vízcső' },
  { key: 'kazan', label: 'Kazán', hint: 'Kazántest hőmérő (kritikus)' },
  { key: 'kulteri', label: 'Kültéri', hint: 'Külső hőmérséklet érzékelő' },
];

const HEAT_WINDOW_SEC = 10;
const HEAT_DELTA_C = 2.0;
const LAST_SCAN_STORAGE_KEY = `futes:last-sensor-scan:${DEVICE_ID}`;

interface StoredScanCache {
  scannedAt: number;
  found: SensorScanItem[];
}

interface ReassignConfirmState {
  oldRole: SensorRoleKey;
  oldLabel: string;
  newRole: SensorRoleKey;
  newLabel: string;
  address: string;
}

function loadCachedScan(): StoredScanCache {
  if (typeof window === 'undefined') {
    return { scannedAt: 0, found: [] };
  }
  try {
    const raw = window.localStorage.getItem(LAST_SCAN_STORAGE_KEY);
    if (!raw) return { scannedAt: 0, found: [] };
    const parsed = JSON.parse(raw) as Partial<StoredScanCache>;
    if (!Array.isArray(parsed.found)) return { scannedAt: 0, found: [] };
    const found = parsed.found.filter(
      (item): item is SensorScanItem =>
        Boolean(item) &&
        typeof item.address === 'string' &&
        item.address.length > 0 &&
        (item.temp === null || (typeof item.temp === 'number' && Number.isFinite(item.temp)))
    );
    return {
      scannedAt: typeof parsed.scannedAt === 'number' ? parsed.scannedAt : 0,
      found,
    };
  } catch {
    return { scannedAt: 0, found: [] };
  }
}

function saveCachedScan(found: SensorScanItem[], scannedAt: number): void {
  if (typeof window === 'undefined') return;
  try {
    window.localStorage.setItem(
      LAST_SCAN_STORAGE_KEY,
      JSON.stringify({ scannedAt, found })
    );
  } catch {
    // localStorage nem elérhető
  }
}

function formatShortAddress(address: string | null | undefined): string {
  if (!address) return 'Nincs cím';
  const parts = address.split(':');
  if (parts.length !== 8) return address;
  return `${parts[0]}:${parts[1]}:..:${parts[6]}:${parts[7]}`;
}

function formatTempValue(value: number | null | undefined): string {
  if (typeof value !== 'number' || !Number.isFinite(value) || value <= -120) {
    return '--.- °C';
  }
  return `${value.toFixed(1)} °C`;
}

function formatScanTime(ms: number): string {
  if (!ms) return '';
  return new Date(ms).toLocaleTimeString('hu-HU', {
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
  });
}

export function SensorsPanel({ status, onSensorLearned }: SensorsPanelProps) {
  const initialCache = useMemo(() => loadCachedScan(), []);

  const [teachingOpen, setTeachingOpen] = useState<boolean>(false);
  const [teachingMode, setTeachingMode] = useState<TeachingMode>('manual');
  const [scanning, setScanning] = useState<boolean>(false);
  const [foundSensors, setFoundSensors] = useState<SensorScanItem[]>(initialCache.found);
  const [lastScannedAt, setLastScannedAt] = useState<number>(initialCache.scannedAt);

  // Kézi tanítás állapot (kiválasztott szerepkör + legutóbb keresett szenzorcím)
  const [manualRole, setManualRole] = useState<SensorRoleKey>('eloremeno');
  const [manualAddress, setManualAddress] = useState<string>(
    initialCache.found[0]?.address ?? ''
  );
  const [savingLearn, setSavingLearn] = useState<boolean>(false);
  const [scanError, setScanError] = useState<string | null>(null);
  const [learnFeedback, setLearnFeedback] = useState<string | null>(null);
  const [reassignConfirm, setReassignConfirm] = useState<ReassignConfirmState | null>(null);
  const cancelReassignButtonRef = useRef<HTMLButtonElement | null>(null);

  // 10 másodperces melegítéses azonosítás állapot
  const [heatRole, setHeatRole] = useState<SensorRoleKey>('eloremeno');
  const [heatRunning, setHeatRunning] = useState<boolean>(false);
  const [heatSecondsLeft, setHeatSecondsLeft] = useState<number>(HEAT_WINDOW_SEC);
  const [heatBaseline, setHeatBaseline] = useState<Record<string, number>>({});
  const [heatMessage, setHeatMessage] = useState<string | null>(null);
  const heatTimerRef = useRef<number | null>(null);
  const heatPollRef = useRef<number | null>(null);

  const sensorMap = status.sensors;
  const anyRoleUnknown = useMemo(
    () =>
      Boolean(status.sensorUnknown) ||
      SENSOR_ROLES.some(({ key }) => sensorMap?.[key]?.ok === false),
    [sensorMap, status.sensorUnknown]
  );

  // Szerepekhez jelenleg rendelt ROM címek gyors visszakeresése (akkor is, ha jelenleg !ok)
  const assignedRoleInfoByAddr = useMemo(() => {
    const map = new Map<string, { key: SensorRoleKey; label: string }>();
    if (sensorMap) {
      for (const { key, label } of SENSOR_ROLES) {
        const addr = sensorMap[key]?.address;
        if (addr) {
          map.set(addr.trim().toUpperCase(), { key, label });
        }
      }
    }
    return map;
  }, [sensorMap]);

  // Dupla cím megerősítő ablak megnyitásakor a "Mégse" gomb kapja az alapértelmezett fókuszt
  useEffect(() => {
    if (!reassignConfirm) return;
    const focusTimer = window.setTimeout(() => {
      cancelReassignButtonRef.current?.focus();
    }, 0);
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key === 'Escape') {
        setReassignConfirm(null);
      }
    };
    window.addEventListener('keydown', onKeyDown);
    return () => {
      window.clearTimeout(focusTimer);
      window.removeEventListener('keydown', onKeyDown);
    };
  }, [reassignConfirm]);

  // Ha nincs még kézi cím kiválasztva, ajánljuk fel a választott szerephez vagy az első keresett címet
  useEffect(() => {
    if (foundSensors.length === 0) return;
    setManualAddress((prev) => {
      if (prev && foundSensors.some((s) => s.address === prev)) {
        return prev;
      }
      const currentForRole = sensorMap?.[manualRole]?.address;
      if (currentForRole && foundSensors.some((s) => s.address === currentForRole)) {
        return currentForRole;
      }
      const unassigned = foundSensors.find(
        (s) => !assignedRoleInfoByAddr.has(s.address.trim().toUpperCase())
      );
      return unassigned?.address ?? foundSensors[0]?.address ?? '';
    });
  }, [assignedRoleInfoByAddr, foundSensors, manualRole, sensorMap]);

  const stopHeatHelper = useCallback(() => {
    if (heatTimerRef.current !== null) {
      window.clearInterval(heatTimerRef.current);
      heatTimerRef.current = null;
    }
    if (heatPollRef.current !== null) {
      window.clearInterval(heatPollRef.current);
      heatPollRef.current = null;
    }
    setHeatRunning(false);
  }, []);

  useEffect(() => {
    return () => {
      stopHeatHelper();
    };
  }, [stopHeatHelper]);

  const runScan = useCallback(async (): Promise<SensorScanItem[] | null> => {
    setScanning(true);
    setScanError(null);
    try {
      const res = await fetch(`${API_URL}/api/heating/${DEVICE_ID}/sensor/scan`);
      const body = (await res.json().catch(() => null)) as SensorScanApiResponse | null;
      if (!res.ok || !body?.acked) {
        const reason =
          body?.reason === 'NRF_TIMEOUT' || body?.reason === 'ACK_TIMEOUT'
            ? 'Nincs válasz a Nano-tól — ellenőrizd az NRF24 rádiót.'
            : body?.error || `Szenzor keresési hiba (HTTP ${res.status})`;
        throw new Error(reason);
      }
      const list = Array.isArray(body.found) ? body.found : [];
      const scannedNow = Date.now();
      setFoundSensors(list);
      setLastScannedAt(scannedNow);
      saveCachedScan(list, scannedNow);
      return list;
    } catch (err) {
      const message =
        err instanceof Error ? err.message : 'Nem sikerült lekérdezni a buszon lévő szenzorokat.';
      setScanError(message);
      return null;
    } finally {
      setScanning(false);
    }
  }, []);

  const openPanelInMode = useCallback(
    (mode: TeachingMode, preferredRole?: SensorRoleKey) => {
      setTeachingMode(mode);
      setTeachingOpen(true);
      setLearnFeedback(null);
      setScanError(null);

      if (preferredRole) {
        setManualRole(preferredRole);
        setHeatRole(preferredRole);
        const currentForRole = sensorMap?.[preferredRole]?.address;
        if (currentForRole && foundSensors.some((s) => s.address === currentForRole)) {
          setManualAddress(currentForRole);
        }
      } else {
        const firstUnknown = SENSOR_ROLES.find(({ key }) => sensorMap?.[key]?.ok === false)?.key;
        if (firstUnknown) {
          setManualRole(firstUnknown);
          setHeatRole(firstUnknown);
        }
      }

      if (mode !== 'heat') {
        stopHeatHelper();
      }
      if (foundSensors.length === 0) {
        void runScan();
      }
    },
    [foundSensors, runScan, sensorMap, stopHeatHelper]
  );

  const handleToggleTeaching = () => {
    if (teachingOpen) {
      setTeachingOpen(false);
      stopHeatHelper();
      return;
    }
    openPanelInMode('manual');
  };

  const handleSelectTeachingMode = (nextMode: TeachingMode) => {
    setTeachingMode(nextMode);
    setScanError(null);
    setLearnFeedback(null);
    if (nextMode !== 'heat') {
      stopHeatHelper();
    }
    if (foundSensors.length === 0) {
      void runScan();
    }
  };

  const handleManualRoleChange = (nextRole: SensorRoleKey) => {
    setManualRole(nextRole);
    setLearnFeedback(null);
    const currentForRole = sensorMap?.[nextRole]?.address;
    if (currentForRole && foundSensors.some((s) => s.address === currentForRole)) {
      setManualAddress(currentForRole);
    }
  };

  const submitSensorLearn = useCallback(
    async (
      role: SensorRoleKey,
      addressRaw: string,
      allowReassign = false
    ): Promise<boolean> => {
      const address = addressRaw.trim().toUpperCase();
      if (!address) {
        setScanError('Válassz egy DS18B20 címet a legördülő menüből a mentés előtt.');
        return false;
      }

      const roleLabel = SENSOR_ROLES.find((r) => r.key === role)?.label ?? role;

      if (!allowReassign) {
        const conflict = assignedRoleInfoByAddr.get(address);
        if (conflict && conflict.key !== role) {
          setScanError(null);
          setLearnFeedback(null);
          setReassignConfirm({
            oldRole: conflict.key,
            oldLabel: conflict.label,
            newRole: role,
            newLabel: roleLabel,
            address,
          });
          return false;
        }
      }

      setSavingLearn(true);
      setScanError(null);
      setLearnFeedback(null);

      try {
        const res = await fetch(`${API_URL}/api/heating/${DEVICE_ID}/sensor/learn`, {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ role, address, allowReassign }),
        });
        const body = (await res.json().catch(() => null)) as SensorLearnApiResponse | null;

        if (
          res.status === 409 &&
          body?.error === 'ADDRESS_ALREADY_USED' &&
          body.oldRole &&
          !allowReassign
        ) {
          const oldLabel = SENSOR_ROLES.find((r) => r.key === body.oldRole)?.label ?? body.oldRole;
          setReassignConfirm({
            oldRole: body.oldRole,
            oldLabel,
            newRole: role,
            newLabel: roleLabel,
            address,
          });
          return false;
        }

        if (!res.ok || !body?.acked) {
          const reason =
            body?.reason === 'NRF_TIMEOUT' || body?.reason === 'ACK_TIMEOUT'
              ? 'Nincs visszaigazolás a Nano-tól — ellenőrizd a rádiót.'
              : body?.reason === 'EEPROM_FAIL'
                ? 'EEPROM írási hiba a Nano oldalon.'
                : body?.error || `Tanítási hiba (HTTP ${res.status})`;
          throw new Error(reason);
        }

        if (body.reassignedFrom) {
          const oldLabel =
            SENSOR_ROLES.find((r) => r.key === body.reassignedFrom)?.label ??
            body.reassignedFrom;
          setLearnFeedback(
            `${roleLabel} (${role}) hozzárendelve és elmentve (${address}). A korábbi ${oldLabel} szerep ISMERETLEN lett.`
          );
        } else {
          setLearnFeedback(
            `${roleLabel} (${role}) sikeresen hozzárendelve és elmentve EEPROM-ba: ${address}`
          );
        }
        await onSensorLearned?.();
        return true;
      } catch (err) {
        const message =
          err instanceof Error ? err.message : 'Hiba a szenzor mentése közben.';
        setScanError(message);
        return false;
      } finally {
        setSavingLearn(false);
      }
    },
    [assignedRoleInfoByAddr, onSensorLearned]
  );

  const handleConfirmReassign = async () => {
    if (!reassignConfirm || savingLearn) return;
    const pending = reassignConfirm;
    setReassignConfirm(null);
    await submitSensorLearn(pending.newRole, pending.address, true);
  };

  const handleStartHeatHelper = async () => {
    stopHeatHelper();
    setScanError(null);
    setLearnFeedback(null);
    setHeatMessage('Alapértékek rögzítése...');
    const initialList = await runScan();
    if (!initialList || initialList.length === 0) {
      setHeatMessage('Nem található DS18B20 szenzor a buszon.');
      return;
    }

    const base: Record<string, number> = {};
    for (const item of initialList) {
      if (typeof item.temp === 'number' && Number.isFinite(item.temp)) {
        base[item.address] = item.temp;
      }
    }
    setHeatBaseline(base);
    setHeatSecondsLeft(HEAT_WINDOW_SEC);
    setHeatRunning(true);
    const targetLabel = SENSOR_ROLES.find((r) => r.key === heatRole)?.label ?? heatRole;
    setHeatMessage(`Melegítsd kézzel a(z) ${targetLabel} szenzort (+${HEAT_DELTA_C.toFixed(1)} °C)...`);

    const startedAt = Date.now();
    heatTimerRef.current = window.setInterval(() => {
      const elapsedSec = Math.floor((Date.now() - startedAt) / 1000);
      const remaining = Math.max(0, HEAT_WINDOW_SEC - elapsedSec);
      setHeatSecondsLeft(remaining);
      if (remaining === 0) {
        stopHeatHelper();
        setHeatMessage('Nem melegedett egyik szenzor sem > 2,0 °C-ot 10 mp alatt. Próbáld újra!');
      }
    }, 500);

    heatPollRef.current = window.setInterval(() => {
      void (async () => {
        const latest = await runScan();
        if (!latest) return;

        let bestMatch: { address: string; delta: number } | null = null;
        for (const item of latest) {
          const baseTemp = base[item.address];
          if (
            typeof baseTemp === 'number' &&
            typeof item.temp === 'number' &&
            Number.isFinite(item.temp)
          ) {
            const delta = item.temp - baseTemp;
            if (delta >= HEAT_DELTA_C && (!bestMatch || delta > bestMatch.delta)) {
              bestMatch = { address: item.address, delta };
            }
          }
        }

        if (bestMatch) {
          stopHeatHelper();
          setManualRole(heatRole);
          setManualAddress(bestMatch.address);
          setHeatMessage(
            `Azonosítva (+${bestMatch.delta.toFixed(1)} °C): ${formatShortAddress(bestMatch.address)} — kattints a Mentésre a rögzítéshez.`
          );
        }
      })();
    }, 2000);
  };

  return (
    <section
      aria-labelledby="sensors-panel-title"
      className="rounded-xl border border-slate-800 bg-slate-900 p-5 sm:p-6"
    >
      {/* Fejléc */}
      <div className="flex flex-wrap items-center justify-between gap-4 border-b border-slate-800/80 pb-4">
        <div className="flex items-start gap-3">
          <Thermometer className="mt-0.5 h-5 w-5 shrink-0 text-orange-400" aria-hidden="true" />
          <div>
            <div className="flex flex-wrap items-center gap-2.5">
              <h2 id="sensors-panel-title" className="text-lg font-semibold text-slate-100">
                Szenzorok
              </h2>
              {anyRoleUnknown ? (
                <span className="inline-flex items-center gap-1.5 rounded border border-red-500 bg-red-950/80 px-2.5 py-0.5 text-xs font-bold text-red-200 whitespace-nowrap animate-pulse">
                  <AlertTriangle className="h-3.5 w-3.5" aria-hidden="true" />
                  <span>TANÍTÁS SZÜKSÉGES</span>
                </span>
              ) : (
                <span className="inline-flex items-center gap-1.5 rounded border border-emerald-500/40 bg-emerald-950/60 px-2.5 py-0.5 text-xs font-semibold text-emerald-300 whitespace-nowrap">
                  <CheckCircle2 className="h-3.5 w-3.5" aria-hidden="true" />
                  <span>4/4 AZONOSÍTVA</span>
                </span>
              )}
            </div>
            <p className="mt-0.5 text-xs text-slate-400">
              DS18B20 OneWire auto-discovery és EEPROM szerep-társítás (csere esetén újratanítható)
            </p>
          </div>
        </div>

        <div className="flex flex-wrap items-center gap-2">
          <button
            type="button"
            onClick={() => openPanelInMode('manual')}
            className={`inline-flex min-h-[44px] items-center gap-2 rounded-lg border px-3.5 py-2 text-xs font-semibold whitespace-nowrap shrink-0 transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-orange-500 sm:text-sm ${
              teachingOpen && teachingMode === 'manual'
                ? 'border-orange-500 bg-orange-950/60 text-orange-200'
                : 'border-slate-700 bg-slate-950 text-slate-200 hover:bg-slate-800'
            }`}
          >
            <Hand className="h-4 w-4" aria-hidden="true" />
            <span>Kézi tanítás</span>
          </button>

          <button
            type="button"
            onClick={handleToggleTeaching}
            className={`inline-flex min-h-[44px] items-center gap-2 rounded-lg px-4 py-2 text-xs font-semibold whitespace-nowrap shrink-0 transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-orange-500 sm:text-sm ${
              anyRoleUnknown
                ? 'bg-orange-500 text-slate-950 hover:bg-orange-400'
                : 'border border-slate-700 bg-slate-950 text-slate-200 hover:bg-slate-800'
            }`}
          >
            <Wrench className="h-4 w-4" aria-hidden="true" />
            <span>{teachingOpen ? 'Tanítás bezárása' : 'Tanítás indítása'}</span>
          </button>
        </div>
      </div>

      {/* Figyelmeztetés ismeretlen/cserélt szenzor esetén */}
      {anyRoleUnknown && (
        <div
          role="alert"
          className="mt-4 flex flex-wrap items-center justify-between gap-3 rounded-lg border border-red-500/60 bg-red-950/70 px-4 py-3 text-sm text-red-100"
        >
          <div className="flex items-center gap-2.5">
            <AlertTriangle className="h-5 w-5 shrink-0 text-red-400" aria-hidden="true" />
            <span>
              <strong>Ismeretlen vagy hiányzó DS18B20 szenzor!</strong> Szenzorcsere vagy első
              indítás után társítsd a fizikai érzékelőket a szerepekhez.
            </span>
          </div>
          {!teachingOpen && (
            <button
              type="button"
              onClick={() => openPanelInMode('manual')}
              className="inline-flex min-h-[40px] items-center gap-1.5 rounded-md bg-red-500 px-3 py-1.5 text-xs font-bold text-slate-950 hover:bg-red-400 whitespace-nowrap"
            >
              Tanítás indítása
            </button>
          )}
        </div>
      )}

      {/* 4 szerep áttekintő rács */}
      <div className="mt-4 grid gap-3 sm:grid-cols-2 lg:grid-cols-4">
        {SENSOR_ROLES.map(({ key, label, hint }) => {
          const info = sensorMap?.[key];
          const isOk = Boolean(info?.ok) && !Boolean(status.sensorUnknown && !info?.ok);
          const tempVal = status.homersekletek?.[key];
          return (
            <div
              key={key}
              className={`rounded-lg border p-4 ${
                info && !info.ok
                  ? 'border-red-500/60 bg-red-950/20'
                  : 'border-slate-800/90 bg-slate-950/60'
              }`}
            >
              <div className="flex items-center justify-between gap-2">
                <span className="text-sm font-semibold text-slate-100">{label}</span>
                <span
                  className={`rounded border px-2 py-0.5 text-[11px] font-semibold whitespace-nowrap ${
                    info?.ok
                      ? 'border-emerald-500/40 bg-emerald-950/60 text-emerald-300'
                      : 'border-red-500/60 bg-red-950/70 text-red-300'
                  }`}
                >
                  {info ? (info.ok ? 'OK' : 'ISMERETLEN') : isOk ? '—' : 'ISMERETLEN'}
                </span>
              </div>
              <p className="mt-2 font-mono text-xl font-bold text-slate-50 tabular-nums">
                {formatTempValue(tempVal)}
              </p>
              <p
                title={info?.address ?? 'Nincs társított EEPROM cím'}
                className="mt-1.5 truncate font-mono text-xs text-slate-400"
              >
                {formatShortAddress(info?.address)}
              </p>
              <div className="mt-2 flex items-center justify-between gap-2">
                <p className="truncate text-[11px] text-slate-500">{hint}</p>
                <button
                  type="button"
                  onClick={() => openPanelInMode('manual', key)}
                  className="text-[11px] font-semibold text-orange-400 hover:text-orange-300 whitespace-nowrap"
                >
                  Módosít
                </button>
              </div>
            </div>
          );
        })}
      </div>

      {/* Tanítási felület */}
      {teachingOpen && (
        <div className="mt-5 space-y-5 rounded-xl border border-slate-800 bg-slate-950/70 p-4 sm:p-5">
          {/* Mód választó és frissítés */}
          <div className="flex flex-wrap items-center justify-between gap-3 border-b border-slate-800/80 pb-4">
            <div
              role="tablist"
              aria-label="Szenzor tanítási módok"
              className="inline-flex items-center gap-1 rounded-lg border border-slate-800 bg-slate-900 p-1"
            >
              <button
                type="button"
                role="tab"
                aria-selected={teachingMode === 'manual'}
                onClick={() => handleSelectTeachingMode('manual')}
                className={`inline-flex min-h-[38px] items-center gap-2 rounded-md px-3.5 py-1.5 text-xs font-semibold transition-colors ${
                  teachingMode === 'manual'
                    ? 'bg-orange-500 text-slate-950'
                    : 'text-slate-300 hover:bg-slate-800 hover:text-slate-100'
                }`}
              >
                <Hand className="h-3.5 w-3.5" aria-hidden="true" />
                <span>Kézi tanítás</span>
              </button>

              <button
                type="button"
                role="tab"
                aria-selected={teachingMode === 'heat'}
                onClick={() => handleSelectTeachingMode('heat')}
                className={`inline-flex min-h-[38px] items-center gap-2 rounded-md px-3.5 py-1.5 text-xs font-semibold transition-colors ${
                  teachingMode === 'heat'
                    ? 'bg-orange-500 text-slate-950'
                    : 'text-slate-300 hover:bg-slate-800 hover:text-slate-100'
                }`}
              >
                <Flame className="h-3.5 w-3.5" aria-hidden="true" />
                <span>Melegítéses tanítás (10 mp)</span>
              </button>
            </div>

            <div className="flex items-center gap-3">
              {lastScannedAt > 0 && (
                <span className="text-xs text-slate-400 tabular-nums">
                  Utolsó keresés: {formatScanTime(lastScannedAt)}
                </span>
              )}
              <button
                type="button"
                onClick={() => void runScan()}
                disabled={scanning}
                className="inline-flex min-h-[40px] items-center gap-2 rounded-lg border border-slate-700 bg-slate-900 px-3.5 py-2 text-xs font-semibold text-slate-200 hover:bg-slate-800 disabled:opacity-60"
              >
                <RefreshCw className={`h-3.5 w-3.5 ${scanning ? 'animate-spin' : ''}`} />
                <span>{scanning ? 'Keresés...' : 'Keresés frissítése'}</span>
              </button>
            </div>
          </div>

          {scanError && (
            <div
              role="alert"
              className="flex items-center gap-2.5 rounded-lg border border-red-500/50 bg-red-950/60 px-4 py-2.5 text-xs text-red-200 sm:text-sm"
            >
              <AlertTriangle className="h-4 w-4 shrink-0 text-red-400" aria-hidden="true" />
              <span>{scanError}</span>
            </div>
          )}

          {learnFeedback && (
            <div
              role="status"
              className="flex items-center gap-2.5 rounded-lg border border-emerald-500/50 bg-emerald-950/50 px-4 py-2.5 text-xs text-emerald-200 sm:text-sm"
            >
              <CheckCircle2 className="h-4 w-4 shrink-0 text-emerald-400" aria-hidden="true" />
              <span>{learnFeedback}</span>
            </div>
          )}

          {/* MÓD 1: Kézi tanítás */}
          {teachingMode === 'manual' && (
            <div className="rounded-lg border border-slate-800 bg-slate-900/80 p-4 space-y-4">
              <div>
                <h3 className="text-sm font-semibold text-slate-100 sm:text-base">
                  Kézi tanítás (cím hozzárendelése szerepkörhöz)
                </h3>
                <p className="mt-0.5 text-xs text-slate-400">
                  Válaszd ki a cél szerepkört és a legutóbb keresett szenzorcímek egyikét, majd
                  küldd el a <span className="font-mono text-slate-300">sensorLearn</span>{' '}
                  parancsot az EEPROM-ba való mentéshez.
                </p>
              </div>

              <div className="grid gap-4 sm:grid-cols-2">
                <div>
                  <label
                    htmlFor="manual-role-select"
                    className="mb-1.5 block text-xs font-semibold uppercase tracking-wide text-slate-300"
                  >
                    Szerepkör
                  </label>
                  <select
                    id="manual-role-select"
                    value={manualRole}
                    onChange={(e) => handleManualRoleChange(e.target.value as SensorRoleKey)}
                    disabled={savingLearn}
                    className="min-h-[44px] w-full rounded-lg border border-slate-700 bg-slate-950 px-3 py-2 text-sm text-slate-100 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-orange-500"
                  >
                    {SENSOR_ROLES.map(({ key, label }) => {
                      const roleInfo = sensorMap?.[key];
                      const badgeText = roleInfo
                        ? roleInfo.ok
                          ? 'OK'
                          : 'ISMERETLEN'
                        : 'nincs adat';
                      return (
                        <option key={key} value={key}>
                          {label} ({key}) — {badgeText}
                        </option>
                      );
                    })}
                  </select>
                </div>

                <div>
                  <label
                    htmlFor="manual-address-select"
                    className="mb-1.5 block text-xs font-semibold uppercase tracking-wide text-slate-300"
                  >
                    Szenzorcím (legutóbbi keresésből: {foundSensors.length} db)
                  </label>
                  <select
                    id="manual-address-select"
                    value={manualAddress}
                    onChange={(e) => setManualAddress(e.target.value)}
                    disabled={savingLearn || foundSensors.length === 0}
                    className="min-h-[44px] w-full rounded-lg border border-slate-700 bg-slate-950 px-3 py-2 font-mono text-xs text-slate-100 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-orange-500 sm:text-sm"
                  >
                    {foundSensors.length === 0 ? (
                      <option value="">
                        {scanning
                          ? 'Keresés folyamatban...'
                          : 'Nincs keresett cím — indíts keresést'}
                      </option>
                    ) : (
                      <>
                        <option value="">— Válassz DS18B20 szenzorcímet —</option>
                        {foundSensors.map((item) => {
                          const assignedInfo = assignedRoleInfoByAddr.get(
                            item.address.trim().toUpperCase()
                          );
                          return (
                            <option key={item.address} value={item.address}>
                              {item.address}
                              {item.temp !== null ? ` (${item.temp.toFixed(1)} °C)` : ''}
                              {assignedInfo ? ` — ${assignedInfo.label}` : ''}
                            </option>
                          );
                        })}
                      </>
                    )}
                  </select>
                </div>
              </div>

              <div className="flex flex-wrap items-center justify-between gap-3 pt-1">
                <p className="text-xs text-slate-400">
                  Jelenleg mentve a(z){' '}
                  <span className="font-semibold text-slate-200">
                    {SENSOR_ROLES.find((r) => r.key === manualRole)?.label}
                  </span>{' '}
                  szerephez:{' '}
                  <span className="font-mono text-slate-300">
                    {sensorMap?.[manualRole]?.address ?? 'nincs társított cím'}
                  </span>
                </p>

                <button
                  type="button"
                  onClick={() => void submitSensorLearn(manualRole, manualAddress)}
                  disabled={savingLearn || !manualAddress}
                  className="inline-flex min-h-[44px] items-center gap-2 rounded-lg bg-orange-500 px-4 py-2 text-sm font-semibold text-slate-950 transition-colors hover:bg-orange-400 disabled:opacity-50"
                >
                  {savingLearn ? (
                    <Loader2 className="h-4 w-4 animate-spin" />
                  ) : (
                    <Save className="h-4 w-4" />
                  )}
                  <span>{savingLearn ? 'Küldés...' : 'Hozzárendelés és mentés'}</span>
                </button>
              </div>
            </div>
          )}

          {/* MÓD 2: 10 mp-es melegítéses azonosítás */}
          {teachingMode === 'heat' && (
            <div className="rounded-lg border border-slate-800 bg-slate-900/80 p-4 space-y-3">
              <div className="flex flex-wrap items-center justify-between gap-3">
                <div>
                  <h3 className="text-sm font-semibold text-slate-100 sm:text-base">
                    Melegítéses azonosítás (10 mp, &gt; {HEAT_DELTA_C.toFixed(1)} °C)
                  </h3>
                  <p className="mt-0.5 text-xs text-slate-400">
                    Válaszd ki a szerepkört, indítsd el a figyelést, majd fogd meg kézzel az adott
                    érzékelőt 10 másodpercig.
                  </p>
                </div>

                <div className="flex flex-wrap items-center gap-2">
                  <select
                    aria-label="Azonosítandó szerep"
                    value={heatRole}
                    onChange={(e) => setHeatRole(e.target.value as SensorRoleKey)}
                    disabled={heatRunning || savingLearn}
                    className="min-h-[40px] rounded-md border border-slate-700 bg-slate-950 px-3 py-1.5 text-xs text-slate-100"
                  >
                    {SENSOR_ROLES.map(({ key, label }) => (
                      <option key={key} value={key}>
                        {label}
                      </option>
                    ))}
                  </select>
                  {!heatRunning ? (
                    <button
                      type="button"
                      onClick={() => void handleStartHeatHelper()}
                      disabled={scanning || savingLearn}
                      className="inline-flex min-h-[40px] items-center gap-1.5 rounded-md border border-orange-500/60 bg-orange-950/60 px-3.5 py-2 text-xs font-semibold text-orange-200 hover:bg-orange-900/70 disabled:opacity-50"
                    >
                      <Flame className="h-3.5 w-3.5" />
                      <span>Melegítés indítása</span>
                    </button>
                  ) : (
                    <button
                      type="button"
                      onClick={stopHeatHelper}
                      className="inline-flex min-h-[40px] items-center gap-1.5 rounded-md border border-slate-600 bg-slate-800 px-3.5 py-2 text-xs font-semibold text-slate-200"
                    >
                      <span>Leállítás ({heatSecondsLeft} mp)</span>
                    </button>
                  )}
                </div>
              </div>

              {heatMessage && (
                <div className="flex flex-wrap items-center justify-between gap-2 rounded-md border border-slate-800 bg-slate-950/70 px-3.5 py-2.5 text-xs text-slate-200">
                  <span>{heatMessage}</span>
                  {!heatRunning && manualAddress && manualRole === heatRole && (
                    <button
                      type="button"
                      onClick={() => void submitSensorLearn(manualRole, manualAddress)}
                      disabled={savingLearn}
                      className="inline-flex min-h-[34px] items-center gap-1.5 rounded-md bg-orange-500 px-3 py-1 text-xs font-semibold text-slate-950 hover:bg-orange-400 disabled:opacity-50"
                    >
                      {savingLearn ? (
                        <Loader2 className="h-3.5 w-3.5 animate-spin" />
                      ) : (
                        <Save className="h-3.5 w-3.5" />
                      )}
                      <span>Mentés EEPROM-ba</span>
                    </button>
                  )}
                </div>
              )}
            </div>
          )}

          {/* Legutóbb keresett szenzorok áttekintése */}
          <div>
            <p className="mb-2 text-xs font-semibold uppercase tracking-wide text-slate-400">
              Legutóbb keresett szenzorcímek ({foundSensors.length} db)
            </p>
            {scanning && foundSensors.length === 0 ? (
              <div className="flex items-center gap-2 rounded-lg border border-slate-800 bg-slate-900/60 px-4 py-3 text-xs text-slate-400">
                <Loader2 className="h-4 w-4 animate-spin text-orange-400" />
                <span>Szenzorok lekérdezése a Nano-tól...</span>
              </div>
            ) : foundSensors.length === 0 ? (
              <div className="rounded-lg border border-slate-800 bg-slate-900/60 px-4 py-3 text-xs text-slate-400">
                Még nincs keresett szenzorcím — nyomd meg a „Keresés frissítése” gombot.
              </div>
            ) : (
              <div className="grid gap-2 sm:grid-cols-2">
                {foundSensors.map((sensor) => {
                  const assignedInfo = assignedRoleInfoByAddr.get(
                    sensor.address.trim().toUpperCase()
                  );
                  const assignedLabel = assignedInfo?.label;
                  const baseTemp = heatBaseline[sensor.address];
                  const delta =
                    typeof baseTemp === 'number' && typeof sensor.temp === 'number'
                      ? sensor.temp - baseTemp
                      : null;
                  const heated = delta !== null && delta >= HEAT_DELTA_C;
                  const isSelected = teachingMode === 'manual' && manualAddress === sensor.address;

                  return (
                    <button
                      type="button"
                      key={sensor.address}
                      onClick={() => {
                        setTeachingMode('manual');
                        setManualAddress(sensor.address);
                      }}
                      className={`flex items-center justify-between gap-3 rounded-lg border px-3.5 py-2.5 text-left text-xs transition-colors ${
                        heated
                          ? 'border-orange-500 bg-orange-950/40'
                          : isSelected
                            ? 'border-orange-500/80 bg-slate-800/80'
                            : 'border-slate-800 bg-slate-900/80 hover:border-slate-700'
                      }`}
                    >
                      <div className="min-w-0">
                        <p className="truncate font-mono font-medium text-slate-100">
                          {sensor.address}
                        </p>
                        <p className="mt-0.5 text-[11px] text-slate-400">
                          {assignedLabel ? `Jelenleg: ${assignedLabel}` : 'Még nincs társítva'}
                          {delta !== null
                            ? ` · Δ ${delta >= 0 ? '+' : ''}${delta.toFixed(1)} °C`
                            : ''}
                        </p>
                      </div>
                      <span className="font-mono text-sm font-bold text-slate-100 tabular-nums shrink-0">
                        {formatTempValue(sensor.temp)}
                      </span>
                    </button>
                  );
                })}
              </div>
            )}
          </div>
        </div>
      )}

      {/* Dupla cím figyelmeztető és áthelyezést megerősítő ablak (alapértelmezetten a Mégse van fókuszban) */}
      {reassignConfirm && (
        <div
          role="dialog"
          aria-modal="true"
          aria-labelledby="reassign-dialog-title"
          className="fixed inset-0 z-50 flex items-center justify-center bg-slate-950/80 p-4 backdrop-blur-sm"
        >
          <div className="w-full max-w-md rounded-xl border border-amber-500/70 bg-slate-900 p-6 shadow-2xl">
            <div className="flex items-start gap-3">
              <div className="rounded-lg border border-amber-500/50 bg-amber-950/60 p-2.5 text-amber-400">
                <AlertTriangle className="h-5 w-5" aria-hidden="true" />
              </div>
              <div className="min-w-0">
                <h3
                  id="reassign-dialog-title"
                  className="text-base font-bold text-slate-100"
                >
                  Cím már használatban van
                </h3>
                <p className="mt-1 font-mono text-xs text-amber-300">
                  {reassignConfirm.address}
                </p>
              </div>
            </div>

            <p className="mt-4 text-sm leading-relaxed text-slate-200">
              Ez a cím már a{' '}
              <span className="font-semibold text-amber-300">{reassignConfirm.oldLabel}</span>{' '}
              szerephez tartozik. Áthelyezed az{' '}
              <span className="font-semibold text-orange-300">{reassignConfirm.newLabel}</span>
              -re? A{' '}
              <span className="font-semibold text-red-300">
                {reassignConfirm.oldLabel} ISMERETLEN lesz
              </span>
              .
            </p>

            <div className="mt-6 flex flex-wrap justify-end gap-3">
              <button
                type="button"
                onClick={() => void handleConfirmReassign()}
                disabled={savingLearn}
                className="inline-flex min-h-[44px] items-center justify-center rounded-lg border border-amber-500/70 bg-amber-500 px-4 py-2 text-sm font-semibold text-slate-950 hover:bg-amber-400 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-amber-300 disabled:opacity-50"
              >
                {savingLearn ? 'Áthelyezés...' : 'Áthelyezés'}
              </button>
              <button
                ref={cancelReassignButtonRef}
                type="button"
                autoFocus
                onClick={() => setReassignConfirm(null)}
                className="inline-flex min-h-[44px] items-center justify-center rounded-lg border border-slate-600 bg-slate-800 px-4 py-2 text-sm font-semibold text-slate-100 hover:bg-slate-700 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-orange-400"
              >
                Mégse
              </button>
            </div>
          </div>
        </div>
      )}
    </section>
  );
}
