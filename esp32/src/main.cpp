#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <WiFiClientSecure.h>
#include <stdio.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <math.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#if __has_include(<esp_idf_version.h>)
#include <esp_idf_version.h>
#endif

#include <time.h>

#include "config.h"
#include "hivemq_ca.h"
#include "lcd.h"
#include "log.h"
#include "menu.h"
#include "spi_dma.h"
#include "../../nano/src/common.h"

// ============================================================================
// ESP32-C3 - SZOBA OLDALI MASTER / GATEWAY (V5 PROTOKOLL + GDMA SPI + MENÜ)
// ============================================================================
// - Master -> Slave REQUEST (0xA1) növekvő seq-el, nem-blokkoló állapotgéppel
//   (NRF_IDLE / NRF_WAITING), SPI2_HOST GDMA átvitellel (10 MHz, 32B DMA puffer),
//   max 500ms/próba, max 3 retry, AckPayload KI.
// - 2x20 I2C LCD menürendszer interrupt alapú rotary encoderrel (delay nélkül).
// - Preferences (NVS, "futes" namespace) tartós tárolás + 3 mp gyári reset.
// ============================================================================

SpiDmaBus spiDmaBus;
NrfDmaRadio nrfDma;
HeatingLcd lcd;
MenuController menu;
Preferences prefs;

WiFiClientSecure wifiSecureClient;
PubSubClient mqtt(wifiSecureClient);
WebServer wifiPortalServer(80);
DNSServer wifiPortalDns;

SystemState state;
Logger logger;

constexpr const char* NVS_NAMESPACE = "futes";
constexpr const char* WIFI_NVS_NAMESPACE = "wifi";
constexpr uint8_t WIFI_DNS_PORT = 53;
constexpr const char* WIFI_PORTAL_IP = "192.168.4.1";

// --- Időzítők ---
uint32_t lastLm35Ms              = 0;
uint32_t lastPing                = 0;
uint32_t lastNrfOk               = 0;
uint32_t lastTelemetryMs         = 0;
uint32_t lastMqttPublishRetryMs  = 0;
uint32_t lastWifiTryMs           = 0;
uint32_t lastMqttTryMs           = 0;
uint32_t mqttRetryIntervalMs     = MQTT_RECONNECT_MS;
bool     mqttAttemptedOnce       = false;
uint32_t lastAlarmBlinkMs        = 0;
uint32_t wifiDisconnectStartedMs = 0;
uint32_t lastWifiPortalTryMs     = 0;
uint32_t lastNtpAttemptMs        = 0;
uint32_t syncRetryStartMs        = 0;
uint32_t pendingTimeSyncEpoch    = 0;
uint32_t lastTrendSampleMs       = 0;
uint32_t uptimePrevMs            = 0;
uint64_t uptimeTotalMs           = 0;
int      lastScheduledYday       = -1;
int      lastScheduledSlot       = -1;
float    trendSamples[TREND_BUFFER_SIZE] = { 0 };
uint8_t  trendHead               = 0;
bool     bootSyncDone            = false;
bool     syncRetryWaiting        = false;
bool     ntpInitialTimeoutLogged = false;
bool     alarmBlinkOn            = false;
bool     wifiPortalActive        = false;
bool     wifiDisconnectTimerActive = false;
bool     wifiConnectionFailureReported = false;
bool     mqttStatusPendingRetry    = false;
bool     mqttTelemetryPendingRetry = false;
bool     nrfErrorBannerShown       = false;
bool     nrfFailLoggedForOutage    = false;
bool     nrfTimeoutLogged          = false;
bool     nrfFail                   = false;
bool     nrfCycleAnyWriteOk        = false;
bool     nrfImmediatePending       = false;
bool     prevWifiConnected         = false;
bool     prevMqttConnected         = false;
bool     ntpConfigured             = false;
bool     pumpLogInitialized        = false;
bool     prevPumpLogged            = false;
bool     prevAlarmActiveLogged     = false;
uint8_t  prevAlarmReasonLogged     = ALARM_REASON_NONE;
bool     lm35OutOfRangeLogged      = false;
String   wifiSsid;
String   wifiPassword;
const char* wifiPortalMessage = "Válaszd ki az otthoni Wi-Fi hálózatot.";

// --- Nem-blokkoló NRF24 Master-Slave állapotgép ---
enum NrfState : uint8_t { NRF_IDLE, NRF_WAITING };
NrfState nrfState              = NRF_IDLE;
uint32_t nrfSentAt             = 0;
uint8_t  nrfRetry              = 0;
const uint32_t NRF_TIMEOUT_MS  = NRF_RESPONSE_TIMEOUT_MS; // 500 ms
const uint8_t  NRF_MAX_RETRY   = NRF_MAX_ATTEMPTS;        // 3
NrfPacket nrfTxPacket{};
uint32_t  nrfTxSyncEpoch       = 0;
bool      dmaQueueFullLogged   = false;

// --- Végponttól végpontig tartó parancs-visszaigazolás (CMD_ACK) ---
bool     pendingCmdActive            = false;
char     pendingCmdId[64]            = { 0 };
uint8_t  pendingCmdSeq               = 0;
uint32_t pendingCmdAt                = 0;
bool     pendingCmdRequestedPumpOff  = false;
bool     pendingCmdSentInCurrentNrf  = false;
bool     pendingCmdWaitingSettle     = false;
uint32_t pendingCmdLastPollAt        = 0;
uint8_t  lastGeneratedCmdSeq         = 0;

// --- DPDT Relé visszacsatolás figyelés (500ms eltérés -> RELAY_FAULT) ---
bool     relayMismatchTiming         = false;
uint32_t relayMismatchStartedMs      = 0;
bool     prevRelayFaultLogged        = false;

// --- Loop Monitor (ESP32-C3, 71 perces micros() túlcsordulás-biztos) ---
uint32_t lastLoopUs                  = 0;
uint32_t maxLoopUs                   = 0;
uint32_t loopOverrunCount            = 0;
bool     loopFault                   = false;
bool     loopMonitorSuspend          = false;
uint8_t  espSlowLoopStreak           = 0;
uint8_t  espFastLoopStreak           = 0;
uint32_t espLastSlowLogMs            = 0;
bool     espHasSlowLogYet            = false;

// --- Nem-blokkoló DS18B20 Auto-Discovery / Tanítás NRF állapotgép (0xA3 / 0xB4) ---
enum SensorJobType : uint8_t {
    SJOB_NONE = 0,
    SJOB_ROLES_ONLY,
    SJOB_SCAN,
    SJOB_LEARN
};

enum SensorJobStep : uint8_t {
    SSTEP_IDLE = 0,
    SSTEP_SEND_SCAN_START,
    SSTEP_WAIT_CONV_DELAY,
    SSTEP_FETCH_FOUND,
    SSTEP_SEND_LEARN,
    SSTEP_FETCH_ROLES
};

SensorJobType sensorJobType          = SJOB_NONE;
SensorJobStep sensorJobStep          = SSTEP_IDLE;
uint32_t      sensorJobStartedMs     = 0;
uint32_t      sensorConvWaitStartMs  = 0;
uint8_t       sensorFetchOffset      = 0;
uint8_t       sensorScanReadyRetries = 0;
uint8_t       sensorLearnRole        = 0;
uint8_t       sensorLearnAddr[8]     = { 0 };
uint8_t       sensorCmdSeqCounter    = 0;
char          sensorMqttCmdId[64]    = { 0 };

constexpr uint32_t SENSOR_JOB_TIMEOUT_MS = 4500UL;

bool startSensorScanJob(const char* cmdId);
bool startSensorLearnJob(uint8_t role, const uint8_t addr[8], const char* cmdId);
bool startSensorRolesSyncJob();

// ============================================================================
// ESEMÉNY- ÉS HIBANAPLÓ (KÖRKÖRÖS PUFFER + SERIAL + AZONNALI MQTT ERROR)
// ============================================================================
static char logsResponsePayload[3584];

void publishMqttLogError(const LogEntry& entry) {
    if (!mqtt.connected()) {
        return;
    }

    JsonDocument doc;
    doc["id"]        = entry.id;
    doc["timestamp"] = entry.timestamp;
    doc["level"]     = Logger::levelToStr(entry.level);
    doc["code"]      = entry.code;
    doc["message"]   = entry.message;

    char payload[256];
    size_t n = serializeJson(doc, payload, sizeof(payload));
    if (n == 0) {
        return;
    }
    if (!mqtt.publish(TOPIC_LOG, reinterpret_cast<const uint8_t*>(payload), n, false)) {
        // Rekurzió elkerülése végett hiba esetén CSAK Serial-ra írunk
        Serial.print(F("MQTT PUBLISH FAILED topic="));
        Serial.println(TOPIC_LOG);
    }
}

void logEvent(uint8_t level, const char* code, const char* message) {
    LogEntry entry{};
    logger.addEntry(level, code, message, &entry);

    const char* tsSerial = entry.timestamp;
    if (strlen(entry.timestamp) >= 19 && entry.timestamp[10] == ' ') {
        tsSerial = &entry.timestamp[11];
    }
    Serial.printf("[%s] %s %s %s\n",
                  Logger::levelToStr(entry.level),
                  tsSerial,
                  entry.code,
                  entry.message);

    if (entry.level == LOG_ERROR) {
        publishMqttLogError(entry);
    }
}

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
    // OVERFLOW-SAFE: uint32_t kivonás, 49 napos millis túlcsorduláskor is helyes.
    if (static_cast<uint32_t>(now - isrBtnEdgeMs) >= BTN_DEBOUNCE_MS) {
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
        case ALARM_REASON_PUMP_FAULT:  return "pump_fault";
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
    if (isnan(cfg.alarmTemp)) {
        cfg.alarmTemp = ALARM_OVERHEAT_TEMP_DEFAULT;
    }
    if (cfg.alarmTemp < ALARM_OVERHEAT_TEMP_MIN) cfg.alarmTemp = ALARM_OVERHEAT_TEMP_MIN;
    if (cfg.alarmTemp > ALARM_OVERHEAT_TEMP_MAX) cfg.alarmTemp = ALARM_OVERHEAT_TEMP_MAX;
    cfg.alarmTemp = round1(cfg.alarmTemp);
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
    state.alarmConfig.alarmTemp      = ALARM_OVERHEAT_TEMP_DEFAULT; // 85.0
    state.alarmConfig.alarmEnabled   = ALARM_ENABLED_DEFAULT ? 1 : 0; // true
    state.heartbeatIntervalSec       = HEARTBEAT_DEFAULT_SEC; // 5
    state.outputsMask                = 0;
    state.userLedPiros               = false;
    state.pumpState                  = PUMP_STATE_VARAKOZAS;
}

enum NvsValueType : uint8_t {
    NVS_TYPE_FLOAT = 0,
    NVS_TYPE_UINT  = 1,
    NVS_TYPE_BOOL  = 2,
    NVS_TYPE_STR   = 3
};

struct PendingNvsItem {
    bool         used = false;
    char         key[24] = {0};
    NvsValueType type = NVS_TYPE_UINT;
    float        fVal = 0.0f;
    uint32_t     uVal = 0;
    bool         bVal = false;
    char         sVal[65] = {0};
};

constexpr uint8_t MAX_PENDING_NVS_ITEMS = 12;
PendingNvsItem pendingNvsItems[MAX_PENDING_NVS_ITEMS];
bool           pendingNvsRetryActive = false;
bool           inNvsRetry            = false;

void queuePendingNvsItem(const char* key,
                         NvsValueType type,
                         float fVal,
                         uint32_t uVal,
                         bool bVal,
                         const char* sVal) {
    if (!key) return;
    int8_t slot = -1;
    for (uint8_t i = 0; i < MAX_PENDING_NVS_ITEMS; i++) {
        if (pendingNvsItems[i].used && strcmp(pendingNvsItems[i].key, key) == 0) {
            slot = static_cast<int8_t>(i);
            break;
        }
        if (!pendingNvsItems[i].used && slot < 0) {
            slot = static_cast<int8_t>(i);
        }
    }
    if (slot < 0) {
        slot = 0;
    }

    PendingNvsItem& item = pendingNvsItems[slot];
    item.used = true;
    strncpy(item.key, key, sizeof(item.key) - 1);
    item.key[sizeof(item.key) - 1] = '\0';
    item.type = type;
    item.fVal = fVal;
    item.uVal = uVal;
    item.bVal = bVal;
    if (sVal) {
        strncpy(item.sVal, sVal, sizeof(item.sVal) - 1);
        item.sVal[sizeof(item.sVal) - 1] = '\0';
    } else {
        item.sVal[0] = '\0';
    }
    pendingNvsRetryActive = true;
}

bool saveSettingInternal(const char* key,
                         NvsValueType type,
                         float fVal,
                         uint32_t uVal,
                         bool bVal,
                         const char* sVal) {
    if (!key) return false;

    if (!prefs.begin(NVS_NAMESPACE, false)) {
        logEvent(LOG_ERROR, "NVS_FAIL", "NVS BEGIN FAILED");
        menu.showErrorBanner("NVS HIBA!", "Mentes sikertelen", 2000);
        if (!inNvsRetry) {
            queuePendingNvsItem(key, type, fVal, uVal, bVal, sVal);
        }
        return false;
    }

    size_t written = 0;
    switch (type) {
        case NVS_TYPE_FLOAT:
            written = prefs.putFloat(key, fVal);
            break;
        case NVS_TYPE_UINT:
            written = prefs.putUInt(key, uVal);
            break;
        case NVS_TYPE_BOOL:
            written = prefs.putBool(key, bVal);
            break;
        case NVS_TYPE_STR:
            written = prefs.putString(key, sVal ? sVal : "");
            break;
    }
    prefs.end();

    if (written == 0) {
        char msg[48];
        snprintf(msg, sizeof(msg), "NVS WRITE FAILED key=%s", key);
        logEvent(LOG_ERROR, "NVS_FAIL", msg);
        menu.showErrorBanner("MENTES HIBA!", key, 2000);
        if (!inNvsRetry) {
            queuePendingNvsItem(key, type, fVal, uVal, bVal, sVal);
        }
        return false;
    }

    return true;
}

bool saveSetting(const char* key, float value) {
    return saveSettingInternal(key, NVS_TYPE_FLOAT, value, 0, false, nullptr);
}

bool saveSetting(const char* key, uint32_t value) {
    return saveSettingInternal(key, NVS_TYPE_UINT, 0.0f, value, false, nullptr);
}

bool saveSetting(const char* key, bool value) {
    return saveSettingInternal(key, NVS_TYPE_BOOL, 0.0f, 0, value, nullptr);
}

bool saveSetting(const char* key, const char* value) {
    return saveSettingInternal(key, NVS_TYPE_STR, 0.0f, 0, false, value);
}

void applyPendingNvsItemToState(const PendingNvsItem& item, uint32_t now) {
    if (strcmp(item.key, "targetTemp") == 0) {
        state.targetTemp = item.fVal;
    } else if (strcmp(item.key, "mode") == 0) {
        state.mode = static_cast<uint8_t>(item.uVal);
    } else if (strcmp(item.key, "manualHeating") == 0) {
        state.manualHeating = item.bVal;
    } else if (strcmp(item.key, "pumpMode") == 0) {
        uint8_t nextPumpMode = static_cast<uint8_t>(item.uVal);
        if (nextPumpMode == PUMP_MODE_MANUAL_OFF && state.pumpConfig.pumpMode != PUMP_MODE_MANUAL_OFF) {
            state.manualOffStartMs = now;
        }
        state.pumpConfig.pumpMode = nextPumpMode;
    } else if (strcmp(item.key, "pumpStartTemp") == 0) {
        state.pumpConfig.pumpStartTemp = item.fVal;
    } else if (strcmp(item.key, "pumpStopTemp") == 0) {
        state.pumpConfig.pumpStopTemp = item.fVal;
    } else if (strcmp(item.key, "pumpSensor") == 0) {
        state.pumpConfig.pumpStartSensor = static_cast<uint8_t>(item.uVal);
    } else if (strcmp(item.key, "alarmTemp") == 0 || strcmp(item.key, "overheatTemp") == 0) {
        state.alarmConfig.alarmTemp = item.fVal;
    } else if (strcmp(item.key, "alarmEnabled") == 0) {
        state.alarmConfig.alarmEnabled = item.bVal ? 1 : 0;
    } else if (strcmp(item.key, "heartbeatSec") == 0) {
        state.heartbeatIntervalSec = static_cast<uint8_t>(item.uVal);
    } else if (strcmp(item.key, "outputsMask") == 0) {
        uint8_t nextMask = static_cast<uint8_t>(item.uVal);
        setBit(nextMask, OUT_BIT_SZIVATTYU, state.pumpOn);
        state.outputsMask  = nextMask;
        state.userLedPiros = getBit(nextMask, OUT_BIT_LED_PIROS);
    }
}

void commitSettingEffects(bool redrawMenu);
void onMenuSettingCommitted();

void processPendingNvsRetry(uint32_t now) {
    if (!pendingNvsRetryActive) return;

    PendingNvsItem retryItems[MAX_PENDING_NVS_ITEMS];
    for (uint8_t i = 0; i < MAX_PENDING_NVS_ITEMS; i++) {
        retryItems[i] = pendingNvsItems[i];
        pendingNvsItems[i].used = false;
    }
    pendingNvsRetryActive = false;

    inNvsRetry = true;
    bool allOk = true;
    for (uint8_t i = 0; i < MAX_PENDING_NVS_ITEMS; i++) {
        if (!retryItems[i].used) continue;
        if (!saveSettingInternal(retryItems[i].key,
                                 retryItems[i].type,
                                 retryItems[i].fVal,
                                 retryItems[i].uVal,
                                 retryItems[i].bVal,
                                 retryItems[i].sVal)) {
            allOk = false;
            break;
        }
    }
    inNvsRetry = false;

    if (allOk) {
        for (uint8_t i = 0; i < MAX_PENDING_NVS_ITEMS; i++) {
            if (retryItems[i].used) {
                applyPendingNvsItemToState(retryItems[i], now);
            }
        }
        logEvent(LOG_INFO, "NVS_OK", "Ujraprobalt NVS mentes sikeres");
        commitSettingEffects(!menu.isEditing());
    }
}

uint32_t readUIntCompat(Preferences& p, const char* key, uint32_t defaultVal) {
    if (!p.isKey(key)) return defaultVal;
    if (p.getType(key) == PT_U8) {
        return static_cast<uint32_t>(p.getUChar(key, static_cast<uint8_t>(defaultVal)));
    }
    return p.getUInt(key, defaultVal);
}

uint8_t computeSavedOutputsMask(const SystemState& src) {
    uint8_t savedOutputsMask = src.outputsMask & static_cast<uint8_t>(~(1U << OUT_BIT_SZIVATTYU));
    setBit(savedOutputsMask, OUT_BIT_LED_PIROS, src.userLedPiros);
    return savedOutputsMask;
}

bool saveChangedSettingsFromState(const SystemState& prev, const SystemState& next) {
    bool allOk = true;

    if (fabsf(prev.targetTemp - next.targetTemp) > 0.01f) {
        if (!saveSetting("targetTemp", next.targetTemp)) allOk = false;
    }
    if (prev.mode != next.mode) {
        if (!saveSetting("mode", static_cast<uint32_t>(next.mode))) allOk = false;
    }
    if (prev.manualHeating != next.manualHeating) {
        if (!saveSetting("manualHeating", next.manualHeating)) allOk = false;
    }
    if (prev.pumpConfig.pumpMode != next.pumpConfig.pumpMode) {
        if (!saveSetting("pumpMode", static_cast<uint32_t>(next.pumpConfig.pumpMode))) allOk = false;
    }
    if (fabsf(prev.pumpConfig.pumpStartTemp - next.pumpConfig.pumpStartTemp) > 0.01f) {
        if (!saveSetting("pumpStartTemp", next.pumpConfig.pumpStartTemp)) allOk = false;
    }
    if (fabsf(prev.pumpConfig.pumpStopTemp - next.pumpConfig.pumpStopTemp) > 0.01f) {
        if (!saveSetting("pumpStopTemp", next.pumpConfig.pumpStopTemp)) allOk = false;
    }
    if (prev.pumpConfig.pumpStartSensor != next.pumpConfig.pumpStartSensor) {
        if (!saveSetting("pumpSensor", static_cast<uint32_t>(next.pumpConfig.pumpStartSensor))) allOk = false;
    }
    if (fabsf(prev.alarmConfig.alarmTemp - next.alarmConfig.alarmTemp) > 0.01f) {
        if (!saveSetting("alarmTemp", next.alarmConfig.alarmTemp)) allOk = false;
    }
    if (prev.alarmConfig.alarmEnabled != next.alarmConfig.alarmEnabled) {
        if (!saveSetting("alarmEnabled", next.alarmConfig.alarmEnabled != 0)) allOk = false;
    }
    if (prev.heartbeatIntervalSec != next.heartbeatIntervalSec) {
        if (!saveSetting("heartbeatSec", static_cast<uint32_t>(next.heartbeatIntervalSec))) allOk = false;
    }

    uint8_t prevSavedMask = computeSavedOutputsMask(prev);
    uint8_t nextSavedMask = computeSavedOutputsMask(next);
    if (prevSavedMask != nextSavedMask) {
        if (!saveSetting("outputsMask", static_cast<uint32_t>(nextSavedMask))) allOk = false;
    }

    return allOk;
}

void loadSettingsFromNvs() {
    applyDefaultSettings();

    if (!prefs.begin(NVS_NAMESPACE, true)) {
        logEvent(LOG_INFO, "NVS_LOAD", "Nincs mentett NVS, alapertelmezes");
        return;
    }

    float tTemp = prefs.getFloat("targetTemp", TARGET_TEMP_DEFAULT);
    if (!isnan(tTemp) && tTemp >= TARGET_TEMP_MIN && tTemp <= TARGET_TEMP_MAX) {
        state.targetTemp = roundf(tTemp * 2.0f) / 2.0f;
    }

    uint32_t m = readUIntCompat(prefs, "mode", MODE_AUTO);
    state.mode = (m <= MODE_OFF) ? static_cast<uint8_t>(m) : MODE_AUTO;

    state.manualHeating = prefs.getBool("manualHeating", false);

    uint32_t pm = readUIntCompat(prefs, "pumpMode", PUMP_MODE_AUTO);
    state.pumpConfig.pumpMode = (pm <= PUMP_MODE_MANUAL_OFF) ? static_cast<uint8_t>(pm) : PUMP_MODE_AUTO;

    state.pumpConfig.pumpStartTemp = prefs.getFloat("pumpStartTemp", PUMP_START_TEMP_DEFAULT);
    state.pumpConfig.pumpStopTemp  = prefs.getFloat("pumpStopTemp", PUMP_STOP_TEMP_DEFAULT);

    uint32_t ps = readUIntCompat(prefs, "pumpSensor", PUMP_SENSOR_KAZAN);
    state.pumpConfig.pumpStartSensor = (ps <= PUMP_SENSOR_MAX) ? static_cast<uint8_t>(ps) : PUMP_SENSOR_KAZAN;
    sanitizePumpConfig(state.pumpConfig);

    if (prefs.isKey("alarmTemp")) {
        state.alarmConfig.alarmTemp = prefs.getFloat("alarmTemp", ALARM_OVERHEAT_TEMP_DEFAULT);
    } else {
        state.alarmConfig.alarmTemp = prefs.getFloat("overheatTemp", ALARM_OVERHEAT_TEMP_DEFAULT);
    }
    state.alarmConfig.alarmEnabled = prefs.getBool("alarmEnabled", ALARM_ENABLED_DEFAULT) ? 1 : 0;
    sanitizeAlarmConfig(state.alarmConfig);

    uint32_t hb = readUIntCompat(prefs, "heartbeatSec", HEARTBEAT_DEFAULT_SEC);
    state.heartbeatIntervalSec = isValidHeartbeatInterval(static_cast<int>(hb))
                                     ? static_cast<uint8_t>(hb)
                                     : HEARTBEAT_DEFAULT_SEC;

    uint8_t loadedMask = static_cast<uint8_t>(readUIntCompat(prefs, "outputsMask", 0) & 0xFFU);
    setBit(loadedMask, OUT_BIT_SZIVATTYU, false);
    state.outputsMask  = loadedMask;
    state.userLedPiros = getBit(loadedMask, OUT_BIT_LED_PIROS);

    prefs.end();
    logEvent(LOG_INFO, "NVS_LOAD", "Beallitasok betoltve");
}

void clearNvsAndResetDefaults() {
    if (!prefs.begin(NVS_NAMESPACE, false)) {
        logEvent(LOG_ERROR, "NVS_FAIL", "NVS BEGIN FAILED");
        menu.showErrorBanner("NVS HIBA!", "Torles hiba", 2000);
    } else {
        if (!prefs.clear()) {
            logEvent(LOG_ERROR, "NVS_FAIL", "NVS WRITE FAILED key=clear");
            menu.showErrorBanner("MENTES HIBA!", "Torles hiba", 2000);
        }
        prefs.end();
    }
    for (uint8_t i = 0; i < MAX_PENDING_NVS_ITEMS; i++) {
        pendingNvsItems[i].used = false;
    }
    pendingNvsRetryActive = false;
    applyDefaultSettings();
    logEvent(LOG_WARN, "FACTORY_RESET", "Gyari visszaallitas kesz");
}

// ============================================================================
// LM35 SZOBAI HŐMÉRSÉKLET OLVASÁSA (10 MÉRÉS ÁTLAGA, 10 mV / °C)
// ============================================================================
float readRoomTempLM35() {
    uint32_t sumRaw = 0;
    for (uint8_t i = 0; i < LM35_SAMPLE_COUNT; i++) {
        sumRaw += static_cast<uint32_t>(analogRead(PIN_LM35));
    }
    float avgRaw = static_cast<float>(sumRaw) / static_cast<float>(LM35_SAMPLE_COUNT);
    float avgMv = (avgRaw / 4095.0f) * 3300.0f;
    float tempC = (avgMv / 10.0f) + LM35_OFFSET;

    if (isnan(tempC) || tempC < -10.0f || tempC > 60.0f) {
        if (!lm35OutOfRangeLogged) {
            lm35OutOfRangeLogged = true;
            char msg[48];
            snprintf(msg, sizeof(msg), "LM35_OUT_OF_RANGE %.1fC", tempC);
            logEvent(LOG_ERROR, "LM35_FAIL", msg);
        }
        return state.currentTemp;
    }

    lm35OutOfRangeLogged = false;
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

bool evaluateRelayFault(uint32_t now, bool nanoReportedMismatch) {
    if (!state.hasRelayFeedback && !nanoReportedMismatch) {
        return false;
    }

    bool prevFault = state.relayFault;
    if (state.pumpOn == state.relayFeedbackOn && !nanoReportedMismatch) {
        relayMismatchTiming = false;
        state.relayFault    = false;
    } else if (state.pumpOn != state.relayFeedbackOn) {
        if (!relayMismatchTiming) {
            relayMismatchTiming    = true;
            relayMismatchStartedMs = now;
        }
        // OVERFLOW-SAFE: uint32_t kivonás, 49 napos millis túlcsorduláskor is helyes.
        if (nanoReportedMismatch ||
            static_cast<uint32_t>(now - relayMismatchStartedMs) >= RELAY_FAULT_DELAY_MS) {
            state.relayFault = true;
        }
    }

    if (state.relayFault != prevRelayFaultLogged) {
        prevRelayFaultLogged = state.relayFault;
        if (state.relayFault) {
            char msg[48];
            snprintf(msg, sizeof(msg), "commanded=%s feedback=%s",
                     state.pumpOn ? "BE" : "KI",
                     state.relayFeedbackOn ? "BE" : "KI");
            logEvent(LOG_WARN, "RELAY_FAULT", msg);
        } else {
            logEvent(LOG_INFO, "RELAY_OK", "Rele allapot ismet egyezik");
        }
    }

    return prevFault != state.relayFault;
}

void evaluateAlarmState() {
    bool alarmOverheat = isSensorValid(state.kazan) && (state.kazan >= state.alarmConfig.alarmTemp);

    if (state.alarmConfig.alarmEnabled == 0) {
        state.alarmState.active = 0;
        state.alarmState.reason = ALARM_REASON_NONE;
    } else if (alarmOverheat ||
               state.safetyOverheat ||
               (state.nanoAlarmState.active != 0 && state.nanoAlarmState.reason == ALARM_REASON_OVERHEAT)) {
        // Saját és Nano-tól jött riasztás egyesítése (OR logika, prioritási sorrendben)
        state.alarmState.active = 1;
        state.alarmState.reason = ALARM_REASON_OVERHEAT;
    } else if (!state.nrf24Ok && state.consecutiveMissedPings >= NRF_MAX_MISSED_CYCLES) {
        state.alarmState.active = 1;
        state.alarmState.reason = ALARM_REASON_NRF_FAIL;
    } else if (state.relayFault ||
               (state.nanoAlarmState.active != 0 && state.nanoAlarmState.reason == ALARM_REASON_PUMP_FAULT)) {
        state.alarmState.active = 1;
        state.alarmState.reason = ALARM_REASON_PUMP_FAULT;
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

    bool activeNow = (state.alarmState.active != 0);
    if (activeNow && (!prevAlarmActiveLogged || prevAlarmReasonLogged != state.alarmState.reason)) {
        char msg[48];
        switch (state.alarmState.reason) {
            case ALARM_REASON_OVERHEAT:
                if (isSensorValid(state.kazan)) {
                    snprintf(msg, sizeof(msg), "Kazan tulmelegedes %.1fC", state.kazan);
                } else {
                    snprintf(msg, sizeof(msg), "Kazan tulmelegedes");
                }
                logEvent(LOG_ERROR, "ALARM_OVERHEAT", msg);
                break;
            case ALARM_REASON_SENSOR_FAIL:
                logEvent(LOG_ERROR, "ALARM_SENSOR",
                         state.sensorUnknown ? "Szenzor ismeretlen - tanitas szukseges" : "Kazan szenzor hiba (-127)");
                break;
            case ALARM_REASON_PUMP_FAULT:
                logEvent(LOG_ERROR, "ALARM_PUMP", "Rele / szivattyu visszacsatolasi hiba");
                break;
            case ALARM_REASON_NRF_FAIL:
                logEvent(LOG_ERROR, "ALARM_NRF", "NRF24 kommunikacios hiba");
                break;
            default:
                snprintf(msg, sizeof(msg), "Riasztas aktiv kod=%u", state.alarmState.reason);
                logEvent(LOG_ERROR, "ALARM_ACTIVE", msg);
                break;
        }
    } else if (!activeNow && prevAlarmActiveLogged) {
        logEvent(LOG_INFO, "ALARM_CLEAR", "Riasztas megszunt");
    }
    prevAlarmActiveLogged = activeNow;
    prevAlarmReasonLogged = state.alarmState.reason;
}

bool evaluateBoilerPumpProtection(uint32_t now, bool nanoReportedMismatch = false) {
    bool modeAutoRestored = false;

    // 10 perces manual_off automatikus visszaállás
    // OVERFLOW-SAFE: uint32_t kivonás, 49 napos millis túlcsorduláskor is helyes.
    if (state.pumpConfig.pumpMode == PUMP_MODE_MANUAL_OFF) {
        if (static_cast<uint32_t>(now - state.manualOffStartMs) >= PUMP_MANUAL_OFF_TIMEOUT_MS) {
            state.pumpConfig.pumpMode = PUMP_MODE_AUTO;
            modeAutoRestored = true;
            logEvent(LOG_INFO, "PUMP_AUTO", "10p letelt, manual_off->auto");
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

    // Biztonsági felülbírálás (CSAK a fix SAFETY_OVERHEAT_TEMP 95.0C és a szenzorhiba -127 számít, SOHA az alarmTemp)
    // SAFETY: alarmEnabled nem kapcsolja ki a pumpavédelmet
    bool kazanValid = isSensorValid(state.kazan);
    state.safetySensorFault = !kazanValid;
    state.safetyOverheat    = kazanValid && (state.kazan >= SAFETY_OVERHEAT_TEMP);

    if (state.safetyOverheat || state.safetySensorFault) {
        desiredPump = true;
        state.pumpState = PUMP_STATE_FUTES;
    }

    // Ha a kazánnál lévő fizikai szerviz kapcsoló BE van kapcsolva, az ESP soha ne küldjön KI parancsot
    if (state.serviceMode) {
        desiredPump = true;
        state.pumpState = PUMP_STATE_FUTES;
    }

    state.pumpOn = desiredPump;

    if (!pumpLogInitialized) {
        pumpLogInitialized = true;
        prevPumpLogged     = state.pumpOn;
        if (state.pumpOn) {
            char msg[48];
            if (state.safetyOverheat) {
                snprintf(msg, sizeof(msg), "Szivattyu BE: kazan %.1fC tulmel", state.kazan);
            } else if (state.safetySensorFault) {
                snprintf(msg, sizeof(msg), "Szivattyu BE: szenzor hiba");
            } else if (state.pumpConfig.pumpMode == PUMP_MODE_MANUAL_ON) {
                snprintf(msg, sizeof(msg), "Szivattyu BE: manual");
            } else {
                float refT = 0.0f;
                if (getSelectedStartSensorTemp(refT)) {
                    snprintf(msg, sizeof(msg), "Szivattyu BE: kazan %.1fC", refT);
                } else {
                    snprintf(msg, sizeof(msg), "Szivattyu BE");
                }
            }
            logEvent(LOG_INFO, "PUMP_ON", msg);
        }
    } else if (state.pumpOn != prevPumpLogged) {
        prevPumpLogged = state.pumpOn;
        char msg[48];
        if (state.pumpOn) {
            if (state.safetyOverheat) {
                snprintf(msg, sizeof(msg), "Szivattyu BE: kazan %.1fC tulmel", state.kazan);
            } else if (state.safetySensorFault) {
                snprintf(msg, sizeof(msg), "Szivattyu BE: szenzor hiba");
            } else if (state.pumpConfig.pumpMode == PUMP_MODE_MANUAL_ON) {
                snprintf(msg, sizeof(msg), "Szivattyu BE: manual");
            } else {
                float refT = 0.0f;
                if (state.pumpConfig.pumpStartSensor == PUMP_SENSOR_ELOREMENO && isSensorValid(state.eloremeno)) {
                    snprintf(msg, sizeof(msg), "Szivattyu BE: eloremeno %.1fC", state.eloremeno);
                } else if (getSelectedStartSensorTemp(refT)) {
                    snprintf(msg, sizeof(msg), "Szivattyu BE: kazan %.1fC", refT);
                } else {
                    snprintf(msg, sizeof(msg), "Szivattyu BE");
                }
            }
            logEvent(LOG_INFO, "PUMP_ON", msg);
        } else {
            if (state.pumpConfig.pumpMode == PUMP_MODE_MANUAL_OFF) {
                snprintf(msg, sizeof(msg), "Szivattyu KI: manual");
            } else if (isSensorValid(state.eloremeno)) {
                snprintf(msg, sizeof(msg), "Szivattyu KI: eloremeno %.1fC", state.eloremeno);
            } else {
                snprintf(msg, sizeof(msg), "Szivattyu KI");
            }
            logEvent(LOG_INFO, "PUMP_OFF", msg);
        }
    }

    evaluateRelayFault(now, nanoReportedMismatch);
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

    // OVERFLOW-SAFE: uint32_t kivonás, 49 napos millis túlcsorduláskor is helyes.
    if (static_cast<uint32_t>(now - lastAlarmBlinkMs) >= ALARM_BLINK_INTERVAL_MS) {
        lastAlarmBlinkMs = now;
        alarmBlinkOn = !alarmBlinkOn;
    }
    digitalWrite(PIN_ALARM_LED, alarmBlinkOn ? HIGH : LOW);
}

// Forward declarations a szinkronizációhoz
bool publishMqttStatus();

// ============================================================================
// V5 MASTER-SLAVE NRF24 KOMMUNIKÁCIÓ (NEM-BLOKKOLÓ ÁLLAPOTGÉP: 0xA1 -> 0xB2)
// ============================================================================
void formatEspIsoTimeStr(char* out, size_t len, uint32_t uptimeSec) {
    if (!out || len == 0) return;
    if (state.ntpSynced) {
        time_t epochNow = time(nullptr);
        if (epochNow >= 1700000000) {
            struct tm tmUtc{};
            if (gmtime_r(&epochNow, &tmUtc)) {
                strftime(out, len, "%Y-%m-%dT%H:%M:%SZ", &tmUtc);
                return;
            }
        }
    }
    snprintf(out, len, "UP %lus", static_cast<unsigned long>(uptimeSec));
}

void publishMqttCmdAck(const char* cmdId, bool ok, const char* reason, bool pumpOn) {
    if (!cmdId || cmdId[0] == '\0' || !mqtt.connected()) {
        return;
    }

    uint32_t uptimeSec = static_cast<uint32_t>(uptimeTotalMs / 1000ULL);
    char execAt[32];
    formatEspIsoTimeStr(execAt, sizeof(execAt), uptimeSec);

    JsonDocument doc;
    doc["cmdId"]      = cmdId;
    doc["ok"]         = ok;
    doc["pumpOn"]     = pumpOn;
    doc["executedAt"] = execAt;
    if (!ok && reason && reason[0] != '\0') {
        doc["reason"] = reason;
    }

    char payload[256];
    size_t n = serializeJson(doc, payload, sizeof(payload));
    if (n == 0) {
        return;
    }

    bool ok1 = mqtt.publish(TOPIC_ACK, reinterpret_cast<const uint8_t*>(payload), n, false);
    bool ok2 = mqtt.publish(TOPIC_ACK_ALT, reinterpret_cast<const uint8_t*>(payload), n, false);
    if (!ok1 && !ok2) {
        logEvent(LOG_WARN, "MQTT_PUB_FAIL", "MQTT PUBLISH FAILED cmd_ack");
    }
}

void publishMqttSensorScanAck(const char* cmdId, bool ok, const char* reason) {
    if (!cmdId || cmdId[0] == '\0' || !mqtt.connected()) {
        return;
    }

    uint32_t uptimeSec = static_cast<uint32_t>(uptimeTotalMs / 1000ULL);
    char execAt[32];
    formatEspIsoTimeStr(execAt, sizeof(execAt), uptimeSec);

    JsonDocument doc;
    doc["cmdId"]      = cmdId;
    doc["ok"]         = ok;
    doc["executedAt"] = execAt;
    if (!ok && reason && reason[0] != '\0') {
        doc["reason"] = reason;
    }

    JsonArray arr = doc["found"].to<JsonArray>();
    if (ok) {
        for (uint8_t i = 0; i < state.scanFoundCount && i < MAX_DISCOVERED_SENSORS; i++) {
            JsonObject item = arr.add<JsonObject>();
            char addrBuf[24];
            formatDeviceAddressHex(state.scanFound[i].address, addrBuf, sizeof(addrBuf));
            item["address"] = addrBuf;
            if (isSensorValid(state.scanFound[i].temp)) {
                item["temp"] = round1(state.scanFound[i].temp);
            } else {
                item["temp"] = nullptr;
            }
        }
    }

    char payload[768];
    size_t n = serializeJson(doc, payload, sizeof(payload));
    if (n == 0) {
        return;
    }

    bool ok1 = mqtt.publish(TOPIC_ACK, reinterpret_cast<const uint8_t*>(payload), n, false);
    bool ok2 = mqtt.publish(TOPIC_ACK_ALT, reinterpret_cast<const uint8_t*>(payload), n, false);
    if (!ok1 && !ok2) {
        logEvent(LOG_WARN, "MQTT_PUB_FAIL", "MQTT PUBLISH FAILED sensor_scan_ack");
    }
}

void clearPendingCmd() {
    pendingCmdActive           = false;
    pendingCmdSentInCurrentNrf = false;
    pendingCmdRequestedPumpOff = false;
    pendingCmdWaitingSettle    = false;
    pendingCmdLastPollAt       = 0;
    pendingCmdSeq              = 0;
    pendingCmdId[0]            = '\0';
}

uint8_t nextCmdSeqFromCmdId(const JsonDocument& doc, const char* cmdId) {
    if (doc["cmdSeq"].is<int>()) {
        uint8_t s = static_cast<uint8_t>(doc["cmdSeq"].as<int>() & 0xFF);
        if (s != 0) {
            return s;
        }
    }
    uint8_t hash = 0;
    if (cmdId) {
        for (size_t i = 0; cmdId[i] != '\0'; i++) {
            hash = static_cast<uint8_t>((hash * 33U) + static_cast<uint8_t>(cmdId[i]));
        }
    }
    if (hash == 0) {
        lastGeneratedCmdSeq++;
        if (lastGeneratedCmdSeq == 0) {
            lastGeneratedCmdSeq = 1;
        }
        hash = lastGeneratedCmdSeq;
    }
    return hash;
}

void buildNrfRequestPacket() {
    state.txSeq++;
    if (state.txSeq == 0) {
        state.txSeq = 1;
    }

    nrfTxSyncEpoch             = pendingTimeSyncEpoch;
    pendingCmdSentInCurrentNrf = pendingCmdActive;

    memset(&nrfTxPacket, 0, sizeof(nrfTxPacket));
    nrfTxPacket.packetType               = PKT_TYPE_REQUEST;
    nrfTxPacket.esp.outputsMask          = state.outputsMask;
    nrfTxPacket.esp.targetTemp           = state.targetTemp;
    nrfTxPacket.esp.modeAndManual        = packModeAndManual(state.mode, state.manualHeating);
    nrfTxPacket.esp.cmdSeq               = pendingCmdActive ? pendingCmdSeq : 0;
    nrfTxPacket.esp.pumpConfig           = state.pumpConfig;
    nrfTxPacket.esp.alarmConfig          = state.alarmConfig;
    nrfTxPacket.esp.heartbeatIntervalSec = state.heartbeatIntervalSec;
    nrfTxPacket.esp.seq                  = state.txSeq;
    nrfTxPacket.esp.timeSyncEpoch        = nrfTxSyncEpoch;
}

void transmitCurrentNrfPacket() {
    bool queued = nrfDma.queueTransmitPacket(nrfTxPacket);
    state.dmaWaitUs = nrfDma.getLastWaitUs();
    if (nrfDma.consumeQueueFullWarning() && !dmaQueueFullLogged) {
        dmaQueueFullLogged = true;
        logEvent(LOG_WARN, "DMA_QUEUE_FULL", "NRF SPI DMA sor megtelt (3)");
    }
    if (!queued) {
        // Queue hiba vagy rádió nincs inicializálva: az 500 ms-os NRF timeout fogja kezelni
    }
}

void startNrfRequest(uint32_t now) {
    buildNrfRequestPacket();
    nrfCycleAnyWriteOk = false;
    nrfRetry           = 0;
    nrfSentAt          = now;
    nrfState           = NRF_WAITING;
    transmitCurrentNrfPacket();
}

void requestImmediateNrfSend() {
    nrfImmediatePending = true;
}

uint8_t nextSensorCmdSeq() {
    sensorCmdSeqCounter++;
    if (sensorCmdSeqCounter == 0) {
        sensorCmdSeqCounter = 1;
    }
    return sensorCmdSeqCounter;
}

void buildSensorNrfPacket(uint8_t subCmd, uint8_t roleOrIndex, const uint8_t* addr) {
    state.txSeq++;
    if (state.txSeq == 0) {
        state.txSeq = 1;
    }

    memset(&nrfTxPacket, 0, sizeof(nrfTxPacket));
    nrfTxPacket.packetType            = PKT_TYPE_SENSOR_CMD;
    nrfTxPacket.sensorCmd.subCmd      = subCmd;
    nrfTxPacket.sensorCmd.cmdSeq      = nextSensorCmdSeq();
    nrfTxPacket.sensorCmd.roleOrIndex = roleOrIndex;
    if (addr) {
        memcpy(nrfTxPacket.sensorCmd.address, addr, 8);
    }
    nrfTxPacket.sensorCmd.seq         = state.txSeq;
}

void startSensorNrfStepPacket(uint32_t now) {
    uint8_t subCmd = SENSOR_CMD_GET_ROLES;
    uint8_t roleOrIdx = sensorFetchOffset;
    const uint8_t* addr = nullptr;

    switch (sensorJobStep) {
        case SSTEP_SEND_SCAN_START:
            subCmd = SENSOR_CMD_SCAN_START;
            roleOrIdx = 0;
            break;
        case SSTEP_FETCH_FOUND:
            subCmd = SENSOR_CMD_GET_FOUND;
            roleOrIdx = sensorFetchOffset;
            break;
        case SSTEP_SEND_LEARN:
            subCmd = SENSOR_CMD_LEARN;
            roleOrIdx = sensorLearnRole;
            addr = sensorLearnAddr;
            break;
        case SSTEP_FETCH_ROLES:
        default:
            subCmd = SENSOR_CMD_GET_ROLES;
            roleOrIdx = sensorFetchOffset;
            break;
    }

    buildSensorNrfPacket(subCmd, roleOrIdx, addr);
    nrfCycleAnyWriteOk = false;
    nrfRetry           = 0;
    nrfSentAt          = now;
    nrfState           = NRF_WAITING;
    transmitCurrentNrfPacket();
}

void finishSensorJob(bool ok, const char* errReason, uint32_t now) {
    SensorJobType finishedType = sensorJobType;
    char finishedCmdId[64];
    strncpy(finishedCmdId, sensorMqttCmdId, sizeof(finishedCmdId) - 1);
    finishedCmdId[sizeof(finishedCmdId) - 1] = '\0';

    sensorJobType      = SJOB_NONE;
    sensorJobStep      = SSTEP_IDLE;
    sensorMqttCmdId[0] = '\0';

    if (finishedType == SJOB_SCAN) {
        state.scanInProgress  = false;
        state.scanResultReady = true;
        state.scanLastOk      = ok;
        state.scanCompletedSeq++;
        if (finishedCmdId[0] != '\0') {
            publishMqttSensorScanAck(finishedCmdId, ok, errReason);
        }
        if (ok) {
            char msg[48];
            snprintf(msg, sizeof(msg), "DS18B20 kereses kesz (%u db)", state.scanFoundCount);
            logEvent(LOG_INFO, "SENSOR_SCAN", msg);
        } else {
            logEvent(LOG_WARN, "SENSOR_SCAN_FAIL", errReason ? errReason : "Szenzor kereses hiba");
        }
        publishMqttStatus();
        menu.requestRedraw();
    } else if (finishedType == SJOB_LEARN) {
        state.learnInProgress  = false;
        state.learnResultReady = true;
        state.learnLastOk      = ok;
        state.learnCompletedSeq++;
        if (finishedCmdId[0] != '\0') {
            publishMqttCmdAck(finishedCmdId, ok, ok ? nullptr : (errReason ? errReason : "LEARN_FAIL"),
                              state.relayFeedbackOn);
        }
        if (ok) {
            char addrBuf[24];
            formatDeviceAddressHex(sensorLearnAddr, addrBuf, sizeof(addrBuf));
            char msg[64];
            snprintf(msg, sizeof(msg), "Tanitas OK: %s -> %s",
                     sensorRoleToKey(sensorLearnRole), addrBuf);
            logEvent(LOG_INFO, "SENSOR_LEARN", msg);
            requestImmediateNrfSend();
        } else {
            logEvent(LOG_WARN, "SENSOR_LEARN_FAIL", errReason ? errReason : "Szenzor tanitas hiba");
        }
        publishMqttStatus();
        menu.requestRedraw();
    } else if (finishedType == SJOB_ROLES_ONLY) {
        if (ok) {
            publishMqttStatus();
            menu.requestRedraw();
        }
    }
    (void)now;
}

bool startSensorScanJob(const char* cmdId) {
    if (sensorJobType == SJOB_SCAN || sensorJobType == SJOB_LEARN) {
        return false;
    }
    sensorJobType          = SJOB_SCAN;
    sensorJobStep          = SSTEP_SEND_SCAN_START;
    sensorJobStartedMs     = millis();
    sensorFetchOffset      = 0;
    sensorScanReadyRetries = 0;
    if (cmdId && cmdId[0] != '\0') {
        strncpy(sensorMqttCmdId, cmdId, sizeof(sensorMqttCmdId) - 1);
        sensorMqttCmdId[sizeof(sensorMqttCmdId) - 1] = '\0';
    } else {
        sensorMqttCmdId[0] = '\0';
    }
    state.scanInProgress   = true;
    state.scanResultReady  = false;
    state.scanLastOk       = false;
    return true;
}

bool startSensorLearnJob(uint8_t role, const uint8_t addr[8], const char* cmdId) {
    if (role >= SENSOR_ROLE_COUNT || !addr || !isValidDs18b20Rom(addr)) {
        return false;
    }
    if (sensorJobType == SJOB_SCAN || sensorJobType == SJOB_LEARN) {
        return false;
    }
    sensorJobType      = SJOB_LEARN;
    sensorJobStep      = SSTEP_SEND_LEARN;
    sensorJobStartedMs = millis();
    sensorLearnRole    = role;
    memcpy(sensorLearnAddr, addr, 8);
    if (cmdId && cmdId[0] != '\0') {
        strncpy(sensorMqttCmdId, cmdId, sizeof(sensorMqttCmdId) - 1);
        sensorMqttCmdId[sizeof(sensorMqttCmdId) - 1] = '\0';
    } else {
        sensorMqttCmdId[0] = '\0';
    }
    state.learnInProgress  = true;
    state.learnResultReady = false;
    state.learnLastOk      = false;
    return true;
}

bool startSensorRolesSyncJob() {
    if (sensorJobType != SJOB_NONE) {
        return false;
    }
    sensorJobType      = SJOB_ROLES_ONLY;
    sensorJobStep      = SSTEP_FETCH_ROLES;
    sensorJobStartedMs = millis();
    sensorFetchOffset  = 0;
    sensorMqttCmdId[0] = '\0';
    return true;
}

void tickSensorJob(uint32_t now) {
    if (sensorJobType == SJOB_NONE) {
        return;
    }
    // OVERFLOW-SAFE: uint32_t kivonás, 49 napos millis túlcsorduláskor is helyes.
    if (static_cast<uint32_t>(now - sensorJobStartedMs) >= SENSOR_JOB_TIMEOUT_MS) {
        finishSensorJob(false, "ACK_TIMEOUT", now);
        return;
    }
    if (sensorJobStep == SSTEP_WAIT_CONV_DELAY &&
        static_cast<uint32_t>(now - sensorConvWaitStartMs) >= SENSOR_SCAN_CONV_WAIT_MS) {
        sensorFetchOffset     = 0;
        state.scanFoundCount  = 0;
        sensorJobStep         = SSTEP_FETCH_FOUND;
    }
}

void processSensorNrfResponse(const NrfPacket& rx, uint32_t now) {
    lastNrfOk                    = now;
    state.consecutiveMissedPings = 0;
    nrfErrorBannerShown          = false;
    nrfFailLoggedForOutage       = false;
    nrfTimeoutLogged             = false;
    if (!state.nrf24Ok) {
        state.nrf24Ok = true;
        logEvent(LOG_INFO, "NRF_OK", "NRF24 kapcsolat helyreallt");
    }

    bool rspOk          = (rx.sensorRsp.statusFlags & SENSOR_RSP_FLAG_OK) != 0;
    bool rspUnknown     = (rx.sensorRsp.statusFlags & SENSOR_RSP_FLAG_UNKNOWN) != 0;
    bool rspScanReady   = (rx.sensorRsp.statusFlags & SENSOR_RSP_FLAG_SCAN_READY) != 0;
    state.sensorUnknown = rspUnknown;

    switch (rx.sensorRsp.subCmd) {
        case SENSOR_CMD_SCAN_START:
            sensorConvWaitStartMs  = now;
            sensorScanReadyRetries = 0;
            sensorJobStep          = SSTEP_WAIT_CONV_DELAY;
            break;

        case SENSOR_CMD_GET_FOUND: {
            if (!rspScanReady && sensorScanReadyRetries < 5) {
                sensorScanReadyRetries++;
                sensorConvWaitStartMs = now - (SENSOR_SCAN_CONV_WAIT_MS - 150UL);
                sensorJobStep         = SSTEP_WAIT_CONV_DELAY;
                break;
            }
            uint8_t total = rx.sensorRsp.totalFound;
            if (total > MAX_DISCOVERED_SENSORS) {
                total = MAX_DISCOVERED_SENSORS;
            }
            for (uint8_t slot = 0; slot < 2; slot++) {
                uint8_t idx = static_cast<uint8_t>(rx.sensorRsp.startIndex + slot);
                if (idx < total &&
                    (rx.sensorRsp.validEntryMask & (1U << slot)) != 0 &&
                    isValidDs18b20Rom(rx.sensorRsp.entries[slot].address)) {
                    memcpy(state.scanFound[idx].address, rx.sensorRsp.entries[slot].address, 8);
                    int16_t deci = rx.sensorRsp.entries[slot].tempDeci;
                    state.scanFound[idx].temp = (deci <= -1200)
                                                    ? SENSOR_DISCONNECTED_VAL
                                                    : (static_cast<float>(deci) / 10.0f);
                    if (idx + 1U > state.scanFoundCount) {
                        state.scanFoundCount = static_cast<uint8_t>(idx + 1U);
                    }
                }
            }
            if (static_cast<uint8_t>(rx.sensorRsp.startIndex + 2U) < total) {
                sensorFetchOffset = static_cast<uint8_t>(rx.sensorRsp.startIndex + 2U);
                sensorJobStep     = SSTEP_FETCH_FOUND;
            } else {
                sensorFetchOffset = 0;
                sensorJobStep     = SSTEP_FETCH_ROLES;
            }
            break;
        }

        case SENSOR_CMD_LEARN:
            if (!rspOk) {
                finishSensorJob(false, "EEPROM_FAIL", now);
            } else {
                sensorFetchOffset = 0;
                sensorJobStep     = SSTEP_FETCH_ROLES;
            }
            break;

        case SENSOR_CMD_GET_ROLES: {
            uint8_t roleMask = static_cast<uint8_t>((rx.sensorRsp.statusFlags >> SENSOR_RSP_ROLE_MASK_SHIFT) & 0x0FU);
            for (uint8_t slot = 0; slot < 2; slot++) {
                uint8_t r = static_cast<uint8_t>(rx.sensorRsp.startIndex + slot);
                if (r < SENSOR_ROLE_COUNT) {
                    if (isValidDs18b20Rom(rx.sensorRsp.entries[slot].address)) {
                        memcpy(state.sensors[r].address, rx.sensorRsp.entries[slot].address, 8);
                        state.sensors[r].assigned = true;
                        state.sensors[r].ok       = ((roleMask & (1U << r)) != 0) &&
                                                    ((rx.sensorRsp.validEntryMask & (1U << slot)) != 0);
                    } else {
                        memset(state.sensors[r].address, 0, 8);
                        state.sensors[r].assigned = false;
                        state.sensors[r].ok       = false;
                    }
                }
            }
            if (sensorJobStep != SSTEP_FETCH_ROLES) {
                break;
            }
            if (static_cast<uint8_t>(rx.sensorRsp.startIndex + 2U) < SENSOR_ROLE_COUNT) {
                sensorFetchOffset = static_cast<uint8_t>(rx.sensorRsp.startIndex + 2U);
                sensorJobStep     = SSTEP_FETCH_ROLES;
            } else {
                state.hasSensorRolesLoaded = true;
                finishSensorJob(true, nullptr, now);
            }
            break;
        }

        default:
            finishSensorJob(false, "BAD_SUBCMD", now);
            break;
    }
}

void processNrfResponsePacket(const NrfPacket& rx, uint32_t rxNowMs) {
    bool prevNrfOk         = state.nrf24Ok;
    bool prevPump          = state.pumpOn;
    bool prevRelayFb       = state.relayFeedbackOn;
    bool prevRelayFault    = state.relayFault;
    bool prevServiceMode   = state.serviceMode;
    bool prevNanoLoopFault = state.nanoLoopFault;
    bool prevSensorUnknown = state.sensorUnknown;
    uint8_t prevPState     = state.pumpState;
    uint8_t prevAlarmAct   = state.alarmState.active;
    uint8_t prevAlarmRsn   = state.alarmState.reason;

    state.eloremeno         = rx.nano.eloremeno;
    state.visszatero        = rx.nano.visszatero;
    state.kazan             = rx.nano.kazan;
    state.kulteri           = rx.nano.kulteri;
    state.nanoCounter       = rx.nano.counter;
    state.sensorValidMask   = rx.nano.sensorValidMask;
    state.nanoAlarmState    = unpackAlarmState(rx.nano.alarmPacked);
    state.hasNrfResponse    = true;
    state.lastNrfResponseMs = rxNowMs;
    lastNrfOk               = rxNowMs;

    for (uint8_t b = 0; b < 8; b++) {
        state.kazanStatus[b] = getBit(rx.nano.kazanStatusMask, b) ? 1 : 0;
    }

    // Napi max/min/átlag frissítése minden érvényes előremenő mérésnél
    if (isSensorValid(state.eloremeno)) {
        char hhmm[6] = { 0 };
        if (state.ntpSynced) {
            struct tm tmStat{};
            if (getLocalTime(&tmStat, 0) && (tmStat.tm_year + 1900 >= 2024)) {
                snprintf(hhmm, sizeof(hhmm), "%02d:%02d", tmStat.tm_hour, tmStat.tm_min);
            }
        }
        if (state.eloremenoCount == 0 || state.eloremeno > state.eloremenoMax) {
            state.eloremenoMax = state.eloremeno;
            if (hhmm[0] != '\0') {
                strncpy(state.eloremenoMaxAt, hhmm, sizeof(state.eloremenoMaxAt) - 1);
            } else {
                state.eloremenoMaxAt[0] = '\0';
            }
        }
        if (state.eloremenoCount == 0 || state.eloremeno < state.eloremenoMin) {
            state.eloremenoMin = state.eloremeno;
            if (hhmm[0] != '\0') {
                strncpy(state.eloremenoMinAt, hhmm, sizeof(state.eloremenoMinAt) - 1);
            } else {
                state.eloremenoMinAt[0] = '\0';
            }
        }
        state.eloremenoSum += static_cast<double>(state.eloremeno);
        state.eloremenoCount++;
    }

    bool ackFlag              = (rx.nano.syncFlags & SYNC_FLAG_ACK) != 0;
    bool staleFlag            = (rx.nano.syncFlags & SYNC_FLAG_STALE) != 0;
    bool rxServiceMode        = (rx.nano.syncFlags & SYNC_FLAG_SERVICE_MODE) != 0;
    bool rxCmdAck             = (rx.nano.syncFlags & SYNC_FLAG_CMD_ACK) != 0;
    bool rxPumpOn             = (rx.nano.syncFlags & SYNC_FLAG_PUMP_ON) != 0;
    bool rxCmdRejectedService = (rx.nano.syncFlags & SYNC_FLAG_CMD_REJECTED_SERVICE) != 0;
    bool rxRelayFbOn          = (rx.nano.syncFlags & SYNC_FLAG_RELAY_FB_ON) != 0;
    bool rxRelayMismatch      = (rx.nano.syncFlags & SYNC_FLAG_RELAY_MISMATCH) != 0;
    bool rxNanoLoopFault      = (rx.nano.alarmPacked & SYNC_FLAG_LOOP_FAULT) != 0;
    bool rxSensorUnknown      = (rx.nano.alarmPacked & SYNC_FLAG_SENSOR_UNKNOWN) != 0;
    state.nanoSynced       = ackFlag;
    state.nanoSyncStale    = staleFlag;
    state.hasRelayFeedback = true;
    state.relayFeedbackOn  = rxRelayFbOn;
    state.nanoLoopFault    = rxNanoLoopFault;
    state.sensorUnknown    = rxSensorUnknown;
    (void)rxPumpOn;

    if (rxSensorUnknown != prevSensorUnknown) {
        if (rxSensorUnknown) {
            logEvent(LOG_WARN, "SENSOR_UNKNOWN", "Ismeretlen DS18B20 szenzor - tanitas szukseges");
        } else {
            logEvent(LOG_INFO, "SENSOR_ROLES_OK", "Mind a 4 DS18B20 szerep azonositva");
        }
    }
    if (!state.hasSensorRolesLoaded || rxSensorUnknown != prevSensorUnknown) {
        startSensorRolesSyncJob();
    }

    if (rxNanoLoopFault != prevNanoLoopFault) {
        if (rxNanoLoopFault) {
            logEvent(LOG_ERROR, "NANO_LOOP_FAULT", "Nano loop ciklusido tul lepett");
        } else {
            logEvent(LOG_INFO, "NANO_LOOP_OK", "Nano loop ciklusido helyreallt");
        }
        menu.requestRedraw();
    }

    if (rxServiceMode != state.serviceMode) {
        state.serviceMode = rxServiceMode;
        if (state.serviceMode) {
            logEvent(LOG_INFO, "SERVICE_MODE_ON_BY_SLAVE", "Kazan oldali szerviz kapcsolo BE");
        } else {
            logEvent(LOG_INFO, "SERVICE_MODE_OFF_BY_SLAVE", "Kazan oldali szerviz kapcsolo KI");
        }
        menu.requestRedraw();
    }

    // Függő parancs végponttól-végpontig tartó visszaigazolásának (CMD_ACK) ellenőrzése (D7 fizikai visszacsatolással)
    bool expectedPumpOn = getBit(nrfTxPacket.esp.outputsMask, OUT_BIT_SZIVATTYU);
    if (pendingCmdActive) {
        if (pendingCmdSentInCurrentNrf && rx.nano.cmdSeq == pendingCmdSeq) {
            if (rxCmdRejectedService || (rxServiceMode && pendingCmdRequestedPumpOff)) {
                char ackId[64];
                strncpy(ackId, pendingCmdId, sizeof(ackId) - 1);
                ackId[sizeof(ackId) - 1] = '\0';
                clearPendingCmd();
                logEvent(LOG_WARN, "CMD_ACK_REJECT", "Parancs elutasitva (szerviz mod)");
                publishMqttCmdAck(ackId, false, "SERVICE_MODE", rxRelayFbOn);
            } else if (rxCmdAck && (rxRelayFbOn == expectedPumpOn)) {
                char ackId[64];
                strncpy(ackId, pendingCmdId, sizeof(ackId) - 1);
                ackId[sizeof(ackId) - 1] = '\0';
                clearPendingCmd();
                logEvent(LOG_INFO, "CMD_ACK_OK", ackId);
                publishMqttCmdAck(ackId, true, nullptr, rxRelayFbOn);
            } else if (rxRelayMismatch ||
                       (static_cast<uint32_t>(rxNowMs - pendingCmdAt) >= RELAY_FAULT_DELAY_MS &&
                        rxRelayFbOn != expectedPumpOn)) {
                char ackId[64];
                strncpy(ackId, pendingCmdId, sizeof(ackId) - 1);
                ackId[sizeof(ackId) - 1] = '\0';
                clearPendingCmd();
                logEvent(LOG_WARN, "CMD_ACK_RELAY_MISMATCH", ackId);
                publishMqttCmdAck(ackId, false, "RELAY_MISMATCH", rxRelayFbOn);
            } else {
                // D7 relé visszacsatolás még kapcsolódik (<500ms) -> 75ms múlva újra lekérdezzük
                pendingCmdSentInCurrentNrf = false;
                pendingCmdWaitingSettle    = true;
                pendingCmdLastPollAt       = rxNowMs;
            }
        } else {
            // Előző ciklus válasza érkezett be; most azonnal küldjük a függő parancsot
            pendingCmdSentInCurrentNrf = false;
            pendingCmdWaitingSettle    = false;
            requestImmediateNrfSend();
        }
    }

    bool syncUpdated = false;
    if (nrfTxSyncEpoch != 0) {
        uint16_t expectedLow16 = static_cast<uint16_t>(nrfTxSyncEpoch & 0xFFFFU);
        if (ackFlag && rx.nano.syncEpochLow16 == expectedLow16) {
            state.lastNanoSyncEpoch   = nrfTxSyncEpoch;
            state.lastNanoSyncMs      = rxNowMs;
            state.lastNanoSyncSuccess = true;
            pendingTimeSyncEpoch      = 0;
            syncRetryWaiting          = false;
            syncUpdated               = true;
            logEvent(LOG_INFO, "NANO_SYNC", "Nano idoszinkron OK");
        } else {
            state.lastNanoSyncSuccess = false;
            pendingTimeSyncEpoch      = 0;
            syncRetryWaiting          = true;
            syncRetryStartMs          = rxNowMs;
            syncUpdated               = true;
            logEvent(LOG_WARN, "SYNC_FAIL", "Nano szinkron ACK hiba, 1h mulva ujra");
        }
        nrfTxSyncEpoch = 0;
    } else if (!ackFlag && state.ntpSynced && state.lastNanoSyncEpoch != 0 && !syncRetryWaiting) {
        // A Nano újraindult és elvesztette a szinkront: következő ciklusban automatikus újraszinkron
        state.lastNanoSyncEpoch = 0;
        bootSyncDone = false;
    }

    state.consecutiveMissedPings = 0;
    nrfErrorBannerShown          = false;
    nrfFailLoggedForOutage       = false;
    nrfTimeoutLogged             = false;
    if (!state.nrf24Ok) {
        state.nrf24Ok = true;
        logEvent(LOG_INFO, "NRF_OK", "NRF24 kapcsolat helyreallt");
    }

    bool modeRestored = evaluateBoilerPumpProtection(rxNowMs, rxRelayMismatch);
    if (modeRestored) {
        saveSetting("pumpMode", static_cast<uint32_t>(state.pumpConfig.pumpMode));
    }
    updateAlarmBlinkAndLed(rxNowMs);

    if (prevPump != state.pumpOn) {
        // A friss kazánhőmérséklet alapján változott a szivattyú parancs -> azonnali küldés
        requestImmediateNrfSend();
    }

    if (mqttStatusPendingRetry || syncUpdated || modeRestored ||
        prevNrfOk != state.nrf24Ok || prevPump != state.pumpOn ||
        prevRelayFb != state.relayFeedbackOn || prevRelayFault != state.relayFault ||
        prevServiceMode != state.serviceMode || prevNanoLoopFault != state.nanoLoopFault ||
        prevSensorUnknown != state.sensorUnknown ||
        prevPState != state.pumpState ||
        prevAlarmAct != state.alarmState.active || prevAlarmRsn != state.alarmState.reason) {
        publishMqttStatus();
    }
    menu.requestRedraw();
}

void handleNrfTimeout(uint32_t now) {
    nrfFail  = true;
    nrfState = NRF_IDLE;
    nrfRetry = 0;
    state.dmaWaitUs = nrfDma.flushAndDrain();
    dmaQueueFullLogged = false;

    if (nrfTxPacket.packetType == PKT_TYPE_SENSOR_CMD) {
        logEvent(LOG_WARN, "SENSOR_NRF_TIMEOUT", "Szenzor NRF parancs timeout");
        finishSensorJob(false, nrfCycleAnyWriteOk ? "NRF_TIMEOUT" : "NRF_WRITE_FAIL", now);
        return;
    }

    if (pendingCmdActive) {
        if (pendingCmdSentInCurrentNrf) {
            char failedId[64];
            strncpy(failedId, pendingCmdId, sizeof(failedId) - 1);
            failedId[sizeof(failedId) - 1] = '\0';
            clearPendingCmd();
            logEvent(LOG_WARN, "CMD_ACK_NRF_TIMEOUT", failedId);
            publishMqttCmdAck(failedId, false, "NRF_TIMEOUT", state.relayFeedbackOn);
        } else {
            requestImmediateNrfSend();
        }
    }

    if (!state.hasNrfResponse && lastNrfOk == 0) {
        lastNrfOk = now;
    }

    if (!nrfTimeoutLogged) {
        nrfTimeoutLogged = true;
        char msg[48];
        snprintf(msg, sizeof(msg), "NRF_TIMEOUT seq=%lu",
                 static_cast<unsigned long>(nrfTxPacket.esp.seq));
        logEvent(LOG_WARN, "NRF_TIMEOUT", msg);
    }

    bool syncUpdated = false;
    if (nrfTxSyncEpoch != 0) {
        state.lastNanoSyncSuccess = false;
        pendingTimeSyncEpoch      = 0;
        syncRetryWaiting          = true;
        syncRetryStartMs          = now;
        nrfTxSyncEpoch            = 0;
        syncUpdated               = true;
        logEvent(LOG_WARN, "SYNC_FAIL", "Nano szinkron sikertelen, 1h mulva ujra");
    }

    bool prevNrfOk       = state.nrf24Ok;
    uint8_t prevAlarmAct = state.alarmState.active;
    uint8_t prevAlarmRsn = state.alarmState.reason;

    if (!nrfCycleAnyWriteOk) {
        state.nrf24Ok                = false;
        state.consecutiveMissedPings = NRF_MAX_MISSED_CYCLES;
        evaluateAlarmState();
        if (state.alarmConfig.alarmEnabled == 0) {
            state.alarmState.reason = ALARM_REASON_NRF_FAIL;
        }
        if (!nrfFailLoggedForOutage) {
            nrfFailLoggedForOutage = true;
            char msg[48];
            snprintf(msg, sizeof(msg), "NRF WRITE FAILED seq=%lu",
                     static_cast<unsigned long>(nrfTxPacket.esp.seq));
            logEvent(LOG_ERROR, "NRF_FAIL", msg);
        }
        if (!nrfErrorBannerShown) {
            nrfErrorBannerShown = true;
            menu.showErrorBanner("NRF HIBA!", "Radio iras hiba", 2000);
        }
        menu.requestRedraw();
    } else {
        if (state.consecutiveMissedPings < 255) {
            state.consecutiveMissedPings++;
        }
        if (state.consecutiveMissedPings == 1) {
            char msg[48];
            snprintf(msg, sizeof(msg), "1. kimaradt ping seq=%lu",
                     static_cast<unsigned long>(nrfTxPacket.esp.seq));
            logEvent(LOG_WARN, "NRF_MISS", msg);
        }
        if (state.consecutiveMissedPings >= NRF_MAX_MISSED_CYCLES) {
            state.nrf24Ok = false;
            evaluateAlarmState();
            if (!nrfFailLoggedForOutage) {
                nrfFailLoggedForOutage = true;
                logEvent(LOG_ERROR, "NRF_FAIL", "3 kimaradt ping");
            }
        }
    }

    if (syncUpdated || prevNrfOk != state.nrf24Ok ||
        prevAlarmAct != state.alarmState.active ||
        prevAlarmRsn != state.alarmState.reason) {
        publishMqttStatus();
        menu.requestRedraw();
    }
}

void handleNrf(uint32_t now) {
    NrfPacket rx{};
    NrfDmaRadio::PollResult dmaRes = nrfDma.poll(&rx);
    if (dmaRes.hasTimingUpdate) {
        state.dmaWaitUs = dmaRes.dmaWaitUs;
    }
    bool qFull = nrfDma.consumeQueueFullWarning() || dmaRes.queueFullWarn;
    if (qFull && !dmaQueueFullLogged) {
        dmaQueueFullLogged = true;
        logEvent(LOG_WARN, "DMA_QUEUE_FULL", "NRF SPI DMA sor megtelt (3)");
    }
    if (dmaRes.txAckedNow) {
        nrfCycleAnyWriteOk = true;
    }

    if (nrfState == NRF_IDLE) {
        uint32_t pingInterval = static_cast<uint32_t>(state.heartbeatIntervalSec) * 1000UL;
        // OVERFLOW-SAFE: uint32_t kivonás, 49 napos millis túlcsorduláskor is helyes.
        if (static_cast<uint32_t>(now - lastPing) >= pingInterval) {
            lastPing            = now;
            nrfImmediatePending = false;
            startNrfRequest(now);
            return;
        }
        if (nrfImmediatePending) {
            nrfImmediatePending = false;
            startNrfRequest(now);
            return;
        }
        if (sensorJobType != SJOB_NONE &&
            sensorJobStep != SSTEP_IDLE &&
            sensorJobStep != SSTEP_WAIT_CONV_DELAY) {
            startSensorNrfStepPacket(now);
            return;
        }
        return;
    }

    // nrfState == NRF_WAITING
    if (dmaRes.rxPacketReady) {
        if (nrfTxPacket.packetType == PKT_TYPE_REQUEST &&
            rx.packetType == PKT_TYPE_RESPONSE &&
            rx.nano.seq == nrfTxPacket.esp.seq) {
            nrfDma.stopListening();
            nrfState           = NRF_IDLE;
            nrfRetry           = 0;
            nrfFail            = false;
            dmaQueueFullLogged = false;
            processNrfResponsePacket(rx, now);
            return;
        }
        if (nrfTxPacket.packetType == PKT_TYPE_SENSOR_CMD &&
            rx.packetType == PKT_TYPE_SENSOR_RSP &&
            rx.sensorRsp.seq == nrfTxPacket.sensorCmd.seq) {
            nrfDma.stopListening();
            nrfState           = NRF_IDLE;
            nrfRetry           = 0;
            nrfFail            = false;
            dmaQueueFullLogged = false;
            processSensorNrfResponse(rx, now);
            return;
        }
    }

    // OVERFLOW-SAFE: uint32_t kivonás, 49 napos millis túlcsorduláskor is helyes.
    if (static_cast<uint32_t>(now - nrfSentAt) >= NRF_TIMEOUT_MS) {
        if (nrfRetry < NRF_MAX_RETRY) {
            // Függő DMA tranzakciók lezárása + FLUSH_TX/FLUSH_RX, majd UGYANAZ a csomag újraküldése nem-blokkolva
            state.dmaWaitUs = nrfDma.flushAndDrain();
            transmitCurrentNrfPacket();
            nrfSentAt = now;
            nrfRetry++;
        } else {
            handleNrfTimeout(now);
        }
    }
}

// ============================================================================
// IDŐFORMÁZÓ SEGÉDEK + MQTT JSON STATUS & TELEMETRY PUBLISH (V5)
// ============================================================================
void formatEpochTimeStr(uint32_t epochSec, char* out, size_t len) {
    if (!out || len == 0) return;
    time_t t = static_cast<time_t>(epochSec);
    struct tm tmLocal{};
    if (localtime_r(&t, &tmLocal)) {
        strftime(out, len, "%Y-%m-%d %H:%M:%S", &tmLocal);
    } else {
        snprintf(out, len, "UP %lus", static_cast<unsigned long>(uptimeTotalMs / 1000ULL));
    }
}

void formatEspCurrentTimeStr(char* out, size_t len, uint32_t uptimeSec) {
    if (!out || len == 0) return;
    if (state.ntpSynced) {
        struct tm tmLocal{};
        if (getLocalTime(&tmLocal, 0) && (tmLocal.tm_year + 1900 >= 2024)) {
            strftime(out, len, "%Y-%m-%d %H:%M:%S", &tmLocal);
            return;
        }
    }
    snprintf(out, len, "UP %lus", static_cast<unsigned long>(uptimeSec));
}

bool computeNextSyncTimeStr(char* out, size_t len) {
    if (!out || len == 0 || !state.ntpSynced) return false;
    struct tm tmNow{};
    if (!getLocalTime(&tmNow, 0) || (tmNow.tm_year + 1900 < 2024)) {
        return false;
    }

    struct tm tmTarget = tmNow;
    tmTarget.tm_sec = 0;
    tmTarget.tm_min = 0;
    tmTarget.tm_isdst = -1;
    if (tmNow.tm_hour < 12) {
        tmTarget.tm_hour = 12;
    } else {
        tmTarget.tm_hour = 0;
        tmTarget.tm_mday += 1;
    }

    time_t nextEpoch = mktime(&tmTarget);
    if (nextEpoch == static_cast<time_t>(-1)) {
        return false;
    }
    struct tm tmNext{};
    if (!localtime_r(&nextEpoch, &tmNext)) {
        return false;
    }
    strftime(out, len, "%Y-%m-%d %H:%M:%S", &tmNext);
    return true;
}

void buildStatusJson(JsonDocument& doc) {
    uint32_t now = millis();
    uint32_t uptimeSec = static_cast<uint32_t>(uptimeTotalMs / 1000ULL);

    doc["currentTemp"] = round1(state.currentTemp);
    doc["targetTemp"]  = round1(state.targetTemp);
    doc["heatingOn"]   = state.heatingOn;
    doc["serviceMode"] = state.serviceMode;
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

    doc["pumpState"]       = pumpStateToStr(state.pumpState);
    doc["pumpOn"]          = state.pumpOn;
    doc["relayFeedbackOn"] = state.relayFeedbackOn;
    doc["relayFault"]      = state.relayFault;
    const char* pumpCtrlStr = "AUTO";
    if (state.serviceMode) {
        pumpCtrlStr = "SERVICE";
    } else if (state.pumpConfig.pumpMode != PUMP_MODE_AUTO) {
        pumpCtrlStr = "MANUAL";
    }
    doc["pumpControl"] = pumpCtrlStr;

    JsonObject aCfg = doc["alarmConfig"].to<JsonObject>();
    aCfg["alarmTemp"]    = round1(state.alarmConfig.alarmTemp);
    aCfg["alarmEnabled"] = (state.alarmConfig.alarmEnabled != 0);

    JsonObject safety = doc["safety"].to<JsonObject>();
    safety["safetyOverheatTemp"] = round1(SAFETY_OVERHEAT_TEMP);

    JsonObject aState = doc["alarmState"].to<JsonObject>();
    aState["active"] = (state.alarmState.active != 0);
    aState["reason"] = alarmReasonToStr(state.alarmState.reason);

    JsonObject hb = doc["heartbeat"].to<JsonObject>();
    hb["intervalSec"] = state.heartbeatIntervalSec;

    doc["nrf24_ok"]      = state.nrf24Ok;
    doc["loopFault"]     = state.loopFault;
    doc["nanoLoopFault"] = state.nanoLoopFault;
    doc["loopMaxMs"]     = state.loopMaxMs;
    doc["loopLastMs"]    = state.loopLastMs;
    doc["dmaWaitUs"]     = state.dmaWaitUs;

    char espTimeBuf[24];
    formatEspCurrentTimeStr(espTimeBuf, sizeof(espTimeBuf), uptimeSec);

    JsonObject timeObj = doc["time"].to<JsonObject>();
    timeObj["espNtpSynced"] = state.ntpSynced;
    timeObj["espTime"]      = espTimeBuf;
    timeObj["espUptimeSec"] = uptimeSec;

    JsonObject syncObj = doc["sync"].to<JsonObject>();
    syncObj["ntpSynced"]  = state.ntpSynced;
    syncObj["espTime"]    = espTimeBuf;
    syncObj["nanoSynced"] = state.nanoSynced;

    if (state.nanoSynced && state.lastNanoSyncEpoch > 0) {
        // OVERFLOW-SAFE: uint32_t kivonás, 49 napos millis túlcsorduláskor is helyes.
        uint32_t elapsedSec = static_cast<uint32_t>(now - state.lastNanoSyncMs) / 1000UL;
        uint32_t nanoEpoch  = state.lastNanoSyncEpoch + elapsedSec;
        char nanoTimeBuf[24];
        formatEpochTimeStr(nanoEpoch, nanoTimeBuf, sizeof(nanoTimeBuf));
        syncObj["nanoTime"]  = nanoTimeBuf;
        syncObj["nanoStale"] = state.nanoSyncStale || (elapsedSec >= NANO_SYNC_STALE_SEC);
    } else {
        syncObj["nanoTime"]  = nullptr;
        syncObj["nanoStale"] = false;
    }

    if (state.lastNanoSyncEpoch > 0) {
        char lastSyncBuf[24];
        formatEpochTimeStr(state.lastNanoSyncEpoch, lastSyncBuf, sizeof(lastSyncBuf));
        syncObj["lastSync"] = lastSyncBuf;
    } else {
        syncObj["lastSync"] = nullptr;
    }

    char nextSyncBuf[24];
    if (computeNextSyncTimeStr(nextSyncBuf, sizeof(nextSyncBuf))) {
        syncObj["nextSync"] = nextSyncBuf;
    } else {
        syncObj["nextSync"] = nullptr;
    }

    syncObj["syncSuccess"] = state.lastNanoSyncSuccess;
    syncObj["millisOverflowInDays"] =
        static_cast<uint32_t>((4294967295ULL - static_cast<uint64_t>(now)) / 86400000ULL);

    if (state.hasNrfResponse) {
        // OVERFLOW-SAFE: uint32_t kivonás, 49 napos millis túlcsorduláskor is helyes.
        syncObj["pingAgeSec"] = static_cast<uint32_t>(now - state.lastNrfResponseMs) / 1000UL;
    } else {
        syncObj["pingAgeSec"] = nullptr;
    }

    const char* trendDirStr = "flat";
    if (state.trendDir == TREND_UP)        trendDirStr = "up";
    else if (state.trendDir == TREND_DOWN) trendDirStr = "down";

    JsonObject trendObj = doc["trend"].to<JsonObject>();
    trendObj["eloremeno"] = trendDirStr;
    trendObj["delta"]     = round1(state.trendDelta);
    if (state.eloremenoCount > 0) {
        float avg = static_cast<float>(state.eloremenoSum / static_cast<double>(state.eloremenoCount));
        trendObj["max"] = round1(state.eloremenoMax);
        trendObj["min"] = round1(state.eloremenoMin);
        trendObj["avg"] = round1(avg);
    } else {
        trendObj["max"] = nullptr;
        trendObj["min"] = nullptr;
        trendObj["avg"] = nullptr;
    }
    if (state.ntpSynced && state.eloremenoMaxAt[0] != '\0') {
        trendObj["maxAt"] = state.eloremenoMaxAt;
    } else {
        trendObj["maxAt"] = nullptr;
    }
    if (state.ntpSynced && state.eloremenoMinAt[0] != '\0') {
        trendObj["minAt"] = state.eloremenoMinAt;
    } else {
        trendObj["minAt"] = nullptr;
    }
    if (state.ntpSynced && state.statsSinceStr[0] != '\0') {
        char sinceWithSuffix[32];
        snprintf(sinceWithSuffix, sizeof(sinceWithSuffix), "%s \xC3\xB3ta", state.statsSinceStr);
        trendObj["since"] = sinceWithSuffix;
    } else {
        trendObj["since"] = "Boot \xC3\xB3ta";
    }

    if (!state.nrf24Ok && state.consecutiveMissedPings >= NRF_MAX_MISSED_CYCLES) {
        doc["error"] = "NRF24 kommunikacios hiba (3 ping kimaradt)";
    } else if (state.safetyOverheat) {
        doc["error"] = "Kazan tulmelegedes (>= 95.0C) - szivattyu kenyszeritett BE";
    } else if (state.relayFault) {
        doc["error"] = state.pumpOn
            ? "Rele hiba: kert BE, fizikai rele KI (D7)"
            : "Rele hiba: kert KI, fizikai rele BE (D7)";
    } else if (state.safetySensorFault) {
        doc["error"] = state.sensorUnknown
            ? "Szenzor ismeretlen vagy nincs tanitva - tanitas kell"
            : "Kazan szenzor hiba (-127) - szivattyu biztonsagi BE";
    } else if (state.sensorUnknown) {
        doc["error"] = "Ismeretlen DS18B20 szenzor - tanitas szukseges";
    } else {
        doc["error"] = nullptr;
    }

    doc["sensorUnknown"] = state.sensorUnknown;
    JsonObject sensorsObj = doc["sensors"].to<JsonObject>();
    for (uint8_t r = 0; r < SENSOR_ROLE_COUNT; r++) {
        JsonObject sObj = sensorsObj[sensorRoleToKey(r)].to<JsonObject>();
        if (state.hasSensorRolesLoaded && state.sensors[r].assigned) {
            char addrBuf[24];
            formatDeviceAddressHex(state.sensors[r].address, addrBuf, sizeof(addrBuf));
            sObj["address"] = addrBuf;
            sObj["ok"]      = state.sensors[r].ok;
        } else {
            sObj["address"] = nullptr;
            sObj["ok"]      = false;
        }
    }
}

bool publishMqttStatus() {
    if (!mqtt.connected()) {
        mqttStatusPendingRetry = true;
        return false;
    }
    JsonDocument doc;
    buildStatusJson(doc);
    char payload[2560];
    size_t n = serializeJson(doc, payload, sizeof(payload));
    if (n == 0) {
        mqttStatusPendingRetry = true;
        return false;
    }
    bool ok = mqtt.publish(TOPIC_STATUS, reinterpret_cast<const uint8_t*>(payload), n, true); // retained=true
    if (!ok) {
        char msg[48];
        snprintf(msg, sizeof(msg), "MQTT PUBLISH FAILED %s", TOPIC_STATUS);
        logEvent(LOG_WARN, "MQTT_PUB_FAIL", msg);
        mqttStatusPendingRetry = true;
        return false;
    }
    mqttStatusPendingRetry = false;
    return true;
}

bool publishMqttTelemetry() {
    if (!mqtt.connected()) {
        mqttTelemetryPendingRetry = true;
        return false;
    }
    JsonDocument doc;
    buildStatusJson(doc);
    doc["nanoCounter"] = state.nanoCounter;
    char payload[2560];
    size_t n = serializeJson(doc, payload, sizeof(payload));
    if (n == 0) {
        mqttTelemetryPendingRetry = true;
        return false;
    }
    bool ok = mqtt.publish(TOPIC_TELEMETRY, reinterpret_cast<const uint8_t*>(payload), n, false);
    if (!ok) {
        char msg[48];
        snprintf(msg, sizeof(msg), "MQTT PUBLISH FAILED %s", TOPIC_TELEMETRY);
        logEvent(LOG_WARN, "MQTT_PUB_FAIL", msg);
        mqttTelemetryPendingRetry = true;
        return false;
    }
    mqttTelemetryPendingRetry = false;
    return true;
}

bool publishMqttLogsResponse(int page, int limit) {
    if (!mqtt.connected()) {
        return false;
    }

    int safeLimit = limit;
    if (safeLimit < 1)  safeLimit = 20;
    if (safeLimit > 20) safeLimit = 20;

    int safePage = (page < 1) ? 1 : page;
    size_t total = logger.count();
    size_t pages = (total == 0)
                       ? 1
                       : ((total + static_cast<size_t>(safeLimit) - 1) / static_cast<size_t>(safeLimit));

    JsonDocument doc;
    JsonArray arr = doc["logs"].to<JsonArray>();

    size_t startOffset = static_cast<size_t>(safePage - 1) * static_cast<size_t>(safeLimit);
    for (int i = 0; i < safeLimit; i++) {
        LogEntry entry{};
        if (!logger.getNewest(startOffset + static_cast<size_t>(i), entry)) {
            break;
        }
        JsonObject item = arr.add<JsonObject>();
        item["id"]        = entry.id;
        item["timestamp"] = entry.timestamp;
        item["level"]     = Logger::levelToStr(entry.level);
        item["code"]      = entry.code;
        item["message"]   = entry.message;
    }

    doc["total"] = total;
    doc["page"]  = safePage;
    doc["pages"] = pages;

    size_t n = serializeJson(doc, logsResponsePayload, sizeof(logsResponsePayload));
    if (n == 0) {
        return false;
    }

    bool ok = mqtt.publish(TOPIC_LOGS,
                           reinterpret_cast<const uint8_t*>(logsResponsePayload),
                           n,
                           false);
    if (!ok) {
        logEvent(LOG_WARN, "MQTT_PUB_FAIL", "MQTT PUBLISH FAILED logs");
        return false;
    }
    return true;
}

// ============================================================================
// MENÜ CALLBACK-EK (AZONNALI MENTÉS + NRF REQUEST + MQTT STATUS)
// ============================================================================
void commitSettingEffects(bool redrawMenu) {
    uint32_t now = millis();
    nrfErrorBannerShown = false;
    evaluateRoomHeatingIndicator();
    evaluateBoilerPumpProtection(now);
    updateAlarmBlinkAndLed(now);

    requestImmediateNrfSend();
    handleNrf(now);

    publishMqttStatus();
    if (redrawMenu) {
        menu.requestRedraw();
    }
}

void onMenuSettingCommitted() {
    commitSettingEffects(true);
}

void onMenuFactoryReset() {
    uint32_t now = millis();
    nrfErrorBannerShown = false;
    clearNvsAndResetDefaults();
    evaluateRoomHeatingIndicator();
    evaluateBoilerPumpProtection(now);
    updateAlarmBlinkAndLed(now);

    requestImmediateNrfSend();
    handleNrf(now);

    publishMqttStatus();
}

void resetEloremenoStats(const char* logCode, const char* logMsg) {
    state.eloremenoMax      = -999.0f;
    state.eloremenoMin      = 999.0f;
    state.eloremenoSum      = 0.0;
    state.eloremenoCount    = 0;
    state.eloremenoMaxAt[0] = '\0';
    state.eloremenoMinAt[0] = '\0';

    if (state.ntpSynced) {
        time_t epochNow = time(nullptr);
        if (epochNow >= 1700000000) {
            formatEpochTimeStr(static_cast<uint32_t>(epochNow),
                               state.statsSinceStr,
                               sizeof(state.statsSinceStr));
        } else {
            state.statsSinceStr[0] = '\0';
        }
    } else {
        state.statsSinceStr[0] = '\0';
    }

    logEvent(LOG_INFO, logCode, logMsg);
    publishMqttStatus();
    menu.requestRedraw();
}

void onMenuTrendReset() {
    resetEloremenoStats("TREND_RESET_BY_MENU", "Statisztika nullazva (menu)");
}

void onMenuRequestSensorScan() {
    startSensorScanJob(nullptr);
}

void onMenuRequestSensorLearn(uint8_t role, const uint8_t* addr) {
    if (role >= SENSOR_ROLE_COUNT || !addr) return;
    startSensorLearnJob(role, addr, nullptr);
}

// ============================================================================
// MQTT CMD FELDOLGOZÁS (Frontend -> ESP)
// ============================================================================
void logMqttSettingChanges(const SystemState& prev, const SystemState& next) {
    char msg[48];
    if (fabsf(prev.targetTemp - next.targetTemp) > 0.01f) {
        snprintf(msg, sizeof(msg), "Celhofok %.1f->%.1f (mqtt)", prev.targetTemp, next.targetTemp);
        logEvent(LOG_INFO, "SETTING", msg);
    }
    if (prev.mode != next.mode) {
        snprintf(msg, sizeof(msg), "Uzmod %s->%s (mqtt)", modeToStr(prev.mode), modeToStr(next.mode));
        logEvent(LOG_INFO, "SETTING", msg);
    }
    if (prev.manualHeating != next.manualHeating) {
        snprintf(msg, sizeof(msg), "ManualFut %s->%s (mqtt)",
                 prev.manualHeating ? "BE" : "KI", next.manualHeating ? "BE" : "KI");
        logEvent(LOG_INFO, "SETTING", msg);
    }
    if (prev.pumpConfig.pumpMode != next.pumpConfig.pumpMode) {
        snprintf(msg, sizeof(msg), "PumpMod %s->%s (mqtt)",
                 pumpModeToStr(prev.pumpConfig.pumpMode), pumpModeToStr(next.pumpConfig.pumpMode));
        logEvent(LOG_INFO, "SETTING", msg);
    }
    if (fabsf(prev.pumpConfig.pumpStartTemp - next.pumpConfig.pumpStartTemp) > 0.01f) {
        snprintf(msg, sizeof(msg), "PumpStart %.1f->%.1f (mqtt)",
                 prev.pumpConfig.pumpStartTemp, next.pumpConfig.pumpStartTemp);
        logEvent(LOG_INFO, "SETTING", msg);
    }
    if (fabsf(prev.pumpConfig.pumpStopTemp - next.pumpConfig.pumpStopTemp) > 0.01f) {
        snprintf(msg, sizeof(msg), "PumpStop %.1f->%.1f (mqtt)",
                 prev.pumpConfig.pumpStopTemp, next.pumpConfig.pumpStopTemp);
        logEvent(LOG_INFO, "SETTING", msg);
    }
    if (prev.pumpConfig.pumpStartSensor != next.pumpConfig.pumpStartSensor) {
        snprintf(msg, sizeof(msg), "PumpSzenz %s->%s (mqtt)",
                 pumpSensorToStr(prev.pumpConfig.pumpStartSensor),
                 pumpSensorToStr(next.pumpConfig.pumpStartSensor));
        logEvent(LOG_INFO, "SETTING", msg);
    }
    if (fabsf(prev.alarmConfig.alarmTemp - next.alarmConfig.alarmTemp) > 0.01f) {
        snprintf(msg, sizeof(msg), "RiasztHof %.1f->%.1f (mqtt)",
                 prev.alarmConfig.alarmTemp, next.alarmConfig.alarmTemp);
        logEvent(LOG_INFO, "SETTING", msg);
    }
    if (prev.alarmConfig.alarmEnabled != next.alarmConfig.alarmEnabled) {
        snprintf(msg, sizeof(msg), "Riasztas %s->%s (mqtt)",
                 prev.alarmConfig.alarmEnabled ? "BE" : "KI",
                 next.alarmConfig.alarmEnabled ? "BE" : "KI");
        logEvent(LOG_INFO, "SETTING", msg);
    }
    if (prev.heartbeatIntervalSec != next.heartbeatIntervalSec) {
        snprintf(msg, sizeof(msg), "Ping %us->%us (mqtt)",
                 prev.heartbeatIntervalSec, next.heartbeatIntervalSec);
        logEvent(LOG_INFO, "SETTING", msg);
    }
    if (computeSavedOutputsMask(prev) != computeSavedOutputsMask(next)) {
        snprintf(msg, sizeof(msg), "Kimenetek 0x%02X->0x%02X (mqtt)",
                 computeSavedOutputsMask(prev), computeSavedOutputsMask(next));
        logEvent(LOG_INFO, "SETTING", msg);
    }
}

void triggerNanoTimeSync() {
    if (!state.ntpSynced) {
        return;
    }
    time_t epochNow = time(nullptr);
    if (epochNow < 1700000000) {
        return;
    }
    uint32_t now = millis();
    pendingTimeSyncEpoch = static_cast<uint32_t>(epochNow);
    syncRetryWaiting     = false;

    requestImmediateNrfSend();
    handleNrf(now);
    publishMqttStatus();
    menu.requestRedraw();
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    if (strcmp(topic, TOPIC_CMD) != 0) return;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload, length);
    if (err) {
        char msg[48];
        snprintf(msg, sizeof(msg), "JSON hiba: %s", err.c_str());
        logEvent(LOG_WARN, "MQTT_JSON", msg);
        return;
    }

    if (doc["action"].is<const char*>()) {
        const char* action = doc["action"].as<const char*>();
        if (strcmp(action, "getLogs") == 0) {
            int page  = doc["page"].is<int>() ? doc["page"].as<int>() : 1;
            int limit = doc["limit"].is<int>() ? doc["limit"].as<int>() : 20;
            publishMqttLogsResponse(page, limit);
            return;
        }
        if (strcmp(action, "clearLogs") == 0) {
            logger.clear();
            logEvent(LOG_INFO, "LOGS_CLEARED", "Naplo uritve (mqtt)");
            menu.requestRedraw();
            return;
        }
        if (strcmp(action, "forceSync") == 0) {
            logEvent(LOG_INFO, "FORCE_SYNC", "Idoszinkron keres (mqtt)");
            if (!state.ntpSynced && WiFi.status() == WL_CONNECTED) {
                loopMonitorSuspend = true;
                configTime(3600, 3600, NTP_SERVER_1, NTP_SERVER_2);
                setenv("TZ", NTP_TZ_INFO, 1);
                tzset();
                ntpConfigured = true;
                ntpInitialTimeoutLogged = false;
                lastNtpAttemptMs = millis();
                struct tm tmCheck{};
                if (getLocalTime(&tmCheck, 0) && (tmCheck.tm_year + 1900 >= 2024)) {
                    state.ntpSynced = true;
                    state.lastNtpSyncEpoch = static_cast<uint32_t>(time(nullptr));
                    logEvent(LOG_INFO, "NTP_OK", "NTP idoszinkron kesz");
                }
            }
            if (state.ntpSynced) {
                bootSyncDone = true;
                triggerNanoTimeSync();
            } else {
                bootSyncDone = false;
                syncRetryWaiting = false;
                publishMqttStatus();
            }
            return;
        }
        if (strcmp(action, "resetTrend") == 0) {
            resetEloremenoStats("TREND_RESET_BY_FRONTEND", "Statisztika nullazva (frontend)");
            return;
        }
        if (strcmp(action, "sensorScan") == 0) {
            const char* cid = doc["cmdId"].is<const char*>() ? doc["cmdId"].as<const char*>() : nullptr;
            if (!startSensorScanJob(cid)) {
                if (cid && cid[0] != '\0') {
                    publishMqttSensorScanAck(cid, false, "BUSY");
                }
            }
            return;
        }
        if (strcmp(action, "sensorLearn") == 0) {
            const char* cid     = doc["cmdId"].is<const char*>() ? doc["cmdId"].as<const char*>() : nullptr;
            const char* roleStr = doc["role"].is<const char*>() ? doc["role"].as<const char*>() : nullptr;
            const char* addrStr = doc["address"].is<const char*>() ? doc["address"].as<const char*>() : nullptr;
            uint8_t roleIdx = 0xFF;
            if (roleStr) {
                if (strcmp(roleStr, "eloremeno") == 0)       roleIdx = SENSOR_ROLE_ELOREMENO;
                else if (strcmp(roleStr, "visszatero") == 0) roleIdx = SENSOR_ROLE_VISSZATERO;
                else if (strcmp(roleStr, "kazan") == 0)      roleIdx = SENSOR_ROLE_KAZAN;
                else if (strcmp(roleStr, "kulteri") == 0)    roleIdx = SENSOR_ROLE_KULTERI;
            }
            uint8_t addrBytes[8] = { 0 };
            if (roleIdx >= SENSOR_ROLE_COUNT || !addrStr || !parseDeviceAddressHex(addrStr, addrBytes)) {
                if (cid && cid[0] != '\0') {
                    publishMqttCmdAck(cid, false, "INVALID_PARAMS", state.relayFeedbackOn);
                }
                return;
            }
            if (!startSensorLearnJob(roleIdx, addrBytes, cid)) {
                if (cid && cid[0] != '\0') {
                    publishMqttCmdAck(cid, false, "BUSY", state.relayFeedbackOn);
                }
            }
            return;
        }
    }

    SystemState staged = state;

    if (doc["targetTemp"].is<float>() || doc["targetTemp"].is<int>()) {
        float t = doc["targetTemp"].as<float>();
        if (t < TARGET_TEMP_MIN) t = TARGET_TEMP_MIN;
        if (t > TARGET_TEMP_MAX) t = TARGET_TEMP_MAX;
        staged.targetTemp = roundf(t * 2.0f) / 2.0f;
    }

    if (doc["mode"].is<const char*>()) {
        staged.mode = strToMode(doc["mode"].as<const char*>());
    }

    if (doc["manualHeating"].is<bool>()) {
        staged.manualHeating = doc["manualHeating"].as<bool>();
    }

    if (doc["kimenetek"].is<JsonObject>()) {
        JsonObject k = doc["kimenetek"].as<JsonObject>();
        if (k["led_zold"].is<bool>())  setBit(staged.outputsMask, OUT_BIT_LED_ZOLD,  k["led_zold"].as<bool>());
        if (k["led_piros"].is<bool>()) staged.userLedPiros = k["led_piros"].as<bool>();
        if (k["tartalek1"].is<bool>()) setBit(staged.outputsMask, OUT_BIT_TARTALEK1, k["tartalek1"].as<bool>());
        if (k["tartalek2"].is<bool>()) setBit(staged.outputsMask, OUT_BIT_TARTALEK2, k["tartalek2"].as<bool>());
        if (k["tartalek3"].is<bool>()) setBit(staged.outputsMask, OUT_BIT_TARTALEK3, k["tartalek3"].as<bool>());
        if (k["tartalek4"].is<bool>()) setBit(staged.outputsMask, OUT_BIT_TARTALEK4, k["tartalek4"].as<bool>());
        if (k["tartalek5"].is<bool>()) setBit(staged.outputsMask, OUT_BIT_TARTALEK5, k["tartalek5"].as<bool>());
    }

    if (doc["pumpConfig"].is<JsonObject>()) {
        JsonObject pc = doc["pumpConfig"].as<JsonObject>();
        if (pc["pumpMode"].is<const char*>()) {
            uint8_t nextPumpMode = strToPumpMode(pc["pumpMode"].as<const char*>());
            if (nextPumpMode == PUMP_MODE_MANUAL_OFF && staged.pumpConfig.pumpMode != PUMP_MODE_MANUAL_OFF) {
                staged.manualOffStartMs = millis();
            }
            staged.pumpConfig.pumpMode = nextPumpMode;
        }
        if (pc["pumpStartTemp"].is<float>() || pc["pumpStartTemp"].is<int>()) {
            staged.pumpConfig.pumpStartTemp = pc["pumpStartTemp"].as<float>();
        }
        if (pc["pumpStopTemp"].is<float>() || pc["pumpStopTemp"].is<int>()) {
            staged.pumpConfig.pumpStopTemp = pc["pumpStopTemp"].as<float>();
        }
        if (pc["pumpStartSensor"].is<const char*>()) {
            staged.pumpConfig.pumpStartSensor = strToPumpSensor(pc["pumpStartSensor"].as<const char*>());
        }
        sanitizePumpConfig(staged.pumpConfig);
    }

    bool safetyIgnored = !doc["safetyOverheatTemp"].isNull() || !doc["safety"].isNull();

    if (doc["alarmConfig"].is<JsonObject>()) {
        JsonObject ac = doc["alarmConfig"].as<JsonObject>();
        if (!ac["safetyOverheatTemp"].isNull()) {
            safetyIgnored = true;
        }
        if (ac["alarmTemp"].is<float>() || ac["alarmTemp"].is<int>()) {
            staged.alarmConfig.alarmTemp = ac["alarmTemp"].as<float>();
        } else if (ac["overheatTemp"].is<float>() || ac["overheatTemp"].is<int>()) {
            staged.alarmConfig.alarmTemp = ac["overheatTemp"].as<float>();
            logEvent(LOG_WARN, "DEPRECATED", "DEPRECATED overheatTemp -> alarmTemp");
        }
        if (ac["alarmEnabled"].is<bool>()) {
            staged.alarmConfig.alarmEnabled = ac["alarmEnabled"].as<bool>() ? 1 : 0;
        }
        sanitizeAlarmConfig(staged.alarmConfig);
    }

    if (safetyIgnored) {
        logEvent(LOG_WARN, "SAFETY_WARN", "SAFETY_TEMP_IGNORED");
    }

    if (doc["heartbeat"].is<JsonObject>()) {
        JsonObject hb = doc["heartbeat"].as<JsonObject>();
        if (hb["intervalSec"].is<int>()) {
            int sec = hb["intervalSec"].as<int>();
            if (isValidHeartbeatInterval(sec)) {
                staged.heartbeatIntervalSec = static_cast<uint8_t>(sec);
            }
        }
    }

    const char* incomingCmdId = nullptr;
    if (doc["cmdId"].is<const char*>()) {
        const char* cid = doc["cmdId"].as<const char*>();
        if (cid && cid[0] != '\0') {
            incomingCmdId = cid;
        }
    }

    bool requestedPumpOff = false;
    if (doc["pumpConfig"].is<JsonObject>()) {
        JsonObject pcCheck = doc["pumpConfig"].as<JsonObject>();
        if (pcCheck["pumpMode"].is<const char*>() &&
            strcmp(pcCheck["pumpMode"].as<const char*>(), "manual_off") == 0) {
            requestedPumpOff = true;
        }
    }
    if (doc["kimenetek"].is<JsonObject>()) {
        JsonObject kCheck = doc["kimenetek"].as<JsonObject>();
        if (kCheck["szivattyu"].is<bool>() && !kCheck["szivattyu"].as<bool>()) {
            requestedPumpOff = true;
        }
    }

    if (!saveChangedSettingsFromState(state, staged)) {
        // Mentési hiba esetén megtartjuk a régi értéket RAM-ban, nem küldünk STATUS-t
        if (incomingCmdId) {
            publishMqttCmdAck(incomingCmdId, false, "NVS_FAIL", state.relayFeedbackOn);
        }
        return;
    }

    logMqttSettingChanges(state, staged);
    state = staged;

    if (incomingCmdId) {
        if (pendingCmdActive && strcmp(pendingCmdId, incomingCmdId) != 0) {
            char prevId[64];
            strncpy(prevId, pendingCmdId, sizeof(prevId) - 1);
            prevId[sizeof(prevId) - 1] = '\0';
            publishMqttCmdAck(prevId, false, "ACK_TIMEOUT", state.relayFeedbackOn);
        }
        pendingCmdActive           = true;
        strncpy(pendingCmdId, incomingCmdId, sizeof(pendingCmdId) - 1);
        pendingCmdId[sizeof(pendingCmdId) - 1] = '\0';
        pendingCmdSeq              = nextCmdSeqFromCmdId(doc, incomingCmdId);
        pendingCmdAt               = millis();
        pendingCmdRequestedPumpOff = requestedPumpOff;
        pendingCmdSentInCurrentNrf = false;
        pendingCmdWaitingSettle    = false;
        pendingCmdLastPollAt       = 0;
    }

    commitSettingEffects(!menu.isEditing());
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
        logEvent(LOG_ERROR, "NVS_FAIL", "WiFi NVS begin hiba");
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
        logEvent(LOG_ERROR, "NVS_FAIL", "WiFi adatok NVS mentesi hiba");
        sendWifiPortalPage("A Wi-Fi adatok mentése nem sikerült. Próbáld újra.", 500);
        return;
    }

    wifiSsid = ssid;
    wifiPassword = password;
    wifiPortalMessage = "Az adatok elmentve. Csatlakozás folyamatban; ha nem sikerül, add meg újra a hálózatot.";
    loopMonitorSuspend = true;
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
    lastWifiTryMs = millis();
    wifiDisconnectStartedMs = lastWifiTryMs;
    wifiDisconnectTimerActive = true;
    wifiConnectionFailureReported = false;
    logEvent(LOG_INFO, "WIFI_SAVE", "WiFi adatok mentve, csatlakozas");
    sendWifiPortalPage(wifiPortalMessage);
}

void handleWifiPortalNotFound() {
    wifiPortalServer.sendHeader("Location", String("http://") + WIFI_PORTAL_IP + "/");
    wifiPortalServer.send(302, "text/plain", "");
}

void beginWifiPortal(const char* message) {
    loopMonitorSuspend = true;
    wifiPortalMessage = message;
    lastWifiPortalTryMs = millis();
    WiFi.mode(wifiSsid.length() == 0 ? WIFI_AP : WIFI_AP_STA);
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char apPassword[13];
    snprintf(apPassword, sizeof(apPassword), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (!WiFi.softAP(WIFI_SETUP_AP_SSID, apPassword)) {
        logEvent(LOG_ERROR, "WIFI_AP", "Beallito AP inditasi hiba");
        return;
    }

    wifiPortalDns.start(WIFI_DNS_PORT, "*", WiFi.softAPIP());
    wifiPortalServer.begin();
    wifiPortalActive = true;
    char apMsg[48];
    snprintf(apMsg, sizeof(apMsg), "AP:%s PW:%s", WIFI_SETUP_AP_SSID, apPassword);
    logEvent(LOG_INFO, "WIFI_AP", apMsg);
}

void stopWifiPortal() {
    loopMonitorSuspend = true;
    wifiPortalDns.stop();
    wifiPortalServer.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    wifiPortalActive = false;
    wifiConnectionFailureReported = false;
    wifiPortalMessage = "Válaszd ki az otthoni Wi-Fi hálózatot.";
    logEvent(LOG_INFO, "WIFI_AP", "Beallito AP leallitva");
}

void loadWifiCredentials() {
    Preferences wifiPrefs;
    if (!wifiPrefs.begin(WIFI_NVS_NAMESPACE, true)) {
        logEvent(LOG_WARN, "WIFI_NVS", "Mentett WiFi adatok nem olvashatok");
        return;
    }
    wifiSsid = wifiPrefs.getString("ssid", "");
    wifiPassword = wifiPrefs.getString("password", "");
    wifiPrefs.end();
}

void startNtpSync(uint32_t now) {
    loopMonitorSuspend = true;
    configTime(3600, 3600, NTP_SERVER_1, NTP_SERVER_2);
    setenv("TZ", NTP_TZ_INFO, 1);
    tzset();
    ntpConfigured    = true;
    lastNtpAttemptMs = now;
}

void maintainTimeAndSync(uint32_t now) {
    if (WiFi.status() == WL_CONNECTED) {
        if (!ntpConfigured) {
            startNtpSync(now);
        }

        struct tm tmNow{};
        if (getLocalTime(&tmNow, 0) && (tmNow.tm_year + 1900 >= 2024)) {
            time_t epochNow = time(nullptr);
            if (!state.ntpSynced) {
                state.ntpSynced = true;
                state.lastNtpSyncEpoch = static_cast<uint32_t>(epochNow);
                logEvent(LOG_INFO, "NTP_OK", "NTP idoszinkron kesz");
                publishMqttStatus();
                menu.requestRedraw();
            } else {
                state.lastNtpSyncEpoch = static_cast<uint32_t>(epochNow);
            }

            // Statisztika kezdőidőpont beállítása egyszer (boot időpont NTP-vel, nincs éjféli nullázás)
            if (state.statsSinceStr[0] == '\0') {
                uint32_t upSec = static_cast<uint32_t>(uptimeTotalMs / 1000ULL);
                uint32_t startEp = (static_cast<uint32_t>(epochNow) > upSec)
                                       ? (static_cast<uint32_t>(epochNow) - upSec)
                                       : static_cast<uint32_t>(epochNow);
                formatEpochTimeStr(startEp, state.statsSinceStr, sizeof(state.statsSinceStr));
            }

            // Boot utáni egyszeri ESP-Nano szinkronizáció
            if (!bootSyncDone) {
                bootSyncDone = true;
                triggerNanoTimeSync();
            }

            // Naponta 2x (00:00 és 12:00 helyi idő szerint) szinkronizáció
            if ((tmNow.tm_hour == 0 || tmNow.tm_hour == 12) && tmNow.tm_min == 0) {
                int slot = tmNow.tm_hour;
                if (lastScheduledYday != tmNow.tm_yday || lastScheduledSlot != slot) {
                    lastScheduledYday = tmNow.tm_yday;
                    lastScheduledSlot = slot;
                    triggerNanoTimeSync();
                }
            }

            // Sikertelen szinkron 1 óra múlva történő újrapróbálása
            // OVERFLOW-SAFE: uint32_t kivonás, 49 napos millis túlcsorduláskor is helyes.
            if (syncRetryWaiting &&
                static_cast<uint32_t>(now - syncRetryStartMs) >= SYNC_RETRY_INTERVAL_MS) {
                triggerNanoTimeSync();
            }
        } else if (!state.ntpSynced) {
            // Nem-blokkoló max 5 mp timeout ellenőrzés + 60 mp újrapróbálkozás
            // OVERFLOW-SAFE: uint32_t kivonás, 49 napos millis túlcsorduláskor is helyes.
            if (!ntpInitialTimeoutLogged &&
                static_cast<uint32_t>(now - lastNtpAttemptMs) >= NTP_TIMEOUT_MS) {
                ntpInitialTimeoutLogged = true;
                logEvent(LOG_WARN, "NTP_WAIT", "NTP varakozas, ujraproba 60mp");
            }
            if (static_cast<uint32_t>(now - lastNtpAttemptMs) >= NTP_RETRY_INTERVAL_MS) {
                startNtpSync(now);
            }
        }
    }
}

void maintainConnectivity(uint32_t now) {
    if (wifiPortalActive) {
        wifiPortalDns.processNextRequest();
        wifiPortalServer.handleClient();
    }

    if (WiFi.status() != WL_CONNECTED) {
        if (prevWifiConnected) {
            prevWifiConnected = false;
            logEvent(LOG_WARN, "WIFI_LOST", "WiFi kapcsolat megszakadt");
        }
        if (prevMqttConnected) {
            prevMqttConnected = false;
            logEvent(LOG_WARN, "MQTT_LOST", "MQTT kapcsolat megszakadt");
        }

        // OVERFLOW-SAFE: uint32_t kivonás, 49 napos millis túlcsorduláskor is helyes.
        if (wifiSsid.length() == 0) {
            if (!wifiPortalActive && static_cast<uint32_t>(now - lastWifiPortalTryMs) >= WIFI_RECONNECT_MS) {
                beginWifiPortal("Válaszd ki az otthoni Wi-Fi hálózatot.");
            }
            return;
        }

        if (!wifiDisconnectTimerActive) {
            wifiDisconnectStartedMs = now;
            wifiDisconnectTimerActive = true;
        }

        if (wifiPortalActive && !wifiConnectionFailureReported &&
            static_cast<uint32_t>(now - wifiDisconnectStartedMs) >= WIFI_PORTAL_TIMEOUT_MS) {
            wifiPortalMessage = "Nem sikerült csatlakozni. Ellenőrizd a hálózatnevet/jelszót, majd próbáld újra.";
            wifiConnectionFailureReported = true;
        }

        if (!wifiPortalActive &&
            static_cast<uint32_t>(now - wifiDisconnectStartedMs) >= WIFI_PORTAL_TIMEOUT_MS &&
            static_cast<uint32_t>(now - lastWifiPortalTryMs) >= WIFI_RECONNECT_MS) {
            beginWifiPortal("Nem sikerült csatlakozni a mentett hálózathoz. Add meg a Wi-Fi adatokat.");
        }

        if (static_cast<uint32_t>(now - lastWifiTryMs) >= WIFI_RECONNECT_MS) {
            lastWifiTryMs = now;
            loopMonitorSuspend = true;
            logEvent(LOG_INFO, "WIFI_RETRY", "WiFi ujracsatlakozas");
            WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
        }
        return;
    }

    if (!prevWifiConnected) {
        prevWifiConnected = true;
        if (!ntpConfigured) {
            startNtpSync(now);
        }
        logEvent(LOG_INFO, "WIFI_OK", "WiFi csatlakozva");
    }

    wifiDisconnectTimerActive = false;
    if (wifiPortalActive) {
        stopWifiPortal();
    }

    if (!mqtt.connected()) {
        if (prevMqttConnected) {
            prevMqttConnected = false;
            logEvent(LOG_WARN, "MQTT_LOST", "MQTT kapcsolat megszakadt");
        }

        // OVERFLOW-SAFE: uint32_t kivonás, 49 napos millis túlcsorduláskor is helyes.
        if (!mqttAttemptedOnce || (static_cast<uint32_t>(now - lastMqttTryMs) >= mqttRetryIntervalMs)) {
            mqttAttemptedOnce = true;
            lastMqttTryMs     = now;

            bool caConfigured =
                strstr_P(HIVEMQ_CA_CERT, PSTR("TODO_")) == nullptr &&
                strstr_P(HIVEMQ_CA_CERT, PSTR("-----BEGIN CERTIFICATE-----")) != nullptr;

            if (!caConfigured) {
                logEvent(LOG_ERROR, "TLS_FAIL", "TLS CERT FAILED");
                menu.showErrorBanner("TLS HIBA!", "CA tanusitvany!", 2000);
                mqttRetryIntervalMs = TLS_CERT_RETRY_MS;
                return;
            }

            loopMonitorSuspend = true;
            esp_task_wdt_reset();
            if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD,
                             TOPIC_STATUS, 1, true,
                             "{\"online\":false,\"error\":\"Gateway offline\"}")) {
                esp_task_wdt_reset();
                prevMqttConnected   = true;
                mqttRetryIntervalMs = MQTT_RECONNECT_MS;
                mqtt.subscribe(TOPIC_CMD);
                logEvent(LOG_INFO, "MQTT_OK", "HiveMQ Cloud csatlakozva");
                publishMqttStatus();
            } else {
                esp_task_wdt_reset();
                char tlsErrBuf[128] = { 0 };
                int tlsErr = wifiSecureClient.lastError(tlsErrBuf, sizeof(tlsErrBuf));
                if (tlsErr != 0) {
                    logEvent(LOG_ERROR, "TLS_FAIL", "TLS CERT FAILED");
                    menu.showErrorBanner("TLS HIBA!", "CA tanusitvany!", 2000);
                    mqttRetryIntervalMs = TLS_CERT_RETRY_MS;
                } else {
                    mqttRetryIntervalMs = MQTT_RECONNECT_MS;
                    char msg[48];
                    snprintf(msg, sizeof(msg), "MQTT hiba rc=%d", mqtt.state());
                    logEvent(LOG_WARN, "MQTT_FAIL", msg);
                }
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
            // OVERFLOW-SAFE: uint32_t kivonás, 49 napos millis túlcsorduláskor is helyes.
            uint32_t heldMs = static_cast<uint32_t>(edgeMs - btnDownStartMs);
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
        // OVERFLOW-SAFE: uint32_t kivonás, 49 napos millis túlcsorduláskor is helyes.
        if (digitalRead(PIN_ENC_SW) == HIGH && (static_cast<uint32_t>(now - lastBtnPollGuardMs) >= BTN_DEBOUNCE_MS)) {
            uint32_t heldMs = static_cast<uint32_t>(now - btnDownStartMs);
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
            uint32_t heldMs = static_cast<uint32_t>(now - btnDownStartMs);
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

void evaluateEspLoopMonitor(uint32_t loopStartUs, uint32_t now) {
    if (loopMonitorSuspend) {
        loopMonitorSuspend = false;
        return;
    }

    // OVERFLOW-SAFE: uint32_t kivonás, 71 perces micros() túlcsorduláskor is helyes.
    lastLoopUs = static_cast<uint32_t>(micros() - loopStartUs);
    if (lastLoopUs > maxLoopUs) {
        maxLoopUs = lastLoopUs;
    }
    state.loopLastMs = lastLoopUs / 1000UL;
    state.loopMaxMs  = maxLoopUs / 1000UL;

    if (lastLoopUs > ESP_LOOP_WARN_US) {
        loopOverrunCount++;
        // OVERFLOW-SAFE: uint32_t kivonás, 49 napos millis túlcsorduláskor is helyes.
        if (!espHasSlowLogYet || static_cast<uint32_t>(now - espLastSlowLogMs) >= ESP_LOOP_WARN_COOLDOWN_MS) {
            espHasSlowLogYet = true;
            espLastSlowLogMs = now;
            char msg[48];
            snprintf(msg, sizeof(msg), "ESP_LOOP_SLOW %lums", static_cast<unsigned long>(state.loopLastMs));
            logEvent(LOG_WARN, "ESP_LOOP_SLOW", msg);
        }
    }

    if (lastLoopUs > ESP_LOOP_FAULT_SINGLE_US) {
        espSlowLoopStreak = ESP_LOOP_FAULT_STREAK_CNT;
        espFastLoopStreak = 0;
        if (!loopFault) {
            loopFault       = true;
            state.loopFault = true;
            char msg[48];
            snprintf(msg, sizeof(msg), "ESP loop lelassult (%lums)", static_cast<unsigned long>(state.loopLastMs));
            logEvent(LOG_ERROR, "ESP_LOOP_FAULT", msg);
            publishMqttStatus();
            menu.requestRedraw();
        }
    } else if (lastLoopUs > ESP_LOOP_FAULT_STREAK_US) {
        espFastLoopStreak = 0;
        if (espSlowLoopStreak < ESP_LOOP_FAULT_STREAK_CNT) {
            espSlowLoopStreak++;
        }
        if (!loopFault && espSlowLoopStreak >= ESP_LOOP_FAULT_STREAK_CNT) {
            loopFault       = true;
            state.loopFault = true;
            char msg[48];
            snprintf(msg, sizeof(msg), "ESP loop tartosan lassu (%lums)", static_cast<unsigned long>(state.loopLastMs));
            logEvent(LOG_ERROR, "ESP_LOOP_FAULT", msg);
            publishMqttStatus();
            menu.requestRedraw();
        }
    } else {
        espSlowLoopStreak = 0;
        if (lastLoopUs < ESP_LOOP_RECOVER_US) {
            if (espFastLoopStreak < ESP_LOOP_RECOVER_CNT) {
                espFastLoopStreak++;
            }
            if (loopFault && espFastLoopStreak >= ESP_LOOP_RECOVER_CNT) {
                loopFault       = false;
                state.loopFault = false;
                logEvent(LOG_INFO, "ESP_LOOP_OK", "ESP loop ciklusido helyreallt");
                publishMqttStatus();
                menu.requestRedraw();
            }
        } else {
            espFastLoopStreak = 0;
        }
    }
}

// ============================================================================
// SETUP & LOOP
// ============================================================================
void setup() {
    Serial.begin(115200);
    delay(200);
    logEvent(LOG_INFO, "BOOT", "Indulas V5.4");
    esp_reset_reason_t resetReason = esp_reset_reason();
    if (resetReason == ESP_RST_TASK_WDT || resetReason == ESP_RST_WDT) {
        logEvent(LOG_WARN, "WDT_RESET", "Elozo ujraindulas hardveres watchdog miatt tortent");
    }

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
    menu.begin(&lcd, &state, {
        onMenuSettingCommitted,
        onMenuFactoryReset,
        onMenuTrendReset,
        onMenuRequestSensorScan,
        onMenuRequestSensorLearn
    });

    if (!spiDmaBus.begin(NRF_SPI_HOST,
                         PIN_NRF_MOSI,
                         PIN_NRF_MISO,
                         PIN_NRF_SCK,
                         NRF_SPI_DMA_CHANNEL,
                         NRF_SPI_MAX_XFER_SZ) ||
        !nrfDma.begin(spiDmaBus,
                      PIN_NRF_CE,
                      PIN_NRF_CSN,
                      NRF_SPI_CLOCK_HZ,
                      NRF_SPI_QUEUE_SIZE)) {
        logEvent(LOG_ERROR, "NRF_INIT", "NRF24L01+ SPI2 DMA nem talalhato");
    } else {
        logEvent(LOG_INFO, "NRF_INIT", "NRF24L01+ MASTER SPI2 GDMA (10MHz) keszenletben");
    }

    uint32_t now = millis();
    uptimePrevMs           = now;
    uptimeTotalMs          = now;
    lastAlarmBlinkMs       = now;
    lastTrendSampleMs      = now;
    lastPing               = now;
    lastNrfOk              = now;
    lastMqttPublishRetryMs = now;
    evaluateRoomHeatingIndicator();
    evaluateBoilerPumpProtection(now);
    startNrfRequest(now);
    updateAlarmBlinkAndLed(now);
    lcd.setStatusIcons(state.pumpOn, state.nrf24Ok, false);
    lcd.setTrend(state.trendDir);
    menu.requestRedraw();
    menu.update(now, alarmBlinkOn);
    lcd.refresh(now);

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

    static_assert(TLS_REQUIRE_CA_CERT, "CA tanusitvany ellenorzes kotelezo");
    wifiSecureClient.setCACert(HIVEMQ_CA_CERT);
    wifiSecureClient.setTimeout(3);

    mqtt.setServer(MQTT_BROKER, MQTT_PORT);
    mqtt.setBufferSize(4096);
    mqtt.setCallback(mqttCallback);

    // 5 másodperces hardveres task watchdog engedélyezése
#if defined(ESP_IDF_VERSION_MAJOR) && (ESP_IDF_VERSION_MAJOR >= 5)
    esp_task_wdt_config_t wdtCfg = {
        .timeout_ms     = ESP_WDT_TIMEOUT_SEC * 1000UL,
        .idle_core_mask = 0,
        .trigger_panic  = true
    };
    esp_task_wdt_reconfigure(&wdtCfg);
#else
    esp_task_wdt_init(ESP_WDT_TIMEOUT_SEC, true);
#endif
    esp_task_wdt_add(nullptr);
    loopMonitorSuspend = false;
}

void loop() {
    uint32_t loopStartUs = micros();
    loopMonitorSuspend   = false;
    uint32_t loopStartMs = millis();
    uint32_t now = loopStartMs;
    // OVERFLOW-SAFE: uint32_t kivonás, 49 napos millis túlcsorduláskor is helyes.
    uptimeTotalMs += static_cast<uint32_t>(now - uptimePrevMs);
    uptimePrevMs = now;

    // 0. Nem-blokkoló 500ms riasztási LED villogás
    updateAlarmBlinkAndLed(now);

    // 0b. Sikertelen NVS mentés egyszeri újrapróbálkozása a következő loop-ban
    processPendingNvsRetry(now);

    // 1. Encoder és gomb események feldolgozása (interrupt + debounce)
    processEncoderAndButton(now);

    // 2. LM35 szobahő mérés és szobafűtés-jelzés (1 mp-enként)
    // OVERFLOW-SAFE: uint32_t kivonás, 49 napos millis túlcsorduláskor is helyes.
    if (static_cast<uint32_t>(now - lastLm35Ms) >= LM35_READ_INTERVAL_MS) {
        lastLm35Ms = now;
        float prevTemp = state.currentTemp;
        bool prevHeat  = state.heatingOn;
        bool prevPump  = state.pumpOn;

        state.currentTemp = readRoomTempLM35();
        evaluateRoomHeatingIndicator();
        bool modeRestored = evaluateBoilerPumpProtection(now);
        if (modeRestored) {
            saveSetting("pumpMode", static_cast<uint32_t>(state.pumpConfig.pumpMode));
        }

        if (modeRestored || prevHeat != state.heatingOn || prevPump != state.pumpOn ||
            fabsf(prevTemp - state.currentTemp) >= 0.2f) {
            requestImmediateNrfSend();
            publishMqttStatus();
        }
    }

    // 3. V5 Master-Slave nem-blokkoló NRF24 állapotgép (minden loop()-ban fut)
    tickSensorJob(now);
    if (pendingCmdActive && pendingCmdWaitingSettle && nrfState == NRF_IDLE &&
        static_cast<uint32_t>(now - pendingCmdLastPollAt) >= CMD_RELAY_SETTLE_POLL_MS) {
        pendingCmdWaitingSettle = false;
        requestImmediateNrfSend();
    }
    handleNrf(now);

    if (evaluateRelayFault(now, false)) {
        evaluateAlarmState();
        if (state.alarmConfig.alarmEnabled != 0 && state.alarmState.active != 0) {
            setBit(state.outputsMask, OUT_BIT_LED_PIROS, true);
        } else {
            setBit(state.outputsMask, OUT_BIT_LED_PIROS, state.userLedPiros);
        }
        requestImmediateNrfSend();
        publishMqttStatus();
        menu.requestRedraw();
    }

    // 3a. End-to-end CMD_ACK 3000ms timeout ellenőrzése
    // OVERFLOW-SAFE: uint32_t kivonás, 49 napos millis túlcsorduláskor is helyes.
    if (pendingCmdActive && (static_cast<uint32_t>(now - pendingCmdAt) >= CMD_ACK_TIMEOUT_MS)) {
        char timedOutId[64];
        strncpy(timedOutId, pendingCmdId, sizeof(timedOutId) - 1);
        timedOutId[sizeof(timedOutId) - 1] = '\0';
        clearPendingCmd();
        logEvent(LOG_WARN, "CMD_ACK_TIMEOUT", timedOutId);
        publishMqttCmdAck(timedOutId, false, "ACK_TIMEOUT", state.relayFeedbackOn);
    }

    // 3b. Sikertelen MQTT status/telemetry újrapróbálása nem-blokkolva
    // OVERFLOW-SAFE: uint32_t kivonás, 49 napos millis túlcsorduláskor is helyes.
    if (mqtt.connected() &&
        (mqttStatusPendingRetry || mqttTelemetryPendingRetry) &&
        static_cast<uint32_t>(now - lastMqttPublishRetryMs) >= MQTT_RECONNECT_MS) {
        lastMqttPublishRetryMs = now;
        if (mqttStatusPendingRetry) {
            publishMqttStatus();
        }
        if (mqttTelemetryPendingRetry) {
            if (publishMqttTelemetry()) {
                lastTelemetryMs = now;
            }
        }
    }

    // 4. Nem-blokkoló LCD és menü frissítés (ikonok + trend + 100ms-es millis() ciklus)
    bool wifiMqttOk = (WiFi.status() == WL_CONNECTED) && mqtt.connected();
    lcd.setStatusIcons(state.pumpOn, state.nrf24Ok, wifiMqttOk);
    lcd.setTrend(state.trendDir);
    menu.update(now, alarmBlinkOn);
    lcd.refresh(now);

    // 5. WiFi, NTP, ESP-Nano szinkron és HiveMQ Cloud MQTT karbantartás
    maintainConnectivity(now);
    maintainTimeAndSync(now);

    // 6. Előremenő trend mintavétel és számítás (60 mp-enként, 20 elemű körkörös puffer)
    // OVERFLOW-SAFE: uint32_t kivonás, 49 napos millis túlcsorduláskor is helyes.
    if (static_cast<uint32_t>(now - lastTrendSampleMs) >= TREND_SAMPLE_INTERVAL_MS) {
        lastTrendSampleMs = now;
        if (state.nrf24Ok && isSensorValid(state.eloremeno)) {
            trendSamples[trendHead] = state.eloremeno;
            trendHead = static_cast<uint8_t>((trendHead + 1) % TREND_BUFFER_SIZE);
            if (state.trendSampleCount < TREND_BUFFER_SIZE) {
                state.trendSampleCount++;
            }
        }

        uint8_t prevTrendDir = state.trendDir;
        if (state.trendSampleCount < TREND_BUFFER_SIZE) {
            state.trendDir   = TREND_FLAT;
            state.trendDelta = 0.0f;
        } else {
            float sumPrev10 = 0.0f;
            float sumLast10 = 0.0f;
            for (uint8_t i = 0; i < 10; i++) {
                sumPrev10 += trendSamples[(trendHead + i) % TREND_BUFFER_SIZE];
                sumLast10 += trendSamples[(trendHead + 10 + i) % TREND_BUFFER_SIZE];
            }
            float delta = (sumLast10 / 10.0f) - (sumPrev10 / 10.0f);
            state.trendDelta = round1(delta);
            if (delta > TREND_THRESHOLD_C) {
                state.trendDir = TREND_UP;
            } else if (delta < -TREND_THRESHOLD_C) {
                state.trendDir = TREND_DOWN;
            } else {
                state.trendDir = TREND_FLAT;
            }
        }

        lcd.setTrend(state.trendDir);
        if (prevTrendDir != state.trendDir) {
            publishMqttStatus();
        }
    }

    // 7. Telemetry 30 másodpercenként
    // OVERFLOW-SAFE: uint32_t kivonás, 49 napos millis túlcsorduláskor is helyes.
    if (static_cast<uint32_t>(now - lastTelemetryMs) >= TELEMETRY_INTERVAL_MS) {
        lastTelemetryMs = now;
        publishMqttTelemetry();
    }

#if defined(DEBUG) || (defined(DEBUG_MODE) && DEBUG_MODE)
    uint32_t loopElapsedMs = static_cast<uint32_t>(millis() - loopStartMs);
    if (loopElapsedMs > 50UL) {
        Serial.printf("[DEBUG] loop() ciklusido=%lums dmaWaitUs=%lu (nrfState=%u)\n",
                      static_cast<unsigned long>(loopElapsedMs),
                      static_cast<unsigned long>(state.dmaWaitUs),
                      static_cast<unsigned>(nrfState));
    }
#endif

    // 8. Loop Monitor + hardveres watchdog reset
    evaluateEspLoopMonitor(loopStartUs, now);
    esp_task_wdt_reset();
}
