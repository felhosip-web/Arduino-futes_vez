#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "common.h"

constexpr uint16_t SENSOR_EEPROM_MAGIC       = 0x4653U; // "FS"
constexpr uint8_t  SENSOR_EEPROM_VERSION     = 1U;
constexpr int      SENSOR_EEPROM_ADDR        = 0;       // Elsődleges slot (visszafelé kompatibilis elrendezés)
constexpr int      SENSOR_EEPROM_BACKUP_ADDR = 64;      // Másodlagos / tranzakciós árnyék slot (ATmega328P 1024B EEPROM)

struct __attribute__((packed)) SensorEepromStore {
    uint16_t magic;
    uint8_t  version;
    uint8_t  assignedMask; // bit0..3 = Eloremeno, Visszatero, Kazan, Kulteri
    uint8_t  addr[SENSOR_ROLE_COUNT][8];
    uint8_t  checksum;
};

static_assert(sizeof(SensorEepromStore) == 37, "SensorEepromStore merete 37 byte!");

enum SensorLearnStatus : uint8_t {
    LEARN_OK = 0,
    LEARN_ERR_INVALID_ROLE,
    LEARN_ERR_INVALID_ROM,
    LEARN_ERR_SCAN_FAILED,
    LEARN_ERR_NOT_ON_BUS,
    LEARN_ERR_DUPLICATE_ROLE,
    LEARN_ERR_EEPROM_WRITE,
    LEARN_ERR_EEPROM_VERIFY
};

struct EepromStorageOps {
    void* ctx;
    bool (*readBlock)(void* ctx, int addr, uint8_t* out, size_t len);
    bool (*writeBlock)(void* ctx, int addr, const uint8_t* data, size_t len);
};

typedef bool (*SensorBusScanFn)(void* ctx, uint8_t outAddrs[MAX_DISCOVERED_SENSORS][8], uint8_t* outCount);

inline uint8_t computeEepromChecksum(const SensorEepromStore& st) {
    const uint8_t* raw = reinterpret_cast<const uint8_t*>(&st);
    uint8_t sum = 0x5AU;
    // Az utolsó checksum byte kivételével minden byte-on végigmegyünk
    for (size_t i = 0; i < sizeof(SensorEepromStore) - 1U; i++) {
        sum = static_cast<uint8_t>(sum ^ raw[i]);
        sum = static_cast<uint8_t>(sum + static_cast<uint8_t>(raw[i] * 3U));
    }
    return sum;
}

inline bool isValidSensorEepromStore(const SensorEepromStore& store) {
    if (store.magic != SENSOR_EEPROM_MAGIC ||
        store.version != SENSOR_EEPROM_VERSION ||
        store.checksum != computeEepromChecksum(store)) {
        return false;
    }
    if ((store.assignedMask & 0xF0U) != 0U) {
        return false;
    }
    for (uint8_t r = 0; r < SENSOR_ROLE_COUNT; r++) {
        if ((store.assignedMask & static_cast<uint8_t>(1U << r)) != 0U) {
            if (!isValidDs18b20Rom(store.addr[r])) {
                return false;
            }
            for (uint8_t r2 = static_cast<uint8_t>(r + 1U); r2 < SENSOR_ROLE_COUNT; r2++) {
                if ((store.assignedMask & static_cast<uint8_t>(1U << r2)) != 0U &&
                    memcmp(store.addr[r], store.addr[r2], 8) == 0) {
                    return false;
                }
            }
        }
    }
    return true;
}

inline SensorEepromStore buildSensorEepromStore(
    uint8_t assignedMask,
    const uint8_t addrs[SENSOR_ROLE_COUNT][8]
) {
    SensorEepromStore store;
    memset(&store, 0, sizeof(store));
    store.magic        = SENSOR_EEPROM_MAGIC;
    store.version      = SENSOR_EEPROM_VERSION;
    store.assignedMask = static_cast<uint8_t>(assignedMask & 0x0FU);
    for (uint8_t r = 0; r < SENSOR_ROLE_COUNT; r++) {
        if ((store.assignedMask & static_cast<uint8_t>(1U << r)) != 0U &&
            isValidDs18b20Rom(addrs[r])) {
            memcpy(store.addr[r], addrs[r], 8);
        } else {
            store.assignedMask &= static_cast<uint8_t>(~(1U << r));
            memset(store.addr[r], 0, 8);
        }
    }
    store.checksum = computeEepromChecksum(store);
    return store;
}

inline bool readAndVerifyStoreSlot(
    const EepromStorageOps& eeprom,
    int slotAddr,
    SensorEepromStore& outStore
) {
    if (!eeprom.readBlock) return false;
    if (!eeprom.readBlock(eeprom.ctx, slotAddr, reinterpret_cast<uint8_t*>(&outStore), sizeof(SensorEepromStore))) {
        memset(&outStore, 0, sizeof(outStore));
        return false;
    }
    return isValidSensorEepromStore(outStore);
}

inline bool writeAndVerifyStoreSlot(
    const EepromStorageOps& eeprom,
    int slotAddr,
    const SensorEepromStore& store,
    SensorLearnStatus* outErr = nullptr
) {
    if (!eeprom.writeBlock || !eeprom.readBlock) {
        if (outErr) *outErr = LEARN_ERR_EEPROM_WRITE;
        return false;
    }
    if (!eeprom.writeBlock(eeprom.ctx, slotAddr, reinterpret_cast<const uint8_t*>(&store), sizeof(SensorEepromStore))) {
        if (outErr) *outErr = LEARN_ERR_EEPROM_WRITE;
        return false;
    }
    SensorEepromStore verify;
    memset(&verify, 0, sizeof(verify));
    if (!eeprom.readBlock(eeprom.ctx, slotAddr, reinterpret_cast<uint8_t*>(&verify), sizeof(SensorEepromStore)) ||
        memcmp(&store, &verify, sizeof(SensorEepromStore)) != 0 ||
        !isValidSensorEepromStore(verify)) {
        if (outErr) *outErr = LEARN_ERR_EEPROM_VERIFY;
        return false;
    }
    if (outErr) *outErr = LEARN_OK;
    return true;
}

inline void invalidateEepromSlot(const EepromStorageOps& eeprom, int slotAddr) {
    if (!eeprom.writeBlock) return;
    SensorEepromStore blank;
    memset(&blank, 0, sizeof(blank));
    (void)eeprom.writeBlock(eeprom.ctx, slotAddr, reinterpret_cast<const uint8_t*>(&blank), sizeof(SensorEepromStore));
}

inline bool loadSensorStore(
    const EepromStorageOps& eeprom,
    uint8_t outAddrs[SENSOR_ROLE_COUNT][8],
    uint8_t& outAssignedMask,
    bool* outRecoveredFromBackup = nullptr
) {
    if (outRecoveredFromBackup) {
        *outRecoveredFromBackup = false;
    }
    memset(outAddrs, 0, sizeof(uint8_t) * SENSOR_ROLE_COUNT * 8U);
    outAssignedMask = 0U;

    SensorEepromStore primary;
    SensorEepromStore backup;
    bool primaryValid = readAndVerifyStoreSlot(eeprom, SENSOR_EEPROM_ADDR, primary);
    bool backupValid  = readAndVerifyStoreSlot(eeprom, SENSOR_EEPROM_BACKUP_ADDR, backup);

    const SensorEepromStore* active = nullptr;
    if (primaryValid) {
        active = &primary;
        // Ha a backup slot még üres (régi single-slot EEPROM) vagy sérült, szinkronizáljuk
        if (!backupValid || memcmp(&primary, &backup, sizeof(SensorEepromStore)) != 0) {
            (void)writeAndVerifyStoreSlot(eeprom, SENSOR_EEPROM_BACKUP_ADDR, primary);
        }
    } else if (backupValid) {
        // Áramszünet / szakadás a Phase 2 írás közben -> visszaállítás a hitelesített backup slotból
        active = &backup;
        if (writeAndVerifyStoreSlot(eeprom, SENSOR_EEPROM_ADDR, backup)) {
            if (outRecoveredFromBackup) {
                *outRecoveredFromBackup = true;
            }
        }
    } else {
        return false;
    }

    outAssignedMask = static_cast<uint8_t>(active->assignedMask & 0x0FU);
    for (uint8_t r = 0; r < SENSOR_ROLE_COUNT; r++) {
        if ((outAssignedMask & static_cast<uint8_t>(1U << r)) != 0U &&
            isValidDs18b20Rom(active->addr[r])) {
            memcpy(outAddrs[r], active->addr[r], 8);
        } else {
            outAssignedMask &= static_cast<uint8_t>(~(1U << r));
            memset(outAddrs[r], 0, 8);
        }
    }
    return true;
}

inline SensorLearnStatus learnSensorRoleTransactional(
    uint8_t role,
    const uint8_t newAddr[8],
    bool allowReassign,
    SensorBusScanFn scanBus,
    void* scanCtx,
    const EepromStorageOps& eeprom,
    uint8_t liveAddrs[SENSOR_ROLE_COUNT][8],
    uint8_t& liveAssignedMask,
    int8_t* outClearedOldRole = nullptr,
    uint8_t (*outFreshBusAddrs)[8] = nullptr,
    uint8_t* outFreshBusCount = nullptr
) {
    if (outClearedOldRole) {
        *outClearedOldRole = -1;
    }
    if (role >= SENSOR_ROLE_COUNT) {
        return LEARN_ERR_INVALID_ROLE;
    }
    if (!newAddr || !isValidDs18b20Rom(newAddr)) {
        return LEARN_ERR_INVALID_ROM;
    }
    if (!scanBus) {
        return LEARN_ERR_SCAN_FAILED;
    }

    // 1. Mindig FRISS OneWire buszkeresést végzünk (soha nem támaszkodunk elavult cache-re)
    uint8_t freshBus[MAX_DISCOVERED_SENSORS][8];
    memset(freshBus, 0, sizeof(freshBus));
    uint8_t rawCount = 0;
    if (!scanBus(scanCtx, freshBus, &rawCount)) {
        return LEARN_ERR_SCAN_FAILED;
    }
    if (rawCount > MAX_DISCOVERED_SENSORS) {
        rawCount = MAX_DISCOVERED_SENSORS;
    }

    uint8_t validCount = 0;
    bool presentOnBus  = false;
    for (uint8_t i = 0; i < rawCount; i++) {
        if (!isValidDs18b20Rom(freshBus[i])) {
            continue;
        }
        if (validCount != i) {
            memcpy(freshBus[validCount], freshBus[i], 8);
        }
        if (memcmp(freshBus[validCount], newAddr, 8) == 0) {
            presentOnBus = true;
        }
        validCount++;
    }
    if (outFreshBusAddrs && outFreshBusCount) {
        memset(outFreshBusAddrs, 0, sizeof(uint8_t) * MAX_DISCOVERED_SENSORS * 8U);
        for (uint8_t i = 0; i < validCount; i++) {
            memcpy(outFreshBusAddrs[i], freshBus[i], 8);
        }
        *outFreshBusCount = validCount;
    }
    if (!presentOnBus) {
        return LEARN_ERR_NOT_ON_BUS;
    }

    // 2. Ideiglenes (staged) hozzárendelés felépítése az élő RAM érintése nélkül
    uint8_t stagedAddrs[SENSOR_ROLE_COUNT][8];
    memcpy(stagedAddrs, liveAddrs, sizeof(stagedAddrs));
    uint8_t stagedMask = static_cast<uint8_t>(liveAssignedMask & 0x0FU);
    int8_t  clearedRole = -1;

    for (uint8_t r = 0; r < SENSOR_ROLE_COUNT; r++) {
        if (r != role &&
            (stagedMask & static_cast<uint8_t>(1U << r)) != 0U &&
            memcmp(stagedAddrs[r], newAddr, 8) == 0) {
            if (!allowReassign) {
                return LEARN_ERR_DUPLICATE_ROLE;
            }
            memset(stagedAddrs[r], 0, 8);
            stagedMask &= static_cast<uint8_t>(~(1U << r));
            clearedRole = static_cast<int8_t>(r);
        }
    }

    memcpy(stagedAddrs[role], newAddr, 8);
    stagedMask |= static_cast<uint8_t>(1U << role);

    SensorEepromStore nextStore = buildSensorEepromStore(stagedMask, stagedAddrs);
    if (!isValidSensorEepromStore(nextStore)) {
        return LEARN_ERR_INVALID_ROM;
    }

    // 3. Korábbi érvényes EEPROM állapot mentése visszagörgetéshez
    SensorEepromStore prevStore;
    bool prevValid = readAndVerifyStoreSlot(eeprom, SENSOR_EEPROM_ADDR, prevStore);
    if (!prevValid) {
        prevValid = readAndVerifyStoreSlot(eeprom, SENSOR_EEPROM_BACKUP_ADDR, prevStore);
    }

    // 4. Kétfázisú atomikus EEPROM tranzakció:
    //    Phase 1: Backup slot (64) írás + visszaolvasási ellenőrzés (Primary és élő RAM még érintetlen!)
    SensorLearnStatus writeErr = LEARN_OK;
    if (!writeAndVerifyStoreSlot(eeprom, SENSOR_EEPROM_BACKUP_ADDR, nextStore, &writeErr)) {
        if (prevValid) {
            (void)writeAndVerifyStoreSlot(eeprom, SENSOR_EEPROM_BACKUP_ADDR, prevStore);
        } else {
            invalidateEepromSlot(eeprom, SENSOR_EEPROM_BACKUP_ADDR);
        }
        return writeErr;
    }

    //    Phase 2: Primary slot (0) írás + visszaolvasási ellenőrzés
    //    (Ha Phase 2 közben áramszünet történik, a következő boot a hitelesített Backup slotból helyreáll.)
    //    (Ha Phase 2 írás/visszaolvasás sikertelen, mindkét slotot visszagörgetjük prevStore-ra.)
    if (!writeAndVerifyStoreSlot(eeprom, SENSOR_EEPROM_ADDR, nextStore, &writeErr)) {
        if (prevValid) {
            (void)writeAndVerifyStoreSlot(eeprom, SENSOR_EEPROM_ADDR, prevStore);
            (void)writeAndVerifyStoreSlot(eeprom, SENSOR_EEPROM_BACKUP_ADDR, prevStore);
        } else {
            invalidateEepromSlot(eeprom, SENSOR_EEPROM_ADDR);
            invalidateEepromSlot(eeprom, SENSOR_EEPROM_BACKUP_ADDR);
        }
        return writeErr;
    }

    // 5. Élő RAM CSAK a sikeres és visszaolvasással igazolt EEPROM mentés után frissül!
    memcpy(liveAddrs, stagedAddrs, sizeof(stagedAddrs));
    liveAssignedMask = stagedMask;
    if (outClearedOldRole) {
        *outClearedOldRole = clearedRole;
    }
    return LEARN_OK;
}
