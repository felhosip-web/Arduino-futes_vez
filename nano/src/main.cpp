#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "common.h"

// ============================================================================
// ARDUINO NANO - KAZÁN OLDALI VEZÉRLŐ (V4 + RIASZTÁSI RENDSZER)
// ============================================================================
// Bekötés (ütközésmentes Hardware SPI kiosztás):
//   DS18B20 OneWire busz    : D2 (4.7 kOhm pull-up az 5V felé)
//   NRF24L01+ (Hardware SPI): CE=D3, CSN=D4, MOSI=D11, MISO=D12, SCK=D13
//   Kimenetek:
//     D5 -> led_zold
//     D6 -> led_piros (aktív riasztás esetén 500ms BE / 500ms KI millis()-el villog)
//     D7 -> szivattyu relé
//     D8 -> tartalek1
//     D9 -> tartalek2
//     D10 -> tartalek3 (OUTPUT módban tartva, hogy az AVR Hardware SPI Master maradjon)
//     A3 (17) -> tartalek4 (D11 szabadon hagyva MOSI-nak)
//     A4 (18) -> tartalek5 (D12 szabadon hagyva MISO-nak)
//
// FAIL-SAFE & ÖNÁLLÓ BIZTONSÁG (V4):
//   - Induláskor és ha 15 mp-ig nem érkezik ESP csomag, a Nano BEKAPCSOLJA a
//     szivattyút (D7), és alarmEnabled=true esetén villogtatja a piros LED-et (reason=nrf_fail).
//   - Ha a saját mért kazan >= alarmConfig.overheatTemp vagy szenzorhiba (-127),
//     a Nano önállóan BEKAPCSOLJA a szivattyút (D7) és villogtatja a piros LED-et (D6).
// ============================================================================

constexpr uint8_t PIN_ONE_WIRE   = 2;
constexpr uint8_t PIN_NRF_CE     = 3;
constexpr uint8_t PIN_NRF_CSN    = 4;

constexpr uint8_t PIN_LED_ZOLD   = 5;
constexpr uint8_t PIN_LED_PIROS  = 6;
constexpr uint8_t PIN_SZIVATTYU  = 7;
constexpr uint8_t PIN_TARTALEK1  = 8;
constexpr uint8_t PIN_TARTALEK2  = 9;
constexpr uint8_t PIN_TARTALEK3  = 10;
constexpr uint8_t PIN_TARTALEK4  = 17; // A3 (digitális kimenet, nincs SPI ütközés)
constexpr uint8_t PIN_TARTALEK5  = 18; // A4 (digitális kimenet, nincs SPI ütközés)

// Relé logika: állítsd true-ra, ha a relémodul LOW jellel húz be
constexpr bool RELAY_ACTIVE_LOW = false;

constexpr uint32_t TEMP_READ_INTERVAL_MS   = 10000UL; // 10 mp-enként DS18B20 olvasás
constexpr uint32_t DS18B20_CONV_TIME_MS    = 800UL;   // 12-bit konverziós idő (~750ms)
constexpr uint32_t FAILSAFE_TIMEOUT_MS     = 15000UL; // 15 mp timeout -> szivattyú BE + nrf_fail
constexpr uint32_t ALARM_BLINK_INTERVAL_MS = 500UL;   // 500ms BE / 500ms KI villogás

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

PumpConfigPacked lastPumpConfig = {
    PUMP_MODE_AUTO,
    40.0f,
    30.0f,
    PUMP_SENSOR_KAZAN
};
bool hasPumpConfig = false;

AlarmConfigPacked currentAlarmConfig = {
    ALARM_OVERHEAT_DEFAULT,
    1 // alapértelmezett: alarmEnabled = true
};

AlarmStatePacked currentAlarmState = {
    0,
    ALARM_REASON_NONE
};

NrfPacket txPacket;
NrfPacket rxPacket;

uint8_t  lastEspOutputsMask = 0;
bool     hasEspPacket       = false;
uint32_t lastEspPacketMs    = 0;
uint32_t lastTempCycleMs    = 0;
bool     conversionRequested = false;
uint32_t conversionReqMs    = 0;
bool     failsafeActive     = false;

uint32_t lastAlarmBlinkMs   = 0;
bool     alarmBlinkOn       = false;

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
    if (isnan(cfg.overheatTemp)) {
        cfg.overheatTemp = ALARM_OVERHEAT_DEFAULT;
    }
    if (cfg.overheatTemp < ALARM_OVERHEAT_MIN) cfg.overheatTemp = ALARM_OVERHEAT_MIN;
    if (cfg.overheatTemp > ALARM_OVERHEAT_MAX) cfg.overheatTemp = ALARM_OVERHEAT_MAX;
    cfg.alarmEnabled = cfg.alarmEnabled ? 1 : 0;
}

void evaluateLocalSafetyAndAlarm() {
    float kazan = txPacket.nano.kazan;
    bool kazanInvalid = isnan(kazan) || (kazan <= -120.0f) || (kazan > 125.0f);
    bool overheat = !kazanInvalid && (kazan >= currentAlarmConfig.overheatTemp);

    if (currentAlarmConfig.alarmEnabled == 0) {
        currentAlarmState.active = 0;
        currentAlarmState.reason = ALARM_REASON_NONE;
    } else if (overheat) {
        currentAlarmState.active = 1;
        currentAlarmState.reason = ALARM_REASON_OVERHEAT;
    } else if (kazanInvalid) {
        currentAlarmState.active = 1;
        currentAlarmState.reason = ALARM_REASON_SENSOR_FAIL;
    } else if (failsafeActive) {
        currentAlarmState.active = 1;
        currentAlarmState.reason = ALARM_REASON_NRF_FAIL;
    } else {
        currentAlarmState.active = 0;
        currentAlarmState.reason = ALARM_REASON_NONE;
    }

    txPacket.nano.alarmState = currentAlarmState;
}

// Kiszámítja a tényleges kimeneteket (szivattyú biztonság + nem-blokkoló 500ms piros LED villogás)
void refreshHardwareOutputs(uint32_t now) {
    float kazan = txPacket.nano.kazan;
    bool kazanInvalid = isnan(kazan) || (kazan <= -120.0f) || (kazan > 125.0f);
    bool overheat = !kazanInvalid && (kazan >= currentAlarmConfig.overheatTemp);

    uint8_t effectiveMask = 0;
    if (!hasEspPacket || failsafeActive) {
        setBit(effectiveMask, OUT_BIT_SZIVATTYU, true);
    } else {
        effectiveMask = lastEspOutputsMask;
    }

    // Szivattyú biztonsági szabály: túlmelegedés vagy szenzorhiba esetén MINDIG BE
    if (overheat || kazanInvalid) {
        setBit(effectiveMask, OUT_BIT_SZIVATTYU, true);
    }

    // Piros LED (D6) vezérlése:
    // - Ha alarmEnabled == false -> nincs riasztás/villogás, simán KI
    // - Ha alarmState.active == true -> 500ms BE / 500ms KI millis() villogás
    // - Különben ha még nincs ESP kapcsolat -> világít jelzésként, vagy ESP kimeneti bit szerint
    bool pirosOn = false;
    if (currentAlarmConfig.alarmEnabled == 0) {
        alarmBlinkOn = false;
        pirosOn = false;
    } else if (currentAlarmState.active != 0) {
        if (now - lastAlarmBlinkMs >= ALARM_BLINK_INTERVAL_MS) {
            lastAlarmBlinkMs = now;
            alarmBlinkOn = !alarmBlinkOn;
        }
        pirosOn = alarmBlinkOn;
    } else {
        alarmBlinkOn = false;
        pirosOn = !hasEspPacket ? true : getBit(effectiveMask, OUT_BIT_LED_PIROS);
    }

    setBit(effectiveMask, OUT_BIT_LED_PIROS, pirosOn);
    applyOutputs(effectiveMask);
}

// ============================================================================
// DS18B20 ADDRESS-KIÍRÓ MÓD (SERIAL)
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
// HŐMÉRSÉKLET OLVASÁS (-127 ESETÉN -127.0f-ET KÜLD)
// ============================================================================
float readSingleSensor(const DeviceAddress addr) {
    float t = sensors.getTempC(addr);
    if (t <= -120.0f || isnan(t) || t > 125.0f) {
        return SENSOR_DISCONNECTED_VAL; // -127.0f
    }
    return t;
}

void updateAllTemperatures() {
    txPacket.nano.eloremeno  = readSingleSensor(ADDR_ELOREMENO);
    txPacket.nano.visszatero = readSingleSensor(ADDR_VISSZATERO);
    txPacket.nano.kazan      = readSingleSensor(ADDR_KAZAN);
    txPacket.nano.kulteri    = readSingleSensor(ADDR_KULTERI);

    uint8_t validMask = 0;
    setBit(validMask, 0, txPacket.nano.eloremeno  > -120.0f);
    setBit(validMask, 1, txPacket.nano.visszatero > -120.0f);
    setBit(validMask, 2, txPacket.nano.kazan      > -120.0f);
    setBit(validMask, 3, txPacket.nano.kulteri    > -120.0f);
    txPacket.nano.sensorValidMask = validMask;

    for (uint8_t i = 0; i < 8; i++) {
        txPacket.nano.kazan_status[i] = 0;
    }

    evaluateLocalSafetyAndAlarm();

    Serial.print(F("[TEMP] Elore:"));
    Serial.print(txPacket.nano.eloremeno, 1);
    Serial.print(F(" Vissza:"));
    Serial.print(txPacket.nano.visszatero, 1);
    Serial.print(F(" Kazan:"));
    Serial.print(txPacket.nano.kazan, 1);
    Serial.print(F(" Kulteri:"));
    Serial.println(txPacket.nano.kulteri, 1);
}

void reloadAckPayload() {
    txPacket.packetType = PKT_TYPE_NANO_TO_ESP;
    txPacket.nano.alarmState = currentAlarmState;
    radio.flush_tx();
    radio.writeAckPayload(1, &txPacket, sizeof(txPacket));
}

void handleSerialCommands() {
    while (Serial.available() > 0) {
        char c = static_cast<char>(Serial.read());
        if (c == 'a' || c == 'A' || c == 's' || c == 'S') {
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
    Serial.println(F("\n[NANO V4] Kazan vezerlo inditas..."));
    Serial.println(F("FIGYELEM: Cseréld le az ADDR_* tömböket a scanner által kiírt valós címekre, különben minden szenzor NAN lesz!"));

    pinMode(PIN_LED_ZOLD, OUTPUT);
    pinMode(PIN_LED_PIROS, OUTPUT);
    pinMode(PIN_SZIVATTYU, OUTPUT);
    pinMode(PIN_TARTALEK1, OUTPUT);
    pinMode(PIN_TARTALEK2, OUTPUT);
    pinMode(PIN_TARTALEK3, OUTPUT);
    pinMode(PIN_TARTALEK4, OUTPUT);
    pinMode(PIN_TARTALEK5, OUTPUT);

    memset(&txPacket, 0, sizeof(txPacket));
    txPacket.packetType      = PKT_TYPE_NANO_TO_ESP;
    txPacket.nano.eloremeno  = SENSOR_DISCONNECTED_VAL;
    txPacket.nano.visszatero = SENSOR_DISCONNECTED_VAL;
    txPacket.nano.kazan      = SENSOR_DISCONNECTED_VAL;
    txPacket.nano.kulteri    = SENSOR_DISCONNECTED_VAL;

    // Induláskor biztonságos állapot: amíg nincs ESP kapcsolat, szivattyú BE + piros LED BE/villog
    uint8_t bootSafeMask = 0;
    setBit(bootSafeMask, OUT_BIT_SZIVATTYU, true);
    setBit(bootSafeMask, OUT_BIT_LED_PIROS, true);
    applyOutputs(bootSafeMask);
    Serial.println(F("[NANO] Indulasi biztonsagi allapot: Szivattyu BE, Piros LED BE (varakozas ESP-re)."));

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
        radio.setChannel(NRF_CHANNEL);
        radio.setDataRate(RF24_250KBPS);
        radio.setPALevel(RF24_PA_HIGH);
        radio.setRetries(6, 15);
        radio.enableDynamicPayloads();
        radio.enableAckPayload();
        radio.openReadingPipe(1, NRF_ADDR_PIPE);
        radio.startListening();
        reloadAckPayload();
        Serial.println(F("[NANO] NRF24L01+ keszenletben (ACK payload mod)."));
    }

    lastEspPacketMs = millis();
    lastAlarmBlinkMs = lastEspPacketMs;
    refreshHardwareOutputs(lastEspPacketMs);
}

void loop() {
    uint32_t now = millis();
    handleSerialCommands();

    // 1. Nem-blokkoló DS18B20 mérés 10 másodpercenként (külön DeviceAddress alapján)
    if (!conversionRequested && (now - lastTempCycleMs >= TEMP_READ_INTERVAL_MS)) {
        sensors.requestTemperatures();
        conversionRequested = true;
        conversionReqMs = now;
    }

    if (conversionRequested && (now - conversionReqMs >= DS18B20_CONV_TIME_MS)) {
        conversionRequested = false;
        lastTempCycleMs = now;
        updateAllTemperatures();
        reloadAckPayload();
    }

    // 2. RF24 csomag fogadása az ESP32-C3-tól
    if (radio.available()) {
        while (radio.available()) {
            radio.read(&rxPacket, sizeof(rxPacket));
        }

        if (rxPacket.packetType == PKT_TYPE_ESP_TO_NANO) {
            hasEspPacket = true;
            lastEspPacketMs = now;
            if (failsafeActive) {
                failsafeActive = false;
                Serial.println(F("[NANO] ESP kapcsolat helyreallt, fail-safe feloldva."));
            }

            lastPumpConfig = rxPacket.esp.pumpConfig;
            hasPumpConfig = true;
            Serial.print(F("[NANO] PumpConfig: mode="));
            Serial.print(lastPumpConfig.pumpMode);
            Serial.print(F(" start="));
            Serial.print(lastPumpConfig.pumpStartTemp, 1);
            Serial.print(F(" stop="));
            Serial.print(lastPumpConfig.pumpStopTemp, 1);
            Serial.print(F(" sensor="));
            Serial.println(lastPumpConfig.pumpStartSensor);

            currentAlarmConfig = rxPacket.esp.alarmConfig;
            sanitizeAlarmConfig(currentAlarmConfig);
            Serial.print(F("[NANO] AlarmConfig: enabled="));
            Serial.print(currentAlarmConfig.alarmEnabled);
            Serial.print(F(" overheat="));
            Serial.println(currentAlarmConfig.overheatTemp, 1);

            lastEspOutputsMask = rxPacket.esp.outputsMask;
            evaluateLocalSafetyAndAlarm();
            txPacket.nano.counter++;
            reloadAckPayload();
        }
    }

    // 3. V4 Fail-safe: ha 15 mp-ig nem jön ESP csomag -> szivattyú BE + nrf_fail riasztás
    if (!failsafeActive && (now - lastEspPacketMs >= FAILSAFE_TIMEOUT_MS)) {
        failsafeActive = true;
        evaluateLocalSafetyAndAlarm();
        reloadAckPayload();
        Serial.println(F("[NANO] FAIL-SAFE AKTIV: Szivattyu BE, Piros LED BE/villog (nrf_fail)"));
    }

    // 4. Kimenetek és nem-blokkoló 500ms riasztási villogás frissítése minden ciklusban
    refreshHardwareOutputs(now);
}
