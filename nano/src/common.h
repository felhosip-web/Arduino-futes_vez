#pragma once

#include <stdint.h>

// ============================================================================
// KÖZÖS NRF24L01+ SZERZŐDÉS V5 (MASTER-SLAVE) - PONTOSAN 32 BYTE
// ============================================================================
// Master: ESP32-C3 (32-bit RISC-V), Slave: Arduino Nano (8-bit AVR).
// Mindkét oldalon fix méretű típusok + __attribute__((packed)) kötelező.
// A WriteAckPayload TILOS; külön REQUEST (0xA1) és RESPONSE (0xB2) csomagok
// közlekednek fél-duplex csatornán, seq azonosítással.
//
// Méret audit (NanoToEspPayload):
//   4x float (16B) + uint32_t seq (4B) + uint32_t counter (4B) +
//   uint8_t kazanStatusMask (1B -> ESP32-n 8 elemű kazan_status[8] tömbbé bővítve) +
//   uint8_t sensorValidMask (1B) + AlarmStatePacked (2B) +
//   uint8_t syncFlags (1B) + uint16_t syncEpochLow16 (2B) = 31 byte.
//   NrfPacket = 1 byte packetType + 31 byte union = pontosan 32 byte!

constexpr uint8_t PKT_TYPE_REQUEST  = 0xA1; // Master -> Slave
constexpr uint8_t PKT_TYPE_RESPONSE = 0xB2; // Slave  -> Master

constexpr float SENSOR_DISCONNECTED_VAL = -127.0f;

// Időszinkron nyugta, parancs-visszaigazolás és slave állapot bitek a NanoToEspPayload.syncFlags mezőben
constexpr uint8_t SYNC_FLAG_ACK                  = 0x01; // bit0: Nano kapott érvényes timeSyncEpoch-ot
constexpr uint8_t SYNC_FLAG_STALE                = 0x02; // bit1: >13 órája nem érkezett friss szinkron
constexpr uint8_t SYNC_FLAG_SERVICE_MODE         = 0x04; // bit2: Kazán oldali szerviz kapcsoló (D6) BE van kapcsolva
constexpr uint8_t SYNC_FLAG_CMD_ACK              = 0x08; // bit3: Nano végrehajtotta és D7-en visszaigazolta az utolsó parancsot (cmdSeq)
constexpr uint8_t SYNC_FLAG_PUMP_ON              = 0x10; // bit4: Parancsolt (commanded) szivattyú állapot (1=BE, 0=KI)
constexpr uint8_t SYNC_FLAG_CMD_REJECTED_SERVICE = 0x20; // bit5: Szerviz mód miatt elutasított pump KI parancs
constexpr uint8_t SYNC_FLAG_RELAY_FB_ON          = 0x40; // bit6: Relé fizikai visszacsatolás D7-ről (1=BE/HIGH, 0=KI/LOW)
constexpr uint8_t SYNC_FLAG_RELAY_MISMATCH       = 0x80; // bit7: Relé visszacsatolási hiba (>500ms eltérés)

// Loop Monitor hiba bit az alarmPacked mező szabad felső bitjén (bit4),
// így a syncFlags összes 8 bitje (0x01..0x80) és a 32 byte-os NrfPacket érintetlen marad.
constexpr uint8_t SYNC_FLAG_LOOP_FAULT           = 0x10; // alarmPacked bit4: Nano loop ciklusidő hiba (>100ms x5 vagy >500ms x1)

#ifndef SAFETY_OVERHEAT_TEMP_DEFINED
#define SAFETY_OVERHEAT_TEMP_DEFINED
// Fix hardveres/szoftveres szivattyú biztonsági küszöb (NEM állítható, NEM menthető, NEM megy át NRF-en)
constexpr float SAFETY_OVERHEAT_TEMP = 95.0f;
#endif

constexpr uint8_t HEARTBEAT_DEFAULT_SEC = 5;
constexpr uint8_t HEARTBEAT_MIN_SEC     = 5;
constexpr uint8_t HEARTBEAT_MAX_SEC     = 30;
constexpr uint8_t HEARTBEAT_STEP_SEC    = 5;

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

// Riasztási okok
enum AlarmReason : uint8_t {
    ALARM_REASON_NONE        = 0,
    ALARM_REASON_OVERHEAT    = 1,
    ALARM_REASON_SENSOR_FAIL = 2,
    ALARM_REASON_NRF_FAIL    = 3,
    ALARM_REASON_PUMP_FAULT  = 4
};

// ESP -> NANO kimeneti bitmaszk bitek
constexpr uint8_t OUT_BIT_SZIVATTYU = 0; // bit0 -> A1 (15) relé meghajtás (D7 a relé visszacsatolás bemenete)
constexpr uint8_t OUT_BIT_LED_ZOLD  = 1; // bit1 -> D5
constexpr uint8_t OUT_BIT_LED_PIROS = 2; // bit2 -> A2 (16) (D6 a Szerviz kapcsoló bemenete)
constexpr uint8_t OUT_BIT_TARTALEK1 = 3; // bit3 -> D8
constexpr uint8_t OUT_BIT_TARTALEK2 = 4; // bit4 -> D9
constexpr uint8_t OUT_BIT_TARTALEK3 = 5; // bit5 -> D10 (OUTPUT módban AVR SPI Master feltétel miatt)
constexpr uint8_t OUT_BIT_TARTALEK4 = 6; // bit6 -> A3 (17)
constexpr uint8_t OUT_BIT_TARTALEK5 = 7; // bit7 -> A4 (18)

struct __attribute__((packed)) PumpConfigPacked {
    uint8_t pumpMode;        // PumpMode (0=auto, 1=manual_on, 2=manual_off)
    float   pumpStartTemp;   // 30.0 - 50.0 °C (alap: 40.0)
    float   pumpStopTemp;    // 20.0 - 40.0 °C (alap: 30.0), mindig < pumpStartTemp - 5
    uint8_t pumpStartSensor; // PumpStartSensor (0=kazan, 1=eloremeno, 2=max)
};

struct __attribute__((packed)) AlarmConfigPacked {
    float   alarmTemp;       // 70.0 - 90.0 °C (alap: 85.0) - csak riasztás
    uint8_t alarmEnabled;    // 0 = false (nincs villogás), 1 = true
};

struct __attribute__((packed)) AlarmStatePacked {
    uint8_t active;          // 0 = false, 1 = true
    uint8_t reason;          // AlarmReason (0=none, 1=overheat, 2=sensor_fail, 3=nrf_fail)
};

struct __attribute__((packed)) EspToNanoPayload {
    uint8_t           outputsMask;          // bit0: szivattyu, bit1: led_zold, bit2: led_piros, bit3-7: tartalek1-5
    float             targetTemp;           // Célhőmérséklet (°C)
    uint8_t           modeAndManual;        // bit0..1: HeatingMode (0..2), bit2: manualHeating (0/1)
    uint8_t           cmdSeq;               // 0 = nincs parancs nyugta igény, 1..255 = cmdId alsó 8 bitje (CMD_ACK)
    PumpConfigPacked  pumpConfig;           // 10 byte
    AlarmConfigPacked alarmConfig;          // 5 byte
    uint8_t           heartbeatIntervalSec; // 5, 10, 15, 20, 25, 30 mp (alap: 5)
    uint32_t          seq;                  // Növekvő kérés sorszám
    uint32_t          timeSyncEpoch;        // 0 ha nincs időszinkron ebben a csomagban, különben UTC epoch (4 byte)
};

struct __attribute__((packed)) NanoToEspPayload {
    float    eloremeno;       // Előremenő vízhőfok (°C), -127.0f hiba esetén
    float    visszatero;      // Visszatérő vízhőfok (°C), -127.0f hiba esetén
    float    kazan;           // Kazán hőfok (°C), -127.0f hiba esetén
    float    kulteri;         // Kültéri hőfok (°C), -127.0f hiba esetén
    uint8_t  kazanStatusMask; // 8 bit státusz maszk (ESP32-n kazan_status[8]-ra bontva, egyelőre 0)
    uint32_t counter;         // Növekvő válaszcsomag számláló
    uint32_t seq;             // UGYANAZ a seq, mint a kérésben
    uint8_t  sensorValidMask; // bit0: eloremeno, bit1: visszatero, bit2: kazan, bit3: kulteri
    uint8_t  alarmPacked;     // bit0: active, bit1..3: AlarmReason (0..3), bit4: SYNC_FLAG_LOOP_FAULT
    uint8_t  cmdSeq;          // Utoljára feldolgozott parancs cmdSeq (1..255, 0 ha nincs)
    uint8_t  syncFlags;       // bit0: ACK, bit1: STALE, bit2: SERVICE_MODE, bit3: CMD_ACK, bit4: PUMP_ON, bit5: CMD_REJECTED_SERVICE, bit6: RELAY_FB_ON, bit7: RELAY_MISMATCH
    uint16_t syncEpochLow16;  // Utolsó kapott timeSyncEpoch alsó 16 bitje nyugtázáshoz (2 byte)
};

struct __attribute__((packed)) NrfPacket {
    uint8_t packetType; // PKT_TYPE_REQUEST (0xA1) vagy PKT_TYPE_RESPONSE (0xB2)
    union {
        EspToNanoPayload esp;
        NanoToEspPayload nano;
    };
};

static_assert(sizeof(PumpConfigPacked)  == 10, "PumpConfigPacked merete 10 byte kell legyen!");
static_assert(sizeof(AlarmConfigPacked) == 5,  "AlarmConfigPacked merete 5 byte kell legyen!");
static_assert(sizeof(AlarmStatePacked)  == 2,  "AlarmStatePacked merete 2 byte kell legyen!");
static_assert(sizeof(EspToNanoPayload)  == 31, "EspToNanoPayload merete 31 byte kell legyen!");
static_assert(sizeof(NanoToEspPayload)  == 31, "NanoToEspPayload merete 31 byte kell legyen!");
static_assert(sizeof(NrfPacket)         == 32, "NrfPacket meretenek pontosan 32 byte-nak kell lennie!");

// Közös RF24 rádió csatorna és külön Master->Slave / Slave->Master csőcímek (5 byte)
constexpr uint8_t NRF_CHANNEL = 108;
constexpr uint8_t NRF_ADDR_MASTER_TO_SLAVE[6] = "FUTE1";
constexpr uint8_t NRF_ADDR_SLAVE_TO_MASTER[6] = "FUTE2";

// Segédfüggvények bitkezeléshez és heartbeat szanitizáláshoz
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

inline uint8_t packModeAndManual(uint8_t mode, bool manualHeating) {
    return static_cast<uint8_t>((mode & 0x03U) | (manualHeating ? 0x04U : 0x00U));
}

inline uint8_t unpackMode(uint8_t modeAndManual) {
    return static_cast<uint8_t>(modeAndManual & 0x03U);
}

inline bool unpackManualHeating(uint8_t modeAndManual) {
    return (modeAndManual & 0x04U) != 0;
}

inline uint8_t packAlarmState(const AlarmStatePacked& alarm) {
    return static_cast<uint8_t>((alarm.active ? 0x01U : 0x00U) | ((alarm.reason & 0x07U) << 1));
}

inline AlarmStatePacked unpackAlarmState(uint8_t packed) {
    AlarmStatePacked out{};
    out.active = (packed & 0x01U) != 0 ? 1U : 0U;
    out.reason = static_cast<uint8_t>((packed >> 1) & 0x07U);
    return out;
}

inline uint8_t sanitizeHeartbeatIntervalSec(uint8_t sec) {
    if (sec < HEARTBEAT_MIN_SEC) return HEARTBEAT_MIN_SEC;
    if (sec > HEARTBEAT_MAX_SEC) return HEARTBEAT_MAX_SEC;
    uint8_t snapped = static_cast<uint8_t>(((sec + 2U) / HEARTBEAT_STEP_SEC) * HEARTBEAT_STEP_SEC);
    if (snapped < HEARTBEAT_MIN_SEC) snapped = HEARTBEAT_MIN_SEC;
    if (snapped > HEARTBEAT_MAX_SEC) snapped = HEARTBEAT_MAX_SEC;
    return snapped;
}
