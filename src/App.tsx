import { useMemo, useState } from 'react';
import { Check, Copy, Download, RotateCcw, Minus, Plus } from 'lucide-react';

import nanoPlatformio from '../nano/platformio.ini?raw';
import nanoCommonH from '../nano/src/common.h?raw';
import nanoMainCpp from '../nano/src/main.cpp?raw';
import espPlatformio from '../esp32/platformio.ini?raw';
import espConfigH from '../esp32/src/config.h?raw';
import espLcdH from '../esp32/src/lcd.h?raw';
import espMainCpp from '../esp32/src/main.cpp?raw';
import serverTs from '../server.ts?raw';
import serverTypesTs from '../server/types.ts?raw';
import serverValidationTs from '../server/validation.ts?raw';
import serverMqttBridgeTs from '../server/mqttBridge.ts?raw';
import serverHeatingRoutesTs from '../server/heatingRoutes.ts?raw';
import envExampleRaw from '../.env.example?raw';

type FileNodeFilter = 'all' | 'nano' | 'esp32' | 'backend';

interface FirmwareFile {
  id: string;
  path: string;
  node: 'nano' | 'esp32' | 'backend';
  tag: string;
  summary: string;
  content: string;
}

const FIRMWARE_FILES: FirmwareFile[] = [
  {
    id: 'backend-server',
    path: 'server.ts',
    node: 'backend',
    tag: 'NODE',
    summary: 'Express + Socket.IO szerver CORS (localhost:5173) támogatással és heating-update továbbítással',
    content: serverTs,
  },
  {
    id: 'backend-mqtt',
    path: 'server/mqttBridge.ts',
    node: 'backend',
    tag: 'MQTT',
    summary: 'HiveMQ Cloud kliens (backend-futes), futes/+/status & telemetry feliratkozás, ESP-elsőbbség',
    content: serverMqttBridgeTs,
  },
  {
    id: 'backend-routes',
    path: 'server/heatingRoutes.ts',
    node: 'backend',
    tag: 'REST',
    summary: 'POST /api/heating/:id/command, GET /api/heating/:id és POST /api/heating/:id/kimenet',
    content: serverHeatingRoutesTs,
  },
  {
    id: 'backend-validation',
    path: 'server/validation.ts',
    node: 'backend',
    tag: 'VALID',
    summary: 'targetTemp (10-30°C) és kimenetek (kizárólag boolean) szigorú validációja',
    content: serverValidationTs,
  },
  {
    id: 'backend-types',
    path: 'server/types.ts',
    node: 'backend',
    tag: 'TYPES',
    summary: 'Közös szerződés TypeScript interfészei (CMD, STATUS, HeatingUpdateEvent)',
    content: serverTypesTs,
  },
  {
    id: 'backend-env',
    path: '.env.example',
    node: 'backend',
    tag: 'ENV',
    summary: 'Környezeti változó sablon (HiveMQ Cloud TLS, CORS_ORIGIN, PORT)',
    content: envExampleRaw,
  },
  {
    id: 'nano-ini',
    path: 'nano/platformio.ini',
    node: 'nano',
    tag: 'AVR',
    summary: 'PlatformIO konfiguráció ATmega328P-hez, RF24 SoftSPI és OneWire függőségekkel',
    content: nanoPlatformio,
  },
  {
    id: 'nano-common',
    path: 'nano/src/common.h',
    node: 'nano',
    tag: 'AVR',
    summary: 'Közös 32 byte-os packed NrfPacket struktúra, bitmaszkok és RF24 csatorna',
    content: nanoCommonH,
  },
  {
    id: 'nano-main',
    path: 'nano/src/main.cpp',
    node: 'nano',
    tag: 'AVR',
    summary: 'Kazán firmware: DS18B20 address-kiíró, 10s nem-blokkoló olvasás, D5-D12 kimenetek, 15s fail-safe',
    content: nanoMainCpp,
  },
  {
    id: 'esp-ini',
    path: 'esp32/platformio.ini',
    node: 'esp32',
    tag: 'C3',
    summary: 'PlatformIO konfiguráció ESP32-C3-hoz (USB CDC, RF24, PubSubClient, ArduinoJson, LCD)',
    content: espPlatformio,
  },
  {
    id: 'esp-config',
    path: 'esp32/src/config.h',
    node: 'esp32',
    tag: 'C3',
    summary: 'WiFi, HiveMQ Cloud TLS 8883, GPIO pinek, 0x27 LCD cím, LM35_OFFSET és időzítők',
    content: espConfigH,
  },
  {
    id: 'esp-lcd',
    path: 'esp32/src/lcd.h',
    node: 'esp32',
    tag: 'C3',
    summary: '2x20 I2C LCD vezérlő villódzásmentes sorfrissítéssel és NRF HIBA kijelzéssel',
    content: espLcdH,
  },
  {
    id: 'esp-main',
    path: 'esp32/src/main.cpp',
    node: 'esp32',
    tag: 'C3',
    summary: 'LM35 átlagolás, Gray-kód encoder ISR, hiszterézis, 2s NRF ping, HiveMQ MQTT TLS gateway',
    content: espMainCpp,
  },
];

const PACKET_LAYOUT = [
  { offset: '0', size: 1, field: 'packetType', type: 'uint8_t', dir: 'Mindkettő', desc: '0xA1 (ESP->Nano) vagy 0xB2 (Nano->ESP)' },
  { offset: '1', size: 1, field: 'outputsMask', type: 'uint8_t', dir: 'ESP -> Nano', desc: 'bit0: szivattyu, bit1: led_zold, bit2: led_piros, bit3-7: tartalek1-5' },
  { offset: '2..5', size: 4, field: 'targetTemp', type: 'float', dir: 'ESP -> Nano', desc: 'Célhőmérséklet (°C, 16.0 - 26.0)' },
  { offset: '6', size: 1, field: 'mode', type: 'uint8_t', dir: 'ESP -> Nano', desc: '0 = auto, 1 = manual, 2 = off' },
  { offset: '7', size: 1, field: 'manualHeating', type: 'uint8_t', dir: 'ESP -> Nano', desc: 'Manuális fűtés parancs (0 vagy 1)' },
  { offset: '8..11', size: 4, field: 'eloremeno', type: 'float', dir: 'Nano -> ESP', desc: 'Előremenő vízhőfok (°C), NAN ha -127' },
  { offset: '12..15', size: 4, field: 'visszatero', type: 'float', dir: 'Nano -> ESP', desc: 'Visszatérő vízhőfok (°C), NAN ha -127' },
  { offset: '16..19', size: 4, field: 'kazan', type: 'float', dir: 'Nano -> ESP', desc: 'Kazán hőfok (°C), NAN ha -127' },
  { offset: '20..23', size: 4, field: 'kulteri', type: 'float', dir: 'Nano -> ESP', desc: 'Kültéri hőfok (°C), NAN ha -127' },
  { offset: '24', size: 1, field: 'kazan_status', type: 'uint8_t', dir: 'Nano -> ESP', desc: 'bit0-7 -> JSON kazan_status[0..7] tömb' },
  { offset: '25', size: 1, field: 'sensorValidMask', type: 'uint8_t', dir: 'Nano -> ESP', desc: 'bit0-3 érvényes szenzor jelző (-127 kiszűréshez)' },
  { offset: '26', size: 1, field: 'nrf24_ok', type: 'uint8_t', dir: 'Nano -> ESP', desc: '1 = aktív kapcsolat, 0 = fail-safe / hiba' },
  { offset: '27', size: 1, field: 'reserved', type: 'uint8_t', dir: 'Fenntartott', desc: '0x00 (32 byte igazítás)' },
  { offset: '28..31', size: 4, field: 'counter', type: 'uint32_t', dir: 'Nano -> ESP', desc: 'Növekvő válaszcsomag számláló' },
];

const NANO_PINS = [
  { pin: 'D2', signal: 'OneWire Bus', detail: '4x DS18B20 (4.7 kOhm pull-up 5V-ra)' },
  { pin: 'D3 / D4', signal: 'NRF24 CE / CSN', detail: 'Rádió chip enable és SPI chip select' },
  { pin: 'A0 / A1 / A2', signal: 'NRF24 SCK / MOSI / MISO', detail: 'SoftSPI (szabadon hagyja a D8-D12 lábakat)' },
  { pin: 'D5', signal: 'led_zold', detail: 'bit1 — fűtés / aktív állapotjelző LED' },
  { pin: 'D6', signal: 'led_piros', detail: 'bit2 — piros visszajelző LED' },
  { pin: 'D7', signal: 'szivattyu', detail: 'bit0 — keringető szivattyú relé' },
  { pin: 'D8..D12', signal: 'tartalek1..tartalek5', detail: 'bit3..7 — 5 db tartalék kimenet' },
];

const ESP_PINS = [
  { pin: 'GPIO 2', signal: 'LM35 ADC', detail: 'ADC1_CH2, 10 mérés átlaga (0-3300mV -> 0-100°C + offset)' },
  { pin: 'GPIO 5 / 6 / 7', signal: 'Encoder CLK / DT / SW', detail: 'Hardveres megszakítás + debounce' },
  { pin: 'GPIO 8 / 9', signal: 'I2C SDA / SCL', detail: '2x20 LCD (0x27 cím)' },
  { pin: 'GPIO 0 / 1', signal: 'NRF24 CE / CSN', detail: 'Rádió vezérlés' },
  { pin: 'GPIO 4 / 10 / 3', signal: 'NRF24 SCK / MOSI / MISO', detail: 'FSPI GPIO mátrixon keresztül' },
];

type ModeType = 'auto' | 'manual' | 'off';

export default function App() {
  const [nodeFilter, setNodeFilter] = useState<FileNodeFilter>('all');
  const [selectedFileId, setSelectedFileId] = useState<string>('backend-server');
  const [copiedId, setCopiedId] = useState<string | null>(null);

  // Szimulátor állapot (a firmware pontos hiszterézis és LCD logikáját modellezi)
  const [roomTemp, setRoomTemp] = useState<number>(20.8);
  const [targetTemp, setTargetTemp] = useState<number>(21.5);
  const [mode, setMode] = useState<ModeType>('auto');
  const [manualHeating, setManualHeating] = useState<boolean>(false);
  const [heatingAutoState, setHeatingAutoState] = useState<boolean>(true);
  const [nrfOk, setNrfOk] = useState<boolean>(true);
  const [boilerFault, setBoilerFault] = useState<boolean>(false);
  const [boilerTemp] = useState<number>(52.0);

  const visibleFiles = useMemo(
    () => (nodeFilter === 'all' ? FIRMWARE_FILES : FIRMWARE_FILES.filter((f) => f.node === nodeFilter)),
    [nodeFilter]
  );

  const activeFile = useMemo(
    () => FIRMWARE_FILES.find((f) => f.id === selectedFileId) ?? FIRMWARE_FILES[0],
    [selectedFileId]
  );

  // Firmware hiszterézis szimuláció
  const effectiveHeatingOn = useMemo(() => {
    if (mode === 'off') return false;
    if (mode === 'manual') return manualHeating;
    return heatingAutoState;
  }, [mode, manualHeating, heatingAutoState]);

  const handleRoomTempChange = (nextTemp: number) => {
    const rounded = Math.round(nextTemp * 10) / 10;
    setRoomTemp(rounded);
    if (rounded < targetTemp - 0.3) {
      setHeatingAutoState(true);
    } else if (rounded > targetTemp + 0.3) {
      setHeatingAutoState(false);
    }
  };

  const handleTargetStep = (delta: number) => {
    const next = Math.min(26.0, Math.max(16.0, Math.round((targetTemp + delta) * 2) / 2));
    setTargetTemp(next);
    if (roomTemp < next - 0.3) {
      setHeatingAutoState(true);
    } else if (roomTemp > next + 0.3) {
      setHeatingAutoState(false);
    }
  };

  const cycleMode = () => {
    setMode((prev) => (prev === 'auto' ? 'manual' : prev === 'manual' ? 'off' : 'auto'));
  };

  // Pontos 20 karakteres LCD sorok (megegyezik esp32/src/lcd.h formázásával)
  const lcdLines = useMemo(() => {
    const raw1 = `Szoba ${roomTemp.toFixed(1).padStart(4, ' ')}C Cel${targetTemp.toFixed(1).padStart(4, ' ')}C`;
    const line1 = raw1.slice(0, 20).padEnd(20, ' ');

    let raw2 = '';
    if (!nrfOk) {
      raw2 = 'NRF HIBA!';
    } else {
      const kazStr = boilerFault ? '--' : String(Math.round(boilerTemp));
      const modeChar = mode === 'auto' ? 'A' : mode === 'manual' ? 'M' : 'O';
      raw2 = `Kaz ${kazStr}C Futes:${effectiveHeatingOn ? 'BE' : 'KI'} ${modeChar}`;
    }
    const line2 = raw2.slice(0, 20).padEnd(20, ' ');
    return { line1, line2 };
  }, [roomTemp, targetTemp, nrfOk, boilerFault, boilerTemp, effectiveHeatingOn, mode]);

  const simulatedJson = useMemo(() => {
    return JSON.stringify(
      {
        currentTemp: roomTemp,
        targetTemp,
        heatingOn: effectiveHeatingOn,
        mode,
        online: true,
        homersekletek: {
          eloremeno: boilerFault ? null : 45.2,
          visszatero: 38.1,
          kazan: boilerFault ? null : boilerTemp,
          kulteri: 2.5,
        },
        kimenetek: {
          szivattyu: effectiveHeatingOn,
          led_zold: effectiveHeatingOn,
          led_piros: false,
          tartalek1: false,
          tartalek2: false,
          tartalek3: false,
          tartalek4: false,
          tartalek5: false,
        },
        kazan_status: [0, 0, 0, 0, 0, 0, 0, 0],
        nrf24_ok: nrfOk,
        error: nrfOk ? null : 'NRF24 kommunikacios hiba (10s timeout)',
      },
      null,
      2
    );
  }, [roomTemp, targetTemp, effectiveHeatingOn, mode, boilerFault, boilerTemp, nrfOk]);

  const handleCopy = async (text: string, id: string) => {
    await navigator.clipboard.writeText(text);
    setCopiedId(id);
    setTimeout(() => setCopiedId(null), 1600);
  };

  const handleDownloadFile = (file: FirmwareFile) => {
    const blob = new Blob([file.content], { type: 'text/plain;charset=utf-8' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = file.path.replace(/\//g, '_');
    a.click();
    URL.revokeObjectURL(url);
  };

  const lines = useMemo(() => activeFile.content.split('\n'), [activeFile.content]);

  return (
    <div className="min-h-screen bg-slate-50 text-slate-900">
      {/* 3-zónás fejléc */}
      <header className="sticky top-0 z-30 bg-white/95 backdrop-blur border-b border-slate-200">
        <div className="max-w-7xl mx-auto px-6 h-16 flex items-center justify-between gap-8">
          <a href="#forraskodok" className="text-base font-bold tracking-tight text-slate-900 whitespace-nowrap shrink-0">
            Fűtésvezérlés Firmware
          </a>

          <nav className="hidden md:flex items-center gap-6 text-sm font-medium text-slate-600">
            <a href="#forraskodok" className="hover:text-slate-900 transition-colors whitespace-nowrap shrink-0">
              Forráskódok
            </a>
            <a href="#szimulator" className="hover:text-slate-900 transition-colors whitespace-nowrap shrink-0">
              LCD Szimulátor
            </a>
            <a href="#csomag" className="hover:text-slate-900 transition-colors whitespace-nowrap shrink-0">
              NRF24 Csomag
            </a>
            <a href="#bekotes" className="hover:text-slate-900 transition-colors whitespace-nowrap shrink-0">
              Bekötés
            </a>
          </nav>

          <button
            type="button"
            onClick={() => handleDownloadFile(activeFile)}
            className="inline-flex items-center gap-2 px-4 py-2 text-xs font-semibold text-white bg-slate-900 rounded-lg hover:bg-slate-800 transition-colors whitespace-nowrap shrink-0 cursor-pointer"
          >
            <Download className="w-3.5 h-3.5" />
            Aktuális fájl mentése
          </button>
        </div>
      </header>

      <main className="max-w-7xl mx-auto px-6 py-10 space-y-14">
        {/* Bevezető sáv */}
        <section className="space-y-3">
          <div className="flex flex-wrap items-center gap-2 text-xs font-medium text-slate-500">
            <span>Arduino Nano (Kazán)</span>
            <span aria-hidden="true">·</span>
            <span>ESP32-C3 (Szoba / Gateway)</span>
            <span aria-hidden="true">·</span>
            <span>NRF24L01+ 32 byte</span>
            <span aria-hidden="true">·</span>
            <span>HiveMQ Cloud TLS 8883</span>
          </div>
          <h1 className="text-3xl font-bold tracking-tight text-slate-900 text-balance max-w-3xl">
            Kétcsomópontos fűtésvezérlő firmware és közös kommunikációs szerződés
          </h1>
          <p className="text-sm text-slate-600 max-w-3xl leading-relaxed">
            Mindkét PlatformIO projekt a munkakönyvtárban is megtalálható (<code className="font-mono text-slate-800">nano/</code> és{' '}
            <code className="font-mono text-slate-800">esp32/</code>). Az ESP32-C3 hozza a szabályozási döntéseket lokális hiszterézissel,
            WiFi-kiesés esetén is önállóan üzemel encoder + 2x20 LCD-vel, a Nano pedig 15 mp-es fail-safe védelemmel hajtja végre a kimeneti biteket.
          </p>
        </section>

        {/* 1. Forráskód böngésző */}
        <section id="forraskodok" className="scroll-mt-24 space-y-4">
          <div className="flex flex-wrap items-center justify-between gap-4">
            <h2 className="text-xl font-semibold text-slate-900">01. Firmware és Backend forrásfájlok ({FIRMWARE_FILES.length} fájl)</h2>
            <div className="flex items-center gap-1 p-1 bg-slate-200/70 rounded-lg">
              {([
                { id: 'all', label: 'Összes' },
                { id: 'backend', label: 'Backend' },
                { id: 'nano', label: 'Nano' },
                { id: 'esp32', label: 'ESP32-C3' },
              ] as const).map((tab) => (
                <button
                  key={tab.id}
                  type="button"
                  onClick={() => {
                    setNodeFilter(tab.id);
                    if (tab.id !== 'all') {
                      const first = FIRMWARE_FILES.find((f) => f.node === tab.id);
                      if (first) setSelectedFileId(first.id);
                    }
                  }}
                  className={`px-3 py-1 text-xs font-medium rounded-md transition-colors whitespace-nowrap shrink-0 cursor-pointer ${
                    nodeFilter === tab.id ? 'bg-white text-slate-900 shadow-sm' : 'text-slate-600 hover:text-slate-900'
                  }`}
                >
                  {tab.label}
                </button>
              ))}
            </div>
          </div>

          <div className="grid grid-cols-1 lg:grid-cols-12 gap-6 items-start">
            {/* Bal oldali fájllista */}
            <aside className="lg:col-span-4 bg-white border border-slate-200 rounded-xl overflow-hidden">
              <div className="px-4 py-3 border-b border-slate-200 bg-slate-50 flex items-center justify-between text-xs font-semibold text-slate-600">
                <span>Projekt struktúra</span>
                <span className="font-mono text-slate-400">{lines.length} sor</span>
              </div>
              <div className="divide-y divide-slate-100">
                {visibleFiles.map((file) => {
                  const isActive = file.id === activeFile.id;
                  return (
                    <button
                      key={file.id}
                      type="button"
                      onClick={() => setSelectedFileId(file.id)}
                      className={`w-full text-left px-4 py-3 transition-colors cursor-pointer ${
                        isActive ? 'bg-orange-50/70' : 'hover:bg-slate-50'
                      }`}
                    >
                      <div className="flex items-center justify-between gap-2">
                        <span className={`font-mono text-xs font-semibold ${isActive ? 'text-orange-700' : 'text-slate-800'}`}>
                          {file.path}
                        </span>
                        <span className="text-[11px] font-medium text-slate-400 shrink-0">
                          {file.tag}
                        </span>
                      </div>
                      <p className="text-xs text-slate-500 mt-1 leading-snug">{file.summary}</p>
                    </button>
                  );
                })}
              </div>
            </aside>

            {/* Jobb oldali kódnéző */}
            <div className="lg:col-span-8 bg-slate-900 text-slate-100 border border-slate-800 rounded-xl overflow-hidden">
              <div className="px-4 py-3 bg-slate-950 border-b border-slate-800 flex items-center justify-between gap-4">
                <div className="min-w-0">
                  <div className="font-mono text-xs font-semibold text-slate-100 truncate">{activeFile.path}</div>
                  <div className="text-xs text-slate-400 truncate mt-0.5">{activeFile.summary}</div>
                </div>
                <div className="flex items-center gap-2 shrink-0">
                  <button
                    type="button"
                    onClick={() => handleCopy(activeFile.content, activeFile.id)}
                    className="inline-flex items-center gap-1.5 px-3 py-1.5 text-xs font-medium bg-slate-800 hover:bg-slate-700 text-slate-100 rounded-md transition-colors whitespace-nowrap cursor-pointer"
                  >
                    {copiedId === activeFile.id ? <Check className="w-3.5 h-3.5 text-emerald-400" /> : <Copy className="w-3.5 h-3.5" />}
                    {copiedId === activeFile.id ? 'Másolva' : 'Másolás'}
                  </button>
                </div>
              </div>

              <div className="max-h-[560px] overflow-auto p-4 text-xs font-mono leading-relaxed">
                <table className="w-full border-collapse">
                  <tbody>
                    {lines.map((line, i) => (
                      <tr key={i} className="hover:bg-slate-800/40">
                        <td className="select-none pr-4 text-right text-slate-500 w-10 align-top">{i + 1}</td>
                        <td className="whitespace-pre text-slate-200">{line || ' '}</td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
            </div>
          </div>
        </section>

        {/* 2. Interaktív 2x20 LCD & Hiszterézis Szimulátor */}
        <section id="szimulator" className="scroll-mt-24 space-y-4">
          <div className="flex flex-wrap items-baseline justify-between gap-4">
            <h2 className="text-xl font-semibold text-slate-900">02. Szabályozási logika és 2x20 LCD szimulátor</h2>
            <span className="text-xs text-slate-500">
              Hiszterézis: BE &lt; cél - 0.3°C · KI &gt; cél + 0.3°C
            </span>
          </div>

          <div className="grid grid-cols-1 lg:grid-cols-12 gap-6 items-stretch">
            {/* Bal oldal: Vezérlők + 2x20 LCD */}
            <div className="lg:col-span-6 bg-white border border-slate-200 rounded-xl p-6 flex flex-col justify-between gap-6">
              {/* 2x20 karakteres LCD modul */}
              <div className="bg-slate-900 rounded-lg p-4 border border-slate-800">
                <div className="flex items-center justify-between text-[11px] text-slate-400 mb-2">
                  <span>2x20 I2C LCD (0x27)</span>
                  <span className="font-mono">20 oszlop × 2 sor</span>
                </div>
                <div className="bg-emerald-950 border border-emerald-800/70 rounded px-4 py-3 font-mono text-lg sm:text-xl tracking-wider text-emerald-300 leading-snug select-all overflow-x-auto">
                  <div className="whitespace-pre">{lcdLines.line1}</div>
                  <div className="whitespace-pre">{lcdLines.line2}</div>
                </div>
              </div>

              {/* Szoba hőmérséklet és Encoder vezérlők */}
              <div className="space-y-5">
                <div>
                  <div className="flex items-center justify-between text-xs font-medium text-slate-700 mb-1.5">
                    <label htmlFor="room-temp-slider">LM35 szobai hőmérséklet (GPIO 2)</label>
                    <span className="font-mono font-semibold text-slate-900">{roomTemp.toFixed(1)} °C</span>
                  </div>
                  <input
                    id="room-temp-slider"
                    type="range"
                    min={15}
                    max={27}
                    step={0.1}
                    value={roomTemp}
                    onChange={(e) => handleRoomTempChange(parseFloat(e.target.value))}
                    className="w-full accent-orange-600 cursor-pointer"
                  />
                </div>

                <div className="grid grid-cols-1 sm:grid-cols-2 gap-4">
                  <div className="border border-slate-200 rounded-lg p-3.5 space-y-2">
                    <div className="text-xs font-medium text-slate-600">Rotary Encoder (célhőfok)</div>
                    <div className="flex items-center justify-between gap-2">
                      <button
                        type="button"
                        onClick={() => handleTargetStep(-0.5)}
                        className="p-2 rounded-md border border-slate-200 hover:bg-slate-100 text-slate-700 cursor-pointer"
                        aria-label="Célhőmérséklet csökkentése 0.5 fokkal"
                      >
                        <Minus className="w-4 h-4" />
                      </button>
                      <span className="font-mono text-lg font-semibold text-slate-900">{targetTemp.toFixed(1)} °C</span>
                      <button
                        type="button"
                        onClick={() => handleTargetStep(0.5)}
                        className="p-2 rounded-md border border-slate-200 hover:bg-slate-100 text-slate-700 cursor-pointer"
                        aria-label="Célhőmérséklet növelése 0.5 fokkal"
                      >
                        <Plus className="w-4 h-4" />
                      </button>
                    </div>
                  </div>

                  <div className="border border-slate-200 rounded-lg p-3.5 space-y-2">
                    <div className="text-xs font-medium text-slate-600">Encoder gomb (GPIO 7)</div>
                    <button
                      type="button"
                      onClick={cycleMode}
                      className="w-full py-2 px-3 rounded-md bg-slate-900 hover:bg-slate-800 text-white text-xs font-semibold flex items-center justify-center gap-2 transition-colors cursor-pointer"
                    >
                      <RotateCcw className="w-3.5 h-3.5" />
                      Mód: {mode.toUpperCase()}
                    </button>
                  </div>
                </div>

                <div className="flex flex-wrap items-center gap-4 pt-1 text-xs">
                  {mode === 'manual' && (
                    <label className="inline-flex items-center gap-2 cursor-pointer text-slate-700 font-medium">
                      <input
                        type="checkbox"
                        checked={manualHeating}
                        onChange={(e) => setManualHeating(e.target.checked)}
                        className="accent-orange-600 w-4 h-4"
                      />
                      manualHeating BE
                    </label>
                  )}

                  <label className="inline-flex items-center gap-2 cursor-pointer text-slate-700 font-medium">
                    <input
                      type="checkbox"
                      checked={!nrfOk}
                      onChange={(e) => setNrfOk(!e.target.checked)}
                      className="accent-red-600 w-4 h-4"
                    />
                    NRF24 10mp timeout szimulálása
                  </label>

                  <label className="inline-flex items-center gap-2 cursor-pointer text-slate-700 font-medium">
                    <input
                      type="checkbox"
                      checked={boilerFault}
                      onChange={(e) => setBoilerFault(e.target.checked)}
                      className="accent-amber-600 w-4 h-4"
                    />
                    DS18B20 -127°C (null) hiba
                  </label>
                </div>
              </div>
            </div>

            {/* Jobb oldal: Élő MQTT JSON Status kimenet */}
            <div className="lg:col-span-6 bg-slate-900 text-slate-100 border border-slate-800 rounded-xl overflow-hidden flex flex-col">
              <div className="px-4 py-3 bg-slate-950 border-b border-slate-800 flex items-center justify-between gap-4">
                <div className="text-xs font-mono text-slate-300">futes/haz-futes/status (retained JSON)</div>
                <button
                  type="button"
                  onClick={() => handleCopy(simulatedJson, 'json-sim')}
                  className="inline-flex items-center gap-1.5 px-2.5 py-1 text-xs font-medium bg-slate-800 hover:bg-slate-700 text-slate-100 rounded transition-colors cursor-pointer"
                >
                  {copiedId === 'json-sim' ? <Check className="w-3.5 h-3.5 text-emerald-400" /> : <Copy className="w-3.5 h-3.5" />}
                  {copiedId === 'json-sim' ? 'Másolva' : 'JSON másolása'}
                </button>
              </div>
              <pre className="p-4 text-xs font-mono text-slate-200 overflow-auto flex-1 leading-relaxed">{simulatedJson}</pre>
            </div>
          </div>
        </section>

        {/* 3. NRF24 32-byte Csomag térkép */}
        <section id="csomag" className="scroll-mt-24 space-y-4">
          <div className="flex flex-wrap items-baseline justify-between gap-4">
            <h2 className="text-xl font-semibold text-slate-900">03. NRF24L01+ 32-byte közös csomagszerkezet</h2>
            <span className="text-xs font-mono text-slate-500">sizeof(NrfPacket) == 32 byte · __attribute__((packed))</span>
          </div>

          <div className="bg-white border border-slate-200 rounded-xl overflow-x-auto">
            <table className="w-full text-left border-collapse text-xs">
              <thead>
                <tr className="border-b border-slate-200 bg-slate-50 text-slate-600">
                  <th className="py-3 px-4 font-semibold">Offset</th>
                  <th className="py-3 px-4 font-semibold">Méret</th>
                  <th className="py-3 px-4 font-semibold">Mező</th>
                  <th className="py-3 px-4 font-semibold">Típus</th>
                  <th className="py-3 px-4 font-semibold">Irány</th>
                  <th className="py-3 px-4 font-semibold">Leírás</th>
                </tr>
              </thead>
              <tbody className="divide-y divide-slate-100">
                {PACKET_LAYOUT.map((row) => (
                  <tr key={row.field} className="hover:bg-slate-50/70">
                    <td className="py-2.5 px-4 font-mono text-slate-600">{row.offset}</td>
                    <td className="py-2.5 px-4 font-mono text-slate-600">{row.size} B</td>
                    <td className="py-2.5 px-4 font-mono font-semibold text-slate-900">{row.field}</td>
                    <td className="py-2.5 px-4 font-mono text-slate-600">{row.type}</td>
                    <td className="py-2.5 px-4 text-slate-700 whitespace-nowrap">{row.dir}</td>
                    <td className="py-2.5 px-4 text-slate-600">{row.desc}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </section>

        {/* 4. Hardver bekötési táblázatok */}
        <section id="bekotes" className="scroll-mt-24 space-y-4">
          <h2 className="text-xl font-semibold text-slate-900">04. Hardver pin-kiosztás és bekötés</h2>

          <div className="grid grid-cols-1 lg:grid-cols-2 gap-6">
            <div className="bg-white border border-slate-200 rounded-xl overflow-hidden">
              <div className="px-4 py-3 border-b border-slate-200 bg-slate-50 flex items-center justify-between">
                <span className="text-sm font-semibold text-slate-900">Arduino Nano (Kazán oldal)</span>
                <span className="text-xs font-mono text-slate-500">SoftSPI: A0/A1/A2</span>
              </div>
              <table className="w-full text-left border-collapse text-xs">
                <tbody className="divide-y divide-slate-100">
                  {NANO_PINS.map((p) => (
                    <tr key={p.pin}>
                      <td className="py-2.5 px-4 font-mono font-semibold text-slate-900 whitespace-nowrap">{p.pin}</td>
                      <td className="py-2.5 px-4 font-mono text-slate-700 whitespace-nowrap">{p.signal}</td>
                      <td className="py-2.5 px-4 text-slate-600">{p.detail}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>

            <div className="bg-white border border-slate-200 rounded-xl overflow-hidden">
              <div className="px-4 py-3 border-b border-slate-200 bg-slate-50 flex items-center justify-between">
                <span className="text-sm font-semibold text-slate-900">ESP32-C3 (Szoba / Gateway)</span>
                <span className="text-xs font-mono text-slate-500">ADC1 + FSPI + I2C</span>
              </div>
              <table className="w-full text-left border-collapse text-xs">
                <tbody className="divide-y divide-slate-100">
                  {ESP_PINS.map((p) => (
                    <tr key={p.pin}>
                      <td className="py-2.5 px-4 font-mono font-semibold text-slate-900 whitespace-nowrap">{p.pin}</td>
                      <td className="py-2.5 px-4 font-mono text-slate-700 whitespace-nowrap">{p.signal}</td>
                      <td className="py-2.5 px-4 text-slate-600">{p.detail}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          </div>
        </section>
      </main>
    </div>
  );
}
