#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../nano/src/common.h"
#include "../nano/src/sensor_store.h"
#include "../nano/src/safety_logic.h"

namespace {

void makeValidDs18b20Rom(uint8_t serial1, uint8_t serial2, uint8_t serial6, uint8_t out[8]) {
    out[0] = DS18B20_FAMILY_CODE;
    out[1] = serial1;
    out[2] = serial2;
    out[3] = 0x11;
    out[4] = 0x22;
    out[5] = 0x33;
    out[6] = serial6;
    out[7] = ds18b20Crc8(out, 7U);
}

struct MockEeprom {
    uint8_t mem[1024];
    bool    failWriteAll;
    int     failWriteAtAddr;       // -1 = disabled
    bool    corruptReadbackAll;
    int     corruptReadbackAtAddr; // -1 = disabled
    int     writeCount;

    void reset() {
        memset(mem, 0xFF, sizeof(mem));
        failWriteAll          = false;
        failWriteAtAddr       = -1;
        corruptReadbackAll    = false;
        corruptReadbackAtAddr = -1;
        writeCount            = 0;
    }

    static bool readBlock(void* ctx, int addr, uint8_t* out, size_t len) {
        MockEeprom* self = static_cast<MockEeprom*>(ctx);
        if (!self || !out || addr < 0 || static_cast<size_t>(addr) + len > sizeof(self->mem)) {
            return false;
        }
        memcpy(out, &self->mem[addr], len);
        if (self->corruptReadbackAll || self->corruptReadbackAtAddr == addr) {
            out[4] ^= 0x5AU; // Visszaolvasási hiba szimulálása
        }
        return true;
    }

    static bool writeBlock(void* ctx, int addr, const uint8_t* data, size_t len) {
        MockEeprom* self = static_cast<MockEeprom*>(ctx);
        if (!self || !data || addr < 0 || static_cast<size_t>(addr) + len > sizeof(self->mem)) {
            return false;
        }
        self->writeCount++;
        if (self->failWriteAll || self->failWriteAtAddr == addr) {
            return false;
        }
        memcpy(&self->mem[addr], data, len);
        return true;
    }

    EepromStorageOps ops() {
        EepromStorageOps o = { this, MockEeprom::readBlock, MockEeprom::writeBlock };
        return o;
    }
};

struct MockBus {
    uint8_t addrs[MAX_DISCOVERED_SENSORS][8];
    uint8_t count;
    bool    scanOk;
    int     scanCalls;

    void reset() {
        memset(addrs, 0, sizeof(addrs));
        count     = 0;
        scanOk    = true;
        scanCalls = 0;
    }

    void add(const uint8_t a[8]) {
        if (count < MAX_DISCOVERED_SENSORS) {
            memcpy(addrs[count], a, 8);
            count++;
        }
    }

    static bool scan(void* ctx, uint8_t outAddrs[MAX_DISCOVERED_SENSORS][8], uint8_t* outCount) {
        MockBus* self = static_cast<MockBus*>(ctx);
        if (!self || !self->scanOk) return false;
        self->scanCalls++;
        *outCount = self->count;
        for (uint8_t i = 0; i < self->count; i++) {
            memcpy(outAddrs[i], self->addrs[i], 8);
        }
        return true;
    }
};

void testRomValidation() {
    uint8_t romValid[8];
    makeValidDs18b20Rom(0xAA, 0xBB, 0x01, romValid);
    assert(isValidDs18b20Rom(romValid));

    // Hex parsing with colons, dashes, lowercase
    char hexColon[24];
    formatDeviceAddressHex(romValid, hexColon, sizeof(hexColon));
    uint8_t parsed[8] = { 0 };
    assert(parseDeviceAddressHex(hexColon, parsed));
    assert(memcmp(parsed, romValid, 8) == 0);

    // Lowercase and dashes
    char lowerDash[24];
    memcpy(lowerDash, hexColon, sizeof(hexColon));
    for (size_t i = 0; lowerDash[i] != '\0'; i++) {
        if (lowerDash[i] == ':') lowerDash[i] = '-';
        if (lowerDash[i] >= 'A' && lowerDash[i] <= 'F') {
            lowerDash[i] = static_cast<char>(lowerDash[i] - 'A' + 'a');
        }
    }
    memset(parsed, 0, sizeof(parsed));
    assert(parseDeviceAddressHex(lowerDash, parsed));
    assert(memcmp(parsed, romValid, 8) == 0);

    // Rossz CRC8 elutasítása (és `out` ne szennyeződjön)
    uint8_t romBadCrc[8];
    memcpy(romBadCrc, romValid, 8);
    romBadCrc[7] ^= 0x01U;
    assert(!isValidDs18b20Rom(romBadCrc));
    char badCrcHex[24];
    formatDeviceAddressHex(romBadCrc, badCrcHex, sizeof(badCrcHex));
    uint8_t sentinel[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    assert(!parseDeviceAddressHex(badCrcHex, sentinel));
    assert(sentinel[0] == 1 && sentinel[7] == 8);

    // Nem 0x28 family code (pl. 0x10 DS18S20), érvényes CRC8-al is elutasítandó!
    uint8_t wrongFamily[8] = { 0x10, 0xAA, 0xBB, 0x11, 0x22, 0x33, 0x01, 0x00 };
    wrongFamily[7] = ds18b20Crc8(wrongFamily, 7U);
    assert(!isValidDs18b20Rom(wrongFamily));
    char wrongFamilyHex[24];
    formatDeviceAddressHex(wrongFamily, wrongFamilyHex, sizeof(wrongFamilyHex));
    assert(!parseDeviceAddressHex(wrongFamilyHex, sentinel));

    // Csupa nulla vagy rossz formátum
    uint8_t zeroAddr[8] = { 0 };
    assert(!isValidDs18b20Rom(zeroAddr));
    assert(!parseDeviceAddressHex("00:00:00:00:00:00:00:00", sentinel));
    assert(!parseDeviceAddressHex("28:AA:BB:11:22:33:01", sentinel));
    assert(!parseDeviceAddressHex("28:AA:BB:11:22:33:01:GG", sentinel));

    printf("[PASS] testRomValidation\n");
}

void testFreshBusAndTransactionalPersistence() {
    uint8_t r0[8], r1[8], r2[8], r3[8], rMissing[8];
    makeValidDs18b20Rom(0x10, 0x01, 0x01, r0);
    makeValidDs18b20Rom(0x10, 0x02, 0x02, r1);
    makeValidDs18b20Rom(0x10, 0x03, 0x03, r2);
    makeValidDs18b20Rom(0x10, 0x04, 0x04, r3);
    makeValidDs18b20Rom(0x10, 0x99, 0x99, rMissing);

    MockEeprom eeprom;
    eeprom.reset();
    MockBus bus;
    bus.reset();
    bus.add(r0);
    bus.add(r1);
    bus.add(r2);
    bus.add(r3);

    uint8_t liveAddrs[SENSOR_ROLE_COUNT][8];
    memset(liveAddrs, 0, sizeof(liveAddrs));
    uint8_t liveMask = 0;

    // 1. Hiányzó ROM elutasítása (akkor is, ha formailag + CRC-ben valid 0x28!)
    SensorLearnStatus st = learnSensorRoleTransactional(
        SENSOR_ROLE_KAZAN, rMissing, true, MockBus::scan, &bus, eeprom.ops(), liveAddrs, liveMask
    );
    assert(st == LEARN_ERR_NOT_ON_BUS);
    assert(liveMask == 0);
    assert(eeprom.writeCount == 0);

    // 2. Rossz family code vagy rossz CRC elutasítása még buszkeresés előtt
    uint8_t badFamily[8] = { 0x22, 0x10, 0x01, 0x11, 0x22, 0x33, 0x01, 0x00 };
    badFamily[7] = ds18b20Crc8(badFamily, 7U);
    int scansBefore = bus.scanCalls;
    st = learnSensorRoleTransactional(
        SENSOR_ROLE_KAZAN, badFamily, true, MockBus::scan, &bus, eeprom.ops(), liveAddrs, liveMask
    );
    assert(st == LEARN_ERR_INVALID_ROM);
    assert(bus.scanCalls == scansBefore);
    assert(liveMask == 0);

    // 3. Sikeres tanítás mind a 4 szerepkörre
    assert(learnSensorRoleTransactional(SENSOR_ROLE_ELOREMENO,  r0, true, MockBus::scan, &bus, eeprom.ops(), liveAddrs, liveMask) == LEARN_OK);
    assert(learnSensorRoleTransactional(SENSOR_ROLE_VISSZATERO, r1, true, MockBus::scan, &bus, eeprom.ops(), liveAddrs, liveMask) == LEARN_OK);
    assert(learnSensorRoleTransactional(SENSOR_ROLE_KAZAN,      r2, true, MockBus::scan, &bus, eeprom.ops(), liveAddrs, liveMask) == LEARN_OK);
    assert(learnSensorRoleTransactional(SENSOR_ROLE_KULTERI,    r3, true, MockBus::scan, &bus, eeprom.ops(), liveAddrs, liveMask) == LEARN_OK);
    assert(liveMask == 0x0FU);

    // Újraindítás szimulálása: betöltés EEPROM-ból
    uint8_t reloadedAddrs[SENSOR_ROLE_COUNT][8];
    uint8_t reloadedMask = 0;
    bool recovered = false;
    assert(loadSensorStore(eeprom.ops(), reloadedAddrs, reloadedMask, &recovered));
    assert(!recovered);
    assert(reloadedMask == 0x0FU);
    assert(memcmp(reloadedAddrs[SENSOR_ROLE_KAZAN], r2, 8) == 0);

    // 4. Elavult cache elleni védelem: eltávolítjuk r2-t a friss buszról -> friss scan nélkül sem fogadható el
    bus.reset();
    bus.add(r0);
    bus.add(r1);
    bus.add(r3);
    st = learnSensorRoleTransactional(
        SENSOR_ROLE_ELOREMENO, r2, true, MockBus::scan, &bus, eeprom.ops(), liveAddrs, liveMask
    );
    assert(st == LEARN_ERR_NOT_ON_BUS);
    assert(memcmp(liveAddrs[SENSOR_ROLE_ELOREMENO], r0, 8) == 0);
    assert(memcmp(liveAddrs[SENSOR_ROLE_KAZAN],     r2, 8) == 0);

    // Visszaállítjuk mind a 4 szenzort a buszra
    bus.add(r2);

    // 5. allowReassign=false esetén ütközés elutasítása RAM/EEPROM módosítás nélkül
    st = learnSensorRoleTransactional(
        SENSOR_ROLE_ELOREMENO, r2, false, MockBus::scan, &bus, eeprom.ops(), liveAddrs, liveMask
    );
    assert(st == LEARN_ERR_DUPLICATE_ROLE);
    assert(memcmp(liveAddrs[SENSOR_ROLE_ELOREMENO], r0, 8) == 0);
    assert(memcmp(liveAddrs[SENSOR_ROLE_KAZAN],     r2, 8) == 0);

    // 6. Phase 1 EEPROM írási hiba (backup 64) allowReassign=true közben:
    //    RÉGI RAM és Primary EEPROM sértetlen marad!
    eeprom.failWriteAtAddr = SENSOR_EEPROM_BACKUP_ADDR;
    int8_t cleared = 99;
    st = learnSensorRoleTransactional(
        SENSOR_ROLE_ELOREMENO, r2, true, MockBus::scan, &bus, eeprom.ops(), liveAddrs, liveMask, &cleared
    );
    assert(st == LEARN_ERR_EEPROM_WRITE);
    assert(cleared == -1);
    assert(liveMask == 0x0FU);
    assert(memcmp(liveAddrs[SENSOR_ROLE_ELOREMENO], r0, 8) == 0);
    assert(memcmp(liveAddrs[SENSOR_ROLE_KAZAN],     r2, 8) == 0);
    assert(loadSensorStore(eeprom.ops(), reloadedAddrs, reloadedMask));
    assert(reloadedMask == 0x0FU);
    assert(memcmp(reloadedAddrs[SENSOR_ROLE_ELOREMENO], r0, 8) == 0);
    assert(memcmp(reloadedAddrs[SENSOR_ROLE_KAZAN],     r2, 8) == 0);
    eeprom.failWriteAtAddr = -1;

    // 7. Phase 2 visszaolvasási hiba (Primary 0): rollback -> régi EEPROM + régi RAM érintetlen!
    eeprom.corruptReadbackAtAddr = SENSOR_EEPROM_ADDR;
    st = learnSensorRoleTransactional(
        SENSOR_ROLE_ELOREMENO, r2, true, MockBus::scan, &bus, eeprom.ops(), liveAddrs, liveMask, &cleared
    );
    assert(st == LEARN_ERR_EEPROM_VERIFY);
    assert(cleared == -1);
    assert(liveMask == 0x0FU);
    assert(memcmp(liveAddrs[SENSOR_ROLE_ELOREMENO], r0, 8) == 0);
    assert(memcmp(liveAddrs[SENSOR_ROLE_KAZAN],     r2, 8) == 0);
    eeprom.corruptReadbackAtAddr = -1;
    assert(loadSensorStore(eeprom.ops(), reloadedAddrs, reloadedMask));
    assert(reloadedMask == 0x0FU);
    assert(memcmp(reloadedAddrs[SENSOR_ROLE_ELOREMENO], r0, 8) == 0);
    assert(memcmp(reloadedAddrs[SENSOR_ROLE_KAZAN],     r2, 8) == 0);

    // 8. Phase 2 írási hiba (Primary 0): rollback -> mindkét slot + élő RAM őrzi a régit!
    eeprom.failWriteAtAddr = SENSOR_EEPROM_ADDR;
    st = learnSensorRoleTransactional(
        SENSOR_ROLE_ELOREMENO, r2, true, MockBus::scan, &bus, eeprom.ops(), liveAddrs, liveMask, &cleared
    );
    assert(st == LEARN_ERR_EEPROM_WRITE);
    eeprom.failWriteAtAddr = -1;
    assert(liveMask == 0x0FU);
    assert(memcmp(liveAddrs[SENSOR_ROLE_ELOREMENO], r0, 8) == 0);
    assert(memcmp(liveAddrs[SENSOR_ROLE_KAZAN],     r2, 8) == 0);
    assert(loadSensorStore(eeprom.ops(), reloadedAddrs, reloadedMask));
    assert(reloadedMask == 0x0FU);
    assert(memcmp(reloadedAddrs[SENSOR_ROLE_ELOREMENO], r0, 8) == 0);

    // 9. Sikeres allowReassign=true áthelyezés: r2 áthelyezése Kazan -> Eloremeno
    st = learnSensorRoleTransactional(
        SENSOR_ROLE_ELOREMENO, r2, true, MockBus::scan, &bus, eeprom.ops(), liveAddrs, liveMask, &cleared
    );
    assert(st == LEARN_OK);
    assert(cleared == SENSOR_ROLE_KAZAN);
    assert((liveMask & (1U << SENSOR_ROLE_KAZAN)) == 0);
    assert((liveMask & (1U << SENSOR_ROLE_ELOREMENO)) != 0);
    assert(memcmp(liveAddrs[SENSOR_ROLE_ELOREMENO], r2, 8) == 0);

    // 10. Áramszünet / törött írás Phase 2 közben (Primary 0 sérült, Backup 64 érvényes):
    //     következő booton a Backup slotból önhelyreáll!
    eeprom.mem[SENSOR_EEPROM_ADDR + 10] ^= 0xFFU;
    assert(loadSensorStore(eeprom.ops(), reloadedAddrs, reloadedMask, &recovered));
    assert(recovered);
    assert(memcmp(reloadedAddrs[SENSOR_ROLE_ELOREMENO], r2, 8) == 0);
    assert((reloadedMask & (1U << SENSOR_ROLE_KAZAN)) == 0);
    // Önhelyreállítás után a Primary is javítva van:
    assert(loadSensorStore(eeprom.ops(), reloadedAddrs, reloadedMask, &recovered));
    assert(!recovered);

    // 10b. Áramszünet Phase 1 közben (Backup 64 sérült, Primary 0 ép):
    //      Primary-ból indul, és automatikusan visszaállítja a Backup slotot is.
    eeprom.mem[SENSOR_EEPROM_BACKUP_ADDR + 8] ^= 0xFFU;
    assert(loadSensorStore(eeprom.ops(), reloadedAddrs, reloadedMask, &recovered));
    assert(!recovered);
    assert(memcmp(reloadedAddrs[SENSOR_ROLE_ELOREMENO], r2, 8) == 0);
    SensorEepromStore healedBackup{};
    assert(readAndVerifyStoreSlot(eeprom.ops(), SENSOR_EEPROM_BACKUP_ADDR, healedBackup));

    // 11. Manipulált duplikált ROM az EEPROM-ban elutasítandó
    SensorEepromStore badDup = buildSensorEepromStore(reloadedMask, reloadedAddrs);
    badDup.assignedMask |= static_cast<uint8_t>(1U << SENSOR_ROLE_KAZAN);
    memcpy(badDup.addr[SENSOR_ROLE_KAZAN], badDup.addr[SENSOR_ROLE_ELOREMENO], 8);
    badDup.checksum = computeEepromChecksum(badDup);
    assert(!isValidSensorEepromStore(badDup));

    printf("[PASS] testFreshBusAndTransactionalPersistence\n");
}

void testSafetyAndRelayRegressions() {
    AlarmConfigPacked alarmCfg{ 85.0f, 1U };

    // 1. Kazán szenzor hiba (-127.0f) -> szivattyú KÉNYSZERÍTETT BE akkor is, ha ESP kikapcsolná és alarmEnabled=0
    AlarmConfigPacked alarmDisabled{ 85.0f, 0U };
    AlarmStatePacked aState = evaluateNanoAlarmState(-127.0f, true, true, false, alarmCfg);
    assert(aState.active == 1U);
    assert(aState.reason == ALARM_REASON_SENSOR_FAIL);
    uint8_t outMask = computeNanoEffectiveOutputs(
        -127.0f, true, false, 0x00U, false, false, false, alarmDisabled, aState, false
    );
    assert(getBit(outMask, OUT_BIT_SZIVATTYU));

    // 2. Fix 95.0C túlmelegedés védelem: alarmEnabled=0 mellett is BEkapcsolja a szivattyút!
    AlarmStatePacked aDis = evaluateNanoAlarmState(95.0f, false, true, false, alarmDisabled);
    assert(aDis.active == 0U);
    outMask = computeNanoEffectiveOutputs(
        95.0f, true, false, 0x00U, false, false, false, alarmDisabled, aDis, false
    );
    assert(getBit(outMask, OUT_BIT_SZIVATTYU));

    // 3. Riasztás helyreáll, ha a kazán szenzor visszaáll érvényes értékre és sensorUnknown=false
    aState = evaluateNanoAlarmState(62.5f, false, true, false, alarmCfg);
    assert(aState.active == 0U);
    assert(aState.reason == ALARM_REASON_NONE);
    outMask = computeNanoEffectiveOutputs(
        62.5f, true, false, 0x00U, false, false, false, alarmCfg, aState, false
    );
    assert(!getBit(outMask, OUT_BIT_SZIVATTYU));

    // 4. Másik szerep kiesése (sensorUnknown=true), érvényes kazán=60.0f mellett:
    //    riasztás SENSOR_FAIL, de a szivattyú parancsot az ESP adja (nem erőlteti BE)
    aState = evaluateNanoAlarmState(60.0f, true, true, false, alarmCfg);
    assert(aState.active == 1U);
    assert(aState.reason == ALARM_REASON_SENSOR_FAIL);
    outMask = computeNanoEffectiveOutputs(
        60.0f, true, false, 0x00U, false, false, false, alarmCfg, aState, false
    );
    assert(!getBit(outMask, OUT_BIT_SZIVATTYU));

    // 5. Szerviz kapcsoló (D6) kényszerített szivattyú BE + zöld LED BE
    outMask = computeNanoEffectiveOutputs(
        25.0f, true, false, 0x00U, true, false, false, alarmCfg, aState, false
    );
    assert(getBit(outMask, OUT_BIT_SZIVATTYU));
    assert(getBit(outMask, OUT_BIT_LED_ZOLD));

    // 6. NRF fail-safe: szivattyú BE + NRF_FAIL
    aState = evaluateNanoAlarmState(50.0f, false, true, true, alarmCfg);
    assert(aState.active == 1U);
    assert(aState.reason == ALARM_REASON_NRF_FAIL);
    outMask = computeNanoEffectiveOutputs(
        50.0f, true, true, 0x00U, false, false, false, alarmCfg, aState, false
    );
    assert(getBit(outMask, OUT_BIT_SZIVATTYU));

    // 7. Relé visszacsatolás (D7) mismatch + millis() 49 napos wraparound + CMD_ACK regresszió
    RelayMismatchState rms{};
    rms.pendingCmdVerifyActive = true;
    uint32_t t0 = 0xFFFFFF00UL;
    assert(stepRelayMismatch(rms, true, false, t0) == RELAY_EVT_NONE);
    assert(!rms.relayMismatchActive);
    // 400ms eltelt (wraparound után is < 500ms) -> még nem hiba
    assert(stepRelayMismatch(rms, true, false, t0 + 400UL) == RELAY_EVT_NONE);
    assert(!rms.relayMismatchActive);
    // 500ms eltelt -> RELAY_EVT_STUCK_OFF + mismatch
    assert(stepRelayMismatch(rms, true, false, t0 + 500UL) == RELAY_EVT_STUCK_OFF);
    assert(rms.relayMismatchActive);
    assert(rms.lastCmdMismatch);
    assert(!rms.lastCmdAck);
    // Következő parancsnál egyező D7 visszacsatolás -> CMD_ACK OK
    rms.pendingCmdVerifyActive = true;
    assert(stepRelayMismatch(rms, true, true, t0 + 600UL) == RELAY_EVT_NONE);
    assert(!rms.relayMismatchActive);
    assert(rms.lastCmdAck);
    assert(!rms.lastCmdMismatch);

    printf("[PASS] testSafetyAndRelayRegressions\n");
}

} // namespace

int main() {
    testRomValidation();
    testFreshBusAndTransactionalPersistence();
    testSafetyAndRelayRegressions();
    printf("MINDEN C++ TESZT SIKERES.\n");
    return 0;
}
