import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import {
  AlertTriangle,
  CheckCircle2,
  Flame,
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

const SENSOR_ROLES: ReadonlyArray<{ key: SensorRoleKey; label: string; hint: string }> = [
  { key: 'eloremeno', label: 'Előremenő', hint: 'Fűtési előremenő vízcső' },
  { key: 'visszatero', label: 'Visszatérő', hint: 'Fűtési visszatérő vízcső' },
  { key: 'kazan', label: 'Kazán', hint: 'Kazántest hőmérő (kritikus)' },
  { key: 'kulteri', label: 'Kültéri', hint: 'Külső hőmérséklet érzékelő' },
];

const HEAT_WINDOW_SEC = 10;
const HEAT_DELTA_C = 2.0;

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

export function SensorsPanel({ status, onSensorLearned }: SensorsPanelProps) {
  const [teachingOpen, setTeachingOpen] = useState<boolean>(false);
  const [scanning, setScanning] = useState<boolean>(false);
  const [foundSensors, setFoundSensors] = useState<SensorScanItem[]>([]);
  const [selectedByRole, setSelectedByRole] = useState<Record<SensorRoleKey, string>>({
    eloremeno: '',
    visszatero: '',
    kazan: '',
    kulteri: '',
  });
  const [savingRole, setSavingRole] = useState<SensorRoleKey | null>(null);
  const [scanError, setScanError] = useState<string | null>(null);
  const [learnFeedback, setLearnFeedback] = useState<string | null>(null);

  // Opcionális 10 másodperces melegítéses azonosítás segéd
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

  // Alapértelmezett kiválasztás frissítése a jelenlegi EEPROM címekből
  useEffect(() => {
    setSelectedByRole((prev) => {
      const next = { ...prev };
      for (const { key } of SENSOR_ROLES) {
        const currentAddr = sensorMap?.[key]?.address;
        if (!next[key] && currentAddr) {
          next[key] = currentAddr;
        }
      }
      return next;
    });
  }, [sensorMap]);

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
      setFoundSensors(list);
      setSelectedByRole((prev) => {
        const next = { ...prev };
        for (const { key } of SENSOR_ROLES) {
          const currentAddr = sensorMap?.[key]?.address;
          if (!next[key]) {
            if (currentAddr && list.some((s) => s.address === currentAddr)) {
              next[key] = currentAddr;
            } else if (list.length > 0 && !sensorMap?.[key]?.ok) {
              // Ha még nincs társítva szerephez, ajánljuk fel az első még szabad címet
              const used = new Set(
                SENSOR_ROLES.map((r) => sensorMap?.[r.key]?.address).filter(Boolean)
              );
              const unassigned = list.find((s) => !used.has(s.address));
              if (unassigned) {
                next[key] = unassigned.address;
              }
            }
          }
        }
        return next;
      });
      return list;
    } catch (err) {
      const message =
        err instanceof Error ? err.message : 'Nem sikerült lekérdezni a buszon lévő szenzorokat.';
      setScanError(message);
      return null;
    } finally {
      setScanning(false);
    }
  }, [sensorMap]);

  const handleOpenTeaching = () => {
    const nextOpen = !teachingOpen;
    setTeachingOpen(nextOpen);
    setLearnFeedback(null);
    if (nextOpen) {
      void runScan();
    } else {
      stopHeatHelper();
    }
  };

  const handleLearnRole = useCallback(
    async (role: SensorRoleKey, addressOverride?: string) => {
      const address = (addressOverride ?? selectedByRole[role] ?? '').trim().toUpperCase();
      if (!address) {
        setScanError('Válassz egy DS18B20 címet a mentés előtt.');
        return;
      }

      setSavingRole(role);
      setScanError(null);
      setLearnFeedback(null);

      try {
        const res = await fetch(`${API_URL}/api/heating/${DEVICE_ID}/sensor/learn`, {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ role, address }),
        });
        const body = (await res.json().catch(() => null)) as SensorLearnApiResponse | null;
        if (!res.ok || !body?.acked) {
          const reason =
            body?.reason === 'NRF_TIMEOUT' || body?.reason === 'ACK_TIMEOUT'
              ? 'Nincs visszaigazolás a Nano-tól — ellenőrizd a rádiót.'
              : body?.reason === 'EEPROM_FAIL'
                ? 'EEPROM írási hiba a Nano oldalon.'
                : body?.error || `Tanítási hiba (HTTP ${res.status})`;
          throw new Error(reason);
        }

        const roleLabel = SENSOR_ROLES.find((r) => r.key === role)?.label ?? role;
        setLearnFeedback(`${roleLabel} szenzor sikeresen elmentve EEPROM-ba (${formatShortAddress(address)}).`);
        setSelectedByRole((prev) => ({ ...prev, [role]: address }));
        await onSensorLearned?.();
      } catch (err) {
        const message =
          err instanceof Error ? err.message : 'Hiba a szenzor mentése közben.';
        setScanError(message);
      } finally {
        setSavingRole(null);
      }
    },
    [onSensorLearned, selectedByRole]
  );

  const handleStartHeatHelper = async () => {
    stopHeatHelper();
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
          setSelectedByRole((prev) => ({ ...prev, [heatRole]: bestMatch.address }));
          setHeatMessage(
            `Azonosítva (+${bestMatch.delta.toFixed(1)} °C): ${formatShortAddress(bestMatch.address)} — kattints a Mentésre vagy már társítható.`
          );
        }
      })();
    }, 2000);
  };

  // Mely szerepekhez van rendelve egy adott ROM cím
  const assignedRoleByAddr = useMemo(() => {
    const map = new Map<string, string>();
    if (sensorMap) {
      for (const { key, label } of SENSOR_ROLES) {
        const addr = sensorMap[key]?.address;
        if (addr && sensorMap[key]?.ok) {
          map.set(addr, label);
        }
      }
    }
    return map;
  }, [sensorMap]);

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

        <button
          type="button"
          onClick={handleOpenTeaching}
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
              onClick={handleOpenTeaching}
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
              <p className="mt-0.5 text-[11px] text-slate-500">{hint}</p>
            </div>
          );
        })}
      </div>

      {/* Tanítási felület */}
      {teachingOpen && (
        <div className="mt-5 space-y-5 rounded-xl border border-slate-800 bg-slate-950/70 p-4 sm:p-5">
          <div className="flex flex-wrap items-center justify-between gap-3">
            <div>
              <h3 className="text-sm font-semibold text-slate-100 sm:text-base">
                DS18B20 szerep-hozzárendelés (EEPROM tanítás)
              </h3>
              <p className="mt-0.5 text-xs text-slate-400">
                Válaszd ki a buszon talált szenzorok közül a megfelelő címet, vagy használd a 10 mp-es
                melegítéses azonosítót.
              </p>
            </div>
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

          {/* Buszon talált szenzorok listája */}
          <div>
            <p className="mb-2 text-xs font-semibold uppercase tracking-wide text-slate-400">
              OneWire buszon talált érzékelők ({foundSensors.length} db)
            </p>
            {scanning && foundSensors.length === 0 ? (
              <div className="flex items-center gap-2 rounded-lg border border-slate-800 bg-slate-900/60 px-4 py-3 text-xs text-slate-400">
                <Loader2 className="h-4 w-4 animate-spin text-orange-400" />
                <span>Szenzorok lekérdezése a Nano-tól...</span>
              </div>
            ) : foundSensors.length === 0 ? (
              <div className="rounded-lg border border-slate-800 bg-slate-900/60 px-4 py-3 text-xs text-slate-400">
                Még nincs beolvasott szenzorlista — nyomd meg a „Keresés frissítése” gombot.
              </div>
            ) : (
              <div className="grid gap-2 sm:grid-cols-2">
                {foundSensors.map((sensor) => {
                  const assignedLabel = assignedRoleByAddr.get(sensor.address);
                  const baseTemp = heatBaseline[sensor.address];
                  const delta =
                    typeof baseTemp === 'number' && typeof sensor.temp === 'number'
                      ? sensor.temp - baseTemp
                      : null;
                  const heated = delta !== null && delta >= HEAT_DELTA_C;

                  return (
                    <div
                      key={sensor.address}
                      className={`flex items-center justify-between gap-3 rounded-lg border px-3.5 py-2.5 text-xs ${
                        heated
                          ? 'border-orange-500 bg-orange-950/40'
                          : 'border-slate-800 bg-slate-900/80'
                      }`}
                    >
                      <div className="min-w-0">
                        <p className="truncate font-mono font-medium text-slate-100">
                          {sensor.address}
                        </p>
                        <p className="mt-0.5 text-[11px] text-slate-400">
                          {assignedLabel ? `Jelenleg: ${assignedLabel}` : 'Még nincs társítva'}
                          {delta !== null ? ` · Δ ${delta >= 0 ? '+' : ''}${delta.toFixed(1)} °C` : ''}
                        </p>
                      </div>
                      <span className="font-mono text-sm font-bold text-slate-100 tabular-nums shrink-0">
                        {formatTempValue(sensor.temp)}
                      </span>
                    </div>
                  );
                })}
              </div>
            )}
          </div>

          {/* Kézi dropdown hozzárendelés szerepenként */}
          <div className="space-y-2.5">
            <p className="text-xs font-semibold uppercase tracking-wide text-slate-400">
              Szerep-társítás (EEPROM mentés)
            </p>
            <div className="grid gap-2.5">
              {SENSOR_ROLES.map(({ key, label }) => {
                const isSaving = savingRole === key;
                const currentValue = selectedByRole[key] ?? '';
                const options = [...foundSensors];
                const savedAddr = sensorMap?.[key]?.address;
                if (savedAddr && !options.some((o) => o.address === savedAddr)) {
                  options.unshift({ address: savedAddr, temp: null });
                }

                return (
                  <div
                    key={key}
                    className="flex flex-col gap-2 rounded-lg border border-slate-800 bg-slate-900/80 p-3 sm:flex-row sm:items-center sm:justify-between"
                  >
                    <label
                      htmlFor={`sensor-select-${key}`}
                      className="text-sm font-medium text-slate-200 sm:w-32 shrink-0"
                    >
                      {label}
                    </label>
                    <div className="flex flex-1 flex-wrap items-center gap-2">
                      <select
                        id={`sensor-select-${key}`}
                        value={currentValue}
                        onChange={(e) =>
                          setSelectedByRole((prev) => ({ ...prev, [key]: e.target.value }))
                        }
                        disabled={isSaving}
                        className="min-h-[40px] flex-1 rounded-lg border border-slate-700 bg-slate-950 px-3 py-1.5 font-mono text-xs text-slate-100 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-orange-500"
                      >
                        <option value="">— Válassz DS18B20 címet —</option>
                        {options.map((item) => (
                          <option key={item.address} value={item.address}>
                            {item.address}
                            {item.temp !== null ? ` (${item.temp.toFixed(1)} °C)` : ''}
                          </option>
                        ))}
                      </select>
                      <button
                        type="button"
                        onClick={() => void handleLearnRole(key)}
                        disabled={isSaving || !currentValue}
                        className="inline-flex min-h-[40px] items-center gap-1.5 rounded-lg bg-orange-500 px-3.5 py-2 text-xs font-semibold text-slate-950 hover:bg-orange-400 disabled:opacity-50 whitespace-nowrap"
                      >
                        {isSaving ? (
                          <Loader2 className="h-3.5 w-3.5 animate-spin" />
                        ) : (
                          <Save className="h-3.5 w-3.5" />
                        )}
                        <span>{isSaving ? 'Mentés...' : 'Mentés'}</span>
                      </button>
                    </div>
                  </div>
                );
              })}
            </div>
          </div>

          {/* Opcionális 10 mp-es melegítéses azonosító */}
          <div className="rounded-lg border border-slate-800 bg-slate-900/70 p-3.5">
            <div className="flex flex-wrap items-center justify-between gap-3">
              <div className="flex items-center gap-2 text-xs font-semibold text-slate-200">
                <Flame className="h-4 w-4 text-orange-400" aria-hidden="true" />
                <span>Melegítéses azonosítás (10 mp, &gt; 2 °C)</span>
              </div>
              <div className="flex flex-wrap items-center gap-2">
                <select
                  aria-label="Azonosítandó szerep"
                  value={heatRole}
                  onChange={(e) => setHeatRole(e.target.value as SensorRoleKey)}
                  disabled={heatRunning}
                  className="min-h-[38px] rounded-md border border-slate-700 bg-slate-950 px-2.5 py-1 text-xs text-slate-100"
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
                    disabled={scanning}
                    className="inline-flex min-h-[38px] items-center gap-1.5 rounded-md border border-orange-500/60 bg-orange-950/60 px-3 py-1.5 text-xs font-semibold text-orange-200 hover:bg-orange-900/70 disabled:opacity-50"
                  >
                    <Flame className="h-3.5 w-3.5" />
                    <span>Melegítés indítása</span>
                  </button>
                ) : (
                  <button
                    type="button"
                    onClick={stopHeatHelper}
                    className="inline-flex min-h-[38px] items-center gap-1.5 rounded-md border border-slate-600 bg-slate-800 px-3 py-1.5 text-xs font-semibold text-slate-200"
                  >
                    <span>Leállítás ({heatSecondsLeft} mp)</span>
                  </button>
                )}
              </div>
            </div>
            {heatMessage && <p className="mt-2 text-xs text-slate-300">{heatMessage}</p>}
          </div>
        </div>
      )}
    </section>
  );
}
