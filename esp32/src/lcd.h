#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <string.h>
#include "config.h"

// ============================================================================
// 2x20 I2C LCD KEZELŐ (0x27, SDA=GPIO8, SCL=GPIO9) - 3 ÁLLAPOT IKON + TREND
// ============================================================================
// - LiquidCrystal_I2C inicializálás kizárólag Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL)
//   használatával (Wire.setPins TILOS!).
// - Saját 5x8 karakterek:
//     0 = SZIVATTYÚ KI, 1 = SZIVATTYÚ BE
//     2 = RÁDIÓ OK,     3 = RÁDIÓ HIBA
//     4 = WIFI OK,      5 = WIFI HIBA
//     6 = TREND FEL,    7 = TREND LE (FLAT = beépített '=' 0x3D)
// - Felső sor (0..19): 0..16 szöveg (szóközökkel 17 kar-ra töltve) + 17..19 ikonok.
// - Alsó sor  (0..19): 0..18 szöveg (szóközökkel 19 kar-ra töltve) + 19 trend nyíl.
// - Beépített millis()-alapú, nem-blokkoló 100ms-es frissítési ciklus és
//   byte-szintű diff cache (memcmp + _lcd.write(), mert a 0-s ikon byte 0x00!).

class HeatingLcd {
public:
    static constexpr uint8_t LCD_TEXT_COLS       = 17;
    static constexpr uint8_t LCD_LINE2_TEXT_COLS = 19;

    static constexpr uint8_t ICON_PUMP_OFF   = 0;
    static constexpr uint8_t ICON_PUMP_ON    = 1;
    static constexpr uint8_t ICON_RADIO_OK   = 2;
    static constexpr uint8_t ICON_RADIO_ERR  = 3;
    static constexpr uint8_t ICON_WIFI_OK    = 4;
    static constexpr uint8_t ICON_WIFI_ERR   = 5;
    static constexpr uint8_t ICON_TREND_UP   = 6;
    static constexpr uint8_t ICON_TREND_DOWN = 7;

    explicit HeatingLcd(uint32_t refreshIntervalMs = LCD_MENU_REFRESH_MS)
        : _lcd(LCD_I2C_ADDR, LCD_COLS, LCD_ROWS),
          _refreshIntervalMs(refreshIntervalMs) {
        memset(_lastLine1, 0xFF, sizeof(_lastLine1));
        memset(_lastLine2, 0xFF, sizeof(_lastLine2));
        memset(_pendingText1, ' ', LCD_TEXT_COLS);
        _pendingText1[LCD_TEXT_COLS] = '\0';
        memset(_pendingText2, ' ', LCD_LINE2_TEXT_COLS);
        _pendingText2[LCD_LINE2_TEXT_COLS] = '\0';
        memset(_pendingLine1, ' ', LCD_COLS);
        memset(_pendingLine2, ' ', LCD_COLS);
    }

    void begin() {
        Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
        _lcd.init();
        _lcd.backlight();

        uint8_t charPumpOff[8] = {
            0b01110,
            0b10001,
            0b10001,
            0b10001,
            0b10001,
            0b10001,
            0b10001,
            0b01110
        };
        uint8_t charPumpOn[8] = {
            0b01110,
            0b11111,
            0b11111,
            0b11111,
            0b11111,
            0b11111,
            0b11111,
            0b01110
        };
        uint8_t charRadioOk[8] = {
            0b00100,
            0b01010,
            0b10101,
            0b00100,
            0b00100,
            0b00100,
            0b00100,
            0b00000
        };
        uint8_t charRadioErr[8] = {
            0b00100,
            0b00100,
            0b00100,
            0b01010,
            0b00100,
            0b01010,
            0b10001,
            0b00000
        };
        uint8_t charWifiOk[8] = {
            0b00000,
            0b01110,
            0b10001,
            0b01010,
            0b00100,
            0b00100,
            0b00100,
            0b00000
        };
        uint8_t charWifiErr[8] = {
            0b10001,
            0b01010,
            0b01110,
            0b10001,
            0b01010,
            0b10101,
            0b10001,
            0b00000
        };
        uint8_t charTrendUp[8] = {
            0b00100,
            0b01110,
            0b10101,
            0b00100,
            0b00100,
            0b00100,
            0b00100,
            0b00000
        };
        uint8_t charTrendDown[8] = {
            0b00100,
            0b00100,
            0b00100,
            0b00100,
            0b10101,
            0b01110,
            0b00100,
            0b00000
        };

        _lcd.createChar(ICON_PUMP_OFF,   charPumpOff);
        _lcd.createChar(ICON_PUMP_ON,    charPumpOn);
        _lcd.createChar(ICON_RADIO_OK,   charRadioOk);
        _lcd.createChar(ICON_RADIO_ERR,  charRadioErr);
        _lcd.createChar(ICON_WIFI_OK,    charWifiOk);
        _lcd.createChar(ICON_WIFI_ERR,   charWifiErr);
        _lcd.createChar(ICON_TREND_UP,   charTrendUp);
        _lcd.createChar(ICON_TREND_DOWN, charTrendDown);

        _lcd.clear();

        memset(_lastLine1, 0xFF, sizeof(_lastLine1));
        memset(_lastLine2, 0xFF, sizeof(_lastLine2));
        memset(_pendingText1, ' ', LCD_TEXT_COLS);
        _pendingText1[LCD_TEXT_COLS] = '\0';
        memset(_pendingText2, ' ', LCD_LINE2_TEXT_COLS);
        _pendingText2[LCD_LINE2_TEXT_COLS] = '\0';
        composeTopLineWithIcons();
        composeBottomLineWithTrend();
        _hasPending    = false;
        _lastRefreshMs = millis();
    }

    // Eltárolja a 3 állapot ikont (nem ír azonnal LCD-re, a refresh() végzi)
    void setStatusIcons(bool pumpOn, bool nrfOk, bool wifiMqttOk) {
        if (_pumpOn == pumpOn && _nrfOk == nrfOk && _wifiMqttOk == wifiMqttOk) {
            return;
        }
        _pumpOn     = pumpOn;
        _nrfOk      = nrfOk;
        _wifiMqttOk = wifiMqttOk;
        composeTopLineWithIcons();
        if (memcmp(_pendingLine1, _lastLine1, LCD_COLS) != 0) {
            _hasPending = true;
        }
    }

    // Eltárolja az előremenő trend nyilat (nem ír azonnal LCD-re, a refresh() végzi)
    void setTrend(uint8_t trend) {
        if (_trend == trend) {
            return;
        }
        _trend = trend;
        composeBottomLineWithTrend();
        if (memcmp(_pendingLine2, _lastLine2, LCD_COLS) != 0) {
            _hasPending = true;
        }
    }

    // Ütemezi a két sor tartalmát; 1. sor 0..16 + ikonok (17..19), 2. sor 0..18 + trend (19)
    void setLines(const char* rawLine1, const char* rawLine2) {
        formatFixed(rawLine1 ? rawLine1 : "", _pendingText1, LCD_TEXT_COLS);
        composeTopLineWithIcons();
        formatFixed(rawLine2 ? rawLine2 : "", _pendingText2, LCD_LINE2_TEXT_COLS);
        composeBottomLineWithTrend();

        if (memcmp(_pendingLine1, _lastLine1, LCD_COLS) != 0 ||
            memcmp(_pendingLine2, _lastLine2, LCD_COLS) != 0) {
            _hasPending = true;
        }
    }

    // Ütemezi a sorokat és ellenőrzi a 100ms-es frissítési ciklust
    void printLines(const char* rawLine1, const char* rawLine2) {
        setLines(rawLine1, rawLine2);
        refresh(millis());
    }

    // Azonnali hardveres kiírás (pl. GYARI BEALLITAS! bannerhez, ikonokkal és trenddel)
    void printLinesImmediate(const char* rawLine1, const char* rawLine2) {
        setLines(rawLine1, rawLine2);
        flushToHardware(millis());
    }

    // Nem-blokkoló millis()-alapú 100ms-es frissítési ciklus (loop-ból hívandó)
    bool refresh(uint32_t now) {
        if (!_hasPending) {
            return false;
        }
        // OVERFLOW-SAFE: uint32_t kivonás, 49 napos millis túlcsorduláskor is helyes.
        if (static_cast<uint32_t>(now - _lastRefreshMs) < _refreshIntervalMs) {
            return false;
        }
        return flushToHardware(now);
    }

    void clear() {
        _lcd.clear();
        memset(_lastLine1, 0xFF, sizeof(_lastLine1));
        memset(_lastLine2, 0xFF, sizeof(_lastLine2));
        memset(_pendingText1, ' ', LCD_TEXT_COLS);
        _pendingText1[LCD_TEXT_COLS] = '\0';
        memset(_pendingText2, ' ', LCD_LINE2_TEXT_COLS);
        _pendingText2[LCD_LINE2_TEXT_COLS] = '\0';
        composeTopLineWithIcons();
        composeBottomLineWithTrend();
        _hasPending    = false;
        _lastRefreshMs = millis();
    }

    void setBacklight(bool enabled) {
        if (enabled) {
            _lcd.backlight();
        } else {
            _lcd.noBacklight();
        }
    }

    void invalidateCache() {
        memset(_lastLine1, 0xFF, sizeof(_lastLine1));
        memset(_lastLine2, 0xFF, sizeof(_lastLine2));
        _hasPending = true;
    }

private:
    LiquidCrystal_I2C _lcd;
    uint32_t _refreshIntervalMs;
    uint32_t _lastRefreshMs = 0;
    bool     _hasPending    = false;

    bool    _pumpOn     = false;
    bool    _nrfOk      = false;
    bool    _wifiMqttOk = false;
    uint8_t _trend      = TREND_FLAT;

    uint8_t _lastLine1[LCD_COLS];
    uint8_t _lastLine2[LCD_COLS];
    char    _pendingText1[LCD_TEXT_COLS + 1];
    char    _pendingText2[LCD_LINE2_TEXT_COLS + 1];
    uint8_t _pendingLine1[LCD_COLS];
    uint8_t _pendingLine2[LCD_COLS];

    void composeTopLineWithIcons() {
        memcpy(_pendingLine1, _pendingText1, LCD_TEXT_COLS);
        _pendingLine1[17] = _pumpOn ? ICON_PUMP_ON : ICON_PUMP_OFF;
        _pendingLine1[18] = _nrfOk ? ICON_RADIO_OK : ICON_RADIO_ERR;
        _pendingLine1[19] = _wifiMqttOk ? ICON_WIFI_OK : ICON_WIFI_ERR;
    }

    uint8_t trendChar() const {
        if (_trend == TREND_UP)   return ICON_TREND_UP;
        if (_trend == TREND_DOWN) return ICON_TREND_DOWN;
        return static_cast<uint8_t>('='); // 0x3D beépített karakter, nem foglal slotot
    }

    void composeBottomLineWithTrend() {
        memcpy(_pendingLine2, _pendingText2, LCD_LINE2_TEXT_COLS);
        _pendingLine2[19] = trendChar();
    }

    bool flushToHardware(uint32_t now) {
        bool wrote = false;

        if (memcmp(_pendingLine1, _lastLine1, LCD_COLS) != 0) {
            _lcd.setCursor(0, 0);
            for (uint8_t i = 0; i < LCD_COLS; i++) {
                _lcd.write(_pendingLine1[i]);
            }
            memcpy(_lastLine1, _pendingLine1, LCD_COLS);
            wrote = true;
        }

        if (memcmp(_pendingLine2, _lastLine2, LCD_COLS) != 0) {
            _lcd.setCursor(0, 1);
            for (uint8_t i = 0; i < LCD_COLS; i++) {
                _lcd.write(_pendingLine2[i]);
            }
            memcpy(_lastLine2, _pendingLine2, LCD_COLS);
            wrote = true;
        }

        _hasPending    = false;
        _lastRefreshMs = now;
        return wrote;
    }

    static void formatFixed(const char* src, char* dst, uint8_t cols) {
        size_t len = strlen(src);
        for (uint8_t i = 0; i < cols; i++) {
            dst[i] = (i < len) ? src[i] : ' ';
        }
        dst[cols] = '\0';
    }
};
