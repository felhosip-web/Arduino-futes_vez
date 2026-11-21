#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include "config.h"
#include "nrf_dma_core.h"
#include "../../nano/src/common.h"

// ============================================================================
// ESP32-C3 SPI2_HOST GDMA BUSZ KEZELŐ (ÚJRAHASZNOSÍTHATÓ MODUL)
// ============================================================================
// - Kizárólag a szabad SPI2_HOST-ot használja (SPI0/SPI1 a Flash memóriáé).
// - Új eszközök (pl. SD kártya, SPI TFT kijelző) később egyszerűen
//   csatlakoztathatók az addDevice(...) hívással külön CS lábbal.
// ============================================================================
class SpiDmaBus {
public:
    bool begin(spi_host_device_t host          = NRF_SPI_HOST,
               int               mosiPin       = PIN_NRF_MOSI,
               int               misoPin       = PIN_NRF_MISO,
               int               sclkPin       = PIN_NRF_SCK,
               spi_dma_chan_t    dmaChan       = NRF_SPI_DMA_CHANNEL,
               int               maxTransferSz = NRF_SPI_MAX_XFER_SZ);

    bool addDevice(int                   csPin,
                   uint32_t              clockHz,
                   uint8_t               mode,
                   uint8_t               queueSize,
                   transaction_cb_t      postCb,
                   spi_device_handle_t*  outHandle) const;

    bool isInitialized() const { return _initialized; }
    spi_host_device_t host() const { return _host; }

private:
    bool              _initialized = false;
    spi_host_device_t _host        = NRF_SPI_HOST;
};

// ============================================================================
// NEM-BLOKKOLÓ NRF24L01+ VEZÉRLŐ GDMA SPI FELETT
// ============================================================================
// - 32 byte-os V5 NrfPacket átvitele MALLOC_CAP_DMA memóriából (soha stack-ről).
// - 10 MHz SPI órajel, queue_size = 3 (a 3 retry blokkolás nélkül elfér).
// - post_cb ISR CSAK a volatile bool _dmaDone flag-et állítja true-ra.
// - A loop() nem-blokkoló módon pollingolja a _dmaDone / get_trans_result-ot.
// - Az állapotgép és a veszteségmentes sor-/hiba-kezelés az NrfDmaCore-ban fut.
// ============================================================================
class NrfDmaRadio {
public:
    using PollResult = NrfDmaCore::PollResult;

    bool begin(SpiDmaBus& bus,
               uint8_t    cePin     = PIN_NRF_CE,
               uint8_t    csnPin    = PIN_NRF_CSN,
               uint32_t   clockHz   = NRF_SPI_CLOCK_HZ,
               uint8_t    queueSize = NRF_SPI_QUEUE_SIZE);

    bool isReady() const { return _core.isReady(); }
    bool isPendingResetCleanup() const { return _core.isPendingResetCleanup(); }

    // Nem-blokkoló adás indítása (W_TX_PAYLOAD 32B NrfPacket DMA sorba tétele)
    bool queueTransmitPacket(const NrfPacket& pkt);

    // Nem-blokkoló állapot-lekérdezés a loop()-ból
    PollResult poll(NrfPacket* outRxPacket);

    // RX leállítása (CE LOW + PRX kikapcsolás ütemezés)
    void stopListening();

    // 500ms NRF timeout esetén: függő DMA tranzakciók lezárása + FLUSH_TX/FLUSH_RX
    uint32_t flushAndDrain();

    bool     consumeQueueFullWarning() { return _core.consumeQueueFullWarning(); }
    uint32_t getLastWaitUs() const { return _core.getLastWaitUs(); }
    uint32_t getMaxWaitUs() const { return _core.getMaxWaitUs(); }
    uint8_t  getInFlightCount() const { return _core.getInFlightCount(); }

private:
    static constexpr size_t  DMA_FRAME_BYTES = NrfDmaCore::DMA_FRAME_BYTES;
    static constexpr uint8_t MAX_SLOTS       = NRF_SPI_QUEUE_SIZE;
    static_assert(NRF_SPI_QUEUE_SIZE <= NrfDmaCore::MAX_SLOTS,
                  "NRF_SPI_QUEUE_SIZE nem haladhatja meg az NrfDmaCore::MAX_SLOTS erteket!");

    struct EspSlot {
        NrfDmaRadio*      owner   = nullptr;
        uint8_t           slotIdx = 0;
        spi_transaction_t trans{};
        uint8_t*          txBuf   = nullptr; // MALLOC_CAP_DMA (36 byte)
        uint8_t*          rxBuf   = nullptr; // MALLOC_CAP_DMA (36 byte)
    };

    spi_device_handle_t _handle      = nullptr;
    uint8_t             _cePin       = PIN_NRF_CE;
    uint8_t             _csnPin      = PIN_NRF_CSN;
    NrfPacket*          _dmaTxPacket = nullptr; // 32 byte-os MALLOC_CAP_DMA tükörpuffer
    NrfPacket*          _dmaRxPacket = nullptr; // 32 byte-os MALLOC_CAP_DMA tükörpuffer
    EspSlot             _espSlots[MAX_SLOTS]{};
    NrfDmaCore          _core{};

    // ISR-ből CSAK ez a flag állítódik true-ra (Serial/MQTT/LCD tilos ISR-ben!)
    volatile bool       _dmaDone     = false;

    static void IRAM_ATTR onDmaPostTransfer(spi_transaction_t* trans);

    static NrfDmaCore::HalStatus halQueueTrans(void* ctx, uint8_t slotIdx, size_t totalBytes);
    static NrfDmaCore::HalStatus halGetTransResult(void* ctx, uint32_t waitTicks, uint8_t* outSlotIdx);
    static bool                  halSyncTrans(void* ctx, uint8_t slotIdx, size_t totalBytes);
    static void                  halSetCe(void* ctx, bool high);
    static void                  halDelayUs(void* ctx, uint32_t us);
    static uint32_t              halGetMicros(void* ctx);
    static uint32_t              halMsToTicks(void* ctx, uint32_t ms);
};
