# Fűtésvezérlő és Kazánvédelmi Rendszer (V5)

Két egységes, **Master–Slave** felépítésű okos fűtésvezérlő és szilárdtüzelésű kazánvédelmi rendszer **Arduino Nano (kazán oldal)**, **ESP32-C3 (szoba oldal / I2C LCD + rotary encoder + WiFi/MQTT gateway)**, **Node.js/Express + Socket.IO backend** és **React frontend** komponensekkel, **HiveMQ Cloud TLS 8883** kapcsolaton keresztül.

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
| **LM35 szobai hőfok** | `GPIO 2` | Analóg bemenet (ADC) | 10 mérés átlaga, `LM35_OFFSET` kalibráció a `esp32/src/config.h`-ban |
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
| **`EspToNanoPayload`** | `outputsMask`, `targetTemp`, `mode`, `manualHeating`, `pumpConfig`, `alarmConfig`, `heartbeatIntervalSec`, `seq`, `reserved[4]` | Az ESP32 kimeneti bitmaszkja, termosztát, szivattyú-, riasztás- és ping beállításai, valamint a növekvő `seq` sorszám. |
| **`NanoToEspPayload`** | `eloremeno`, `visszatero`, `kazan`, `kulteri`, `kazanStatusMask`, `counter`, `seq`, `sensorValidMask`, `alarmState`, `reserved[3]` | A 4 mért hőfok (`-127.0f` hiba esetén), az 1 byte-os `kazanStatusMask` (amelyet az ESP32 bont vissza 8 elemű `kazan_status` tömbbé), a válasz `counter`, a szenzor-érvényességi maszk, az `alarmState`, és az eredeti `seq`. |

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
    "overheatTemp": 85.0,
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
    "overheatTemp": 85.0,
    "alarmEnabled": true
  },
  "alarmState": {
    "active": false,
    "reason": "none"
  },
  "heartbeat": {
    "intervalSec": 5
  },
  "nrf24_ok": true,
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

1. **Túlmelegedés (`kazan >= overheatTemp`)**: a szivattyú kényszerítve **BEkapcsol**.
2. **Kazán szenzor hiba (`kazan == -127` / érvénytelen)**: a szivattyú kényszerítve **BEkapcsol**.
3. **NRF24 rádiós szakadás**: az Arduino Nano fail-safe módba lép és **BEkapcsolja** a szivattyút.

---

## 5. Riasztás

- **`alarmConfig`**:
  - `overheatTemp`: `70.0 – 90.0 °C` (alapérték: **`85.0 °C`**)
  - `alarmEnabled`: `true` / `false` (alapérték: **`true`**)
- **`alarmState.reason`**:
  - `"none"` – nincs riasztás
  - `"overheat"` – a kazán elérte vagy meghaladta az `overheatTemp` értéket
  - `"sensor_fail"` – a kazán DS18B20 szenzor nem olvasható (`-127 °C`)
  - `"nrf_fail"` – az ESP32 és Nano közötti rádiós kapcsolat megszakadt
- **LED jelzés**:
  - Ha `alarmState.active == true` **ÉS** `alarmEnabled == true`, akkor **MINDKÉT piros LED** (Nano `D6` és ESP32 `GPIO 11`) szinkron nélkül, nem-blokkoló `millis()` időzítéssel villog **`500 ms BE / 500 ms KI`** ütemben (`delay()` használata nélkül).
  - Ha `alarmEnabled == false`, akkor **nincs piros LED villogás** és az LCD sem villogtat riasztási sort, **DE a szivattyú biztonsági védelme túlmelegedés vagy szenzorhiba esetén továbbra is kényszerítve BE marad!**
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
  - **Master oldalon**: válaszonként max. **`500 ms`** várakozás, kérésenként legfeljebb **`3` próba**. Ha **`3` egymást követő ping ciklus** sikertelen, a Master `nrf24_ok = false` és `alarmState.reason = "nrf_fail"` állapotba vált.
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

---

## 7. LCD Menü + Ikonok (Szobai vezérlés)

### 7.1 Főképernyő

```text
+--------------------+
|E:45.2C K: 2.5C  ▮◉ᯤ|   <- 1. sor: 0..16 szöveg, 17..19 állapot ikonok
|Sz:20.8C C:21.5C    |   <- 2. sor: szoba & célhőmérséklet (riasztáskor 500ms váltakozás)
+--------------------+
```

- **`E`** = Előremenő vízhőfok (`eloremeno`)
- **`K`** = Kültéri hőfok (`kulteri`)
- **`Sz`** = Szobai hőmérséklet az LM35 szenzorról (`currentTemp`)
- **`C`** = Célhőmérséklet (`targetTemp`)
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
      │    └── VISSZA
      ├── STATUS
      │    ├── Szivattyu
      │    ├── Futes
      │    ├── Pumpa Allapot
      │    ├── Riasztas
      │    ├── Radio
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
  - `overheatTemp`: **`85.0 °C`**
  - `heartbeatIntervalSec`: **`5 mp`**
- **A szivattyú kimeneti bitet (`OUT_BIT_SZIVATTYU`) NEM menti el** a `Preferences`, azt minden indítás után és működés közben a kazánvédelem állapotgépe számolja újra.

---

## 8. HiveMQ Cloud beállítás lépésről-lépésre

1. Hozz létre egy **HiveMQ Cloud** klasztert (a **Serverless / Free tier** csomag tökéletesen elegendő).
2. A klaszteren belül az **Access Management** menüpontban hozz létre egy új felhasználót és jelszót (pl. külön vagy közös hitelesítéssel az ESP32 és a backend számára).
3. A klaszter **URL-jét (Host)**, a **`8883` TLS portot**, valamint a felhasználónevet és jelszót írd be az `esp32/src/config.h` fájlba:
   - `WIFI_SSID` és `WIFI_PASSWORD` (a projektben `WIFI_PASS` / `WIFI_PASSWORD`)
   - `MQTT_BROKER` (cluster URL, `MQTT_HOST`), `MQTT_PORT` (`8883`), `MQTT_USER`, `MQTT_PASSWORD` (`MQTT_PASS`)
4. A backend gyökérkönyvtárában hozd létre a `.env` fájlt az `.env.example` alapján, és add meg ugyanazt a HiveMQ `MQTT_HOST`, `MQTT_PORT=8883`, `MQTT_USER` és `MQTT_PASS` értéket.
5. **Biztonsági figyelmeztetés:** A jelszavakat és WiFi/MQTT hitelesítő adatokat **SOHA ne commitold Git-be!** A backendhez használd a `.gitignore`-ban szereplő `.env` fájlt, az ESP32 `esp32/src/config.h` fájlját pedig tartsd helyben (vedd fel `.gitignore`-ba vagy használj `config.h.example` sablont).

---

## 9. Startup sorrend (Indítás)

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
3. **ESP32-C3 feltöltése**:
   ```bash
   pio run -d esp32 -t upload
   ```
   Ellenőrizd a Serial Monitoron, hogy a WiFi és a HiveMQ Cloud MQTT kapcsolat rendben feláll (`SIKERES!`).
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
│       ├── lcd.h                   # 2x20 I2C LCD + 0..5 saját ikonok + 100ms refresh
│       ├── menu.h                  # Menürendszer típusok és osztálydeklaráció
│       ├── menu.cpp                # Főképernyő, almenük, szerkesztés, 20s timeout
│       └── main.cpp                # LM35, NRF24 master, MQTT TLS, Preferences NVS
├── server/                         # Backend modulok
│   ├── types.ts                    # V4 + alarm + heartbeat TypeScript típusok
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
        ├── Temperatures.tsx        # 4 kazánoldali + szobai hőmérséklet kijelzés
        ├── BoilerControl.tsx       # 8 kimenet kapcsolása (szivattyú zárolással)
        ├── BoilerStatus.tsx        # 8 bites kazán státusz bitek
        ├── PumpControl.tsx         # Kazánvédelem szivattyú beállítások
        ├── AlarmControl.tsx        # Túlmelegedés riasztás és villogó állapotjelző
        └── HeartbeatControl.tsx    # V5 NRF24 életjel és ping intervallum (5–30s)
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
| **Riasztás nem villog, pedig 90 °C a kazán** | `alarmEnabled == false` (riasztás kikapcsolva) | Kapcsold **BE** a Riasztást a frontend **Túlmelegedés riasztás** kártyáján vagy az ESP32 menüben (`BEALLITAS -> Riasztas`). *(A szivattyú ettől függetlenül kényszerítve BE marad!)* |
| **Áramkimaradás után visszaáll `21.5 °C`-ra** | A `Preferences` NVS nem mentette el a módosítást | Ellenőrizd, hogy szerkesztés után megvolt-e a **rövid nyomás (jóváhagyás)** — ne csak tekerd az értéket, mert 20 mp után vagy hosszú nyomásra eldobódik. |
