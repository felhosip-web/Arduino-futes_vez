#pragma once

#include <stdint.h>

// ============================================================================
// KÖZÖS NRF24L01+ SZERZŐDÉS V4 - PONTOSAN 32 BYTE MINDKÉT OLDALON
// ============================================================================
// Nano (8-bit AVR) és ESP32-C3 (32-bit RISC-V) közötti bináris kompatibilitás
// érdekében fix méretű típusokat és __attribute__((packed)) union csomagot
// használunk, így minden V4 mező natív típusban fér el 32 byte-ban.

constexpr uint8_t PKT_TYPE_ESP_TO_NANO = 0xA1;
constexpr uint8_t PKT_TYPE_NANO_TO_ESP = 0xB2;

constexpr float SENSOR_DISCONNECTED_VAL = -127.0f;

// Szobatermosztát módok
enum HeatingMode : uint8_t {
    MODE_AUTO   = 0,
    MODE_MANUAL = 1,
    MODE_OFF    = 2
};

// Kazánvédelem szivattyú módok
enum PumpMode : uint8_t {
    PUMP_MODE_AUTO       = 0,
    PUMP_MODE_MANUAL_ON  = 1,
    PUMP_MODE_MANUAL_OFF = 2
};

// Szivattyú indítási szenzor választás
enum PumpStartSensor : uint8_t {
    PUMP_SENSOR_KAZAN     = 0,
    PUMP_SENSOR_ELOREMENO = 1,
    PUMP_SENSOR_MAX       = 2
};

// Szivattyú állapotgép állapotai
enum PumpState : uint8_t {
    PUMP_STATE_VARAKOZAS = 0,
    PUMP_STATE_FUTES     = 1
};

// ESP -> NANO kimeneti bitmaszk bitek
constexpr uint8_t OUT_BIT_SZIVATTYU = 0; // bit0 -> D7 relé
constexpr uint8_t OUT_BIT_LED_ZOLD  = 1; // bit1 -> D5
constexpr uint8_t OUT_BIT_LED_PIROS = 2; // bit2 -> D6
constexpr uint8_t OUT_BIT_TARTALEK1 = 3; // bit3 -> D8
constexpr uint8_t OUT_BIT_TARTALEK2 = 4; // bit4 -> D9
constexpr uint8_t OUT_BIT_TARTALEK3 = 5; // bit5 -> D10
constexpr uint8_t OUT_BIT_TARTALEK4 = 6; // bit6 -> A3 (17) (D11 foglalt Hardware SPI MOSI-nak)
constexpr uint8_t OUT_BIT_TARTALEK5 = 7; // bit7 -> A4 (18) (D12 foglalt Hardware SPI MISO-nak)

struct __attribute__((packed)) PumpConfigPacked {
    uint8_t pumpMode;        // PumpMode (0=auto, 1=manual_on, 2=manual_off)
    float   pumpStartTemp;   // 30.0 - 50.0 °C (alap: 40.0)
    float   pumpStopTemp;    // 20.0 - 40.0 °C (alap: 30.0), mindig < pumpStartTemp - 5
    uint8_t pumpStartSensor; // PumpStartSensor (0=kazan, 1=eloremeno, 2=max)
};

struct __attribute__((packed)) EspToNanoPayload {
    uint8_t          outputsMask;    // bit0: szivattyu, bit1: led_zold, bit2: led_piros, bit3-7: tartalek1-5
    float            targetTemp;     // Célhőmérséklet (°C)
    uint8_t          mode;           // HeatingMode (0=auto, 1=manual, 2=off)
    uint8_t          manualHeating;  // 0 = false, 1 = true
    PumpConfigPacked pumpConfig;     // 10 byte
    uint8_t          reserved[14];   // Kitöltés 31 byte-ra
};

struct __attribute__((packed)) NanoToEspPayload {
    float    eloremeno;       // Előremenő vízhőfok (°C), -127.0f hiba esetén
    float    visszatero;      // Visszatérő vízhőfok (°C), -127.0f hiba esetén
    float    kazan;           // Kazán hőfok (°C), -127.0f hiba esetén
    float    kulteri;         // Kültéri hőfok (°C), -127.0f hiba esetén
    uint8_t  kazan_status[8]; // 8 byte tartalék státusz tömb (egyelőre mind 0)
    uint32_t counter;         // Növekvő válaszcsomag számláló
    uint8_t  sensorValidMask; // bit0: eloremeno, bit1: visszatero, bit2: kazan, bit3: kulteri
    uint8_t  reserved[2];     // Kitöltés 31 byte-ra
};

struct __attribute__((packed)) NrfPacket {
    uint8_t packetType; // PKT_TYPE_ESP_TO_NANO vagy PKT_TYPE_NANO_TO_ESP
    union {
        EspToNanoPayload esp;
        NanoToEspPayload nano;
    };
};

static_assert(sizeof(PumpConfigPacked) == 10, "PumpConfigPacked merete 10 byte kell legyen!");
static_assert(sizeof(EspToNanoPayload) == 31, "EspToNanoPayload merete 31 byte kell legyen!");
static_assert(sizeof(NanoToEspPayload) == 31, "NanoToEspPayload merete 31 byte kell legyen!");
static_assert(sizeof(NrfPacket) == 32, "NrfPacket meretenek pontosan 32 byte-nak kell lennie!");

// Közös RF24 rádió csatorna és cím (5 byte)
constexpr uint8_t NRF_CHANNEL = 108;
constexpr uint8_t NRF_ADDR_PIPE[6] = "FUTES";

// Segédfüggvények bitkezeléshez
inline bool getBit(uint8_t mask, uint8_t bitIndex) {
    return (mask & (1U << bitIndex)) != 0;
}

inline void setBit(uint8_t& mask, uint8_t bitIndex, bool value) {
    if (value) {
        mask |= static_cast<uint8_t>(1U << bitIndex);
    } else {
        mask &= static_cast<uint8_t>(~(1U << bitIndex));
    }
}
