#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <math.h>
#include "config.h"

// ============================================================================
// 2x20 I2C LCD KEZELŐ (0x27)
// ============================================================================
// 1. sor (20 kar): "Szoba 20.8C Cel21.5C"
// 2. sor (20 kar): "Kaz 52C Futes: BE  A"  vagy hiba esetén: "NRF HIBA!"

class HeatingLcd {
public:
    HeatingLcd() : _lcd(LCD_I2C_ADDR, LCD_COLS, LCD_ROWS) {}

    void begin() {
        Wire.setPins(PIN_I2C_SDA, PIN_I2C_SCL);
        Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
        _lcd.init();
        _lcd.backlight();
        _lcd.clear();
        _lastLine1[0] = '\0';
        _lastLine2[0] = '\0';
    }

    void update(float roomTemp, float targetTemp, float boilerTemp,
                bool heatingOn, uint8_t mode, bool nrfOk) {
        char line1[LCD_COLS + 1];
        char line2[LCD_COLS + 1];

        // --- 1. sor formázása (pontosan 20 karakter) ---
        char roomStr[8];
        if (isnan(roomTemp)) {
            snprintf(roomStr, sizeof(roomStr), "--.-");
        } else {
            snprintf(roomStr, sizeof(roomStr), "%4.1f", roomTemp);
        }

        char rawLine1[32];
        snprintf(rawLine1, sizeof(rawLine1), "Szoba %sC Cel%4.1fC", roomStr, targetTemp);
        formatFixed20(rawLine1, line1);

        // --- 2. sor formázása (pontosan 20 karakter) ---
        char rawLine2[32];
        if (!nrfOk) {
            snprintf(rawLine2, sizeof(rawLine2), "NRF HIBA!");
        } else {
            char kazStr[6];
            if (isnan(boilerTemp)) {
                snprintf(kazStr, sizeof(kazStr), "--");
            } else {
                snprintf(kazStr, sizeof(kazStr), "%d", static_cast<int>(lroundf(boilerTemp)));
            }

            char modeChar = 'A';
            if (mode == 1) modeChar = 'M';
            else if (mode == 2) modeChar = 'O';

            snprintf(rawLine2, sizeof(rawLine2), "Kaz %sC Futes:%s %c",
                     kazStr,
                     heatingOn ? "BE" : "KI",
                     modeChar);
        }
        formatFixed20(rawLine2, line2);

        // Csak változás esetén írjuk felül a sort (villódzásmentes)
        if (strncmp(line1, _lastLine1, LCD_COLS) != 0) {
            _lcd.setCursor(0, 0);
            _lcd.print(line1);
            strncpy(_lastLine1, line1, sizeof(_lastLine1));
        }
        if (strncmp(line2, _lastLine2, LCD_COLS) != 0) {
            _lcd.setCursor(0, 1);
            _lcd.print(line2);
            strncpy(_lastLine2, line2, sizeof(_lastLine2));
        }
    }

private:
    LiquidCrystal_I2C _lcd;
    char _lastLine1[LCD_COLS + 1];
    char _lastLine2[LCD_COLS + 1];

    static void formatFixed20(const char* src, char* dst) {
        size_t len = strlen(src);
        for (uint8_t i = 0; i < LCD_COLS; i++) {
            dst[i] = (i < len) ? src[i] : ' ';
        }
        dst[LCD_COLS] = '\0';
    }
};
