#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "common.h"

// ============================================================================
// ARDUINO NANO - KAZÁN OLDALI VEZÉRLŐ (V4)
// ============================================================================
// Bekötés (ütközésmentes Hardware SPI kiosztás):
//   DS18B20 OneWire busz    : D2 (4.7 kOhm pull-up az 5V felé)
//   NRF24L01+ (Hardware SPI): CE=D3, CSN=D4, MOSI=D11, MISO=D12, SCK=D13
//   Kimenetek:
//     D5 -> led_zold
//     D6 -> led_piros
//     D7 -> szivattyu relé
//     D8 -> tartalek1
//     D9 -> tartalek2
//     D10 -> tartalek3 (OUTPUT módban tartva, hogy az AVR Hardware SPI Master maradjon)
//     A3 (17) -> tartalek4 (D11 szabadon hagyva MOSI-nak)
//     A4 (18) -> tartalek5 (D12 szabadon hagyva MISO-nak)
//
// FAIL-SAFE (V4):
//   Induláskor és ha 15 mp-ig nem érkezik ESP csomag, a Nano BEKAPCSOLJA a
//   szivattyút (D7) és a piros LED-et (D6) a kazán túlforrásának megelőzésére!
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

constexpr uint32_t TEMP_READ_INTERVAL_MS = 10000UL; // 10 mp-enként DS18B20 olvasás
constexpr uint32_t DS18B20_CONV_TIME_MS  = 800UL;   // 12-bit konverziós idő (~750ms)
constexpr uint32_t FAILSAFE_TIMEOUT_MS   = 15000UL; // 15 mp timeout -> szivattyú + piros LED BE

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

NrfPacket txPacket;
NrfPacket rxPacket;

uint32_t lastEspPacketMs = 0;
uint32_t lastTempCycleMs = 0;
bool conversionRequested = false;
uint32_t conversionReqMs = 0;
bool failsafeActive = false;

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

    // Induláskor biztonságos állapot: amíg nincs ESP kapcsolat, szivattyú BE + piros LED BE
    uint8_t bootSafeMask = 0;
    setBit(bootSafeMask, OUT_BIT_SZIVATTYU, true);
    setBit(bootSafeMask, OUT_BIT_LED_PIROS, true);
    applyOutputs(bootSafeMask);
    Serial.println(F("[NANO] Indulasi biztonsagi allapot: Szivattyu BE, Piros LED BE (varakozas ESP-re)."));

    memset(&txPacket, 0, sizeof(txPacket));
    txPacket.packetType      = PKT_TYPE_NANO_TO_ESP;
    txPacket.nano.eloremeno  = SENSOR_DISCONNECTED_VAL;
    txPacket.nano.visszatero = SENSOR_DISCONNECTED_VAL;
    txPacket.nano.kazan      = SENSOR_DISCONNECTED_VAL;
    txPacket.nano.kulteri    = SENSOR_DISCONNECTED_VAL;

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

    // 2. RF24 csomag fogadása az ESP32-C3-tól és kimenetek végrehajtása
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

            applyOutputs(rxPacket.esp.outputsMask);
            txPacket.nano.counter++;
            reloadAckPayload();
        }
    }

    // 3. V4 Fail-safe: ha 15 mp-ig nem jön ESP csomag -> szivattyú BE + piros LED BE
    if (!failsafeActive && (now - lastEspPacketMs >= FAILSAFE_TIMEOUT_MS)) {
        failsafeActive = true;
        uint8_t fsMask = 0;
        setBit(fsMask, OUT_BIT_SZIVATTYU, true);
        setBit(fsMask, OUT_BIT_LED_PIROS, true);
        applyOutputs(fsMask);
        Serial.println(F("[NANO] FAIL-SAFE AKTIV: Szivattyu BE, Piros LED BE"));
    }
}
