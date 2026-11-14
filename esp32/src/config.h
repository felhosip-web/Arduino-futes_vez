#pragma once

#include <stdint.h>

// ============================================================================
// ESP32-C3 KONFIGURÁCIÓ: WIFI, MQTT (HiveMQ Cloud TLS), PIN-EK, LCD, KALIBRÁCIÓ
// ============================================================================

// --- WiFi beállítások ---
constexpr const char* WIFI_SSID     = "YOUR_WIFI_SSID";
constexpr const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// --- HiveMQ Cloud MQTT (TLS 8883) ---
constexpr const char* MQTT_BROKER   = "your-cluster-id.s1.eu.hivemq.cloud";
constexpr uint16_t    MQTT_PORT     = 8883;
constexpr const char* MQTT_USER     = "your_mqtt_username";
constexpr const char* MQTT_PASSWORD = "your_mqtt_password";
constexpr const char* MQTT_CLIENT_ID = "esp32c3-futes-gateway";

// --- MQTT Topic-ok ---
constexpr const char* TOPIC_CMD       = "futes/haz-futes/cmd";
constexpr const char* TOPIC_STATUS    = "futes/haz-futes/status";    // retained
constexpr const char* TOPIC_TELEMETRY = "futes/haz-futes/telemetry"; // 30 mp

// --- PIN-ek (ESP32-C3) ---
// LM35 szobai hőmérő (ADC1_CH2 - WiFi mellett is megbízhatóan működik)
constexpr uint8_t PIN_LM35      = 2;

// Rotary Encoder
constexpr uint8_t PIN_ENC_CLK   = 5;
constexpr uint8_t PIN_ENC_DT    = 6;
constexpr uint8_t PIN_ENC_SW    = 7;

// 2x20 I2C LCD
constexpr uint8_t PIN_I2C_SDA   = 8;
constexpr uint8_t PIN_I2C_SCL   = 9;
constexpr uint8_t LCD_I2C_ADDR  = 0x27;
constexpr uint8_t LCD_COLS      = 20;
constexpr uint8_t LCD_ROWS      = 2;

// NRF24L01+ (FSPI GPIO mátrixon keresztül)
constexpr uint8_t PIN_NRF_CE    = 0;
constexpr uint8_t PIN_NRF_CSN   = 1;
constexpr uint8_t PIN_NRF_MISO  = 3;
constexpr uint8_t PIN_NRF_SCK   = 4;
constexpr uint8_t PIN_NRF_MOSI  = 10;

// --- Kalibráció és szabályozási paraméterek ---
// LM35 map: 0-3300 mV = 0-100 °C + LM35_OFFSET
constexpr float LM35_OFFSET         = 0.0f;
constexpr uint8_t LM35_SAMPLE_COUNT = 10;

constexpr float TARGET_TEMP_DEFAULT = 21.5f;
constexpr float TARGET_TEMP_MIN     = 16.0f;
constexpr float TARGET_TEMP_MAX     = 26.0f;
constexpr float TARGET_TEMP_STEP    = 0.5f;

// Hiszterézis (BE ha currentTemp < target - 0.3, KI ha currentTemp > target + 0.3)
constexpr float HYST_LOW_OFFSET     = 0.3f;
constexpr float HYST_HIGH_OFFSET    = 0.3f;

// --- Időzítések (ms) ---
constexpr uint32_t NRF_PING_INTERVAL_MS      = 2000UL;   // 2 mp-enként ping a Nano felé
constexpr uint32_t NRF_TIMEOUT_MS            = 10000UL;  // 10 mp után nrf24_ok = false
constexpr uint32_t TELEMETRY_INTERVAL_MS     = 30000UL;  // 30 mp-enként MQTT telemetry
constexpr uint32_t LM35_READ_INTERVAL_MS     = 1000UL;   // 1 mp-enként szobai hőmérséklet mérés
constexpr uint32_t WIFI_RECONNECT_MS         = 10000UL;  // 10 mp-enként nem-blokkoló WiFi retry
constexpr uint32_t MQTT_RECONNECT_MS         = 5000UL;   // 5 mp-enként nem-blokkoló MQTT retry
constexpr uint32_t BTN_DEBOUNCE_MS           = 60UL;     // Nyomógomb debounce
