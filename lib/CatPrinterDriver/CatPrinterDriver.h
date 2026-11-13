/**
 * @file CatPrinterDriver.h
 * @brief On-demand BLE connection and print transport for Cat Printers.
 *
 * The driver is deliberately *transport only*: it knows how to connect to a
 * printer, find the write/notify characteristics described by a
 * PrinterProfile, push bytes with flow control and disconnect again. Packet
 * construction is delegated to a catgfx::ProtocolEncoder chosen from the
 * profile, which keeps the driver independent from a specific printer
 * protocol (see docs/CATGFX_TODO.md).
 *
 * Connection policy
 * -----------------
 *  - connect() is only called by the job worker when a job is processed.
 *  - disconnect() is called immediately after the job (also by the
 *    destructor, so an early return can never leak a connection).
 *  - Sleeping printers frequently ignore the first connection request, so
 *    connect() retries with a delay before giving up.
 *
 * Thread safety: a driver instance is used by exactly one task (the job
 * worker). The caller must hold the BleRadioLock for the whole session so
 * scanning and printing never compete for the radio.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "CatProtocol.h"
#include "MonoBitmap.h"
#include "PrinterProfile.h"

class NimBLEClient;
class NimBLERemoteCharacteristic;

class CatPrinterDriver
{
public:
    /** Result codes reported by lastError(). */
    enum class Error : uint8_t
    {
        None,
        ConnectFailed,          ///< Printer did not accept a connection (off, asleep, out of range)
        ServiceNotFound,        ///< Connected but the profile service is missing
        CharacteristicNotFound, ///< Service found but no writable print characteristic
        UnsupportedProtocol,    ///< No encoder for the profile's protocol family
        NotConnected,           ///< Operation requires an open connection
        Disconnected,           ///< Link dropped while sending
        WriteFailed,            ///< BLE stack rejected a write
        FlowTimeout,            ///< Printer signalled "buffer full" and never resumed
        Cancelled,              ///< Progress callback requested cancellation
    };

    /** Tunables, normally derived from ProtocolConfig. */
    struct Options
    {
        uint8_t connectAttempts = 3;       ///< Connection attempts before ConnectFailed
        uint32_t connectTimeoutMs = 6000;  ///< Timeout of a single attempt
        uint32_t retryDelayMs = 1500;      ///< Pause between attempts (lets sleeping printers wake)
        uint16_t chunkSize = 180;          ///< Upper bound for a single write (clamped to MTU - 3)
        uint8_t packetDelayMs = 4;         ///< Delay between writes
        uint32_t flowPauseTimeoutMs = 8000; ///< Maximum time to wait for a "resume" notification
        uint16_t preferredMtu = 247;       ///< MTU requested during connection
    };

    /**
     * Progress callback, invoked after each bitmap row.
     * @return false to cancel the print.
     */
    using ProgressFn = std::function<bool(uint32_t rowsDone, uint32_t rowsTotal)>;

    CatPrinterDriver();
    ~CatPrinterDriver();

    CatPrinterDriver(const CatPrinterDriver &) = delete;
    CatPrinterDriver &operator=(const CatPrinterDriver &) = delete;

    void setOptions(const Options &options) { options_ = options; }
    const Options &options() const { return options_; }

    /**
     * Connect to a printer and prepare it for printing.
     *
     * Performs connection (with retries), service discovery and
     * characteristic discovery, then subscribes to the notify characteristic
     * (if any) for flow control.
     */
    bool connect(const std::string &mac, uint8_t addrType, const PrinterProfile &profile);

    /** Close the connection and release all NimBLE resources. Safe to call repeatedly. */
    void disconnect();

    /** True while the BLE link is up. */
    bool isConnected() const;

    /** Send pre-encoded bytes (split into MTU-sized writes, honouring flow control). */
    bool sendRaw(const uint8_t *data, size_t len);

    /**
     * Print a 1-bit bitmap (MSB-first rows, 1 = black) followed by a paper feed.
     * The bitmap width must match the printer width; narrower rows are padded.
     */
    bool printBitmap(const raster::MonoBitmap &bitmap, const catgfx::PrintSettings &settings, uint16_t feedLines,
                     const ProgressFn &progress = nullptr);

    /** Feed paper by @p lines dot rows (8 rows = 1 mm at 203 dpi). */
    bool feedPaper(uint16_t lines);

    /**
     * Print the identification page ("CAT PRINTER IDENTIFY", name and MAC)
     * followed by a 20 mm paper feed.
     */
    bool identify(const std::string &displayName, const std::string &mac, const catgfx::PrintSettings &settings);

    Error lastError() const { return error_; }
    static const char *errorString(Error error);

    /** Negotiated ATT MTU of the current connection (0 when disconnected). */
    uint16_t mtu() const;

    /** Number of "pause" notifications seen during the current session. */
    uint32_t flowPauses() const { return flowPauses_; }

    /** Rows per millimetre at 203 dpi (used for "feed 20 mm"). */
    static constexpr uint16_t kRowsPerMm = 8;

private:
    bool attemptConnect(const std::string &mac, uint8_t addrType);
    bool discoverServices();
    bool discoverCharacteristics();
    bool writeChunk(const uint8_t *data, size_t len);
    bool waitWhilePaused();
    bool flush(std::vector<uint8_t> &buffer);
    bool fail(Error error);

    Options options_;
    const PrinterProfile *profile_ = nullptr;
    std::unique_ptr<catgfx::ProtocolEncoder> encoder_;
    NimBLEClient *client_ = nullptr;
    NimBLERemoteCharacteristic *writeChar_ = nullptr;
    NimBLERemoteCharacteristic *notifyChar_ = nullptr;
    bool writeWithResponse_ = false;
    std::atomic<bool> paused_{false};
    std::atomic<uint32_t> flowPauses_{0};
    std::string mac_;
    Error error_ = Error::None;
};
