/**
 * @file BleDiscovery.h
 * @brief Continuous, UUID based discovery of BLE Cat Printers.
 *
 * The scanner task runs an active scan window (default 5 s) every scan
 * interval (default 30 s). Every advertisement is checked against the GATT
 * service UUIDs of all PrinterProfiles. Advertising names are ignored on
 * purpose - only the advertised service UUIDs decide whether a device is a
 * Cat Printer.
 *
 * Discovery flow:
 * @verbatim
 *   BLE advertisement
 *          │
 *   advertised service UUIDs ── match PrinterProfile? ──yes──► DiscoveryEvent
 *          │ no
 *   complete UUID list advertised? ──yes──► not a printer (ignored)
 *          │ no (incomplete / no UUIDs) and probing enabled
 *   temporary GATT connection after the scan window
 *          │ discover services → match? ──yes──► DiscoveryEvent (probed=true)
 *          ▼
 *   disconnect immediately, negative result cached for 30 min
 * @endverbatim
 *
 * Discovery never connects during normal scanning; the optional probe runs
 * only after the scan window finished, only for devices that did not
 * advertise a complete UUID list, and is disabled by default.
 *
 * Results are published as DiscoveryEvent through a FreeRTOS queue, consumed
 * by the registry task (decoupling BLE timing from registry/NVS work).
 */
#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

class NimBLEAdvertisedDevice;
struct PrinterProfile;

/** Published for every matching advertisement / successful probe. */
struct DiscoveryEvent
{
    char mac[18];         ///< Canonical MAC "aa:bb:cc:dd:ee:ff"
    char deviceName[32];  ///< Advertised name (informational only)
    int8_t rssi;          ///< Signal strength in dBm
    uint8_t addrType;     ///< BLE address type
    uint8_t profileIndex; ///< Index into PrinterProfiles::all()
    bool probed;          ///< True when identified through a GATT probe
};

class BleDiscovery
{
public:
    struct Settings
    {
        uint32_t scanIntervalMs = 30000;       ///< Period between scan window starts
        uint32_t scanDurationMs = 5000;        ///< Active scan window length
        bool probeUnknownDevices = false;      ///< Allow temporary service discovery connections
        uint8_t maxProbesPerCycle = 2;         ///< Upper bound on probe connections per cycle
        uint32_t probeConnectTimeoutMs = 4000; ///< Connect timeout for a probe
        uint32_t negativeCacheSec = 1800;      ///< Do not re-probe a non-printer for this long
    };

    /** Create the event queue. NimBLEDevice::init() must have been called. */
    bool begin(const Settings &settings, size_t eventQueueLength = 32);

    /** Update timing/probing settings at runtime. */
    void applySettings(const Settings &settings);

    /** Start the "BLE Scanner" FreeRTOS task. */
    bool startTask(UBaseType_t priority = 2, BaseType_t core = 1, uint32_t stackSize = 6144);

    /** Wait for the next discovery event. */
    bool receive(DiscoveryEvent &event, TickType_t ticks);

    /** Wake the scanner and start a scan window as soon as the radio is free. */
    void requestScan();

    // ---- Diagnostics ------------------------------------------------------
    uint32_t scanCount() const { return scans_; }
    uint32_t lastScanDevices() const { return lastScanDevices_; }
    uint32_t lastScanMatches() const { return lastScanMatches_; }
    uint32_t droppedEvents() const { return dropped_; }
    bool scanning() const { return scanning_; }

private:
    struct ProbeCandidate
    {
        std::string mac;
        uint8_t addrType;
        int8_t rssi;
        std::string name;
    };

    static void taskEntry(void *arg);
    void run();
    void scanOnce();
    void publish(const std::string &mac, const std::string &name, int rssi, uint8_t addrType, size_t profileIndex,
                 bool probed);
    int matchAdvertisement(const NimBLEAdvertisedDevice *device) const;
    static bool hasCompleteUuidList(const NimBLEAdvertisedDevice *device);
    bool shouldProbe(const std::string &mac);
    int probe(const ProbeCandidate &candidate);

    Settings settings_;
    QueueHandle_t queue_ = nullptr;
    TaskHandle_t task_ = nullptr;
    std::map<std::string, uint32_t> negativeCache_; ///< mac -> expiry (uptime seconds)
    std::atomic<uint32_t> scans_{0};
    std::atomic<uint32_t> lastScanDevices_{0};
    std::atomic<uint32_t> lastScanMatches_{0};
    std::atomic<uint32_t> dropped_{0};
    std::atomic<bool> scanning_{false};
};
