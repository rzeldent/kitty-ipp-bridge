/**
 * @file MdnsPublisher.cpp
 * @brief DNS-SD advertisement of the dynamic IPP printers.
 */
#include "MdnsPublisher.h"

#include <ESPmDNS.h>
#include <mdns.h>

#include "Logging.h"
#include "SystemStats.h"

static const char *TAG = "mdns";

/** DNS-SD instance names are limited to 63 bytes. */
static constexpr size_t kMaxInstanceLen = 63;

bool MdnsPublisher::begin(const std::string &hostname, uint16_t ippPort, uint16_t httpPort)
{
    hostname_ = hostname;
    ippPort_ = ippPort;
    httpPort_ = httpPort;

    if (!MDNS.begin(hostname_.c_str()))
    {
        LOG_E(TAG, "mDNS responder failed to start");
        running_ = false;
        return false;
    }
    MDNS.setInstanceName("CatBridge");
    MDNS.addService("http", "tcp", httpPort_);
    MDNS.addServiceTxt("http", "tcp", "path", "/");
    running_ = true;
    SystemStats::instance().mdnsRestarts++;
    LOG_I(TAG, "mDNS responder started: %s.local", hostname_.c_str());
    return true;
}

bool MdnsPublisher::restart()
{
    LOG_W(TAG, "Restarting mDNS responder");
    MDNS.end();
    published_.clear();
    running_ = false;
    if (!begin(hostname_, ippPort_, httpPort_))
        return false;
    sync(lastWanted_);
    return true;
}

bool MdnsPublisher::healthy() const
{
    if (!running_)
        return false;
    char name[64] = {};
    return mdns_hostname_get(name) == ESP_OK && hostname_ == name;
}

std::string MdnsPublisher::uniqueInstanceName(const Entry &entry,
                                              const std::map<std::string, std::string> &taken) const
{
    std::string base = entry.name.substr(0, kMaxInstanceLen);
    std::string candidate = base;
    if (taken.count(candidate))
    {
        // Duplicate alias: disambiguate with the end of the MAC/id.
        const std::string suffix = " (" + entry.id.substr(entry.id.size() >= 4 ? entry.id.size() - 4 : 0) + ")";
        candidate = base.substr(0, kMaxInstanceLen - suffix.size()) + suffix;
    }
    return candidate;
}

void MdnsPublisher::sync(const std::vector<Entry> &wanted)
{
    lastWanted_ = wanted;
    if (!running_)
        return;

    // Compute desired instance names (unique, limited count).
    std::map<std::string, std::string> taken; // instance -> id
    std::map<std::string, std::pair<std::string, const Entry *>> desired; // id -> (instance, entry)
    for (const Entry &e : wanted)
    {
        if (desired.size() >= kMaxPrinters)
        {
            LOG_W(TAG, "mDNS service limit reached, %s not advertised (still reachable by URI)", e.id.c_str());
            continue;
        }
        const std::string instance = uniqueInstanceName(e, taken);
        taken[instance] = e.id;
        desired[e.id] = {instance, &e};
    }

    // Remove stale or changed services.
    for (auto it = published_.begin(); it != published_.end();)
    {
        auto d = desired.find(it->first);
        const bool keep = d != desired.end() && d->second.first == it->second.instance &&
                          d->second.second->uuid == it->second.entry.uuid &&
                          d->second.second->model == it->second.entry.model &&
                          d->second.second->location == it->second.entry.location;
        if (!keep)
        {
            unpublish(it->second.instance);
            it = published_.erase(it);
        }
        else
            ++it;
    }

    // Add new ones.
    for (const auto &d : desired)
    {
        if (published_.count(d.first))
            continue;
        if (publish(*d.second.second, d.second.first))
            published_[d.first] = Published{d.second.first, *d.second.second};
    }
}

bool MdnsPublisher::publish(const Entry &entry, const std::string &instance)
{
    const std::string rp = "ipp/" + entry.id;
    const std::string adminurl = "http://" + hostname_ + ".local/";
    const std::string product = "(" + entry.model + ")";

    // IPP Everywhere / Bonjour Printing TXT keys.
    mdns_txt_item_t txt[] = {
        {"txtvers", "1"},
        {"qtotal", "1"},
        {"rp", rp.c_str()},
        {"ty", entry.model.c_str()},
        {"product", product.c_str()},
        {"note", entry.location.c_str()},
        {"adminurl", adminurl.c_str()},
        {"pdl", "application/pwg-raster,application/octet-stream"},
        {"Color", "F"},
        {"Duplex", "F"},
        {"kind", "receipt,roll"},
        {"priority", "50"},
        {"UUID", entry.uuid.c_str()},
    };
    const size_t count = sizeof(txt) / sizeof(txt[0]);

    esp_err_t err = mdns_service_add_for_host(instance.c_str(), "_ipp", "_tcp", nullptr, ippPort_, txt, count);
    if (err != ESP_OK)
    {
        LOG_E(TAG, "Advertising '%s' failed: %s", instance.c_str(), esp_err_to_name(err));
        return false;
    }
    err = mdns_service_subtype_add_for_host(instance.c_str(), "_ipp", "_tcp", nullptr, "_print");
    if (err != ESP_OK)
        LOG_W(TAG, "Adding _print subtype for '%s' failed: %s", instance.c_str(), esp_err_to_name(err));

    LOG_I(TAG, "Advertising IPP printer '%s' (%s)", instance.c_str(), rp.c_str());
    return true;
}

void MdnsPublisher::unpublish(const std::string &instance)
{
    const esp_err_t err = mdns_service_remove_for_host(instance.c_str(), "_ipp", "_tcp", nullptr);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND)
        LOG_W(TAG, "Removing '%s' failed: %s", instance.c_str(), esp_err_to_name(err));
    else
        LOG_I(TAG, "Withdrew IPP printer '%s'", instance.c_str());
}
