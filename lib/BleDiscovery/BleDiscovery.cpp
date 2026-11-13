#include "BleDiscovery.h"

#include <NimBLEDevice.h>

#include <algorithm>
#include <cstring>

#include "BleArbiter.h"
#include "Logging.h"
#include "MacUtil.h"
#include "PrinterProfile.h"
#include "SystemStats.h"
#include "Watchdog.h"

namespace
{
    constexpr const char *TAG = "ble-scan";
    constexpr size_t kMaxNegativeCache = 128;
    constexpr uint32_t kRadioBusyRetryMs = 5000;

    // Advertising data types listing service UUIDs.
    constexpr uint8_t AD_INCOMPLETE_16 = 0x02;
    constexpr uint8_t AD_COMPLETE_16 = 0x03;
    constexpr uint8_t AD_INCOMPLETE_32 = 0x04;
    constexpr uint8_t AD_COMPLETE_32 = 0x05;
    constexpr uint8_t AD_INCOMPLETE_128 = 0x06;
    constexpr uint8_t AD_COMPLETE_128 = 0x07;

    uint32_t nowSec() { return static_cast<uint32_t>(millis() / 1000); }

    void copyString(char *dst, size_t dstSize, const std::string &src)
    {
        strncpy(dst, src.c_str(), dstSize - 1);
        dst[dstSize - 1] = '\0';
    }
} // namespace

bool BleDiscovery::begin(const Settings &settings, size_t eventQueueLength)
{
    settings_ = settings;
    if (!queue_)
        queue_ = xQueueCreate(eventQueueLength, sizeof(DiscoveryEvent));
    if (!queue_)
    {
        LOG_E(TAG, "Failed to create discovery queue");
        return false;
    }

    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setActiveScan(true);   // request scan responses: many printers put UUIDs there
    scan->setInterval(100);
    scan->setWindow(90);
    scan->setDuplicateFilter(1);
    scan->setMaxResults(0xFF);
    return true;
}

void BleDiscovery::applySettings(const Settings &settings)
{
    settings_ = settings;
    LOG_I(TAG, "Scan settings: interval=%lus window=%lus probe=%s",
          static_cast<unsigned long>(settings.scanIntervalMs / 1000),
          static_cast<unsigned long>(settings.scanDurationMs / 1000), settings.probeUnknownDevices ? "on" : "off");
    requestScan();
}

bool BleDiscovery::startTask(UBaseType_t priority, BaseType_t core, uint32_t stackSize)
{
    if (task_)
        return true;
    return xTaskCreatePinnedToCore(taskEntry, "ble_scan", stackSize, this, priority, &task_, core) == pdPASS;
}

bool BleDiscovery::receive(DiscoveryEvent &event, TickType_t ticks)
{
    return queue_ && xQueueReceive(queue_, &event, ticks) == pdTRUE;
}

void BleDiscovery::requestScan()
{
    if (task_)
        xTaskNotifyGive(task_);
}

void BleDiscovery::taskEntry(void *arg)
{
    static_cast<BleDiscovery *>(arg)->run();
}

void BleDiscovery::run()
{
    watchdog::subscribe();
    LOG_I(TAG, "Scanner task started");
    for (;;)
    {
        watchdog::feed();
        const uint32_t cycleStart = millis();
        uint32_t waitMs;
        {
            // Never scan while a print job owns the radio.
            BleRadioLock radio(pdMS_TO_TICKS(100));
            if (radio.owns())
            {
                scanOnce();
                const uint32_t elapsed = millis() - cycleStart;
                waitMs = settings_.scanIntervalMs > elapsed ? settings_.scanIntervalMs - elapsed : 0;
            }
            else
            {
                LOG_D(TAG, "Radio busy (print job active), postponing scan");
                waitMs = kRadioBusyRetryMs;
            }
        }
        // Sleep until the next cycle, waking early when requestScan() is called.
        // Wait in slices so the watchdog keeps being fed.
        while (waitMs > 0)
        {
            const uint32_t slice = waitMs > 10000 ? 10000 : waitMs;
            if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(slice)) > 0)
                break;
            waitMs -= slice;
            watchdog::feed();
        }
    }
}

int BleDiscovery::matchAdvertisement(const NimBLEAdvertisedDevice *device) const
{
    const auto &profiles = PrinterProfiles::all();
    const uint8_t count = device->getServiceUUIDCount();
    for (uint8_t i = 0; i < count; ++i)
    {
        const NimBLEUUID uuid = device->getServiceUUID(i);
        const PrinterProfile *profile = PrinterProfiles::matchService(uuid);
        if (profile)
            return static_cast<int>(profile - profiles.data());
    }
    return -1;
}

bool BleDiscovery::hasCompleteUuidList(const NimBLEAdvertisedDevice *device)
{
    // A device advertising a *complete* list that does not contain a known
    // UUID is definitively not a Cat Printer. Incomplete lists (or no list at
    // all) leave room for a hidden print service.
    const bool complete = device->haveType(AD_COMPLETE_16) || device->haveType(AD_COMPLETE_32) ||
                          device->haveType(AD_COMPLETE_128);
    const bool incomplete = device->haveType(AD_INCOMPLETE_16) || device->haveType(AD_INCOMPLETE_32) ||
                            device->haveType(AD_INCOMPLETE_128);
    return complete && !incomplete;
}

void BleDiscovery::scanOnce()
{
    NimBLEScan *scan = NimBLEDevice::getScan();
    scanning_ = true;
    LOG_D(TAG, "Active scan for %lu ms", static_cast<unsigned long>(settings_.scanDurationMs));

    // Blocking scan: returns after the window elapsed.
    NimBLEScanResults results = scan->getResults(settings_.scanDurationMs, false);
    scanning_ = false;
    watchdog::feed();

    uint32_t matches = 0;
    std::vector<ProbeCandidate> candidates;
    const int count = results.getCount();
    for (int i = 0; i < count; ++i)
    {
        const NimBLEAdvertisedDevice *device = results.getDevice(i);
        if (!device)
            continue;
        const std::string mac = macutil::normalize(device->getAddress().toString());
        if (mac.empty())
            continue;

        const int profileIndex = matchAdvertisement(device);
        if (profileIndex >= 0)
        {
            ++matches;
            publish(mac, device->getName(), device->getRSSI(), device->getAddressType(),
                    static_cast<size_t>(profileIndex), false);
        }
        else if (settings_.probeUnknownDevices && device->isConnectable() && !hasCompleteUuidList(device) &&
                 shouldProbe(mac))
        {
            candidates.push_back({mac, device->getAddressType(), device->getRSSI(), device->getName()});
        }
    }
    scan->clearResults();

    // Optional verification probes, strongest signal first.
    if (!candidates.empty())
    {
        std::sort(candidates.begin(), candidates.end(),
                  [](const ProbeCandidate &a, const ProbeCandidate &b) { return a.rssi > b.rssi; });
        size_t probes = 0;
        for (const auto &c : candidates)
        {
            if (probes++ >= settings_.maxProbesPerCycle)
                break;
            watchdog::feed();
            const int profileIndex = probe(c);
            if (profileIndex >= 0)
            {
                ++matches;
                publish(c.mac, c.name, c.rssi, c.addrType, static_cast<size_t>(profileIndex), true);
            }
            else
            {
                if (negativeCache_.size() >= kMaxNegativeCache)
                    negativeCache_.erase(negativeCache_.begin());
                negativeCache_[c.mac] = nowSec() + settings_.negativeCacheSec;
            }
        }
    }

    lastScanDevices_ = static_cast<uint32_t>(count);
    lastScanMatches_ = matches;
    ++scans_;
    SystemStats::instance().bleScans++;
    LOG_D(TAG, "Scan done: %d device(s), %lu printer(s)", count, static_cast<unsigned long>(matches));
}

bool BleDiscovery::shouldProbe(const std::string &mac)
{
    auto it = negativeCache_.find(mac);
    if (it == negativeCache_.end())
        return true;
    if (nowSec() >= it->second)
    {
        negativeCache_.erase(it);
        return true;
    }
    return false;
}

int BleDiscovery::probe(const ProbeCandidate &candidate)
{
    SystemStats::instance().bleProbes++;
    LOG_D(TAG, "Probing %s for printer services", candidate.mac.c_str());

    NimBLEClient *client = NimBLEDevice::createClient();
    if (!client)
        return -1;
    client->setConnectTimeout(settings_.probeConnectTimeoutMs);

    int result = -1;
    const NimBLEAddress address(candidate.mac, candidate.addrType);
    if (client->connect(address, true, false, false))
    {
        const auto &profiles = PrinterProfiles::all();
        for (NimBLERemoteService *service : client->getServices(true))
        {
            const PrinterProfile *profile = PrinterProfiles::matchService(service->getUUID());
            if (profile)
            {
                result = static_cast<int>(profile - profiles.data());
                break;
            }
        }
        client->disconnect();
    }
    NimBLEDevice::deleteClient(client);

    LOG_D(TAG, "Probe %s: %s", candidate.mac.c_str(), result >= 0 ? "printer" : "not a printer");
    return result;
}

void BleDiscovery::publish(const std::string &mac, const std::string &name, int rssi, uint8_t addrType,
                           size_t profileIndex, bool probed)
{
    DiscoveryEvent ev{};
    copyString(ev.mac, sizeof(ev.mac), mac);
    copyString(ev.deviceName, sizeof(ev.deviceName), name);
    ev.rssi = static_cast<int8_t>(rssi);
    ev.addrType = addrType;
    ev.profileIndex = static_cast<uint8_t>(profileIndex);
    ev.probed = probed;
    if (xQueueSend(queue_, &ev, 0) != pdTRUE)
    {
        ++dropped_;
        LOG_W(TAG, "Discovery queue full, dropping event for %s", mac.c_str());
    }
}
