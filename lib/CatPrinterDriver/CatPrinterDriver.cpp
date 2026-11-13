/**
 * @file CatPrinterDriver.cpp
 * @brief Implementation of the on-demand Cat Printer BLE transport.
 */
#include "CatPrinterDriver.h"

#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <vector>

#include "Logging.h"
#include "SystemStats.h"
#include "TextRenderer.h"
#include "Watchdog.h"

static const char *TAG = "driver";

/** Bytes collected before a flush when streaming bitmap rows. */
static constexpr size_t kStreamBufferBytes = 1024;

CatPrinterDriver::CatPrinterDriver() = default;

CatPrinterDriver::~CatPrinterDriver()
{
    disconnect();
}

const char *CatPrinterDriver::errorString(Error error)
{
    switch (error)
    {
    case Error::None:
        return "none";
    case Error::ConnectFailed:
        return "connect failed";
    case Error::ServiceNotFound:
        return "print service not found";
    case Error::CharacteristicNotFound:
        return "print characteristic not found";
    case Error::UnsupportedProtocol:
        return "unsupported protocol";
    case Error::NotConnected:
        return "not connected";
    case Error::Disconnected:
        return "disconnected while printing";
    case Error::WriteFailed:
        return "write failed";
    case Error::FlowTimeout:
        return "printer busy (flow control timeout)";
    case Error::Cancelled:
        return "cancelled";
    }
    return "unknown";
}

bool CatPrinterDriver::fail(Error error)
{
    error_ = error;
    return false;
}

// ---------------------------------------------------------------------------
// Connection management
// ---------------------------------------------------------------------------
bool CatPrinterDriver::connect(const std::string &mac, uint8_t addrType, const PrinterProfile &profile)
{
    disconnect();
    error_ = Error::None;
    profile_ = &profile;
    mac_ = mac;
    paused_ = false;
    flowPauses_ = 0;

    encoder_ = catgfx::createEncoder(profile.protocol, profile.printWidthPx);
    if (!encoder_)
    {
        LOG_E(TAG, "Profile %s uses unsupported protocol %s", profile.profileName.c_str(),
              catgfx::familyName(profile.protocol));
        return fail(Error::UnsupportedProtocol);
    }

    bool connected = false;
    for (uint8_t attempt = 1; attempt <= options_.connectAttempts && !connected; ++attempt)
    {
        watchdog::feed();
        LOG_I(TAG, "Connecting to %s (attempt %u/%u)", mac.c_str(), attempt, options_.connectAttempts);
        connected = attemptConnect(mac, addrType);
        if (!connected)
        {
            SystemStats::instance().bleConnectFailures++;
            disconnect();
            if (attempt < options_.connectAttempts)
                vTaskDelay(pdMS_TO_TICKS(options_.retryDelayMs * attempt)); // linear back-off
        }
    }
    if (!connected)
    {
        LOG_W(TAG, "Unable to connect to %s", mac.c_str());
        return fail(Error::ConnectFailed);
    }

    if (!discoverServices() || !discoverCharacteristics())
    {
        disconnect();
        return false;
    }

    LOG_I(TAG, "Connected to %s (MTU %u, %s)", mac.c_str(), mtu(),
          writeWithResponse_ ? "write with response" : "write without response");
    return true;
}

bool CatPrinterDriver::attemptConnect(const std::string &mac, uint8_t addrType)
{
    client_ = NimBLEDevice::createClient();
    if (!client_)
    {
        LOG_E(TAG, "No free BLE client slot");
        return false;
    }
    client_->setConnectTimeout(options_.connectTimeoutMs);
    // 15-30 ms connection interval: fast enough for bitmap streaming, tolerant for cheap printers.
    client_->setConnectionParams(12, 24, 0, 400);

    const NimBLEAddress address(mac, addrType);
    return client_->connect(address, true, false, true);
}

bool CatPrinterDriver::discoverServices()
{
    // Full discovery is cheap on these devices (2-4 services) and lets us fall back
    // to a characteristic search when a variant moves the print characteristic.
    const auto &services = client_->getServices(true);
    if (services.empty())
    {
        LOG_W(TAG, "%s exposes no GATT services", mac_.c_str());
        return fail(Error::ServiceNotFound);
    }
    LOG_D(TAG, "%s exposes %u services", mac_.c_str(), static_cast<unsigned>(services.size()));
    return true;
}

bool CatPrinterDriver::discoverCharacteristics()
{
    writeChar_ = nullptr;
    notifyChar_ = nullptr;

    NimBLERemoteService *service = client_->getService(profile_->serviceUUID);
    if (service)
    {
        writeChar_ = service->getCharacteristic(profile_->writeUUID);
        if (profile_->notifyUUID.bitSize() != 0)
            notifyChar_ = service->getCharacteristic(profile_->notifyUUID);
    }

    if (!writeChar_)
    {
        // Fallback: look for the write characteristic in any service.
        for (NimBLERemoteService *svc : client_->getServices(false))
        {
            NimBLERemoteCharacteristic *c = svc->getCharacteristic(profile_->writeUUID);
            if (c)
            {
                writeChar_ = c;
                if (!notifyChar_ && profile_->notifyUUID.bitSize() != 0)
                    notifyChar_ = svc->getCharacteristic(profile_->notifyUUID);
                break;
            }
        }
    }

    if (!writeChar_)
    {
        LOG_W(TAG, "%s: write characteristic %s not found", mac_.c_str(), profile_->writeUUID.toString().c_str());
        return fail(service ? Error::CharacteristicNotFound : Error::ServiceNotFound);
    }

    writeWithResponse_ = !writeChar_->canWriteNoResponse() && writeChar_->canWrite();

    if (notifyChar_ && (notifyChar_->canNotify() || notifyChar_->canIndicate()))
    {
        const bool ok = notifyChar_->subscribe(
            notifyChar_->canNotify(),
            [this](NimBLERemoteCharacteristic *, uint8_t *data, size_t len, bool) {
                if (!encoder_)
                    return;
                switch (encoder_->parseNotification(data, len))
                {
                case catgfx::FlowControl::Pause:
                    paused_ = true;
                    flowPauses_++;
                    break;
                case catgfx::FlowControl::Resume:
                    paused_ = false;
                    break;
                case catgfx::FlowControl::None:
                    break;
                }
            },
            true);
        if (!ok)
            LOG_W(TAG, "%s: notification subscription failed, printing without flow control", mac_.c_str());
    }
    return true;
}

void CatPrinterDriver::disconnect()
{
    if (client_)
    {
        if (client_->isConnected())
        {
            client_->disconnect();
            // Give the controller a moment to tear down the link before the client is deleted.
            for (int i = 0; i < 20 && client_->isConnected(); ++i)
                vTaskDelay(pdMS_TO_TICKS(25));
            LOG_D(TAG, "Disconnected from %s", mac_.c_str());
        }
        NimBLEDevice::deleteClient(client_);
        client_ = nullptr;
    }
    writeChar_ = nullptr;
    notifyChar_ = nullptr;
    paused_ = false;
}

bool CatPrinterDriver::isConnected() const
{
    return client_ && client_->isConnected();
}

uint16_t CatPrinterDriver::mtu() const
{
    return isConnected() ? client_->getMTU() : 0;
}

// ---------------------------------------------------------------------------
// Data transfer
// ---------------------------------------------------------------------------
bool CatPrinterDriver::waitWhilePaused()
{
    if (!paused_)
        return true;
    const TickType_t start = xTaskGetTickCount();
    while (paused_)
    {
        if (!isConnected())
            return fail(Error::Disconnected);
        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(options_.flowPauseTimeoutMs))
        {
            LOG_W(TAG, "%s: printer did not resume within %u ms", mac_.c_str(),
                  static_cast<unsigned>(options_.flowPauseTimeoutMs));
            paused_ = false;
            return fail(Error::FlowTimeout);
        }
        watchdog::feed();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

bool CatPrinterDriver::writeChunk(const uint8_t *data, size_t len)
{
    // The NimBLE host can temporarily run out of mbufs when writing without
    // response; back off briefly and retry before declaring failure.
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        if (!isConnected())
            return fail(Error::Disconnected);
        if (writeChar_->writeValue(data, len, writeWithResponse_))
            return true;
        vTaskDelay(pdMS_TO_TICKS(20 * (attempt + 1)));
    }
    return fail(Error::WriteFailed);
}

bool CatPrinterDriver::sendRaw(const uint8_t *data, size_t len)
{
    if (!isConnected() || !writeChar_)
        return fail(Error::NotConnected);

    const uint16_t currentMtu = mtu();
    size_t chunk = options_.chunkSize ? options_.chunkSize : 20;
    if (currentMtu > 3 && chunk > static_cast<size_t>(currentMtu - 3))
        chunk = currentMtu - 3;
    if (chunk < 20)
        chunk = 20;

    size_t offset = 0;
    while (offset < len)
    {
        if (!waitWhilePaused())
            return false;
        const size_t n = (len - offset) < chunk ? (len - offset) : chunk;
        if (!writeChunk(data + offset, n))
        {
            LOG_W(TAG, "%s: %s after %u/%u bytes", mac_.c_str(), errorString(error_), static_cast<unsigned>(offset),
                  static_cast<unsigned>(len));
            return false;
        }
        offset += n;
        if (options_.packetDelayMs)
            vTaskDelay(pdMS_TO_TICKS(options_.packetDelayMs));
        else
            taskYIELD();
    }
    return true;
}

bool CatPrinterDriver::flush(std::vector<uint8_t> &buffer)
{
    if (buffer.empty())
        return true;
    const bool ok = sendRaw(buffer.data(), buffer.size());
    buffer.clear();
    return ok;
}

bool CatPrinterDriver::printBitmap(const raster::MonoBitmap &bitmap, const catgfx::PrintSettings &settings,
                                   uint16_t feedLines, const ProgressFn &progress)
{
    if (!isConnected() || !encoder_)
        return fail(Error::NotConnected);

    std::vector<uint8_t> buffer;
    buffer.reserve(kStreamBufferBytes + 128);

    encoder_->beginJob(buffer, settings);
    const size_t rowBytes = bitmap.rowBytes();
    for (uint32_t y = 0; y < bitmap.height; ++y)
    {
        encoder_->bitmapRow(buffer, bitmap.row(y), rowBytes);
        if (buffer.size() >= kStreamBufferBytes && !flush(buffer))
            return false;
        if (progress && (y % 16 == 0 || y + 1 == bitmap.height) && !progress(y + 1, bitmap.height))
        {
            // Finish the job cleanly so the printer does not wait for more rows.
            buffer.clear();
            encoder_->endJob(buffer);
            flush(buffer);
            return fail(Error::Cancelled);
        }
        if ((y & 63) == 0)
            watchdog::feed();
    }
    if (feedLines)
        encoder_->feed(buffer, feedLines);
    encoder_->endJob(buffer);
    if (!flush(buffer))
        return false;

    // Allow the printer to drain its buffer before the caller disconnects;
    // some models drop queued rows when the link closes too early.
    const uint32_t drainMs = 300 + bitmap.height / 4;
    vTaskDelay(pdMS_TO_TICKS(drainMs > 3000 ? 3000 : drainMs));
    return true;
}

bool CatPrinterDriver::feedPaper(uint16_t lines)
{
    if (!isConnected() || !encoder_)
        return fail(Error::NotConnected);
    std::vector<uint8_t> buffer;
    encoder_->feed(buffer, lines);
    return flush(buffer);
}

bool CatPrinterDriver::identify(const std::string &displayName, const std::string &mac,
                                const catgfx::PrintSettings &settings)
{
    const uint16_t width = profile_ ? profile_->printWidthPx : 384;
    raster::MonoBitmap page = raster::buildIdentifyPage(displayName, mac, width);
    // Step 2: print "CAT PRINTER IDENTIFY"; step 3: feed 20 mm.
    if (!printBitmap(page, settings, 0))
        return false;
    return feedPaper(20 * kRowsPerMm);
}
