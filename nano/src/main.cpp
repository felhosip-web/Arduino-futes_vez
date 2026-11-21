#include <Arduino.h>
#include <EEPROM.h>
#include <avr/wdt.h>
#include <SPI.h>
#include <RF24.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "common.h"
#include "sensor_store.h"
#include "safety_logic.h"

// ============================================================================
// ARDUINO NANO - KAZÁN OLDALI SLAVE VEZÉRLŐ (V5 MASTER-SLAVE PROTOKOLL)
// ============================================================================
// Bekötés (Hardware SPI):
//   DS18B20 OneWire busz    : D2 (4.7 kOhm pull-up az 5V felé)
//   NRF24L01+ (Hardware SPI): CE=D3, CSN=D4, MOSI=D11, MISO=D12, SCK=D13
//   Bemenetek:
//     D6 -> Szerviz kapcsoló (SPST GND-re, INPUT_PULLUP, aktív LOW = Szerviz BE)
//     D7 -> Relé fizikai visszacsatolás (DPDT 2. pólus: COM->GND, NC->D7, INPUT_PULLUP; D7 HIGH = BE, D7 LOW = KI)
//   Kimenetek:
//     D5      -> led_zold (Szerviz módban folyamatos BE)
//     A2 (16) -> led_piros (aktív riasztás esetén 500ms BE / 500ms KI villogás; D6 szabadon hagyva a szerviz kapcsolónak)
//     A1 (15) -> szivattyu relé meghajtás (D7 átadva a DPDT visszacsatolásnak)
//     D8      -> tartalek1
//     D9      -> tartalek2
//     D10     -> tartalek3 (OUTPUT módban marad, hogy az AVR Hardware SPI Master maradjon)
//     A3 (17) -> tartalek4
//     A4 (18) -> tartalek5
//
// V5 SLAVE SZABÁLYOK:
//   - SOHA nem ad önállóan, csak a Master (ESP32-C3) 0xA1 REQUEST / 0xA3 SENSOR_CMD
//     kérésére válaszol 200ms-on belül (sima write(), writeAckPayload TILOS).
//   - Seq duplikáció-védelem: ha már feldolgozott seq érkezik, ugyanazt a
//     RESPONSE csomagot küldi vissza a kimenetek / EEPROM újbóli végrehajtása nélkül.
//   - Fail-safe: induláskor amíg nincs ESP kapcsolat, valamint ha
//     (millis() - lastRequestMs) > 3 * heartbeatIntervalSec * 1000 + 2000:
//     szivattyú BE, piros LED 500ms-es villogás, alarmState reason = nrf_fail.
//   - Szerviz kapcsoló (D6): 50ms debounce után azonnali szivattyú BE + zöld LED
//     folyamatos BE, ESP pump parancstól függetlenül.
//   - Relé visszacsatolás (D7): 50ms debounce, >500ms eltérésnél RELAY_MISMATCH.
//   - DS18B20 Auto-Discovery + EEPROM tanítás: nincs beégetett ROM cím,
//     boot-kor OneWire keresés + EEPROM ellenőrzés; eltérésnél ISMERETLEN / tanítás.
// ============================================================================

constexpr uint8_t PIN_ONE_WIRE       = 2;
constexpr uint8_t PIN_NRF_CE         = 3;
constexpr uint8_t PIN_NRF_CSN        = 4;

constexpr uint8_t PIN_LED_ZOLD       = 5;
constexpr uint8_t PIN_SERVICE_SWITCH = 6;  // D6: SPST szerviz kapcsoló GND-re (INPUT_PULLUP, aktív LOW)
constexpr uint8_t PIN_RELAY_FEEDBACK = 7;  // D7: DPDT 2. pólus NC visszacsatolás (COM->GND, INPUT_PULLUP; HIGH=BE, LOW=KI)
constexpr uint8_t PIN_SZIVATTYU      = 15; // A1: szivattyú relé tekercs meghajtás
constexpr uint8_t PIN_TARTALEK1      = 8;
constexpr uint8_t PIN_TARTALEK2      = 9;
constexpr uint8_t PIN_TARTALEK3      = 10;
constexpr uint8_t PIN_LED_PIROS      = 16; // A2 (D6 átadva a szerviz kapcsolónak)
constexpr uint8_t PIN_TARTALEK4      = 17; // A3
constexpr uint8_t PIN_TARTALEK5      = 18; // A4

// Relé logika: állítsd true-ra, ha a relémodul LOW jellel húz be
constexpr bool RELAY_ACTIVE_LOW = false;

constexpr uint32_t TEMP_READ_INTERVAL_MS     = 10000UL;    // 10 mp-enként DS18B20 olvasás
constexpr uint32_t DS18B20_CONV_TIME_MS      = 800UL;      // 12-bit konverzió (~750ms)
constexpr uint32_t ALARM_BLINK_INTERVAL_MS   = 500UL;      // 500ms BE / 500ms KI
constexpr uint32_t SLAVE_TX_TURNAROUND_MS    = 5UL;        // 5ms szünet, hogy a Master biztosan RX módban legyen (< 200ms)
constexpr uint32_t NANO_SYNC_STALE_MS        = 46800000UL; // 13 óra (2x12h + 1h tűrés) -> syncStale = true
constexpr uint32_t SERVICE_DEBOUNCE_MS       = 50UL;       // 50ms szoftveres debounce a szerviz kapcsolónak
constexpr uint32_t RELAY_FB_DEBOUNCE_MS      = 50UL;       // 50ms szoftveres debounce a D7 relé visszacsatolásnak
constexpr uint32_t RELAY_MISMATCH_TIMEOUT_MS = 500UL;      // 500ms után RELAY_MISMATCH / RELAY_STUCK_*

// --- Loop Monitor küszöbök (Arduino Nano cél: < 10 ms) ---
constexpr uint32_t LOOP_WARN_US              = 50000UL;    // >50 ms -> WARN + overrun
constexpr uint32_t LOOP_FAULT_STREAK_US      = 100000UL;   // 5 egymást követő >100 ms -> loopFault
constexpr uint32_t LOOP_FAULT_SINGLE_US      = 500000UL;   // 1 loop >500 ms -> azonnali loopFault
constexpr uint32_t LOOP_RECOVER_US           = 10000UL;    // 10 egymást követő <10 ms -> helyreáll
constexpr uint32_t LOOP_WARN_COOLDOWN_MS     = 10000UL;    // 10 mp log cooldown
constexpr uint32_t LOOP_FAULT_BLINK_MS       = 100UL;      // 100 ms gyors zöld LED villogás hiba esetén
constexpr uint8_t  LOOP_FAULT_STREAK_COUNT   = 5;
constexpr uint8_t  LOOP_RECOVER_STREAK_COUNT = 10;

constexpr float ALARM_OVERHEAT_DEFAULT = 85.0f;
constexpr float ALARM_OVERHEAT_MIN     = 70.0f;
constexpr float ALARM_OVERHEAT_MAX     = 90.0f;

OneWire oneWire(PIN_ONE_WIRE);
DallasTemperature sensors(&oneWire);
RF24 radio(PIN_NRF_CE, PIN_NRF_CSN);

// --- EEPROM-ból betöltött és buszon felderített DS18B20 címek ---
DeviceAddress sensorAddr[SENSOR_ROLE_COUNT] = { {0}, {0}, {0}, {0} };
uint8_t       roleAssignedMask              = 0;
uint8_t       rolePresentMask               = 0;
uint8_t       roleKnownMask                 = 0;
bool          sensorUnknown                 = true;

DeviceAddress foundBusAddr[MAX_DISCOVERED_SENSORS] = { {0} };
int16_t       foundBusTempsDeci[MAX_DISCOVERED_SENSORS] = { -1270, -1270, -1270, -1270, -1270, -1270 };
uint8_t       foundBusCount                 = 0;
bool          scanConvPending               = false;
uint32_t      scanConvReqMs                 = 0;
bool          scanReadPending               = false;
uint8_t       scanReadStep                  = 0;
bool          scanReady                     = false;

// --- Tárolt konfiguráció és mért állapot ---
PumpConfigPacked lastPumpConfig = {
    PUMP_MODE_AUTO,
    40.0f,
    30.0f,
    PUMP_SENSOR_KAZAN
};

AlarmConfigPacked currentAlarmConfig = {
    ALARM_OVERHEAT_DEFAULT,
    1
};

AlarmStatePacked currentAlarmState = {
    1,
    ALARM_REASON_NRF_FAIL
};

uint8_t heartbeatIntervalSec = HEARTBEAT_DEFAULT_SEC;

float   latestEloremeno       = SENSOR_DISCONNECTED_VAL;
float   latestVisszatero      = SENSOR_DISCONNECTED_VAL;
float   latestKazan           = SENSOR_DISCONNECTED_VAL;
float   latestKulteri         = SENSOR_DISCONNECTED_VAL;
uint8_t latestSensorValidMask = 0;

NrfPacket rxPacket;
NrfPacket lastResponsePacket;
NrfPacket lastSensorResponsePacket;

uint8_t  lastEspOutputsMask     = 0;
bool     hasEspRequest          = false;
bool     hasProcessedSeq        = false;
uint32_t lastProcessedSeq       = 0;
bool     hasProcessedSensorSeq  = false;
uint32_t lastProcessedSensorSeq = 0;
uint32_t responseCounter        = 0;
uint32_t lastRequestMs          = 0;
uint32_t lastTempCycleMs        = 0;
bool     conversionRequested    = false;
uint32_t conversionReqMs        = 0;
bool     failsafeActive         = true; // Induláskor nincs még ESP kapcsolat -> biztonságos alapállapot

uint32_t lastAlarmBlinkMs     = 0;
bool     alarmBlinkOn         = true;

// --- Hardveres szerviz kapcsoló (D6 aktív LOW, 50ms túlcsordulás-biztos debounce) ---
bool     serviceMode          = false;
bool     serviceRawReading    = false;
uint32_t serviceDebounceMs    = 0;

// --- Relé fizikai visszacsatolás (DPDT 2. pólus NC -> D7 INPUT_PULLUP, 50ms debounce) ---
bool     relayFeedbackOn       = false;
bool     relayFbRawReading     = false;
uint32_t relayFbDebounceMs     = 0;

// --- Végponttól végpontig tartó parancs-visszaigazolás (CMD_ACK) ---
bool               commandedPumpOn = true; // Induláskor fail-safe szivattyú BE
uint8_t            lastCmdSeq      = 0;
RelayMismatchState relayMismatch   = {
    false, 0UL, false, STUCK_LOG_NONE, false, false, false, false
};

// --- ESP -> Nano időszinkron (49 napos millis() túlcsordulás-biztos) ---
uint32_t syncEpoch            = 0;
uint32_t syncMillis           = 0;
bool     hasTimeSync          = false;
bool     syncStale            = false;

// --- Loop Monitor + aszinkron DS18B20 lépésenkénti kiolvasás ---
uint32_t lastLoopUs           = 0;
uint32_t maxLoopUs            = 0;
uint32_t loopOverrunCount     = 0;
bool     loopFault            = false;
bool     loopMonitorSuspend   = false;
uint8_t  slowLoopStreak       = 0;
uint8_t  fastLoopStreak       = 0;
uint32_t lastSlowLogMs        = 0;
bool     hasSlowLogYet        = false;
uint32_t loopFaultBlinkMs     = 0;
bool     loopFaultBlinkOn     = false;

bool     dsReadPending        = false;
uint8_t  dsReadStep           = 0;
float    dsPendingEloremeno   = SENSOR_DISCONNECTED_VAL;
float    dsPendingVisszatero  = SENSOR_DISCONNECTED_VAL;
float    dsPendingKazan       = SENSOR_DISCONNECTED_VAL;
float    dsPendingKulteri     = SENSOR_DISCONNECTED_VAL;

uint32_t getNanoEpoch(uint32_t now) {
    if (!hasTimeSync) return 0UL;
    // OVERFLOW-SAFE: uint32_t kivonás, 49 napos millis túlcsorduláskor is helyes.
    return syncEpoch + (static_cast<uint32_t>(now - syncMillis) / 1000UL);
}

void updateServiceSwitch(uint32_t now) {
    bool rawLow = (digitalRead(PIN_SERVICE_SWITCH) == LOW);
    if (rawLow != serviceRawReading) {
        serviceRawReading = rawLow;
        serviceDebounceMs = now;
        return;
    }

    // OVERFLOW-SAFE: uint32_t kivonás, 49 napos millis túlcsorduláskor is helyes.
    if (serviceRawReading != serviceMode &&
        (static_cast<uint32_t>(now - serviceDebounceMs) >= SERVICE_DEBOUNCE_MS)) {
        serviceMode = serviceRawReading;
        Serial.println(serviceMode ? F("SERVICE_MODE_ON") : F("SERVICE_MODE_OFF"));
    }
}

void updateRelayFeedback(uint32_t now) {
    // D7 HIGH = NC kinyílt -> relé fizikailag BE; D7 LOW = NC zárva GND-re -> relé fizikailag KI
    bool rawHigh = (digitalRead(PIN_RELAY_FEEDBACK) == HIGH);
    if (rawHigh != relayFbRawReading) {
        relayFbRawReading = rawHigh;
        relayFbDebounceMs = now;
        return;
    }

    // OVERFLOW-SAFE: uint32_t kivonás, 49 napos millis túlcsorduláskor is helyes.
    if (relayFbRawReading != relayFeedbackOn &&
        (static_cast<uint32_t>(now - relayFbDebounceMs) >= RELAY_FB_DEBOUNCE_MS)) {
        relayFeedbackOn = relayFbRawReading;
    }
}

void evaluateRelayMismatch(uint32_t now) {
    RelayMismatchEvent evt = stepRelayMismatch(relayMismatch, commandedPumpOn, relayFeedbackOn, now);
    if (evt == RELAY_EVT_STUCK_ON) {
        Serial.println(F("RELAY_STUCK_ON"));
    } else if (evt == RELAY_EVT_STUCK_OFF) {
        Serial.println(F("RELAY_STUCK_OFF"));
    }
}

// ============================================================================
// KIMENETEK ÉS RIASZTÁSI VILLOGÁS KEZELÉSE
// ============================================================================
void writeRelayPin(uint8_t pin, bool on) {
    if (RELAY_ACTIVE_LOW) {
        digitalWrite(pin, on ? LOW : HIGH);
    } else {
        digitalWrite(pin, on ? HIGH : LOW);
    }
}

void applyOutputs(uint8_t mask) {
    writeRelayPin(PIN_SZIVATTYU, getBit(mask, OUT_BIT_SZIVATTYU));
    digitalWrite(PIN_LED_ZOLD,   getBit(mask, OUT_BIT_LED_ZOLD)  ? HIGH : LOW);
    digitalWrite(PIN_LED_PIROS,  getBit(mask, OUT_BIT_LED_PIROS) ? HIGH : LOW);
    digitalWrite(PIN_TARTALEK1,  getBit(mask, OUT_BIT_TARTALEK1) ? HIGH : LOW);
    digitalWrite(PIN_TARTALEK2,  getBit(mask, OUT_BIT_TARTALEK2) ? HIGH : LOW);
    digitalWrite(PIN_TARTALEK3,  getBit(mask, OUT_BIT_TARTALEK3) ? HIGH : LOW);
    digitalWrite(PIN_TARTALEK4,  getBit(mask, OUT_BIT_TARTALEK4) ? HIGH : LOW);
    digitalWrite(PIN_TARTALEK5,  getBit(mask, OUT_BIT_TARTALEK5) ? HIGH : LOW);
}

void sanitizeAlarmConfig(AlarmConfigPacked& cfg) {
    if (isnan(cfg.alarmTemp)) {
        cfg.alarmTemp = ALARM_OVERHEAT_DEFAULT;
    }
    if (cfg.alarmTemp < ALARM_OVERHEAT_MIN) cfg.alarmTemp = ALARM_OVERHEAT_MIN;
    if (cfg.alarmTemp > ALARM_OVERHEAT_MAX) cfg.alarmTemp = ALARM_OVERHEAT_MAX;
    cfg.alarmEnabled = cfg.alarmEnabled ? 1 : 0;
}

void evaluateLocalSafetyAndAlarm() {
    currentAlarmState = evaluateNanoAlarmState(
        latestKazan,
        sensorUnknown,
        hasEspRequest,
        failsafeActive,
        currentAlarmConfig
    );
}

void refreshHardwareOutputs(uint32_t now) {
    if (loopFault) {
        // OVERFLOW-SAFE: uint32_t kivonás, 49 napos millis túlcsorduláskor is helyes.
        if (static_cast<uint32_t>(now - loopFaultBlinkMs) >= LOOP_FAULT_BLINK_MS) {
            loopFaultBlinkMs = now;
            loopFaultBlinkOn = !loopFaultBlinkOn;
        }
    } else {
        loopFaultBlinkOn = false;
    }

    if (currentAlarmConfig.alarmEnabled == 0U) {
        alarmBlinkOn = false;
    } else if (currentAlarmState.active != 0U || !hasEspRequest || failsafeActive) {
        // OVERFLOW-SAFE: uint32_t kivonás, 49 napos millis túlcsorduláskor is helyes.
        if (static_cast<uint32_t>(now - lastAlarmBlinkMs) >= ALARM_BLINK_INTERVAL_MS) {
            lastAlarmBlinkMs = now;
            alarmBlinkOn = !alarmBlinkOn;
        }
    } else {
        alarmBlinkOn = false;
    }

    uint8_t effectiveMask = computeNanoEffectiveOutputs(
        latestKazan,
        hasEspRequest,
        failsafeActive,
        lastEspOutputsMask,
        serviceMode,
        loopFault,
        loopFaultBlinkOn,
        currentAlarmConfig,
        currentAlarmState,
        alarmBlinkOn
    );
    commandedPumpOn = getBit(effectiveMask, OUT_BIT_SZIVATTYU);
    applyOutputs(effectiveMask);
}

// ============================================================================
// EEPROM + ONEWIRE AUTO-DISCOVERY + TANÍTÁS
// ============================================================================
inline void eepromCommit() {
    // AVR <EEPROM.h> EEPROM.update() közvetlenül ír; ESP8266/ESP32 kompatibilitásért külön wrapper.
#if defined(ESP8266) || defined(ESP32)
    EEPROM.commit();
#endif
}

bool avrReadEepromBlock(void*, int addr, uint8_t* out, size_t len) {
    if (!out || addr < 0) return false;
    for (size_t i = 0; i < len; i++) {
        out[i] = EEPROM.read(addr + static_cast<int>(i));
    }
    return true;
}

bool avrWriteEepromBlock(void*, int addr, const uint8_t* data, size_t len) {
    if (!data || addr < 0) return false;
    for (size_t i = 0; i < len; i++) {
        EEPROM.update(addr + static_cast<int>(i), data[i]);
    }
    eepromCommit();
    return true;
}

inline EepromStorageOps nanoEepromOps() {
    EepromStorageOps ops = { nullptr, avrReadEepromBlock, avrWriteEepromBlock };
    return ops;
}

bool nanoScanBusCallback(void*, uint8_t outAddrs[MAX_DISCOVERED_SENSORS][8], uint8_t* outCount) {
    if (!outAddrs || !outCount) return false;
    uint8_t count = 0;
    uint8_t addr[8];
    oneWire.reset_search();
    delayMicroseconds(250);
    while (count < MAX_DISCOVERED_SENSORS && oneWire.search(addr)) {
        wdt_reset();
        if (isValidDs18b20Rom(addr)) {
            memcpy(outAddrs[count], addr, 8);
            count++;
        }
    }
    oneWire.reset_search();
    *outCount = count;
    return true;
}

void printSerialHexAddr(const uint8_t addr[8]) {
    char buf[24];
    formatDeviceAddressHex(addr, buf, sizeof(buf));
    Serial.print(buf);
}

void setRoleTemperatureDisconnected(uint8_t role) {
    if (role == SENSOR_ROLE_ELOREMENO)       latestEloremeno  = SENSOR_DISCONNECTED_VAL;
    else if (role == SENSOR_ROLE_VISSZATERO) latestVisszatero = SENSOR_DISCONNECTED_VAL;
    else if (role == SENSOR_ROLE_KAZAN)      latestKazan      = SENSOR_DISCONNECTED_VAL;
    else if (role == SENSOR_ROLE_KULTERI)    latestKulteri    = SENSOR_DISCONNECTED_VAL;
}

void loadSensorAddressesFromEeprom() {
    bool recovered = false;
    if (!loadSensorStore(nanoEepromOps(), sensorAddr, roleAssignedMask, &recovered)) {
        Serial.println(F("[SENSOR] EEPROM ures vagy ervenytelen - szenzor tanitas szukseges."));
        return;
    }
    if (recovered) {
        Serial.println(F("WARN EEPROM_RECOVERED_FROM_BACKUP"));
    }
    Serial.print(F("[SENSOR] EEPROM betoltve, assignedMask=0x"));
    Serial.println(roleAssignedMask, HEX);
}

bool saveSensorAddressToEeprom(uint8_t role, const uint8_t newAddr[8]) {
    int8_t clearedOldRole = -1;
    uint8_t freshAddrs[MAX_DISCOVERED_SENSORS][8];
    uint8_t freshCount = 0;
    SensorLearnStatus st = learnSensorRoleTransactional(
        role,
        newAddr,
        true,
        nanoScanBusCallback,
        nullptr,
        nanoEepromOps(),
        sensorAddr,
        roleAssignedMask,
        &clearedOldRole,
        freshAddrs,
        &freshCount
    );

    if (st != LEARN_OK) {
        if (st == LEARN_ERR_INVALID_ROM) {
            Serial.println(F("ERROR INVALID_DS18B20_ROM"));
        } else if (st == LEARN_ERR_NOT_ON_BUS || st == LEARN_ERR_SCAN_FAILED) {
            Serial.println(F("ERROR SENSOR_NOT_FOUND_ON_BUS"));
        } else if (st == LEARN_ERR_EEPROM_WRITE || st == LEARN_ERR_EEPROM_VERIFY) {
            Serial.println(F("ERROR EEPROM_VERIFY_FAIL"));
        } else {
            Serial.println(F("ERROR SENSOR_LEARN_REJECTED"));
        }
        return false;
    }

    // Friss keresési lista átvétele (foundBusTempsDeci nullázása az újraolvasásig)
    foundBusCount = freshCount;
    memset(foundBusAddr, 0, sizeof(foundBusAddr));
    for (uint8_t i = 0; i < MAX_DISCOVERED_SENSORS; i++) {
        foundBusTempsDeci[i] = -1270;
    }
    for (uint8_t i = 0; i < foundBusCount; i++) {
        memcpy(foundBusAddr[i], freshAddrs[i], 8);
    }
    scanReady = false;

    if (clearedOldRole >= 0) {
        setRoleTemperatureDisconnected(static_cast<uint8_t>(clearedOldRole));
        Serial.print(F("[SENSOR] Regi szerep torolve (csere): "));
        Serial.println(sensorRoleToKey(static_cast<uint8_t>(clearedOldRole)));
    }

    Serial.print(F("[SENSOR] Tanitva EEPROM-ba role="));
    Serial.print(sensorRoleToKey(role));
    Serial.print(F(" addr="));
    printSerialHexAddr(newAddr);
    Serial.println();
    return true;
}

void recomputeRolePresence(bool logMissing) {
    rolePresentMask = 0;
    for (uint8_t r = 0; r < SENSOR_ROLE_COUNT; r++) {
        if ((roleAssignedMask & (1U << r)) == 0) {
            continue;
        }
        for (uint8_t i = 0; i < foundBusCount; i++) {
            if (memcmp(sensorAddr[r], foundBusAddr[i], 8) == 0) {
                rolePresentMask |= static_cast<uint8_t>(1U << r);
                break;
            }
        }
    }

    roleKnownMask = static_cast<uint8_t>(roleAssignedMask & rolePresentMask);
    sensorUnknown = (foundBusCount < SENSOR_ROLE_COUNT) || (roleKnownMask != 0x0FU);

    if (logMissing) {
        if (sensorUnknown) {
            for (uint8_t r = 0; r < SENSOR_ROLE_COUNT; r++) {
                if ((roleKnownMask & (1U << r)) == 0) {
                    Serial.print(F("WARN SENSOR_UNKNOWN role="));
                    Serial.println(sensorRoleToKey(r));
                }
            }
        } else {
            Serial.println(F("[SENSOR] Mind a 4 DS18B20 szerep azonosítva és jelen van a buszon."));
        }
    }
}

void discoverBusAddresses(bool logMissing) {
    foundBusCount = 0;
    memset(foundBusAddr, 0, sizeof(foundBusAddr));
    for (uint8_t i = 0; i < MAX_DISCOVERED_SENSORS; i++) {
        foundBusTempsDeci[i] = -1270;
    }
    scanReady = false;

    nanoScanBusCallback(nullptr, foundBusAddr, &foundBusCount);

    sensors.begin();
    sensors.setResolution(12);
    sensors.setWaitForConversion(false);

    recomputeRolePresence(logMissing);
}

// ============================================================================
// DS18B20 ADDRESS-KIÍRÓ MÓD (SERIAL 'a' KARAKTERRE)
// ============================================================================
void printDeviceAddressCArray(const DeviceAddress addr) {
    Serial.print(F("{ "));
    for (uint8_t i = 0; i < 8; i++) {
        Serial.print(F("0x"));
        if (addr[i] < 16) Serial.print('0');
        Serial.print(addr[i], HEX);
        if (i < 7) Serial.print(F(", "));
    }
    Serial.print(F(" }"));
}

void runAddressScanner() {
    loopMonitorSuspend = true;
    wdt_reset();
    Serial.println(F("\n=== DS18B20 AUTO-DISCOVERY ==="));
    discoverBusAddresses(true);
    Serial.print(F("Talalt szenzorok szama: "));
    Serial.println(foundBusCount);

    sensors.setWaitForConversion(true);
    sensors.requestTemperatures();
    wdt_reset();

    for (uint8_t i = 0; i < foundBusCount; i++) {
        wdt_reset();
        float tempC = sensors.getTempC(foundBusAddr[i]);
        foundBusTempsDeci[i] = (tempC > -120.0f && !isnan(tempC) && tempC <= 125.0f)
                                   ? static_cast<int16_t>(lroundf(tempC * 10.0f))
                                   : -1270;
        Serial.print(F("  ["));
        Serial.print(i);
        Serial.print(F("] Cim: "));
        printSerialHexAddr(foundBusAddr[i]);
        Serial.print(F(" "));
        printDeviceAddressCArray(foundBusAddr[i]);
        Serial.print(F("  | Homerseklet: "));
        if (tempC <= -120.0f) {
            Serial.println(F("HIBA (-127)"));
        } else {
            Serial.print(tempC, 2);
            Serial.println(F(" C"));
        }
    }
    scanReady = true;
    sensors.setWaitForConversion(false);
    Serial.println(F("Ujraolvasas: kuldj 'a' karaktert, vagy tanits LCD/Frontend menubol."));
    Serial.println(F("=================================\n"));
    wdt_reset();
}

// ============================================================================
// DS18B20 OLVASÁS (-127 ESETÉN -127.0f)
// ============================================================================
float readSingleSensor(const DeviceAddress addr) {
    if (!isValidDs18b20Rom(addr)) {
        return SENSOR_DISCONNECTED_VAL;
    }
    float t = sensors.getTempC(addr);
    if (!isNanoTempValid(t)) {
        return SENSOR_DISCONNECTED_VAL;
    }
    return t;
}

float readRoleTemperature(uint8_t role) {
    if (role >= SENSOR_ROLE_COUNT || (roleAssignedMask & (1U << role)) == 0) {
        return SENSOR_DISCONNECTED_VAL;
    }
    return readSingleSensor(sensorAddr[role]);
}

void commitTemperatureResults() {
    uint8_t validMask = 0;
    bool eloreOk  = ((roleAssignedMask & (1U << SENSOR_ROLE_ELOREMENO))  != 0) && isNanoTempValid(latestEloremeno);
    bool visszaOk = ((roleAssignedMask & (1U << SENSOR_ROLE_VISSZATERO)) != 0) && isNanoTempValid(latestVisszatero);
    bool kazanOk  = ((roleAssignedMask & (1U << SENSOR_ROLE_KAZAN))      != 0) && isNanoTempValid(latestKazan);
    bool kulterOk = ((roleAssignedMask & (1U << SENSOR_ROLE_KULTERI))    != 0) && isNanoTempValid(latestKulteri);

    setBit(validMask, 0, eloreOk);
    setBit(validMask, 1, visszaOk);
    setBit(validMask, 2, kazanOk);
    setBit(validMask, 3, kulterOk);
    latestSensorValidMask = validMask;

    // Futásidejű lecsatlakozás és visszatérés követése (Match ROM + 9-byte scratchpad CRC8 alapján)
    rolePresentMask = validMask;
    roleKnownMask   = static_cast<uint8_t>(roleAssignedMask & rolePresentMask);
    sensorUnknown   = (roleKnownMask != 0x0FU);

    evaluateLocalSafetyAndAlarm();

    Serial.print(F("[TEMP] Elore:"));
    Serial.print(latestEloremeno, 1);
    Serial.print(F(" Vissza:"));
    Serial.print(latestVisszatero, 1);
    Serial.print(F(" Kazan:"));
    Serial.print(latestKazan, 1);
    Serial.print(F(" Kulteri:"));
    Serial.println(latestKulteri, 1);
}

void updateAllTemperatures() {
    latestEloremeno  = readRoleTemperature(SENSOR_ROLE_ELOREMENO);
    latestVisszatero = readRoleTemperature(SENSOR_ROLE_VISSZATERO);
    latestKazan      = readRoleTemperature(SENSOR_ROLE_KAZAN);
    latestKulteri    = readRoleTemperature(SENSOR_ROLE_KULTERI);
    commitTemperatureResults();
}

void stepAsyncTemperatureRead() {
    switch (dsReadStep) {
        case 0:
            dsPendingEloremeno = readRoleTemperature(SENSOR_ROLE_ELOREMENO);
            dsReadStep = 1;
            break;
        case 1:
            dsPendingVisszatero = readRoleTemperature(SENSOR_ROLE_VISSZATERO);
            dsReadStep = 2;
            break;
        case 2:
            dsPendingKazan = readRoleTemperature(SENSOR_ROLE_KAZAN);
            dsReadStep = 3;
            break;
        case 3:
        default:
            dsPendingKulteri = readRoleTemperature(SENSOR_ROLE_KULTERI);
            latestEloremeno  = dsPendingEloremeno;
            latestVisszatero = dsPendingVisszatero;
            latestKazan      = dsPendingKazan;
            latestKulteri    = dsPendingKulteri;
            dsReadPending    = false;
            dsReadStep       = 0;
            commitTemperatureResults();
            break;
    }
}

void stepAsyncScanRead() {
    if (scanReadStep >= foundBusCount) {
        scanReadPending = false;
        scanReadStep    = 0;
        scanReady       = true;
        return;
    }
    float t = readSingleSensor(foundBusAddr[scanReadStep]);
    foundBusTempsDeci[scanReadStep] = (t > -120.0f)
                                          ? static_cast<int16_t>(lroundf(t * 10.0f))
                                          : -1270;
    scanReadStep++;
    if (scanReadStep >= foundBusCount) {
        scanReadPending = false;
        scanReadStep    = 0;
        scanReady       = true;
    }
}

void buildResponsePacket(uint32_t seq) {
    memset(&lastResponsePacket, 0, sizeof(lastResponsePacket));
    lastResponsePacket.packetType           = PKT_TYPE_RESPONSE;
    lastResponsePacket.nano.eloremeno       = latestEloremeno;
    lastResponsePacket.nano.visszatero      = latestVisszatero;
    lastResponsePacket.nano.kazan           = latestKazan;
    lastResponsePacket.nano.kulteri         = latestKulteri;
    lastResponsePacket.nano.kazanStatusMask = 0;
    lastResponsePacket.nano.counter         = responseCounter;
    lastResponsePacket.nano.seq             = seq;
    lastResponsePacket.nano.sensorValidMask = latestSensorValidMask;

    uint8_t aPacked = packAlarmState(currentAlarmState);
    if (loopFault) {
        aPacked |= SYNC_FLAG_LOOP_FAULT;
    }
    if (sensorUnknown) {
        aPacked |= SYNC_FLAG_SENSOR_UNKNOWN;
    }
    lastResponsePacket.nano.alarmPacked     = aPacked;
    lastResponsePacket.nano.cmdSeq          = lastCmdSeq;

    uint8_t sFlags = 0;
    if (hasTimeSync)                         sFlags |= SYNC_FLAG_ACK;
    if (syncStale)                           sFlags |= SYNC_FLAG_STALE;
    if (serviceMode)                         sFlags |= SYNC_FLAG_SERVICE_MODE;
    if (relayMismatch.lastCmdAck)             sFlags |= SYNC_FLAG_CMD_ACK;
    if (commandedPumpOn)                     sFlags |= SYNC_FLAG_PUMP_ON;
    if (relayMismatch.lastCmdRejectedService) sFlags |= SYNC_FLAG_CMD_REJECTED_SERVICE;
    if (relayFeedbackOn)                     sFlags |= SYNC_FLAG_RELAY_FB_ON;
    if (relayMismatch.relayMismatchActive || relayMismatch.lastCmdMismatch) sFlags |= SYNC_FLAG_RELAY_MISMATCH;
    lastResponsePacket.nano.syncFlags      = sFlags;
    lastResponsePacket.nano.syncEpochLow16 = hasTimeSync ? static_cast<uint16_t>(syncEpoch & 0xFFFFU) : 0U;
}

void sendResponseToMaster(const NrfPacket& resp) {
    delay(SLAVE_TX_TURNAROUND_MS);
    radio.stopListening();
    radio.openWritingPipe(NRF_ADDR_SLAVE_TO_MASTER);
    radio.write(&resp, sizeof(resp));
    radio.startListening();
}

void buildSensorResponsePacket(uint32_t seq, uint8_t subCmd, uint8_t cmdSeq, uint8_t startIdx, bool ok) {
    memset(&lastSensorResponsePacket, 0, sizeof(lastSensorResponsePacket));
    lastSensorResponsePacket.packetType           = PKT_TYPE_SENSOR_RSP;
    lastSensorResponsePacket.sensorRsp.seq        = seq;
    lastSensorResponsePacket.sensorRsp.subCmd     = subCmd;
    lastSensorResponsePacket.sensorRsp.cmdSeq     = cmdSeq;
    lastSensorResponsePacket.sensorRsp.totalFound = foundBusCount;
    lastSensorResponsePacket.sensorRsp.startIndex = startIdx;

    uint8_t sFlags = 0;
    if (ok)            sFlags |= SENSOR_RSP_FLAG_OK;
    if (sensorUnknown) sFlags |= SENSOR_RSP_FLAG_UNKNOWN;
    if (scanReady)     sFlags |= SENSOR_RSP_FLAG_SCAN_READY;
    sFlags |= static_cast<uint8_t>((roleKnownMask & 0x0FU) << SENSOR_RSP_ROLE_MASK_SHIFT);
    lastSensorResponsePacket.sensorRsp.statusFlags = sFlags;

    uint8_t vMask = 0;
    if (subCmd == SENSOR_CMD_GET_ROLES) {
        for (uint8_t slot = 0; slot < 2; slot++) {
            uint8_t r = static_cast<uint8_t>(startIdx + slot);
            if (r < SENSOR_ROLE_COUNT && (roleAssignedMask & (1U << r)) != 0) {
                memcpy(lastSensorResponsePacket.sensorRsp.entries[slot].address, sensorAddr[r], 8);
                if ((roleKnownMask & (1U << r)) != 0) {
                    vMask |= static_cast<uint8_t>(1U << slot);
                }
            }
            lastSensorResponsePacket.sensorRsp.entries[slot].tempDeci = -1270;
        }
    } else if (subCmd == SENSOR_CMD_GET_FOUND) {
        for (uint8_t slot = 0; slot < 2; slot++) {
            uint8_t idx = static_cast<uint8_t>(startIdx + slot);
            if (idx < foundBusCount) {
                memcpy(lastSensorResponsePacket.sensorRsp.entries[slot].address, foundBusAddr[idx], 8);
                lastSensorResponsePacket.sensorRsp.entries[slot].tempDeci = foundBusTempsDeci[idx];
                vMask |= static_cast<uint8_t>(1U << slot);
            } else {
                lastSensorResponsePacket.sensorRsp.entries[slot].tempDeci = -1270;
            }
        }
    }
    lastSensorResponsePacket.sensorRsp.validEntryMask = vMask;
}

void handleSensorCommandPacket(const NrfPacket& pkt, uint32_t now) {
    lastRequestMs = now;
    uint32_t incomingSeq = pkt.sensorCmd.seq;

    if (hasProcessedSensorSeq && incomingSeq == lastProcessedSensorSeq) {
        sendResponseToMaster(lastSensorResponsePacket);
        return;
    }

    hasProcessedSensorSeq  = true;
    lastProcessedSensorSeq = incomingSeq;
    hasEspRequest          = true;
    if (failsafeActive) {
        failsafeActive = false;
        Serial.println(F("[NANO] Master kapcsolat helyreallt (sensorCmd), fail-safe feloldva."));
    }

    uint8_t subCmd      = pkt.sensorCmd.subCmd;
    uint8_t cmdSeq      = pkt.sensorCmd.cmdSeq;
    uint8_t roleOrIndex = pkt.sensorCmd.roleOrIndex;
    bool    ok          = true;

    switch (subCmd) {
        case SENSOR_CMD_SCAN_START:
            loopMonitorSuspend = true;
            discoverBusAddresses(false);
            sensors.requestTemperatures();
            scanConvPending = true;
            scanConvReqMs   = now;
            scanReadPending = false;
            scanReadStep    = 0;
            scanReady       = false;
            break;

        case SENSOR_CMD_GET_FOUND:
            break;

        case SENSOR_CMD_GET_ROLES:
            break;

        case SENSOR_CMD_LEARN: {
            loopMonitorSuspend = true;
            ok = saveSensorAddressToEeprom(roleOrIndex, pkt.sensorCmd.address);
            if (ok) {
                recomputeRolePresence(false);
                // Frissen tanított szerepnél új konverziót indítunk, elkerülve az unconverted 85.0°C power-on reset értéket
                setRoleTemperatureDisconnected(roleOrIndex);
                sensors.setWaitForConversion(false);
                sensors.requestTemperatures();
                conversionRequested = true;
                conversionReqMs     = now;
                dsReadPending       = false;
                commitTemperatureResults();
                refreshHardwareOutputs(now);
            }
            break;
        }

        default:
            ok = false;
            break;
    }

    buildSensorResponsePacket(incomingSeq, subCmd, cmdSeq, roleOrIndex, ok);
    sendResponseToMaster(lastSensorResponsePacket);
}

void handleSerialCommands() {
    while (Serial.available() > 0) {
        char c = static_cast<char>(Serial.read());
        if (c == 'a' || c == 'A') {
            runAddressScanner();
        }
    }
}

void evaluateLoopMonitor(uint32_t loopStartUs, uint32_t now) {
    if (loopMonitorSuspend) {
        loopMonitorSuspend = false;
        return;
    }

    // OVERFLOW-SAFE: uint32_t kivonás, 71 perces micros() túlcsorduláskor is helyes.
    lastLoopUs = static_cast<uint32_t>(micros() - loopStartUs);
    if (lastLoopUs > maxLoopUs) {
        maxLoopUs = lastLoopUs;
    }

    if (lastLoopUs > LOOP_WARN_US) {
        loopOverrunCount++;
        // OVERFLOW-SAFE: uint32_t kivonás, 49 napos millis túlcsorduláskor is helyes.
        if (!hasSlowLogYet || static_cast<uint32_t>(now - lastSlowLogMs) >= LOOP_WARN_COOLDOWN_MS) {
            hasSlowLogYet = true;
            lastSlowLogMs = now;
            Serial.print(F("WARN LOOP_SLOW "));
            Serial.print(lastLoopUs / 1000UL);
            Serial.println(F("ms"));
        }
    }

    if (lastLoopUs > LOOP_FAULT_SINGLE_US) {
        slowLoopStreak = LOOP_FAULT_STREAK_COUNT;
        fastLoopStreak = 0;
        if (!loopFault) {
            loopFault = true;
            Serial.print(F("ERROR LOOP_FAULT "));
            Serial.print(lastLoopUs / 1000UL);
            Serial.println(F("ms"));
        }
    } else if (lastLoopUs > LOOP_FAULT_STREAK_US) {
        fastLoopStreak = 0;
        if (slowLoopStreak < LOOP_FAULT_STREAK_COUNT) {
            slowLoopStreak++;
        }
        if (!loopFault && slowLoopStreak >= LOOP_FAULT_STREAK_COUNT) {
            loopFault = true;
            Serial.print(F("ERROR LOOP_FAULT "));
            Serial.print(lastLoopUs / 1000UL);
            Serial.println(F("ms"));
        }
    } else {
        slowLoopStreak = 0;
        if (lastLoopUs < LOOP_RECOVER_US) {
            if (fastLoopStreak < LOOP_RECOVER_STREAK_COUNT) {
                fastLoopStreak++;
            }
            if (loopFault && fastLoopStreak >= LOOP_RECOVER_STREAK_COUNT) {
                loopFault = false;
                Serial.println(F("LOOP_OK"));
            }
        } else {
            fastLoopStreak = 0;
        }
    }
}

// ============================================================================
// SETUP & LOOP
// ============================================================================
void setup() {
    uint8_t mcusrSnapshot = MCUSR;
    MCUSR = 0;
    wdt_disable();

    Serial.begin(115200);
    delay(200);
    Serial.println(F("\n[NANO V5] Kazan vezerlo SLAVE inditas..."));
    if (mcusrSnapshot & _BV(WDRF)) {
        Serial.println(F("WARN WATCHDOG_RESET: elozo ujraindulas hardveres watchdog miatt tortent!"));
    }

    pinMode(PIN_LED_ZOLD, OUTPUT);
    pinMode(PIN_SERVICE_SWITCH, INPUT_PULLUP);
    pinMode(PIN_RELAY_FEEDBACK, INPUT_PULLUP);
    pinMode(PIN_LED_PIROS, OUTPUT);
    pinMode(PIN_SZIVATTYU, OUTPUT);
    pinMode(PIN_TARTALEK1, OUTPUT);
    pinMode(PIN_TARTALEK2, OUTPUT);
    pinMode(PIN_TARTALEK3, OUTPUT); // D10 OUTPUT marad (AVR SPI Master)
    pinMode(PIN_TARTALEK4, OUTPUT);
    pinMode(PIN_TARTALEK5, OUTPUT);

    // Kezdeti szerviz kapcsoló és relé visszacsatolás (D7) beolvasása induláskor
    serviceRawReading = (digitalRead(PIN_SERVICE_SWITCH) == LOW);
    serviceMode       = serviceRawReading;
    serviceDebounceMs = millis();
    if (serviceMode) {
        Serial.println(F("SERVICE_MODE_ON"));
    }
    relayFbRawReading = (digitalRead(PIN_RELAY_FEEDBACK) == HIGH);
    relayFeedbackOn   = relayFbRawReading;
    relayFbDebounceMs = millis();

    // Indulási biztonságos alapállapot: amíg nincs ESP kapcsolat, szivattyú BE + piros LED villog
    failsafeActive = true;
    hasEspRequest = false;
    lastAlarmBlinkMs = millis();
    alarmBlinkOn = true;
    refreshHardwareOutputs(lastAlarmBlinkMs);
    Serial.println(F("[NANO] Indulasi biztonsagi allapot: Szivattyu BE, Piros LED villog."));

    // EEPROM DS18B20 szerep-címek betöltése + OneWire busz auto-discovery
    loadSensorAddressesFromEeprom();
    runAddressScanner();
    loopMonitorSuspend = false;
    sensors.setResolution(12);
    sensors.setWaitForConversion(true);
    sensors.requestTemperatures();
    updateAllTemperatures();
    sensors.setWaitForConversion(false);
    lastTempCycleMs = millis();

    if (!radio.begin()) {
        Serial.println(F("[NANO] HIBA: NRF24L01+ nem talalhato!"));
    } else {
        radio.disableAckPayload();
        radio.setPayloadSize(sizeof(NrfPacket));
        radio.setAutoAck(true);
        radio.setChannel(NRF_CHANNEL);
        radio.setDataRate(RF24_250KBPS);
        radio.setPALevel(RF24_PA_HIGH);
        radio.setRetries(5, 15);
        radio.openWritingPipe(NRF_ADDR_SLAVE_TO_MASTER);
        radio.openReadingPipe(1, NRF_ADDR_MASTER_TO_SLAVE);
        radio.startListening();
        Serial.println(F("[NANO] NRF24L01+ SLAVE keszenletben (AckPayload KI)."));
    }

    lastRequestMs = millis();
    refreshHardwareOutputs(lastRequestMs);

    // 2 másodperces hardveres AVR watchdog engedélyezése a hosszú setup() végén
    wdt_enable(WDTO_2S);
}

void loop() {
    uint32_t loopStartUs = micros();
    loopMonitorSuspend   = false;
    uint32_t now         = millis();
    handleSerialCommands();
    updateServiceSwitch(now);
    updateRelayFeedback(now);

    // 1. Nem-blokkoló aszinkron DS18B20 mérés 10 másodpercenként (4 szenzor ciklusokra bontva, 750ms konverzió nélkül)
    // OVERFLOW-SAFE: uint32_t kivonás, 49 napos millis túlcsorduláskor is helyes.
    if (!scanConvPending && !scanReadPending && !conversionRequested && !dsReadPending &&
        (static_cast<uint32_t>(now - lastTempCycleMs) >= TEMP_READ_INTERVAL_MS)) {
        sensors.requestTemperatures();
        conversionRequested = true;
        conversionReqMs = now;
    }

    // OVERFLOW-SAFE: uint32_t kivonás, 49 napos millis túlcsorduláskor is helyes.
    if (conversionRequested && (static_cast<uint32_t>(now - conversionReqMs) >= DS18B20_CONV_TIME_MS)) {
        conversionRequested = false;
        lastTempCycleMs     = now;
        dsReadPending       = true;
        dsReadStep          = 0;
    }

    if (dsReadPending) {
        stepAsyncTemperatureRead();
    }

    // 1b. Nem-blokkoló DS18B20 keresés (sensorScan) konverzió és lépésenkénti kiolvasás
    // OVERFLOW-SAFE: uint32_t kivonás, 49 napos millis túlcsorduláskor is helyes.
    if (scanConvPending && (static_cast<uint32_t>(now - scanConvReqMs) >= DS18B20_CONV_TIME_MS)) {
        scanConvPending = false;
        scanReadPending = true;
        scanReadStep    = 0;
    }

    if (scanReadPending) {
        stepAsyncScanRead();
    }

    // 2. Master (ESP32-C3) REQUEST (0xA1) vagy SENSOR_CMD (0xA3) fogadása és azonnali válasz
    if (radio.available()) {
        while (radio.available()) {
            radio.read(&rxPacket, sizeof(rxPacket));
        }

        if (rxPacket.packetType == PKT_TYPE_SENSOR_CMD) {
            handleSensorCommandPacket(rxPacket, now);
        } else if (rxPacket.packetType == PKT_TYPE_REQUEST) {
            lastRequestMs = now;
            uint32_t incomingSeq = rxPacket.esp.seq;

            if (hasProcessedSeq && incomingSeq == lastProcessedSeq) {
                // Seq duplikáció védelem: ugyanazt a RESPONSE-t küldjük vissza, kimeneteket NEM hajtjuk végre újra
                Serial.print(F("[NANO] Duplikalt seq="));
                Serial.println(incomingSeq);
                sendResponseToMaster(lastResponsePacket);
            } else {
                hasProcessedSeq  = true;
                lastProcessedSeq = incomingSeq;
                hasEspRequest    = true;

                if (failsafeActive) {
                    failsafeActive = false;
                    Serial.println(F("[NANO] Master kapcsolat helyreallt, fail-safe feloldva."));
                }

                lastPumpConfig       = rxPacket.esp.pumpConfig;
                currentAlarmConfig   = rxPacket.esp.alarmConfig;
                sanitizeAlarmConfig(currentAlarmConfig);
                heartbeatIntervalSec = sanitizeHeartbeatIntervalSec(rxPacket.esp.heartbeatIntervalSec);
                lastEspOutputsMask   = rxPacket.esp.outputsMask;

                if (rxPacket.esp.timeSyncEpoch > 0UL) {
                    syncEpoch   = rxPacket.esp.timeSyncEpoch;
                    syncMillis  = now;
                    hasTimeSync = true;
                    syncStale   = false;
                    Serial.print(F("[NANO] Idoszinkron OK epoch="));
                    Serial.println(syncEpoch);
                }

                evaluateLocalSafetyAndAlarm();
                refreshHardwareOutputs(now);
                evaluateRelayMismatch(now);

                uint8_t incomingCmdSeq = rxPacket.esp.cmdSeq;
                if (incomingCmdSeq != 0) {
                    lastCmdSeq = incomingCmdSeq;
                    bool requestedPumpOff =
                        !getBit(lastEspOutputsMask, OUT_BIT_SZIVATTYU) ||
                        (lastPumpConfig.pumpMode == PUMP_MODE_MANUAL_OFF);
                    if (serviceMode && requestedPumpOff) {
                        // A szerviz kapcsoló felülírja a szivattyú KI parancsot -> elutasítás SERVICE_MODE okkal
                        relayMismatch.lastCmdAck             = false;
                        relayMismatch.lastCmdRejectedService = true;
                        relayMismatch.lastCmdMismatch        = false;
                        relayMismatch.pendingCmdVerifyActive = false;
                        Serial.println(F("[NANO] CMD_REJECTED: SERVICE_MODE"));
                    } else if (commandedPumpOn == relayFeedbackOn) {
                        // D7 visszacsatolás már egyezik a parancsolt állapottal
                        relayMismatch.lastCmdAck             = true;
                        relayMismatch.lastCmdRejectedService = false;
                        relayMismatch.lastCmdMismatch        = false;
                        relayMismatch.pendingCmdVerifyActive = false;
                    } else if (relayMismatch.relayMismatchActive) {
                        // >500ms eltelt és a D7 továbbra sem egyezik -> RELAY_MISMATCH
                        relayMismatch.lastCmdAck             = false;
                        relayMismatch.lastCmdRejectedService = false;
                        relayMismatch.lastCmdMismatch        = true;
                        relayMismatch.pendingCmdVerifyActive = false;
                    } else {
                        // A relé épp kapcsol (50ms debounce / <500ms ablak) -> várunk a következő lekérdezésre
                        relayMismatch.lastCmdAck             = false;
                        relayMismatch.lastCmdRejectedService = false;
                        relayMismatch.lastCmdMismatch        = false;
                        relayMismatch.pendingCmdVerifyActive = true;
                    }
                } else {
                    lastCmdSeq                           = 0;
                    relayMismatch.lastCmdAck             = false;
                    relayMismatch.lastCmdRejectedService = false;
                    relayMismatch.lastCmdMismatch        = false;
                    relayMismatch.pendingCmdVerifyActive = false;
                }

                responseCounter++;
                buildResponsePacket(incomingSeq);
                sendResponseToMaster(lastResponsePacket);
            }
        }
    }

    // 3. V5 Fail-safe: ha (uint32_t)(now - lastRequestMs) > 3 * heartbeatIntervalSec * 1000 + 2000
    // OVERFLOW-SAFE: uint32_t kivonás, 49 napos millis túlcsorduláskor is helyes.
    uint32_t failsafeTimeoutMs =
        (3UL * static_cast<uint32_t>(heartbeatIntervalSec) * 1000UL) + 2000UL;

    if (!failsafeActive && (static_cast<uint32_t>(now - lastRequestMs) > failsafeTimeoutMs)) {
        failsafeActive = true;
        evaluateLocalSafetyAndAlarm();
        Serial.println(F("[NANO] FAIL-SAFE AKTIV: Szivattyu BE, Piros LED villog (nrf_fail)"));
    }

    // 3b. Időszinkron elavulás ellenőrzése (13 óra szinkron nélkül -> syncStale = true, pumpavédelem érintetlen)
    // OVERFLOW-SAFE: uint32_t kivonás, 49 napos millis túlcsorduláskor is helyes.
    if (hasTimeSync && !syncStale && (static_cast<uint32_t>(now - syncMillis) >= NANO_SYNC_STALE_MS)) {
        syncStale = true;
        Serial.println(F("[NANO] Idoszinkron elavult (>13h) - pumpa es fail-safe tovabbra is aktiv."));
    }

    // 4. Kimenetek, relé visszacsatolás mismatch (500ms) és nem-blokkoló 500ms riasztási villogás frissítése
    refreshHardwareOutputs(now);
    evaluateRelayMismatch(now);

    // 5. Loop Monitor + hardveres watchdog reset
    evaluateLoopMonitor(loopStartUs, now);
    wdt_reset();
}
