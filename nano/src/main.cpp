#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "common.h"

// ============================================================================
// ARDUINO NANO - KAZÁN OLDALI VEZÉRLŐ
// ============================================================================
// Bekötés:
//   DS18B20 OneWire busz : D2 (4.7 kOhm pull-up az 5V felé)
//   NRF24L01+ (SoftSPI)  : CE=D3, CSN=D4, SCK=A0(14), MOSI=A1(15), MISO=A2(16)
//   (Megjegyzés: A SoftSPI miatt a D8-D12 lábak teljesen szabadok maradnak
//    a tartalek1-5 kimenetek számára, nem ütköznek a hardveres SPI-val.)
//   Kimenetek:
//     D5 -> led_zold
//     D6 -> led_piros
//     D7 -> szivattyu relé
//     D8 -> tartalek1
//     D9 -> tartalek2
//     D10 -> tartalek3
//     D11 -> tartalek4
//     D12 -> tartalek5
// ============================================================================

// --- Pin definíciók ---
constexpr uint8_t PIN_ONE_WIRE   = 2;
constexpr uint8_t PIN_NRF_CE     = 3;
constexpr uint8_t PIN_NRF_CSN    = 4;

constexpr uint8_t PIN_LED_ZOLD   = 5;
constexpr uint8_t PIN_LED_PIROS  = 6;
constexpr uint8_t PIN_SZIVATTYU  = 7;
constexpr uint8_t PIN_TARTALEK1  = 8;
constexpr uint8_t PIN_TARTALEK2  = 9;
constexpr uint8_t PIN_TARTALEK3  = 10;
constexpr uint8_t PIN_TARTALEK4  = 11;
constexpr uint8_t PIN_TARTALEK5  = 12;

// Relé logika: állítsd true-ra, ha a relémodul LOW jellel húz be (aktív-alacsony)
constexpr bool RELAY_ACTIVE_LOW = false;

// Időzítések (ms)
constexpr uint32_t TEMP_READ_INTERVAL_MS = 10000UL; // 10 mp-enként DS18B20 olvasás
constexpr uint32_t DS18B20_CONV_TIME_MS  = 800UL;   // 12-bit konverziós idő (~750ms)
constexpr uint32_t FAILSAFE_TIMEOUT_MS   = 15000UL; // 15 mp ESP csomag nélkül -> minden KI

// --- DS18B20 szenzor címek (cseréld le a Serial address-kiíró mód alapján!) ---
DeviceAddress ADDR_ELOREMENO  = { 0x28, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 };
DeviceAddress ADDR_VISSZATERO = { 0x28, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02 };
DeviceAddress ADDR_KAZAN      = { 0x28, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03 };
DeviceAddress ADDR_KULTERI    = { 0x28, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04 };

// --- Példányok ---
OneWire oneWire(PIN_ONE_WIRE);
DallasTemperature sensors(&oneWire);
RF24 radio(PIN_NRF_CE, PIN_NRF_CSN);

// --- Állapotváltozók ---
NrfPacket txPacket;
NrfPacket rxPacket;

uint32_t lastEspPacketMs = 0;
uint32_t lastTempCycleMs = 0;
bool conversionRequested = false;
uint32_t conversionReqMs = 0;
bool failsafeActive = true;
uint8_t currentOutputsMask = 0;

// ============================================================================
// KIMENETEK KEZELÉSE
// ============================================================================
void writeRelayPin(uint8_t pin, bool on) {
    if (RELAY_ACTIVE_LOW) {
        digitalWrite(pin, on ? LOW : HIGH);
    } else {
        digitalWrite(pin, on ? HIGH : LOW);
    }
}

void applyOutputs(uint8_t mask) {
    currentOutputsMask = mask;
    writeRelayPin(PIN_SZIVATTYU, getBit(mask, OUT_BIT_SZIVATTYU));
    digitalWrite(PIN_LED_ZOLD,   getBit(mask, OUT_BIT_LED_ZOLD)  ? HIGH : LOW);
    digitalWrite(PIN_LED_PIROS,  getBit(mask, OUT_BIT_LED_PIROS) ? HIGH : LOW);
    digitalWrite(PIN_TARTALEK1,  getBit(mask, OUT_BIT_TARTALEK1) ? HIGH : LOW);
    digitalWrite(PIN_TARTALEK2,  getBit(mask, OUT_BIT_TARTALEK2) ? HIGH : LOW);
    digitalWrite(PIN_TARTALEK3,  getBit(mask, OUT_BIT_TARTALEK3) ? HIGH : LOW);
    digitalWrite(PIN_TARTALEK4,  getBit(mask, OUT_BIT_TARTALEK4) ? HIGH : LOW);
    digitalWrite(PIN_TARTALEK5,  getBit(mask, OUT_BIT_TARTALEK5) ? HIGH : LOW);
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
                Serial.println(F("HIBA (-127 / null)"));
            } else {
                Serial.print(tempC, 2);
                Serial.println(F(" C"));
            }
        }
    }
    sensors.setWaitForConversion(false);
    Serial.println(F("Masold be a cimeket a ADDR_* tombokbe! (Ujraolvasas: kuldj 'a' karaktert Serial-on)"));
    Serial.println(F("=================================\n"));
}

// ============================================================================
// HŐMÉRSÉKLET OLVASÁS (-127 -> NAN / NULL JELZÉS)
// ============================================================================
float readSingleSensor(const DeviceAddress addr, uint8_t validBit, uint8_t& validMask) {
    float t = sensors.getTempC(addr);
    // DEVICE_DISCONNECTED_C == -127.0f
    if (t <= -120.0f || isnan(t) || t > 125.0f) {
        setBit(validMask, validBit, false);
        return NAN;
    }
    setBit(validMask, validBit, true);
    return t;
}

void updateAllTemperatures() {
    uint8_t validMask = 0;
    txPacket.eloremeno  = readSingleSensor(ADDR_ELOREMENO,  SENS_VALID_ELOREMENO,  validMask);
    txPacket.visszatero = readSingleSensor(ADDR_VISSZATERO, SENS_VALID_VISSZATERO, validMask);
    txPacket.kazan      = readSingleSensor(ADDR_KAZAN,      SENS_VALID_KAZAN,      validMask);
    txPacket.kulteri    = readSingleSensor(ADDR_KULTERI,    SENS_VALID_KULTERI,    validMask);
    txPacket.sensorValidMask = validMask;
    txPacket.kazan_status    = 0; // Egyelőre 0-k (bit0-7 tartalék)
    txPacket.nrf24_ok        = failsafeActive ? 0 : 1;

    Serial.print(F("[TEMP] Elore:"));
    if (isnan(txPacket.eloremeno)) Serial.print(F("null")); else Serial.print(txPacket.eloremeno, 1);
    Serial.print(F(" Vissza:"));
    if (isnan(txPacket.visszatero)) Serial.print(F("null")); else Serial.print(txPacket.visszatero, 1);
    Serial.print(F(" Kazan:"));
    if (isnan(txPacket.kazan)) Serial.print(F("null")); else Serial.print(txPacket.kazan, 1);
    Serial.print(F(" Kulteri:"));
    if (isnan(txPacket.kulteri)) Serial.print(F("null")); else Serial.print(txPacket.kulteri, 1);
    Serial.println();
}

void reloadAckPayload() {
    txPacket.packetType = PKT_TYPE_NANO_TO_ESP;
    txPacket.outputsMask = currentOutputsMask;
    txPacket.nrf24_ok = failsafeActive ? 0 : 1;
    radio.flush_tx();
    radio.writeAckPayload(1, &txPacket, sizeof(txPacket));
}

// ============================================================================
// SETUP & LOOP
// ============================================================================
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println(F("\n[NANO] Kazan vezerlo inditas..."));

    // Kimenetek alapállapotba (minden KI)
    pinMode(PIN_LED_ZOLD, OUTPUT);
    pinMode(PIN_LED_PIROS, OUTPUT);
    pinMode(PIN_SZIVATTYU, OUTPUT);
    pinMode(PIN_TARTALEK1, OUTPUT);
    pinMode(PIN_TARTALEK2, OUTPUT);
    pinMode(PIN_TARTALEK3, OUTPUT);
    pinMode(PIN_TARTALEK4, OUTPUT);
    pinMode(PIN_TARTALEK5, OUTPUT);
    applyOutputs(0);

    // Kezdeti csomag inicializálás
    memset(&txPacket, 0, sizeof(txPacket));
    txPacket.packetType = PKT_TYPE_NANO_TO_ESP;
    txPacket.eloremeno = NAN;
    txPacket.visszatero = NAN;
    txPacket.kazan = NAN;
    txPacket.kulteri = NAN;

    // DS18B20 inicializálás + address-kiíró mód futtatása induláskor
    runAddressScanner();
    sensors.setResolution(12);
    sensors.setWaitForConversion(true);
    sensors.requestTemperatures();
    updateAllTemperatures();
    sensors.setWaitForConversion(false);
    lastTempCycleMs = millis();

    // NRF24L01+ inicializálás
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
}

void handleSerialCommands() {
    while (Serial.available() > 0) {
        char c = static_cast<char>(Serial.read());
        if (c == 'a' || c == 'A' || c == 's' || c == 'S') {
            runAddressScanner();
        }
    }
}

void loop() {
    uint32_t now = millis();

    handleSerialCommands();

    // 1. Nem-blokkoló DS18B20 mérés 10 másodpercenként
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
            lastEspPacketMs = now;
            if (failsafeActive) {
                failsafeActive = false;
                Serial.println(F("[NANO] ESP kapcsolat helyreallt, fail-safe feloldva."));
            }

            applyOutputs(rxPacket.outputsMask);
            txPacket.counter++;
            reloadAckPayload();
        }
    }

    // 3. Fail-safe: ha 15 mp-ig nem jön ESP csomag, kapcsoljon KI mindent
    if (!failsafeActive && (now - lastEspPacketMs >= FAILSAFE_TIMEOUT_MS)) {
        failsafeActive = true;
        applyOutputs(0);
        reloadAckPayload();
        Serial.println(F("[NANO] FAIL-SAFE AKTIV: 15mp-ig nem jott ESP csomag! Minden kimenet KI."));
    }
}
