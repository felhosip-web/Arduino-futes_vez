#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <WiFiClientSecure.h>
#include <stdio.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <RF24.h>
#include <math.h>

#include "config.h"
#include "lcd.h"
#include "menu.h"
#include "../../nano/src/common.h"

// ============================================================================
// ESP32-C3 - SZOBA OLDALI MASTER / GATEWAY (V5 PROTOKOLL + MENÜ + NVS)
// ============================================================================
// - Master -> Slave REQUEST (0xA1) növekvő seq-el, majd max 500ms várakozás
//   Slave -> Master RESPONSE (0xB2) csomagra, max 3 próbával, AckPayload KI.
// - 2x20 I2C LCD menürendszer interrupt alapú rotary encoderrel (delay nélkül).
// - Preferences (NVS, "futes" namespace) tartós tárolás + 3 mp gyári reset.
// ============================================================================

SPIClass spiNrf(FSPI);
RF24 radio(PIN_NRF_CE, PIN_NRF_CSN);
HeatingLcd lcd;
MenuController menu;
Preferences prefs;

WiFiClientSecure wifiSecureClient;
PubSubClient mqtt(wifiSecureClient);
WebServer wifiPortalServer(80);
DNSServer wifiPortalDns;

SystemState state;

constexpr const char* NVS_NAMESPACE = "futes";
constexpr const char* WIFI_NVS_NAMESPACE = "wifi";
constexpr uint8_t WIFI_DNS_PORT = 53;
constexpr const char* WIFI_PORTAL_IP = "192.168.4.1";

// --- Időzítők ---
uint32_t lastLm35Ms       = 0;
uint32_t lastHeartbeatMs  = 0;
uint32_t lastTelemetryMs  = 0;
uint32_t lastWifiTryMs    = 0;
uint32_t lastMqttTryMs    = 0;
uint32_t lastAlarmBlinkMs = 0;
uint32_t wifiDisconnectStartedMs = 0;
uint32_t lastWifiPortalTryMs = 0;
bool     alarmBlinkOn     = false;
bool     wifiPortalActive = false;
bool     wifiDisconnectTimerActive = false;
bool     wifiConnectionFailureReported = false;
String   wifiSsid;
String   wifiPassword;
const char* wifiPortalMessage = "Válaszd ki az otthoni Wi-Fi hálózatot.";

// --- Rotary Encoder + Gomb ISR állapot (Gray-kód tábla + szoftveres debounce) ---
volatile int8_t   encStepAccum    = 0;
volatile uint8_t  encPrevState    = 0;
volatile int8_t   encQuarterAccum = 0;

volatile bool     isrBtnEdgePending = false;
volatile bool     isrBtnLevelLow    = false;
volatile uint32_t isrBtnEdgeMs      = 0;

// Gomb lenyomási idő állapotgép (nem-blokkoló)
bool     btnIsDown           = false;
uint32_t btnDownStartMs      = 0;
bool     btnLongFired        = false;
bool     btnFactoryFired     = false;
uint32_t lastBtnPollGuardMs  = 0;

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
    if (now - isrBtnEdgeMs >= BTN_DEBOUNCE_MS) {
        isrBtnLevelLow    = (digitalRead(PIN_ENC_SW) == LOW);
        isrBtnEdgeMs      = now;
        isrBtnEdgePending = true;
    }
}

// ============================================================================
// SEGÉDFÜGGVÉNYEK ÉS SZANITIZÁLÁS
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
    if (isnan(cfg.pumpStartTemp)) cfg.pumpStartTemp = PUMP_START_TEMP_DEFAULT;
    if (isnan(cfg.pumpStopTemp))  cfg.pumpStopTemp  = PUMP_STOP_TEMP_DEFAULT;

    if (cfg.pumpStartTemp < PUMP_START_TEMP_MIN) cfg.pumpStartTemp = PUMP_START_TEMP_MIN;
    if (cfg.pumpStartTemp > PUMP_START_TEMP_MAX) cfg.pumpStartTemp = PUMP_START_TEMP_MAX;

    if (cfg.pumpStopTemp < PUMP_STOP_TEMP_MIN) cfg.pumpStopTemp = PUMP_STOP_TEMP_MIN;
    if (cfg.pumpStopTemp > PUMP_STOP_TEMP_MAX) cfg.pumpStopTemp = PUMP_STOP_TEMP_MAX;

    // MINDIG pumpStopTemp < pumpStartTemp - 5
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

bool isValidHeartbeatInterval(int val) {
    return val == 5 || val == 10 || val == 15 || val == 20 || val == 25 || val == 30;
}

// ============================================================================
// PREFERENCES (NVS) TARTÓS MENTÉS ÉS BETÖLTÉS (namespace: "futes")
// ============================================================================
void applyDefaultSettings() {
    state.targetTemp                 = TARGET_TEMP_DEFAULT; // 21.5
    state.mode                       = MODE_AUTO;
    state.manualHeating              = false;
    state.pumpConfig.pumpMode        = PUMP_MODE_AUTO;
    state.pumpConfig.pumpStartTemp   = PUMP_START_TEMP_DEFAULT; // 40.0
    state.pumpConfig.pumpStopTemp    = PUMP_STOP_TEMP_DEFAULT;  // 30.0
    state.pumpConfig.pumpStartSensor = PUMP_SENSOR_KAZAN;
    state.alarmConfig.overheatTemp   = ALARM_OVERHEAT_TEMP_DEFAULT; // 85.0
    state.alarmConfig.alarmEnabled   = ALARM_ENABLED_DEFAULT ? 1 : 0; // true
    state.heartbeatIntervalSec       = HEARTBEAT_DEFAULT_SEC; // 5
    state.outputsMask                = 0;
    state.userLedPiros               = false;
    state.pumpState                  = PUMP_STATE_VARAKOZAS;
}

void saveSettingsToNvs() {
    if (!prefs.begin(NVS_NAMESPACE, false)) {
        Serial.println(F("[NVS] Hiba a Preferences megnyitasakor (iras)."));
        return;
    }

    prefs.putFloat("targetTemp", state.targetTemp);
    prefs.putUChar("mode", state.mode);
    prefs.putBool("manualHeating", state.manualHeating);
    prefs.putUChar("pumpMode", state.pumpConfig.pumpMode);
    prefs.putFloat("pumpStartTemp", state.pumpConfig.pumpStartTemp);
    prefs.putFloat("pumpStopTemp", state.pumpConfig.pumpStopTemp);
    prefs.putUChar("pumpSensor", state.pumpConfig.pumpStartSensor);
    prefs.putFloat("overheatTemp", state.alarmConfig.overheatTemp);
    prefs.putBool("alarmEnabled", state.alarmConfig.alarmEnabled != 0);
    prefs.putUChar("heartbeatSec", state.heartbeatIntervalSec);

    // A szivattyú bitet (OUT_BIT_SZIVATTYU) NEM mentjük el, azt az állapotgép számolja
    uint8_t savedOutputsMask = state.outputsMask & static_cast<uint8_t>(~(1U << OUT_BIT_SZIVATTYU));
    setBit(savedOutputsMask, OUT_BIT_LED_PIROS, state.userLedPiros);
    prefs.putUChar("outputsMask", savedOutputsMask);

    prefs.end();
}

void loadSettingsFromNvs() {
    applyDefaultSettings();

    if (!prefs.begin(NVS_NAMESPACE, true)) {
        Serial.println(F("[NVS] Meg nincs mentett beallitas, alapertelmezett ertekek aktivak."));
        return;
    }

    float tTemp = prefs.getFloat("targetTemp", TARGET_TEMP_DEFAULT);
    if (!isnan(tTemp) && tTemp >= TARGET_TEMP_MIN && tTemp <= TARGET_TEMP_MAX) {
        state.targetTemp = roundf(tTemp * 2.0f) / 2.0f;
    }

    uint8_t m = prefs.getUChar("mode", MODE_AUTO);
    state.mode = (m <= MODE_OFF) ? m : MODE_AUTO;

    state.manualHeating = prefs.getBool("manualHeating", false);

    uint8_t pm = prefs.getUChar("pumpMode", PUMP_MODE_AUTO);
    state.pumpConfig.pumpMode = (pm <= PUMP_MODE_MANUAL_OFF) ? pm : PUMP_MODE_AUTO;

    state.pumpConfig.pumpStartTemp = prefs.getFloat("pumpStartTemp", PUMP_START_TEMP_DEFAULT);
    state.pumpConfig.pumpStopTemp  = prefs.getFloat("pumpStopTemp", PUMP_STOP_TEMP_DEFAULT);

    uint8_t ps = prefs.getUChar("pumpSensor", PUMP_SENSOR_KAZAN);
    state.pumpConfig.pumpStartSensor = (ps <= PUMP_SENSOR_MAX) ? ps : PUMP_SENSOR_KAZAN;
    sanitizePumpConfig(state.pumpConfig);

    state.alarmConfig.overheatTemp = prefs.getFloat("overheatTemp", ALARM_OVERHEAT_TEMP_DEFAULT);
    state.alarmConfig.alarmEnabled = prefs.getBool("alarmEnabled", ALARM_ENABLED_DEFAULT) ? 1 : 0;
    sanitizeAlarmConfig(state.alarmConfig);

    uint8_t hb = prefs.getUChar("heartbeatSec", HEARTBEAT_DEFAULT_SEC);
    state.heartbeatIntervalSec = isValidHeartbeatInterval(hb) ? hb : HEARTBEAT_DEFAULT_SEC;

    uint8_t loadedMask = prefs.getUChar("outputsMask", 0);
    setBit(loadedMask, OUT_BIT_SZIVATTYU, false);
    state.outputsMask  = loadedMask;
    state.userLedPiros = getBit(loadedMask, OUT_BIT_LED_PIROS);

    prefs.end();
    Serial.println(F("[NVS] Beallitasok betoltve a Preferences-bol."));
}

void clearNvsAndResetDefaults() {
    if (prefs.begin(NVS_NAMESPACE, false)) {
        prefs.clear();
        prefs.end();
    }
    applyDefaultSettings();
    Serial.println(F("[NVS] Gyari visszaallitas: Preferences torolve, alapertelmezesek visszaallitva."));
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
// SZOBAFŰTÉS HISZTERÉZIS (CSAK JELZÉS A FRONTENDNEK ÉS STATUS MENÜNEK)
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
    if (state.currentTemp < (state.targetTemp - HYST_LOW_OFFSET)) {
        state.heatingOn = true;
    } else if (state.currentTemp > (state.targetTemp + HYST_HIGH_OFFSET)) {
        state.heatingOn = false;
    }
}

// ============================================================================
// KAZÁNVÉDELEM ÁLLAPOTGÉP + BIZTONSÁGI SZABÁLYOK + RIASZTÁS (OR LOGIKA)
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

    // Saját és Nano-tól jött riasztás egyesítése (OR logika, prioritási sorrendben)
    if (state.safetyOverheat ||
        (state.nanoAlarmState.active != 0 && state.nanoAlarmState.reason == ALARM_REASON_OVERHEAT)) {
        state.alarmState.active = 1;
        state.alarmState.reason = ALARM_REASON_OVERHEAT;
    } else if (!state.nrf24Ok && state.consecutiveMissedPings >= NRF_MAX_MISSED_CYCLES) {
        state.alarmState.active = 1;
        state.alarmState.reason = ALARM_REASON_NRF_FAIL;
    } else if (state.safetySensorFault ||
               (state.nanoAlarmState.active != 0 && state.nanoAlarmState.reason == ALARM_REASON_SENSOR_FAIL)) {
        state.alarmState.active = 1;
        state.alarmState.reason = ALARM_REASON_SENSOR_FAIL;
    } else if (state.nanoAlarmState.active != 0) {
        state.alarmState.active = 1;
        state.alarmState.reason = state.nanoAlarmState.reason;
    } else {
        state.alarmState.active = 0;
        state.alarmState.reason = ALARM_REASON_NONE;
    }
}

bool evaluateBoilerPumpProtection(uint32_t now) {
    bool modeAutoRestored = false;

    // 10 perces manual_off automatikus visszaállás
    if (state.pumpConfig.pumpMode == PUMP_MODE_MANUAL_OFF) {
        if (now - state.manualOffStartMs >= PUMP_MANUAL_OFF_TIMEOUT_MS) {
            state.pumpConfig.pumpMode = PUMP_MODE_AUTO;
            modeAutoRestored = true;
            Serial.println(F("[PUMP] 10 perc letelt: manual_off automatikusan visszaallt auto-ra."));
        }
    }

    bool desiredPump = false;
    if (state.pumpConfig.pumpMode == PUMP_MODE_AUTO) {
        if (state.pumpState == PUMP_STATE_VARAKOZAS) {
            float startTemp = 0.0f;
            if (getSelectedStartSensorTemp(startTemp) && startTemp >= state.pumpConfig.pumpStartTemp) {
                state.pumpState = PUMP_STATE_FUTES;
            }
        } else {
            // Hűlésnél CSAK az eloremeno <= pumpStopTemp feltételt figyeljük
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

    // Biztonsági felülbírálás (még manual_off-ot is felülír)
    bool kazanValid = isSensorValid(state.kazan);
    state.safetySensorFault = !kazanValid;
    state.safetyOverheat    = kazanValid && (state.kazan >= state.alarmConfig.overheatTemp);

    if (state.safetyOverheat || state.safetySensorFault) {
        desiredPump = true;
        state.pumpState = PUMP_STATE_FUTES;
    }

    state.pumpOn = desiredPump;

    evaluateAlarmState();

    setBit(state.outputsMask, OUT_BIT_SZIVATTYU, state.pumpOn);

    // Piros LED: riasztás alatt a riasztás vezérli, riasztásmentes állapotban a kézi userLedPiros érvényesül
    if (state.alarmConfig.alarmEnabled != 0 && state.alarmState.active != 0) {
        setBit(state.outputsMask, OUT_BIT_LED_PIROS, true);
    } else {
        setBit(state.outputsMask, OUT_BIT_LED_PIROS, state.userLedPiros);
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
    }
    digitalWrite(PIN_ALARM_LED, alarmBlinkOn ? HIGH : LOW);
}

// ============================================================================
// V5 MASTER-SLAVE NRF24 KOMMUNIKÁCIÓ (REQUEST 0xA1 -> RESPONSE 0xB2)
// ============================================================================
bool sendRequestToNano() {
    state.txSeq++;
    if (state.txSeq == 0) {
        state.txSeq = 1;
    }

    NrfPacket tx{};
    tx.packetType               = PKT_TYPE_REQUEST;
    tx.esp.outputsMask          = state.outputsMask;
    tx.esp.targetTemp           = state.targetTemp;
    tx.esp.mode                 = state.mode;
    tx.esp.manualHeating        = state.manualHeating ? 1 : 0;
    tx.esp.pumpConfig           = state.pumpConfig;
    tx.esp.alarmConfig          = state.alarmConfig;
    tx.esp.heartbeatIntervalSec = state.heartbeatIntervalSec;
    tx.esp.seq                  = state.txSeq;

    for (uint8_t attempt = 0; attempt < NRF_MAX_ATTEMPTS; attempt++) {
        radio.stopListening();
        radio.flush_rx();
        radio.write(&tx, sizeof(tx));
        radio.startListening();

        uint32_t waitStartMs = millis();
        while (millis() - waitStartMs < NRF_RESPONSE_TIMEOUT_MS) {
            updateAlarmBlinkAndLed(millis());

            if (radio.available()) {
                NrfPacket rx{};
                radio.read(&rx, sizeof(rx));

                if (rx.packetType == PKT_TYPE_RESPONSE && rx.nano.seq == tx.esp.seq) {
                    radio.stopListening();

                    state.eloremeno       = rx.nano.eloremeno;
                    state.visszatero      = rx.nano.visszatero;
                    state.kazan           = rx.nano.kazan;
                    state.kulteri         = rx.nano.kulteri;
                    state.nanoCounter     = rx.nano.counter;
                    state.sensorValidMask = rx.nano.sensorValidMask;
                    state.nanoAlarmState  = rx.nano.alarmState;

                    for (uint8_t b = 0; b < 8; b++) {
                        state.kazanStatus[b] = getBit(rx.nano.kazanStatusMask, b) ? 1 : 0;
                    }

                    state.consecutiveMissedPings = 0;
                    if (!state.nrf24Ok) {
                        state.nrf24Ok = true;
                        Serial.println(F("[ESP] NRF24 Master-Slave kapcsolat helyreallt."));
                    }
                    return true;
                }
            }
            delay(1);
        }

        radio.stopListening();
    }

    if (state.consecutiveMissedPings < 255) {
        state.consecutiveMissedPings++;
    }
    if (state.consecutiveMissedPings >= NRF_MAX_MISSED_CYCLES) {
        if (state.nrf24Ok) {
            Serial.println(F("[ESP] HIBA: 3 egymas utani pingre nincs valasz -> NRF HIBA!"));
        }
        state.nrf24Ok = false;
    }

    return false;
}

// ============================================================================
// MQTT JSON STATUS & TELEMETRY PUBLISH (V5)
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

    JsonObject hb = doc["heartbeat"].to<JsonObject>();
    hb["intervalSec"] = state.heartbeatIntervalSec;

    doc["nrf24_ok"] = state.nrf24Ok;

    if (!state.nrf24Ok && state.consecutiveMissedPings >= NRF_MAX_MISSED_CYCLES) {
        doc["error"] = "NRF24 kommunikacios hiba (3 ping kimaradt)";
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
    char payload[1536];
    size_t n = serializeJson(doc, payload, sizeof(payload));
    if (n > 0) {
        mqtt.publish(TOPIC_STATUS, reinterpret_cast<const uint8_t*>(payload), n, true); // retained=true
    }
}

void publishMqttTelemetry() {
    if (!mqtt.connected()) return;
    JsonDocument doc;
    buildStatusJson(doc);
    doc["nanoCounter"] = state.nanoCounter;
    char payload[1536];
    size_t n = serializeJson(doc, payload, sizeof(payload));
    if (n > 0) {
        mqtt.publish(TOPIC_TELEMETRY, reinterpret_cast<const uint8_t*>(payload), n, false);
    }
}

// ============================================================================
// MENÜ CALLBACK-EK (AZONNALI MENTÉS + NRF REQUEST + MQTT STATUS)
// ============================================================================
void onMenuSettingCommitted() {
    uint32_t now = millis();
    evaluateRoomHeatingIndicator();
    evaluateBoilerPumpProtection(now);
    updateAlarmBlinkAndLed(now);
    saveSettingsToNvs();

    sendRequestToNano();
    lastHeartbeatMs = millis();
    evaluateBoilerPumpProtection(lastHeartbeatMs);
    updateAlarmBlinkAndLed(lastHeartbeatMs);

    publishMqttStatus();
    menu.requestRedraw();
}

void onMenuFactoryReset() {
    uint32_t now = millis();
    clearNvsAndResetDefaults();
    evaluateRoomHeatingIndicator();
    evaluateBoilerPumpProtection(now);
    updateAlarmBlinkAndLed(now);

    sendRequestToNano();
    lastHeartbeatMs = millis();
    evaluateBoilerPumpProtection(lastHeartbeatMs);
    updateAlarmBlinkAndLed(lastHeartbeatMs);

    publishMqttStatus();
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

    if (doc["heartbeat"].is<JsonObject>()) {
        JsonObject hb = doc["heartbeat"].as<JsonObject>();
        if (hb["intervalSec"].is<int>()) {
            int sec = hb["intervalSec"].as<int>();
            if (isValidHeartbeatInterval(sec)) {
                state.heartbeatIntervalSec = static_cast<uint8_t>(sec);
            }
        }
    }

    uint32_t now = millis();
    evaluateRoomHeatingIndicator();
    evaluateBoilerPumpProtection(now);
    updateAlarmBlinkAndLed(now);
    saveSettingsToNvs();

    sendRequestToNano();
    lastHeartbeatMs = millis();
    evaluateBoilerPumpProtection(lastHeartbeatMs);
    updateAlarmBlinkAndLed(lastHeartbeatMs);

    // Ha nincs éppen MENU_EDIT szerkesztésben, a menü azonnal átveszi a friss értékeket
    if (!menu.isEditing()) {
        menu.requestRedraw();
    }
    publishMqttStatus();
}

// ============================================================================
// NEM-BLOKKOLÓ WIFI ÉS MQTT KAPCSOLATKEZELÉS
// ============================================================================
void sendWifiPortalPage(const char* message, uint16_t statusCode = 200) {
    String page = F(
        "<!doctype html><html lang=\"hu\"><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Fűtésvezérlő Wi-Fi</title>"
        "<style>body{font:16px sans-serif;max-width:28rem;margin:3rem auto;padding:0 1rem}"
        "input,button{box-sizing:border-box;width:100%;padding:.75rem;margin:.4rem 0}"
        "button{cursor:pointer}</style><h1>Wi-Fi beállítás</h1><p>"
    );
    page += message;
    page += F(
        "</p><form method=\"post\" action=\"/save\">"
        "<label>Hálózat neve (SSID)<input name=\"ssid\" maxlength=\"32\" required></label>"
        "<label>Wi-Fi jelszó<input name=\"password\" type=\"password\" maxlength=\"64\" "
        "autocomplete=\"current-password\"></label>"
        "<button type=\"submit\">Mentés és csatlakozás</button></form></html>"
    );
    wifiPortalServer.send(statusCode, "text/html; charset=utf-8", page);
}

void handleWifiPortalRoot() {
    sendWifiPortalPage(wifiPortalMessage);
}

void handleWifiPortalSave() {
    if (!wifiPortalServer.hasArg("ssid") || !wifiPortalServer.hasArg("password")) {
        sendWifiPortalPage("Add meg a hálózat nevét és jelszavát.", 400);
        return;
    }

    String ssid = wifiPortalServer.arg("ssid");
    String password = wifiPortalServer.arg("password");
    if (ssid.length() == 0 || ssid.length() > 32 ||
        (password.length() > 0 && password.length() < 8) || password.length() > 63) {
        sendWifiPortalPage("Érvénytelen adat. Az SSID legfeljebb 32, a jelszó 8–63 karakter lehet (vagy üres nyílt hálózathoz).", 400);
        return;
    }

    Preferences wifiPrefs;
    if (!wifiPrefs.begin(WIFI_NVS_NAMESPACE, false)) {
        Serial.println(F("[WIFI] Hiba a Wi-Fi adatok NVS-be mentésekor."));
        sendWifiPortalPage("A Wi-Fi adatok mentése nem sikerült. Próbáld újra.", 500);
        return;
    }
    size_t ssidBytes = wifiPrefs.putString("ssid", ssid);
    size_t passwordBytes = wifiPrefs.putString("password", password);
    wifiPrefs.end();
    bool credentialsStored = ssidBytes > 0 && (password.length() == 0 || passwordBytes > 0);
    if (credentialsStored) {
        Preferences verifyPrefs;
        if (!verifyPrefs.begin(WIFI_NVS_NAMESPACE, true)) {
            credentialsStored = false;
        } else {
            credentialsStored =
                verifyPrefs.getString("ssid", "") == ssid &&
                verifyPrefs.getString("password", "") == password;
            verifyPrefs.end();
        }
    }
    if (!credentialsStored) {
        Serial.println(F("[WIFI] Hiba a Wi-Fi adatok NVS-be mentésekor."));
        sendWifiPortalPage("A Wi-Fi adatok mentése nem sikerült. Próbáld újra.", 500);
        return;
    }

    wifiSsid = ssid;
    wifiPassword = password;
    wifiPortalMessage = "Az adatok elmentve. Csatlakozás folyamatban; ha nem sikerül, add meg újra a hálózatot.";
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
    lastWifiTryMs = millis();
    wifiDisconnectStartedMs = lastWifiTryMs;
    wifiDisconnectTimerActive = true;
    wifiConnectionFailureReported = false;
    Serial.println(F("[WIFI] Beállítás elmentve; csatlakozás indul."));
    sendWifiPortalPage(wifiPortalMessage);
}

void handleWifiPortalNotFound() {
    wifiPortalServer.sendHeader("Location", String("http://") + WIFI_PORTAL_IP + "/");
    wifiPortalServer.send(302, "text/plain", "");
}

void beginWifiPortal(const char* message) {
    wifiPortalMessage = message;
    lastWifiPortalTryMs = millis();
    WiFi.mode(wifiSsid.length() == 0 ? WIFI_AP : WIFI_AP_STA);
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char apPassword[13];
    snprintf(apPassword, sizeof(apPassword), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (!WiFi.softAP(WIFI_SETUP_AP_SSID, apPassword)) {
        Serial.println(F("[WIFI] HIBA: a beállító hozzáférési pont nem indítható el."));
        return;
    }

    wifiPortalDns.start(WIFI_DNS_PORT, "*", WiFi.softAPIP());
    wifiPortalServer.begin();
    wifiPortalActive = true;
    Serial.print(F("[WIFI] Beállító hálózat: "));
    Serial.println(WIFI_SETUP_AP_SSID);
    Serial.print(F("[WIFI] Beállító jelszó: "));
    Serial.println(apPassword);
    Serial.print(F("[WIFI] Beállító oldal: http://"));
    Serial.println(WiFi.softAPIP());
}

void stopWifiPortal() {
    wifiPortalDns.stop();
    wifiPortalServer.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    wifiPortalActive = false;
    wifiConnectionFailureReported = false;
    wifiPortalMessage = "Válaszd ki az otthoni Wi-Fi hálózatot.";
    Serial.println(F("[WIFI] Beállító hozzáférési pont leállítva."));
}

void loadWifiCredentials() {
    Preferences wifiPrefs;
    if (!wifiPrefs.begin(WIFI_NVS_NAMESPACE, true)) {
        Serial.println(F("[WIFI] A mentett Wi-Fi adatok nem olvashatók."));
        return;
    }
    wifiSsid = wifiPrefs.getString("ssid", "");
    wifiPassword = wifiPrefs.getString("password", "");
    wifiPrefs.end();
}

void maintainConnectivity(uint32_t now) {
    if (wifiPortalActive) {
        wifiPortalDns.processNextRequest();
        wifiPortalServer.handleClient();
    }

    if (WiFi.status() != WL_CONNECTED) {
        if (wifiSsid.length() == 0) {
            if (!wifiPortalActive && now - lastWifiPortalTryMs >= WIFI_RECONNECT_MS) {
                beginWifiPortal("Válaszd ki az otthoni Wi-Fi hálózatot.");
            }
            return;
        }

        if (!wifiDisconnectTimerActive) {
            wifiDisconnectStartedMs = now;
            wifiDisconnectTimerActive = true;
        }

        if (wifiPortalActive && !wifiConnectionFailureReported &&
            now - wifiDisconnectStartedMs >= WIFI_PORTAL_TIMEOUT_MS) {
            wifiPortalMessage = "Nem sikerült csatlakozni. Ellenőrizd a hálózatnevet/jelszót, majd próbáld újra.";
            wifiConnectionFailureReported = true;
        }

        if (!wifiPortalActive &&
            now - wifiDisconnectStartedMs >= WIFI_PORTAL_TIMEOUT_MS &&
            now - lastWifiPortalTryMs >= WIFI_RECONNECT_MS) {
            beginWifiPortal("Nem sikerült csatlakozni a mentett hálózathoz. Add meg a Wi-Fi adatokat.");
        }

        if (now - lastWifiTryMs >= WIFI_RECONNECT_MS) {
            lastWifiTryMs = now;
            Serial.println(F("[WIFI] Ujracsatlakozas..."));
            WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
        }
        return;
    }

    wifiDisconnectTimerActive = false;
    if (wifiPortalActive) {
        stopWifiPortal();
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
// ENCODER ÉS NYOMÓGOMB ESEMÉNYEK FELDOLGOZÁSA (INTERRUPT + DEBOUNCE, DELAY NÉLKÜL)
// ============================================================================
void processEncoderAndButton(uint32_t now) {
    int8_t steps        = 0;
    bool   edgePending  = false;
    bool   edgeLow      = false;
    uint32_t edgeMs     = 0;

    noInterrupts();
    if (encStepAccum != 0) {
        steps = encStepAccum;
        encStepAccum = 0;
    }
    if (isrBtnEdgePending) {
        edgePending       = true;
        edgeLow           = isrBtnLevelLow;
        edgeMs            = isrBtnEdgeMs;
        isrBtnEdgePending = false;
    }
    interrupts();

    bool shortPress   = false;
    bool longPress    = false;
    bool factoryPress = false;

    if (edgePending) {
        if (edgeLow && !btnIsDown) {
            btnIsDown       = true;
            btnDownStartMs  = edgeMs;
            btnLongFired    = false;
            btnFactoryFired = false;
        } else if (!edgeLow && btnIsDown) {
            uint32_t heldMs = edgeMs - btnDownStartMs;
            if (!btnLongFired && !btnFactoryFired) {
                if (heldMs < SHORT_PRESS_MAX_MS) {
                    shortPress = true;
                } else if (heldMs >= LONG_PRESS_MS) {
                    longPress = true;
                }
            }
            btnIsDown = false;
        }
    }

    // Folyamatos nyomva tartás kezelése (hosszú nyomás >1000ms vagy 3mp gyári visszaállítás)
    if (btnIsDown) {
        // Védelem esetleges elmaradt felfutó él ellen
        if (digitalRead(PIN_ENC_SW) == HIGH && (now - lastBtnPollGuardMs >= BTN_DEBOUNCE_MS)) {
            uint32_t heldMs = now - btnDownStartMs;
            if (!btnLongFired && !btnFactoryFired) {
                if (heldMs < SHORT_PRESS_MAX_MS) {
                    shortPress = true;
                } else if (heldMs >= LONG_PRESS_MS) {
                    longPress = true;
                }
            }
            btnIsDown = false;
        } else {
            lastBtnPollGuardMs = now;
            uint32_t heldMs = now - btnDownStartMs;
            if (menu.isOnFactoryResetItem()) {
                if (!btnFactoryFired && heldMs >= FACTORY_RESET_HOLD_MS) {
                    btnFactoryFired = true;
                    factoryPress    = true;
                }
            } else {
                if (!btnLongFired && heldMs >= LONG_PRESS_MS) {
                    btnLongFired = true;
                    longPress    = true;
                }
            }
        }
    }

    if (steps != 0 || shortPress || longPress || factoryPress) {
        menu.handleInput(steps, shortPress, longPress, factoryPress, now);
    }
}

// ============================================================================
// SETUP & LOOP
// ============================================================================
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println(F("\n[ESP32-C3 V5] Master / Gateway + Menu inditas..."));

    pinMode(PIN_ALARM_LED, OUTPUT);
    digitalWrite(PIN_ALARM_LED, LOW);

    loadSettingsFromNvs();

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
    attachInterrupt(digitalPinToInterrupt(PIN_ENC_SW),  isrEncoderButton, CHANGE);

    lcd.begin();
    menu.begin(&lcd, &state, { onMenuSettingCommitted, onMenuFactoryReset });

    spiNrf.begin(PIN_NRF_SCK, PIN_NRF_MISO, PIN_NRF_MOSI, PIN_NRF_CSN);
    if (!radio.begin(&spiNrf)) {
        Serial.println(F("[ESP] HIBA: NRF24L01+ nem talalhato!"));
    } else {
        radio.disableAckPayload();
        radio.setPayloadSize(sizeof(NrfPacket));
        radio.setAutoAck(true);
        radio.setChannel(NRF_CHANNEL);
        radio.setDataRate(RF24_250KBPS);
        radio.setPALevel(RF24_PA_HIGH);
        radio.setRetries(5, 15);
        radio.openWritingPipe(NRF_ADDR_MASTER_TO_SLAVE);
        radio.openReadingPipe(1, NRF_ADDR_SLAVE_TO_MASTER);
        radio.stopListening();
        Serial.println(F("[ESP] NRF24L01+ MASTER keszenletben (AckPayload KI)."));
    }

    uint32_t now = millis();
    lastAlarmBlinkMs = now;
    evaluateRoomHeatingIndicator();
    evaluateBoilerPumpProtection(now);
    sendRequestToNano();
    lastHeartbeatMs = millis();
    evaluateBoilerPumpProtection(lastHeartbeatMs);
    updateAlarmBlinkAndLed(lastHeartbeatMs);
    lcd.setStatusIcons(state.pumpOn, state.nrf24Ok, false);
    menu.requestRedraw();
    menu.update(lastHeartbeatMs, alarmBlinkOn);
    lcd.refresh(lastHeartbeatMs);

    wifiPortalServer.on("/", HTTP_GET, handleWifiPortalRoot);
    wifiPortalServer.on("/save", HTTP_POST, handleWifiPortalSave);
    wifiPortalServer.onNotFound(handleWifiPortalNotFound);

    loadWifiCredentials();
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    if (wifiSsid.length() == 0) {
        beginWifiPortal("Első indítás: válaszd ki az otthoni Wi-Fi hálózatot.");
    } else {
        WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
        lastWifiTryMs = millis();
        wifiDisconnectStartedMs = lastWifiTryMs;
        wifiDisconnectTimerActive = true;
    }

    wifiSecureClient.setInsecure();
    wifiSecureClient.setTimeout(3);

    mqtt.setServer(MQTT_BROKER, MQTT_PORT);
    mqtt.setBufferSize(2048);
    mqtt.setCallback(mqttCallback);
}

void loop() {
    uint32_t now = millis();

    // 0. Nem-blokkoló 500ms riasztási LED villogás
    updateAlarmBlinkAndLed(now);

    // 1. Encoder és gomb események feldolgozása (interrupt + debounce)
    processEncoderAndButton(now);

    // 2. LM35 szobahő mérés és szobafűtés-jelzés (1 mp-enként)
    if (now - lastLm35Ms >= LM35_READ_INTERVAL_MS) {
        lastLm35Ms = now;
        float prevTemp = state.currentTemp;
        bool prevHeat  = state.heatingOn;
        bool prevPump  = state.pumpOn;

        state.currentTemp = readRoomTempLM35();
        evaluateRoomHeatingIndicator();
        bool modeRestored = evaluateBoilerPumpProtection(now);

        if (modeRestored || prevHeat != state.heatingOn || prevPump != state.pumpOn ||
            fabsf(prevTemp - state.currentTemp) >= 0.2f) {
            sendRequestToNano();
            lastHeartbeatMs = millis();
            evaluateBoilerPumpProtection(lastHeartbeatMs);
            publishMqttStatus();
        }
    }

    // 3. V5 Master-Slave Heartbeat ping minden heartbeatIntervalSec másodpercben
    uint32_t heartbeatIntervalMs = static_cast<uint32_t>(state.heartbeatIntervalSec) * 1000UL;
    if (now - lastHeartbeatMs >= heartbeatIntervalMs) {
        lastHeartbeatMs = now;

        bool prevNrfOk       = state.nrf24Ok;
        bool prevPump        = state.pumpOn;
        uint8_t prevPState   = state.pumpState;
        uint8_t prevAlarmAct = state.alarmState.active;
        uint8_t prevAlarmRsn = state.alarmState.reason;

        sendRequestToNano();
        bool modeRestored = evaluateBoilerPumpProtection(millis());

        if (prevPump != state.pumpOn) {
            sendRequestToNano();
        }

        if (modeRestored || prevNrfOk != state.nrf24Ok ||
            prevPump != state.pumpOn || prevPState != state.pumpState ||
            prevAlarmAct != state.alarmState.active || prevAlarmRsn != state.alarmState.reason) {
            publishMqttStatus();
        }
    }

    // 4. Nem-blokkoló LCD és menü frissítés (ikonok + 100ms-es millis() ciklus)
    bool wifiMqttOk = (WiFi.status() == WL_CONNECTED) && mqtt.connected();
    lcd.setStatusIcons(state.pumpOn, state.nrf24Ok, wifiMqttOk);
    menu.update(now, alarmBlinkOn);
    lcd.refresh(now);

    // 5. WiFi és HiveMQ Cloud MQTT karbantartás
    maintainConnectivity(now);

    // 6. Telemetry 30 másodpercenként
    if (now - lastTelemetryMs >= TELEMETRY_INTERVAL_MS) {
        lastTelemetryMs = now;
        publishMqttTelemetry();
    }
}
