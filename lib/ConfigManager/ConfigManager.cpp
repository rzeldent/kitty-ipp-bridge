#include "ConfigManager.h"

#include <nvs.h>
#include <nvs_flash.h>

#include <cstdlib>
#include <vector>

#include "Logging.h"
#include "MacUtil.h"

#ifndef CATBRIDGE_HOSTNAME
#define CATBRIDGE_HOSTNAME "catbridge"
#endif

namespace
{
    constexpr const char *TAG = "config";
    constexpr const char *NS_NET = "cb_net";
    constexpr const char *NS_CFG = "cb_cfg";
    constexpr const char *NS_PROTO = "cb_proto";
    constexpr const char *NS_ALIAS = "cb_alias";
    constexpr const char *NS_KNOWN = "cb_known";

    /** Enumerate all string keys of an NVS namespace. */
    std::vector<std::string> listStringKeys(const char *ns)
    {
        std::vector<std::string> keys;
        nvs_iterator_t it = nullptr;
        esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, ns, NVS_TYPE_STR, &it);
        while (err == ESP_OK)
        {
            nvs_entry_info_t info;
            nvs_entry_info(it, &info);
            keys.emplace_back(info.key);
            err = nvs_entry_next(&it);
        }
        nvs_release_iterator(it);
        return keys;
    }

    std::string getString(Preferences &p, const char *key, const std::string &def)
    {
        if (!p.isKey(key))
            return def;
        return std::string(p.getString(key, def.c_str()).c_str());
    }

    /** Serialise a KnownPrinter as "addrType;profile;deviceName". */
    std::string encodeKnown(const KnownPrinter &k)
    {
        return std::to_string(k.addrType) + ";" + k.profileName + ";" + k.deviceName;
    }

    bool decodeKnown(const std::string &mac, const std::string &value, KnownPrinter &out)
    {
        size_t a = value.find(';');
        if (a == std::string::npos)
            return false;
        size_t b = value.find(';', a + 1);
        if (b == std::string::npos)
            return false;
        out.mac = mac;
        out.addrType = static_cast<uint8_t>(std::atoi(value.substr(0, a).c_str()));
        out.profileName = value.substr(a + 1, b - a - 1);
        out.deviceName = value.substr(b + 1);
        return true;
    }
} // namespace

bool ConfigManager::begin()
{
    // Preferences initialises NVS on first use; make sure a corrupted or
    // outdated partition is recovered instead of failing silently.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        LOG_W(TAG, "NVS partition needs erase (%d), resetting configuration", err);
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK)
    {
        LOG_E(TAG, "NVS init failed: %d", err);
        return false;
    }
    loadAll();
    return true;
}

void ConfigManager::loadAll()
{
    RtosLock lock(mutex_);
    Preferences p;

    network_ = NetworkConfig{};
    network_.hostname = CATBRIDGE_HOSTNAME;
    if (p.begin(NS_NET, true))
    {
        network_.ssid = getString(p, "ssid", "");
        network_.password = getString(p, "pass", "");
        network_.hostname = getString(p, "host", network_.hostname);
        network_.apPassword = getString(p, "ap_pass", network_.apPassword);
        p.end();
    }
#if defined(CATBRIDGE_WIFI_SSID)
    // Compile time credentials (platformio_override.ini) are used when nothing
    // has been configured through the Web UI yet.
    if (network_.ssid.empty())
    {
        network_.ssid = CATBRIDGE_WIFI_SSID;
#if defined(CATBRIDGE_WIFI_PASSWORD)
        network_.password = CATBRIDGE_WIFI_PASSWORD;
#endif
    }
#endif

    bridge_ = BridgeConfig{};
    if (p.begin(NS_CFG, true))
    {
        bridge_.scanIntervalSec = p.getUShort("scan_int", bridge_.scanIntervalSec);
        bridge_.scanDurationSec = p.getUShort("scan_dur", bridge_.scanDurationSec);
        bridge_.offlineTimeoutSec = p.getUShort("offline_to", bridge_.offlineTimeoutSec);
        bridge_.probeUnknownDevices = p.getBool("probe", bridge_.probeUnknownDevices);
        bridge_.queueSize = p.getUChar("q_size", bridge_.queueSize);
        bridge_.maxRetries = p.getUChar("retries", bridge_.maxRetries);
        bridge_.logLevel = p.getUChar("log_lvl", bridge_.logLevel);
        bridge_.trimBlankRows = p.getBool("trim", bridge_.trimBlankRows);
        p.end();
    }

    protocol_ = ProtocolConfig{};
    if (p.begin(NS_PROTO, true))
    {
        protocol_.energy = p.getUShort("energy", protocol_.energy);
        protocol_.speed = p.getUChar("speed", protocol_.speed);
        protocol_.feedAfterJobLines = p.getUShort("feed", protocol_.feedAfterJobLines);
        protocol_.chunkSize = p.getUShort("chunk", protocol_.chunkSize);
        protocol_.packetDelayMs = p.getUChar("delay", protocol_.packetDelayMs);
        p.end();
    }

    aliases_.clear();
    if (p.begin(NS_ALIAS, true))
    {
        for (const auto &key : listStringKeys(NS_ALIAS))
        {
            std::string mac = macutil::normalize(key);
            if (!mac.empty())
                aliases_[mac] = getString(p, key.c_str(), "");
        }
        p.end();
    }
    LOG_I(TAG, "Configuration loaded (%u aliases)", static_cast<unsigned>(aliases_.size()));
}

NetworkConfig ConfigManager::network() const
{
    RtosLock lock(mutex_);
    return network_;
}

bool ConfigManager::setNetwork(const NetworkConfig &cfg)
{
    RtosLock lock(mutex_);
    Preferences p;
    if (!p.begin(NS_NET, false))
        return false;
    p.putString("ssid", cfg.ssid.c_str());
    p.putString("pass", cfg.password.c_str());
    p.putString("host", cfg.hostname.c_str());
    p.putString("ap_pass", cfg.apPassword.c_str());
    p.end();
    network_ = cfg;
    return true;
}

BridgeConfig ConfigManager::bridge() const
{
    RtosLock lock(mutex_);
    return bridge_;
}

bool ConfigManager::setBridge(const BridgeConfig &cfg)
{
    RtosLock lock(mutex_);
    Preferences p;
    if (!p.begin(NS_CFG, false))
        return false;
    p.putUShort("scan_int", cfg.scanIntervalSec);
    p.putUShort("scan_dur", cfg.scanDurationSec);
    p.putUShort("offline_to", cfg.offlineTimeoutSec);
    p.putBool("probe", cfg.probeUnknownDevices);
    p.putUChar("q_size", cfg.queueSize);
    p.putUChar("retries", cfg.maxRetries);
    p.putUChar("log_lvl", cfg.logLevel);
    p.putBool("trim", cfg.trimBlankRows);
    p.end();
    bridge_ = cfg;
    return true;
}

ProtocolConfig ConfigManager::protocol() const
{
    RtosLock lock(mutex_);
    return protocol_;
}

bool ConfigManager::setProtocol(const ProtocolConfig &cfg)
{
    RtosLock lock(mutex_);
    Preferences p;
    if (!p.begin(NS_PROTO, false))
        return false;
    p.putUShort("energy", cfg.energy);
    p.putUChar("speed", cfg.speed);
    p.putUShort("feed", cfg.feedAfterJobLines);
    p.putUShort("chunk", cfg.chunkSize);
    p.putUChar("delay", cfg.packetDelayMs);
    p.end();
    protocol_ = cfg;
    return true;
}

std::string ConfigManager::alias(const std::string &mac) const
{
    RtosLock lock(mutex_);
    auto it = aliases_.find(mac);
    return it == aliases_.end() ? std::string() : it->second;
}

bool ConfigManager::setAlias(const std::string &mac, const std::string &alias)
{
    std::string key = macutil::toId(mac);
    RtosLock lock(mutex_);
    Preferences p;
    if (!p.begin(NS_ALIAS, false))
        return false;
    if (alias.empty())
    {
        p.remove(key.c_str());
        aliases_.erase(mac);
    }
    else
    {
        p.putString(key.c_str(), alias.c_str());
        aliases_[mac] = alias;
    }
    p.end();
    return true;
}

std::map<std::string, KnownPrinter> ConfigManager::knownPrinters() const
{
    RtosLock lock(mutex_);
    std::map<std::string, KnownPrinter> out;
    Preferences p;
    if (!p.begin(NS_KNOWN, true))
        return out;
    for (const auto &key : listStringKeys(NS_KNOWN))
    {
        std::string mac = macutil::normalize(key);
        KnownPrinter k;
        if (!mac.empty() && decodeKnown(mac, getString(p, key.c_str(), ""), k))
            out[mac] = k;
    }
    p.end();
    return out;
}

bool ConfigManager::saveKnownPrinter(const KnownPrinter &printer)
{
    std::string key = macutil::toId(printer.mac);
    std::string value = encodeKnown(printer);
    RtosLock lock(mutex_);
    Preferences p;
    if (!p.begin(NS_KNOWN, false))
        return false;
    // Avoid flash wear: only write when the value actually changed.
    bool ok = true;
    if (getString(p, key.c_str(), "") != value)
        ok = p.putString(key.c_str(), value.c_str()) > 0;
    p.end();
    return ok;
}

bool ConfigManager::removeKnownPrinter(const std::string &mac)
{
    std::string key = macutil::toId(mac);
    RtosLock lock(mutex_);
    Preferences p;
    if (!p.begin(NS_KNOWN, false))
        return false;
    p.remove(key.c_str());
    p.end();
    return true;
}

void ConfigManager::factoryReset()
{
    {
        RtosLock lock(mutex_);
        for (const char *ns : {NS_NET, NS_CFG, NS_PROTO, NS_ALIAS, NS_KNOWN})
        {
            Preferences p;
            if (p.begin(ns, false))
            {
                p.clear();
                p.end();
            }
        }
    }
    loadAll();
}
