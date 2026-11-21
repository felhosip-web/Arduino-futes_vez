#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "../../nano/src/common.h"

// ============================================================================
// NRF24L01+ SPI + DMA ÁLLAPOTGÉP ÉS TRANZAKCIÓS SOR MAG (HARDVER-FÜGGETLEN)
// ============================================================================
// - Pure C++11 implementáció külső RTOS/Arduino függőség nélkül, így egyaránt
//   fordul az ESP32-C3 SPI2_HOST GDMA firmware-ben és a host g++ tesztekben.
// - Szigorú slot-tulajdonlás: az inUse flag kizárólag a getTransResult()
//   visszatérésekor törlődik; beragadt DMA esetén syncTrans() TILOS!
// - Veszteségmentes queueOp(): a sorból leürített kész tranzakciók a
//   _deferredDone gyűrűspufferbe kerülnek, így egyetlen állapotgép-esemény
//   sem veszhet el.
// ============================================================================
class NrfDmaCore {
public:
    enum HalStatus : int8_t {
        HAL_OK      = 0,
        HAL_TIMEOUT = 1,
        HAL_BUSY    = 2,
        HAL_ERROR   = -1
    };

    enum Phase : uint8_t {
        PHASE_IDLE = 0,
        PHASE_TX_PREP_CONFIG,
        PHASE_TX_PREP_STATUS,
        PHASE_TX_PAYLOAD_PENDING,
        PHASE_TX_AIR_WAIT,
        PHASE_RX_SWITCH_WAIT,
        PHASE_RX_LISTEN,
        PHASE_RX_PAYLOAD_PENDING,
        PHASE_TX_FAILED
    };

    enum SlotOp : uint8_t {
        OP_NONE = 0,
        OP_IGNORE,
        OP_PTX_CONFIG,
        OP_TX_CONFIG,
        OP_TX_STATUS_CLR,
        OP_TX_PAYLOAD,
        OP_POLL_TX_STATUS,
        OP_RX_STATUS_CLR,
        OP_RX_CONFIG,
        OP_POLL_RX_STATUS,
        OP_RX_PAYLOAD,
        OP_FLUSH_TX
    };

    struct PollResult {
        bool     txAckedNow      = false;
        bool     txFailedNow     = false;
        bool     rxPacketReady   = false;
        bool     queueFullWarn   = false;
        bool     hasTimingUpdate = false;
        uint32_t dmaWaitUs       = 0;
    };

    struct Hal {
        void*          ctx = nullptr;
        HalStatus    (*queueTrans)(void* ctx, uint8_t slotIdx, size_t totalBytes)      = nullptr;
        HalStatus    (*getTransResult)(void* ctx, uint32_t waitTicks, uint8_t* outIdx) = nullptr;
        bool         (*syncTrans)(void* ctx, uint8_t slotIdx, size_t totalBytes)       = nullptr;
        void         (*setCe)(void* ctx, bool high)                                    = nullptr;
        void         (*delayUs)(void* ctx, uint32_t us)                                = nullptr;
        uint32_t     (*getMicros)(void* ctx)                                           = nullptr;
        uint32_t     (*msToTicks)(void* ctx, uint32_t ms)                              = nullptr;
        volatile bool* dmaDoneFlag = nullptr;
    };

    // 1 byte NRF SPI parancs + 32 byte NrfPacket = 33 byte; 4-byte DMA igazítással 36 byte
    static constexpr size_t  DMA_FRAME_BYTES     = 36;
    static constexpr size_t  DMA_MAX_DATA_BYTES  = 32;
    static constexpr uint8_t MAX_SLOTS           = 3;
    static constexpr uint8_t DEFERRED_CAP        = 8;

    static constexpr uint8_t NRF_CMD_R_REGISTER   = 0x00;
    static constexpr uint8_t NRF_CMD_W_REGISTER   = 0x20;
    static constexpr uint8_t NRF_CMD_R_RX_PAYLOAD = 0x61;
    static constexpr uint8_t NRF_CMD_W_TX_PAYLOAD = 0xA0;
    static constexpr uint8_t NRF_CMD_FLUSH_TX     = 0xE1;
    static constexpr uint8_t NRF_CMD_FLUSH_RX     = 0xE2;
    static constexpr uint8_t NRF_CMD_NOP          = 0xFF;

    static constexpr uint8_t NRF_REG_CONFIG       = 0x00;
    static constexpr uint8_t NRF_REG_EN_AA        = 0x01;
    static constexpr uint8_t NRF_REG_EN_RXADDR    = 0x02;
    static constexpr uint8_t NRF_REG_SETUP_AW     = 0x03;
    static constexpr uint8_t NRF_REG_SETUP_RETR   = 0x04;
    static constexpr uint8_t NRF_REG_RF_CH        = 0x05;
    static constexpr uint8_t NRF_REG_RF_SETUP     = 0x06;
    static constexpr uint8_t NRF_REG_STATUS       = 0x07;
    static constexpr uint8_t NRF_REG_RX_ADDR_P0   = 0x0A;
    static constexpr uint8_t NRF_REG_RX_ADDR_P1   = 0x0B;
    static constexpr uint8_t NRF_REG_TX_ADDR      = 0x10;
    static constexpr uint8_t NRF_REG_RX_PW_P0     = 0x11;
    static constexpr uint8_t NRF_REG_RX_PW_P1     = 0x12;
    static constexpr uint8_t NRF_REG_DYNPD        = 0x1C;
    static constexpr uint8_t NRF_REG_FEATURE      = 0x1D;

    static constexpr uint8_t NRF_CONFIG_PTX       = 0x0E;
    static constexpr uint8_t NRF_CONFIG_PRX       = 0x0F;

    static constexpr uint8_t NRF_STATUS_RX_DR     = 0x40;
    static constexpr uint8_t NRF_STATUS_TX_DS     = 0x20;
    static constexpr uint8_t NRF_STATUS_MAX_RT    = 0x10;
    static constexpr uint8_t NRF_STATUS_CLR_ALL   = 0x70;
    static constexpr uint8_t NRF_STATUS_RX_P_NO   = 0x0E;

    static constexpr uint32_t NRF_POLL_INTERVAL_US = 200UL;

    struct Slot {
        bool     inUse      = false;
        SlotOp   op         = OP_NONE;
        uint32_t queuedAtUs = 0;
        uint8_t* txBuf      = nullptr;
        uint8_t* rxBuf      = nullptr;
    };

    void attach(const Hal& hal,
                uint8_t    queueSize,
                NrfPacket* dmaTxPacket,
                NrfPacket* dmaRxPacket,
                uint8_t*   txBufs[MAX_SLOTS],
                uint8_t*   rxBufs[MAX_SLOTS]) {
        _hal = hal;
        _queueSize = (queueSize > MAX_SLOTS) ? MAX_SLOTS : (queueSize == 0 ? 1 : queueSize);
        _dmaTxPacket = dmaTxPacket;
        _dmaRxPacket = dmaRxPacket;
        for (uint8_t i = 0; i < MAX_SLOTS; i++) {
            _slots[i].txBuf = txBufs ? txBufs[i] : nullptr;
            _slots[i].rxBuf = rxBufs ? rxBufs[i] : nullptr;
            _slots[i].inUse = false;
            _slots[i].op    = OP_NONE;
            _slots[i].queuedAtUs = 0;
        }
        resetState();
    }

    void resetState() {
        _ready                   = false;
        _isRxMode                = false;
        _ptxConfirmed            = false;
        _pendingResetCleanup     = false;
        _phase                   = PHASE_IDLE;
        _inFlightCount           = 0;
        _lastPollUs              = 0;
        _lastWaitUs              = 0;
        _maxWaitUs               = 0;
        _queueFullWarn           = false;
        _pendingStepOp           = OP_NONE;
        _needRxConfigAfterClr    = false;
        _needStatusClrAfterFlush = false;
        _pendingPostRxClr        = false;
        _deferredHead            = 0;
        _deferredCount           = 0;
        if (_hal.dmaDoneFlag) {
            *_hal.dmaDoneFlag = false;
        }
    }

    bool initRadioRegisters() {
        if (!_hal.syncTrans || !_hal.setCe || !_hal.delayUs || !_dmaTxPacket || !_dmaRxPacket) {
            return false;
        }
        if (_inFlightCount > 0) {
            return false;
        }
        for (uint8_t i = 0; i < _queueSize; i++) {
            if (!_slots[i].txBuf || !_slots[i].rxBuf || _slots[i].inUse) {
                return false;
            }
        }

        _hal.setCe(_hal.ctx, false);
        _hal.delayUs(_hal.ctx, 5000UL);

        bool initOk = true;
        initOk &= syncWriteReg(NRF_REG_CONFIG, 0x0C); // EN_CRC | CRCO (16-bit), power down
        initOk &= syncWriteReg(NRF_REG_EN_AA, 0x3F);
        initOk &= syncWriteReg(NRF_REG_EN_RXADDR, 0x03);
        initOk &= syncWriteReg(NRF_REG_SETUP_AW, 0x03);
        initOk &= syncWriteReg(NRF_REG_SETUP_RETR, 0x5F);
        initOk &= syncWriteReg(NRF_REG_RF_CH, NRF_CHANNEL);
        initOk &= syncWriteReg(NRF_REG_RF_SETUP, 0x24); // 250 kbps + PA_HIGH (-6 dBm)
        initOk &= syncWriteReg(NRF_REG_FEATURE, 0x00);
        initOk &= syncWriteReg(NRF_REG_DYNPD, 0x00);
        initOk &= syncWriteReg(NRF_REG_RX_PW_P0, sizeof(NrfPacket));
        initOk &= syncWriteReg(NRF_REG_RX_PW_P1, sizeof(NrfPacket));
        initOk &= syncWriteRegBuf(NRF_REG_TX_ADDR, NRF_ADDR_MASTER_TO_SLAVE, 5);
        initOk &= syncWriteRegBuf(NRF_REG_RX_ADDR_P0, NRF_ADDR_MASTER_TO_SLAVE, 5);
        initOk &= syncWriteRegBuf(NRF_REG_RX_ADDR_P1, NRF_ADDR_SLAVE_TO_MASTER, 5);
        initOk &= syncTransfer(NRF_CMD_FLUSH_TX, nullptr, nullptr, 0);
        initOk &= syncTransfer(NRF_CMD_FLUSH_RX, nullptr, nullptr, 0);
        initOk &= syncWriteReg(NRF_REG_STATUS, NRF_STATUS_CLR_ALL);
        initOk &= syncWriteReg(NRF_REG_CONFIG, NRF_CONFIG_PTX);
        _hal.delayUs(_hal.ctx, 5000UL); // Tpd2stby (max 4.5 ms)

        bool rfOk = false;
        bool chOk = false;
        uint8_t rfSetup = syncReadReg(NRF_REG_RF_SETUP, &rfOk);
        uint8_t rfCh    = syncReadReg(NRF_REG_RF_CH, &chOk);
        if (!initOk || !rfOk || !chOk || (rfSetup & 0x26U) != 0x24U || rfCh != NRF_CHANNEL) {
            _ready        = false;
            _ptxConfirmed = false;
            return false;
        }

        _ready               = true;
        _isRxMode            = false;
        _ptxConfirmed        = true;
        _pendingResetCleanup = false;
        _phase               = PHASE_IDLE;
        return true;
    }

    bool isReady() const { return _ready; }
    bool isPendingResetCleanup() const { return _pendingResetCleanup; }
    Phase getPhase() const { return _phase; }
    bool isRxMode() const { return _isRxMode; }
    bool isPtxConfirmed() const { return _ptxConfirmed; }
    uint8_t getInFlightCount() const { return _inFlightCount; }
    uint32_t getLastWaitUs() const { return _lastWaitUs; }
    uint32_t getMaxWaitUs() const { return _maxWaitUs; }
    bool isSlotInUse(uint8_t idx) const { return (idx < MAX_SLOTS) ? _slots[idx].inUse : false; }

    bool consumeQueueFullWarning() {
        bool v = _queueFullWarn;
        _queueFullWarn = false;
        return v;
    }

    bool syncTransfer(uint8_t cmd, const uint8_t* txData, uint8_t* rxData, size_t dataLen) {
        if (!_hal.syncTrans || dataLen > DMA_MAX_DATA_BYTES || _inFlightCount > 0 || _pendingResetCleanup) {
            return false;
        }
        Slot& slot = _slots[0];
        if (slot.inUse || !slot.txBuf || !slot.rxBuf) {
            return false;
        }

        memset(slot.txBuf, 0, DMA_FRAME_BYTES);
        memset(slot.rxBuf, 0, DMA_FRAME_BYTES);
        slot.txBuf[0] = cmd;
        if (txData && dataLen > 0) {
            memcpy(slot.txBuf + 1, txData, dataLen);
        }

        size_t totalBytes = 1U + dataLen;
        if (!_hal.syncTrans(_hal.ctx, 0, totalBytes)) {
            return false;
        }
        if (rxData && dataLen > 0) {
            memcpy(rxData, slot.rxBuf + 1, dataLen);
        }
        return true;
    }

    bool syncWriteReg(uint8_t reg, uint8_t val) {
        return syncTransfer(static_cast<uint8_t>(NRF_CMD_W_REGISTER | (reg & 0x1FU)), &val, nullptr, 1);
    }

    bool syncWriteRegBuf(uint8_t reg, const uint8_t* buf, size_t len) {
        return syncTransfer(static_cast<uint8_t>(NRF_CMD_W_REGISTER | (reg & 0x1FU)), buf, nullptr, len);
    }

    uint8_t syncReadReg(uint8_t reg, bool* okOut = nullptr) {
        uint8_t val = 0;
        bool ok = syncTransfer(static_cast<uint8_t>(NRF_CMD_R_REGISTER | (reg & 0x1FU)), nullptr, &val, 1);
        if (okOut) {
            *okOut = ok;
        }
        return val;
    }

    bool queueTransmitPacket(const NrfPacket& pkt) {
        if (!_ready || !_dmaTxPacket) {
            return false;
        }

        uint32_t stepWaitUs = 0;

        // Előbb próbáljuk nem-blokkolóan begyűjteni a lezárult tranzakciókat
        if (_pendingResetCleanup) {
            drainAbortedInFlight(0, &stepWaitUs);
            if (_inFlightCount == 0) {
                executeSyncRadioCleanup(&stepWaitUs);
            }
            if (stepWaitUs > 0) {
                recordWaitUs(stepWaitUs);
            }
            if (_pendingResetCleanup) {
                // Hardveres DMA még mindig foglalva: NE írjuk felül és NE küldjünk új TX-et
                return false;
            }
        } else {
            // Ha előző RX/stopListening kész op még a sorban van, ürítsük a gyűrűspufferbe
            // és csak egy már aktív TX közbeni átfedést tiltunk
            drainCompletedToDeferred(0, &stepWaitUs);
            for (uint8_t i = 0; i < _deferredCount; i++) {
                const DeferredEvent& ev = peekDeferred(i);
                if (ev.op == OP_PTX_CONFIG || ev.op == OP_TX_CONFIG) {
                    _isRxMode     = false;
                    _ptxConfirmed = true;
                } else if (ev.op == OP_RX_CONFIG) {
                    _isRxMode     = true;
                    _ptxConfirmed = false;
                }
            }
            _deferredHead  = 0;
            _deferredCount = 0;
        }

        // Ha aktív TXPAYLOAD / TX prep lépés van folyamatban a buszon, ne írjuk felül a _dmaTxPacket-et
        if (hasInFlightTxPrepOrPayload()) {
            if (stepWaitUs > 0) {
                recordWaitUs(stepWaitUs);
            }
            return false;
        }

        memcpy(_dmaTxPacket, &pkt, sizeof(NrfPacket));
        _pendingTxPkt = pkt;
        _hal.setCe(_hal.ctx, false);

        _pendingStepOp           = OP_NONE;
        _needRxConfigAfterClr    = false;
        _needStatusClrAfterFlush = false;
        _pendingPostRxClr        = false;

        HalStatus qSt = HAL_OK;
        if (_isRxMode || !_ptxConfirmed) {
            _phase = PHASE_TX_PREP_CONFIG;
            uint8_t ptxCfg = NRF_CONFIG_PTX;
            qSt = queueOp(OP_TX_CONFIG,
                          static_cast<uint8_t>(NRF_CMD_W_REGISTER | NRF_REG_CONFIG),
                          &ptxCfg,
                          1,
                          &stepWaitUs);
        } else {
            _phase = PHASE_TX_PREP_STATUS;
            uint8_t clrVal = NRF_STATUS_CLR_ALL;
            qSt = queueOp(OP_TX_STATUS_CLR,
                          static_cast<uint8_t>(NRF_CMD_W_REGISTER | NRF_REG_STATUS),
                          &clrVal,
                          1,
                          &stepWaitUs);
        }

        if (stepWaitUs > 0) {
            recordWaitUs(stepWaitUs);
        }
        return (qSt == HAL_OK);
    }

    PollResult poll(NrfPacket* outRxPacket) {
        PollResult res{};
        if (!_ready) {
            return res;
        }

        uint32_t accumWaitUs = 0;
        bool hadAnyWaitEvent = false;

        // 1. Ha sikertelen flushAndDrain utáni kényszerített drain fut, kizárólag lezárunk
        if (_pendingResetCleanup) {
            if (drainAbortedInFlight(0, &accumWaitUs)) {
                hadAnyWaitEvent = true;
            }
            if (_inFlightCount == 0) {
                if (executeSyncRadioCleanup(&accumWaitUs)) {
                    hadAnyWaitEvent = true;
                }
            }
            if (_queueFullWarn) {
                res.queueFullWarn = true;
            }
            if (hadAnyWaitEvent || accumWaitUs > 0) {
                recordWaitUs(accumWaitUs);
                res.hasTimingUpdate = true;
                res.dmaWaitUs       = accumWaitUs;
            } else {
                res.dmaWaitUs = _lastWaitUs;
            }
            return res;
        }

        // 2. ISR flag vagy függő tranzakciók nem-blokkoló begyűjtése (timeout = 0)
        bool dmaDone = (_hal.dmaDoneFlag != nullptr) ? *_hal.dmaDoneFlag : false;
        if (dmaDone || _inFlightCount > 0) {
            uint8_t drained = drainCompletedToDeferred(0, &accumWaitUs);
            if (drained > 0) {
                hadAnyWaitEvent = true;
            }
        }

        // 3. Összes kész esemény (beleértve a queueOp alatt mentetteket is!) feldolgozása
        while (_deferredCount > 0) {
            const DeferredEvent& head = peekDeferred(0);
            // Ha már ebben a poll()-ban adtunk vissza RX csomagot, a következőt hagyjuk a soron következő poll()-ra,
            // hogy egyetlen vett csomag se íródjon felül!
            if (head.op == OP_RX_PAYLOAD && res.rxPacketReady) {
                break;
            }
            DeferredEvent ev = popDeferred();
            processCompletedEvent(ev, res, outRxPacket, &accumWaitUs, &hadAnyWaitEvent);
        }

        // 4. Ha korábban sor-telítődés miatt elmaradt egy állapotgép lépés, pótoljuk amint van szabad slot
        if (_pendingStepOp != OP_NONE && _inFlightCount < _queueSize) {
            retryPendingStepOp(&accumWaitUs, &hadAnyWaitEvent, res);
        }
        if (_pendingPostRxClr && _inFlightCount < _queueSize) {
            uint8_t clrRx = NRF_STATUS_RX_DR;
            HalStatus st = queueOp(OP_IGNORE,
                                   static_cast<uint8_t>(NRF_CMD_W_REGISTER | NRF_REG_STATUS),
                                   &clrRx,
                                   1,
                                   &accumWaitUs);
            hadAnyWaitEvent = true;
            if (st == HAL_OK) {
                _pendingPostRxClr = false;
            } else if (st == HAL_ERROR) {
                _pendingPostRxClr = false;
            }
        }

        // 5. Ha üres az SPI sor és nincs függő lépés, időzített 200 us-os STATUS lekérdezés
        if (_inFlightCount == 0 && _pendingStepOp == OP_NONE && _deferredCount == 0) {
            uint32_t nowUs = _hal.getMicros(_hal.ctx);
            if (_phase == PHASE_TX_AIR_WAIT &&
                static_cast<uint32_t>(nowUs - _lastPollUs) >= NRF_POLL_INTERVAL_US) {
                _lastPollUs = nowUs;
                queueOp(OP_POLL_TX_STATUS, NRF_CMD_NOP, nullptr, 0, &accumWaitUs);
                hadAnyWaitEvent = true;
            } else if (_phase == PHASE_RX_LISTEN &&
                       static_cast<uint32_t>(nowUs - _lastPollUs) >= NRF_POLL_INTERVAL_US) {
                _lastPollUs = nowUs;
                queueOp(OP_POLL_RX_STATUS, NRF_CMD_NOP, nullptr, 0, &accumWaitUs);
                hadAnyWaitEvent = true;
            }
        }

        if (_queueFullWarn) {
            res.queueFullWarn = true;
        }

        if (hadAnyWaitEvent || accumWaitUs > 0) {
            recordWaitUs(accumWaitUs);
            res.hasTimingUpdate = true;
            res.dmaWaitUs       = accumWaitUs;
        } else {
            res.dmaWaitUs = _lastWaitUs;
        }

        return res;
    }

    void stopListening() {
        if (_hal.setCe) {
            _hal.setCe(_hal.ctx, false);
        }
        _phase                = PHASE_IDLE;
        _pendingStepOp        = OP_NONE;
        _needRxConfigAfterClr = false;
        if (_ready && (_isRxMode || !_ptxConfirmed) && !_pendingResetCleanup) {
            uint8_t ptxCfg = NRF_CONFIG_PTX;
            uint32_t w = 0;
            HalStatus st = queueOp(OP_PTX_CONFIG,
                                   static_cast<uint8_t>(NRF_CMD_W_REGISTER | NRF_REG_CONFIG),
                                   &ptxCfg,
                                   1,
                                   &w);
            if (w > 0) {
                recordWaitUs(w);
            }
            if (st != HAL_OK) {
                // Nem sikerült sorba tenni -> maradjon _ptxConfirmed = false, hogy a következő TX beállítsa
                _ptxConfirmed = false;
            }
        }
    }

    uint32_t flushAndDrain() {
        if (_hal.setCe) {
            _hal.setCe(_hal.ctx, false);
        }

        _pendingStepOp           = OP_NONE;
        _needRxConfigAfterClr    = false;
        _needStatusClrAfterFlush = false;
        _pendingPostRxClr        = false;
        _deferredHead            = 0;
        _deferredCount           = 0;

        if (!_ready) {
            _phase               = PHASE_IDLE;
            _isRxMode            = false;
            _ptxConfirmed        = false;
            _pendingResetCleanup = false;
            return 0;
        }

        uint32_t totalWaitUs = 0;
        uint32_t waitTicks   = _hal.msToTicks ? _hal.msToTicks(_hal.ctx, 5U) : 5U;
        uint8_t  safetyCount = 0;

        while (_inFlightCount > 0 && safetyCount < (_queueSize + 2U)) {
            uint8_t slotIdx = 0;
            HalStatus st = drainOneCompleted(waitTicks, &slotIdx, &totalWaitUs);
            if (st != HAL_OK) {
                break;
            }
            _slots[slotIdx].op = OP_NONE;
            safetyCount++;
        }

        if (_inFlightCount > 0) {
            // KRITIKUS BIZTONSÁGI VÉDELEM:
            // Legalább egy tranzakció még az ESP-IDF SPI / GDMA driver tulajdonában van!
            // Tilos a slot->inUse / _inFlightCount kényszerített törlése vagy spi_device_transmit() hívása.
            _pendingResetCleanup = true;
            _isRxMode            = false;
            _ptxConfirmed        = false;
            _phase               = PHASE_IDLE;
            recordWaitUs(totalWaitUs);
            return totalWaitUs;
        }

        executeSyncRadioCleanup(&totalWaitUs);
        recordWaitUs(totalWaitUs);
        return totalWaitUs;
    }

private:
    struct DeferredEvent {
        SlotOp  op = OP_NONE;
        uint8_t status = 0;
        uint8_t payload[DMA_MAX_DATA_BYTES]{};
    };

    Hal          _hal{};
    uint8_t      _queueSize               = MAX_SLOTS;
    bool         _ready                   = false;
    bool         _isRxMode                = false;
    bool         _ptxConfirmed            = false;
    bool         _pendingResetCleanup     = false;
    Phase        _phase                   = PHASE_IDLE;
    NrfPacket    _pendingTxPkt{};
    NrfPacket*   _dmaTxPacket             = nullptr;
    NrfPacket*   _dmaRxPacket             = nullptr;
    Slot         _slots[MAX_SLOTS]{};
    uint8_t      _inFlightCount           = 0;
    uint32_t     _lastPollUs              = 0;
    uint32_t     _lastWaitUs              = 0;
    uint32_t     _maxWaitUs               = 0;
    bool         _queueFullWarn           = false;

    SlotOp       _pendingStepOp           = OP_NONE;
    bool         _needRxConfigAfterClr    = false;
    bool         _needStatusClrAfterFlush = false;
    bool         _pendingPostRxClr        = false;

    DeferredEvent _deferred[DEFERRED_CAP]{};
    uint8_t       _deferredHead           = 0;
    uint8_t       _deferredCount          = 0;

    void recordWaitUs(uint32_t us) {
        _lastWaitUs = us;
        if (us > _maxWaitUs) {
            _maxWaitUs = us;
        }
    }

    bool hasInFlightTxPrepOrPayload() const {
        for (uint8_t i = 0; i < _queueSize; i++) {
            if (_slots[i].inUse) {
                SlotOp op = _slots[i].op;
                if (op == OP_TX_CONFIG || op == OP_TX_STATUS_CLR || op == OP_TX_PAYLOAD) {
                    return true;
                }
            }
        }
        return false;
    }

    bool pushDeferred(const Slot& slot) {
        if (slot.op == OP_NONE || slot.op == OP_IGNORE) {
            return true;
        }
        if (_deferredCount >= DEFERRED_CAP) {
            return false;
        }
        uint8_t tail = static_cast<uint8_t>((_deferredHead + _deferredCount) % DEFERRED_CAP);
        _deferred[tail].op     = slot.op;
        _deferred[tail].status = slot.rxBuf[0];
        if (slot.op == OP_RX_PAYLOAD) {
            memcpy(_deferred[tail].payload, slot.rxBuf + 1, sizeof(NrfPacket));
        }
        _deferredCount++;
        return true;
    }

    const DeferredEvent& peekDeferred(uint8_t offset) const {
        uint8_t idx = static_cast<uint8_t>((_deferredHead + offset) % DEFERRED_CAP);
        return _deferred[idx];
    }

    DeferredEvent popDeferred() {
        DeferredEvent ev = _deferred[_deferredHead];
        _deferredHead    = static_cast<uint8_t>((_deferredHead + 1U) % DEFERRED_CAP);
        _deferredCount--;
        return ev;
    }

    HalStatus drainOneCompleted(uint32_t waitTicks, uint8_t* outSlotIdx, uint32_t* accumWaitUs) {
        if (!_hal.getTransResult || _inFlightCount == 0) {
            return HAL_TIMEOUT;
        }
        uint8_t slotIdx = MAX_SLOTS;
        uint32_t t0 = _hal.getMicros(_hal.ctx);
        HalStatus err = _hal.getTransResult(_hal.ctx, waitTicks, &slotIdx);
        uint32_t dtUs = static_cast<uint32_t>(_hal.getMicros(_hal.ctx) - t0);
        if (accumWaitUs) {
            *accumWaitUs += dtUs;
        }
        if (err != HAL_OK || slotIdx >= _queueSize) {
            return err;
        }

        _slots[slotIdx].inUse = false;
        if (_inFlightCount > 0) {
            _inFlightCount--;
        }
        if (_inFlightCount == 0 && _hal.dmaDoneFlag) {
            *_hal.dmaDoneFlag = false;
        }
        if (outSlotIdx) {
            *outSlotIdx = slotIdx;
        }
        return HAL_OK;
    }

    uint8_t drainCompletedToDeferred(uint32_t waitTicks, uint32_t* accumWaitUs) {
        uint8_t count = 0;
        while (_inFlightCount > 0 && _deferredCount < DEFERRED_CAP) {
            uint8_t slotIdx = 0;
            if (drainOneCompleted(waitTicks, &slotIdx, accumWaitUs) != HAL_OK) {
                break;
            }
            pushDeferred(_slots[slotIdx]);
            _slots[slotIdx].op = OP_NONE;
            count++;
        }
        return count;
    }

    bool drainAbortedInFlight(uint32_t waitTicks, uint32_t* accumWaitUs) {
        bool drainedAny = false;
        while (_inFlightCount > 0) {
            uint8_t slotIdx = 0;
            if (drainOneCompleted(waitTicks, &slotIdx, accumWaitUs) != HAL_OK) {
                break;
            }
            _slots[slotIdx].op = OP_NONE;
            drainedAny = true;
        }
        return drainedAny;
    }

    bool executeSyncRadioCleanup(uint32_t* totalWaitUs) {
        if (_inFlightCount > 0) {
            return false;
        }
        if (_hal.dmaDoneFlag) {
            *_hal.dmaDoneFlag = false;
        }
        _isRxMode            = false;
        _phase               = PHASE_IDLE;
        _pendingResetCleanup = false;

        uint32_t t0 = _hal.getMicros(_hal.ctx);
        bool ok = true;
        ok &= syncTransfer(NRF_CMD_FLUSH_TX, nullptr, nullptr, 0);
        ok &= syncTransfer(NRF_CMD_FLUSH_RX, nullptr, nullptr, 0);
        ok &= syncWriteReg(NRF_REG_STATUS, NRF_STATUS_CLR_ALL);
        ok &= syncWriteReg(NRF_REG_CONFIG, NRF_CONFIG_PTX);
        if (totalWaitUs) {
            *totalWaitUs += static_cast<uint32_t>(_hal.getMicros(_hal.ctx) - t0);
        }
        _ptxConfirmed = ok;
        return true;
    }

    HalStatus queueOp(SlotOp op, uint8_t cmd, const uint8_t* data, size_t dataLen, uint32_t* accumWaitUs) {
        if (!_ready || !_hal.queueTrans || dataLen > DMA_MAX_DATA_BYTES || _pendingResetCleanup) {
            return HAL_ERROR;
        }

        // Ha tele a sor, próbáljunk nem-blokkoló módon kész tranzakciókat begyűjteni,
        // DE a befejezett op-ot SOHA nem dobjuk el, hanem a _deferred gyűrűspufferbe tesszük!
        if (_inFlightCount >= _queueSize) {
            drainCompletedToDeferred(0, accumWaitUs);
        }

        if (_inFlightCount >= _queueSize) {
            _queueFullWarn = true;
            return HAL_BUSY;
        }

        int freeIdx = -1;
        for (uint8_t i = 0; i < _queueSize; i++) {
            if (!_slots[i].inUse) {
                freeIdx = static_cast<int>(i);
                break;
            }
        }
        if (freeIdx < 0) {
            _queueFullWarn = true;
            return HAL_BUSY;
        }

        Slot& freeSlot = _slots[freeIdx];
        memset(freeSlot.txBuf, 0, DMA_FRAME_BYTES);
        memset(freeSlot.rxBuf, 0, DMA_FRAME_BYTES);
        freeSlot.txBuf[0] = cmd;
        if (data && dataLen > 0) {
            memcpy(freeSlot.txBuf + 1, data, dataLen);
        }

        freeSlot.op         = op;
        freeSlot.inUse      = true;
        freeSlot.queuedAtUs = _hal.getMicros(_hal.ctx);
        _inFlightCount++;

        size_t totalBytes = 1U + dataLen;
        uint32_t t0       = _hal.getMicros(_hal.ctx);
        HalStatus err     = _hal.queueTrans(_hal.ctx, static_cast<uint8_t>(freeIdx), totalBytes);
        uint32_t dtUs     = static_cast<uint32_t>(_hal.getMicros(_hal.ctx) - t0);
        if (accumWaitUs) {
            *accumWaitUs += dtUs;
        }

        if (err != HAL_OK) {
            freeSlot.inUse = false;
            freeSlot.op    = OP_NONE;
            if (_inFlightCount > 0) {
                _inFlightCount--;
            }
            if (err == HAL_BUSY || err == HAL_TIMEOUT) {
                _queueFullWarn = true;
                return HAL_BUSY;
            }
            return HAL_ERROR;
        }

        return HAL_OK;
    }

    void queueStepOrDefer(SlotOp op,
                          uint8_t cmd,
                          const uint8_t* data,
                          size_t dataLen,
                          uint32_t* accumWaitUs,
                          bool* hadAnyWaitEvent,
                          PollResult& res) {
        HalStatus st = queueOp(op, cmd, data, dataLen, accumWaitUs);
        *hadAnyWaitEvent = true;
        if (st == HAL_BUSY) {
            _pendingStepOp = op;
        } else if (st == HAL_ERROR) {
            _pendingStepOp = OP_NONE;
            res.txFailedNow = true;
            _phase = PHASE_TX_FAILED;
        }
    }

    void retryPendingStepOp(uint32_t* accumWaitUs, bool* hadAnyWaitEvent, PollResult& res) {
        SlotOp op = _pendingStepOp;
        _pendingStepOp = OP_NONE;
        switch (op) {
            case OP_TX_CONFIG: {
                uint8_t ptxCfg = NRF_CONFIG_PTX;
                queueStepOrDefer(OP_TX_CONFIG, NRF_CMD_W_REGISTER | NRF_REG_CONFIG, &ptxCfg, 1,
                                 accumWaitUs, hadAnyWaitEvent, res);
                break;
            }
            case OP_TX_STATUS_CLR: {
                uint8_t clrVal = NRF_STATUS_CLR_ALL;
                queueStepOrDefer(OP_TX_STATUS_CLR, NRF_CMD_W_REGISTER | NRF_REG_STATUS, &clrVal, 1,
                                 accumWaitUs, hadAnyWaitEvent, res);
                break;
            }
            case OP_TX_PAYLOAD:
                queueStepOrDefer(OP_TX_PAYLOAD,
                                 NRF_CMD_W_TX_PAYLOAD,
                                 reinterpret_cast<const uint8_t*>(_dmaTxPacket),
                                 sizeof(NrfPacket),
                                 accumWaitUs, hadAnyWaitEvent, res);
                break;
            case OP_POLL_TX_STATUS:
                queueStepOrDefer(OP_POLL_TX_STATUS, NRF_CMD_NOP, nullptr, 0,
                                 accumWaitUs, hadAnyWaitEvent, res);
                break;
            case OP_RX_STATUS_CLR: {
                uint8_t clrVal = NRF_STATUS_CLR_ALL;
                queueStepOrDefer(OP_RX_STATUS_CLR, NRF_CMD_W_REGISTER | NRF_REG_STATUS, &clrVal, 1,
                                 accumWaitUs, hadAnyWaitEvent, res);
                break;
            }
            case OP_RX_CONFIG: {
                uint8_t prxCfg = NRF_CONFIG_PRX;
                queueStepOrDefer(OP_RX_CONFIG, NRF_CMD_W_REGISTER | NRF_REG_CONFIG, &prxCfg, 1,
                                 accumWaitUs, hadAnyWaitEvent, res);
                break;
            }
            case OP_RX_PAYLOAD:
                queueStepOrDefer(OP_RX_PAYLOAD, NRF_CMD_R_RX_PAYLOAD, nullptr, sizeof(NrfPacket),
                                 accumWaitUs, hadAnyWaitEvent, res);
                break;
            case OP_FLUSH_TX:
                queueStepOrDefer(OP_FLUSH_TX, NRF_CMD_FLUSH_TX, nullptr, 0,
                                 accumWaitUs, hadAnyWaitEvent, res);
                break;
            case OP_IGNORE: {
                // MAX_RT utáni STATUS törlés pótlása
                uint8_t clrVal = NRF_STATUS_CLR_ALL;
                queueStepOrDefer(OP_IGNORE, NRF_CMD_W_REGISTER | NRF_REG_STATUS, &clrVal, 1,
                                 accumWaitUs, hadAnyWaitEvent, res);
                break;
            }
            default:
                break;
        }
    }

    void processCompletedEvent(const DeferredEvent& ev,
                               PollResult& res,
                               NrfPacket* outRxPacket,
                               uint32_t* accumWaitUs,
                               bool* hadAnyWaitEvent) {
        switch (ev.op) {
            case OP_PTX_CONFIG:
                _isRxMode     = false;
                _ptxConfirmed = true;
                break;

            case OP_TX_CONFIG: {
                _isRxMode     = false;
                _ptxConfirmed = true;
                _phase        = PHASE_TX_PREP_STATUS;
                uint8_t clrVal = NRF_STATUS_CLR_ALL;
                queueStepOrDefer(OP_TX_STATUS_CLR,
                                 static_cast<uint8_t>(NRF_CMD_W_REGISTER | NRF_REG_STATUS),
                                 &clrVal,
                                 1,
                                 accumWaitUs,
                                 hadAnyWaitEvent,
                                 res);
                break;
            }

            case OP_TX_STATUS_CLR: {
                _phase = PHASE_TX_PAYLOAD_PENDING;
                queueStepOrDefer(OP_TX_PAYLOAD,
                                 NRF_CMD_W_TX_PAYLOAD,
                                 reinterpret_cast<const uint8_t*>(_dmaTxPacket),
                                 sizeof(NrfPacket),
                                 accumWaitUs,
                                 hadAnyWaitEvent,
                                 res);
                break;
            }

            case OP_TX_PAYLOAD: {
                // Legalább 10us CE impulzus indítja a rádióadást az NRF24-ben
                _hal.setCe(_hal.ctx, true);
                _hal.delayUs(_hal.ctx, 15UL);
                _hal.setCe(_hal.ctx, false);
                _phase      = PHASE_TX_AIR_WAIT;
                _lastPollUs = _hal.getMicros(_hal.ctx);
                queueStepOrDefer(OP_POLL_TX_STATUS,
                                 NRF_CMD_NOP,
                                 nullptr,
                                 0,
                                 accumWaitUs,
                                 hadAnyWaitEvent,
                                 res);
                break;
            }

            case OP_POLL_TX_STATUS: {
                if (_phase != PHASE_TX_AIR_WAIT) {
                    break;
                }
                if ((ev.status & NRF_STATUS_TX_DS) != 0) {
                    res.txAckedNow = true;
                    _phase         = PHASE_RX_SWITCH_WAIT;
                    _ptxConfirmed  = false;
                    uint8_t clrVal = NRF_STATUS_CLR_ALL;
                    uint8_t prxCfg = NRF_CONFIG_PRX;
                    HalStatus s1 = queueOp(OP_RX_STATUS_CLR,
                                           static_cast<uint8_t>(NRF_CMD_W_REGISTER | NRF_REG_STATUS),
                                           &clrVal,
                                           1,
                                           accumWaitUs);
                    *hadAnyWaitEvent = true;
                    if (s1 == HAL_OK) {
                        HalStatus s2 = queueOp(OP_RX_CONFIG,
                                               static_cast<uint8_t>(NRF_CMD_W_REGISTER | NRF_REG_CONFIG),
                                               &prxCfg,
                                               1,
                                               accumWaitUs);
                        if (s2 == HAL_BUSY) {
                            _pendingStepOp = OP_RX_CONFIG;
                        } else if (s2 == HAL_ERROR) {
                            res.txFailedNow = true;
                            _phase = PHASE_TX_FAILED;
                        }
                    } else if (s1 == HAL_BUSY) {
                        _pendingStepOp        = OP_RX_STATUS_CLR;
                        _needRxConfigAfterClr = true;
                    } else {
                        res.txFailedNow = true;
                        _phase = PHASE_TX_FAILED;
                    }
                } else if ((ev.status & NRF_STATUS_MAX_RT) != 0) {
                    res.txFailedNow = true;
                    _phase          = PHASE_TX_FAILED;
                    uint8_t clrVal  = NRF_STATUS_CLR_ALL;
                    HalStatus s1 = queueOp(OP_FLUSH_TX, NRF_CMD_FLUSH_TX, nullptr, 0, accumWaitUs);
                    *hadAnyWaitEvent = true;
                    if (s1 == HAL_OK) {
                        HalStatus s2 = queueOp(OP_IGNORE,
                                               static_cast<uint8_t>(NRF_CMD_W_REGISTER | NRF_REG_STATUS),
                                               &clrVal,
                                               1,
                                               accumWaitUs);
                        if (s2 == HAL_BUSY) {
                            _pendingStepOp = OP_IGNORE;
                        }
                    } else if (s1 == HAL_BUSY) {
                        _pendingStepOp           = OP_FLUSH_TX;
                        _needStatusClrAfterFlush = true;
                    }
                }
                break;
            }

            case OP_RX_STATUS_CLR: {
                if (_needRxConfigAfterClr && _phase == PHASE_RX_SWITCH_WAIT) {
                    _needRxConfigAfterClr = false;
                    uint8_t prxCfg = NRF_CONFIG_PRX;
                    queueStepOrDefer(OP_RX_CONFIG,
                                     static_cast<uint8_t>(NRF_CMD_W_REGISTER | NRF_REG_CONFIG),
                                     &prxCfg,
                                     1,
                                     accumWaitUs,
                                     hadAnyWaitEvent,
                                     res);
                } else {
                    _needRxConfigAfterClr = false;
                }
                break;
            }

            case OP_FLUSH_TX: {
                if (_needStatusClrAfterFlush) {
                    _needStatusClrAfterFlush = false;
                    uint8_t clrVal = NRF_STATUS_CLR_ALL;
                    queueStepOrDefer(OP_IGNORE,
                                     static_cast<uint8_t>(NRF_CMD_W_REGISTER | NRF_REG_STATUS),
                                     &clrVal,
                                     1,
                                     accumWaitUs,
                                     hadAnyWaitEvent,
                                     res);
                }
                break;
            }

            case OP_RX_CONFIG:
                _isRxMode     = true;
                _ptxConfirmed = false;
                if (_phase == PHASE_RX_SWITCH_WAIT) {
                    _hal.setCe(_hal.ctx, true);
                    _phase      = PHASE_RX_LISTEN;
                    _lastPollUs = _hal.getMicros(_hal.ctx);
                }
                break;

            case OP_POLL_RX_STATUS: {
                if (_phase != PHASE_RX_LISTEN) {
                    break;
                }
                bool fifoHasPacket = ((ev.status & NRF_STATUS_RX_P_NO) != NRF_STATUS_RX_P_NO);
                if (fifoHasPacket) {
                    _phase = PHASE_RX_PAYLOAD_PENDING;
                    queueStepOrDefer(OP_RX_PAYLOAD,
                                     NRF_CMD_R_RX_PAYLOAD,
                                     nullptr,
                                     sizeof(NrfPacket),
                                     accumWaitUs,
                                     hadAnyWaitEvent,
                                     res);
                } else if ((ev.status & NRF_STATUS_RX_DR) != 0) {
                    // Hamis/beragadt RX_DR üres RX FIFO mellett -> csak RX_DR törlése olvasás nélkül
                    uint8_t clrRx = NRF_STATUS_RX_DR;
                    queueOp(OP_IGNORE,
                            static_cast<uint8_t>(NRF_CMD_W_REGISTER | NRF_REG_STATUS),
                            &clrRx,
                            1,
                            accumWaitUs);
                    *hadAnyWaitEvent = true;
                }
                break;
            }

            case OP_RX_PAYLOAD: {
                memcpy(_dmaRxPacket, ev.payload, sizeof(NrfPacket));
                if (outRxPacket) {
                    memcpy(outRxPacket, _dmaRxPacket, sizeof(NrfPacket));
                }
                res.rxPacketReady = true;
                uint8_t clrRx = NRF_STATUS_RX_DR;
                HalStatus sClr = queueOp(OP_IGNORE,
                                         static_cast<uint8_t>(NRF_CMD_W_REGISTER | NRF_REG_STATUS),
                                         &clrRx,
                                         1,
                                         accumWaitUs);
                *hadAnyWaitEvent = true;
                if (sClr == HAL_BUSY) {
                    _pendingPostRxClr = true;
                }
                if (_phase == PHASE_RX_PAYLOAD_PENDING) {
                    _phase = PHASE_RX_LISTEN;
                }
                break;
            }

            case OP_IGNORE:
            default:
                break;
        }
    }
};
