#pragma once

#include <stdint.h>

// ============================================================================
// ESP32-C3 KONFIGURÁCIÓ (V4 + RIASZTÁS): WIFI, MQTT, PIN-EK, LCD, LM35
// ============================================================================

// --- WiFi beállítások ---
constexpr const char* WIFI_SSID      = "YOUR_WIFI_SSID";
constexpr const char* WIFI_PASSWORD  = "YOUR_WIFI_PASSWORD";

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

// --- PIN-ek (ESP32-C3, ütközésmentes kiosztás) ---
// Megjegyzés: GPIO 0/2/8/9 strapping pinek; GPIO 18/19 az USB CDC (D-/D+) miatt foglaltak.
// A PIN_NRF_CE a szabad GPIO 20 lábon, az új riasztó piros LED a szabad GPIO 11 lábon van.
constexpr uint8_t PIN_LM35      = 2;  // ADC1_CH2

constexpr uint8_t PIN_ENC_CLK   = 5;
constexpr uint8_t PIN_ENC_DT    = 6;
constexpr uint8_t PIN_ENC_SW    = 7;

constexpr uint8_t PIN_I2C_SDA   = 8;
constexpr uint8_t PIN_I2C_SCL   = 9;
constexpr uint8_t LCD_I2C_ADDR  = 0x27;
constexpr uint8_t LCD_COLS      = 20;
constexpr uint8_t LCD_ROWS      = 2;

constexpr uint8_t PIN_NRF_CE    = 20; // Szabad GPIO20 (korábbi GPIO0 strapping pin helyett; GPIO18/19 USB CDC)
constexpr uint8_t PIN_NRF_CSN   = 1;
constexpr uint8_t PIN_NRF_MISO  = 3;
constexpr uint8_t PIN_NRF_SCK   = 4;
constexpr uint8_t PIN_NRF_MOSI  = 10;

constexpr uint8_t PIN_ALARM_LED = 11; // Riasztó piros LED (GPIO11, 500ms villogás aktív riasztásnál)

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

constexpr float BOILER_OVERHEAT_TEMP        = 85.0f; // Alap túlmelegedési küszöb (°C)
constexpr float ALARM_OVERHEAT_TEMP_DEFAULT = 85.0f;
constexpr float ALARM_OVERHEAT_TEMP_MIN     = 70.0f;
constexpr float ALARM_OVERHEAT_TEMP_MAX     = 90.0f;
constexpr bool  ALARM_ENABLED_DEFAULT       = true;

constexpr uint32_t PUMP_MANUAL_OFF_TIMEOUT_MS = 600000UL; // 10 perc -> vissza auto-ra
constexpr uint32_t ALARM_BLINK_INTERVAL_MS    = 500UL;    // 500ms BE / 500ms KI villogás

// --- Időzítések (ms) ---
constexpr uint32_t NRF_PING_INTERVAL_MS   = 2000UL;  // 2 mp-enként ping
constexpr uint32_t NRF_TIMEOUT_MS         = 10000UL; // 10 mp -> nrf24_ok = false
constexpr uint32_t TELEMETRY_INTERVAL_MS  = 30000UL; // 30 mp-enként telemetry
constexpr uint32_t LM35_READ_INTERVAL_MS  = 1000UL;  // 1 mp-enként szobahő mérés
constexpr uint32_t WIFI_RECONNECT_MS      = 10000UL; // 10 mp nem-blokkoló WiFi retry
constexpr uint32_t MQTT_RECONNECT_MS      = 5000UL;  // 5 mp nem-blokkoló MQTT retry
constexpr uint32_t BTN_DEBOUNCE_MS        = 60UL;
