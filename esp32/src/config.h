#pragma once

#include <stdint.h>
#include <stddef.h>
#include <driver/spi_master.h>

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
constexpr const char* TOPIC_ACK       = "futes/haz-futes/ack";       // End-to-end CMD_ACK
constexpr const char* TOPIC_ACK_ALT   = "heating/haz-futes/ack";     // Kompatibilitási ACK topic

// --- NTP időszinkron ---
constexpr const char* NTP_SERVER_1           = "pool.ntp.org";
constexpr const char* NTP_SERVER_2           = "time.nist.gov";
constexpr const char* NTP_TZ_INFO            = "CET-1CEST,M3.5.0,M10.5.0/3";
constexpr uint32_t    NTP_TIMEOUT_MS         = 5000UL;     // Max 5 mp NTP timeout
constexpr uint32_t    NTP_RETRY_INTERVAL_MS  = 60000UL;    // 60 mp NTP újrapróbálkozás
constexpr uint32_t    SYNC_RETRY_INTERVAL_MS = 3600000UL;  // 1 óra múlva újrapróba sikertelen Nano szinkronnál
constexpr uint32_t    NANO_SYNC_STALE_SEC    = 46800UL;    // 13 óra (2x12h + 1h tűrés)

// --- PIN-ek (ESP32-C3, ütközésmentes kiosztás — BEKÖTÉS VÁLTOZATLAN) ---
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

// --- NRF24 SPI + GDMA konfiguráció (ESP32-C3 SPI2_HOST) ---
//  * SPI0/SPI1 kizárólag a Flash memóriáé — csak a szabad SPI2_HOST használható.
//  * A GDMA nem látja a stack-et / flash-t -> DMA pufferek MALLOC_CAP_DMA memóriában.
//  * NRF24L01+ maximális stabil SPI órajel: 10 MHz (10000000 Hz) — NE lépd túl!
//  * Queue méret = 3: a 3 retry (NRF_MAX_ATTEMPTS) sorban állás nélkül elfér.
//  * Az I2C 2x20 LCD és a WiFi DMA érintetlen marad.
constexpr spi_host_device_t NRF_SPI_HOST        = SPI2_HOST;
constexpr spi_dma_chan_t    NRF_SPI_DMA_CHANNEL = SPI_DMA_CH_AUTO;
constexpr uint32_t          NRF_SPI_CLOCK_HZ    = 10000000UL; // 10 MHz
constexpr uint8_t           NRF_SPI_QUEUE_SIZE  = 3;          // Max 3 függő DMA tranzakció
constexpr size_t            NRF_DMA_BUFFER_BYTES = 32;        // V5 fix 32 byte-os NrfPacket
constexpr int               NRF_SPI_MAX_XFER_SZ = 4096;       // Közös busz max átviteli méret (pl. jövőbeli SD/TFT)

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
constexpr uint32_t CMD_ACK_TIMEOUT_MS         = 3000UL; // Max 3000ms end-to-end CMD_ACK várakozás
constexpr uint32_t RELAY_FAULT_DELAY_MS       = 500UL;  // Max 500ms eltérés kért vs D7 visszacsatolás között
constexpr uint32_t CMD_RELAY_SETTLE_POLL_MS   = 75UL;   // Relé D7 beállási újrakérdezés (50ms debounce után)
constexpr uint32_t SENSOR_SCAN_CONV_WAIT_MS   = 850UL;  // Nano 800ms DS18B20 scan konverzió kivárása
constexpr uint32_t SENSOR_TEACH_HEAT_WINDOW_MS = 10000UL; // 10 mp melegítési ablak LCD tanításkor
constexpr uint32_t SENSOR_TEACH_POLL_INTERVAL_MS = 1400UL; // Tanítás közbeni keresési ciklus (nem-blokkoló)
constexpr float    SENSOR_TEACH_DELTA_C       = 2.0f;   // > 2.0 °C emelkedés = azonosított szenzor

// --- TLS biztonsági beállítások (kizárólag CA tanúsítvány-ellenőrzéssel) ---
constexpr bool     TLS_REQUIRE_CA_CERT    = true;
constexpr uint32_t TLS_CERT_RETRY_MS      = 30000UL; // Sikertelen TLS handshake után 30 mp múlva újrapróba

constexpr uint32_t TELEMETRY_INTERVAL_MS  = 30000UL; // 30 mp-enként telemetry
constexpr uint32_t LM35_READ_INTERVAL_MS  = 1000UL;  // 1 mp-enként szobahő mérés
constexpr uint32_t WIFI_RECONNECT_MS      = 10000UL; // 10 mp nem-blokkoló WiFi retry
constexpr uint32_t WIFI_PORTAL_TIMEOUT_MS = 30000UL; // 30 mp sikertelen WiFi után beállító portál
constexpr uint32_t MQTT_RECONNECT_MS      = 5000UL;  // 5 mp nem-blokkoló MQTT retry
constexpr uint32_t BTN_DEBOUNCE_MS        = 50UL;

// Állítsd 1-re (vagy adj meg -DDEBUG build flag-et), ha a >50 ms loop() ciklusidő naplózást kéred sorosra
#ifndef DEBUG_MODE
#define DEBUG_MODE 0
#endif

// --- Loop Monitor (cél < 20 ms) + Hardveres Watchdog (5 mp) ---
constexpr uint32_t ESP_WDT_TIMEOUT_SEC       = 5UL;       // 5 mp hardware task watchdog
constexpr uint32_t ESP_LOOP_WARN_US          = 100000UL;  // >100 ms -> WARN + overrun
constexpr uint32_t ESP_LOOP_FAULT_STREAK_US  = 500000UL;  // 5 egymást követő >500 ms -> loopFault
constexpr uint32_t ESP_LOOP_FAULT_SINGLE_US  = 2000000UL; // 1 loop >2000 ms -> azonnali loopFault
constexpr uint32_t ESP_LOOP_RECOVER_US       = 20000UL;   // 10 egymást követő <20 ms -> helyreáll
constexpr uint32_t ESP_LOOP_WARN_COOLDOWN_MS = 10000UL;   // 10 mp cooldown log spam ellen
constexpr uint8_t  ESP_LOOP_FAULT_STREAK_CNT = 5;
constexpr uint8_t  ESP_LOOP_RECOVER_CNT      = 10;

// --- Előremenő hőmérséklet trend és napi statisztika ---
constexpr uint32_t TREND_SAMPLE_INTERVAL_MS = 60000UL; // 1 perces mintavétel
constexpr uint8_t  TREND_BUFFER_SIZE        = 20;      // 20 perc (10+10 perc)
constexpr float    TREND_THRESHOLD_C        = 0.5f;    // ±0.5 °C küszöb

enum TrendDirection : uint8_t {
    TREND_FLAT = 0,
    TREND_UP   = 1,
    TREND_DOWN = 2
};
