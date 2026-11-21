#include "spi_dma.h"
#include <string.h>
#if __has_include(<esp_idf_version.h>)
#include <esp_idf_version.h>
#endif

// ============================================================================
// NRF24L01+ SPI PARANCSOK ÉS REGISZTEREK
// ============================================================================
namespace {
constexpr uint8_t NRF_CMD_R_REGISTER    = 0x00;
constexpr uint8_t NRF_CMD_W_REGISTER    = 0x20;
constexpr uint8_t NRF_CMD_R_RX_PAYLOAD  = 0x61;
constexpr uint8_t NRF_CMD_W_TX_PAYLOAD  = 0xA0;
constexpr uint8_t NRF_CMD_FLUSH_TX      = 0xE1;
constexpr uint8_t NRF_CMD_FLUSH_RX      = 0xE2;
constexpr uint8_t NRF_CMD_NOP           = 0xFF;

constexpr uint8_t NRF_REG_CONFIG        = 0x00;
constexpr uint8_t NRF_REG_EN_AA         = 0x01;
constexpr uint8_t NRF_REG_EN_RXADDR     = 0x02;
constexpr uint8_t NRF_REG_SETUP_AW      = 0x03;
constexpr uint8_t NRF_REG_SETUP_RETR    = 0x04;
constexpr uint8_t NRF_REG_RF_CH         = 0x05;
constexpr uint8_t NRF_REG_RF_SETUP      = 0x06;
constexpr uint8_t NRF_REG_STATUS        = 0x07;
constexpr uint8_t NRF_REG_RX_ADDR_P0    = 0x0A;
constexpr uint8_t NRF_REG_RX_ADDR_P1    = 0x0B;
constexpr uint8_t NRF_REG_TX_ADDR       = 0x10;
constexpr uint8_t NRF_REG_RX_PW_P0      = 0x11;
constexpr uint8_t NRF_REG_RX_PW_P1      = 0x12;
constexpr uint8_t NRF_REG_DYNPD         = 0x1C;
constexpr uint8_t NRF_REG_FEATURE       = 0x1D;

// CONFIG bitek: EN_CRC(0x08) | CRCO 16-bit(0x04) | PWR_UP(0x02) | PRIM_RX(0x01)
constexpr uint8_t NRF_CONFIG_PTX        = 0x0E;
constexpr uint8_t NRF_CONFIG_PRX        = 0x0F;

// STATUS bitek
constexpr uint8_t NRF_STATUS_RX_DR      = 0x40;
constexpr uint8_t NRF_STATUS_TX_DS      = 0x20;
constexpr uint8_t NRF_STATUS_MAX_RT     = 0x10;
constexpr uint8_t NRF_STATUS_CLR_ALL    = 0x70;
constexpr uint8_t NRF_STATUS_RX_P_NO    = 0x0E;

constexpr uint32_t NRF_POLL_INTERVAL_US = 200UL;
} // namespace

// ============================================================================
// SpiDmaBus — KÖZÖS SPI2_HOST GDMA BUSZ (NRF24 + JÖVŐBELI SD / TFT ESZKÖZÖK)
// ============================================================================
bool SpiDmaBus::begin(spi_host_device_t host,
                      int               mosiPin,
                      int               misoPin,
                      int               sclkPin,
                      spi_dma_chan_t    dmaChan,
                      int               maxTransferSz) {
    if (_initialized) {
        return (_host == host);
    }

    spi_bus_config_t busCfg{};
    busCfg.mosi_io_num   = mosiPin;
    busCfg.miso_io_num   = misoPin;
    busCfg.sclk_io_num   = sclkPin;
    busCfg.quadwp_io_num = -1;
    busCfg.quadhd_io_num = -1;
#if defined(ESP_IDF_VERSION_MAJOR) && (ESP_IDF_VERSION_MAJOR >= 5)
    busCfg.data4_io_num  = -1;
    busCfg.data5_io_num  = -1;
    busCfg.data6_io_num  = -1;
    busCfg.data7_io_num  = -1;
#endif
    busCfg.max_transfer_sz = maxTransferSz;

    esp_err_t err = spi_bus_initialize(host, &busCfg, dmaChan);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return false;
    }

    _host        = host;
    _initialized = true;
    return true;
}

bool SpiDmaBus::addDevice(int                  csPin,
                          uint32_t             clockHz,
                          uint8_t              mode,
                          uint8_t              queueSize,
                          transaction_cb_t     postCb,
                          spi_device_handle_t* outHandle) const {
    if (!_initialized || !outHandle) {
        return false;
    }

    spi_device_interface_config_t devCfg{};
    devCfg.command_bits   = 0;
    devCfg.address_bits   = 0;
    devCfg.dummy_bits     = 0;
    devCfg.mode           = mode;
    devCfg.clock_speed_hz = static_cast<int>(clockHz);
    devCfg.spics_io_num   = csPin;
    devCfg.queue_size     = queueSize;
    devCfg.post_cb        = postCb;

    esp_err_t err = spi_bus_add_device(_host, &devCfg, outHandle);
    return (err == ESP_OK);
}

// ============================================================================
// NrfDmaRadio — ISR POST CALLBACK (CSAK _dmaDone FLAG ÁLLÍTÁSA)
// ============================================================================
void IRAM_ATTR NrfDmaRadio::onDmaPostTransfer(spi_transaction_t* trans) {
    if (!trans || !trans->user) {
        return;
    }
    auto* slot = static_cast<DmaSlot*>(trans->user);
    if (slot->owner) {
        slot->owner->_dmaDone = true;
    }
}

void NrfDmaRadio::recordWaitUs(uint32_t us) {
    _lastWaitUs = us;
    if (us > _maxWaitUs) {
        _maxWaitUs = us;
    }
}

// ============================================================================
// INICIALIZÁLÁS (CSAK SETUP()-BAN HÍVÓDIK)
// ============================================================================
bool NrfDmaRadio::syncTransfer(uint8_t cmd, const uint8_t* txData, uint8_t* rxData, size_t dataLen) {
    if (!_handle || dataLen > NRF_DMA_BUFFER_BYTES) {
        return false;
    }
    DmaSlot& slot = _slots[0];
    if (!slot.txBuf || !slot.rxBuf) {
        return false;
    }

    memset(slot.txBuf, 0, DMA_FRAME_BYTES);
    memset(slot.rxBuf, 0, DMA_FRAME_BYTES);
    slot.txBuf[0] = cmd;
    if (txData && dataLen > 0) {
        memcpy(slot.txBuf + 1, txData, dataLen);
    }

    memset(&slot.trans, 0, sizeof(slot.trans));
    size_t totalBytes   = 1U + dataLen;
    slot.trans.length   = totalBytes * 8U;
    slot.trans.rxlength = totalBytes * 8U;
    slot.trans.tx_buffer = slot.txBuf;
    slot.trans.rx_buffer = slot.rxBuf;
    slot.trans.user      = nullptr;

    esp_err_t err = spi_device_transmit(_handle, &slot.trans);
    if (err != ESP_OK) {
        return false;
    }
    if (rxData && dataLen > 0) {
        memcpy(rxData, slot.rxBuf + 1, dataLen);
    }
    return true;
}

bool NrfDmaRadio::syncWriteReg(uint8_t reg, uint8_t val) {
    return syncTransfer(static_cast<uint8_t>(NRF_CMD_W_REGISTER | (reg & 0x1FU)), &val, nullptr, 1);
}

bool NrfDmaRadio::syncWriteRegBuf(uint8_t reg, const uint8_t* buf, size_t len) {
    return syncTransfer(static_cast<uint8_t>(NRF_CMD_W_REGISTER | (reg & 0x1FU)), buf, nullptr, len);
}

uint8_t NrfDmaRadio::syncReadReg(uint8_t reg, bool* okOut) {
    uint8_t val = 0;
    bool ok = syncTransfer(static_cast<uint8_t>(NRF_CMD_R_REGISTER | (reg & 0x1FU)), nullptr, &val, 1);
    if (okOut) {
        *okOut = ok;
    }
    return val;
}

bool NrfDmaRadio::begin(SpiDmaBus& bus,
                        uint8_t    cePin,
                        uint8_t    csnPin,
                        uint32_t   clockHz,
                        uint8_t    queueSize) {
    _cePin     = cePin;
    _csnPin    = csnPin;
    _queueSize = (queueSize > MAX_SLOTS) ? MAX_SLOTS : (queueSize == 0 ? 1 : queueSize);
    _ready     = false;
    _isRxMode  = false;
    _phase     = PHASE_IDLE;
    _inFlightCount = 0;
    _dmaDone   = false;
    _queueFullWarn = false;

    pinMode(_cePin, OUTPUT);
    digitalWrite(_cePin, LOW);

    if (!bus.isInitialized()) {
        return false;
    }

    // 32 byte-os DMA-képes NrfPacket pufferek és DMA tranzakciós slotok foglalása
    if (!_dmaTxPacket) {
        _dmaTxPacket = static_cast<NrfPacket*>(heap_caps_malloc(sizeof(NrfPacket), MALLOC_CAP_DMA));
    }
    if (!_dmaRxPacket) {
        _dmaRxPacket = static_cast<NrfPacket*>(heap_caps_malloc(sizeof(NrfPacket), MALLOC_CAP_DMA));
    }
    if (!_dmaTxPacket || !_dmaRxPacket) {
        return false;
    }
    memset(_dmaTxPacket, 0, sizeof(NrfPacket));
    memset(_dmaRxPacket, 0, sizeof(NrfPacket));

    for (uint8_t i = 0; i < MAX_SLOTS; i++) {
        _slots[i].inUse = false;
        _slots[i].op    = OP_NONE;
        _slots[i].owner = this;
        if (!_slots[i].txBuf) {
            _slots[i].txBuf = static_cast<uint8_t*>(heap_caps_malloc(DMA_FRAME_BYTES, MALLOC_CAP_DMA));
        }
        if (!_slots[i].rxBuf) {
            _slots[i].rxBuf = static_cast<uint8_t*>(heap_caps_malloc(DMA_FRAME_BYTES, MALLOC_CAP_DMA));
        }
        if (!_slots[i].txBuf || !_slots[i].rxBuf) {
            return false;
        }
        memset(_slots[i].txBuf, 0, DMA_FRAME_BYTES);
        memset(_slots[i].rxBuf, 0, DMA_FRAME_BYTES);
    }

    uint32_t clampedClock = (clockHz > 10000000UL) ? 10000000UL : clockHz;
    if (!_handle) {
        if (!bus.addDevice(static_cast<int>(_csnPin),
                           clampedClock,
                           0,
                           _queueSize,
                           &NrfDmaRadio::onDmaPostTransfer,
                           &_handle)) {
            return false;
        }
    }

    // Rádió modul stabilizáció + V5 RF24-kompatibilis regiszter konfiguráció
    delay(5);
    syncWriteReg(NRF_REG_CONFIG, 0x0C);          // EN_CRC | CRCO (16-bit CRC), power down
    syncWriteReg(NRF_REG_EN_AA, 0x3F);           // Auto-ACK minden pipe-on
    syncWriteReg(NRF_REG_EN_RXADDR, 0x03);       // Pipe 0 (Auto-ACK) + Pipe 1 (Slave->Master)
    syncWriteReg(NRF_REG_SETUP_AW, 0x03);        // 5 byte-os címek
    syncWriteReg(NRF_REG_SETUP_RETR, 0x5F);      // ARD=1500us (5), ARC=15 (RF24 setRetries(5,15))
    syncWriteReg(NRF_REG_RF_CH, NRF_CHANNEL);    // Csatorna 108
    syncWriteReg(NRF_REG_RF_SETUP, 0x24);        // 250 kbps (0x20) + PA_HIGH -6dBm (0x04)
    syncWriteReg(NRF_REG_FEATURE, 0x00);         // AckPayload KI, dinamikus payload KI
    syncWriteReg(NRF_REG_DYNPD, 0x00);
    syncWriteReg(NRF_REG_RX_PW_P0, sizeof(NrfPacket)); // 32 byte
    syncWriteReg(NRF_REG_RX_PW_P1, sizeof(NrfPacket)); // 32 byte
    syncWriteRegBuf(NRF_REG_TX_ADDR, NRF_ADDR_MASTER_TO_SLAVE, 5);
    syncWriteRegBuf(NRF_REG_RX_ADDR_P0, NRF_ADDR_MASTER_TO_SLAVE, 5);
    syncWriteRegBuf(NRF_REG_RX_ADDR_P1, NRF_ADDR_SLAVE_TO_MASTER, 5);
    syncTransfer(NRF_CMD_FLUSH_TX, nullptr, nullptr, 0);
    syncTransfer(NRF_CMD_FLUSH_RX, nullptr, nullptr, 0);
    syncWriteReg(NRF_REG_STATUS, NRF_STATUS_CLR_ALL);
    syncWriteReg(NRF_REG_CONFIG, NRF_CONFIG_PTX); // PWR_UP + PTX
    delay(5);                                     // Tpd2stby (max 4.5 ms)

    bool rfOk = false;
    bool chOk = false;
    uint8_t rfSetup = syncReadReg(NRF_REG_RF_SETUP, &rfOk);
    uint8_t rfCh    = syncReadReg(NRF_REG_RF_CH, &chOk);
    if (!rfOk || !chOk || (rfSetup & 0x26U) != 0x24U || rfCh != NRF_CHANNEL) {
        _ready = false;
        return false;
    }

    _ready    = true;
    _isRxMode = false;
    _phase    = PHASE_IDLE;
    return true;
}

// ============================================================================
// NEM-BLOKKOLÓ DMA TRANZAKCIÓ SORBA ÁLLÍTÁS ÉS LEKÉRDEZÉS
// ============================================================================
bool NrfDmaRadio::drainOneCompleted(TickType_t waitTicks, DmaSlot** outSlot, uint32_t* accumWaitUs) {
    if (outSlot) {
        *outSlot = nullptr;
    }
    if (!_handle || _inFlightCount == 0) {
        return false;
    }

    spi_transaction_t* doneTrans = nullptr;
    uint32_t t0 = micros();
    esp_err_t err = spi_device_get_trans_result(_handle, &doneTrans, waitTicks);
    uint32_t dtUs = static_cast<uint32_t>(micros() - t0);
    if (accumWaitUs) {
        *accumWaitUs += dtUs;
    }

    if (err != ESP_OK || !doneTrans) {
        return false;
    }

    auto* slot = static_cast<DmaSlot*>(doneTrans->user);
    if (slot) {
        slot->inUse = false;
        if (_inFlightCount > 0) {
            _inFlightCount--;
        }
        if (outSlot) {
            *outSlot = slot;
        }
    }
    if (_inFlightCount == 0) {
        _dmaDone = false;
    }
    return true;
}

bool NrfDmaRadio::queueOp(SlotOp op, uint8_t cmd, const uint8_t* data, size_t dataLen) {
    if (!_ready || !_handle || dataLen > NRF_DMA_BUFFER_BYTES) {
        return false;
    }

    // Ha tele a 3 elemű DMA sor, próbáljunk nem-blokkoló módon felszabadítani
    if (_inFlightCount >= _queueSize) {
        DmaSlot* drained = nullptr;
        uint32_t waitUs  = 0;
        drainOneCompleted(0, &drained, &waitUs);
        if (waitUs > 0) {
            recordWaitUs(waitUs);
        }
    }

    // Ha továbbra is 3 tranzakció várakozik a sorban -> DMA_QUEUE_FULL figyelmeztetés + legrégebbi eldobása
    if (_inFlightCount >= _queueSize) {
        _queueFullWarn   = true;
        DmaSlot* dropped = nullptr;
        uint32_t waitUs  = 0;
        drainOneCompleted(pdMS_TO_TICKS(1), &dropped, &waitUs);
        recordWaitUs(waitUs);
        if (_inFlightCount >= _queueSize) {
            return false;
        }
    }

    DmaSlot* freeSlot = nullptr;
    for (uint8_t i = 0; i < _queueSize; i++) {
        if (!_slots[i].inUse) {
            freeSlot = &_slots[i];
            break;
        }
    }
    if (!freeSlot) {
        _queueFullWarn = true;
        return false;
    }

    memset(freeSlot->txBuf, 0, DMA_FRAME_BYTES);
    memset(freeSlot->rxBuf, 0, DMA_FRAME_BYTES);
    freeSlot->txBuf[0] = cmd;
    if (data && dataLen > 0) {
        memcpy(freeSlot->txBuf + 1, data, dataLen);
    }

    memset(&freeSlot->trans, 0, sizeof(freeSlot->trans));
    size_t totalBytes          = 1U + dataLen;
    freeSlot->trans.length     = totalBytes * 8U;
    freeSlot->trans.rxlength   = totalBytes * 8U;
    freeSlot->trans.tx_buffer  = freeSlot->txBuf;
    freeSlot->trans.rx_buffer  = freeSlot->rxBuf;
    freeSlot->trans.user       = freeSlot;

    freeSlot->op         = op;
    freeSlot->inUse      = true;
    freeSlot->queuedAtUs = micros();
    _inFlightCount++;

    uint32_t t0 = micros();
    esp_err_t err = spi_device_queue_trans(_handle, &freeSlot->trans, 0);
    uint32_t dtUs = static_cast<uint32_t>(micros() - t0);
    recordWaitUs(dtUs);

    if (err != ESP_OK) {
        freeSlot->inUse = false;
        freeSlot->op    = OP_NONE;
        if (_inFlightCount > 0) {
            _inFlightCount--;
        }
        if (err == ESP_ERR_NO_MEM || err == ESP_ERR_TIMEOUT) {
            _queueFullWarn = true;
        }
        return false;
    }

    return true;
}

bool NrfDmaRadio::queueTransmitPacket(const NrfPacket& pkt) {
    if (!_ready) {
        return false;
    }

    // 32 byte-os NrfPacket DMA-képes tükörpufferbe másolása (nem stack-en)
    memcpy(_dmaTxPacket, &pkt, sizeof(NrfPacket));
    _pendingTxPkt = pkt;

    digitalWrite(_cePin, LOW);

    if (_isRxMode) {
        _phase = PHASE_TX_PREP_CONFIG;
        uint8_t ptxCfg = NRF_CONFIG_PTX;
        return queueOp(OP_TX_CONFIG, NRF_CMD_W_REGISTER | NRF_REG_CONFIG, &ptxCfg, 1);
    }

    _phase = PHASE_TX_PREP_STATUS;
    uint8_t clrVal = NRF_STATUS_CLR_ALL;
    return queueOp(OP_TX_STATUS_CLR, NRF_CMD_W_REGISTER | NRF_REG_STATUS, &clrVal, 1);
}

NrfDmaRadio::PollResult NrfDmaRadio::poll(NrfPacket* outRxPacket) {
    PollResult res{};
    if (!_ready) {
        return res;
    }

    if (_queueFullWarn) {
        res.queueFullWarn = true;
    }

    uint32_t accumWaitUs = 0;

    // A _dmaDone ISR flag vagy a kész tranzakciók nem-blokkoló begyűjtése (timeout = 0)
    if (_dmaDone || _inFlightCount > 0) {
        DmaSlot* doneSlot = nullptr;
        while (drainOneCompleted(0, &doneSlot, &accumWaitUs)) {
            res.hasTimingUpdate = true;
            if (!doneSlot) {
                continue;
            }
            SlotOp op      = doneSlot->op;
            uint8_t status = doneSlot->rxBuf[0];
            doneSlot->op   = OP_NONE;

            switch (op) {
                case OP_TX_CONFIG: {
                    _isRxMode = false;
                    _phase    = PHASE_TX_PREP_STATUS;
                    uint8_t clrVal = NRF_STATUS_CLR_ALL;
                    queueOp(OP_TX_STATUS_CLR, NRF_CMD_W_REGISTER | NRF_REG_STATUS, &clrVal, 1);
                    break;
                }

                case OP_TX_STATUS_CLR: {
                    _phase = PHASE_TX_PAYLOAD_PENDING;
                    queueOp(OP_TX_PAYLOAD,
                            NRF_CMD_W_TX_PAYLOAD,
                            reinterpret_cast<const uint8_t*>(_dmaTxPacket),
                            sizeof(NrfPacket));
                    break;
                }

                case OP_TX_PAYLOAD: {
                    // Legalább 10us CE impulzus indítja a rádióadást az NRF24-ben
                    digitalWrite(_cePin, HIGH);
                    delayMicroseconds(15);
                    digitalWrite(_cePin, LOW);
                    _phase      = PHASE_TX_AIR_WAIT;
                    _lastPollUs = micros();
                    queueOp(OP_POLL_TX_STATUS, NRF_CMD_NOP, nullptr, 0);
                    break;
                }

                case OP_POLL_TX_STATUS: {
                    if ((status & NRF_STATUS_TX_DS) != 0) {
                        res.txAckedNow = true;
                        _phase         = PHASE_RX_SWITCH_WAIT;
                        uint8_t clrVal = NRF_STATUS_CLR_ALL;
                        uint8_t prxCfg = NRF_CONFIG_PRX;
                        queueOp(OP_RX_STATUS_CLR, NRF_CMD_W_REGISTER | NRF_REG_STATUS, &clrVal, 1);
                        queueOp(OP_RX_CONFIG, NRF_CMD_W_REGISTER | NRF_REG_CONFIG, &prxCfg, 1);
                    } else if ((status & NRF_STATUS_MAX_RT) != 0) {
                        res.txFailedNow = true;
                        _phase          = PHASE_TX_FAILED;
                        uint8_t clrVal  = NRF_STATUS_CLR_ALL;
                        queueOp(OP_FLUSH_TX, NRF_CMD_FLUSH_TX, nullptr, 0);
                        queueOp(OP_IGNORE, NRF_CMD_W_REGISTER | NRF_REG_STATUS, &clrVal, 1);
                    }
                    break;
                }

                case OP_RX_CONFIG: {
                    _isRxMode   = true;
                    digitalWrite(_cePin, HIGH);
                    _phase      = PHASE_RX_LISTEN;
                    _lastPollUs = micros();
                    break;
                }

                case OP_POLL_RX_STATUS: {
                    bool rxAvailable = ((status & NRF_STATUS_RX_DR) != 0) ||
                                       ((status & NRF_STATUS_RX_P_NO) != NRF_STATUS_RX_P_NO);
                    if (rxAvailable) {
                        _phase = PHASE_RX_PAYLOAD_PENDING;
                        queueOp(OP_RX_PAYLOAD, NRF_CMD_R_RX_PAYLOAD, nullptr, sizeof(NrfPacket));
                    }
                    break;
                }

                case OP_RX_PAYLOAD: {
                    memcpy(_dmaRxPacket, doneSlot->rxBuf + 1, sizeof(NrfPacket));
                    if (outRxPacket) {
                        memcpy(outRxPacket, _dmaRxPacket, sizeof(NrfPacket));
                    }
                    res.rxPacketReady = true;
                    uint8_t clrRx = NRF_STATUS_RX_DR;
                    queueOp(OP_IGNORE, NRF_CMD_W_REGISTER | NRF_REG_STATUS, &clrRx, 1);
                    if (_phase == PHASE_RX_PAYLOAD_PENDING) {
                        _phase = PHASE_RX_LISTEN;
                    }
                    break;
                }

                case OP_RX_STATUS_CLR:
                case OP_FLUSH_TX:
                case OP_IGNORE:
                default:
                    break;
            }
        }
    }

    // Ha nincs folyamatban lévő SPI tranzakció, időzített nem-blokkoló STATUS lekérdezés (200 us)
    if (_inFlightCount == 0) {
        uint32_t nowUs = micros();
        if (_phase == PHASE_TX_AIR_WAIT &&
            static_cast<uint32_t>(nowUs - _lastPollUs) >= NRF_POLL_INTERVAL_US) {
            _lastPollUs = nowUs;
            queueOp(OP_POLL_TX_STATUS, NRF_CMD_NOP, nullptr, 0);
        } else if (_phase == PHASE_RX_LISTEN &&
                   static_cast<uint32_t>(nowUs - _lastPollUs) >= NRF_POLL_INTERVAL_US) {
            _lastPollUs = nowUs;
            queueOp(OP_POLL_RX_STATUS, NRF_CMD_NOP, nullptr, 0);
        }
    }

    if (res.hasTimingUpdate || accumWaitUs > 0) {
        recordWaitUs(accumWaitUs);
        res.hasTimingUpdate = true;
        res.dmaWaitUs       = accumWaitUs;
    } else {
        res.dmaWaitUs = _lastWaitUs;
    }

    return res;
}

void NrfDmaRadio::stopListening() {
    digitalWrite(_cePin, LOW);
    _phase = PHASE_IDLE;
    if (_ready && _isRxMode) {
        _isRxMode = false;
        uint8_t ptxCfg = NRF_CONFIG_PTX;
        queueOp(OP_IGNORE, NRF_CMD_W_REGISTER | NRF_REG_CONFIG, &ptxCfg, 1);
    }
}

uint32_t NrfDmaRadio::flushAndDrain() {
    digitalWrite(_cePin, LOW);
    uint32_t totalWaitUs = 0;

    if (!_ready || !_handle) {
        _phase         = PHASE_IDLE;
        _inFlightCount = 0;
        _dmaDone       = false;
        return 0;
    }

    // Függő DMA tranzakciók lezárása, hogy ne maradjon beragadt elem a DMA sorban
    uint8_t safetyCount = 0;
    while (_inFlightCount > 0 && safetyCount < (_queueSize + 2U)) {
        DmaSlot* slot = nullptr;
        if (!drainOneCompleted(pdMS_TO_TICKS(5), &slot, &totalWaitUs)) {
            break;
        }
        safetyCount++;
    }

    for (uint8_t i = 0; i < MAX_SLOTS; i++) {
        _slots[i].inUse = false;
        _slots[i].op    = OP_NONE;
    }
    _inFlightCount = 0;
    _dmaDone       = false;
    _isRxMode      = false;
    _phase         = PHASE_IDLE;

    // FLUSH_TX + FLUSH_RX + STATUS + PTX mód biztosítása tiszta állapotba állításhoz
    uint32_t t0 = micros();
    syncTransfer(NRF_CMD_FLUSH_TX, nullptr, nullptr, 0);
    syncTransfer(NRF_CMD_FLUSH_RX, nullptr, nullptr, 0);
    syncWriteReg(NRF_REG_STATUS, NRF_STATUS_CLR_ALL);
    syncWriteReg(NRF_REG_CONFIG, NRF_CONFIG_PTX);
    totalWaitUs += static_cast<uint32_t>(micros() - t0);

    recordWaitUs(totalWaitUs);
    return totalWaitUs;
}

bool NrfDmaRadio::consumeQueueFullWarning() {
    bool v = _queueFullWarn;
    _queueFullWarn = false;
    return v;
}
