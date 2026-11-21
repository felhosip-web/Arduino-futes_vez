#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#include "../nano/src/common.h"
#include "../esp32/src/nrf_dma_core.h"

namespace {

// ============================================================================
// ESP-IDF SPI2/GDMA ÉS NRF24L01+ HARDVER SZIMULÁTOR (SZIGORÚ SZERZŐDÉSELLENŐRZÉS)
// ============================================================================
struct PendingDmaItem {
    uint8_t slotIdx;
    size_t  totalBytes;
    bool    doneInHw;
};

struct MockSpiNrfHw {
    uint8_t* txBufs[NrfDmaCore::MAX_SLOTS]{};
    uint8_t* rxBufs[NrfDmaCore::MAX_SLOTS]{};

    uint8_t frameTx[NrfDmaCore::MAX_SLOTS][NrfDmaCore::DMA_FRAME_BYTES]{};
    uint8_t frameRx[NrfDmaCore::MAX_SLOTS][NrfDmaCore::DMA_FRAME_BYTES]{};

    NrfPacket dmaTxPkt{};
    NrfPacket dmaRxPkt{};

    volatile bool dmaDoneFlag = false;

    // Mock NRF24 regiszterek és FIFO-k
    uint8_t regs[32]{};
    uint8_t txAddr[5]{};
    uint8_t rxAddrP0[5]{};
    uint8_t rxAddrP1[5]{};
    bool    ceHigh         = false;
    bool    cePulsed       = false;
    uint32_t lastCePulseUs = 0;
    bool    hasTxPayload   = false;
    NrfPacket txFifoPkt{};
    std::vector<NrfPacket> rxFifo;

    // Mock SPI2 queue + hiba injektálás
    uint8_t queueCap = 3;
    std::vector<PendingDmaItem> hwQueue;
    bool autoCompleteOnQueue   = true;
    bool stallCompletions      = false;
    bool forceQueueErrorOnce   = false;
    bool forceSyncFail         = false;
    int  queueErrorLeft        = 0;
    int  syncTransCalls        = 0;
    int  flushTxCalls          = 0;
    int  flushRxCalls          = 0;
    int  rRxPayloadCalls       = 0;
    int  wTxPayloadCalls       = 0;
    uint32_t nowMicros         = 1000UL;
    uint32_t spiApiStepUs      = 5UL;

    void init(NrfDmaCore& core, uint8_t qSize = 3) {
        memset(frameTx, 0, sizeof(frameTx));
        memset(frameRx, 0, sizeof(frameRx));
        memset(&dmaTxPkt, 0, sizeof(dmaTxPkt));
        memset(&dmaRxPkt, 0, sizeof(dmaRxPkt));
        memset(regs, 0, sizeof(regs));
        memset(txAddr, 0, sizeof(txAddr));
        memset(rxAddrP0, 0, sizeof(rxAddrP0));
        memset(rxAddrP1, 0, sizeof(rxAddrP1));
        dmaDoneFlag = false;
        ceHigh = false;
        cePulsed = false;
        lastCePulseUs = 0;
        hasTxPayload = false;
        rxFifo.clear();
        queueCap = qSize;
        hwQueue.clear();
        autoCompleteOnQueue = true;
        stallCompletions = false;
        forceQueueErrorOnce = false;
        forceSyncFail = false;
        queueErrorLeft = 0;
        syncTransCalls = 0;
        flushTxCalls = 0;
        flushRxCalls = 0;
        rRxPayloadCalls = 0;
        wTxPayloadCalls = 0;
        nowMicros = 1000UL;
        spiApiStepUs = 5UL;

        for (uint8_t i = 0; i < NrfDmaCore::MAX_SLOTS; i++) {
            txBufs[i] = frameTx[i];
            rxBufs[i] = frameRx[i];
        }

        NrfDmaCore::Hal hal{};
        hal.ctx            = this;
        hal.queueTrans     = &MockSpiNrfHw::halQueueTrans;
        hal.getTransResult = &MockSpiNrfHw::halGetTransResult;
        hal.syncTrans      = &MockSpiNrfHw::halSyncTrans;
        hal.setCe          = &MockSpiNrfHw::halSetCe;
        hal.delayUs        = &MockSpiNrfHw::halDelayUs;
        hal.getMicros      = &MockSpiNrfHw::halGetMicros;
        hal.msToTicks      = &MockSpiNrfHw::halMsToTicks;
        hal.dmaDoneFlag    = &dmaDoneFlag;

        core.attach(hal, qSize, &dmaTxPkt, &dmaRxPkt, txBufs, rxBufs);
    }

    uint8_t computeStatusReg() const {
        uint8_t st = static_cast<uint8_t>(regs[NrfDmaCore::NRF_REG_STATUS] & 0x70U);
        if (rxFifo.empty()) {
            st |= NrfDmaCore::NRF_STATUS_RX_P_NO; // 0x0E = üres RX FIFO
        } else {
            st |= static_cast<uint8_t>(0x02U);    // Pipe 1 payload elérhető
        }
        return st;
    }

    void executeNrfSpiFrame(uint8_t slotIdx, size_t totalBytes) {
        const uint8_t* tx = txBufs[slotIdx];
        uint8_t* rx       = rxBufs[slotIdx];
        uint8_t cmd       = tx[0];

        memset(rx, 0, NrfDmaCore::DMA_FRAME_BYTES);
        rx[0] = computeStatusReg();

        if (cmd == NrfDmaCore::NRF_CMD_NOP) {
            return;
        }
        if (cmd == NrfDmaCore::NRF_CMD_FLUSH_TX) {
            flushTxCalls++;
            hasTxPayload = false;
            return;
        }
        if (cmd == NrfDmaCore::NRF_CMD_FLUSH_RX) {
            flushRxCalls++;
            rxFifo.clear();
            return;
        }
        if (cmd == NrfDmaCore::NRF_CMD_W_TX_PAYLOAD) {
            wTxPayloadCalls++;
            if (totalBytes >= 1U + sizeof(NrfPacket)) {
                memcpy(&txFifoPkt, tx + 1, sizeof(NrfPacket));
                hasTxPayload = true;
            }
            return;
        }
        if (cmd == NrfDmaCore::NRF_CMD_R_RX_PAYLOAD) {
            rRxPayloadCalls++;
            if (!rxFifo.empty()) {
                memcpy(rx + 1, &rxFifo.front(), sizeof(NrfPacket));
                rxFifo.erase(rxFifo.begin());
            } else {
                // Üres RX FIFO olvasása -> hibás/ismételt szemét szimuláció
                memset(rx + 1, 0xEE, sizeof(NrfPacket));
            }
            return;
        }
        uint8_t reg = static_cast<uint8_t>(cmd & 0x1FU);
        if ((cmd & 0xE0U) == NrfDmaCore::NRF_CMD_W_REGISTER) {
            if (reg == NrfDmaCore::NRF_REG_TX_ADDR && totalBytes >= 6) {
                memcpy(txAddr, tx + 1, 5);
            } else if (reg == NrfDmaCore::NRF_REG_RX_ADDR_P0 && totalBytes >= 6) {
                memcpy(rxAddrP0, tx + 1, 5);
            } else if (reg == NrfDmaCore::NRF_REG_RX_ADDR_P1 && totalBytes >= 6) {
                memcpy(rxAddrP1, tx + 1, 5);
            } else if (reg == NrfDmaCore::NRF_REG_STATUS) {
                regs[NrfDmaCore::NRF_REG_STATUS] &= static_cast<uint8_t>(~(tx[1] & 0x70U));
            } else {
                regs[reg] = tx[1];
            }
        } else if ((cmd & 0xE0U) == NrfDmaCore::NRF_CMD_R_REGISTER) {
            if (reg == NrfDmaCore::NRF_REG_STATUS) {
                rx[1] = computeStatusReg();
            } else {
                rx[1] = regs[reg];
            }
        }
    }

    void completeNextInHw(size_t count = 1) {
        size_t done = 0;
        for (size_t i = 0; i < hwQueue.size() && done < count; i++) {
            if (!hwQueue[i].doneInHw) {
                executeNrfSpiFrame(hwQueue[i].slotIdx, hwQueue[i].totalBytes);
                hwQueue[i].doneInHw = true;
                dmaDoneFlag = true; // ISR szimuláció
                done++;
            }
        }
    }

    void completeAllInHw() {
        completeNextInHw(hwQueue.size());
    }

    void pushRxPacket(const NrfPacket& pkt) {
        if (rxFifo.size() < 3) {
            rxFifo.push_back(pkt);
            regs[NrfDmaCore::NRF_REG_STATUS] |= NrfDmaCore::NRF_STATUS_RX_DR;
        }
    }

    static NrfDmaCore::HalStatus halQueueTrans(void* ctx, uint8_t slotIdx, size_t totalBytes) {
        auto* self = static_cast<MockSpiNrfHw*>(ctx);
        self->nowMicros += self->spiApiStepUs;

        // SZERZŐDÉS ELLENŐRZÉS: aktív driver tulajdonban lévő slotot SOHA nem szabad újraküldeni!
        for (const auto& item : self->hwQueue) {
            assert(item.slotIdx != slotIdx && "DMA Use-After-Free: active slot requeued!");
        }

        if (self->forceQueueErrorOnce || self->queueErrorLeft > 0) {
            self->forceQueueErrorOnce = false;
            if (self->queueErrorLeft > 0) {
                self->queueErrorLeft--;
            }
            return NrfDmaCore::HAL_ERROR;
        }

        if (self->hwQueue.size() >= self->queueCap) {
            return NrfDmaCore::HAL_BUSY;
        }

        PendingDmaItem item{ slotIdx, totalBytes, false };
        if (self->autoCompleteOnQueue && !self->stallCompletions) {
            self->executeNrfSpiFrame(slotIdx, totalBytes);
            item.doneInHw = true;
            self->dmaDoneFlag = true;
        }
        self->hwQueue.push_back(item);
        return NrfDmaCore::HAL_OK;
    }

    static NrfDmaCore::HalStatus halGetTransResult(void* ctx, uint32_t waitTicks, uint8_t* outIdx) {
        auto* self = static_cast<MockSpiNrfHw*>(ctx);
        self->nowMicros += self->spiApiStepUs;
        if (self->hwQueue.empty()) {
            return NrfDmaCore::HAL_TIMEOUT;
        }
        if (self->hwQueue.front().doneInHw) {
            *outIdx = self->hwQueue.front().slotIdx;
            self->hwQueue.erase(self->hwQueue.begin());
            return NrfDmaCore::HAL_OK;
        }
        if (waitTicks > 0 && !self->stallCompletions) {
            self->executeNrfSpiFrame(self->hwQueue.front().slotIdx, self->hwQueue.front().totalBytes);
            *outIdx = self->hwQueue.front().slotIdx;
            self->hwQueue.erase(self->hwQueue.begin());
            return NrfDmaCore::HAL_OK;
        }
        return NrfDmaCore::HAL_TIMEOUT;
    }

    static bool halSyncTrans(void* ctx, uint8_t slotIdx, size_t totalBytes) {
        auto* self = static_cast<MockSpiNrfHw*>(ctx);
        // ESP-IDF SZERZŐDÉS ELLENŐRZÉS: függő queued DMA tranzakció mellett tilos syncTrans!
        assert(self->hwQueue.empty() && "Illegal spi_device_transmit while queued DMA transactions exist!");
        self->syncTransCalls++;
        if (self->forceSyncFail) {
            return false;
        }
        self->executeNrfSpiFrame(slotIdx, totalBytes);
        return true;
    }

    static void halSetCe(void* ctx, bool high) {
        auto* self = static_cast<MockSpiNrfHw*>(ctx);
        self->ceHigh = high;
    }

    static void halDelayUs(void* ctx, uint32_t us) {
        auto* self = static_cast<MockSpiNrfHw*>(ctx);
        self->nowMicros += us;
        if (self->ceHigh && us >= 10UL) {
            self->cePulsed = true;
            self->lastCePulseUs = us;
        }
    }

    static uint32_t halGetMicros(void* ctx) {
        return static_cast<MockSpiNrfHw*>(ctx)->nowMicros;
    }

    static uint32_t halMsToTicks(void* ctx, uint32_t ms) {
        (void)ctx;
        return ms;
    }
};

NrfPacket makeRequestPkt(uint32_t seq, uint8_t outputsMask = 0x01, uint8_t cmdSeq = 0) {
    NrfPacket pkt{};
    pkt.packetType               = PKT_TYPE_REQUEST;
    pkt.esp.outputsMask          = outputsMask;
    pkt.esp.targetTemp           = 21.5f;
    pkt.esp.modeAndManual        = packModeAndManual(MODE_AUTO, false);
    pkt.esp.cmdSeq               = cmdSeq;
    pkt.esp.pumpConfig           = { PUMP_MODE_AUTO, 40.0f, 30.0f, PUMP_SENSOR_KAZAN };
    pkt.esp.alarmConfig          = { 85.0f, 1 };
    pkt.esp.heartbeatIntervalSec = 5;
    pkt.esp.seq                  = seq;
    return pkt;
}

NrfPacket makeResponsePkt(uint32_t seq, uint8_t cmdSeq = 0, uint8_t syncFlags = 0) {
    NrfPacket pkt{};
    pkt.packetType          = PKT_TYPE_RESPONSE;
    pkt.nano.eloremeno      = 48.2f;
    pkt.nano.visszatero     = 39.4f;
    pkt.nano.kazan          = 55.0f;
    pkt.nano.kulteri        = 4.5f;
    pkt.nano.seq            = seq;
    pkt.nano.counter        = seq;
    pkt.nano.sensorValidMask = 0x0F;
    pkt.nano.cmdSeq         = cmdSeq;
    pkt.nano.syncFlags      = syncFlags;
    return pkt;
}

// 1. Regiszterkonfiguráció paritás ellenőrzése a Nano RF24 implementációval
void testRegisterConfigParity() {
    MockSpiNrfHw hw;
    NrfDmaCore core;
    hw.init(core, 3);
    assert(core.initRadioRegisters());
    assert(core.isReady());
    assert(hw.regs[NrfDmaCore::NRF_REG_CONFIG] == NrfDmaCore::NRF_CONFIG_PTX);
    assert(hw.regs[NrfDmaCore::NRF_REG_EN_AA] == 0x3F);
    assert(hw.regs[NrfDmaCore::NRF_REG_EN_RXADDR] == 0x03);
    assert(hw.regs[NrfDmaCore::NRF_REG_SETUP_AW] == 0x03);
    assert(hw.regs[NrfDmaCore::NRF_REG_SETUP_RETR] == 0x5F); // 1500us, 15 retry (RF24 setRetries(5,15))
    assert(hw.regs[NrfDmaCore::NRF_REG_RF_CH] == NRF_CHANNEL);
    assert((hw.regs[NrfDmaCore::NRF_REG_RF_SETUP] & 0x26U) == 0x24U); // 250kbps + PA_HIGH
    assert(hw.regs[NrfDmaCore::NRF_REG_FEATURE] == 0x00);
    assert(hw.regs[NrfDmaCore::NRF_REG_DYNPD] == 0x00);
    assert(hw.regs[NrfDmaCore::NRF_REG_RX_PW_P0] == 32);
    assert(hw.regs[NrfDmaCore::NRF_REG_RX_PW_P1] == 32);
    assert(memcmp(hw.txAddr, NRF_ADDR_MASTER_TO_SLAVE, 5) == 0);
    assert(memcmp(hw.rxAddrP0, NRF_ADDR_MASTER_TO_SLAVE, 5) == 0);
    assert(memcmp(hw.rxAddrP1, NRF_ADDR_SLAVE_TO_MASTER, 5) == 0);
    assert(hw.flushTxCalls == 1 && hw.flushRxCalls == 1);

    // Sync hiba esetén az init bukjon el tisztán
    NrfDmaCore coreFail;
    MockSpiNrfHw hwFail;
    hwFail.init(coreFail, 3);
    hwFail.forceSyncFail = true;
    assert(!coreFail.initRadioRegisters());
    assert(!coreFail.isReady());
}

// 2. Teljes TX -> TX_DS -> PRX -> RX_PAYLOAD -> stopListening ciklus + timing
void testNormalTxAckRxFlow() {
    MockSpiNrfHw hw;
    NrfDmaCore core;
    hw.init(core, 3);
    assert(core.initRadioRegisters());

    NrfPacket req = makeRequestPkt(42, 0x03, 7);
    assert(core.queueTransmitPacket(req));
    assert(hw.wTxPayloadCalls == 0);

    NrfPacket rxOut{};
    // OP_TX_STATUS_CLR -> OP_TX_PAYLOAD -> CE 15us pulse -> OP_POLL_TX_STATUS
    for (int i = 0; i < 4; i++) {
        auto r = core.poll(&rxOut);
        assert(!r.txAckedNow);
        assert(!r.rxPacketReady);
        if (i == 1) {
            assert(hw.wTxPayloadCalls == 1);
            assert(memcmp(&hw.txFifoPkt, &req, sizeof(NrfPacket)) == 0);
        }
    }
    assert(hw.cePulsed && hw.lastCePulseUs >= 10UL);

    // Nano nyugtázza -> TX_DS beállítva
    hw.regs[NrfDmaCore::NRF_REG_STATUS] |= NrfDmaCore::NRF_STATUS_TX_DS;
    hw.nowMicros += 250UL;
    core.poll(&rxOut); // Step 5: elküldi a 200us-os OP_POLL_TX_STATUS DMA lekérdezést
    auto rAck = core.poll(&rxOut); // Következő poll begyűjti a TX_DS-t!
    assert(rAck.txAckedNow);
    assert(rAck.hasTimingUpdate);
    assert(rAck.dmaWaitUs > 0);

    // Átállás PRX-be (OP_RX_STATUS_CLR + OP_RX_CONFIG)
    core.poll(&rxOut);
    assert(core.isRxMode());
    assert(hw.ceHigh);
    assert(hw.regs[NrfDmaCore::NRF_REG_CONFIG] == NrfDmaCore::NRF_CONFIG_PRX);

    // Nano visszaküldi a 32 byte-os RESPONSE-t
    NrfPacket rsp = makeResponsePkt(42, 7, SYNC_FLAG_CMD_ACK | SYNC_FLAG_PUMP_ON);
    hw.pushRxPacket(rsp);

    hw.nowMicros += 250UL;
    core.poll(&rxOut); // Step 5: elküldi OP_POLL_RX_STATUS-t
    core.poll(&rxOut); // Begyűjti OP_POLL_RX_STATUS-t -> sorba teszi OP_RX_PAYLOAD-ot
    auto rRx = core.poll(&rxOut); // OP_RX_PAYLOAD befejeződik
    assert(rRx.rxPacketReady);
    assert(memcmp(&rxOut, &rsp, sizeof(NrfPacket)) == 0);
    assert(hw.rRxPayloadCalls == 1);

    core.stopListening();
    assert(!hw.ceHigh);
    core.poll(&rxOut); // OP_PTX_CONFIG lefut
    assert(!core.isRxMode());
    assert(core.isPtxConfirmed());
}

// 3. Queue telítődés és veszteségmentes _deferredDone + _pendingStepOp feldolgozás
void testQueueSaturationNoLostEvents() {
    MockSpiNrfHw hw;
    NrfDmaCore core;
    hw.init(core, 3);
    assert(core.initRadioRegisters());

    hw.autoCompleteOnQueue = false;

    NrfPacket req = makeRequestPkt(100);
    assert(core.queueTransmitPacket(req));
    assert(core.getInFlightCount() == 1); // OP_TX_STATUS_CLR sorban van

    hw.completeNextInHw(1);
    NrfPacket rxOut{};
    core.poll(&rxOut); // OP_TX_PAYLOAD sorban

    hw.completeNextInHw(1);
    core.poll(&rxOut); // CE pulse + OP_POLL_TX_STATUS sorban (1 in-flight)

    hw.regs[NrfDmaCore::NRF_REG_STATUS] |= NrfDmaCore::NRF_STATUS_TX_DS;
    hw.completeNextInHw(1);
    auto rAck = core.poll(&rxOut);
    assert(rAck.txAckedNow);
    // OP_RX_STATUS_CLR + OP_RX_CONFIG = 2 elem a sorban
    assert(core.getInFlightCount() == 2);

    // Kész mindkettő -> PRX mód aktív
    hw.completeAllInHw();
    core.poll(&rxOut);
    assert(core.isRxMode());

    // Tegyünk két RX csomagot a 3-elemű RX FIFO-ba!
    NrfPacket p1 = makeResponsePkt(100);
    NrfPacket p2 = makeResponsePkt(101);
    hw.pushRxPacket(p1);
    hw.pushRxPacket(p2);

    hw.nowMicros += 300UL;
    core.poll(&rxOut); // OP_POLL_RX_STATUS sorban
    hw.completeNextInHw(1);
    core.poll(&rxOut); // OP_RX_PAYLOAD sorban (1 db)
    hw.completeNextInHw(1);
    auto r1 = core.poll(&rxOut); // p1 megjön!
    assert(r1.rxPacketReady);
    assert(!r1.queueFullWarn);
    assert(rxOut.nano.seq == 100);

    // Második poll a 2. csomagra (FIFO még nem üres)
    hw.completeAllInHw();
    hw.nowMicros += 300UL;
    core.poll(&rxOut); // OP_POLL_RX_STATUS
    hw.completeAllInHw();
    core.poll(&rxOut); // OP_RX_PAYLOAD sorban
    hw.completeAllInHw();
    auto r2 = core.poll(&rxOut); // p2 megjön, és sorba teszi OP_IGNORE (RX_DR törlés)!
    assert(r2.rxPacketReady);
    assert(rxOut.nano.seq == 101);
    hw.completeAllInHw();
    core.poll(&rxOut); // OP_IGNORE lefut -> sor üres
    assert(core.getInFlightCount() == 0);

    // Explicit telített queue teszt (queueCap = 1 alatti átmeneti HAL_BUSY + _pendingStepOp helyreállás):
    // Ha a 2-lépcsős TX_DS -> (OP_RX_STATUS_CLR + OP_RX_CONFIG) alatt a 2. slot HAL_BUSY-t kap,
    // a _pendingStepOp pótolja, és egyetlen esemény sem vész el!
    NrfDmaCore coreSat;
    MockSpiNrfHw hwSat;
    hwSat.init(coreSat, 1); // Szűk 1 elemű sor a garantált queue saturation kiváltásához
    assert(coreSat.initRadioRegisters());
    hwSat.autoCompleteOnQueue = false;

    assert(coreSat.queueTransmitPacket(makeRequestPkt(150)));
    hwSat.completeAllInHw();
    coreSat.poll(&rxOut); // OP_TX_PAYLOAD (1/1)
    hwSat.completeAllInHw();
    coreSat.poll(&rxOut); // OP_POLL_TX_STATUS (1/1)
    hwSat.regs[NrfDmaCore::NRF_REG_STATUS] |= NrfDmaCore::NRF_STATUS_TX_DS;
    hwSat.completeAllInHw();
    auto rSatAck = coreSat.poll(&rxOut);
    // OP_RX_STATUS_CLR elfért (1/1), OP_RX_CONFIG -> HAL_BUSY -> queueFullWarn = true, _pendingStepOp = OP_RX_CONFIG!
    assert(rSatAck.txAckedNow);
    assert(rSatAck.queueFullWarn);
    assert(coreSat.consumeQueueFullWarning());
    assert(!coreSat.isRxMode());

    // Következő poll-ban felszabadul a slot és _pendingStepOp elküldi OP_RX_CONFIG-ot -> PRX helyreáll!
    hwSat.completeAllInHw();
    coreSat.poll(&rxOut); // OP_RX_STATUS_CLR kész -> pótolja OP_RX_CONFIG-ot
    hwSat.completeAllInHw();
    coreSat.poll(&rxOut); // OP_RX_CONFIG kész -> RX mód aktív!
    assert(coreSat.isRxMode());
    assert(hwSat.ceHigh);

    // Veszteségmentes queueOp()-drain teszt: OP_POLL_RX_STATUS befejeződik a hardverben úgy, hogy
    // közben egy másik művelet (stopListening) felszabadítja a telített 1-elemű sort!
    NrfPacket rspLostCheck = makeResponsePkt(150);
    hwSat.pushRxPacket(rspLostCheck);
    hwSat.nowMicros += 300UL;
    coreSat.poll(&rxOut); // Step 5: OP_POLL_RX_STATUS (1/1)
    assert(coreSat.getInFlightCount() == 1);
    hwSat.completeAllInHw(); // OP_POLL_RX_STATUS kész (FIFO van csomag!)
    coreSat.stopListening(); // queueOp belülről leüríti OP_POLL_RX_STATUS-t a _deferred-be, és berakja OP_PTX_CONFIG-ot!
    hwSat.completeAllInHw();
    coreSat.poll(&rxOut); // OP_PTX_CONFIG + _deferred OP_POLL_RX_STATUS is feldolgozva (nem veszett el!)
    assert(!coreSat.isRxMode());

    // Most teszteljük a 3-elemű soron a párhuzamos stopListening + queueTransmitPacket sorrendet
    core.stopListening(); // OP_PTX_CONFIG in flight (1/3)
    assert(core.getInFlightCount() == 1);
    NrfPacket req2 = makeRequestPkt(102);
    assert(core.queueTransmitPacket(req2)); // OP_TX_CONFIG (2/3)
    assert(core.getInFlightCount() == 2);
    hw.completeNextInHw(1); // csak az OP_PTX_CONFIG fejeződik be
    core.poll(&rxOut);
    assert(core.getInFlightCount() == 1); // OP_TX_CONFIG még függőben

    hw.completeAllInHw();
    core.poll(&rxOut); // OP_TX_STATUS_CLR queued
    hw.completeAllInHw();
    core.poll(&rxOut); // OP_TX_PAYLOAD queued
    hw.completeAllInHw();
    core.poll(&rxOut); // CE pulse + OP_POLL_TX_STATUS queued
    assert(hw.wTxPayloadCalls == 2);
}

// 4. flushAndDrain() biztonságossága: sikertelen drain + beragadt DMA + Use-After-Free tilalom + utólagos helyreállás
void testFlushAndDrainStallSafetyAndRecovery() {
    MockSpiNrfHw hw;
    NrfDmaCore core;
    hw.init(core, 3);
    assert(core.initRadioRegisters());

    hw.autoCompleteOnQueue = false;
    NrfPacket req = makeRequestPkt(200);
    assert(core.queueTransmitPacket(req));
    assert(core.getInFlightCount() == 1);

    // Beragad a DMA! A getTransResult timeout-ol (stallCompletions = true)
    hw.stallCompletions = true;
    int syncBefore = hw.syncTransCalls;
    core.flushAndDrain();

    // KRITIKUS ELLENŐRZÉS:
    // 1) A beragadt slot inUse marad, _inFlightCount == 1 marad!
    // 2) TILOS volt a syncTrans (spi_device_transmit) hívása!
    // 3) _pendingResetCleanup == true!
    assert(core.isPendingResetCleanup());
    assert(core.getInFlightCount() == 1);
    assert(core.isSlotInUse(0));
    assert(hw.syncTransCalls == syncBefore);

    // Amíg a beragadt DMA nincs lezárva, új TX nem írhatja felül a slotot!
    NrfPacket reqBlocked = makeRequestPkt(201);
    assert(!core.queueTransmitPacket(reqBlocked));
    assert(core.getInFlightCount() == 1);

    // Tegyük fel a hardver DMA később végre befejeződik
    hw.stallCompletions = false;
    hw.completeAllInHw();

    NrfPacket rxOut{};
    int flushBefore = hw.flushTxCalls;
    core.poll(&rxOut);

    // Most a poll() biztonságosan lezárta a régi slotot és lefuttatta a FLUSH_TX/FLUSH_RX/STATUS/PTX helyreállítást!
    assert(!core.isPendingResetCleanup());
    assert(core.getInFlightCount() == 0);
    assert(!core.isSlotInUse(0));
    assert(hw.flushTxCalls == flushBefore + 1);
    assert(hw.flushRxCalls >= 2);
    assert(core.isPtxConfirmed());

    // Azonnali új adás tökéletesen működik utána!
    hw.autoCompleteOnQueue = true;
    assert(core.queueTransmitPacket(makeRequestPkt(202)));
    for (int i = 0; i < 4; i++) {
        core.poll(&rxOut);
    }
    assert(hw.wTxPayloadCalls == 1);
}

// 5. queueOp() visszatérési hiba (HAL_ERROR) és queue full retry recovery teszt
void testSpiTransactionErrorAndQueueFullRetry() {
    MockSpiNrfHw hw;
    NrfDmaCore core;
    hw.init(core, 3);
    assert(core.initRadioRegisters());

    // Queue error injektálása TX_STATUS_CLR közben -> ne ragadjon be a state machine!
    hw.forceQueueErrorOnce = true;
    assert(!core.queueTransmitPacket(makeRequestPkt(300)));

    // Következő normál adásnál hibamentesen megy tovább
    assert(core.queueTransmitPacket(makeRequestPkt(301)));
    NrfPacket rxOut{};
    core.poll(&rxOut); // OP_TX_STATUS_CLR -> OP_TX_PAYLOAD
    core.poll(&rxOut); // OP_TX_PAYLOAD -> OP_POLL_TX_STATUS
    core.poll(&rxOut); // OP_POLL_TX_STATUS lefut (TX_DS == 0)
    hw.regs[NrfDmaCore::NRF_REG_STATUS] |= NrfDmaCore::NRF_STATUS_TX_DS;
    hw.nowMicros += 250UL;
    core.poll(&rxOut); // Step 5 elküldi OP_POLL_TX_STATUS-t
    hw.forceQueueErrorOnce = true; // Következő poll()-ban az OP_RX_STATUS_CLR hiba -> txFailedNow / PHASE_TX_FAILED
    auto rErr = core.poll(&rxOut);
    assert(rErr.txFailedNow);
    assert(core.getPhase() == NrfDmaCore::PHASE_TX_FAILED);

    // flushAndDrain + új TX visszaállítja a működést
    core.flushAndDrain();
    assert(core.queueTransmitPacket(makeRequestPkt(302)));
}

// 6. Beragadt RX_DR üres RX FIFO mellett + MAX_RT kezelés
void testStaleRxDrAndMaxRtRecovery() {
    MockSpiNrfHw hw;
    NrfDmaCore core;
    hw.init(core, 3);
    assert(core.initRadioRegisters());

    // MAX_RT eset szimulálása
    assert(core.queueTransmitPacket(makeRequestPkt(400)));
    NrfPacket rxOut{};
    core.poll(&rxOut); // STATUS_CLR -> TX_PAYLOAD
    core.poll(&rxOut); // TX_PAYLOAD -> POLL_TX_STATUS
    core.poll(&rxOut); // POLL_TX_STATUS kész
    hw.regs[NrfDmaCore::NRF_REG_STATUS] |= NrfDmaCore::NRF_STATUS_MAX_RT;
    hw.nowMicros += 250UL;
    core.poll(&rxOut); // Step 5: OP_POLL_TX_STATUS sorba
    int flushBefore = hw.flushTxCalls;
    auto rMaxRt = core.poll(&rxOut); // Begyűjti MAX_RT-t és sorba teszi OP_FLUSH_TX + STATUS_CLR-t
    assert(rMaxRt.txFailedNow);
    assert(core.getPhase() == NrfDmaCore::PHASE_TX_FAILED);
    core.poll(&rxOut); // Befejeződik a sorban álló FLUSH_TX / STATUS_CLR
    assert(hw.flushTxCalls == flushBefore + 1);
    assert((hw.regs[NrfDmaCore::NRF_REG_STATUS] & NrfDmaCore::NRF_STATUS_MAX_RT) == 0);

    // Átmenet normál RX listen módba + hamis RX_DR teszt (üres RX FIFO!)
    core.flushAndDrain();
    assert(core.queueTransmitPacket(makeRequestPkt(401)));
    core.poll(&rxOut);
    core.poll(&rxOut);
    core.poll(&rxOut);
    hw.regs[NrfDmaCore::NRF_REG_STATUS] |= NrfDmaCore::NRF_STATUS_TX_DS;
    hw.nowMicros += 250UL;
    core.poll(&rxOut);
    core.poll(&rxOut);
    core.poll(&rxOut);
    assert(core.isRxMode());

    // Hamis RX_DR flag beállítva, de az rxFifo ÜRES!
    hw.regs[NrfDmaCore::NRF_REG_STATUS] |= NrfDmaCore::NRF_STATUS_RX_DR;
    int rxReadBefore = hw.rRxPayloadCalls;
    hw.nowMicros += 250UL;
    core.poll(&rxOut); // Step 5: OP_POLL_RX_STATUS
    auto rFake = core.poll(&rxOut); // OP_POLL_RX_STATUS begyűjtése -> csak RX_DR törlés
    assert(!rFake.rxPacketReady);
    core.poll(&rxOut); // RX_DR törlése lefut
    assert(hw.rRxPayloadCalls == rxReadBefore); // NEM olvasott üres FIFO-ból!
    assert((hw.regs[NrfDmaCore::NRF_REG_STATUS] & NrfDmaCore::NRF_STATUS_RX_DR) == 0);
}

// 7. stopListening() busy sor mellett + elveszett/hibás válasz + 32-bites millis/micros wraparound
void testStopListeningAnd32BitWraparound() {
    MockSpiNrfHw hw;
    NrfDmaCore core;
    hw.init(core, 3);
    assert(core.initRadioRegisters());

    // 32-bit micros() túlcsordulás átlépése (0xFFFFFF80 -> 0x00000100)
    hw.nowMicros = 0xFFFFFF80UL;
    assert(core.queueTransmitPacket(makeRequestPkt(500)));
    NrfPacket rxOut{};
    core.poll(&rxOut);
    core.poll(&rxOut);
    core.poll(&rxOut); // _lastPollUs rögzítve ~0xFFFFFFD0 körül
    hw.regs[NrfDmaCore::NRF_REG_STATUS] |= NrfDmaCore::NRF_STATUS_TX_DS;
    hw.nowMicros = 0x00000200UL; // Wraparound után! Delta > 200 us
    core.poll(&rxOut); // Step 5: OP_POLL_TX_STATUS
    auto rWrap = core.poll(&rxOut); // TX_DS
    assert(rWrap.txAckedNow);

    // stopListening()
    core.poll(&rxOut); // most RX listen módban vagyunk (_isRxMode = true)
    assert(core.isRxMode());
    hw.autoCompleteOnQueue = false;
    hw.nowMicros += 300UL;
    core.poll(&rxOut); // OP_POLL_RX_STATUS (1/3)
    hw.completeAllInHw();
    hw.nowMicros += 300UL;
    core.stopListening();
    hw.completeAllInHw();
    core.poll(&rxOut);
    assert(!core.isRxMode());
    assert(core.isPtxConfirmed());

    // Hibás/rossz seq-ű RX csomag nem fogadódik el V5 szerződésben (szimulált upper layer)
    hw.autoCompleteOnQueue = true;
    NrfPacket req501 = makeRequestPkt(501);
    assert(core.queueTransmitPacket(req501));
    core.poll(&rxOut);
    core.poll(&rxOut);
    core.poll(&rxOut);
    hw.regs[NrfDmaCore::NRF_REG_STATUS] |= NrfDmaCore::NRF_STATUS_TX_DS;
    hw.nowMicros += 250UL;
    core.poll(&rxOut);
    core.poll(&rxOut);
    core.poll(&rxOut);
    // Küldjünk előbb rossz seq=499 csomagot, majd a jó seq=501 csomagot!
    hw.pushRxPacket(makeResponsePkt(499));
    hw.nowMicros += 250UL;
    core.poll(&rxOut); // Step 5: OP_POLL_RX_STATUS
    core.poll(&rxOut); // OP_POLL_RX_STATUS -> OP_RX_PAYLOAD
    auto rWrong = core.poll(&rxOut);
    assert(rWrong.rxPacketReady);
    assert(rxOut.nano.seq != req501.esp.seq); // A felsőbb réteg eldobja, a rádió marad RX_LISTEN-ben!
    assert(core.getPhase() == NrfDmaCore::PHASE_RX_LISTEN);

    hw.pushRxPacket(makeResponsePkt(501));
    hw.nowMicros += 250UL;
    core.poll(&rxOut); // Step 5: OP_POLL_RX_STATUS
    core.poll(&rxOut); // OP_POLL_RX_STATUS -> OP_RX_PAYLOAD
    auto rRight = core.poll(&rxOut);
    assert(rRight.rxPacketReady);
    assert(rxOut.nano.seq == req501.esp.seq);
}

// 8. Ismételt NRF timeout, millis wraparound, NRF fail-safe, CMD_ACK és helyreállás regresszió
void testRepeatedTimeoutFailsafeAndRecoveryRegression() {
    MockSpiNrfHw hw;
    NrfDmaCore core;
    hw.init(core, 3);
    assert(core.initRadioRegisters());

    // V5 Master állapotgép szimulációja (handleNrf / handleNrfTimeout pontos logikája)
    constexpr uint32_t NRF_TIMEOUT_MS = 500UL;
    constexpr uint8_t  NRF_MAX_RETRY  = 3;
    constexpr uint8_t  MAX_MISSED     = 3;

    bool nrf24Ok = true;
    uint8_t consecutiveMissed = 0;
    bool cmdAckOk = false;
    bool cmdAckTimeout = false;

    uint32_t nowMs = 0xFFFFFCE0UL; // Közel a 49 napos millis() túlcsorduláshoz!
    uint32_t nanoLastReqMs = nowMs;
    const uint8_t hbSec = 5;
    const uint32_t nanoFailsafeMs = (3UL * hbSec * 1000UL) + 2000UL; // 17000 ms

    // Indítsunk el egy pinget (CMD_ACK igénnyel, cmdSeq=9), amelyre a Nano NEM válaszol
    uint32_t seq = 1;
    NrfPacket req = makeRequestPkt(seq, 0x01, 9);
    assert(core.queueTransmitPacket(req));
    uint32_t sentAt = nowMs;
    uint8_t retry = 0;
    bool anyWriteOk = false;

    // 3 missed ping ciklus (egyikre sem jön válasz)
    for (int cycle = 0; cycle < MAX_MISSED; cycle++) {
        retry = 0;
        anyWriteOk = false;
        while (true) {
            for (int k = 0; k < 5; k++) {
                NrfPacket rxOut{};
                auto r = core.poll(&rxOut);
                if (r.txAckedNow) anyWriteOk = true;
                hw.nowMicros += 250UL;
            }
            nowMs += 500UL; // uint32_t wraparound átlépve az első ciklusban!
            if (static_cast<uint32_t>(nowMs - sentAt) >= NRF_TIMEOUT_MS) {
                if (retry < NRF_MAX_RETRY) {
                    core.flushAndDrain();
                    assert(core.queueTransmitPacket(req));
                    sentAt = nowMs;
                    retry++;
                } else {
                    core.flushAndDrain();
                    if (cycle == 0) {
                        cmdAckTimeout = true;
                    }
                    if (!anyWriteOk) {
                        nrf24Ok = false;
                        consecutiveMissed = MAX_MISSED;
                    } else {
                        consecutiveMissed++;
                        if (consecutiveMissed >= MAX_MISSED) {
                            nrf24Ok = false;
                        }
                    }
                    break;
                }
            }
        }
        seq++;
        req = makeRequestPkt(seq, 0x01, 0);
        assert(core.queueTransmitPacket(req));
        sentAt = nowMs;
    }

    assert(!nrf24Ok);
    assert(cmdAckTimeout);
    assert(!cmdAckOk);

    // 17+ mp múlva Nano oldali fail-safe (overflow-safe uint32_t kivonás) aktiválódik
    nowMs += 12000UL; // összes eltelt idő > 17000 ms
    bool nanoFailsafeActive = (static_cast<uint32_t>(nowMs - nanoLastReqMs) > nanoFailsafeMs);
    assert(nanoFailsafeActive);

    // Most a rádió és a Nano újra válaszol -> helyreállás
    req = makeRequestPkt(seq, 0x01, 12);
    core.flushAndDrain();
    assert(core.queueTransmitPacket(req));
    NrfPacket rxOut{};
    core.poll(&rxOut);
    core.poll(&rxOut);
    core.poll(&rxOut);
    hw.regs[NrfDmaCore::NRF_REG_STATUS] |= NrfDmaCore::NRF_STATUS_TX_DS;
    hw.nowMicros += 250UL;
    core.poll(&rxOut); // Step 5: OP_POLL_TX_STATUS
    auto rAck = core.poll(&rxOut);
    assert(rAck.txAckedNow);
    core.poll(&rxOut);

    hw.pushRxPacket(makeResponsePkt(seq, 12, SYNC_FLAG_CMD_ACK | SYNC_FLAG_PUMP_ON));
    hw.nowMicros += 250UL;
    core.poll(&rxOut); // Step 5: OP_POLL_RX_STATUS
    core.poll(&rxOut); // OP_RX_PAYLOAD sorba
    auto rRsp = core.poll(&rxOut);
    assert(rRsp.rxPacketReady);
    assert(rxOut.nano.seq == seq);
    assert((rxOut.nano.syncFlags & SYNC_FLAG_CMD_ACK) != 0);

    core.stopListening();
    core.poll(&rxOut);
    nrf24Ok = true;
    consecutiveMissed = 0;
    cmdAckOk = true;
    nanoFailsafeActive = false;

    assert(nrf24Ok);
    assert(cmdAckOk);
    assert(!nanoFailsafeActive);
}

} // namespace

int main() {
    testRegisterConfigParity();
    testNormalTxAckRxFlow();
    testQueueSaturationNoLostEvents();
    testFlushAndDrainStallSafetyAndRecovery();
    testSpiTransactionErrorAndQueueFullRetry();
    testStaleRxDrAndMaxRtRecovery();
    testStopListeningAnd32BitWraparound();
    testRepeatedTimeoutFailsafeAndRecoveryRegression();

    printf("ALL ESP32 NRF24 SPI DMA TESTS PASSED\n");
    return 0;
}
