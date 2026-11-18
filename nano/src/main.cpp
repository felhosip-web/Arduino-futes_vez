#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "common.h"

// ============================================================================
// ARDUINO NANO - KAZÁN OLDALI SLAVE VEZÉRLŐ (V5 MASTER-SLAVE PROTOKOLL)
// ============================================================================
// Bekötés (Hardware SPI):
//   DS18B20 OneWire busz    : D2 (4.7 kOhm pull-up az 5V felé)
//   NRF24L01+ (Hardware SPI): CE=D3, CSN=D4, MOSI=D11, MISO=D12, SCK=D13
//   Bemenetek:
//     D6 -> Szerviz kapcsoló (SPST GND-re, INPUT_PULLUP, aktív LOW = Szerviz BE)
//   Kimenetek:
//     D5      -> led_zold (Szerviz módban folyamatos BE)
//     A2 (16) -> led_piros (aktív riasztás esetén 500ms BE / 500ms KI villogás; D6 szabadon hagyva a szerviz kapcsolónak)
//     D7      -> szivattyu relé
//     D8      -> tartalek1
//     D9      -> tartalek2
//     D10     -> tartalek3 (OUTPUT módban marad, hogy az AVR Hardware SPI Master maradjon)
//     A3 (17) -> tartalek4
//     A4 (18) -> tartalek5
//
// V5 SLAVE SZABÁLYOK:
//   - SOHA nem ad önállóan, csak a Master (ESP32-C3) 0xA1 REQUEST-jére válaszol
//     200ms-on belül 0xB2 RESPONSE csomaggal (sima write(), writeAckPayload TILOS).
//   - Seq duplikáció-védelem: ha már feldolgozott seq érkezik, ugyanazt a
//     RESPONSE csomagot küldi vissza a kimenetek újbóli végrehajtása nélkül.
//   - Fail-safe: induláskor amíg nincs ESP kapcsolat, valamint ha
//     (millis() - lastRequestMs) > 3 * heartbeatIntervalSec * 1000 + 2000:
//     szivattyú BE, piros LED 500ms-es villogás, alarmState reason = nrf_fail.
//   - Szerviz kapcsoló (D6): 50ms debounce után azonnali szivattyú BE + zöld LED
//     folyamatos BE, ESP pump parancstól függetlenül.
// ============================================================================

constexpr uint8_t PIN_ONE_WIRE       = 2;
constexpr uint8_t PIN_NRF_CE         = 3;
constexpr uint8_t PIN_NRF_CSN        = 4;

constexpr uint8_t PIN_LED_ZOLD       = 5;
constexpr uint8_t PIN_SERVICE_SWITCH = 6;  // D6: SPST szerviz kapcsoló GND-re (INPUT_PULLUP, aktív LOW)
constexpr uint8_t PIN_SZIVATTYU      = 7;
constexpr uint8_t PIN_TARTALEK1      = 8;
constexpr uint8_t PIN_TARTALEK2      = 9;
constexpr uint8_t PIN_TARTALEK3      = 10;
constexpr uint8_t PIN_LED_PIROS      = 16; // A2 (D6 átadva a szerviz kapcsolónak)
constexpr uint8_t PIN_TARTALEK4      = 17; // A3
constexpr uint8_t PIN_TARTALEK5      = 18; // A4

// Relé logika: állítsd true-ra, ha a relémodul LOW jellel húz be
constexpr bool RELAY_ACTIVE_LOW = false;

constexpr uint32_t TEMP_READ_INTERVAL_MS   = 10000UL;    // 10 mp-enként DS18B20 olvasás
constexpr uint32_t DS18B20_CONV_TIME_MS    = 800UL;      // 12-bit konverzió (~750ms)
constexpr uint32_t ALARM_BLINK_INTERVAL_MS = 500UL;      // 500ms BE / 500ms KI
constexpr uint32_t SLAVE_TX_TURNAROUND_MS  = 5UL;        // 5ms szünet, hogy a Master biztosan RX módban legyen (< 200ms)
constexpr uint32_t NANO_SYNC_STALE_MS      = 46800000UL; // 13 óra (2x12h + 1h tűrés) -> syncStale = true
constexpr uint32_t SERVICE_DEBOUNCE_MS     = 50UL;       // 50ms szoftveres debounce a szerviz kapcsolónak

constexpr float ALARM_OVERHEAT_DEFAULT = 85.0f;
constexpr float ALARM_OVERHEAT_MIN     = 70.0f;
constexpr float ALARM_OVERHEAT_MAX     = 90.0f;

// --- DS18B20 szenzor címek (cseréld le a Serial address-kiíró mód alapján!) ---
DeviceAddress ADDR_ELOREMENO  = { 0x28, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 }; // TODO: CSERÉLD LE VALÓS CÍMRE!
DeviceAddress ADDR_VISSZATERO = { 0x28, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02 }; // TODO: CSERÉLD LE VALÓS CÍMRE!
DeviceAddress ADDR_KAZAN      = { 0x28, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03 }; // TODO: CSERÉLD LE VALÓS CÍMRE!
DeviceAddress ADDR_KULTERI    = { 0x28, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04 }; // TODO: CSERÉLD LE VALÓS CÍMRE!

OneWire oneWire(PIN_ONE_WIRE);
DallasTemperature sensors(&oneWire);
RF24 radio(PIN_NRF_CE, PIN_NRF_CSN);

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

uint8_t  lastEspOutputsMask   = 0;
bool     hasEspRequest        = false;
bool     hasProcessedSeq      = false;
uint32_t lastProcessedSeq     = 0;
uint32_t responseCounter      = 0;
uint32_t lastRequestMs        = 0;
uint32_t lastTempCycleMs      = 0;
bool     conversionRequested  = false;
uint32_t conversionReqMs      = 0;
bool     failsafeActive       = true; // Induláskor nincs még ESP kapcsolat -> biztonságos alapállapot

uint32_t lastAlarmBlinkMs     = 0;
bool     alarmBlinkOn         = true;

// --- Hardveres szerviz kapcsoló (D6 aktív LOW, 50ms túlcsordulás-biztos debounce) ---
bool     serviceMode          = false;
bool     serviceRawReading    = false;
uint32_t serviceDebounceMs    = 0;

// --- ESP -> Nano időszinkron (49 napos millis() túlcsordulás-biztos) ---
uint32_t syncEpoch            = 0;
uint32_t syncMillis           = 0;
bool     hasTimeSync          = false;
bool     syncStale            = false;

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
    bool kazanInvalid = isnan(latestKazan) || (latestKazan <= -120.0f) || (latestKazan > 125.0f);
    bool alarmOverheat = !kazanInvalid && (latestKazan >= currentAlarmConfig.alarmTemp);

    if (currentAlarmConfig.alarmEnabled == 0) {
        currentAlarmState.active = 0;
        currentAlarmState.reason = ALARM_REASON_NONE;
    } else if (alarmOverheat) {
        currentAlarmState.active = 1;
        currentAlarmState.reason = ALARM_REASON_OVERHEAT;
    } else if (kazanInvalid) {
        currentAlarmState.active = 1;
        currentAlarmState.reason = ALARM_REASON_SENSOR_FAIL;
    } else if (!hasEspRequest || failsafeActive) {
        currentAlarmState.active = 1;
        currentAlarmState.reason = ALARM_REASON_NRF_FAIL;
    } else {
        currentAlarmState.active = 0;
        currentAlarmState.reason = ALARM_REASON_NONE;
    }
}

void refreshHardwareOutputs(uint32_t now) {
    bool kazanInvalid   = isnan(latestKazan) || (latestKazan <= -120.0f) || (latestKazan > 125.0f);
    bool safetyOverheat = !kazanInvalid && (latestKazan >= SAFETY_OVERHEAT_TEMP);

    uint8_t effectiveMask = 0;
    if (!hasEspRequest || failsafeActive) {
        setBit(effectiveMask, OUT_BIT_SZIVATTYU, true);
    } else {
        effectiveMask = lastEspOutputsMask;
    }

    // Biztonsági szabály: CSAK a fix SAFETY_OVERHEAT_TEMP (95.0C) vagy kazán szenzorhiba (-127) kapcsolja BE a szivattyút
    // SAFETY: alarmEnabled nem kapcsolja ki a pumpavédelmet
    if (safetyOverheat || kazanInvalid) {
        setBit(effectiveMask, OUT_BIT_SZIVATTYU, true);
    }

    // Szerviz kapcsoló (D6 aktív LOW): szivattyú AZONNAL BE az ESP parancstól függetlenül, zöld LED folyamatos BE
    if (serviceMode) {
        setBit(effectiveMask, OUT_BIT_SZIVATTYU, true);
        setBit(effectiveMask, OUT_BIT_LED_ZOLD, true);
    }

    // Piros LED (A2): 500ms villogás, ha riasztás aktív és alarmEnabled=true (szerviz módban is a riasztás szerint)
    bool pirosOn = false;
    if (currentAlarmConfig.alarmEnabled == 0) {
        alarmBlinkOn = false;
        pirosOn = false;
    } else if (currentAlarmState.active != 0 || !hasEspRequest || failsafeActive) {
        // OVERFLOW-SAFE: uint32_t kivonás, 49 napos millis túlcsorduláskor is helyes.
        if (static_cast<uint32_t>(now - lastAlarmBlinkMs) >= ALARM_BLINK_INTERVAL_MS) {
            lastAlarmBlinkMs = now;
            alarmBlinkOn = !alarmBlinkOn;
        }
        pirosOn = alarmBlinkOn;
    } else {
        alarmBlinkOn = false;
        pirosOn = getBit(effectiveMask, OUT_BIT_LED_PIROS);
    }

    setBit(effectiveMask, OUT_BIT_LED_PIROS, pirosOn);
    applyOutputs(effectiveMask);
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
    Serial.println(F("\n=== DS18B20 ADDRESS-KIIRO MOD ==="));
    sensors.begin();
    uint8_t count = sensors.getDeviceCount();
    Serial.print(F("Talalt szenzorok szama: "));
    Serial.println(count);

    sensors.setWaitForConversion(true);
    sensors.requestTemperatures();

    DeviceAddress foundAddr;
    for (uint8_t i = 0; i < count; i++) {
        if (sensors.getAddress(foundAddr, i)) {
            float tempC = sensors.getTempC(foundAddr);
            Serial.print(F("  ["));
            Serial.print(i);
            Serial.print(F("] Cim: "));
            printDeviceAddressCArray(foundAddr);
            Serial.print(F("  | Homerseklet: "));
            if (tempC <= -120.0f) {
                Serial.println(F("HIBA (-127)"));
            } else {
                Serial.print(tempC, 2);
                Serial.println(F(" C"));
            }
        }
    }
    sensors.setWaitForConversion(false);
    Serial.println(F("Masold be a cimeket az ADDR_* tombokbe! (Ujraolvasas: kuldj 'a' karaktert)"));
    Serial.println(F("=================================\n"));
}

// ============================================================================
// DS18B20 OLVASÁS (-127 ESETÉN -127.0f)
// ============================================================================
float readSingleSensor(const DeviceAddress addr) {
    float t = sensors.getTempC(addr);
    if (t <= -120.0f || isnan(t) || t > 125.0f) {
        return SENSOR_DISCONNECTED_VAL;
    }
    return t;
}

void updateAllTemperatures() {
    latestEloremeno  = readSingleSensor(ADDR_ELOREMENO);
    latestVisszatero = readSingleSensor(ADDR_VISSZATERO);
    latestKazan      = readSingleSensor(ADDR_KAZAN);
    latestKulteri    = readSingleSensor(ADDR_KULTERI);

    uint8_t validMask = 0;
    setBit(validMask, 0, latestEloremeno  > -120.0f);
    setBit(validMask, 1, latestVisszatero > -120.0f);
    setBit(validMask, 2, latestKazan      > -120.0f);
    setBit(validMask, 3, latestKulteri    > -120.0f);
    latestSensorValidMask = validMask;

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
    lastResponsePacket.nano.alarmState      = currentAlarmState;

    uint8_t sFlags = 0;
    if (hasTimeSync) sFlags |= SYNC_FLAG_ACK;
    if (syncStale)   sFlags |= SYNC_FLAG_STALE;
    sFlags = static_cast<uint8_t>((sFlags & ~SYNC_FLAG_SERVICE_MODE) |
                                  (serviceMode ? SYNC_FLAG_SERVICE_MODE : 0U));
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

void handleSerialCommands() {
    while (Serial.available() > 0) {
        char c = static_cast<char>(Serial.read());
        if (c == 'a' || c == 'A') {
            runAddressScanner();
        }
    }
}

// ============================================================================
// SETUP & LOOP
// ============================================================================
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println(F("\n[NANO V5] Kazan vezerlo SLAVE inditas..."));
    Serial.println(F("FIGYELEM: Csereld le az ADDR_* tomboket a scanner altal kiirt valos cimekre, kulonben minden szenzor -127 lesz!"));

    pinMode(PIN_LED_ZOLD, OUTPUT);
    pinMode(PIN_SERVICE_SWITCH, INPUT_PULLUP);
    pinMode(PIN_LED_PIROS, OUTPUT);
    pinMode(PIN_SZIVATTYU, OUTPUT);
    pinMode(PIN_TARTALEK1, OUTPUT);
    pinMode(PIN_TARTALEK2, OUTPUT);
    pinMode(PIN_TARTALEK3, OUTPUT); // D10 OUTPUT marad (AVR SPI Master)
    pinMode(PIN_TARTALEK4, OUTPUT);
    pinMode(PIN_TARTALEK5, OUTPUT);

    // Kezdeti szerviz kapcsoló állapot beolvasása induláskor
    serviceRawReading = (digitalRead(PIN_SERVICE_SWITCH) == LOW);
    serviceMode       = serviceRawReading;
    serviceDebounceMs = millis();
    if (serviceMode) {
        Serial.println(F("SERVICE_MODE_ON"));
    }

    // Indulási biztonságos alapállapot: amíg nincs ESP kapcsolat, szivattyú BE + piros LED villog
    failsafeActive = true;
    hasEspRequest = false;
    lastAlarmBlinkMs = millis();
    alarmBlinkOn = true;
    refreshHardwareOutputs(lastAlarmBlinkMs);
    Serial.println(F("[NANO] Indulasi biztonsagi allapot: Szivattyu BE, Piros LED villog."));

    runAddressScanner();
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
}

void loop() {
    uint32_t now = millis();
    handleSerialCommands();
    updateServiceSwitch(now);

    // 1. Nem-blokkoló DS18B20 mérés 10 másodpercenként (ne blokkolja az RF24 vételt!)
    // OVERFLOW-SAFE: uint32_t kivonás, 49 napos millis túlcsorduláskor is helyes.
    if (!conversionRequested && (static_cast<uint32_t>(now - lastTempCycleMs) >= TEMP_READ_INTERVAL_MS)) {
        sensors.requestTemperatures();
        conversionRequested = true;
        conversionReqMs = now;
    }

    // OVERFLOW-SAFE: uint32_t kivonás, 49 napos millis túlcsorduláskor is helyes.
    if (conversionRequested && (static_cast<uint32_t>(now - conversionReqMs) >= DS18B20_CONV_TIME_MS)) {
        conversionRequested = false;
        lastTempCycleMs = now;
        updateAllTemperatures();
    }

    // 2. Master (ESP32-C3) REQUEST fogadása (0xA1) és azonnali RESPONSE küldése (0xB2)
    if (radio.available()) {
        while (radio.available()) {
            radio.read(&rxPacket, sizeof(rxPacket));
        }

        if (rxPacket.packetType == PKT_TYPE_REQUEST) {
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

    // 4. Kimenetek és nem-blokkoló 500ms riasztási villogás frissítése
    refreshHardwareOutputs(now);
}
