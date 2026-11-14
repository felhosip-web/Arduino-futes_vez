#pragma once

#include <stdint.h>
#include <math.h>

// ============================================================================
// KÖZÖS NRF24L01+ SZERZŐDÉS - PONTOSAN 32 BYTE MINDKÉT OLDALON
// ============================================================================
// Nano (8-bit AVR) és ESP32-C3 (32-bit RISC-V) közötti bináris kompatibilitás
// érdekében fix méretű típusokat és __attribute__((packed)) attribútumot használunk.

constexpr uint8_t PKT_TYPE_ESP_TO_NANO = 0xA1;
constexpr uint8_t PKT_TYPE_NANO_TO_ESP = 0xB2;

// Módok
enum HeatingMode : uint8_t {
    MODE_AUTO   = 0,
    MODE_MANUAL = 1,
    MODE_OFF    = 2
};

// ESP -> NANO kimeneti bitmaszk bitek
constexpr uint8_t OUT_BIT_SZIVATTYU = 0; // bit0 -> D7 relé
constexpr uint8_t OUT_BIT_LED_ZOLD  = 1; // bit1 -> D5
constexpr uint8_t OUT_BIT_LED_PIROS = 2; // bit2 -> D6
constexpr uint8_t OUT_BIT_TARTALEK1 = 3; // bit3 -> D8
constexpr uint8_t OUT_BIT_TARTALEK2 = 4; // bit4 -> D9
constexpr uint8_t OUT_BIT_TARTALEK3 = 5; // bit5 -> D10
constexpr uint8_t OUT_BIT_TARTALEK4 = 6; // bit6 -> D11
constexpr uint8_t OUT_BIT_TARTALEK5 = 7; // bit7 -> D12

// NANO -> ESP szenzor érvényességi bitmaszk bitek
constexpr uint8_t SENS_VALID_ELOREMENO  = 0;
constexpr uint8_t SENS_VALID_VISSZATERO = 1;
constexpr uint8_t SENS_VALID_KAZAN      = 2;
constexpr uint8_t SENS_VALID_KULTERI    = 3;

struct __attribute__((packed)) NrfPacket {
    // --- Fejléc (1 byte) ---
    uint8_t packetType;       // PKT_TYPE_ESP_TO_NANO vagy PKT_TYPE_NANO_TO_ESP

    // --- ESP -> NANO mezők (7 byte) ---
    uint8_t outputsMask;      // bit0: szivattyu, bit1: led_zold, bit2: led_piros, bit3-7: tartalek1-5
    float targetTemp;         // Célhőmérséklet (°C)
    uint8_t mode;             // HeatingMode (0=AUTO, 1=MANUAL, 2=OFF)
    uint8_t manualHeating;    // 0 = KI, 1 = BE

    // --- NANO -> ESP mezők (24 byte) ---
    float eloremeno;          // Előremenő vízhőfok (°C), NAN ha -127
    float visszatero;         // Visszatérő vízhőfok (°C), NAN ha -127
    float kazan;              // Kazán hőfok (°C), NAN ha -127
    float kulteri;            // Kültéri hőfok (°C), NAN ha -127
    uint8_t kazan_status;     // bit0-7 -> kazan_status[0..7] tartalék állapotbitek
    uint8_t sensorValidMask;  // bit0-3 jelzi az érvényes DS18B20 méréseket
    uint8_t nrf24_ok;         // 1 = kapcsolat rendben, 0 = hiba
    uint8_t reserved;         // Fenntartott (0)
    uint32_t counter;         // Növekvő válaszcsomag számláló
};

static_assert(sizeof(NrfPacket) == 32, "NrfPacket meretenek pontosan 32 byte-nak kell lennie!");

// Közös RF24 rádió csatorna és címek (5 byte)
constexpr uint8_t NRF_CHANNEL = 108;
constexpr uint8_t NRF_ADDR_PIPE[6] = "FUTES";

// Segédfüggvény kimeneti bit írásához/olvasásához
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
