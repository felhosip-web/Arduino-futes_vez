#pragma once

#include <stdint.h>

// ============================================================================
// ESP32-C3 KONFIGURÁCIÓ (V5 MASTER / GATEWAY): WIFI, MQTT, PIN-EK, LCD, LM35
// ============================================================================

// --- WiFi beállító hozzáférési pont ---
constexpr const char* WIFI_SETUP_AP_SSID = "Futes-ESP32-Setup";

// --- HiveMQ Cloud MQTT (TLS 8883) ---
constexpr const char* MQTT_BROKER    = "your-cluster-id.s1.eu.hivemq.cloud";
constexpr uint16_t    MQTT_PORT      = 8883;
constexpr const char* MQTT_USER      = "your_mqtt_username";
constexpr const char* MQTT_PASSWORD  = "your_mqtt_password";
constexpr const char* MQTT_CLIENT_ID = "esp32c3-futes-gateway";

// --- MQTT Topic-ok ---
constexpr const char* TOPIC_CMD       = "futes/haz-futes/cmd";
constexpr const char* TOPIC_STATUS    = "futes/haz-futes/status";    // retained=true
constexpr const char* TOPIC_TELEMETRY = "futes/haz-futes/telemetry"; // 30 mp-enként
constexpr const char* TOPIC_LOG       = "futes/haz-futes/log";       // ERROR események azonnal
constexpr const char* TOPIC_LOGS      = "futes/haz-futes/logs";      // getLogs válasz

// --- NTP időszinkron ---
constexpr const char* NTP_SERVER_1           = "pool.ntp.org";
constexpr const char* NTP_SERVER_2           = "time.nist.gov";
constexpr const char* NTP_TZ_INFO            = "CET-1CEST,M3.5.0,M10.5.0/3";
constexpr uint32_t    NTP_TIMEOUT_MS         = 5000UL;     // Max 5 mp NTP timeout
constexpr uint32_t    NTP_RETRY_INTERVAL_MS  = 60000UL;    // 60 mp NTP újrapróbálkozás
constexpr uint32_t    SYNC_RETRY_INTERVAL_MS = 3600000UL;  // 1 óra múlva újrapróba sikertelen Nano szinkronnál
constexpr uint32_t    NANO_SYNC_STALE_SEC    = 46800UL;    // 13 óra (2x12h + 1h tűrés)

// --- PIN-ek (ESP32-C3, ütközésmentes kiosztás) ---
constexpr uint8_t PIN_LM35      = 2;  // ADC1_CH2

constexpr uint8_t PIN_ENC_CLK   = 5;
constexpr uint8_t PIN_ENC_DT    = 6;
constexpr uint8_t PIN_ENC_SW    = 7;

constexpr uint8_t PIN_I2C_SDA   = 8;
constexpr uint8_t PIN_I2C_SCL   = 9;
constexpr uint8_t LCD_I2C_ADDR  = 0x27;
constexpr uint8_t LCD_COLS      = 20;
constexpr uint8_t LCD_ROWS      = 2;

constexpr uint8_t PIN_ALARM_LED = 11; // Riasztó piros LED (GPIO 11, 500ms villogás)

constexpr uint8_t PIN_NRF_CE    = 20; // Szabad GPIO20 (GPIO18/19 USB CDC)
constexpr uint8_t PIN_NRF_CSN   = 1;
constexpr uint8_t PIN_NRF_MISO  = 3;
constexpr uint8_t PIN_NRF_SCK   = 4;
constexpr uint8_t PIN_NRF_MOSI  = 10;

// --- Encoder és Menü beállítások ---
// Állítsd 1-re, ha az encoder forgási iránya fordított a hardvereden
#define ENCODER_REVERSE 0

constexpr uint32_t MENU_TIMEOUT_MS        = 20000UL; // 20 mp tétlenség -> vissza a főképernyőre
constexpr uint32_t LONG_PRESS_MS          = 1000UL;  // >1000ms -> vissza egy szintet / kilépés
constexpr uint32_t SHORT_PRESS_MAX_MS     = 800UL;   // <800ms  -> belépés / jóváhagyás
constexpr uint32_t FACTORY_RESET_HOLD_MS  = 3000UL;  // 3 mp hosszú nyomás -> gyári visszaállítás
constexpr uint32_t FACTORY_BANNER_MS      = 2000UL;  // 2 mp "GYARI BEALLITAS!" üzenet
constexpr uint32_t LCD_MAIN_REFRESH_MS    = 1000UL;  // Főképernyő 1 mp-es frissítés
constexpr uint32_t LCD_MENU_REFRESH_MS    = 100UL;   // Menü 100 ms-es frissítés (ha változott)
constexpr uint32_t EDIT_BLINK_INTERVAL_MS = 500UL;   // Szerkesztés közbeni < > villogás

// --- LM35 Kalibráció és szobai hiszterézis ---
constexpr float   LM35_OFFSET         = 0.0f;
constexpr uint8_t LM35_SAMPLE_COUNT   = 10;

constexpr float TARGET_TEMP_DEFAULT   = 21.5f;
constexpr float TARGET_TEMP_MIN       = 16.0f;
constexpr float TARGET_TEMP_MAX       = 26.0f;
constexpr float TARGET_TEMP_STEP      = 0.5f;

constexpr float HYST_LOW_OFFSET       = 0.3f; // BE ha currentTemp < target - 0.3
constexpr float HYST_HIGH_OFFSET      = 0.3f; // KI ha currentTemp > target + 0.3

// --- Kazánvédelem szivattyú alapértékek és korlátok ---
constexpr float PUMP_START_TEMP_DEFAULT = 40.0f;
constexpr float PUMP_START_TEMP_MIN     = 30.0f;
constexpr float PUMP_START_TEMP_MAX     = 50.0f;

constexpr float PUMP_STOP_TEMP_DEFAULT  = 30.0f;
constexpr float PUMP_STOP_TEMP_MIN      = 20.0f;
constexpr float PUMP_STOP_TEMP_MAX      = 40.0f;
constexpr float PUMP_MIN_HYSTERESIS_GAP = 5.0f;  // pumpStopTemp < pumpStartTemp - 5

// --- Riasztás és fix szivattyú biztonsági küszöb ---
constexpr float ALARM_OVERHEAT_TEMP_DEFAULT = 85.0f;
constexpr float ALARM_OVERHEAT_TEMP_MIN     = 70.0f;
constexpr float ALARM_OVERHEAT_TEMP_MAX     = 90.0f;
constexpr bool  ALARM_ENABLED_DEFAULT       = true;

#ifndef SAFETY_OVERHEAT_TEMP_DEFINED
#define SAFETY_OVERHEAT_TEMP_DEFINED
// Fix szivattyú biztonsági határ (NEM állítható, NEM mentődik NVS-be, NEM módosítható menüből/MQTT-ről)
constexpr float SAFETY_OVERHEAT_TEMP        = 95.0f;
#endif

constexpr uint32_t PUMP_MANUAL_OFF_TIMEOUT_MS = 600000UL; // 10 perc -> vissza auto-ra
constexpr uint32_t ALARM_BLINK_INTERVAL_MS    = 500UL;    // 500ms BE / 500ms KI

// --- V5 Master-Slave Életjel (Heartbeat) időzítések ---
constexpr uint32_t NRF_RESPONSE_TIMEOUT_MS  = 500UL; // Max 500ms várakozás RESPONSE-ra
constexpr uint8_t  NRF_MAX_ATTEMPTS         = 3;     // Max 3 próba egy ping ciklusban
constexpr uint8_t  NRF_MAX_MISSED_CYCLES    = 3;     // 3 egymás utáni sikertelen ciklus -> nrf_fail

// --- TLS biztonsági beállítások (kizárólag CA tanúsítvány-ellenőrzéssel) ---
constexpr bool     TLS_REQUIRE_CA_CERT    = true;
constexpr uint32_t TLS_CERT_RETRY_MS      = 30000UL; // Sikertelen TLS handshake után 30 mp múlva újrapróba

constexpr uint32_t TELEMETRY_INTERVAL_MS  = 30000UL; // 30 mp-enként telemetry
constexpr uint32_t LM35_READ_INTERVAL_MS  = 1000UL;  // 1 mp-enként szobahő mérés
constexpr uint32_t WIFI_RECONNECT_MS      = 10000UL; // 10 mp nem-blokkoló WiFi retry
constexpr uint32_t WIFI_PORTAL_TIMEOUT_MS = 30000UL; // 30 mp sikertelen WiFi után beállító portál
constexpr uint32_t MQTT_RECONNECT_MS      = 5000UL;  // 5 mp nem-blokkoló MQTT retry
constexpr uint32_t BTN_DEBOUNCE_MS        = 50UL;

// --- Előremenő hőmérséklet trend és napi statisztika ---
constexpr uint32_t TREND_SAMPLE_INTERVAL_MS = 60000UL; // 1 perces mintavétel
constexpr uint8_t  TREND_BUFFER_SIZE        = 20;      // 20 perc (10+10 perc)
constexpr float    TREND_THRESHOLD_C        = 0.5f;    // ±0.5 °C küszöb

enum TrendDirection : uint8_t {
    TREND_FLAT = 0,
    TREND_UP   = 1,
    TREND_DOWN = 2
};
