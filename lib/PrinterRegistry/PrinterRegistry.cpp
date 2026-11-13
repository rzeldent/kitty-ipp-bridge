#include "PrinterRegistry.h"

#include <Arduino.h>

#include "ConfigManager.h"
#include "Logging.h"
#include "MacUtil.h"

namespace
{
    constexpr const char *TAG = "registry";
}

std::string PrinterRecord::displayName() const
{
    if (!alias.empty())
        return alias;
    return "Cat Printer " + macutil::shortSuffix(mac);
}

const char *registryEventName(RegistryEvent event)
{
    switch (event)
    {
    case RegistryEvent::Added:
        return "added";
    case RegistryEvent::Updated:
        return "updated";
    case RegistryEvent::WentOnline:
        return "online";
    case RegistryEvent::WentOffline:
        return "offline";
    case RegistryEvent::AliasChanged:
        return "alias";
    case RegistryEvent::Removed:
        return "removed";
    }
    return "unknown";
}

uint32_t PrinterRegistry::nowSeconds()
{
    return static_cast<uint32_t>(millis() / 1000);
}

void PrinterRegistry::begin(ConfigManager *config, uint32_t offlineTimeoutSec)
{
    config_ = config;
    offlineTimeoutSec_ = offlineTimeoutSec;
    if (!config_)
        return;

    RtosLock lock(mutex_);
    for (const auto &kv : config_->knownPrinters())
    {
        PrinterRecord r;
        r.mac = kv.first;
        r.id = macutil::toId(r.mac);
        r.deviceName = kv.second.deviceName;
        r.profileName = kv.second.profileName;
        r.addrType = kv.second.addrType;
        r.alias = config_->alias(r.mac);
        r.online = false;
        r.rssi = 0;
        r.lastSeen = 0;
        printers_[r.mac] = r;
    }
    LOG_I(TAG, "Restored %u known printer(s)", static_cast<unsigned>(printers_.size()));
}

bool PrinterRegistry::upsert(const std::string &macIn, const std::string &deviceName, int rssi, uint8_t addrType,
                             const std::string &profileName)
{
    const std::string mac = macutil::normalize(macIn);
    if (mac.empty())
        return false;

    RegistryEvent event;
    PrinterRecord snapshot;
    bool persist = false;
    {
        RtosLock lock(mutex_);
        auto it = printers_.find(mac);
        const uint32_t now = nowSeconds();
        if (it == printers_.end())
        {
            PrinterRecord r;
            r.mac = mac;
            r.id = macutil::toId(mac);
            r.deviceName = deviceName;
            r.rssi = rssi;
            r.online = true;
            r.lastSeen = now;
            r.firstSeen = now;
            r.addrType = addrType;
            r.profileName = profileName;
            r.alias = config_ ? config_->alias(mac) : std::string();
            it = printers_.emplace(mac, r).first;
            event = RegistryEvent::Added;
            persist = true;
        }
        else
        {
            PrinterRecord &r = it->second;
            const bool wasOnline = r.online;
            if (!deviceName.empty() && deviceName != r.deviceName)
            {
                r.deviceName = deviceName;
                persist = true;
            }
            if (r.profileName != profileName || r.addrType != addrType)
            {
                r.profileName = profileName;
                r.addrType = addrType;
                persist = true;
            }
            if (r.firstSeen == 0)
                r.firstSeen = now;
            r.rssi = rssi;
            r.lastSeen = now;
            r.online = true;
            event = wasOnline ? RegistryEvent::Updated : RegistryEvent::WentOnline;
        }
        snapshot = it->second;
    }

    if (persist && config_)
        config_->saveKnownPrinter(KnownPrinter{snapshot.mac, snapshot.deviceName, snapshot.profileName, snapshot.addrType});

    if (event == RegistryEvent::Added)
        LOG_I(TAG, "New printer %s (%s) rssi=%d profile=%s", snapshot.mac.c_str(), snapshot.displayName().c_str(), rssi,
              profileName.c_str());
    else if (event == RegistryEvent::WentOnline)
        LOG_I(TAG, "Printer %s (%s) is online", snapshot.mac.c_str(), snapshot.displayName().c_str());

    notify(event, snapshot);
    return event == RegistryEvent::Added;
}

void PrinterRegistry::touch(const std::string &mac)
{
    PrinterRecord snapshot;
    bool wentOnline = false;
    {
        RtosLock lock(mutex_);
        auto it = printers_.find(mac);
        if (it == printers_.end())
            return;
        wentOnline = !it->second.online;
        it->second.online = true;
        it->second.lastSeen = nowSeconds();
        snapshot = it->second;
    }
    notify(wentOnline ? RegistryEvent::WentOnline : RegistryEvent::Updated, snapshot);
}

void PrinterRegistry::markOffline(const std::string &mac)
{
    PrinterRecord snapshot;
    {
        RtosLock lock(mutex_);
        auto it = printers_.find(mac);
        if (it == printers_.end() || !it->second.online)
            return;
        it->second.online = false;
        snapshot = it->second;
    }
    LOG_I(TAG, "Printer %s (%s) is offline", snapshot.mac.c_str(), snapshot.displayName().c_str());
    notify(RegistryEvent::WentOffline, snapshot);
}

size_t PrinterRegistry::checkTimeouts()
{
    std::vector<PrinterRecord> changed;
    {
        RtosLock lock(mutex_);
        const uint32_t now = nowSeconds();
        for (auto &kv : printers_)
        {
            PrinterRecord &r = kv.second;
            if (r.online && now - r.lastSeen >= offlineTimeoutSec_)
            {
                r.online = false;
                changed.push_back(r);
            }
        }
    }
    for (const auto &r : changed)
    {
        LOG_I(TAG, "Printer %s (%s) timed out -> offline", r.mac.c_str(), r.displayName().c_str());
        notify(RegistryEvent::WentOffline, r);
    }
    return changed.size();
}

bool PrinterRegistry::get(const std::string &mac, PrinterRecord &out) const
{
    RtosLock lock(mutex_);
    auto it = printers_.find(mac);
    if (it == printers_.end())
        return false;
    out = it->second;
    return true;
}

bool PrinterRegistry::find(const std::string &macOrId, PrinterRecord &out) const
{
    const std::string mac = macutil::normalize(macOrId);
    return !mac.empty() && get(mac, out);
}

std::vector<PrinterRecord> PrinterRegistry::list() const
{
    RtosLock lock(mutex_);
    std::vector<PrinterRecord> out;
    out.reserve(printers_.size());
    for (const auto &kv : printers_)
        out.push_back(kv.second);
    return out;
}

bool PrinterRegistry::setAlias(const std::string &mac, const std::string &alias)
{
    PrinterRecord snapshot;
    {
        RtosLock lock(mutex_);
        auto it = printers_.find(mac);
        if (it == printers_.end())
            return false;
        if (it->second.alias == alias)
            return true;
        it->second.alias = alias;
        snapshot = it->second;
    }
    if (config_ && !config_->setAlias(mac, alias))
        LOG_W(TAG, "Failed to persist alias for %s", mac.c_str());
    LOG_I(TAG, "Alias of %s set to '%s'", mac.c_str(), alias.c_str());
    notify(RegistryEvent::AliasChanged, snapshot);
    return true;
}

bool PrinterRegistry::remove(const std::string &mac)
{
    PrinterRecord snapshot;
    {
        RtosLock lock(mutex_);
        auto it = printers_.find(mac);
        if (it == printers_.end())
            return false;
        snapshot = it->second;
        printers_.erase(it);
    }
    if (config_)
    {
        config_->removeKnownPrinter(mac);
        config_->setAlias(mac, "");
    }
    LOG_I(TAG, "Printer %s removed from registry", mac.c_str());
    notify(RegistryEvent::Removed, snapshot);
    return true;
}

size_t PrinterRegistry::size() const
{
    RtosLock lock(mutex_);
    return printers_.size();
}

size_t PrinterRegistry::onlineCount() const
{
    RtosLock lock(mutex_);
    size_t n = 0;
    for (const auto &kv : printers_)
        n += kv.second.online ? 1 : 0;
    return n;
}

void PrinterRegistry::addListener(Listener listener)
{
    RtosLock lock(mutex_);
    listeners_.push_back(std::move(listener));
}

void PrinterRegistry::notify(RegistryEvent event, const PrinterRecord &record)
{
    std::vector<Listener> listeners;
    {
        RtosLock lock(mutex_);
        listeners = listeners_;
    }
    for (auto &l : listeners)
        l(event, record);
}
