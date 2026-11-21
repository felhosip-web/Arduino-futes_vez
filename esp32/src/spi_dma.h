#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include "config.h"
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
// ============================================================================
class NrfDmaRadio {
public:
    struct PollResult {
        bool     txAckedNow      = false;
        bool     txFailedNow     = false;
        bool     rxPacketReady   = false;
        bool     queueFullWarn   = false;
        bool     hasTimingUpdate = false;
        uint32_t dmaWaitUs       = 0;
    };

    bool begin(SpiDmaBus& bus,
               uint8_t    cePin     = PIN_NRF_CE,
               uint8_t    csnPin    = PIN_NRF_CSN,
               uint32_t   clockHz   = NRF_SPI_CLOCK_HZ,
               uint8_t    queueSize = NRF_SPI_QUEUE_SIZE);

    bool isReady() const { return _ready; }

    // Nem-blokkoló adás indítása (W_TX_PAYLOAD 32B NrfPacket DMA sorba tétele)
    bool queueTransmitPacket(const NrfPacket& pkt);

    // Nem-blokkoló állapot-lekérdezés a loop()-ból
    PollResult poll(NrfPacket* outRxPacket);

    // RX leállítása (CE LOW + PRX kikapcsolás ütemezés)
    void stopListening();

    // 500ms NRF timeout esetén: függő DMA tranzakciók lezárása + FLUSH_TX/FLUSH_RX
    uint32_t flushAndDrain();

    bool     consumeQueueFullWarning();
    uint32_t getLastWaitUs() const { return _lastWaitUs; }
    uint32_t getMaxWaitUs() const { return _maxWaitUs; }
    uint8_t  getInFlightCount() const { return _inFlightCount; }

private:
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

    // 1 byte NRF SPI parancs + 32 byte NrfPacket = 33 byte; 4-byte DMA igazítással 36 byte
    static constexpr size_t DMA_FRAME_BYTES = 36;
    static constexpr uint8_t MAX_SLOTS      = NRF_SPI_QUEUE_SIZE;

    struct DmaSlot {
        bool              inUse      = false;
        SlotOp            op         = OP_NONE;
        uint32_t          queuedAtUs = 0;
        spi_transaction_t trans{};
        uint8_t*          txBuf      = nullptr; // MALLOC_CAP_DMA (36 byte)
        uint8_t*          rxBuf      = nullptr; // MALLOC_CAP_DMA (36 byte)
        NrfDmaRadio*      owner      = nullptr;
    };

    spi_device_handle_t _handle        = nullptr;
    uint8_t             _cePin         = PIN_NRF_CE;
    uint8_t             _csnPin        = PIN_NRF_CSN;
    uint8_t             _queueSize     = NRF_SPI_QUEUE_SIZE;
    bool                _ready         = false;
    bool                _isRxMode      = false;
    Phase               _phase         = PHASE_IDLE;
    NrfPacket           _pendingTxPkt{};
    NrfPacket*          _dmaTxPacket   = nullptr; // 32 byte-os MALLOC_CAP_DMA tükörpuffer
    NrfPacket*          _dmaRxPacket   = nullptr; // 32 byte-os MALLOC_CAP_DMA tükörpuffer
    DmaSlot             _slots[MAX_SLOTS];
    uint8_t             _inFlightCount = 0;
    uint32_t            _lastPollUs    = 0;
    uint32_t            _lastWaitUs    = 0;
    uint32_t            _maxWaitUs     = 0;
    bool                _queueFullWarn = false;

    // ISR-ből CSAK ez a flag állítódik true-ra (Serial/MQTT/LCD tilos ISR-ben!)
    volatile bool       _dmaDone       = false;

    static void IRAM_ATTR onDmaPostTransfer(spi_transaction_t* trans);

    bool syncTransfer(uint8_t cmd, const uint8_t* txData, uint8_t* rxData, size_t dataLen);
    bool syncWriteReg(uint8_t reg, uint8_t val);
    bool syncWriteRegBuf(uint8_t reg, const uint8_t* buf, size_t len);
    uint8_t syncReadReg(uint8_t reg, bool* okOut = nullptr);

    bool queueOp(SlotOp op, uint8_t cmd, const uint8_t* data, size_t dataLen);
    bool drainOneCompleted(TickType_t waitTicks, DmaSlot** outSlot, uint32_t* accumWaitUs);
    void recordWaitUs(uint32_t us);
};
