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
#include "../../nano/src/common.h"

// ============================================================================
// ESP32-C3 - SZOBA OLDALI TERMOSZTÁT, KAZÁNVÉDELEM, RIASZTÁS & GATEWAY (V4)
// ============================================================================

SPIClass spiNrf(FSPI);
RF24 radio(PIN_NRF_CE, PIN_NRF_CSN);
HeatingLcd lcd;

WiFiClientSecure wifiSecureClient;
PubSubClient mqtt(wifiSecureClient);

// --- Rendszer állapot ---
struct SystemState {
    // Szobatermosztát (csak jelzés + encoder)
    float   currentTemp   = 20.0f;
    float   targetTemp    = TARGET_TEMP_DEFAULT;
    bool    heatingOn     = false;
    uint8_t mode          = MODE_AUTO;
    bool    manualHeating = false;

    // Kimenetek (bit0-t a kazánvédelmi állapotgép + biztonsági szabályok vezérlik)
    uint8_t outputsMask   = 0;
    bool    userLedPiros  = false;

    // Nano felől érkező adatok (-127.0f ha szenzorhiba)
    float            eloremeno       = SENSOR_DISCONNECTED_VAL;
    float            visszatero      = SENSOR_DISCONNECTED_VAL;
    float            kazan           = SENSOR_DISCONNECTED_VAL;
    float            kulteri         = SENSOR_DISCONNECTED_VAL;
    uint8_t          kazanStatus[8]  = { 0 };
    uint32_t         nanoCounter     = 0;
    uint8_t          sensorValidMask = 0;
    AlarmStatePacked nanoAlarmState  = { 0, ALARM_REASON_NONE };
    bool             nrf24Ok         = false;

    // Kazánvédelem (szivattyú állapotgép)
    PumpConfigPacked pumpConfig = {
        PUMP_MODE_AUTO,
        PUMP_START_TEMP_DEFAULT,
        PUMP_STOP_TEMP_DEFAULT,
        PUMP_SENSOR_KAZAN
    };
    uint8_t  pumpState         = PUMP_STATE_VARAKOZAS;
    bool     pumpOn            = false;
    uint32_t manualOffStartMs  = 0;
    bool     safetyOverheat    = false;
    bool     safetySensorFault = false;

    // Riasztási konfiguráció és állapot
    AlarmConfigPacked alarmConfig = {
        ALARM_OVERHEAT_TEMP_DEFAULT,
        ALARM_ENABLED_DEFAULT ? 1 : 0
    };
    AlarmStatePacked alarmState = {
        0,
        ALARM_REASON_NONE
    };
} state;

// --- Időzítők ---
uint32_t lastLm35Ms       = 0;
uint32_t lastNrfPingMs    = 0;
uint32_t lastNrfAckMs     = 0;
uint32_t lastTelemetryMs  = 0;
uint32_t lastWifiTryMs    = 0;
uint32_t lastMqttTryMs    = 0;
uint32_t lastAlarmBlinkMs = 0;
bool     alarmBlinkOn     = false;

// --- Rotary Encoder ISR állapot (Gray-kód tábla + debounce) ---
volatile int8_t   encStepAccum    = 0;
volatile uint8_t  encPrevState    = 0;
volatile bool     btnPressedEvent = false;
volatile uint32_t lastBtnIsrMs    = 0;
volatile int8_t   encQuarterAccum = 0;

static const int8_t QEM_TABLE[16] = {
     0, -1,  1,  0,
     1,  0,  0, -1,
    -1,  0,  0,  1,
     0,  1, -1,  0
};

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
// SEGÉDFÜGGVÉNYEK ÉS KONVERZIÓK
// ============================================================================
inline bool isSensorValid(float t) {
    return !isnan(t) && t > -120.0f && t <= 125.0f;
}

float round1(float v) {
    return roundf(v * 10.0f) / 10.0f;
}

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

const char* pumpModeToStr(uint8_t pm) {
    switch (pm) {
        case PUMP_MODE_AUTO:       return "auto";
        case PUMP_MODE_MANUAL_ON:  return "manual_on";
        case PUMP_MODE_MANUAL_OFF: return "manual_off";
        default:                   return "auto";
    }
}

uint8_t strToPumpMode(const char* s) {
    if (!s) return PUMP_MODE_AUTO;
    if (strcmp(s, "manual_on") == 0)  return PUMP_MODE_MANUAL_ON;
    if (strcmp(s, "manual_off") == 0) return PUMP_MODE_MANUAL_OFF;
    return PUMP_MODE_AUTO;
}

const char* pumpSensorToStr(uint8_t ps) {
    switch (ps) {
        case PUMP_SENSOR_KAZAN:     return "kazan";
        case PUMP_SENSOR_ELOREMENO: return "eloremeno";
        case PUMP_SENSOR_MAX:       return "max";
        default:                    return "kazan";
    }
}

uint8_t strToPumpSensor(const char* s) {
    if (!s) return PUMP_SENSOR_KAZAN;
    if (strcmp(s, "eloremeno") == 0) return PUMP_SENSOR_ELOREMENO;
    if (strcmp(s, "max") == 0)       return PUMP_SENSOR_MAX;
    return PUMP_SENSOR_KAZAN;
}

const char* pumpStateToStr(uint8_t ps) {
    return (ps == PUMP_STATE_FUTES) ? "futes" : "varakozas";
}

const char* alarmReasonToStr(uint8_t r) {
    switch (r) {
        case ALARM_REASON_OVERHEAT:    return "overheat";
        case ALARM_REASON_SENSOR_FAIL: return "sensor_fail";
        case ALARM_REASON_NRF_FAIL:    return "nrf_fail";
        case ALARM_REASON_NONE:
        default:                       return "none";
    }
}

void sanitizePumpConfig(PumpConfigPacked& cfg) {
    if (cfg.pumpStartTemp < PUMP_START_TEMP_MIN) cfg.pumpStartTemp = PUMP_START_TEMP_MIN;
    if (cfg.pumpStartTemp > PUMP_START_TEMP_MAX) cfg.pumpStartTemp = PUMP_START_TEMP_MAX;

    if (cfg.pumpStopTemp < PUMP_STOP_TEMP_MIN) cfg.pumpStopTemp = PUMP_STOP_TEMP_MIN;
    if (cfg.pumpStopTemp > PUMP_STOP_TEMP_MAX) cfg.pumpStopTemp = PUMP_STOP_TEMP_MAX;

    // MINDIG pumpStopTemp < pumpStartTemp - 5 kell legyen
    float maxAllowedStop = cfg.pumpStartTemp - PUMP_MIN_HYSTERESIS_GAP;
    if (cfg.pumpStopTemp >= maxAllowedStop) {
        cfg.pumpStopTemp = maxAllowedStop - 0.5f;
        if (cfg.pumpStopTemp < PUMP_STOP_TEMP_MIN) {
            cfg.pumpStopTemp = PUMP_STOP_TEMP_MIN;
        }
    }
}

void sanitizeAlarmConfig(AlarmConfigPacked& cfg) {
    if (isnan(cfg.overheatTemp)) {
        cfg.overheatTemp = ALARM_OVERHEAT_TEMP_DEFAULT;
    }
    if (cfg.overheatTemp < ALARM_OVERHEAT_TEMP_MIN) cfg.overheatTemp = ALARM_OVERHEAT_TEMP_MIN;
    if (cfg.overheatTemp > ALARM_OVERHEAT_TEMP_MAX) cfg.overheatTemp = ALARM_OVERHEAT_TEMP_MAX;
    cfg.overheatTemp = round1(cfg.overheatTemp);
    cfg.alarmEnabled = cfg.alarmEnabled ? 1 : 0;
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
    float tempC = (avgMv / 3300.0f) * 100.0f + LM35_OFFSET;
    return round1(tempC);
}

// ============================================================================
// SZOBAFŰTÉS HISZTERÉZIS (CSAK JELZÉS A FRONTENDNEK, NEM VEZÉRLI A SZIVATTYÚT)
// ============================================================================
void evaluateRoomHeatingIndicator() {
    if (state.mode == MODE_OFF) {
        state.heatingOn = false;
        return;
    }
    if (state.mode == MODE_MANUAL) {
        state.heatingOn = state.manualHeating;
        return;
    }
    // MODE_AUTO: BE ha currentTemp < target - 0.3, KI ha > target + 0.3
    if (state.currentTemp < (state.targetTemp - HYST_LOW_OFFSET)) {
        state.heatingOn = true;
    } else if (state.currentTemp > (state.targetTemp + HYST_HIGH_OFFSET)) {
        state.heatingOn = false;
    }
}

// ============================================================================
// KAZÁNVÉDELEM ÁLLAPOTGÉP + BIZTONSÁGI SZABÁLYOK + RIASZTÁS ÁLLAPOT
// ============================================================================
bool getSelectedStartSensorTemp(float& outTemp) {
    bool kOk = isSensorValid(state.kazan);
    bool eOk = isSensorValid(state.eloremeno);

    switch (state.pumpConfig.pumpStartSensor) {
        case PUMP_SENSOR_ELOREMENO:
            if (!eOk) return false;
            outTemp = state.eloremeno;
            return true;

        case PUMP_SENSOR_MAX:
            if (kOk && eOk) {
                outTemp = fmaxf(state.kazan, state.eloremeno);
                return true;
            }
            if (kOk) { outTemp = state.kazan; return true; }
            if (eOk) { outTemp = state.eloremeno; return true; }
            return false;

        case PUMP_SENSOR_KAZAN:
        default:
            if (!kOk) return false;
            outTemp = state.kazan;
            return true;
    }
}

void evaluateAlarmState() {
    if (state.alarmConfig.alarmEnabled == 0) {
        state.alarmState.active = 0;
        state.alarmState.reason = ALARM_REASON_NONE;
        return;
    }

    if (state.safetyOverheat) {
        state.alarmState.active = 1;
        state.alarmState.reason = ALARM_REASON_OVERHEAT;
    } else if (state.safetySensorFault) {
        state.alarmState.active = 1;
        state.alarmState.reason = ALARM_REASON_SENSOR_FAIL;
    } else if (!state.nrf24Ok) {
        state.alarmState.active = 1;
        state.alarmState.reason = ALARM_REASON_NRF_FAIL;
    } else if (state.nanoAlarmState.active != 0) {
        state.alarmState.active = 1;
        state.alarmState.reason = state.nanoAlarmState.reason;
    } else {
        state.alarmState.active = 0;
        state.alarmState.reason = ALARM_REASON_NONE;
    }
}

// Visszatér true-val, ha manual_off 10 perc után automatikusan visszaállt auto-ra
bool evaluateBoilerPumpProtection(uint32_t now) {
    bool modeAutoRestored = false;

    // 0. 10 perces manual_off automatikus visszaállás ellenőrzése
    if (state.pumpConfig.pumpMode == PUMP_MODE_MANUAL_OFF) {
        if (now - state.manualOffStartMs >= PUMP_MANUAL_OFF_TIMEOUT_MS) {
            state.pumpConfig.pumpMode = PUMP_MODE_AUTO;
            modeAutoRestored = true;
            Serial.println(F("[PUMP] 10 perc letelt: manual_off automatikusan visszaallt auto-ra."));
        }
    }

    // 1. Állapotgép (pumpMode == "auto") vagy manuális felülbírálás
    bool desiredPump = false;
    if (state.pumpConfig.pumpMode == PUMP_MODE_AUTO) {
        if (state.pumpState == PUMP_STATE_VARAKOZAS) {
            float startTemp = 0.0f;
            if (getSelectedStartSensorTemp(startTemp) && startTemp >= state.pumpConfig.pumpStartTemp) {
                state.pumpState = PUMP_STATE_FUTES;
            }
        } else {
            // PUMP_STATE_FUTES: hűlésnél CSAK az eloremeno <= pumpStopTemp feltételt figyeljük!
            if (isSensorValid(state.eloremeno) && state.eloremeno <= state.pumpConfig.pumpStopTemp) {
                state.pumpState = PUMP_STATE_VARAKOZAS;
            }
        }
        desiredPump = (state.pumpState == PUMP_STATE_FUTES);
    } else if (state.pumpConfig.pumpMode == PUMP_MODE_MANUAL_ON) {
        desiredPump = true;
        state.pumpState = PUMP_STATE_FUTES;
    } else { // PUMP_MODE_MANUAL_OFF
        desiredPump = false;
        state.pumpState = PUMP_STATE_VARAKOZAS;
    }

    // 2. BIZTONSÁGI SZABÁLYOK (minden mást felülírnak, még a manual_off-ot is!)
    bool kazanValid = isSensorValid(state.kazan);
    state.safetySensorFault = !kazanValid;
    state.safetyOverheat    = kazanValid && (state.kazan >= state.alarmConfig.overheatTemp);

    if (state.safetyOverheat || state.safetySensorFault) {
        desiredPump = true;
        state.pumpState = PUMP_STATE_FUTES;
    }

    state.pumpOn = desiredPump;

    // 3. Riasztási állapot kiértékelése
    evaluateAlarmState();

    // Kimeneti bitmaszk szinkronizálása: bit0 mindig a kazánvédelmi döntés!
    setBit(state.outputsMask, OUT_BIT_SZIVATTYU, state.pumpOn);
    setBit(state.outputsMask, OUT_BIT_LED_ZOLD,  state.pumpOn);

    if (state.alarmConfig.alarmEnabled == 0) {
        setBit(state.outputsMask, OUT_BIT_LED_PIROS, false);
    } else {
        setBit(state.outputsMask, OUT_BIT_LED_PIROS,
               (state.alarmState.active != 0) || state.userLedPiros);
    }

    return modeAutoRestored;
}

void updateAlarmBlinkAndLed(uint32_t now) {
    if (state.alarmConfig.alarmEnabled == 0 || state.alarmState.active == 0) {
        alarmBlinkOn = false;
        digitalWrite(PIN_ALARM_LED, LOW);
        return;
    }

    if (now - lastAlarmBlinkMs >= ALARM_BLINK_INTERVAL_MS) {
        lastAlarmBlinkMs = now;
        alarmBlinkOn = !alarmBlinkOn;
        lcd.update(state.currentTemp, state.targetTemp, state.kazan,
                   state.pumpOn, state.mode, state.nrf24Ok,
                   true, state.alarmState.reason, alarmBlinkOn);
    }
    digitalWrite(PIN_ALARM_LED, alarmBlinkOn ? HIGH : LOW);
}

void refreshLcd() {
    lcd.update(state.currentTemp, state.targetTemp, state.kazan,
               state.pumpOn, state.mode, state.nrf24Ok,
               state.alarmState.active != 0, state.alarmState.reason, alarmBlinkOn);
}

// ============================================================================
// NRF24 KOMMUNIKÁCIÓ A NANO FELÉ (32 BYTE STRUCT + ACK PAYLOAD)
// ============================================================================
bool sendToNanoAndReadAck() {
    NrfPacket tx{};
    tx.packetType        = PKT_TYPE_ESP_TO_NANO;
    tx.esp.outputsMask   = state.outputsMask;
    tx.esp.targetTemp    = state.targetTemp;
    tx.esp.mode          = state.mode;
    tx.esp.manualHeating = state.manualHeating ? 1 : 0;
    tx.esp.pumpConfig    = state.pumpConfig;
    tx.esp.alarmConfig   = state.alarmConfig;

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
                state.eloremeno   = rx.nano.eloremeno;
                state.visszatero  = rx.nano.visszatero;
                state.kazan       = rx.nano.kazan;
                state.kulteri     = rx.nano.kulteri;
                memcpy(state.kazanStatus, rx.nano.kazan_status, 8);
                state.nanoCounter     = rx.nano.counter;
                state.sensorValidMask = rx.nano.sensorValidMask;
                state.nanoAlarmState  = rx.nano.alarmState;
            }
        }
    }
    return ok;
}

// ============================================================================
// MQTT JSON STATUS & TELEMETRY PUBLISH (V4 + ALARM)
// ============================================================================
void buildStatusJson(JsonDocument& doc) {
    doc["currentTemp"] = round1(state.currentTemp);
    doc["targetTemp"]  = round1(state.targetTemp);
    doc["heatingOn"]   = state.heatingOn;
    doc["mode"]        = modeToStr(state.mode);
    doc["online"]      = true;

    JsonObject temps = doc["homersekletek"].to<JsonObject>();
    if (!isSensorValid(state.eloremeno))  temps["eloremeno"]  = nullptr; else temps["eloremeno"]  = round1(state.eloremeno);
    if (!isSensorValid(state.visszatero)) temps["visszatero"] = nullptr; else temps["visszatero"] = round1(state.visszatero);
    if (!isSensorValid(state.kazan))      temps["kazan"]      = nullptr; else temps["kazan"]      = round1(state.kazan);
    if (!isSensorValid(state.kulteri))    temps["kulteri"]    = nullptr; else temps["kulteri"]    = round1(state.kulteri);

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
        kStat.add(state.kazanStatus[i]);
    }

    JsonObject pCfg = doc["pumpConfig"].to<JsonObject>();
    pCfg["pumpMode"]        = pumpModeToStr(state.pumpConfig.pumpMode);
    pCfg["pumpStartTemp"]   = round1(state.pumpConfig.pumpStartTemp);
    pCfg["pumpStopTemp"]    = round1(state.pumpConfig.pumpStopTemp);
    pCfg["pumpStartSensor"] = pumpSensorToStr(state.pumpConfig.pumpStartSensor);

    doc["pumpState"] = pumpStateToStr(state.pumpState);

    JsonObject aCfg = doc["alarmConfig"].to<JsonObject>();
    aCfg["overheatTemp"] = round1(state.alarmConfig.overheatTemp);
    aCfg["alarmEnabled"] = (state.alarmConfig.alarmEnabled != 0);

    JsonObject aState = doc["alarmState"].to<JsonObject>();
    aState["active"] = (state.alarmState.active != 0);
    aState["reason"] = alarmReasonToStr(state.alarmState.reason);

    doc["nrf24_ok"] = state.nrf24Ok;

    if (!state.nrf24Ok) {
        doc["error"] = "NRF24 kommunikacios hiba (10s timeout)";
    } else if (state.safetyOverheat) {
        doc["error"] = "Kazan tulmelegedes (>= overheatTemp) - szivattyu kenyszeritett BE";
    } else if (state.safetySensorFault) {
        doc["error"] = "Kazan szenzor hiba (-127) - szivattyu biztonsagi BE";
    } else {
        doc["error"] = nullptr;
    }
}

void publishMqttStatus() {
    if (!mqtt.connected()) return;
    JsonDocument doc;
    buildStatusJson(doc);
    char payload[1280];
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
    char payload[1280];
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

    if (doc["kimenetek"].is<JsonObject>()) {
        JsonObject k = doc["kimenetek"].as<JsonObject>();
        // bit0 (szivattyu) védelmi állapotgép alá tartozik, a többi bit állítható
        if (k["led_zold"].is<bool>())  setBit(state.outputsMask, OUT_BIT_LED_ZOLD,  k["led_zold"].as<bool>());
        if (k["led_piros"].is<bool>()) state.userLedPiros = k["led_piros"].as<bool>();
        if (k["tartalek1"].is<bool>()) setBit(state.outputsMask, OUT_BIT_TARTALEK1, k["tartalek1"].as<bool>());
        if (k["tartalek2"].is<bool>()) setBit(state.outputsMask, OUT_BIT_TARTALEK2, k["tartalek2"].as<bool>());
        if (k["tartalek3"].is<bool>()) setBit(state.outputsMask, OUT_BIT_TARTALEK3, k["tartalek3"].as<bool>());
        if (k["tartalek4"].is<bool>()) setBit(state.outputsMask, OUT_BIT_TARTALEK4, k["tartalek4"].as<bool>());
        if (k["tartalek5"].is<bool>()) setBit(state.outputsMask, OUT_BIT_TARTALEK5, k["tartalek5"].as<bool>());
    }

    if (doc["pumpConfig"].is<JsonObject>()) {
        JsonObject pc = doc["pumpConfig"].as<JsonObject>();
        if (pc["pumpMode"].is<const char*>()) {
            uint8_t nextPumpMode = strToPumpMode(pc["pumpMode"].as<const char*>());
            if (nextPumpMode == PUMP_MODE_MANUAL_OFF && state.pumpConfig.pumpMode != PUMP_MODE_MANUAL_OFF) {
                state.manualOffStartMs = millis();
            }
            state.pumpConfig.pumpMode = nextPumpMode;
        }
        if (pc["pumpStartTemp"].is<float>() || pc["pumpStartTemp"].is<int>()) {
            state.pumpConfig.pumpStartTemp = pc["pumpStartTemp"].as<float>();
        }
        if (pc["pumpStopTemp"].is<float>() || pc["pumpStopTemp"].is<int>()) {
            state.pumpConfig.pumpStopTemp = pc["pumpStopTemp"].as<float>();
        }
        if (pc["pumpStartSensor"].is<const char*>()) {
            state.pumpConfig.pumpStartSensor = strToPumpSensor(pc["pumpStartSensor"].as<const char*>());
        }
        sanitizePumpConfig(state.pumpConfig);
    }

    if (doc["alarmConfig"].is<JsonObject>()) {
        JsonObject ac = doc["alarmConfig"].as<JsonObject>();
        if (ac["overheatTemp"].is<float>() || ac["overheatTemp"].is<int>()) {
            state.alarmConfig.overheatTemp = ac["overheatTemp"].as<float>();
        }
        if (ac["alarmEnabled"].is<bool>()) {
            state.alarmConfig.alarmEnabled = ac["alarmEnabled"].as<bool>() ? 1 : 0;
        }
        sanitizeAlarmConfig(state.alarmConfig);
    }

    uint32_t now = millis();
    evaluateRoomHeatingIndicator();
    evaluateBoilerPumpProtection(now);
    updateAlarmBlinkAndLed(now);
    sendToNanoAndReadAck();
    evaluateBoilerPumpProtection(now);
    refreshLcd();
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
        return;
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
void handleEncoderEvents(uint32_t now) {
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
        }
    }

    if (btnClicked) {
        state.mode = (state.mode + 1) % 3; // AUTO -> MANUAL -> OFF -> AUTO
        changed = true;
    }

    if (changed) {
        evaluateRoomHeatingIndicator();
        evaluateBoilerPumpProtection(now);
        sendToNanoAndReadAck();
        refreshLcd();
        publishMqttStatus();
    }
}

// ============================================================================
// SETUP & LOOP
// ============================================================================
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println(F("\n[ESP32-C3 V4] Szobai termosztat, Kazanvedelem & Riaszto Gateway inditas..."));

    pinMode(PIN_ALARM_LED, OUTPUT);
    digitalWrite(PIN_ALARM_LED, LOW);

    analogReadResolution(12);
    analogSetPinAttenuation(PIN_LM35, ADC_11db);
    state.currentTemp = readRoomTempLM35();

    pinMode(PIN_ENC_CLK, INPUT_PULLUP);
    pinMode(PIN_ENC_DT,  INPUT_PULLUP);
    pinMode(PIN_ENC_SW,  INPUT_PULLUP);
    encPrevState = (static_cast<uint8_t>(digitalRead(PIN_ENC_CLK)) << 1) |
                    static_cast<uint8_t>(digitalRead(PIN_ENC_DT));
    attachInterrupt(digitalPinToInterrupt(PIN_ENC_CLK), isrEncoderRotate, CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_ENC_DT),  isrEncoderRotate, CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_ENC_SW),  isrEncoderButton, FALLING);

    lcd.begin();

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

    uint32_t now = millis();
    lastNrfAckMs = now;
    lastAlarmBlinkMs = now;
    evaluateRoomHeatingIndicator();
    evaluateBoilerPumpProtection(now);
    sendToNanoAndReadAck();
    evaluateBoilerPumpProtection(now);
    updateAlarmBlinkAndLed(now);
    refreshLcd();

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    lastWifiTryMs = now;

    wifiSecureClient.setInsecure();
    wifiSecureClient.setTimeout(3);

    mqtt.setServer(MQTT_BROKER, MQTT_PORT);
    mqtt.setBufferSize(2048);
    mqtt.setCallback(mqttCallback);
}

void loop() {
    uint32_t now = millis();

    // 0. Nem-blokkoló 500ms riasztási villogás (GPIO 11 LED + LCD 2. sor váltakozás)
    updateAlarmBlinkAndLed(now);

    // 1. Encoder és gomb események azonnali feldolgozása
    handleEncoderEvents(now);

    // 2. LM35 szobai hőmérséklet mérés és szobafűtés-jelzés frissítése
    if (now - lastLm35Ms >= LM35_READ_INTERVAL_MS) {
        lastLm35Ms = now;
        float prevTemp = state.currentTemp;
        bool prevHeat  = state.heatingOn;
        bool prevPump  = state.pumpOn;

        state.currentTemp = readRoomTempLM35();
        evaluateRoomHeatingIndicator();
        bool modeRestored = evaluateBoilerPumpProtection(now);

        refreshLcd();

        if (modeRestored || prevHeat != state.heatingOn || prevPump != state.pumpOn ||
            fabsf(prevTemp - state.currentTemp) >= 0.2f) {
            sendToNanoAndReadAck();
            publishMqttStatus();
        }
    }

    // 3. NRF24 ping 2 másodpercenként + 10 mp timeout + kazánvédelem & riasztás újraszámolás
    if (now - lastNrfPingMs >= NRF_PING_INTERVAL_MS) {
        lastNrfPingMs = now;
        bool prevNrfOk       = state.nrf24Ok;
        bool prevPump        = state.pumpOn;
        uint8_t prevPState   = state.pumpState;
        uint8_t prevAlarmAct = state.alarmState.active;
        uint8_t prevAlarmRsn = state.alarmState.reason;

        sendToNanoAndReadAck();

        if (state.nrf24Ok && (now - lastNrfAckMs >= NRF_TIMEOUT_MS)) {
            state.nrf24Ok = false;
            Serial.println(F("[ESP] FIGYELEM: NRF24 timeout (>10mp)!"));
        }

        bool modeRestored = evaluateBoilerPumpProtection(now);

        if (prevPump != state.pumpOn) {
            sendToNanoAndReadAck();
        }

        refreshLcd();

        if (modeRestored || prevNrfOk != state.nrf24Ok ||
            prevPump != state.pumpOn || prevPState != state.pumpState ||
            prevAlarmAct != state.alarmState.active || prevAlarmRsn != state.alarmState.reason) {
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
