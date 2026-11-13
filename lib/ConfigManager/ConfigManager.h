/**
 * @file ConfigManager.h
 * @brief Persistent configuration stored in NVS through the Preferences API.
 *
 * NVS layout (namespaces are limited to 15 characters, keys to 15 characters):
 *
 * | Namespace     | Key                | Content                                   |
 * |---------------|--------------------|-------------------------------------------|
 * | cb_net        | ssid, pass, host,  | Wi-Fi credentials, mDNS hostname,         |
 * |               | ap_pass            | password of the fallback access point     |
 * | cb_cfg        | scan_int, scan_dur | BLE scan interval / window (seconds)      |
 * |               | offline_to         | Registry online timeout (seconds)         |
 * |               | probe, q_size,     | Probe unknown devices, queue size,        |
 * |               | retries, log_lvl,  | job retries, log level                    |
 * |               | trim               | Trim blank rows from rasterised pages     |
 * | cb_proto      | energy, speed,     | Print density / motor speed,              |
 * |               | feed, chunk,       | feed after job, BLE write chunk size,     |
 * |               | delay              | inter-packet delay (future CatGFX knobs)  |
 * | cb_alias      | <printer-id>       | User friendly alias per printer           |
 * | cb_known      | <printer-id>       | Persisted registry entry (JSON)           |
 *
 * All methods are thread-safe.
 */
#pragma once

#include <Preferences.h>

#include <cstdint>
#include <map>
#include <string>

#include "RtosMutex.h"

/** Network / connectivity settings. */
struct NetworkConfig
{
    std::string ssid;               ///< Station SSID; empty = start setup access point
    std::string password;           ///< Station password
    std::string hostname = "catbridge"; ///< mDNS host name (catbridge.local)
    std::string apPassword = "catbridge"; ///< WPA2 password of the setup AP (>= 8 chars)
};

/** Behavioural settings of the bridge. */
struct BridgeConfig
{
    uint16_t scanIntervalSec = 30;   ///< Time between the start of two scan windows
    uint16_t scanDurationSec = 5;    ///< Length of an active scan window
    uint16_t offlineTimeoutSec = 120; ///< Printer is offline when not seen for this long
    bool probeUnknownDevices = false; ///< Allow temporary GATT probe for devices without complete UUID list
    uint8_t queueSize = 8;           ///< Maximum number of queued jobs
    uint8_t maxRetries = 3;          ///< Attempts per job (1 = no retry)
    uint8_t logLevel = 1;            ///< LogLevel as integer (0=DEBUG .. 3=ERROR)
    bool trimBlankRows = true;       ///< Remove leading/trailing white rows from pages
};

/**
 * @brief Printer protocol tuning (kept generic for future CatGFX variants).
 */
struct ProtocolConfig
{
    uint16_t energy = 0x3000;   ///< Print head energy / density (legacy protocol: 0x0000-0xFFFF)
    uint8_t speed = 32;         ///< Motor speed for bitmap printing (lower = slower = darker)
    uint16_t feedAfterJobLines = 80; ///< Paper feed after every job (8 lines = 1 mm)
    uint16_t chunkSize = 180;   ///< Maximum bytes per BLE write (clamped to MTU-3)
    uint8_t packetDelayMs = 4;  ///< Delay between BLE writes (flow control for cheap printers)
};

/** Persisted information about a printer discovered in the past. */
struct KnownPrinter
{
    std::string mac;
    std::string deviceName;
    std::string profileName;
    uint8_t addrType = 0;
};

class ConfigManager
{
public:
    /** Open NVS and load all settings into RAM. */
    bool begin();

    // ---- Network --------------------------------------------------------
    NetworkConfig network() const;
    bool setNetwork(const NetworkConfig &cfg);

    // ---- Bridge configuration -------------------------------------------
    BridgeConfig bridge() const;
    bool setBridge(const BridgeConfig &cfg);

    // ---- Protocol settings ----------------------------------------------
    ProtocolConfig protocol() const;
    bool setProtocol(const ProtocolConfig &cfg);

    // ---- Printer aliases ------------------------------------------------
    /** Alias for a canonical MAC, empty if none. */
    std::string alias(const std::string &mac) const;
    /** Set (or clear with an empty string) the alias of a printer. */
    bool setAlias(const std::string &mac, const std::string &alias);

    // ---- Known printers (registry persistence) ---------------------------
    std::map<std::string, KnownPrinter> knownPrinters() const;
    bool saveKnownPrinter(const KnownPrinter &printer);
    bool removeKnownPrinter(const std::string &mac);

    /** Erase every namespace owned by the bridge (factory reset). */
    void factoryReset();

private:
    void loadAll();

    mutable RtosMutex mutex_;
    NetworkConfig network_;
    BridgeConfig bridge_;
    ProtocolConfig protocol_;
    std::map<std::string, std::string> aliases_; ///< key: canonical MAC
};
