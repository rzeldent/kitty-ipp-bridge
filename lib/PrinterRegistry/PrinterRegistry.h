/**
 * @file PrinterRegistry.h
 * @brief Thread-safe registry of all Cat Printers known to the bridge.
 *
 * The registry is the single source of truth for "which printers exist".
 * BleDiscovery publishes sightings, the registry task applies them and
 * periodically marks stale entries offline. Consumers (IPP server, Web UI)
 * read snapshots and subscribe to change notifications.
 *
 * Lifecycle of an entry:
 * @verbatim
 *   first sighting ──► Added (online) ──► Updated (rssi/lastSeen)
 *                          │
 *        not seen for offlineTimeout (120 s)
 *                          ▼
 *                     WentOffline  (entry and IPP endpoint stay visible)
 *                          │  seen again
 *                          ▼
 *                     WentOnline
 * @endverbatim
 *
 * Entries survive printer power cycles and, because they are persisted in
 * NVS, also bridge reboots (they are restored in the offline state).
 */
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "RtosMutex.h"

class ConfigManager;

struct PrinterRecord
{
    std::string mac;        ///< Canonical MAC "aa:bb:cc:dd:ee:ff"
    std::string alias;      ///< User assigned friendly name (may be empty)
    std::string deviceName; ///< Advertised name (informational only, never used for matching)
    int rssi = 0;           ///< Last RSSI in dBm
    bool online = false;    ///< Seen within the offline timeout
    uint32_t lastSeen = 0;  ///< Uptime (seconds) of the last sighting, 0 = never seen since boot

    // ---- Extensions ---------------------------------------------------------
    std::string id;          ///< Printer id used in URLs ("aabbccddeeff")
    std::string profileName; ///< PrinterProfile used to talk to the device
    uint8_t addrType = 0;    ///< BLE address type (public/random)
    uint32_t firstSeen = 0;  ///< Uptime (seconds) when the entry was created

    /** Alias if set, otherwise a generated name ("Cat Printer EEFF"). */
    std::string displayName() const;
};

enum class RegistryEvent : uint8_t
{
    Added,
    Updated,
    WentOnline,
    WentOffline,
    AliasChanged,
    Removed,
};

const char *registryEventName(RegistryEvent event);

class PrinterRegistry
{
public:
    using Listener = std::function<void(RegistryEvent, const PrinterRecord &)>;

    /**
     * Restore persisted printers (as offline) and load aliases.
     * @param config        configuration storage (aliases and persistence)
     * @param offlineTimeoutSec seconds without sighting before a printer is offline
     */
    void begin(ConfigManager *config, uint32_t offlineTimeoutSec = 120);

    /**
     * Add or refresh a printer after a sighting.
     * @return true if the printer was not known before.
     */
    bool upsert(const std::string &mac, const std::string &deviceName, int rssi, uint8_t addrType,
                const std::string &profileName);

    /** Refresh lastSeen/online without other changes (e.g. after a successful connection). */
    void touch(const std::string &mac);

    /** Mark a printer offline immediately. */
    void markOffline(const std::string &mac);

    /** Mark printers offline whose lastSeen is older than the timeout. Returns number changed. */
    size_t checkTimeouts();

    /** Lookup by canonical MAC. */
    bool get(const std::string &mac, PrinterRecord &out) const;

    /** Lookup by MAC or printer id (any accepted MAC notation). */
    bool find(const std::string &macOrId, PrinterRecord &out) const;

    /** Snapshot of all printers, sorted by MAC. */
    std::vector<PrinterRecord> list() const;

    /** Set (or clear) an alias; persists to NVS. */
    bool setAlias(const std::string &mac, const std::string &alias);

    /** Forget a printer (removes persistence and alias). */
    bool remove(const std::string &mac);

    size_t size() const;
    size_t onlineCount() const;

    void setOfflineTimeout(uint32_t seconds) { offlineTimeoutSec_ = seconds; }
    uint32_t offlineTimeout() const { return offlineTimeoutSec_; }

    /** Register a change listener (called outside the registry lock). */
    void addListener(Listener listener);

    /** Uptime in seconds used for lastSeen bookkeeping. */
    static uint32_t nowSeconds();

private:
    void notify(RegistryEvent event, const PrinterRecord &record);

    mutable RtosMutex mutex_;
    std::map<std::string, PrinterRecord> printers_;
    std::vector<Listener> listeners_;
    ConfigManager *config_ = nullptr;
    volatile uint32_t offlineTimeoutSec_ = 120;
};
