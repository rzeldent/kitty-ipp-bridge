/**
 * @file IppServer.h
 * @brief Minimal IPP Everywhere server exposing every BLE printer as a network printer.
 *
 * Endpoints (port 631):
 *
 *   ipp://catbridge.local/ipp/{printer-id}   one per registry entry (id = MAC without ':')
 *   ipp://catbridge.local/ipp/print          alias for the first known printer
 *
 * Supported operations:
 *
 *   Get-Printer-Attributes, Print-Job, Validate-Job, Get-Job-Attributes,
 *   Get-Jobs, Cancel-Job, Identify-Printer
 *
 * Document formats: application/pwg-raster and application/octet-stream
 * (auto-detected PWG raster). Documents are converted while they are being
 * received (see raster::PwgJobConverter) and handed to the JobQueue as a
 * 1-bit bitmap.
 *
 * The server runs in its own FreeRTOS task ("ipp") and handles one HTTP
 * connection at a time (HTTP/1.1, keep-alive, chunked transfer encoding,
 * Expect: 100-continue). The same task maintains the DNS-SD advertisements
 * (MdnsPublisher) so that the set of advertised printers follows the
 * registry dynamically, and recovers the mDNS responder after network loss.
 */
#pragma once

#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "IppMessage.h"
#include "MdnsPublisher.h"

class PrinterRegistry;
class JobQueue;
class ConfigManager;
struct PrinterRecord;
struct JobStatus;

class IppServer
{
public:
    struct Settings
    {
        uint16_t port = 631;
        uint16_t httpPort = 80;          ///< Web UI port (adminurl / printer-more-info)
        std::string hostname = "catbridge";
        size_t maxJobBytes = 160 * 1024; ///< Converted bitmap budget per job
        uint32_t ioTimeoutMs = 15000;    ///< Socket inactivity timeout while receiving a request
        uint32_t keepAliveMs = 3000;     ///< Idle time before an idle keep-alive connection is closed
    };

    bool begin(PrinterRegistry *registry, JobQueue *jobs, ConfigManager *config, const Settings &settings);
    bool startTask(UBaseType_t priority = 2, BaseType_t core = 0, uint32_t stackSize = 10240);

    /** Force a full mDNS re-advertisement (e.g. after the network came back). */
    void requestMdnsRestart() { mdnsRestart_ = true; }

    /** Number of IPP printers currently advertised via mDNS. */
    size_t advertisedPrinters() const { return advertised_; }

    /** Printer URI for a registry entry (using the mDNS host name). */
    std::string printerUri(const std::string &printerId) const;

private:
    class HttpConnection;
    class BodyReader;

    struct HttpRequest
    {
        std::string method;
        std::string path;
        std::string version;
        std::string host;
        std::string contentType;
        std::string transferEncoding;
        std::string connection;
        std::string expect;
        long contentLength = -1;
    };

    static void taskEntry(void *arg);
    void run();
    void maintainMdns();
    std::vector<MdnsPublisher::Entry> mdnsEntries() const;

    void handleClient(WiFiClient &client);
    bool readRequestHead(HttpConnection &conn, HttpRequest &req);
    void sendHttpResponse(HttpConnection &conn, int code, const char *reason, const char *contentType,
                          const uint8_t *body, size_t len, bool keepAlive);

    /** Process one IPP request; @return response body. */
    std::vector<uint8_t> handleIpp(const HttpRequest &http, BodyReader &body);

    bool resolvePrinter(const std::string &path, const ipp::Request &req, PrinterRecord &out) const;
    std::string baseUri(const HttpRequest &http) const;

    void opGetPrinterAttributes(const ipp::Request &req, const PrinterRecord &printer, const std::string &uri,
                                ipp::Writer &w);
    uint16_t opPrintJob(const ipp::Request &req, const PrinterRecord &printer, const std::string &uri,
                        BodyReader &body, std::vector<uint8_t> &leftover, ipp::Writer &w);
    uint16_t opValidateJob(const ipp::Request &req, ipp::Writer &w);
    uint16_t opGetJobAttributes(const ipp::Request &req, const PrinterRecord &printer, const std::string &uri,
                                ipp::Writer &w);
    uint16_t opGetJobs(const ipp::Request &req, const PrinterRecord &printer, const std::string &uri,
                       ipp::Writer &w);
    uint16_t opCancelJob(const ipp::Request &req, const PrinterRecord &printer);
    uint16_t opIdentifyPrinter(const PrinterRecord &printer);

    void writeJobAttributes(ipp::Writer &w, const JobStatus &job, const std::string &printerUri,
                            const std::vector<std::string> &requested) const;
    static uint32_t requestedJobId(const ipp::Request &req);
    static bool formatSupported(const std::string &format);
    static std::string printerUuid(const std::string &printerId);

    Settings settings_;
    PrinterRegistry *registry_ = nullptr;
    JobQueue *jobs_ = nullptr;
    ConfigManager *config_ = nullptr;
    WiFiServer *server_ = nullptr;
    TaskHandle_t task_ = nullptr;
    MdnsPublisher mdns_;
    std::atomic<bool> mdnsDirty_{true};
    std::atomic<bool> mdnsRestart_{false};
    std::atomic<size_t> advertised_{0};
    std::string statusMessage_; ///< status-message of the operation being processed (IPP task only)
    bool networkWasUp_ = false;
    uint32_t lastMdnsCheck_ = 0;
};
