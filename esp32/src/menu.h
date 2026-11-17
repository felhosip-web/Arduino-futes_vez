#pragma once

#include <Arduino.h>
#include <stdint.h>
#include "config.h"
#include "lcd.h"
#include "log.h"
#include "../../nano/src/common.h"

// ============================================================================
// KÖZÖS RENDSZERÁLLAPOT (ESP32-C3 MASTER + MENÜ + MQTT + NVS)
// ============================================================================
struct SystemState {
    // Szobatermosztát (LM35 + menü)
    float   currentTemp   = 20.0f;
    float   targetTemp    = TARGET_TEMP_DEFAULT;
    bool    heatingOn     = false;
    uint8_t mode          = MODE_AUTO;
    bool    manualHeating = false;

    // Kimenetek (bit0 a kazánvédelmi állapotgéphez kötött)
    uint8_t outputsMask   = 0;
    bool    userLedPiros  = false;

    // Nano felől érkező adatok (-127.0f ha szenzorhiba)
    float            eloremeno              = SENSOR_DISCONNECTED_VAL;
    float            visszatero             = SENSOR_DISCONNECTED_VAL;
    float            kazan                  = SENSOR_DISCONNECTED_VAL;
    float            kulteri                = SENSOR_DISCONNECTED_VAL;
    uint8_t          kazanStatus[8]         = { 0 };
    uint32_t         nanoCounter            = 0;
    uint8_t          sensorValidMask        = 0;
    AlarmStatePacked nanoAlarmState         = { 0, ALARM_REASON_NONE };
    bool             nrf24Ok                = false;
    uint8_t          consecutiveMissedPings = 0;

    // V5 Master-Slave életjel protokoll
    uint8_t  heartbeatIntervalSec = HEARTBEAT_DEFAULT_SEC;
    uint32_t txSeq                = 0;

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

    // Riasztási konfiguráció és egyesített állapot
    AlarmConfigPacked alarmConfig = {
        ALARM_OVERHEAT_TEMP_DEFAULT,
        ALARM_ENABLED_DEFAULT ? 1 : 0
    };
    AlarmStatePacked alarmState = {
        0,
        ALARM_REASON_NONE
    };
};

// ============================================================================
// MENÜ ÁLLAPOTOK ÉS MENÜPONT STRUKTÚRA
// ============================================================================
enum MenuState : uint8_t {
    MENU_HOME = 0,     // Alap főképernyő (3 hőfok)
    MENU_MAIN,         // Főmenü (4 pont)
    MENU_FOKOK,        // FOKOK almenü (csak lekérdezés)
    MENU_STATUS,       // STATUS almenü (csak lekérdezés)
    MENU_BEALLITAS,    // BEALLITAS almenü (szerkeszthető)
    MENU_KIMENETEK,    // KIMENETEK almenü (kapcsolható)
    MENU_LOGS,         // STATUS -> NAPLO (utolsó 5 naplóbejegyzés lapozása)
    MENU_EDIT          // Érték szerkesztés (BEALLITAS pont)
};

enum MenuItemType : uint8_t {
    ITEM_SUBMENU = 0,
    ITEM_READONLY_TEMP,
    ITEM_READONLY_STATUS,
    ITEM_EDIT_FLOAT,
    ITEM_EDIT_ENUM,
    ITEM_EDIT_BOOL,
    ITEM_OUTPUT_BOOL,
    ITEM_ACTION_BACK,
    ITEM_ACTION_FACTORY_RESET
};

struct MenuItem {
    const char*  name;
    MenuItemType type;
    void*        valuePtr;
    float        minVal;
    float        maxVal;
    float        stepVal;
    uint8_t      extraId;
};

struct MenuCallbacks {
    void (*onSettingCommitted)(void);
    void (*onFactoryReset)(void);
};

// Egységes NVS mentő függvények (implementáció: main.cpp)
bool saveSetting(const char* key, float value);
bool saveSetting(const char* key, uint32_t value);
bool saveSetting(const char* key, bool value);
bool saveSetting(const char* key, const char* value);

class MenuController {
public:
    void begin(HeatingLcd* lcd, SystemState* state, const MenuCallbacks& callbacks);

    void handleInput(int8_t encSteps,
                     bool shortPress,
                     bool longPress,
                     bool factoryPress,
                     uint32_t now);

    void update(uint32_t now, bool alarmBlinkPhase);
    void requestRedraw();
    void showErrorBanner(const char* line1, const char* line2, uint32_t durationMs = 2000);

    MenuState getState() const { return _menuState; }
    bool isEditing() const { return _menuState == MENU_EDIT; }
    bool isOnFactoryResetItem() const;

    // Külön kirajzoló függvények a specifikáció szerint
    void drawMainScreen(bool alarmBlinkPhase);
    void drawMenu();
    void drawEditValue(bool editBlinkPhase);
    void drawLogViewer();

private:
    HeatingLcd*   _lcd   = nullptr;
    SystemState*  _state = nullptr;
    MenuCallbacks _callbacks{};

    MenuState _menuState      = MENU_HOME;
    uint8_t   _cursor         = 0;
    uint8_t   _mainCursor     = 0;
    uint8_t   _logCursor      = 0;

    // Szerkesztési puffer (MQTT CMD vagy 20mp timeout nem írja felül félbehagyva)
    uint8_t _editItemIndex    = 0;
    float   _editFloat        = 0.0f;
    float   _editPartnerFloat = 0.0f; // Pump Start állításakor a Pump Stop automatikus követéséhez
    uint8_t _editEnum         = 0;
    bool    _editBool         = false;

    uint32_t _lastActivityMs  = 0;
    uint32_t _lastMenuDrawMs  = 0;
    uint32_t _lastMainDrawMs  = 0;
    uint32_t _lastEditBlinkMs = 0;
    uint32_t _bannerUntilMs   = 0;
    uint32_t _alertUntilMs    = 0;

    bool _editBlinkOn       = true;
    bool _lastAlarmBlink    = false;
    bool _menuDirty         = true;
    bool _mainDirty         = true;

    // Dinamikus menütáblák adott menüszinthez
    static constexpr uint8_t MAX_MENU_ITEMS = 14;
    MenuItem _items[MAX_MENU_ITEMS];
    uint8_t  _itemCount = 0;

    void buildItemsForState(MenuState targetState);
    void enterSubmenu(MenuState sub, uint8_t initialCursor, uint32_t now);
    void enterEditMode(uint8_t itemIndex, uint32_t now);
    void commitEdit(uint32_t now);
    void toggleOutput(uint8_t itemIndex, uint32_t now);
    void navigateBack(uint32_t now);

    void formatMenuItemValue(const MenuItem& item, char* buf, size_t bufLen) const;
    void formatEditValueString(const MenuItem& item, char* buf, size_t bufLen) const;
    static bool isTempValid(float t);
    static float roundHalf(float v);
};
