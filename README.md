# Fűtésvezérlő és Kazánvédelmi Rendszer (V5)

Két egységes, **Master–Slave** felépítésű okos fűtésvezérlő és szilárdtüzelésű kazánvédelmi rendszer **Arduino Nano (kazán oldal)**, **ESP32-C3 (szoba oldal / I2C LCD + rotary encoder + WiFi/MQTT gateway)**, **Node.js/Express + Socket.IO backend** és **React frontend** komponensekkel, **HiveMQ Cloud TLS 8883** kapcsolaton keresztül.

### Változásnapló

| Dátum | Változás |
| :--- | :--- |
| V5.0 | V5 Master–Slave életjel + heartbeat protokoll (32 byte NRF csomag, `5–30s` ping, `3 * ping + 2 mp` Nano fail-safe) |
| V5.1 | ESP32-C3 2×20 I2C LCD menürendszer (interrupt rotary encoder, `20s` timeout, NVS mentés, `0..5` állapot ikonok) |
| V5.2 | WiFi beállító portál (`Futes-ESP32-Setup`, `192.168.4.1`) eszközönként egyedi WPA2 jelszóval és NVS tárolással |
| V5.3 | Mentés és kommunikáció ellenőrzése (NVS `Preferences`, NRF `radio.write()`, MQTT `publish()` visszatérési értékek + hibajelzés) |
| V5.4 | CA TLS ellenőrzés (`esp32/src/hivemq_ca.h`, `setCACert`) + Backend `127.0.0.1` bind + `HEATING_API_KEY` Bearer auth + szigorú `FRONTEND_ORIGIN` CORS |

---

## 1. Rendszer áttekintés

```text
+------------------+       HTTP / Socket.IO       +----------------------------------+
|  Frontend React  | <--------------------------> | Backend Node/Express + MQTT      |
|  (Webes felület) |                              | Bridge (V4/V5 API & Mutex)       |
+------------------+                              +----------------------------------+
                                                                   ^
                                                                   | MQTT over TLS (8883)
                                                                   v
+------------------+        NRF24L01+ (32B)       +----------------------------------+
| Arduino Nano     | <--------------------------> | HiveMQ Cloud (TLS 8883 Broker)   |
| SLAVE / Kazán    |      0xA1 REQ / 0xB2 RSP     +----------------------------------+
| (DS18B20 + Relé) |                                               ^
+------------------+                                               | WiFi + TLS 8883
        ^                                                          v
        |          NRF24L01+ (250 kbps, Ch 108)   +----------------------------------+
        +---------------------------------------> | ESP32-C3 MASTER / Gateway        |
                                                  | (LM35 + 2x20 LCD + Encoder + NVS)|
                                                  +----------------------------------+
```

- **Frontend React (`src/`)**: Böngészőből elérhető valós idejű vezérlőpult, amely Socket.IO-n kapja a frissítéseket, és REST API-n küldi a célhőmérséklet-, kimenet-, szivattyú-, riasztás- és életjel-beállításokat.
- **Backend Node/Express + MQTT Bridge (`server.ts`, `server/`)**: Validálja a webes kéréseket (`pumpConfig`, `alarmConfig`, `heartbeat`), eszközszintű mutexszel és frissességi logikával (`resolveFreshBase`) fésüli össze az állapotot, majd továbbítja az MQTT broker felé.
- **HiveMQ Cloud TLS 8883**: Felhőalapú MQTT broker titkosított TLS 8883-as porton, amely összeköti a backendet a szobai ESP32-C3 gateway-jel.
- **ESP32-C3 MASTER / Gateway (`esp32/`)**: A rendszer központi vezérlője: méri a szobahőmérsékletet (LM35), kezeli a 2×20 I2C LCD menüt és rotary encodert, NVS `Preferences`-be menti a beállításokat, és a beállított ping intervallummal kérdezi le a kazánt NRF24-en.
- **NRF24 (2.4 GHz rádió, `FUTE1` / `FUTE2`)**: Fél-duplex, szinkron Master–Slave rádiós kapcsolat 250 kbps sebességgel és fix 32 byte-os bináris csomagokkal (`AckPayload` nélkül).
- **Arduino Nano SLAVE / Kazán (`nano/`)**: Passzív kazánoldali végrehajtó és szenzorolvasó: 4 db DS18B20 érzékelőt mér, vezérli a szivattyú relét és a kimeneteket, és csak a Master kéréseire válaszol, de kapcsolatvesztéskor önálló fail-safe módba lép.

---

## 2. Hardver Wiring táblázatok

### 2.1 Arduino Nano (kazánnál, SLAVE)

A 4 db **DS18B20** hőmérséklet-szenzor közös **OneWire** buszon van a **D2** lábon (4,7 kΩ felhúzó ellenállással 5V felé). Mindegyik szenzort egyedi 8 byte-os `DeviceAddress` alapján azonosítjuk, **nem index alapján**. Első indításkor a Serial Monitoron (115200 baud) az **`a`** karakter elküldésére a Nano kilistázza a buszon talált címeket — ezeket kell bemásolni a `nano/src/main.cpp` fájl `// TODO: CSERÉLD LE VALÓS CÍMRE!` soraira (`ADDR_ELOREMENO`, `ADDR_VISSZATERO`, `ADDR_KAZAN`, `ADDR_KULTERI`).

| Komponens / Jel | Arduino Nano láb | Irány / Típus | Megjegyzés |
| :--- | :--- | :--- | :--- |
| **4× DS18B20 OneWire busz** | `D2` | Digitális (OneWire) | 4,7 kΩ pull-up 5V-ra; cím szerinti olvasás (`'a'` parancs a Serial Monitoron) |
| **NRF24L01+ CE** | `D3` | Kimenet | Rádió modul engedélyező |
| **NRF24L01+ CSN** | `D4` | Kimenet | SPI Chip Select |
| **NRF24L01+ MOSI** | `D11` | Hardveres SPI | Master Out Slave In |
| **NRF24L01+ MISO** | `D12` | Hardveres SPI | Master In Slave Out |
| **NRF24L01+ SCK** | `D13` | Hardveres SPI | SPI órajel |
| **Zöld LED (`led_zold`)** | `D5` | Kimenet | Állapot / felhasználói kimenet |
| **Piros LED (`led_piros`)** | `D6` | Kimenet | Riasztás jelzés (aktív riasztáskor 500 ms BE / 500 ms KI villogás) |
| **Szivattyú relé (`szivattyu`)** | `D7` | Kimenet | Keringető szivattyú relévezérlés |
| **Tartalék 1 (`tartalek1`)** | `D8` | Kimenet | Szabadon kapcsolható kimenet |
| **Tartalék 2 (`tartalek2`)** | `D9` | Kimenet | Szabadon kapcsolható kimenet |
| **Tartalék 3 (`tartalek3`)** | `D10` | Kimenet | **OUTPUT módban kell maradnia** az AVR hardveres SPI Master feltétel miatt |
| **Tartalék 4 (`tartalek4`)** | `A3` (`17`) | Kimenet | Analóg láb digitális kimenetként |
| **Tartalék 5 (`tartalek5`)** | `A4` (`18`) | Kimenet | Analóg láb digitális kimenetként |

> **Figyelem:** Az NRF24L01+ modul **3,3 V** tápfeszültséget igényel (a Nano 3.3V kimenete gyenge lehet, célszerű külső 3.3V stabilizátor és 10–100 µF kondenzátor a modul tápján).

### 2.2 ESP32-C3 (szobában, MASTER / Gateway)

| Komponens / Jel | ESP32-C3 GPIO | Irány / Típus | Megjegyzés |
| :--- | :--- | :--- | :--- |
| **LM35 szobai hőfok** | `GPIO 2` | Analóg bemenet (ADC, 12 bit, `ADC_11db`, 3300 mV) | LM35 **10 mV / °C**, 10 mérés átlaga: `avgMv = (adcRaw / 4095.0) * 3300.0`, `tempC = mV / 10 + OFFSET` (`LM35_OFFSET` a `esp32/src/config.h`-ban; `-10..60 °C` tartományon kívül `LM35_OUT_OF_RANGE` hiba) |
| **Rotary Encoder CLK** | `GPIO 5` | Bemenet (`INPUT_PULLUP`) | Interrupt-vezérelt Gray-kód olvasás |
| **Rotary Encoder DT** | `GPIO 6` | Bemenet (`INPUT_PULLUP`) | Interrupt-vezérelt Gray-kód olvasás |
| **Rotary Encoder SW** | `GPIO 7` | Bemenet (`INPUT_PULLUP`) | Nyomógomb (LOW-ra húz), szoftveres debounce |
| **2×20 I2C LCD SDA** | `GPIO 8` | I2C adat (`0x27`) | `Wire.begin(8, 9)` használatával (`Wire.setPins()` TILOS) |
| **2×20 I2C LCD SCL** | `GPIO 9` | I2C órajel (`0x27`) | `Wire.begin(8, 9)` használatával |
| **Piros riasztás LED** | `GPIO 11` | Kimenet | Riasztáskor 500 ms BE / 500 ms KI villogás `millis()`-el |
| **NRF24L01+ CSN** | `GPIO 1` | Kimenet (FSPI) | SPI Chip Select |
| **NRF24L01+ MISO** | `GPIO 3` | SPI (FSPI) | Master In Slave Out |
| **NRF24L01+ SCK** | `GPIO 4` | SPI (FSPI) | SPI órajel |
| **NRF24L01+ MOSI** | `GPIO 10` | SPI (FSPI) | Master Out Slave In |
| **NRF24L01+ CE** | `GPIO 20` | Kimenet | Rádió modul engedélyező (`GPIO 18/19` az USB CDC miatt szabadon hagyva) |

---

## 3. V5 Közös Szerződés (NRF24 + MQTT)

### NRF24 bináris csomag (`nano/src/common.h`)

Mindkét mikrovezérlő ugyanazt a `common.h` fejlécet használja `__attribute__((packed))` struktúrákkal. A csomag mérete fordítási időben ellenőrzött: **`static_assert(sizeof(NrfPacket) == 32)`**.

- **Rádió csatorna:** `108` (`250 kbps`, `AckPayload` kikapcsolva)
- **Csőcímek:** `NRF_ADDR_MASTER_TO_SLAVE = "FUTE1"`, `NRF_ADDR_SLAVE_TO_MASTER = "FUTE2"`
- **`packetType` mező (1 byte):**
  - `0xA1` (`PKT_TYPE_REQUEST`): **Master → Slave** (ESP32-C3 → Arduino Nano)
  - `0xB2` (`PKT_TYPE_RESPONSE`): **Slave → Master** (Arduino Nano → ESP32-C3)

| Struktúra (31 byte payload) | Mezők pontos sorrendben | Leírás |
| :--- | :--- | :--- |
| **`EspToNanoPayload`** | `outputsMask`, `targetTemp`, `mode`, `manualHeating`, `pumpConfig`, `alarmConfig`, `heartbeatIntervalSec`, `seq`, `timeSyncEpoch` | Az ESP32 kimeneti bitmaszkja, termosztát, szivattyú-, riasztás- és ping beállításai, a növekvő `seq` sorszám, valamint a 4 byte-os `timeSyncEpoch` időszinkron érték. |
| **`NanoToEspPayload`** | `eloremeno`, `visszatero`, `kazan`, `kulteri`, `kazanStatusMask`, `counter`, `seq`, `sensorValidMask`, `alarmState`, `syncFlags`, `syncEpochLow16` | A 4 mért hőfok (`-127.0f` hiba esetén), az 1 byte-os `kazanStatusMask` (amelyet az ESP32 bont vissza 8 elemű `kazan_status` tömbbé), a válasz `counter`, a szenzor-érvényességi maszk, az `alarmState`, a `syncFlags` (`ACK`/`STALE`) + `syncEpochLow16`, és az eredeti `seq`. |

### MQTT topicok (HiveMQ Cloud TLS 8883)

| Topic | Irány | Beállítás | Szerep |
| :--- | :--- | :--- | :--- |
| `futes/haz-futes/cmd` | Backend → ESP32 | `retained=false` | Parancsok és konfigurációs frissítések küldése az ESP32-nek |
| `futes/haz-futes/status` | ESP32 → Backend | **`retained=true`** | Azonnali állapotjelentés változáskor és csatlakozáskor |
| `futes/haz-futes/telemetry` | ESP32 → Backend | **30 mp-enként** | Időszakos telemetria (`nanoCounter` értékkel kiegészítve) |

### Teljes JSON `CMD` példa (`futes/haz-futes/cmd`)

```json
{
  "targetTemp": 21.5,
  "mode": "auto",
  "manualHeating": false,
  "kimenetek": {
    "szivattyu": false,
    "led_zold": false,
    "led_piros": false,
    "tartalek1": false,
    "tartalek2": false,
    "tartalek3": false,
    "tartalek4": false,
    "tartalek5": false
  },
  "pumpConfig": {
    "pumpMode": "auto",
    "pumpStartTemp": 40.0,
    "pumpStopTemp": 30.0,
    "pumpStartSensor": "kazan"
  },
  "alarmConfig": {
    "alarmTemp": 85.0,
    "alarmEnabled": true
  },
  "heartbeat": {
    "intervalSec": 5
  }
}
```

### Teljes JSON `STATUS` példa (`futes/haz-futes/status`)

```json
{
  "currentTemp": 20.8,
  "targetTemp": 21.5,
  "heatingOn": true,
  "mode": "auto",
  "online": true,
  "homersekletek": {
    "eloremeno": 45.2,
    "visszatero": 38.4,
    "kazan": 54.0,
    "kulteri": 2.5
  },
  "kimenetek": {
    "szivattyu": true,
    "led_zold": false,
    "led_piros": false,
    "tartalek1": false,
    "tartalek2": false,
    "tartalek3": false,
    "tartalek4": false,
    "tartalek5": false
  },
  "kazan_status": [0, 0, 0, 0, 0, 0, 0, 0],
  "pumpConfig": {
    "pumpMode": "auto",
    "pumpStartTemp": 40.0,
    "pumpStopTemp": 30.0,
    "pumpStartSensor": "kazan"
  },
  "pumpState": "futes",
  "alarmConfig": {
    "alarmTemp": 85.0,
    "alarmEnabled": true
  },
  "safety": {
    "safetyOverheatTemp": 95.0
  },
  "alarmState": {
    "active": false,
    "reason": "none"
  },
  "heartbeat": {
    "intervalSec": 5
  },
  "nrf24_ok": true,
  "time": {
    "espNtpSynced": true,
    "espTime": "2026-11-17 12:00:00",
    "espUptimeSec": 12345
  },
  "sync": {
    "ntpSynced": true,
    "espTime": "2026-11-17 12:00:00",
    "nanoSynced": true,
    "nanoTime": "2026-11-17 12:00:02",
    "nanoStale": false,
    "lastSync": "2026-11-17 12:00:00",
    "nextSync": "2026-11-18 00:00:00",
    "syncSuccess": true,
    "millisOverflowInDays": 42,
    "pingAgeSec": 3
  },
  "trend": {
    "eloremeno": "up",
    "delta": 1.2,
    "max": 58.4,
    "min": 22.1,
    "avg": 41.3,
    "maxAt": "14:32",
    "minAt": "06:15",
    "since": "2026-11-17 08:12:00 óta"
  },
  "error": null
}
```

---

## 4. Szivattyú védelem (Kazánvédelem állapotgép)

### Paraméterek (`pumpConfig`)

| Paraméter | Értékek / Tartomány | Alapérték | Leírás |
| :--- | :--- | :--- | :--- |
| `pumpMode` | `auto` / `manual_on` / `manual_off` | `auto` | Szivattyú üzemmód |
| `pumpStartTemp` | `30.0` – `50.0 °C` | `40.0 °C` | Indítási küszöbérték |
| `pumpStopTemp` | `20.0` – `40.0 °C` | `30.0 °C` | Leállítási küszöbérték (**MINDIG `Start - Stop >= 5 °C`**) |
| `pumpStartSensor` | `kazan` / `eloremeno` / `max` | `kazan` | Indításnál figyelt hőmérő (`max` = kettő közül a magasabb) |

### Állapotgép működése (`pumpState`)

- **`varakozas` → `futes`**: ha `pumpMode == "auto"` és a `pumpStartSensor` által kiválasztott szenzor hőmérséklete **`>= pumpStartTemp`**, a szivattyú **BEkapcsol**.
- **`futes` → `varakozas`**: hűlésnél **CSAK az `eloremeno <= pumpStopTemp`** feltételt figyeljük (mert nincsenek zárt radiátorok / termofejek, így az előremenő lehűlése biztosan jelzi a tűz kialvását).
- **`manual_off` időkorlát**: `manual_off` módból **10 perc (`600 000 ms`)** elteltével a rendszer automatikusan visszaáll `auto` módba.

### Biztonsági felülírások (MINDENT felülírnak, még a `manual_off`-ot is!)

1. **Túlmelegedés (`kazan >= 95.0 °C`, fix `SAFETY_OVERHEAT_TEMP`)**: a szivattyú kényszerítve **BEkapcsol**. A szivattyú védelem **SOHA nem a felhasználói `alarmTemp` értéket**, hanem kizárólag a nem állítható `95.0 °C`-os biztonsági határt használja (akkor is aktív marad, ha `alarmEnabled == false`).
2. **Kazán szenzor hiba (`kazan == -127` / érvénytelen)**: a szivattyú kényszerítve **BEkapcsol**.
3. **NRF24 rádiós szakadás**: az Arduino Nano fail-safe módba lép és **BEkapcsolja** a szivattyút.

---

## 5. Riasztás

- **`alarmConfig`** (felhasználói riasztási küszöb — nem vezérli a szivattyút):
  - `alarmTemp`: `70.0 – 90.0 °C` (alapérték: **`85.0 °C`**)
  - `alarmEnabled`: `true` / `false` (alapérték: **`true`**)
- **`safety`** (csak olvasható, nem állítható, nem mentődik NVS-be):
  - `safetyOverheatTemp`: **`95.0 °C`** (`SAFETY_OVERHEAT_TEMP` fix konstans mindkét mikrokontrolleren)
- **`alarmState.reason`**:
  - `"none"` – nincs riasztás
  - `"overheat"` – a kazán elérte vagy meghaladta az `alarmTemp` értéket
  - `"sensor_fail"` – a kazán DS18B20 szenzor nem olvasható (`-127 °C`)
  - `"nrf_fail"` – az ESP32 és Nano közötti rádiós kapcsolat megszakadt
- **LED jelzés**:
  - Ha `alarmState.active == true` **ÉS** `alarmEnabled == true`, akkor **MINDKÉT piros LED** (Nano `D6` és ESP32 `GPIO 11`) szinkron nélkül, nem-blokkoló `millis()` időzítéssel villog **`500 ms BE / 500 ms KI`** ütemben (`delay()` használata nélkül).
  - Ha `alarmEnabled == false`, akkor **nincs piros LED villogás** és az LCD sem villogtat riasztási sort, **DE a szivattyú `95.0 °C`-os (`SAFETY_OVERHEAT_TEMP`) és szenzorhibás (`-127`) biztonsági védelme továbbra is kényszerítve BE marad!**
- **LCD jelzés**:
  - Aktív és engedélyezett riasztás esetén a főképernyő 2. sora 500 ms-enként váltakozik a normál `Sz:20.8C C:21.5C` sor és a riasztási üzenet között (pl. `RIASZTAS! Kaz 87C`, `RIASZTAS! SZENZOR!`, `RIASZTAS! NRF HIBA!`, vagy angol megfelelője `ALARM! Kaz 87C`).

---

## 6. V5 Master–Slave Életjel (Rádiós protokoll)

- **Szigorú Master–Slave irányítás**: Adást **MINDIG a Master (ESP32-C3)** indíthat. A **Slave (Arduino Nano) SOHA nem ad önállóan**, kizárólag a Master kérésére válaszol.
- **`AckPayload` TILOS**: Mindkét oldalon `radio.disableAckPayload()` van beállítva; a kommunikáció sima `write()` hívásokkal zajlik külön `REQUEST (0xA1)` és `RESPONSE (0xB2)` csomagokkal.
- **Ping intervallum (`heartbeat.intervalSec`)**: `5, 10, 15, 20, 25, 30` másodperc (5 mp-es lépésekben, alapérték: **`5 mp`**).
- **Szinkron és duplikáció-védelem**:
  - Minden pingnél a Master növeli a `seq` sorszámot és elküldi a `REQUEST (0xA1)` csomagot.
  - A Slave ugyanazzal a `seq` számmal válaszol `RESPONSE (0xB2)` csomagban.
  - Ha az újrapróbálkozás miatt ugyanaz a `seq` érkezik meg még egyszer a Slave-hez, a Nano **nem hajtja végre újra** a parancsot, hanem a tárolt választ küldi vissza.
- **Időtúrák és fail-safe**:
  - **Master oldalon**: válaszonként max. **`500 ms`** várakozás, kérésenként legfeljebb **`3` próba**. Ha **`3` egymást követő ping ciklus** sikertelen, a Master `nrf24_ok = false` és `alarmState.reason = "nrf_fail"` állapotba vált. Ha egy kérésnél mindhárom `radio.write()` hívás `false` értékkel tér vissza, az ESP32 azonnal `nrf24_ok = false` és `alarmState.reason = "nrf_fail"` állapotba kapcsol, az LCD-n megjelenik a **`"NRF HIBA!"`** üzenet, és a soros porton naplózza a hibát (`NRF WRITE FAILED seq=<seq>`).
  - **Slave oldalon (Nano fail-safe)**: ha `(millis() - utolsó kérés ideje) > (3 * heartbeatIntervalSec * 1000 + 2000)`, akkor a Nano fail-safe módba lép: **szivattyú BE**, **piros LED villog**, `alarmState.reason = "nrf_fail"`.
- **DS18B20 mérés**: A Nano **10 másodpercenként** mér nem-blokkoló módon (`TEMP_READ_INTERVAL_MS = 10000`), nem minden egyes rádió pingnél, így mindig azonnal (<200 ms-on belül) tud válaszolni a Master kérésére.

| Beállított ping (`heartbeatIntervalSec`) | Slave fail-safe képlet (`3 * ping + 2 mp`) | Fail-safe aktiválódási idő |
| :--- | :--- | :--- |
| **`5 s`** (alapértelmezett) | `3 × 5 + 2` | **`17 mp`** |
| `10 s` | `3 × 10 + 2` | `32 mp` |
| `15 s` | `3 × 15 + 2` | `47 mp` |
| `20 s` | `3 × 20 + 2` | `62 mp` |
| `25 s` | `3 × 25 + 2` | `77 mp` |
| **`30 s`** | `3 × 30 + 2` | **`92 mp`** |

### 6.1 NTP + millis túlcsordulás

- **NTP szinkronizáció (ESP32-C3)**: WiFi csatlakozás után az ESP32 nem-blokkoló módon szinkronizál az `pool.ntp.org` és `time.nist.gov` szerverekről (`Europe/Budapest` időzóna + automatikus téli/nyári DST átállás). Sikertelen szinkronnál 60 mp-enként újrapróbál; WiFi vagy NTP hiányában minden naplóbejegyzés és állapot `UP <sec>s` formátumot használ, és a `setup()` sosem akad el.
- **49 napos `millis()` túlcsordulás-védelem**: Mind az ESP32-C3, mind az Arduino Nano **kizárólag előjel nélküli kivonást** (`static_cast<uint32_t>(now - last) >= INTERVAL`) használ minden időzítőnél (NRF fail-safe, ping ciklus, 20 mp menü timeout, 500 ms riasztás LED, 10 mp DS18B20 olvasás, 10 perces `manual_off`, 30 mp telemetria, hosszú/gyári gombnyomás). Ez a 32 bites számláló ~49,7 napos körbefordulásakor is pontos marad.
- **ESP–Nano időszinkron (naponta 2×)**: Az ESP32 boot-kor egyszer, majd naponta `00:00`-kor és `12:00`-kor (valamint kézi `POST /api/heating/:id/sync` vagy `{"action": "forceSync"}` parancsra azonnal) elküldi az aktuális epoch időt a 32 byte-os `REQUEST` csomag `timeSyncEpoch` mezőjében. A Nano a `RESPONSE` csomagban (`syncFlags` + `syncEpochLow16`) visszaigazolja a szinkront, és `(uint32_t)(millis() - syncMillis) / 1000` képlettel számolja tovább az idejét. Sikertelen szinkronnál (`3` próba után) 1 óra múlva automatikus újrapróba történik; ha 13 órán át nem kap friss szinkront, `nanoStale = true` jelzés jelenik meg, miközben a kazánvédelem és fail-safe zavartalanul működik tovább.

---

## 7. LCD Menü + Ikonok (Szobai vezérlés)

### 7.1 Főképernyő

```text
+--------------------+
|E:45.2C K: 2.5C  ▮◉ᯤ|   <- 1. sor: 0..16 szöveg, 17..19 állapot ikonok
|Sz:20.8C C:21.5C   =|   <- 2. sor: 0..18 szöveg, 19 trend nyíl (=, ↑, ↓)
+--------------------+
```

- **`E`** = Előremenő vízhőfok (`eloremeno`)
- **`K`** = Kültéri hőfok (`kulteri`)
- **`Sz`** = Szobai hőmérséklet az LM35 szenzorról (`currentTemp`, **10 mV / °C**, `tempC = mV / 10 + OFFSET`, érvényes tartomány: `-10..60 °C`)
- **`C`** = Célhőmérséklet (`targetTemp`)
- **2. sor 20. karakter (index `19`)**: minden képernyőn az `eloremeno` 20 perces trendje (`↑` slot `6`, `↓` slot `7`, vagy `=` ha `FLAT`).
- Ha nincs érvényes rádiókapcsolat (`nrf24_ok == false`), az előremenő és kültéri helyén **`E: --.- K: --.-`** jelenik meg.

### 7.2 Állapot ikonok (felső sor jobb széle: `17, 18, 19` pozíció)

Az LCD `0..5` saját karakterhelye (`createChar`) jeleníti meg a 3 valós idejű állapot ikont minden képernyőn (főképernyő, menük, szerkesztés, riasztás és gyári reset banner alatt is):

| Pozíció | Jelentés | Slot `0` / `2` / `4` | Slot `1` / `3` / `5` | Feltétel |
| :--- | :--- | :--- | :--- | :--- |
| **17** | **Szivattyú** | **Üres kocka** (`0`) = `KI` | **Teli kocka** (`1`) = `BE` | `state.pumpOn` értéke |
| **18** | **Rádió (NRF24)** | **Antenna + hullámok** (`2`) = `OK` | **Antenna + X** (`3`) = `NINCS KAPCSOLAT` | `state.nrf24Ok` értéke |
| **19** | **WiFi + MQTT** | **WiFi ívek** (`4`) = `WiFi + MQTT OK` | **WiFi ív + X** (`5`) = `HIBA` | **Csak akkor OK (`4`), ha a WiFi ÉS az MQTT is él!** (`WiFi.status() == WL_CONNECTED && mqtt.connected()`) |

### 7.3 Vezérlés (Rotary Encoder)

- **Rövid nyomás (`< 800 ms`)**: belépés a főmenübe / almenübe / szerkesztésbe, vagy az új érték **jóváhagyása**.
- **Tekerés**: navigálás a menüpontok között vagy érték módosítása.
- **Hosszú nyomás (`> 1000 ms`)**: visszalépés egy szintet (szerkesztés közben eldobja a még jóvá nem hagyott módosítást).
- **20 mp tétlenség**: automatikus visszatérés a főképernyőre (a félbehagyott szerkesztést eldobja).
- **Gyári visszaállítás**: a `BEALLITAS -> Gyari visszaallitas` menüponton **3 mp folyamatos nyomás** törli az NVS mentést és visszaállítja az alapértékeket (`GYARI BEALLITAS!` banner).

### 7.4 Menüfa

```text
FŐKÉPERNYŐ
 └── [Rövid nyomás] -> FŐMENÜ
      ├── FOKOK
      │    ├── Eloremeno
      │    ├── Visszatero
      │    ├── Kazan
      │    ├── Kulteri
      │    ├── Szoba
      │    ├── TREND              (Trend/Delta, Max/Min, Átlag/Bootóta, NULLAZAS 2 lépésben)
      │    └── VISSZA
      ├── STATUS
      │    ├── Szivattyu
      │    ├── Futes
      │    ├── Pumpa Allapot
      │    ├── Riasztas
      │    ├── Radio
      │    ├── Bizt. Hatar        (95.0C (FIX), csak olvasható)
      │    ├── NAPLO
      │    ├── CHECK              (NTP, Nano, Radio, Millis)
      │    └── VISSZA
      ├── BEALLITAS
      │    ├── Celhofok           (16.0 - 26.0 °C, lépés: 0.5 °C)
      │    ├── Uzmod              (AUTO / MANUAL / OFF)
      │    ├── Szivattyu Mod      (AUTO / BE / KI)
      │    ├── Pump Start         (30.0 - 50.0 °C)
      │    ├── Pump Stop          (20.0 - 40.0 °C, Start - Stop >= 5 °C)
      │    ├── Pump Szenzor       (KAZAN / ELOREMENO / MAX)
      │    ├── Riasztas Hofok     (70.0 - 90.0 °C)
      │    ├── Riasztas           (BE / KI)
      │    ├── Ping               (5s / 10s / 15s / 20s / 25s / 30s)
      │    ├── Manual Futes       (BE / KI)
      │    ├── Gyari visszaallit  (Tartsd 3mp-ig!)
      │    └── VISSZA
      └── KIMENETEK
           ├── Szivattyu          (AUTO módban ZÁROLVA, csak manual_on/off esetén kapcsolható)
           ├── LED Zold
           ├── LED Piros          (Aktív riasztás alatt a riasztás vezérli!)
           ├── Tartalek1
           ├── Tartalek2
           ├── Tartalek3
           ├── Tartalek4
           ├── Tartalek5
           └── VISSZA
```

### 7.5 Mentés (`Preferences` / NVS)

- Az ESP32-C3 a beépített **`Preferences` (NVS)** tárolót használja **`"futes"`** névtérrel (nem EEPROM-ot).
- **Minden jóváhagyás után** (a menüből vagy MQTT parancsból) azonnal:
  1. menti a beállításokat az NVS-be,
  2. küld egy azonnali `NRF REQUEST (0xA1)` csomagot a Nano-nak,
  3. publikálja a friss `STATUS` JSON-t (`retained=true`) MQTT-n.
- **Boot-kor** automatikusan visszatölti a mentett értékeket; első indításkor az alapértékek lépnek életbe:
  - `targetTemp`: **`21.5 °C`**
  - `pumpStartTemp`: **`40.0 °C`**, `pumpStopTemp`: **`30.0 °C`**
  - `alarmTemp`: **`85.0 °C`** *(a fix `95.0 °C`-os `SAFETY_OVERHEAT_TEMP` nem mentődik NVS-be)*
  - `heartbeatIntervalSec`: **`5 mp`**
- **A szivattyú kimeneti bitet (`OUT_BIT_SZIVATTYU`) NEM menti el** a `Preferences`, azt minden indítás után és működés közben a kazánvédelem állapotgépe számolja újra.

### 7.6 Trend (Előremenő figyelés és napi statisztika)

- **Figyelt szenzor**: kizárólag az **`eloremeno`** vízhőfok (a kazán és a szoba nem szól bele a trendbe).
- **20 perces mozgóátlag**: az ESP32 **60 másodpercenként** elment egy érvényes `eloremeno` mintát egy 20 elemű körkörös pufferbe (`-127 °C` szenzorhiba kihagyva). Az első 20 minta összegyűjtéséig a trend fixen **`FLAT` (`=`)**, így induláskor nem mutat hamis nyilat.
- **Irány meghatározása**: a 60 mp-enkénti újraszámítás az utolsó 10 minta átlagát veti össze az azt megelőző 10 minta átlagával (`delta = utolsó10 - előző10`):
  - `delta > +0.5 °C` → **`UP`** (`↑`, LCD slot `6`)
  - `delta < -0.5 °C` → **`DOWN`** (`↓`, LCD slot `7`)
  - `-0.5 °C <= delta <= +0.5 °C` → **`FLAT`** (beépített `'='` karakter, nem foglal saját CGRAM slotot)
- **LCD 2. sor 20. karakter (oszlop `19`)**: minden képernyőn (főképernyő, menü, szerkesztés, riasztás) megjeleníti az aktuális trend nyilat; a 2. sor szövege max. 19 karakterre korlátozott.
- **Napi/futásidejű Max / Min / Átlag (NINCS automatikus éjféli nullázás)**: a `Max`, `Min` és `Átlag` minden érvényes `eloremeno` olvasáskor frissül, a **boot óta folyamatosan gyűlik**, és **NEM nullázódik éjfélkor**. Kizárólag szándékos nullázással törölhető:
  - **Menüből**: `FOKOK -> TREND -> NULLAZAS` ponton kétlépcsős megerősítéssel (`BIZTOS?` + 5 mp visszaszámlálás, 2. rövid nyomásra `NULLAZVA!`, egyébként nem nulláz).
  - **Frontendről**: az **`Előremenő statisztika`** kártya **`Nullázás`** gombjával (kétlépcsős megerősítés, `POST /api/heating/:id/trend/reset` → `{"action": "resetTrend"}`).
- **Backend naplózás**: a backend kizárólag a **`UP ↔ DOWN`** irányváltásokat rögzíti `INFO` szinten (`TREND_UP` / `TREND_DOWN`), valamint naplózza a szándékos nullázásokat (`TREND_RESET_BY_USER`).

### 7.7 Mentés ellenőrzés és hibajelzés

Az ESP32-C3 firmware a `saveSetting(...)` egységes NVS-kezelőn keresztül minden írási és kommunikációs művelet visszatérési értékét ellenőrzi, hogy ne maradjon észrevétlen adatvesztés:

- **`Preferences` megnyitás ellenőrzése (`begin`)**: minden `prefs.begin("futes", false)` hívás visszatérési értéke ellenőrzött. Ha `false`, a soros porton **`NVS BEGIN FAILED`** jelenik meg, az LCD-n pedig **2 másodpercig** a **`"NVS HIBA!"`** üzenet látható.
- **`Preferences` írás ellenőrzése (`putFloat` / `putUInt` / `putBool` / `putString`)**: ha bármely `put*` művelet `0`-val tér vissza, a soros porton **`NVS WRITE FAILED key=<kulcsnév>`** jelenik meg, az LCD-n pedig **2 másodpercig** a **`"MENTES HIBA!"`** üzenet.
- **Visszagörgetés és egyszeri újrapróbálkozás**: sikertelen NVS íráskor az ESP32 **nem jelöli elmentettnek** a módosítást, megtartja a régi értéket a RAM-ban, a következő `loop()` ciklusban **egyszer automatikusan újrapróbálja** az írást, és **csak akkor** hívja meg az `onMenuSettingCommitted()`-et (valamint küld MQTT `STATUS`-t), ha minden `put*` művelet sikeres volt.
- **MQTT `publish()` ellenőrzés**: ha a `mqtt.publish()` `false` értékkel tér vissza, a soros porton **`MQTT PUBLISH FAILED topic=<topic>`** jelenik meg, a firmware pedig megjegyzi a függőben lévő státuszt, és a következő `STATUS` (heartbeat) ciklusban újraküldi, nem dobja el csendben.
- **Miért fontos ez?**: ha írás közben megtelik az NVS partíció vagy áramszünet / flash-hiba lép fel, a felhasználó azonnal látja az LCD-n és a soros porton, hogy a `22.0 °C` nem íródott tartós memóriába (így elkerülhető a látszólagos mentés utáni váratlan visszaállás `21.5 °C`-ra).

---

## 8. HiveMQ Cloud beállítás lépésről-lépésre

1. Hozz létre egy **HiveMQ Cloud** klasztert (a **Serverless / Free tier** csomag tökéletesen elegendő).
2. A klaszteren belül az **Access Management** menüpontban hozz létre egy új felhasználót és jelszót (pl. külön vagy közös hitelesítéssel az ESP32 és a backend számára).
3. A klaszter **URL-jét (Host)**, a **`8883` TLS portot**, valamint a felhasználónevet és jelszót írd be az `esp32/src/config.h` fájlba:
   - `MQTT_BROKER` (cluster URL), `MQTT_PORT` (`8883`), `MQTT_USER` és `MQTT_PASSWORD`
4. A backend gyökérkönyvtárában hozd létre a `.env` fájlt az `.env.example` alapján, és add meg ugyanazt a HiveMQ `MQTT_HOST`, `MQTT_PORT=8883`, `MQTT_USER` és `MQTT_PASS` értéket.
5. **Biztonsági figyelmeztetés:** A jelszavakat és MQTT hitelesítő adatokat **SOHA ne commitold Git-be!** A backendhez használd a `.gitignore`-ban szereplő `.env` fájlt. A Wi-Fi SSID-t és jelszót az ESP32 első indításkor a saját beállító hálózatán kéri el, majd NVS-ben tárolja.

### ESP32 Wi-Fi beállítása

1. Első indításkor nyisd meg a Serial Monitort (`115200` baud), és a megjelenő jelszóval csatlakozz a **`Futes-ESP32-Setup`** Wi-Fi hálózathoz.
2. Nyisd meg a **http://192.168.4.1** címet, add meg az otthoni Wi-Fi hálózat SSID-jét és jelszavát, majd mentsd el.
3. Sikeres csatlakozáskor a beállító hálózat leáll, a hitelesítő adatok pedig NVS-ben maradnak. Ha a hálózat 30 másodperc alatt nem érhető el, a portálon hibaüzenet jelenik meg, és újra megadhatod az adatokat.

A beállító hozzáférési pont eszközönként egyedi WPA2 jelszót használ, amelyet a soros monitor ír ki; ezt kezeld bizalmasan. A beállítás közben az ESP32 fűtésvezérlő főciklusa tovább fut.

### 8.1 TLS CA ellenőrzés

- Az ESP32-C3 firmware **SOHA nem használ `setInsecure()` módot** (sem alapértelmezettként, sem hiba esetén tartalékként); a TLS 8883-as kapcsolathoz minden esetben szigorú CA tanúsítvány-ellenőrzés (`WiFiClientSecure.setCACert(HIVEMQ_CA_CERT)`) történik.
- A HiveMQ Cloud gyökér CA tanúsítványa (Let's Encrypt **ISRG Root X1**) az `esp32/src/hivemq_ca.h` fájlban, `PROGMEM`-ben (`HIVEMQ_CA_CERT`) van tárolva.
- Ha a CA tanúsítvány hiányzik, hibás vagy lejárt, és a TLS handshake meghiúsul, az ESP32 **nem csatlakozik** az MQTT brokerhez, a soros portra kiírja a **`TLS CERT FAILED`** üzenetet, az LCD-n **2 másodpercre** megjeleníti a **`"TLS HIBA!"`** nem-blokkoló bannert (a felső sor állapotikonjainak megtartásával), majd **30 másodperc múlva** próbálja újra.
- **Tanúsítvány frissítése / beillesztése**: nyisd meg a **`HiveMQ Console -> Cluster -> Details`** oldalt, töltsd le a CA tanúsítványt (PEM), másold be a `-----BEGIN CERTIFICATE-----` ... `-----END CERTIFICATE-----` blokkot az `esp32/src/hivemq_ca.h` fájlba a `TODO` helyére, majd töltsd fel újra a firmware-t.

---

## 9. Startup sorrend (Indítás)

> **Biztonsági ajánlás külső eléréshez:** A backend alapértelmezetten csak a **`127.0.0.1`** (`localhost only`) címen figyel. Ha külső hálózatról is el szeretnéd érni a felületet, **NE használj router port-forwardot** — használj inkább VPN-t (**Tailscale** / **WireGuard**) vagy TLS-sel védett fordított proxyt, és **MINDIG állíts be erős `HEATING_API_KEY` értéket** az `.env` fájlban! (`HOST=0.0.0.0` esetén `HEATING_API_KEY` nélkül a szerver el sem indul.)

Indítsd el a rendszert pontosan ebben a sorrendben:

1. **Backend indítása**:
   ```bash
   npm install
   npm run dev   # vagy élesben: npm run build && npm start
   ```
   Ellenőrzés: a `GET http://localhost:3000/api/health` végpont válasza `{"ok":true,...}`.
2. **Arduino Nano feltöltése**:
   ```bash
   pio run -d nano -t upload
   ```
   Nyisd meg a Serial Monitort (`115200` baud), küldj egy **`a`** karaktert, ellenőrizd a 4 db DS18B20 `DeviceAddress` címét, és ha még a placeholder címek vannak benne, írd be őket a `nano/src/main.cpp`-be, majd töltsd fel újra.
3. **ESP32-C3 feltöltése és Wi-Fi beállítása**:
   ```bash
   pio run -d esp32 -t upload
   ```
   Első indításkor állítsd be a Wi-Fi-t a fenti beállító oldalon. Ezután ellenőrizd a Serial Monitoron, hogy a HiveMQ Cloud MQTT kapcsolat rendben feláll (`SIKERES!`).
4. **Frontend indítása**:
   Fejlesztői módban az `npm run dev` (Express + Vite middleware a `3000`-es porton) automatikusan kiszolgálja a frontendet; nyisd meg a böngészőben (`http://localhost:3000`).
5. **Első ellenőrzés**:
   A frontend felületén megjelenik a **Kapcsolat OK** státusz, az LCD felső sorának jobb szélén látszik a **3 állapot ikon**, a főképernyőn pedig olvasható a **3 hőmérséklet** (`E`, `K`, `Sz`).

> **Fontos:** Ha az Arduino Nano-t töltöd fel és helyezed áram alá először az ESP32 nélkül, a szivattyú reléje **BE fog kapcsolni** — ez a V5 fail-safe biztonságos alapállapota, **normális működés, nem hiba!**

---

## 10. Teszt lépések (Üzembe helyezés előtt)

### 10.1 Rádió / Fail-safe teszt

1. **Alap ping**: Mindkét egység bekapcsolva, Ping `5s` -> a frontendben **Kapcsolat OK**, és a válasz számláló (`nanoCounter`) folyamatosan növekszik. *(Ha nem forog, a csőcímek `FUTE1`/`FUTE2` fel vannak cserélve vagy az `AckPayload` nincs letiltva.)*
2. **Fail-safe (`5s` ping)**: Húzd ki a Nano tápját -> **`17 mp`** múlva a frontendben **NINCS KAPCSOLAT** piros villogó kártya jelenik meg, a Nano oldalon (táp visszaadása után ESP nélkül is) a szivattyú relé BEkapcsol és a piros LED villog. Az ESP újracsatlakozása után `5 mp`-en belül helyreáll a **Kapcsolat OK**.
3. **Lassú ping (`30s`)**: Állítsd a frontendben a pinget **`30s`**-ra -> húzd ki a Nano tápját -> **`92 mp`-ig NE legyen riasztás**. *(Ha 17 mp-nél már riaszt, a Nano még régi fix timeoutot használ.)*

### 10.2 LCD / Menü teszt

1. **Főképernyő**: Látszik az `E`, `K`, `Sz` és `C` hőfok?
2. **Menü belépés**: Rövid gombnyomásra megjelenik a `FOKOK` / `STATUS` / `BEALLITAS` / `KIMENETEK` főmenü?
3. **Érték módosítás és szinkron**: `BEALLITAS -> Celhofok` állítása `22.0 °C`-ra -> rövid nyomással jóváhagyás után a frontendben is azonnal `22.0 °C` jelenik meg?
4. **Inaktivitási timeout**: `20 mp` várakozás után a kijelző magától visszalép a főképernyőre?
5. **NVS megmaradás**: ESP32 áramtalanítása majd visszakapcsolása után megmaradt a `22.0 °C`? *(Preferences mentés ellenőrzése.)*

### 10.3 Ikon teszt

1. **Bekapcsolás**: Kezdetben az 1. ikon **üres kocka** (szivattyú KI), a 2. **antenna + X**, a 3. **WiFi + X**.
2. **WiFi + MQTT csatlakozás**: A 3. ikon átvált **WiFi ívekre** (csak akkor, ha mind a WiFi, mind az MQTT él).
3. **Nano bekapcsolás**: A középső (2.) ikon átvált **antenna + hullámokra**.
4. **Szivattyú BE**: Az 1. ikon átvált **teli kockára**.

### 10.4 Szivattyú teszt (hideg kazán)

1. **Hideg kazán**: `Pumpa Allapot: VARAKOZAS`, szivattyú `KI`.
2. **Felfűtés (`> 40 °C`)**: A kazán `40.0 °C` fölé melegszik -> szivattyú `BE`, `Pumpa Allapot: FUTES`.
3. **Lehűlés (`<= 30 °C`)**: Az előremenő (`eloremeno`) `30.0 °C` alá hűl -> szivattyú `KI`, `Pumpa Allapot: VARAKOZAS`.

---

## 11. Repo struktúra

```text
/
├── README.md
├── .env.example
├── .gitignore
├── package.json
├── server.ts                       # Express + Socket.IO + Vite belépési pont
├── nano/                           # Arduino Nano (SLAVE / Kazán) firmware
│   ├── platformio.ini
│   └── src/
│       ├── common.h                # Közös 32 byte-os V5 NRF24 szerződés
│       └── main.cpp                # DS18B20 cím alapú olvasás, fail-safe, relék
├── esp32/                          # ESP32-C3 (MASTER / Gateway) firmware
│   ├── platformio.ini
│   └── src/
│       ├── config.h                # WiFi, HiveMQ TLS, GPIO kiosztás, kalibráció
│       ├── config.h.example        # (Ajánlott sablon jelszavak nélkül Git-hez)
│       ├── lcd.h                   # 2x20 I2C LCD + 0..5 állapot + 6..7 trend ikonok + 100ms refresh
│       ├── menu.h                  # Menürendszer típusok és osztálydeklaráció
│       ├── menu.cpp                # Főképernyő, almenük, TREND nézet, szerkesztés, 20s timeout
│       └── main.cpp                # LM35, NRF24 master, MQTT TLS, NVS, NTP, trend & statisztika
├── server/                         # Backend modulok
│   ├── types.ts                    # V4 + alarm + heartbeat + trend TypeScript típusok
│   ├── validation.ts               # Validáció és frissességi alapú parancsépítés
│   ├── mqttBridge.ts               # HiveMQ TLS kliens, mély merge, eszköz mutex
│   └── heatingRoutes.ts            # REST API (/api/heating/:id/*)
└── src/                            # Frontend React + Tailwind alkalmazás
    ├── App.tsx                     # Fő dashboard, Socket.IO és API kezelők
    ├── config.ts                   # Frontend konstansok (pl. heartbeat opciók)
    ├── types/
    │   └── heating.ts              # Frontend típusdefiníciók
    └── components/
        ├── HeatingControl.tsx      # Szobatermosztát és üzemmód vezérlő
        ├── Temperatures.tsx        # 4 kazánoldali + szobai hőmérséklet + előremenő trend nyíl
        ├── EloremenoStats.tsx      # Előremenő napi Max / Min / Átlag statisztikai kártya
        ├── BoilerControl.tsx       # 8 kimenet kapcsolása (szivattyú zárolással)
        ├── BoilerStatus.tsx        # 8 bites kazán státusz bitek
        ├── PumpControl.tsx         # Kazánvédelem szivattyú beállítások
        ├── AlarmControl.tsx        # Túlmelegedés riasztás és villogó állapotjelző
        ├── HeartbeatControl.tsx    # V5 NRF24 életjel és ping intervallum (5–30s)
        ├── SystemCheck.tsx         # Rendszer ellenőrzés (NTP, Nano, Radio, Millis + kézi szinkron)
        └── LogsView.tsx            # Esemény és hiba napló szűréssel, lapozással és .txt exporttal
```

---

## 12. Hibaelhárítás (Gyakori hibák)

| Tünet | Ok | Megoldás |
| :--- | :--- | :--- |
| **LCD-n semmi / csak üres kockák látszanak** | Rossz `createChar` sorrend (még `init()` előtt hívva) vagy nem `0x27` az I2C cím | Ellenőrizd a `Wire.begin(8, 9)` → `_lcd.init()` → `createChar()` → `clear()` sorrendet a `esp32/src/lcd.h`-ban, és futtass I2C scannert (pl. `0x3F` vs `0x27`). |
| **Rádió: nem forog a számláló** | Felcserélt csőcímek (`FUTE1`/`FUTE2`) vagy bekapcsolva maradt `AckPayload` | Ellenőrizd mindkét oldalon a `NRF_ADDR_MASTER_TO_SLAVE` / `NRF_ADDR_SLAVE_TO_MASTER` címeket és a `radio.disableAckPayload()` hívást. |
| **Szivattyú nem kapcsol a `KIMENETEK` menüben/kártyán** | `pumpMode` `AUTO` módban van, így a kézi kapcsolás szándékosan zárolva van | Állítsd a **Szivattyú Módot** `manual_on`-ra (`BEALLITAS -> Szivattyu Mod`), vagy várd meg a `40.0 °C`-ot. |
| **Frontend `409 Conflict` hibát dob** | Még nem érkezett első ESP32 `STATUS` üzenet a backendhez | Indítsd el az ESP32-t, ellenőrizd a WiFi/MQTT kapcsolatot, és várd meg az első retained `STATUS` csomagot. |
| **Frontend `400 Bad Request` (`heartbeat: 12`)** | A ping intervallum csak `5, 10, 15, 20, 25, 30` másodperc lehet | Válassz a frontend 6 előre definiált gombja vagy az ESP32 `Ping` menüpontja közül. |
| **Riasztás nem villog, pedig 90 °C a kazán** | `alarmEnabled == false` (riasztás kikapcsolva) | Kapcsold **BE** a Riasztást a frontend **Túlmelegedés riasztás** kártyáján vagy az ESP32 menüben (`BEALLITAS -> Riasztas`). *(A szivattyú `95.0 °C` felett és szenzorhiba esetén ettől függetlenül kényszerítve BE marad!)* |
| **Riasztást 90-re állítottam, de a szivattyú már 85-nél bekapcsolt** | Ez nem hiba, a riasztás és a biztonság szét van választva | Ellenőrizd, hogy a szivattyú a 95°C-os biztonsági határnál kapcsol-e be, nem a riasztásnál. |
| **Áramkimaradás után visszaáll `21.5 °C`-ra** | A `Preferences` NVS nem mentette el a módosítást | Ellenőrizd, hogy szerkesztés után megvolt-e a **rövid nyomás (jóváhagyás)** — ne csak tekerd az értéket, mert 20 mp után vagy hosszú nyomásra eldobódik. |
| **`"MENTES HIBA!"` az LCD-n** | A `Preferences.put*()` `0` értékkel tért vissza (pl. megtelt NVS partíció vagy flash írási hiba) | Nézd meg a Serial Monitort (`NVS WRITE FAILED key=<kulcsnév>`). A firmware megőrzi a régi értéket és 1× újrapróbálja; tartós hiba esetén futtass 3 mp-es gyári visszaállítást vagy `pio run -d esp32 -t erase` törlést. |
| **`"NVS HIBA!"` az LCD-n** | A `Preferences.begin("futes", false)` `false`-szal tért vissza (sérült/nem inicializált NVS névtér) | Ellenőrizd a Serial Monitorban az `NVS BEGIN FAILED` naplót, és indítsd újra az ESP32-t; ha ismétlődik, töröld és töltsd újra a flash NVS partíciót. |
| **Frontend nem frissül, pedig LCD-n elmentve látszik** | Az `mqtt.publish()` sikertelen volt (WiFi/TLS megszakadt vagy `MQTT PUBLISH FAILED`) | Ellenőrizd az LCD jobb felső WiFi/MQTT ikonját (4-es ívek) és a Serial Monitort (`MQTT PUBLISH FAILED topic=...`); a következő `STATUS` ciklusban az ESP32 automatikusan újraküldi. |
| **LCD-n `"TLS HIBA!"`** | A CA tanúsítvány nem stimmel, hiányzik a `TODO` helyéről vagy lejárt | Töltsd le újra a HiveMQ CA-t (`HiveMQ Console -> Cluster -> Details`) és írd be a `esp32/src/hivemq_ca.h`-ba; **ne** kapcsold vissza a `setInsecure()` módot! |
| **POST `401 Unauthorized`** | Hiányzik vagy hibás az `Authorization: Bearer <HEATING_API_KEY>` fejléc | Ellenőrizd az `.env` fájl `HEATING_API_KEY` értékét és a kérés `Authorization` fejlécét. |
| **Szerver nem indul: `"HEATING_API_KEY kötelező"`** | `HOST=0.0.0.0` van beállítva, de nincs megadva `HEATING_API_KEY` | Állíts be erős `HEATING_API_KEY` kulcsot az `.env`-ben, vagy tedd vissza `HOST=127.0.0.1`-re. |
| **Logban `UP` idő van dátum helyett** | Az ESP32 még nem kapott NTP időszinkront (nincs WiFi vagy nem érhető el a `pool.ntp.org`) | Ellenőrizd a WiFi kapcsolatot és az NTP szerver elérhetőségét (`pool.ntp.org` / `time.nist.gov`); szinkron után a dátum automatikusan `YYYY-MM-DD HH:MM:SS` formátumra vált. |
| **Nano szinkron `ELAVULT`** | Több mint 13 órája nem érkezett sikeres `timeSyncEpoch` frissítés a Nano-hoz | Ellenőrizd az NRF24 rádiót a `STATUS -> CHECK` menüben vagy a **Rendszer ellenőrzés** kártyán, és indíts kézi frissítést a **Szinkronizálás most** gombbal; a kazánvédelem és fail-safe közben aktív marad. |
| **Trend nyíl villog fel-le** | Túl rövid mintavétel / szenzor ugrál | Ellenőrizd, hogy a 20 perces puffer megvan-e (`TREND_BUFFER_SIZE = 20`, `TREND_SAMPLE_INTERVAL_MS = 60000`), ne 1 perces különbséget nézz. |
| **Reggelre eltűnt a tegnapi max** | Ez nem hiba, boot volt éjszaka (áramkimaradás) | Ha áramkimaradás volt, a statisztika újraindul, ez normális (nincs éjféli automatikus nullázás). |
