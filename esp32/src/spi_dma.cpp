#include "spi_dma.h"
#include <string.h>
#if __has_include(<esp_idf_version.h>)
#include <esp_idf_version.h>
#endif

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
    auto* slot = static_cast<EspSlot*>(trans->user);
    if (slot->owner) {
        slot->owner->_dmaDone = true;
    }
}

// ============================================================================
// ESP-IDF SPI2_HOST / ARDUINO HAL ADAPTEREK Az NrfDmaCore SZÁMÁRA
// ============================================================================
NrfDmaCore::HalStatus NrfDmaRadio::halQueueTrans(void* ctx, uint8_t slotIdx, size_t totalBytes) {
    auto* self = static_cast<NrfDmaRadio*>(ctx);
    if (!self || !self->_handle || slotIdx >= MAX_SLOTS) {
        return NrfDmaCore::HAL_ERROR;
    }
    EspSlot& slot = self->_espSlots[slotIdx];
    if (!slot.txBuf || !slot.rxBuf) {
        return NrfDmaCore::HAL_ERROR;
    }

    memset(&slot.trans, 0, sizeof(slot.trans));
    slot.trans.length    = totalBytes * 8U;
    slot.trans.rxlength  = totalBytes * 8U;
    slot.trans.tx_buffer = slot.txBuf;
    slot.trans.rx_buffer = slot.rxBuf;
    slot.trans.user      = &slot;

    esp_err_t err = spi_device_queue_trans(self->_handle, &slot.trans, 0);
    if (err == ESP_OK) {
        return NrfDmaCore::HAL_OK;
    }
    if (err == ESP_ERR_NO_MEM || err == ESP_ERR_TIMEOUT) {
        return NrfDmaCore::HAL_BUSY;
    }
    return NrfDmaCore::HAL_ERROR;
}

NrfDmaCore::HalStatus NrfDmaRadio::halGetTransResult(void* ctx, uint32_t waitTicks, uint8_t* outSlotIdx) {
    auto* self = static_cast<NrfDmaRadio*>(ctx);
    if (!self || !self->_handle) {
        return NrfDmaCore::HAL_ERROR;
    }
    spi_transaction_t* doneTrans = nullptr;
    esp_err_t err = spi_device_get_trans_result(self->_handle,
                                                &doneTrans,
                                                static_cast<TickType_t>(waitTicks));
    if (err == ESP_ERR_TIMEOUT) {
        return NrfDmaCore::HAL_TIMEOUT;
    }
    if (err != ESP_OK || !doneTrans || !doneTrans->user) {
        return NrfDmaCore::HAL_ERROR;
    }
    auto* slot = static_cast<EspSlot*>(doneTrans->user);
    if (slot->owner != self || slot->slotIdx >= MAX_SLOTS) {
        return NrfDmaCore::HAL_ERROR;
    }
    if (outSlotIdx) {
        *outSlotIdx = slot->slotIdx;
    }
    return NrfDmaCore::HAL_OK;
}

bool NrfDmaRadio::halSyncTrans(void* ctx, uint8_t slotIdx, size_t totalBytes) {
    auto* self = static_cast<NrfDmaRadio*>(ctx);
    if (!self || !self->_handle || slotIdx >= MAX_SLOTS || self->_core.getInFlightCount() > 0) {
        return false;
    }
    EspSlot& slot = self->_espSlots[slotIdx];
    if (!slot.txBuf || !slot.rxBuf) {
        return false;
    }

    memset(&slot.trans, 0, sizeof(slot.trans));
    slot.trans.length    = totalBytes * 8U;
    slot.trans.rxlength  = totalBytes * 8U;
    slot.trans.tx_buffer = slot.txBuf;
    slot.trans.rx_buffer = slot.rxBuf;
    slot.trans.user      = nullptr;

    esp_err_t err = spi_device_transmit(self->_handle, &slot.trans);
    return (err == ESP_OK);
}

void NrfDmaRadio::halSetCe(void* ctx, bool high) {
    auto* self = static_cast<NrfDmaRadio*>(ctx);
    if (self) {
        digitalWrite(self->_cePin, high ? HIGH : LOW);
    }
}

void NrfDmaRadio::halDelayUs(void* ctx, uint32_t us) {
    (void)ctx;
    delayMicroseconds(us);
}

uint32_t NrfDmaRadio::halGetMicros(void* ctx) {
    (void)ctx;
    return micros();
}

uint32_t NrfDmaRadio::halMsToTicks(void* ctx, uint32_t ms) {
    (void)ctx;
    return static_cast<uint32_t>(pdMS_TO_TICKS(ms));
}

// ============================================================================
// INICIALIZÁLÁS ÉS NYILVÁNOS KEZELŐ METÓDUSOK
// ============================================================================
bool NrfDmaRadio::begin(SpiDmaBus& bus,
                        uint8_t    cePin,
                        uint8_t    csnPin,
                        uint32_t   clockHz,
                        uint8_t    queueSize) {
    if (_core.getInFlightCount() > 0) {
        _core.flushAndDrain();
        if (_core.getInFlightCount() > 0) {
            return false;
        }
    }

    _cePin   = cePin;
    _csnPin  = csnPin;
    _dmaDone = false;

    uint8_t clampedQueue = (queueSize > MAX_SLOTS) ? MAX_SLOTS : (queueSize == 0 ? 1 : queueSize);

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

    uint8_t* txPtrs[NrfDmaCore::MAX_SLOTS] = { nullptr, nullptr, nullptr };
    uint8_t* rxPtrs[NrfDmaCore::MAX_SLOTS] = { nullptr, nullptr, nullptr };

    for (uint8_t i = 0; i < MAX_SLOTS; i++) {
        _espSlots[i].owner   = this;
        _espSlots[i].slotIdx = i;
        if (!_espSlots[i].txBuf) {
            _espSlots[i].txBuf = static_cast<uint8_t*>(heap_caps_malloc(DMA_FRAME_BYTES, MALLOC_CAP_DMA));
        }
        if (!_espSlots[i].rxBuf) {
            _espSlots[i].rxBuf = static_cast<uint8_t*>(heap_caps_malloc(DMA_FRAME_BYTES, MALLOC_CAP_DMA));
        }
        if (!_espSlots[i].txBuf || !_espSlots[i].rxBuf) {
            return false;
        }
        memset(_espSlots[i].txBuf, 0, DMA_FRAME_BYTES);
        memset(_espSlots[i].rxBuf, 0, DMA_FRAME_BYTES);
        txPtrs[i] = _espSlots[i].txBuf;
        rxPtrs[i] = _espSlots[i].rxBuf;
    }

    uint32_t clampedClock = (clockHz > 10000000UL) ? 10000000UL : clockHz;
    if (!_handle) {
        if (!bus.addDevice(static_cast<int>(_csnPin),
                           clampedClock,
                           0,
                           clampedQueue,
                           &NrfDmaRadio::onDmaPostTransfer,
                           &_handle)) {
            return false;
        }
    }

    NrfDmaCore::Hal hal{};
    hal.ctx            = this;
    hal.queueTrans     = &NrfDmaRadio::halQueueTrans;
    hal.getTransResult = &NrfDmaRadio::halGetTransResult;
    hal.syncTrans      = &NrfDmaRadio::halSyncTrans;
    hal.setCe          = &NrfDmaRadio::halSetCe;
    hal.delayUs        = &NrfDmaRadio::halDelayUs;
    hal.getMicros      = &NrfDmaRadio::halGetMicros;
    hal.msToTicks      = &NrfDmaRadio::halMsToTicks;
    hal.dmaDoneFlag    = &_dmaDone;

    _core.attach(hal, clampedQueue, _dmaTxPacket, _dmaRxPacket, txPtrs, rxPtrs);
    return _core.initRadioRegisters();
}

bool NrfDmaRadio::queueTransmitPacket(const NrfPacket& pkt) {
    return _core.queueTransmitPacket(pkt);
}

NrfDmaRadio::PollResult NrfDmaRadio::poll(NrfPacket* outRxPacket) {
    return _core.poll(outRxPacket);
}

void NrfDmaRadio::stopListening() {
    _core.stopListening();
}

uint32_t NrfDmaRadio::flushAndDrain() {
    return _core.flushAndDrain();
}
