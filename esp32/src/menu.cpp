#include "menu.h"
#include <math.h>
#include <stdio.h>

// ============================================================================
// BELSŐ AZONOSÍTÓ KONSTANSOK A MENÜPONTOKHOZ
// ============================================================================
namespace {
enum FokSrcId : uint8_t {
    SENSOR_SRC_NANO = 0,
    SENSOR_SRC_ROOM = 1
};

enum StatusFieldId : uint8_t {
    STAT_SZIVATTYU  = 0,
    STAT_FUTES      = 1,
    STAT_PUMP_STATE = 2,
    STAT_RIASZTAS   = 3,
    STAT_RADIO      = 4
};

enum EditFieldId : uint8_t {
    EDIT_TARGET_TEMP    = 0,
    EDIT_MODE           = 1,
    EDIT_PUMP_MODE      = 2,
    EDIT_PUMP_START     = 3,
    EDIT_PUMP_STOP      = 4,
    EDIT_PUMP_SENSOR    = 5,
    EDIT_OVERHEAT_TEMP  = 6,
    EDIT_ALARM_ENABLED  = 7,
    EDIT_HEARTBEAT      = 8,
    EDIT_MANUAL_HEATING = 9
};

constexpr uint8_t HEARTBEAT_OPTIONS[6] = { 5, 10, 15, 20, 25, 30 };

uint8_t heartbeatSecToIndex(uint8_t sec) {
    for (uint8_t i = 0; i < 6; i++) {
        if (HEARTBEAT_OPTIONS[i] == sec) return i;
    }
    return 0;
}
} // namespace

// ============================================================================
// SEGÉDFÜGGVÉNYEK
// ============================================================================
bool MenuController::isTempValid(float t) {
    return !isnan(t) && t > -120.0f && t <= 125.0f;
}

float MenuController::roundHalf(float v) {
    return roundf(v * 2.0f) / 2.0f;
}

void MenuController::begin(HeatingLcd* lcd, SystemState* state, const MenuCallbacks& callbacks) {
    _lcd       = lcd;
    _state     = state;
    _callbacks = callbacks;

    _menuState       = MENU_HOME;
    _cursor          = 0;
    _mainCursor      = 0;
    _lastActivityMs  = millis();
    _lastMenuDrawMs  = 0;
    _lastMainDrawMs  = 0;
    _lastEditBlinkMs = 0;
    _bannerUntilMs   = 0;
    _editBlinkOn     = true;
    _menuDirty       = true;
    _mainDirty       = true;

    buildItemsForState(MENU_MAIN);
    drawMainScreen(false);
}

void MenuController::requestRedraw() {
    _menuDirty = true;
    _mainDirty = true;
}

bool MenuController::isOnFactoryResetItem() const {
    if (_menuState != MENU_BEALLITAS || _cursor >= _itemCount) {
        return false;
    }
    return _items[_cursor].type == ITEM_ACTION_FACTORY_RESET;
}

// ============================================================================
// MENÜFA FELÉPÍTÉSE ADOTT SZINTHEZ (PONTOSAN A SPECIFIKÁCIÓ SZERINT)
// ============================================================================
void MenuController::buildItemsForState(MenuState targetState) {
    _itemCount = 0;
    if (!_state) return;

    switch (targetState) {
        case MENU_MAIN:
            _items[_itemCount++] = { "FOKOK",     ITEM_SUBMENU, nullptr, 0, 0, 0, MENU_FOKOK };
            _items[_itemCount++] = { "STATUS",    ITEM_SUBMENU, nullptr, 0, 0, 0, MENU_STATUS };
            _items[_itemCount++] = { "BEALLITAS", ITEM_SUBMENU, nullptr, 0, 0, 0, MENU_BEALLITAS };
            _items[_itemCount++] = { "KIMENETEK", ITEM_SUBMENU, nullptr, 0, 0, 0, MENU_KIMENETEK };
            break;

        case MENU_FOKOK:
            _items[_itemCount++] = { "Eloremeno",  ITEM_READONLY_TEMP, &_state->eloremeno,   0, 0, 0, SENSOR_SRC_NANO };
            _items[_itemCount++] = { "Visszatero", ITEM_READONLY_TEMP, &_state->visszatero,  0, 0, 0, SENSOR_SRC_NANO };
            _items[_itemCount++] = { "Kazan",      ITEM_READONLY_TEMP, &_state->kazan,       0, 0, 0, SENSOR_SRC_NANO };
            _items[_itemCount++] = { "Kulteri",    ITEM_READONLY_TEMP, &_state->kulteri,     0, 0, 0, SENSOR_SRC_NANO };
            _items[_itemCount++] = { "Szoba",      ITEM_READONLY_TEMP, &_state->currentTemp, 0, 0, 0, SENSOR_SRC_ROOM };
            _items[_itemCount++] = { "VISSZA",     ITEM_ACTION_BACK,   nullptr,              0, 0, 0, MENU_MAIN };
            break;

        case MENU_STATUS:
            _items[_itemCount++] = { "Szivattyu",     ITEM_READONLY_STATUS, &_state->pumpOn,     0, 0, 0, STAT_SZIVATTYU };
            _items[_itemCount++] = { "Futes",         ITEM_READONLY_STATUS, &_state->heatingOn,  0, 0, 0, STAT_FUTES };
            _items[_itemCount++] = { "Pumpa Allapot", ITEM_READONLY_STATUS, &_state->pumpState,  0, 0, 0, STAT_PUMP_STATE };
            _items[_itemCount++] = { "Riasztas",      ITEM_READONLY_STATUS, &_state->alarmState, 0, 0, 0, STAT_RIASZTAS };
            _items[_itemCount++] = { "Radio",         ITEM_READONLY_STATUS, &_state->nrf24Ok,    0, 0, 0, STAT_RADIO };
            _items[_itemCount++] = { "VISSZA",        ITEM_ACTION_BACK,     nullptr,             0, 0, 0, MENU_MAIN };
            break;

        case MENU_BEALLITAS:
            _items[_itemCount++] = { "Celhofok",            ITEM_EDIT_FLOAT,           &_state->targetTemp,                 TARGET_TEMP_MIN,        TARGET_TEMP_MAX,        0.5f, EDIT_TARGET_TEMP };
            _items[_itemCount++] = { "Uzmod",               ITEM_EDIT_ENUM,            &_state->mode,                       0.0f,                   2.0f,                   1.0f, EDIT_MODE };
            _items[_itemCount++] = { "Szivattyu Mod",       ITEM_EDIT_ENUM,            &_state->pumpConfig.pumpMode,        0.0f,                   2.0f,                   1.0f, EDIT_PUMP_MODE };
            _items[_itemCount++] = { "Pump Start",          ITEM_EDIT_FLOAT,           &_state->pumpConfig.pumpStartTemp,   PUMP_START_TEMP_MIN,    PUMP_START_TEMP_MAX,    0.5f, EDIT_PUMP_START };
            _items[_itemCount++] = { "Pump Stop",           ITEM_EDIT_FLOAT,           &_state->pumpConfig.pumpStopTemp,    PUMP_STOP_TEMP_MIN,     PUMP_STOP_TEMP_MAX,     0.5f, EDIT_PUMP_STOP };
            _items[_itemCount++] = { "Pump Szenzor",        ITEM_EDIT_ENUM,            &_state->pumpConfig.pumpStartSensor, 0.0f,                   2.0f,                   1.0f, EDIT_PUMP_SENSOR };
            _items[_itemCount++] = { "Riasztas Hofok",      ITEM_EDIT_FLOAT,           &_state->alarmConfig.overheatTemp,   ALARM_OVERHEAT_TEMP_MIN, ALARM_OVERHEAT_TEMP_MAX, 0.5f, EDIT_OVERHEAT_TEMP };
            _items[_itemCount++] = { "Riasztas",            ITEM_EDIT_BOOL,            &_state->alarmConfig.alarmEnabled,   0.0f,                   1.0f,                   1.0f, EDIT_ALARM_ENABLED };
            _items[_itemCount++] = { "Ping",                ITEM_EDIT_ENUM,            &_state->heartbeatIntervalSec,       0.0f,                   5.0f,                   1.0f, EDIT_HEARTBEAT };
            _items[_itemCount++] = { "Manual Futes",        ITEM_EDIT_BOOL,            &_state->manualHeating,              0.0f,                   1.0f,                   1.0f, EDIT_MANUAL_HEATING };
            _items[_itemCount++] = { "Gyari visszaallitas", ITEM_ACTION_FACTORY_RESET, nullptr,                             0.0f,                   0.0f,                   0.0f, 0 };
            _items[_itemCount++] = { "VISSZA",              ITEM_ACTION_BACK,          nullptr,                             0.0f,                   0.0f,                   0.0f, MENU_MAIN };
            break;

        case MENU_KIMENETEK:
            _items[_itemCount++] = { "Szivattyu", ITEM_OUTPUT_BOOL, &_state->outputsMask, 0, 0, 0, OUT_BIT_SZIVATTYU };
            _items[_itemCount++] = { "LED Zold",  ITEM_OUTPUT_BOOL, &_state->outputsMask, 0, 0, 0, OUT_BIT_LED_ZOLD };
            _items[_itemCount++] = { "LED Piros", ITEM_OUTPUT_BOOL, &_state->outputsMask, 0, 0, 0, OUT_BIT_LED_PIROS };
            _items[_itemCount++] = { "Tartalek1", ITEM_OUTPUT_BOOL, &_state->outputsMask, 0, 0, 0, OUT_BIT_TARTALEK1 };
            _items[_itemCount++] = { "Tartalek2", ITEM_OUTPUT_BOOL, &_state->outputsMask, 0, 0, 0, OUT_BIT_TARTALEK2 };
            _items[_itemCount++] = { "Tartalek3", ITEM_OUTPUT_BOOL, &_state->outputsMask, 0, 0, 0, OUT_BIT_TARTALEK3 };
            _items[_itemCount++] = { "Tartalek4", ITEM_OUTPUT_BOOL, &_state->outputsMask, 0, 0, 0, OUT_BIT_TARTALEK4 };
            _items[_itemCount++] = { "Tartalek5", ITEM_OUTPUT_BOOL, &_state->outputsMask, 0, 0, 0, OUT_BIT_TARTALEK5 };
            _items[_itemCount++] = { "VISSZA",    ITEM_ACTION_BACK, nullptr,              0, 0, 0, MENU_MAIN };
            break;

        case MENU_HOME:
        case MENU_EDIT:
        default:
            break;
    }
}

// ============================================================================
// NAVIGÁCIÓ ÉS SZERKESZTÉS KEZELÉS
// ============================================================================
void MenuController::enterSubmenu(MenuState sub, uint8_t initialCursor, uint32_t now) {
    _menuState = sub;
    buildItemsForState(sub);
    _cursor = (_itemCount > 0 && initialCursor < _itemCount) ? initialCursor : 0;
    _lastActivityMs = now;
    _menuDirty = true;
}

void MenuController::enterEditMode(uint8_t itemIndex, uint32_t now) {
    if (!_state || itemIndex >= _itemCount) return;

    const MenuItem& item = _items[itemIndex];
    _editItemIndex = itemIndex;
    _editBlinkOn   = true;
    _lastEditBlinkMs = now;

    switch (item.extraId) {
        case EDIT_TARGET_TEMP:
            _editFloat = roundHalf(_state->targetTemp);
            break;
        case EDIT_MODE:
            _editEnum = _state->mode;
            break;
        case EDIT_PUMP_MODE:
            _editEnum = _state->pumpConfig.pumpMode;
            break;
        case EDIT_PUMP_START:
            _editFloat        = roundHalf(_state->pumpConfig.pumpStartTemp);
            _editPartnerFloat = roundHalf(_state->pumpConfig.pumpStopTemp);
            break;
        case EDIT_PUMP_STOP:
            _editFloat        = roundHalf(_state->pumpConfig.pumpStopTemp);
            _editPartnerFloat = roundHalf(_state->pumpConfig.pumpStartTemp);
            break;
        case EDIT_PUMP_SENSOR:
            _editEnum = _state->pumpConfig.pumpStartSensor;
            break;
        case EDIT_OVERHEAT_TEMP:
            _editFloat = roundHalf(_state->alarmConfig.overheatTemp);
            break;
        case EDIT_ALARM_ENABLED:
            _editBool = (_state->alarmConfig.alarmEnabled != 0);
            break;
        case EDIT_HEARTBEAT:
            _editEnum = heartbeatSecToIndex(_state->heartbeatIntervalSec);
            break;
        case EDIT_MANUAL_HEATING:
            _editBool = _state->manualHeating;
            break;
        default:
            break;
    }

    _menuState      = MENU_EDIT;
    _lastActivityMs = now;
    _menuDirty      = true;
}

void MenuController::commitEdit(uint32_t now) {
    if (!_state || _editItemIndex >= _itemCount) {
        _menuState = MENU_BEALLITAS;
        _menuDirty = true;
        return;
    }

    const MenuItem& item = _items[_editItemIndex];
    switch (item.extraId) {
        case EDIT_TARGET_TEMP:
            _state->targetTemp = _editFloat;
            break;

        case EDIT_MODE:
            _state->mode = _editEnum % 3;
            break;

        case EDIT_PUMP_MODE: {
            uint8_t nextMode = _editEnum % 3;
            if (nextMode == PUMP_MODE_MANUAL_OFF &&
                _state->pumpConfig.pumpMode != PUMP_MODE_MANUAL_OFF) {
                _state->manualOffStartMs = now;
            }
            _state->pumpConfig.pumpMode = nextMode;
            break;
        }

        case EDIT_PUMP_START:
            _state->pumpConfig.pumpStartTemp = _editFloat;
            _state->pumpConfig.pumpStopTemp  = _editPartnerFloat;
            break;

        case EDIT_PUMP_STOP:
            _state->pumpConfig.pumpStopTemp = _editFloat;
            break;

        case EDIT_PUMP_SENSOR:
            _state->pumpConfig.pumpStartSensor = _editEnum % 3;
            break;

        case EDIT_OVERHEAT_TEMP:
            _state->alarmConfig.overheatTemp = _editFloat;
            break;

        case EDIT_ALARM_ENABLED:
            _state->alarmConfig.alarmEnabled = _editBool ? 1 : 0;
            break;

        case EDIT_HEARTBEAT:
            _state->heartbeatIntervalSec = HEARTBEAT_OPTIONS[_editEnum % 6];
            break;

        case EDIT_MANUAL_HEATING:
            _state->manualHeating = _editBool;
            break;

        default:
            break;
    }

    _menuState      = MENU_BEALLITAS;
    _lastActivityMs = now;
    _menuDirty      = true;

    if (_callbacks.onSettingCommitted) {
        _callbacks.onSettingCommitted();
    }
}

void MenuController::toggleOutput(uint8_t itemIndex, uint32_t now) {
    if (!_state || itemIndex >= _itemCount) return;

    const MenuItem& item = _items[itemIndex];
    uint8_t bit = item.extraId;

    if (bit == OUT_BIT_SZIVATTYU) {
        // AUTO módban zárolva, csak manual_on / manual_off módban kapcsolható
        if (_state->pumpConfig.pumpMode == PUMP_MODE_AUTO) {
            return;
        }
        if (_state->pumpOn) {
            _state->pumpConfig.pumpMode = PUMP_MODE_MANUAL_OFF;
            _state->manualOffStartMs    = now;
        } else {
            _state->pumpConfig.pumpMode = PUMP_MODE_MANUAL_ON;
        }
    } else if (bit == OUT_BIT_LED_PIROS) {
        // Ha riasztás aktív, a riasztás vezérli a piros LED-et
        if (_state->alarmConfig.alarmEnabled != 0 && _state->alarmState.active != 0) {
            return;
        }
        _state->userLedPiros = !_state->userLedPiros;
        setBit(_state->outputsMask, OUT_BIT_LED_PIROS, _state->userLedPiros);
    } else {
        bool current = getBit(_state->outputsMask, bit);
        setBit(_state->outputsMask, bit, !current);
    }

    _lastActivityMs = now;
    _menuDirty      = true;

    if (_callbacks.onSettingCommitted) {
        _callbacks.onSettingCommitted();
    }
}

void MenuController::navigateBack(uint32_t now) {
    _lastActivityMs = now;

    switch (_menuState) {
        case MENU_EDIT:
            // Félbehagyott szerkesztés eldobása -> vissza BEALLITAS listába
            _menuState = MENU_BEALLITAS;
            _menuDirty = true;
            break;

        case MENU_FOKOK:
        case MENU_STATUS:
        case MENU_BEALLITAS:
        case MENU_KIMENETEK:
            enterSubmenu(MENU_MAIN, _mainCursor, now);
            break;

        case MENU_MAIN:
        case MENU_HOME:
        default:
            _menuState = MENU_HOME;
            _mainDirty = true;
            break;
    }
}

// ============================================================================
// ENCODER ÉS GOMB BEMENETEK KEZELÉSE (NEM-BLOKKOLÓ)
// ============================================================================
void MenuController::handleInput(int8_t encSteps,
                                 bool shortPress,
                                 bool longPress,
                                 bool factoryPress,
                                 uint32_t now) {
    if (!_state) return;

    // Gyári visszaállítás banner alatt ne reagáljon navigációra
    if (_bannerUntilMs != 0) {
        return;
    }

    int8_t steps = encSteps;
#if ENCODER_REVERSE
    steps = static_cast<int8_t>(-steps);
#endif

    // 1. Gyári visszaállítás (3 mp nyomás a Gyari visszaallitas ponton)
    if (factoryPress && isOnFactoryResetItem()) {
        if (_callbacks.onFactoryReset) {
            _callbacks.onFactoryReset();
        }
        _bannerUntilMs = now + FACTORY_BANNER_MS;
        _menuState     = MENU_HOME;
        _cursor        = 0;
        _mainCursor    = 0;
        _menuDirty     = true;
        _mainDirty     = true;
        if (_lcd) {
            _lcd->printLinesImmediate("GYARI BEALLITAS!", "Alaphelyzet kesz");
        }
        return;
    }

    // 2. Hosszú nyomás (>1000ms) -> vissza egy szintet / kilépés (szerkesztés eldobva)
    if (longPress) {
        navigateBack(now);
        return;
    }

    // 3. Főképernyőn vagyunk: rövid nyomás belép a Főmenübe
    if (_menuState == MENU_HOME) {
        if (shortPress) {
            enterSubmenu(MENU_MAIN, 0, now);
        }
        return;
    }

    // 4. Szerkesztés módban (`MENU_EDIT`)
    if (_menuState == MENU_EDIT) {
        if (steps != 0) {
            _lastActivityMs = now;
            const MenuItem& item = _items[_editItemIndex];

            if (item.type == ITEM_EDIT_FLOAT) {
                float nextVal = roundHalf(_editFloat + (static_cast<float>(steps) * item.stepVal));

                if (item.extraId == EDIT_PUMP_START) {
                    if (nextVal < item.minVal) nextVal = item.minVal;
                    if (nextVal > item.maxVal) nextVal = item.maxVal;
                    _editFloat = nextVal;

                    // Pump Stop + 5.0 alatt ne maradjon ütközés: toljuk le automatikusan
                    // (a V4/V5 pumpStopTemp < pumpStartTemp - 5 feltétel miatt max = Start - 5.5,
                    // de min = 20.0; ha nem ütközik, érintetlen marad)
                    float maxAllowedStop = roundHalf(_editFloat - PUMP_MIN_HYSTERESIS_GAP - 0.5f);
                    if (_editPartnerFloat > maxAllowedStop) {
                        _editPartnerFloat = (maxAllowedStop < PUMP_STOP_TEMP_MIN)
                                                ? PUMP_STOP_TEMP_MIN
                                                : maxAllowedStop;
                    }
                } else if (item.extraId == EDIT_PUMP_STOP) {
                    float maxAllowed = roundHalf(_editPartnerFloat - PUMP_MIN_HYSTERESIS_GAP - 0.5f);
                    if (maxAllowed > item.maxVal) maxAllowed = item.maxVal;
                    if (maxAllowed < item.minVal) maxAllowed = item.minVal;
                    if (nextVal < item.minVal) nextVal = item.minVal;
                    if (nextVal > maxAllowed)  nextVal = maxAllowed;
                    _editFloat = nextVal;
                } else {
                    if (nextVal < item.minVal) nextVal = item.minVal;
                    if (nextVal > item.maxVal) nextVal = item.maxVal;
                    _editFloat = nextVal;
                }
            } else if (item.type == ITEM_EDIT_ENUM) {
                uint8_t count = static_cast<uint8_t>(item.maxVal) + 1;
                int16_t raw   = static_cast<int16_t>(_editEnum) + steps;
                while (raw < 0) raw += count;
                _editEnum = static_cast<uint8_t>(raw % count);
            } else if (item.type == ITEM_EDIT_BOOL) {
                if ((steps & 1) != 0) {
                    _editBool = !_editBool;
                }
            }
            _menuDirty = true;
        }

        if (shortPress) {
            commitEdit(now);
        }
        return;
    }

    // 5. Lista navigáció (`MENU_MAIN`, `MENU_FOKOK`, `MENU_STATUS`, `MENU_BEALLITAS`, `MENU_KIMENETEK`)
    if (steps != 0 && _itemCount > 0) {
        _lastActivityMs = now;
        int16_t next = static_cast<int16_t>(_cursor) + steps;
        while (next < 0) next += _itemCount;
        _cursor = static_cast<uint8_t>(next % _itemCount);
        if (_menuState == MENU_MAIN) {
            _mainCursor = _cursor;
        }
        _menuDirty = true;
    }

    if (shortPress && _cursor < _itemCount) {
        _lastActivityMs = now;
        const MenuItem& selected = _items[_cursor];

        switch (selected.type) {
            case ITEM_SUBMENU:
                if (_menuState == MENU_MAIN) {
                    _mainCursor = _cursor;
                }
                enterSubmenu(static_cast<MenuState>(selected.extraId), 0, now);
                break;

            case ITEM_ACTION_BACK:
                enterSubmenu(MENU_MAIN, _mainCursor, now);
                break;

            case ITEM_EDIT_FLOAT:
            case ITEM_EDIT_ENUM:
            case ITEM_EDIT_BOOL:
                enterEditMode(_cursor, now);
                break;

            case ITEM_OUTPUT_BOOL:
                toggleOutput(_cursor, now);
                break;

            case ITEM_READONLY_TEMP:
            case ITEM_READONLY_STATUS:
            case ITEM_ACTION_FACTORY_RESET:
            default:
                // Csak lekérdezés vagy 3mp hosszú nyomás esetén aktív
                break;
        }
    }
}

// ============================================================================
// IDŐZÍTETT FRISSÍTÉS (20mp TIMEOUT + VILLOGÁS + 100ms / 1000ms LCD RAJZOLÁS)
// ============================================================================
void MenuController::update(uint32_t now, bool alarmBlinkPhase) {
    if (!_lcd || !_state) return;

    // 1. "GYARI BEALLITAS!" 2 mp-es nem-blokkoló banner kezelése
    if (_bannerUntilMs != 0) {
        if (static_cast<int32_t>(now - _bannerUntilMs) >= 0) {
            _bannerUntilMs = 0;
            _mainDirty     = true;
            _lastMainDrawMs = 0;
        } else {
            return;
        }
    }

    // 2. 20 mp tétlenség ellenőrzése: vissza a főképernyőre, félbehagyott szerkesztés eldobva
    if (_menuState != MENU_HOME) {
        if (now - _lastActivityMs >= MENU_TIMEOUT_MS) {
            _menuState      = MENU_HOME;
            _mainDirty      = true;
            _lastMainDrawMs = 0;
        }
    }

    // 3. Főképernyő frissítése (1 mp-enként, vagy riasztási villogás fázisváltásakor)
    if (_menuState == MENU_HOME) {
        bool alarmBlinking = (_state->alarmConfig.alarmEnabled != 0) &&
                             (_state->alarmState.active != 0);
        if (alarmBlinking && alarmBlinkPhase != _lastAlarmBlink) {
            _lastAlarmBlink = alarmBlinkPhase;
            _mainDirty      = true;
        }

        if (_mainDirty || (now - _lastMainDrawMs >= LCD_MAIN_REFRESH_MS)) {
            _lastMainDrawMs = now;
            _mainDirty      = false;
            drawMainScreen(alarmBlinkPhase);
        }
        return;
    }

    // 4. Szerkesztés közbeni < > jel 500ms-os villogása
    if (_menuState == MENU_EDIT) {
        if (now - _lastEditBlinkMs >= EDIT_BLINK_INTERVAL_MS) {
            _lastEditBlinkMs = now;
            _editBlinkOn     = !_editBlinkOn;
            _menuDirty       = true;
        }
    }

    // 5. Menü LCD frissítése 100ms-enként, ha változott (vagy lekérdező menüben 1mp-enként)
    if (_menuState == MENU_FOKOK || _menuState == MENU_STATUS) {
        if (now - _lastMainDrawMs >= LCD_MAIN_REFRESH_MS) {
            _lastMainDrawMs = now;
            _menuDirty      = true;
        }
    }

    if (_menuDirty && (now - _lastMenuDrawMs >= LCD_MENU_REFRESH_MS)) {
        _lastMenuDrawMs = now;
        _menuDirty      = false;
        if (_menuState == MENU_EDIT) {
            drawEditValue(_editBlinkOn);
        } else {
            drawMenu();
        }
    }

    _lcd->refresh(now);
}

// ============================================================================
// KIRAJZOLÓ FÜGGVÉNYEK: drawMainScreen(), drawMenu(), drawEditValue()
// ============================================================================
void MenuController::drawMainScreen(bool alarmBlinkPhase) {
    if (!_lcd || !_state) return;

    char line1[32];
    char line2[32];

    // 1. sor: "E:45.2C K: 2.5C" vagy ha nrf24_ok == false: "E: --.- K: --.-"
    if (!_state->nrf24Ok) {
        snprintf(line1, sizeof(line1), "E: --.- K: --.-");
    } else {
        char eStr[10];
        char kStr[10];

        if (isTempValid(_state->eloremeno)) {
            snprintf(eStr, sizeof(eStr), "%4.1fC", _state->eloremeno);
        } else {
            snprintf(eStr, sizeof(eStr), "--.-C");
        }

        if (isTempValid(_state->kulteri)) {
            snprintf(kStr, sizeof(kStr), "%4.1fC", _state->kulteri);
        } else {
            snprintf(kStr, sizeof(kStr), "--.-C");
        }

        snprintf(line1, sizeof(line1), "E:%s K:%s", eStr, kStr);
    }

    // 2. sor: normál esetben "Sz:20.8C C:21.5C", riasztáskor 500ms-enként váltakozva
    bool showAlarmLine = (_state->alarmConfig.alarmEnabled != 0) &&
                         (_state->alarmState.active != 0) &&
                         alarmBlinkPhase;

    if (showAlarmLine) {
        switch (_state->alarmState.reason) {
            case ALARM_REASON_OVERHEAT: {
                if (isTempValid(_state->kazan)) {
                    snprintf(line2, sizeof(line2), "RIASZTAS! Kaz %dC",
                             static_cast<int>(lroundf(_state->kazan)));
                } else {
                    snprintf(line2, sizeof(line2), "RIASZTAS! Kaz --C");
                }
                break;
            }
            case ALARM_REASON_SENSOR_FAIL:
                snprintf(line2, sizeof(line2), "RIASZTAS! SZENZOR!");
                break;
            case ALARM_REASON_NRF_FAIL:
            default:
                snprintf(line2, sizeof(line2), "RIASZTAS! NRF HIBA!");
                break;
        }
    } else {
        char szStr[10];
        if (!isnan(_state->currentTemp)) {
            snprintf(szStr, sizeof(szStr), "%4.1fC", _state->currentTemp);
        } else {
            snprintf(szStr, sizeof(szStr), "--.-C");
        }
        snprintf(line2, sizeof(line2), "Sz:%s C:%4.1fC", szStr, _state->targetTemp);
    }

    _lcd->printLines(line1, line2);
}

void MenuController::drawMenu() {
    if (!_lcd || !_state || _cursor >= _itemCount) return;

    const MenuItem& item = _items[_cursor];
    char line2[32];
    formatMenuItemValue(item, line2, sizeof(line2));
    _lcd->printLines(item.name, line2);
}

void MenuController::drawEditValue(bool editBlinkPhase) {
    if (!_lcd || !_state || _editItemIndex >= _itemCount) return;

    const MenuItem& item = _items[_editItemIndex];
    char valStr[24];
    formatEditValueString(item, valStr, sizeof(valStr));

    char line2[32];
    if (editBlinkPhase) {
        snprintf(line2, sizeof(line2), "%s < >", valStr);
    } else {
        snprintf(line2, sizeof(line2), "%s", valStr);
    }

    _lcd->printLines(item.name, line2);
}

// ============================================================================
// ÉRTÉKEK FORMÁZÁSA KIJELZÉSHEZ
// ============================================================================
void MenuController::formatMenuItemValue(const MenuItem& item, char* buf, size_t bufLen) const {
    if (!_state || bufLen == 0) return;

    switch (item.type) {
        case ITEM_SUBMENU:
            snprintf(buf, bufLen, "> BELEPES");
            break;

        case ITEM_ACTION_BACK:
            snprintf(buf, bufLen, "> FOMENU");
            break;

        case ITEM_ACTION_FACTORY_RESET:
            snprintf(buf, bufLen, "Tartsd 3mp-ig!");
            break;

        case ITEM_READONLY_TEMP: {
            float t = *static_cast<const float*>(item.valuePtr);
            if (item.extraId == SENSOR_SRC_NANO) {
                if (!_state->nrf24Ok || !isTempValid(t)) {
                    snprintf(buf, bufLen, "--.-C");
                } else {
                    snprintf(buf, bufLen, "%.1fC", t);
                }
            } else {
                if (isnan(t)) {
                    snprintf(buf, bufLen, "--.-C");
                } else {
                    snprintf(buf, bufLen, "%.1fC", t);
                }
            }
            break;
        }

        case ITEM_READONLY_STATUS:
            switch (item.extraId) {
                case STAT_SZIVATTYU:
                    snprintf(buf, bufLen, "%s", _state->pumpOn ? "BE" : "KI");
                    break;
                case STAT_FUTES:
                    snprintf(buf, bufLen, "%s", _state->heatingOn ? "BE" : "KI");
                    break;
                case STAT_PUMP_STATE:
                    snprintf(buf, bufLen, "%s",
                             (_state->pumpState == PUMP_STATE_FUTES) ? "FUTES" : "VARAKOZAS");
                    break;
                case STAT_RIASZTAS:
                    if (_state->alarmState.active == 0) {
                        snprintf(buf, bufLen, "NINCS");
                    } else if (_state->alarmState.reason == ALARM_REASON_OVERHEAT) {
                        snprintf(buf, bufLen, "TULMELEGEDES");
                    } else if (_state->alarmState.reason == ALARM_REASON_SENSOR_FAIL) {
                        snprintf(buf, bufLen, "SZENZOR HIBA");
                    } else if (_state->alarmState.reason == ALARM_REASON_NRF_FAIL) {
                        snprintf(buf, bufLen, "NRF HIBA");
                    } else {
                        snprintf(buf, bufLen, "AKTIV");
                    }
                    break;
                case STAT_RADIO:
                    if (_state->nrf24Ok) {
                        snprintf(buf, bufLen, "OK %us", _state->heartbeatIntervalSec);
                    } else {
                        snprintf(buf, bufLen, "NINCS KAPCSOLAT");
                    }
                    break;
                default:
                    snprintf(buf, bufLen, "-");
                    break;
            }
            break;

        case ITEM_EDIT_FLOAT: {
            float v = *static_cast<const float*>(item.valuePtr);
            snprintf(buf, bufLen, "%.1fC", v);
            break;
        }

        case ITEM_EDIT_ENUM:
            switch (item.extraId) {
                case EDIT_MODE:
                    if (_state->mode == MODE_MANUAL)      snprintf(buf, bufLen, "MANUAL");
                    else if (_state->mode == MODE_OFF)    snprintf(buf, bufLen, "OFF");
                    else                                  snprintf(buf, bufLen, "AUTO");
                    break;
                case EDIT_PUMP_MODE:
                    if (_state->pumpConfig.pumpMode == PUMP_MODE_MANUAL_ON)       snprintf(buf, bufLen, "BE");
                    else if (_state->pumpConfig.pumpMode == PUMP_MODE_MANUAL_OFF) snprintf(buf, bufLen, "KI");
                    else                                                          snprintf(buf, bufLen, "AUTO");
                    break;
                case EDIT_PUMP_SENSOR:
                    if (_state->pumpConfig.pumpStartSensor == PUMP_SENSOR_ELOREMENO) snprintf(buf, bufLen, "ELOREMENO");
                    else if (_state->pumpConfig.pumpStartSensor == PUMP_SENSOR_MAX)  snprintf(buf, bufLen, "MAX");
                    else                                                             snprintf(buf, bufLen, "KAZAN");
                    break;
                case EDIT_HEARTBEAT:
                    snprintf(buf, bufLen, "%us", _state->heartbeatIntervalSec);
                    break;
                default:
                    snprintf(buf, bufLen, "-");
                    break;
            }
            break;

        case ITEM_EDIT_BOOL:
            if (item.extraId == EDIT_ALARM_ENABLED) {
                snprintf(buf, bufLen, "%s", (_state->alarmConfig.alarmEnabled != 0) ? "BE" : "KI");
            } else if (item.extraId == EDIT_MANUAL_HEATING) {
                snprintf(buf, bufLen, "%s", _state->manualHeating ? "BE" : "KI");
            } else {
                snprintf(buf, bufLen, "-");
            }
            break;

        case ITEM_OUTPUT_BOOL: {
            uint8_t bit = item.extraId;
            if (bit == OUT_BIT_SZIVATTYU) {
                if (_state->pumpConfig.pumpMode == PUMP_MODE_AUTO) {
                    snprintf(buf, bufLen, "AUTO - ZAROLVA");
                } else {
                    snprintf(buf, bufLen, "%s", _state->pumpOn ? "BE" : "KI");
                }
            } else if (bit == OUT_BIT_LED_PIROS) {
                if (_state->alarmConfig.alarmEnabled != 0 && _state->alarmState.active != 0) {
                    snprintf(buf, bufLen, "BE (RIASZTAS)");
                } else {
                    snprintf(buf, bufLen, "%s", getBit(_state->outputsMask, OUT_BIT_LED_PIROS) ? "BE" : "KI");
                }
            } else {
                snprintf(buf, bufLen, "%s", getBit(_state->outputsMask, bit) ? "BE" : "KI");
            }
            break;
        }

        default:
            snprintf(buf, bufLen, "-");
            break;
    }
}

void MenuController::formatEditValueString(const MenuItem& item, char* buf, size_t bufLen) const {
    if (bufLen == 0) return;

    if (item.type == ITEM_EDIT_FLOAT) {
        if (item.extraId == EDIT_PUMP_START && _editPartnerFloat < _state->pumpConfig.pumpStopTemp) {
            snprintf(buf, bufLen, "%.1fC(St%.0f)", _editFloat, _editPartnerFloat);
        } else {
            snprintf(buf, bufLen, "%.1fC", _editFloat);
        }
        return;
    }

    if (item.type == ITEM_EDIT_ENUM) {
        switch (item.extraId) {
            case EDIT_MODE:
                if (_editEnum == MODE_MANUAL)      snprintf(buf, bufLen, "MANUAL");
                else if (_editEnum == MODE_OFF)    snprintf(buf, bufLen, "OFF");
                else                               snprintf(buf, bufLen, "AUTO");
                break;
            case EDIT_PUMP_MODE:
                if (_editEnum == PUMP_MODE_MANUAL_ON)       snprintf(buf, bufLen, "BE");
                else if (_editEnum == PUMP_MODE_MANUAL_OFF) snprintf(buf, bufLen, "KI");
                else                                        snprintf(buf, bufLen, "AUTO");
                break;
            case EDIT_PUMP_SENSOR:
                if (_editEnum == PUMP_SENSOR_ELOREMENO) snprintf(buf, bufLen, "ELOREMENO");
                else if (_editEnum == PUMP_SENSOR_MAX)  snprintf(buf, bufLen, "MAX");
                else                                    snprintf(buf, bufLen, "KAZAN");
                break;
            case EDIT_HEARTBEAT:
                snprintf(buf, bufLen, "%us", HEARTBEAT_OPTIONS[_editEnum % 6]);
                break;
            default:
                snprintf(buf, bufLen, "-");
                break;
        }
        return;
    }

    if (item.type == ITEM_EDIT_BOOL) {
        snprintf(buf, bufLen, "%s", _editBool ? "BE" : "KI");
        return;
    }

    snprintf(buf, bufLen, "-");
}
