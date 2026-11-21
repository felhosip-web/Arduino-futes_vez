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
constexpr uint8_t SYNC_FLAG_SENSOR_UNKNOWN       = 0x20; // alarmPacked bit5: Ismeretlen / tanítatlan DS18B20 szenzor

// Opcionális DS18B20 felderítési és tanítási csomagtípusok (továbbra is pontosan 32 byte-os NrfPacket)
constexpr uint8_t PKT_TYPE_SENSOR_CMD            = 0xA3; // Master -> Slave szenzor parancs
constexpr uint8_t PKT_TYPE_SENSOR_RSP            = 0xB4; // Slave  -> Master szenzor válasz

constexpr uint8_t SENSOR_ROLE_COUNT              = 4;
constexpr uint8_t MAX_DISCOVERED_SENSORS         = 6;

enum SensorRole : uint8_t {
    SENSOR_ROLE_ELOREMENO  = 0,
    SENSOR_ROLE_VISSZATERO = 1,
    SENSOR_ROLE_KAZAN      = 2,
    SENSOR_ROLE_KULTERI    = 3
};

enum SensorSubCmd : uint8_t {
    SENSOR_CMD_SCAN_START = 0,
    SENSOR_CMD_GET_FOUND  = 1,
    SENSOR_CMD_GET_ROLES  = 2,
    SENSOR_CMD_LEARN      = 3
};

constexpr uint8_t SENSOR_RSP_FLAG_OK             = 0x01;
constexpr uint8_t SENSOR_RSP_FLAG_UNKNOWN        = 0x02;
constexpr uint8_t SENSOR_RSP_FLAG_SCAN_READY     = 0x04;
constexpr uint8_t SENSOR_RSP_ROLE_MASK_SHIFT     = 3; // bits 3..6 = role 0..3 known/ok

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
    uint8_t  alarmPacked;     // bit0: active, bit1..3: AlarmReason, bit4: LOOP_FAULT, bit5: SENSOR_UNKNOWN
    uint8_t  cmdSeq;          // Utoljára feldolgozott parancs cmdSeq (1..255, 0 ha nincs)
    uint8_t  syncFlags;       // bit0: ACK, bit1: STALE, bit2: SERVICE_MODE, bit3: CMD_ACK, bit4: PUMP_ON, bit5: CMD_REJECTED_SERVICE, bit6: RELAY_FB_ON, bit7: RELAY_MISMATCH
    uint16_t syncEpochLow16;  // Utolsó kapott timeSyncEpoch alsó 16 bitje nyugtázáshoz (2 byte)
};

struct __attribute__((packed)) SensorScanEntryPacked {
    uint8_t address[8];
    int16_t tempDeci; // 0.1 °C egységben (-1270 = érvénytelen)
};

struct __attribute__((packed)) SensorCmdPayload {
    uint8_t  subCmd;       // SensorSubCmd
    uint8_t  cmdSeq;       // Parancs sorszám / hash
    uint8_t  roleOrIndex;  // Szerep (0..3) vagy kezdőindex
    uint8_t  address[8];   // DS18B20 8 byte-os ROM cím
    uint32_t seq;          // Master kérés sorszám
    uint8_t  reserved[16];
};

struct __attribute__((packed)) SensorRspPayload {
    uint32_t              seq;
    uint8_t               subCmd;
    uint8_t               cmdSeq;
    uint8_t               statusFlags;    // bit0=ok, bit1=sensorUnknown, bit2=scanReady, bits3..6=roleKnownMask
    uint8_t               totalFound;
    uint8_t               startIndex;
    SensorScanEntryPacked entries[2];
    uint8_t               validEntryMask; // bit0=entry0 valid, bit1=entry1 valid
    uint8_t               reserved;
};

struct __attribute__((packed)) NrfPacket {
    uint8_t packetType; // PKT_TYPE_REQUEST (0xA1), PKT_TYPE_RESPONSE (0xB2), PKT_TYPE_SENSOR_CMD (0xA3), PKT_TYPE_SENSOR_RSP (0xB4)
    union {
        EspToNanoPayload esp;
        NanoToEspPayload nano;
        SensorCmdPayload sensorCmd;
        SensorRspPayload sensorRsp;
    };
};

static_assert(sizeof(PumpConfigPacked)      == 10, "PumpConfigPacked merete 10 byte kell legyen!");
static_assert(sizeof(AlarmConfigPacked)     == 5,  "AlarmConfigPacked merete 5 byte kell legyen!");
static_assert(sizeof(AlarmStatePacked)      == 2,  "AlarmStatePacked merete 2 byte kell legyen!");
static_assert(sizeof(EspToNanoPayload)      == 31, "EspToNanoPayload merete 31 byte kell legyen!");
static_assert(sizeof(NanoToEspPayload)      == 31, "NanoToEspPayload merete 31 byte kell legyen!");
static_assert(sizeof(SensorScanEntryPacked) == 10, "SensorScanEntryPacked merete 10 byte kell legyen!");
static_assert(sizeof(SensorCmdPayload)      == 31, "SensorCmdPayload merete 31 byte kell legyen!");
static_assert(sizeof(SensorRspPayload)      == 31, "SensorRspPayload merete 31 byte kell legyen!");
static_assert(sizeof(NrfPacket)             == 32, "NrfPacket meretenek pontosan 32 byte-nak kell lennie!");

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

inline const char* sensorRoleToKey(uint8_t role) {
    switch (role) {
        case SENSOR_ROLE_ELOREMENO:  return "eloremeno";
        case SENSOR_ROLE_VISSZATERO: return "visszatero";
        case SENSOR_ROLE_KAZAN:      return "kazan";
        case SENSOR_ROLE_KULTERI:    return "kulteri";
        default:                     return "ismeretlen";
    }
}

inline const char* sensorRoleToLabel(uint8_t role) {
    switch (role) {
        case SENSOR_ROLE_ELOREMENO:  return "Eloremeno";
        case SENSOR_ROLE_VISSZATERO: return "Visszatero";
        case SENSOR_ROLE_KAZAN:      return "Kazan";
        case SENSOR_ROLE_KULTERI:    return "Kulteri";
        default:                     return "Ismeretlen";
    }
}

constexpr uint8_t DS18B20_FAMILY_CODE = 0x28U;

inline bool isDeviceAddressNonZero(const uint8_t addr[8]) {
    if (!addr) return false;
    uint8_t acc = 0;
    for (uint8_t i = 0; i < 8; i++) {
        acc |= addr[i];
    }
    return acc != 0;
}

inline uint8_t ds18b20Crc8(const uint8_t* data, uint8_t len) {
    if (!data) return 0U;
    uint8_t crc = 0U;
    for (uint8_t i = 0; i < len; i++) {
        uint8_t inbyte = data[i];
        for (uint8_t j = 0; j < 8U; j++) {
            uint8_t mix = static_cast<uint8_t>((crc ^ inbyte) & 0x01U);
            crc >>= 1;
            if (mix != 0U) {
                crc ^= 0x8CU;
            }
            inbyte >>= 1;
        }
    }
    return crc;
}

inline bool isValidDs18b20Rom(const uint8_t addr[8]) {
    if (!addr) return false;
    if (!isDeviceAddressNonZero(addr)) return false;
    if (addr[0] != DS18B20_FAMILY_CODE) return false;
    return ds18b20Crc8(addr, 7U) == addr[7];
}

inline char hexNibbleUpper(uint8_t v) {
    uint8_t n = static_cast<uint8_t>(v & 0x0FU);
    return (n < 10U) ? static_cast<char>('0' + n) : static_cast<char>('A' + (n - 10U));
}

inline int8_t hexCharVal(char c) {
    if (c >= '0' && c <= '9') return static_cast<int8_t>(c - '0');
    if (c >= 'A' && c <= 'F') return static_cast<int8_t>(c - 'A' + 10);
    if (c >= 'a' && c <= 'f') return static_cast<int8_t>(c - 'a' + 10);
    return -1;
}

inline void formatDeviceAddressHex(const uint8_t addr[8], char* out, unsigned int outLen) {
    if (!out || outLen < 24U) {
        if (out && outLen > 0U) out[0] = '\0';
        return;
    }
    unsigned int pos = 0;
    for (uint8_t i = 0; i < 8; i++) {
        out[pos++] = hexNibbleUpper(addr[i] >> 4);
        out[pos++] = hexNibbleUpper(addr[i] & 0x0FU);
        if (i < 7U) {
            out[pos++] = ':';
        }
    }
    out[pos] = '\0';
}

inline void formatDeviceAddressShort(const uint8_t addr[8], char* out, unsigned int outLen) {
    if (!out || outLen < 12U) {
        if (out && outLen > 0U) out[0] = '\0';
        return;
    }
    // "28:FF:9A:BC" (első 2 byte + utolsó 2 byte)
    out[0]  = hexNibbleUpper(addr[0] >> 4);
    out[1]  = hexNibbleUpper(addr[0] & 0x0FU);
    out[2]  = ':';
    out[3]  = hexNibbleUpper(addr[1] >> 4);
    out[4]  = hexNibbleUpper(addr[1] & 0x0FU);
    out[5]  = ':';
    out[6]  = hexNibbleUpper(addr[6] >> 4);
    out[7]  = hexNibbleUpper(addr[6] & 0x0FU);
    out[8]  = ':';
    out[9]  = hexNibbleUpper(addr[7] >> 4);
    out[10] = hexNibbleUpper(addr[7] & 0x0FU);
    out[11] = '\0';
}

inline bool parseDeviceAddressHex(const char* str, uint8_t out[8]) {
    if (!str || !out) return false;
    uint8_t tmp[8] = { 0 };
    unsigned int idx = 0;
    for (uint8_t b = 0; b < 8; b++) {
        int8_t hi = hexCharVal(str[idx++]);
        int8_t lo = hexCharVal(str[idx++]);
        if (hi < 0 || lo < 0) return false;
        tmp[b] = static_cast<uint8_t>((hi << 4) | lo);
        if (b < 7U) {
            if (str[idx] != ':' && str[idx] != '-') return false;
            idx++;
        }
    }
    if (str[idx] != '\0') return false;
    if (!isValidDs18b20Rom(tmp)) return false;
    for (uint8_t b = 0; b < 8; b++) {
        out[b] = tmp[b];
    }
    return true;
}
