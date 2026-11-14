#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <RF24.h>
#include <math.h>

#include "config.h"
#include "lcd.h"

#if __has_include("../../nano/src/common.h")
#include "../../nano/src/common.h"
#else
#include <stdint.h>
constexpr uint8_t PKT_TYPE_ESP_TO_NANO = 0xA1;
constexpr uint8_t PKT_TYPE_NANO_TO_ESP = 0xB2;
enum HeatingMode : uint8_t { MODE_AUTO = 0, MODE_MANUAL = 1, MODE_OFF = 2 };
constexpr uint8_t OUT_BIT_SZIVATTYU = 0;
constexpr uint8_t OUT_BIT_LED_ZOLD  = 1;
constexpr uint8_t OUT_BIT_LED_PIROS = 2;
constexpr uint8_t OUT_BIT_TARTALEK1 = 3;
constexpr uint8_t OUT_BIT_TARTALEK2 = 4;
constexpr uint8_t OUT_BIT_TARTALEK3 = 5;
constexpr uint8_t OUT_BIT_TARTALEK4 = 6;
constexpr uint8_t OUT_BIT_TARTALEK5 = 7;
constexpr uint8_t SENS_VALID_ELOREMENO  = 0;
constexpr uint8_t SENS_VALID_VISSZATERO = 1;
constexpr uint8_t SENS_VALID_KAZAN      = 2;
constexpr uint8_t SENS_VALID_KULTERI    = 3;
struct __attribute__((packed)) NrfPacket {
    uint8_t packetType;
    uint8_t outputsMask;
    float targetTemp;
    uint8_t mode;
    uint8_t manualHeating;
    float eloremeno;
    float visszatero;
    float kazan;
    float kulteri;
    uint8_t kazan_status;
    uint8_t sensorValidMask;
    uint8_t nrf24_ok;
    uint8_t reserved;
    uint32_t counter;
};
static_assert(sizeof(NrfPacket) == 32, "NrfPacket must be 32 bytes");
constexpr uint8_t NRF_CHANNEL = 108;
constexpr uint8_t NRF_ADDR_PIPE[6] = "FUTES";
inline bool getBit(uint8_t mask, uint8_t bitIndex) { return (mask & (1U << bitIndex)) != 0; }
inline void setBit(uint8_t& mask, uint8_t bitIndex, bool value) {
    if (value) mask |= static_cast<uint8_t>(1U << bitIndex);
    else mask &= static_cast<uint8_t>(~(1U << bitIndex));
}
#endif

// ============================================================================
// ESP32-C3 - SZOBA OLDALI TERMOSZTÁT ÉS MQTT GATEWAY
// ============================================================================

// --- Globális példányok ---
SPIClass spiNrf(FSPI);
RF24 radio(PIN_NRF_CE, PIN_NRF_CSN);
HeatingLcd lcd;

WiFiClientSecure wifiSecureClient;
PubSubClient mqtt(wifiSecureClient);

// --- Rendszer állapot ---
struct SystemState {
    float currentTemp   = 20.0f;
    float targetTemp    = TARGET_TEMP_DEFAULT;
    bool  heatingOn     = false;
    uint8_t mode        = MODE_AUTO;
    bool  manualHeating = false;

    uint8_t outputsMask = 0; // bit0..7 (szivattyu..tartalek5)

    // Nano felől érkező adatok
    float eloremeno     = NAN;
    float visszatero    = NAN;
    float kazan         = NAN;
    float kulteri       = NAN;
    uint8_t kazanStatus = 0;
    uint8_t sensorValidMask = 0;
    bool  nrf24Ok       = false;
    uint32_t nanoCounter = 0;
} state;

// --- Időzítők ---
uint32_t lastLm35Ms      = 0;
uint32_t lastNrfPingMs   = 0;
uint32_t lastNrfAckMs    = 0;
uint32_t lastTelemetryMs = 0;
uint32_t lastWifiTryMs   = 0;
uint32_t lastMqttTryMs   = 0;
bool     mqttConnectedOnce = false;

// --- Rotary Encoder ISR állapot (Gray-code tábla + debounce) ---
volatile int8_t  encStepAccum = 0;
volatile uint8_t encPrevState = 0;
volatile bool    btnPressedEvent = false;
volatile uint32_t lastBtnIsrMs = 0;

// Quadrature state lookup table (0 = no change, +1 = CW, -1 = CCW)
static const int8_t QEM_TABLE[16] = {
     0, -1,  1,  0,
     1,  0,  0, -1,
    -1,  0,  0,  1,
     0,  1, -1,  0
};
volatile int8_t encQuarterAccum = 0;

void IRAM_ATTR isrEncoderRotate() {
    uint8_t msb = static_cast<uint8_t>(digitalRead(PIN_ENC_CLK));
    uint8_t lsb = static_cast<uint8_t>(digitalRead(PIN_ENC_DT));
    uint8_t currState = (msb << 1) | lsb;
    uint8_t idx = (encPrevState << 2) | currState;
    encPrevState = currState;

    int8_t delta = QEM_TABLE[idx & 0x0F];
    if (delta != 0) {
        encQuarterAccum += delta;
        if (encQuarterAccum >= 4) {
            encStepAccum++;
            encQuarterAccum = 0;
        } else if (encQuarterAccum <= -4) {
            encStepAccum--;
            encQuarterAccum = 0;
        }
    }
}

void IRAM_ATTR isrEncoderButton() {
    uint32_t now = millis();
    if (now - lastBtnIsrMs >= BTN_DEBOUNCE_MS) {
        if (digitalRead(PIN_ENC_SW) == LOW) {
            btnPressedEvent = true;
            lastBtnIsrMs = now;
        }
    }
}

// ============================================================================
// SEGÉDFÜGGVÉNYEK
// ============================================================================
const char* modeToStr(uint8_t mode) {
    switch (mode) {
        case MODE_AUTO:   return "auto";
        case MODE_MANUAL: return "manual";
        case MODE_OFF:    return "off";
        default:          return "auto";
    }
}

uint8_t strToMode(const char* s) {
    if (!s) return MODE_AUTO;
    if (strcmp(s, "manual") == 0) return MODE_MANUAL;
    if (strcmp(s, "off") == 0)    return MODE_OFF;
    return MODE_AUTO;
}

float round1(float v) {
    return roundf(v * 10.0f) / 10.0f;
}

// ============================================================================
// LM35 SZOBAI HŐMÉRSÉKLET OLVASÁSA (10 MÉRÉS ÁTLAGA)
// ============================================================================
float readRoomTempLM35() {
    uint32_t sumMv = 0;
    for (uint8_t i = 0; i < LM35_SAMPLE_COUNT; i++) {
        sumMv += analogReadMilliVolts(PIN_LM35);
    }
    float avgMv = static_cast<float>(sumMv) / static_cast<float>(LM35_SAMPLE_COUNT);
    // Map: 0-3300mV = 0-100°C + kalibrációs offset
    float tempC = (avgMv / 3300.0f) * 100.0f + LM35_OFFSET;
    return round1(tempC);
}

// ============================================================================
// FŰTÉSVEZÉRLÉSI LOGIKA (AZ ESP HOZZA A DÖNTÉST, A NANO VÉGREHAJTJA)
// ============================================================================
void evaluateControlLogic() {
    if (state.mode == MODE_OFF) {
        state.heatingOn = false;
        state.outputsMask = 0; // Minden kimenet KI
        return;
    }

    if (state.mode == MODE_AUTO) {
        // Hiszterézis 0.5°C: BE ha currentTemp < target - 0.3, KI ha > target + 0.3
        if (state.currentTemp < (state.targetTemp - HYST_LOW_OFFSET)) {
            state.heatingOn = true;
        } else if (state.currentTemp > (state.targetTemp + HYST_HIGH_OFFSET)) {
            state.heatingOn = false;
        }

        setBit(state.outputsMask, OUT_BIT_SZIVATTYU, state.heatingOn);
        setBit(state.outputsMask, OUT_BIT_LED_ZOLD,  state.heatingOn);
        setBit(state.outputsMask, OUT_BIT_LED_PIROS, false);
        // A tartalek1-5 bitek változatlanok maradnak AUTO módban
        return;
    }

    if (state.mode == MODE_MANUAL) {
        state.heatingOn = state.manualHeating;
        setBit(state.outputsMask, OUT_BIT_SZIVATTYU, state.manualHeating);
        setBit(state.outputsMask, OUT_BIT_LED_ZOLD,  state.manualHeating);
        setBit(state.outputsMask, OUT_BIT_LED_PIROS, false);
    }
}

// ============================================================================
// NRF24 KOMMUNIKÁCIÓ A NANO FELÉ (32 BYTE STRUCT + ACK PAYLOAD)
// ============================================================================
bool sendToNanoAndReadAck() {
    NrfPacket tx{};
    tx.packetType    = PKT_TYPE_ESP_TO_NANO;
    tx.outputsMask   = state.outputsMask;
    tx.targetTemp    = state.targetTemp;
    tx.mode          = state.mode;
    tx.manualHeating = state.manualHeating ? 1 : 0;

    bool ok = radio.write(&tx, sizeof(tx));
    if (ok) {
        lastNrfAckMs = millis();
        if (!state.nrf24Ok) {
            state.nrf24Ok = true;
            Serial.println(F("[ESP] NRF24 kapcsolat helyreallt."));
        }

        while (radio.isAckPayloadAvailable()) {
            NrfPacket rx{};
            radio.read(&rx, sizeof(rx));
            if (rx.packetType == PKT_TYPE_NANO_TO_ESP) {
                state.sensorValidMask = rx.sensorValidMask;
                state.eloremeno       = getBit(rx.sensorValidMask, SENS_VALID_ELOREMENO)  ? rx.eloremeno  : NAN;
                state.visszatero      = getBit(rx.sensorValidMask, SENS_VALID_VISSZATERO) ? rx.visszatero : NAN;
                state.kazan           = getBit(rx.sensorValidMask, SENS_VALID_KAZAN)      ? rx.kazan      : NAN;
                state.kulteri         = getBit(rx.sensorValidMask, SENS_VALID_KULTERI)    ? rx.kulteri    : NAN;
                state.kazanStatus     = rx.kazan_status;
                state.nanoCounter     = rx.counter;
            }
        }
    }
    return ok;
}

// ============================================================================
// MQTT JSON STATUS & TELEMETRY PUBLISH
// ============================================================================
void buildStatusJson(JsonDocument& doc) {
    doc["currentTemp"] = round1(state.currentTemp);
    doc["targetTemp"]  = round1(state.targetTemp);
    doc["heatingOn"]   = state.heatingOn;
    doc["mode"]        = modeToStr(state.mode);
    doc["online"]      = true;

    JsonObject temps = doc["homersekletek"].to<JsonObject>();
    if (isnan(state.eloremeno))  temps["eloremeno"]  = nullptr; else temps["eloremeno"]  = round1(state.eloremeno);
    if (isnan(state.visszatero)) temps["visszatero"] = nullptr; else temps["visszatero"] = round1(state.visszatero);
    if (isnan(state.kazan))      temps["kazan"]      = nullptr; else temps["kazan"]      = round1(state.kazan);
    if (isnan(state.kulteri))    temps["kulteri"]    = nullptr; else temps["kulteri"]    = round1(state.kulteri);

    JsonObject outs = doc["kimenetek"].to<JsonObject>();
    outs["szivattyu"] = getBit(state.outputsMask, OUT_BIT_SZIVATTYU);
    outs["led_zold"]  = getBit(state.outputsMask, OUT_BIT_LED_ZOLD);
    outs["led_piros"] = getBit(state.outputsMask, OUT_BIT_LED_PIROS);
    outs["tartalek1"] = getBit(state.outputsMask, OUT_BIT_TARTALEK1);
    outs["tartalek2"] = getBit(state.outputsMask, OUT_BIT_TARTALEK2);
    outs["tartalek3"] = getBit(state.outputsMask, OUT_BIT_TARTALEK3);
    outs["tartalek4"] = getBit(state.outputsMask, OUT_BIT_TARTALEK4);
    outs["tartalek5"] = getBit(state.outputsMask, OUT_BIT_TARTALEK5);

    JsonArray kStat = doc["kazan_status"].to<JsonArray>();
    for (uint8_t i = 0; i < 8; i++) {
        kStat.add(getBit(state.kazanStatus, i) ? 1 : 0);
    }

    doc["nrf24_ok"] = state.nrf24Ok;
    if (state.nrf24Ok) {
        doc["error"] = nullptr;
    } else {
        doc["error"] = "NRF24 kommunikacios hiba (10s timeout)";
    }
}

void publishMqttStatus() {
    if (!mqtt.connected()) return;
    JsonDocument doc;
    buildStatusJson(doc);
    char payload[768];
    size_t n = serializeJson(doc, payload, sizeof(payload));
    if (n > 0) {
        mqtt.publish(TOPIC_STATUS, reinterpret_cast<const uint8_t*>(payload), n, true); // retained = true
    }
}

void publishMqttTelemetry() {
    if (!mqtt.connected()) return;
    JsonDocument doc;
    buildStatusJson(doc);
    doc["nanoCounter"] = state.nanoCounter;
    char payload[768];
    size_t n = serializeJson(doc, payload, sizeof(payload));
    if (n > 0) {
        mqtt.publish(TOPIC_TELEMETRY, reinterpret_cast<const uint8_t*>(payload), n, false);
    }
}

// ============================================================================
// MQTT CMD FELDOLGOZÁS (Frontend -> ESP)
// ============================================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    if (strcmp(topic, TOPIC_CMD) != 0) return;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload, length);
    if (err) {
        Serial.print(F("[MQTT] JSON hiba: "));
        Serial.println(err.c_str());
        return;
    }

    if (doc["targetTemp"].is<float>() || doc["targetTemp"].is<int>()) {
        float t = doc["targetTemp"].as<float>();
        if (t < TARGET_TEMP_MIN) t = TARGET_TEMP_MIN;
        if (t > TARGET_TEMP_MAX) t = TARGET_TEMP_MAX;
        state.targetTemp = roundf(t * 2.0f) / 2.0f;
    }

    if (doc["mode"].is<const char*>()) {
        state.mode = strToMode(doc["mode"].as<const char*>());
    }

    if (doc["manualHeating"].is<bool>()) {
        state.manualHeating = doc["manualHeating"].as<bool>();
    }

    bool hasKimenetek = doc["kimenetek"].is<JsonObject>();
    if (hasKimenetek) {
        JsonObject k = doc["kimenetek"].as<JsonObject>();
        if (k["tartalek1"].is<bool>()) setBit(state.outputsMask, OUT_BIT_TARTALEK1, k["tartalek1"].as<bool>());
        if (k["tartalek2"].is<bool>()) setBit(state.outputsMask, OUT_BIT_TARTALEK2, k["tartalek2"].as<bool>());
        if (k["tartalek3"].is<bool>()) setBit(state.outputsMask, OUT_BIT_TARTALEK3, k["tartalek3"].as<bool>());
        if (k["tartalek4"].is<bool>()) setBit(state.outputsMask, OUT_BIT_TARTALEK4, k["tartalek4"].as<bool>());
        if (k["tartalek5"].is<bool>()) setBit(state.outputsMask, OUT_BIT_TARTALEK5, k["tartalek5"].as<bool>());
    }

    evaluateControlLogic();

    // MANUAL módban a kimenetek objektum felülírhatja a szivattyú/LED biteket is
    if (state.mode == MODE_MANUAL && hasKimenetek) {
        JsonObject k = doc["kimenetek"].as<JsonObject>();
        if (k["szivattyu"].is<bool>()) {
            bool pump = k["szivattyu"].as<bool>();
            setBit(state.outputsMask, OUT_BIT_SZIVATTYU, pump);
            state.manualHeating = pump;
            state.heatingOn = pump;
        }
        if (k["led_zold"].is<bool>())  setBit(state.outputsMask, OUT_BIT_LED_ZOLD,  k["led_zold"].as<bool>());
        if (k["led_piros"].is<bool>()) setBit(state.outputsMask, OUT_BIT_LED_PIROS, k["led_piros"].as<bool>());
    }

    sendToNanoAndReadAck();
    lcd.update(state.currentTemp, state.targetTemp, state.kazan,
               state.heatingOn, state.mode, state.nrf24Ok);
    publishMqttStatus();
}

// ============================================================================
// NEM-BLOKKOLÓ WIFI ÉS MQTT KAPCSOLATKEZELÉS
// ============================================================================
void maintainConnectivity(uint32_t now) {
    if (WiFi.status() != WL_CONNECTED) {
        if (now - lastWifiTryMs >= WIFI_RECONNECT_MS) {
            lastWifiTryMs = now;
            Serial.println(F("[WIFI] Ujracsatlakozas..."));
            WiFi.disconnect(false);
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        }
        return; // Ha nincs WiFi, ne próbálkozzunk MQTT-vel, menjen tovább a lokális szabályzás!
    }

    if (!mqtt.connected()) {
        if (now - lastMqttTryMs >= MQTT_RECONNECT_MS) {
            lastMqttTryMs = now;
            Serial.print(F("[MQTT] Csatlakozas HiveMQ Cloud-hoz... "));
            if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD,
                             TOPIC_STATUS, 1, true,
                             "{\"online\":false,\"error\":\"Gateway offline\"}")) {
                Serial.println(F("SIKERES!"));
                mqtt.subscribe(TOPIC_CMD);
                publishMqttStatus();
                mqttConnectedOnce = true;
            } else {
                Serial.print(F("HIBA, rc="));
                Serial.println(mqtt.state());
            }
        }
    } else {
        mqtt.loop();
    }
}

// ============================================================================
// ENCODER ÉS NYOMÓGOMB KEZELÉS
// ============================================================================
void handleEncoderEvents() {
    int8_t steps = 0;
    bool btnClicked = false;

    noInterrupts();
    if (encStepAccum != 0) {
        steps = encStepAccum;
        encStepAccum = 0;
    }
    if (btnPressedEvent) {
        btnClicked = true;
        btnPressedEvent = false;
    }
    interrupts();

    bool changed = false;

    if (steps != 0) {
        float nextTarget = state.targetTemp + (static_cast<float>(steps) * TARGET_TEMP_STEP);
        if (nextTarget < TARGET_TEMP_MIN) nextTarget = TARGET_TEMP_MIN;
        if (nextTarget > TARGET_TEMP_MAX) nextTarget = TARGET_TEMP_MAX;
        if (fabsf(nextTarget - state.targetTemp) >= 0.01f) {
            state.targetTemp = nextTarget;
            changed = true;
            Serial.print(F("[ENC] Uj celhomerseklet: "));
            Serial.println(state.targetTemp, 1);
        }
    }

    if (btnClicked) {
        // Mód váltás: AUTO -> MANUAL -> OFF -> AUTO
        state.mode = (state.mode + 1) % 3;
        changed = true;
        Serial.print(F("[ENC] Uj mod: "));
        Serial.println(modeToStr(state.mode));
    }

    if (changed) {
        evaluateControlLogic();
        sendToNanoAndReadAck();
        lcd.update(state.currentTemp, state.targetTemp, state.kazan,
                   state.heatingOn, state.mode, state.nrf24Ok);
        publishMqttStatus();
    }
}

// ============================================================================
// SETUP & LOOP
// ============================================================================
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println(F("\n[ESP32-C3] Szobai termosztat & Gateway inditas..."));

    // ADC beállítás LM35-höz
    analogReadResolution(12);
    analogSetPinAttenuation(PIN_LM35, ADC_11db);
    state.currentTemp = readRoomTempLM35();

    // Encoder lábak és megszakítások
    pinMode(PIN_ENC_CLK, INPUT_PULLUP);
    pinMode(PIN_ENC_DT,  INPUT_PULLUP);
    pinMode(PIN_ENC_SW,  INPUT_PULLUP);
    encPrevState = (static_cast<uint8_t>(digitalRead(PIN_ENC_CLK)) << 1) |
                    static_cast<uint8_t>(digitalRead(PIN_ENC_DT));
    attachInterrupt(digitalPinToInterrupt(PIN_ENC_CLK), isrEncoderRotate, CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_ENC_DT),  isrEncoderRotate, CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_ENC_SW),  isrEncoderButton, FALLING);

    // 2x20 I2C LCD
    lcd.begin();

    // NRF24L01+ inicializálás
    spiNrf.begin(PIN_NRF_SCK, PIN_NRF_MISO, PIN_NRF_MOSI, PIN_NRF_CSN);
    if (!radio.begin(&spiNrf)) {
        Serial.println(F("[ESP] HIBA: NRF24L01+ nem talalhato!"));
    } else {
        radio.setChannel(NRF_CHANNEL);
        radio.setDataRate(RF24_250KBPS);
        radio.setPALevel(RF24_PA_HIGH);
        radio.setRetries(6, 15);
        radio.enableDynamicPayloads();
        radio.enableAckPayload();
        radio.openWritingPipe(NRF_ADDR_PIPE);
        radio.stopListening();
        Serial.println(F("[ESP] NRF24L01+ keszenletben."));
    }

    // Első szabályozási kiértékelés és kijelzés WiFi-tól függetlenül
    evaluateControlLogic();
    lastNrfAckMs = millis();
    sendToNanoAndReadAck();
    lcd.update(state.currentTemp, state.targetTemp, state.kazan,
               state.heatingOn, state.mode, state.nrf24Ok);

    // WiFi + HiveMQ Cloud TLS (nem-blokkoló indítás)
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    lastWifiTryMs = millis();

    // HiveMQ Cloud TLS (éles rendszerben érdemes beágyazott Root CA tanúsítvánnyal használni)
    wifiSecureClient.setInsecure();
    wifiSecureClient.setTimeout(3);

    mqtt.setServer(MQTT_BROKER, MQTT_PORT);
    mqtt.setBufferSize(1024);
    mqtt.setCallback(mqttCallback);
}

void loop() {
    uint32_t now = millis();

    // 1. Encoder és gomb események azonnali feldolgozása
    handleEncoderEvents();

    // 2. LM35 szobai hőmérséklet mérés és hiszterézis szabályozás
    if (now - lastLm35Ms >= LM35_READ_INTERVAL_MS) {
        lastLm35Ms = now;
        float prevTemp = state.currentTemp;
        bool prevHeat = state.heatingOn;

        state.currentTemp = readRoomTempLM35();
        evaluateControlLogic();

        lcd.update(state.currentTemp, state.targetTemp, state.kazan,
                   state.heatingOn, state.mode, state.nrf24Ok);

        if (prevHeat != state.heatingOn || fabsf(prevTemp - state.currentTemp) >= 0.2f) {
            sendToNanoAndReadAck();
            publishMqttStatus();
        }
    }

    // 3. NRF24 ping 2 másodpercenként + 10 mp timeout ellenőrzés
    if (now - lastNrfPingMs >= NRF_PING_INTERVAL_MS) {
        lastNrfPingMs = now;
        bool prevNrfOk = state.nrf24Ok;
        bool prevHeat  = state.heatingOn;

        sendToNanoAndReadAck();

        if (state.nrf24Ok && (now - lastNrfAckMs >= NRF_TIMEOUT_MS)) {
            state.nrf24Ok = false;
            Serial.println(F("[ESP] FIGYELEM: NRF24 timeout (>10mp)!"));
        }

        lcd.update(state.currentTemp, state.targetTemp, state.kazan,
                   state.heatingOn, state.mode, state.nrf24Ok);

        if (prevNrfOk != state.nrf24Ok || prevHeat != state.heatingOn) {
            publishMqttStatus();
        }
    }

    // 4. WiFi és HiveMQ Cloud MQTT nem-blokkoló karbantartása
    maintainConnectivity(now);

    // 5. MQTT Telemetry küldés 30 másodpercenként
    if (now - lastTelemetryMs >= TELEMETRY_INTERVAL_MS) {
        lastTelemetryMs = now;
        publishMqttTelemetry();
    }
}
