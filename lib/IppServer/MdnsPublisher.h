/**
 * @file MdnsPublisher.h
 * @brief DNS-SD advertisement of the dynamic IPP printers.
 *
 * Every printer in the registry is advertised as its own `_ipp._tcp`
 * instance (sub-type `_print`) with IPP Everywhere TXT records, so Windows,
 * CUPS and macOS list one printer per BLE device:
 *
 *   "Desk Label Printer._ipp._tcp.local"  rp=ipp/aabbccddeeff
 *   "Kitchen Label Printer._ipp._tcp.local" rp=ipp/112233445566
 *
 * The publisher is reconciliation based: sync() receives the wanted set of
 * printers and adds / removes / renames instances as needed. restart() tears
 * the responder down and re-publishes everything (mDNS recovery after Wi-Fi
 * reconnects or responder failures).
 *
 * Not thread-safe: called from the IPP task only.
 */
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct PrinterRecord;

class MdnsPublisher
{
public:
    /** Data needed to advertise one printer. */
    struct Entry
    {
        std::string id;       ///< Printer id ("aabbccddeeff"), resource path ipp/{id}
        std::string name;     ///< Desired instance name (alias or generated)
        std::string uuid;     ///< printer-uuid without "urn:uuid:"
        std::string model;    ///< ty= value
        std::string location; ///< note= value
    };

    /** Start the responder with @p hostname (without ".local") and advertise the Web UI. */
    bool begin(const std::string &hostname, uint16_t ippPort, uint16_t httpPort);

    /** Reconcile advertised services with @p wanted. */
    void sync(const std::vector<Entry> &wanted);

    /** Stop and restart the responder, re-publishing everything. */
    bool restart();

    /** Verify the responder is still alive (hostname query succeeds). */
    bool healthy() const;

    bool running() const { return running_; }
    size_t publishedCount() const { return published_.size(); }

    /** Maximum number of IPP instances (limited by CONFIG_MDNS_MAX_SERVICES). */
    static constexpr size_t kMaxPrinters = 8;

private:
    bool publish(const Entry &entry, const std::string &instance);
    void unpublish(const std::string &instance);
    std::string uniqueInstanceName(const Entry &entry, const std::map<std::string, std::string> &taken) const;

    struct Published
    {
        std::string instance;
        Entry entry;
    };

    std::string hostname_;
    uint16_t ippPort_ = 631;
    uint16_t httpPort_ = 80;
    bool running_ = false;
    std::map<std::string, Published> published_; ///< key: printer id
    std::vector<Entry> lastWanted_;
};
