/**
 * @file IppServer.cpp
 * @brief Minimal IPP Everywhere server (HTTP/1.1 transport, operations, mDNS upkeep).
 */
#include "IppServer.h"

#include <Arduino.h>

#include <algorithm>
#include <cctype>
#include <cstring>

#include "ConfigManager.h"
#include "JobQueue.h"
#include "Logging.h"
#include "MacUtil.h"
#include "PrinterProfile.h"
#include "PrinterRegistry.h"
#include "PwgJobConverter.h"
#include "SystemStats.h"
#include "Watchdog.h"

#ifndef CATBRIDGE_VERSION
#define CATBRIDGE_VERSION "dev"
#endif

static const char *TAG = "ipp";

namespace
{
    /** Media advertised to clients. Widths match the 48 mm printable area (384 dots @ 203 dpi). */
    struct MediaSize
    {
        const char *keyword;
        int32_t x; ///< hundredths of a millimetre
        int32_t y;
    };

    const MediaSize kMedia[] = {
        {"om_cat-small_48x50mm", 4800, 5000},
        {"om_cat-medium_48x100mm", 4800, 10000},
        {"om_cat-long_48x200mm", 4800, 20000},
    };
    const MediaSize &kDefaultMedia = kMedia[1];

    constexpr int32_t kDpi = 203;
    constexpr size_t kMaxHeaderLine = 1024;
    constexpr size_t kMaxHeaders = 48;
    constexpr size_t kMaxIppAttributeBytes = 16 * 1024;
    constexpr size_t kMaxDrainBytes = 64 * 1024;

    bool iequals(const std::string &a, const char *b)
    {
        const size_t n = strlen(b);
        if (a.size() != n)
            return false;
        for (size_t i = 0; i < n; ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        return true;
    }

    std::string lower(std::string s)
    {
        for (auto &c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    std::string trim(const std::string &s)
    {
        size_t b = 0, e = s.size();
        while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
            ++b;
        while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
            --e;
        return s.substr(b, e - b);
    }

    /** Keep only characters that are valid in a URI authority (the Host header is client controlled). */
    std::string sanitizeHost(const std::string &host)
    {
        std::string out;
        for (char c : host)
        {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == ':' || c == '[' ||
                c == ']')
                out.push_back(c);
            if (out.size() >= 128)
                break;
        }
        return out;
    }

    std::string hostWithoutPort(const std::string &host)
    {
        if (!host.empty() && host[0] == '[')
        {
            const size_t end = host.find(']');
            return end == std::string::npos ? host : host.substr(0, end + 1);
        }
        const size_t colon = host.find(':');
        return colon == std::string::npos ? host : host.substr(0, colon);
    }

    void writeMediaSize(ipp::Writer &w, const MediaSize &m)
    {
        w.memberName("x-dimension");
        w.integer("", m.x);
        w.memberName("y-dimension");
        w.integer("", m.y);
    }

    /** Write one media-col value (the caller supplies the attribute name, "" for additional values). */
    void writeMediaCol(ipp::Writer &w, const std::string &name, const MediaSize &m)
    {
        w.beginCollection(name);
        w.memberName("media-size");
        w.beginCollection("");
        writeMediaSize(w, m);
        w.endCollection();
        for (const char *margin :
             {"media-bottom-margin", "media-left-margin", "media-right-margin", "media-top-margin"})
        {
            w.memberName(margin);
            w.integer("", 0);
        }
        w.memberName("media-source");
        w.keyword("", "main");
        w.memberName("media-type");
        w.keyword("", "stationery");
        w.endCollection();
    }

    int32_t ippJobState(JobState s)
    {
        switch (s)
        {
        case JobState::Pending:
            return ipp::JOB_PENDING;
        case JobState::Processing:
            return ipp::JOB_PROCESSING;
        case JobState::Completed:
            return ipp::JOB_COMPLETED;
        case JobState::Failed:
            return ipp::JOB_ABORTED;
        case JobState::Cancelled:
            return ipp::JOB_CANCELED;
        }
        return ipp::JOB_PENDING;
    }

    const char *ippJobStateReason(JobState s)
    {
        switch (s)
        {
        case JobState::Pending:
            return "none";
        case JobState::Processing:
            return "job-printing";
        case JobState::Completed:
            return "job-completed-successfully";
        case JobState::Failed:
            return "aborted-by-system";
        case JobState::Cancelled:
            return "job-canceled-by-user";
        }
        return "none";
    }

    /** IPP times are "seconds since printer start" and must be non-zero. */
    int32_t ippTime(uint32_t uptimeSeconds)
    {
        return static_cast<int32_t>(uptimeSeconds + 1);
    }
} // namespace

// ===========================================================================
// HTTP transport helpers
// ===========================================================================

/** Buffered, timeout aware reader/writer on top of WiFiClient. */
class IppServer::HttpConnection
{
public:
    HttpConnection(WiFiClient &client, uint32_t timeoutMs) : client_(client), timeoutMs_(timeoutMs) {}

    bool hasBuffered() const { return pos_ < len_; }

    /** Make sure at least one byte is buffered. @return false on timeout or peer close. */
    bool fill()
    {
        if (pos_ < len_)
            return true;
        const uint32_t start = millis();
        for (;;)
        {
            const int avail = client_.available();
            if (avail > 0)
            {
                const int n = client_.read(buf_, std::min<size_t>(static_cast<size_t>(avail), sizeof(buf_)));
                if (n > 0)
                {
                    pos_ = 0;
                    len_ = static_cast<size_t>(n);
                    return true;
                }
            }
            else if (!client_.connected())
                return false;
            if (millis() - start > timeoutMs_)
                return false;
            watchdog::feed();
            delay(2);
        }
    }

    int readByte()
    {
        if (!fill())
            return -1;
        return buf_[pos_++];
    }

    size_t read(uint8_t *out, size_t max)
    {
        if (!max || !fill())
            return 0;
        const size_t n = std::min(max, len_ - pos_);
        memcpy(out, buf_ + pos_, n);
        pos_ += n;
        return n;
    }

    /** Read a CRLF (or LF) terminated line without the terminator. */
    bool readLine(std::string &line, size_t max)
    {
        line.clear();
        for (;;)
        {
            const int ch = readByte();
            if (ch < 0)
                return false;
            if (ch == '\n')
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                return true;
            }
            if (line.size() >= max)
                return false;
            line.push_back(static_cast<char>(ch));
        }
    }

    bool write(const uint8_t *data, size_t len)
    {
        size_t off = 0;
        uint32_t start = millis();
        while (off < len)
        {
            const size_t n = client_.write(data + off, len - off);
            if (n == 0)
            {
                if (!client_.connected() || millis() - start > timeoutMs_)
                    return false;
                delay(5);
                continue;
            }
            off += n;
            start = millis();
        }
        return true;
    }

    bool write(const std::string &s) { return write(reinterpret_cast<const uint8_t *>(s.data()), s.size()); }

private:
    WiFiClient &client_;
    uint32_t timeoutMs_;
    uint8_t buf_[1024];
    size_t pos_ = 0;
    size_t len_ = 0;
};

/** HTTP message body reader supporting Content-Length and chunked transfer encoding. */
class IppServer::BodyReader
{
public:
    BodyReader(HttpConnection &conn, bool chunked, long contentLength)
        : conn_(conn), chunked_(chunked), remaining_(contentLength > 0 ? static_cast<size_t>(contentLength) : 0)
    {
        done_ = !chunked_ && remaining_ == 0;
    }

    /** Read up to @p max body bytes. @return 0 at end of body or on error. */
    size_t read(uint8_t *out, size_t max)
    {
        if (done_ || error_)
            return 0;
        if (chunked_)
        {
            if (remaining_ == 0)
            {
                std::string line;
                if (needCrlf_)
                {
                    if (!conn_.readLine(line, 16) || !line.empty())
                        return setError();
                    needCrlf_ = false;
                }
                if (!conn_.readLine(line, 128))
                    return setError();
                char *end = nullptr;
                const unsigned long size = strtoul(line.c_str(), &end, 16);
                if (end == line.c_str())
                    return setError();
                if (size == 0)
                {
                    // Skip optional trailers up to the terminating empty line.
                    for (int i = 0; i < 32; ++i)
                    {
                        if (!conn_.readLine(line, kMaxHeaderLine))
                            return setError();
                        if (line.empty())
                        {
                            done_ = true;
                            return 0;
                        }
                    }
                    return setError();
                }
                remaining_ = size;
                needCrlf_ = true;
            }
        }
        const size_t n = conn_.read(out, std::min(max, remaining_));
        if (n == 0)
            return setError();
        remaining_ -= n;
        if (!chunked_ && remaining_ == 0)
            done_ = true;
        return n;
    }

    /** Discard the rest of the body (bounded). @return true when the body end was reached. */
    bool drain(size_t maxBytes)
    {
        uint8_t tmp[256];
        size_t total = 0;
        while (!done_ && !error_ && total <= maxBytes)
        {
            const size_t n = read(tmp, sizeof(tmp));
            if (!n)
                break;
            total += n;
        }
        return done_;
    }

    bool done() const { return done_; }
    bool error() const { return error_; }

private:
    size_t setError()
    {
        error_ = true;
        return 0;
    }

    HttpConnection &conn_;
    bool chunked_;
    size_t remaining_;
    bool needCrlf_ = false;
    bool done_ = false;
    bool error_ = false;
};

// ===========================================================================
// Life cycle
// ===========================================================================
bool IppServer::begin(PrinterRegistry *registry, JobQueue *jobs, ConfigManager *config, const Settings &settings)
{
    registry_ = registry;
    jobs_ = jobs;
    config_ = config;
    settings_ = settings;

    registry_->addListener([this](RegistryEvent event, const PrinterRecord &) {
        if (event == RegistryEvent::Added || event == RegistryEvent::AliasChanged || event == RegistryEvent::Removed)
            mdnsDirty_ = true;
    });

    server_ = new WiFiServer(settings_.port, 4);
    return server_ != nullptr;
}

bool IppServer::startTask(UBaseType_t priority, BaseType_t core, uint32_t stackSize)
{
    return xTaskCreatePinnedToCore(taskEntry, "ipp", stackSize, this, priority, &task_, core) == pdPASS;
}

void IppServer::taskEntry(void *arg)
{
    static_cast<IppServer *>(arg)->run();
}

std::string IppServer::printerUri(const std::string &printerId) const
{
    std::string uri = "ipp://" + settings_.hostname + ".local";
    if (settings_.port != 631)
        uri += ":" + std::to_string(settings_.port);
    return uri + "/ipp/" + printerId;
}

std::string IppServer::printerUuid(const std::string &printerId)
{
    // Stable, MAC derived RFC 4122 layout: "catbridge" prefix + printer id.
    return "63617462-7269-4467-8565-" + printerId;
}

void IppServer::run()
{
    watchdog::subscribe();
    server_->begin();
    server_->setNoDelay(true);
    LOG_I(TAG, "IPP server listening on port %u", settings_.port);

    for (;;)
    {
        watchdog::feed();
        maintainMdns();

        WiFiClient client = server_->accept();
        if (client)
        {
            handleClient(client);
            client.stop();
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

// ===========================================================================
// mDNS maintenance (dynamic printers + recovery)
// ===========================================================================
std::vector<MdnsPublisher::Entry> IppServer::mdnsEntries() const
{
    std::vector<MdnsPublisher::Entry> entries;
    for (const PrinterRecord &r : registry_->list())
    {
        MdnsPublisher::Entry e;
        e.id = r.id;
        e.name = r.displayName();
        e.uuid = printerUuid(r.id);
        e.model = "CatBridge Cat Printer";
        e.location = "BLE " + r.mac;
        entries.push_back(e);
    }
    return entries;
}

void IppServer::maintainMdns()
{
    const bool up = WiFi.isConnected() || (WiFi.getMode() & WIFI_MODE_AP);
    if (!up)
    {
        networkWasUp_ = false;
        return;
    }
    if (!networkWasUp_)
    {
        networkWasUp_ = true;
        lastMdnsCheck_ = millis();
        // Network (re)appeared: start or restart the responder so it binds to the new address.
        if (mdns_.running())
            mdns_.restart();
        else
            mdns_.begin(settings_.hostname, settings_.port, settings_.httpPort);
        mdnsDirty_ = true;
    }
    if (mdnsRestart_.exchange(false))
    {
        mdns_.restart();
        mdnsDirty_ = true;
    }
    if (mdnsDirty_.exchange(false))
    {
        mdns_.sync(mdnsEntries());
        advertised_ = mdns_.publishedCount();
    }
    if (millis() - lastMdnsCheck_ > 60000)
    {
        lastMdnsCheck_ = millis();
        if (!mdns_.healthy())
        {
            LOG_W(TAG, "mDNS responder unhealthy, recovering");
            mdns_.restart();
            advertised_ = mdns_.publishedCount();
        }
    }
}

// ===========================================================================
// HTTP handling
// ===========================================================================
bool IppServer::readRequestHead(HttpConnection &conn, HttpRequest &req)
{
    std::string line;
    // Tolerate stray empty lines between pipelined requests.
    do
    {
        if (!conn.readLine(line, kMaxHeaderLine))
            return false;
    } while (line.empty());

    const size_t sp1 = line.find(' ');
    const size_t sp2 = line.rfind(' ');
    if (sp1 == std::string::npos || sp2 == sp1)
        return false;
    req.method = line.substr(0, sp1);
    req.path = line.substr(sp1 + 1, sp2 - sp1 - 1);
    req.version = line.substr(sp2 + 1);

    for (size_t count = 0;; ++count)
    {
        if (count > kMaxHeaders || !conn.readLine(line, kMaxHeaderLine))
            return false;
        if (line.empty())
            break;
        const size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        const std::string name = lower(trim(line.substr(0, colon)));
        const std::string value = trim(line.substr(colon + 1));
        if (name == "host")
            req.host = value;
        else if (name == "content-type")
            req.contentType = value;
        else if (name == "content-length")
            req.contentLength = strtol(value.c_str(), nullptr, 10);
        else if (name == "transfer-encoding")
            req.transferEncoding = lower(value);
        else if (name == "connection")
            req.connection = value;
        else if (name == "expect")
            req.expect = value;
    }
    return true;
}

void IppServer::sendHttpResponse(HttpConnection &conn, int code, const char *reason, const char *contentType,
                                 const uint8_t *body, size_t len, bool keepAlive)
{
    char head[256];
    const int n = snprintf(head, sizeof(head),
                           "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nConnection: %s\r\n"
                           "Server: CatBridge/%s IPP/2.0\r\n\r\n",
                           code, reason, contentType, static_cast<unsigned>(len), keepAlive ? "keep-alive" : "close",
                           CATBRIDGE_VERSION);
    if (n > 0 && conn.write(reinterpret_cast<const uint8_t *>(head), static_cast<size_t>(n)) && len)
        conn.write(body, len);
}

void IppServer::handleClient(WiFiClient &client)
{
    client.setNoDelay(true);
    HttpConnection conn(client, settings_.ioTimeoutMs);

    for (int served = 0; served < 100; ++served)
    {
        if (served > 0)
        {
            // Keep-alive: wait briefly for the next request, but yield to waiting clients.
            const uint32_t start = millis();
            while (!conn.hasBuffered() && client.available() <= 0)
            {
                if (!client.connected() || millis() - start > settings_.keepAliveMs || server_->hasClient())
                    return;
                watchdog::feed();
                delay(10);
            }
        }

        HttpRequest req;
        if (!readRequestHead(conn, req))
            return;

        bool keepAlive = req.version == "HTTP/1.1" ? !iequals(req.connection, "close")
                                                    : iequals(req.connection, "keep-alive");

        if (req.method == "GET" || req.method == "HEAD")
        {
            const std::string host = hostWithoutPort(sanitizeHost(req.host.empty() ? settings_.hostname + ".local"
                                                                                    : req.host));
            const std::string html = "<!doctype html><html><head><title>CatBridge</title></head><body>"
                                     "<h1>CatBridge IPP endpoint</h1><p>Manage printers at <a href=\"http://" +
                                     host + "/\">http://" + host + "/</a></p></body></html>";
            const bool head = req.method == "HEAD";
            sendHttpResponse(conn, 200, "OK", "text/html", reinterpret_cast<const uint8_t *>(html.data()),
                             head ? 0 : html.size(), keepAlive);
            if (!keepAlive)
                return;
            continue;
        }

        if (req.method != "POST")
        {
            sendHttpResponse(conn, 405, "Method Not Allowed", "text/plain", nullptr, 0, false);
            return;
        }
        if (lower(req.contentType).rfind("application/ipp", 0) != 0)
        {
            sendHttpResponse(conn, 415, "Unsupported Media Type", "text/plain", nullptr, 0, false);
            return;
        }
        const bool chunked = req.transferEncoding.find("chunked") != std::string::npos;
        if (!chunked && req.contentLength < 0)
        {
            sendHttpResponse(conn, 411, "Length Required", "text/plain", nullptr, 0, false);
            return;
        }
        if (iequals(req.expect, "100-continue"))
            conn.write(std::string("HTTP/1.1 100 Continue\r\n\r\n"));

        BodyReader body(conn, chunked, req.contentLength);
        const std::vector<uint8_t> response = handleIpp(req, body);

        if (!body.done() && !body.drain(kMaxDrainBytes))
            keepAlive = false; // unread document data left: the connection cannot be reused

        sendHttpResponse(conn, 200, "OK", "application/ipp", response.data(), response.size(), keepAlive);
        if (!keepAlive)
            return;
    }
}

// ===========================================================================
// IPP dispatch
// ===========================================================================
std::string IppServer::baseUri(const HttpRequest &http) const
{
    std::string host = sanitizeHost(http.host);
    if (host.empty())
    {
        host = settings_.hostname + ".local";
        if (settings_.port != 631)
            host += ":" + std::to_string(settings_.port);
    }
    return "ipp://" + host;
}

bool IppServer::resolvePrinter(const std::string &rawPath, const ipp::Request &req, PrinterRecord &out) const
{
    auto fromPath = [&](std::string path) -> int {
        const size_t q = path.find('?');
        if (q != std::string::npos)
            path.resize(q);
        // Accept absolute URIs (printer-uri attribute) as well as plain paths.
        const size_t scheme = path.find("://");
        if (scheme != std::string::npos)
        {
            const size_t slash = path.find('/', scheme + 3);
            path = slash == std::string::npos ? "/" : path.substr(slash);
        }
        while (path.size() > 1 && path.back() == '/')
            path.pop_back();

        std::string id;
        if (path == "/" || path == "/ipp" || path == "/ipp/print")
            id.clear(); // default printer
        else if (path.rfind("/ipp/", 0) == 0)
        {
            id = path.substr(5);
            const size_t slash = id.find('/');
            if (slash != std::string::npos)
                id.resize(slash);
        }
        else
            return -1; // not an IPP path

        if (!id.empty())
            return registry_->find(id, out) ? 1 : 0;

        // Default printer: first online one, else the first known.
        const auto all = registry_->list();
        for (const auto &r : all)
            if (r.online)
            {
                out = r;
                return 1;
            }
        if (!all.empty())
        {
            out = all.front();
            return 1;
        }
        return 0;
    };

    const int byPath = fromPath(rawPath);
    if (byPath >= 0)
        return byPath == 1;
    const std::string uri = req.getString("printer-uri");
    return !uri.empty() && fromPath(uri) == 1;
}

std::vector<uint8_t> IppServer::handleIpp(const HttpRequest &http, BodyReader &body)
{
    SystemStats::instance().ippRequests++;

    ipp::RequestParser parser(kMaxIppAttributeBytes);
    std::vector<uint8_t> leftover;
    uint8_t buf[512];
    while (!parser.complete() && !parser.error())
    {
        const size_t n = body.read(buf, sizeof(buf));
        if (!n)
            break;
        const size_t used = parser.feed(buf, n);
        if (parser.complete() && used < n)
            leftover.assign(buf + used, buf + n);
    }

    const ipp::Request &req = parser.request();
    uint8_t major = req.versionMajor ? req.versionMajor : 1;
    uint8_t minor = req.versionMajor ? req.versionMinor : 1;
    uint16_t status = ipp::STATUS_OK;
    std::string message;
    ipp::Writer groups; // everything after the operation attributes group

    if (!parser.complete())
    {
        status = ipp::CLIENT_ERROR_BAD_REQUEST;
        message = parser.error() ? parser.errorMessage() : "incomplete request";
    }
    else if (req.versionMajor < 1 || req.versionMajor > 2)
    {
        status = ipp::SERVER_ERROR_VERSION_NOT_SUPPORTED;
        major = 1;
        minor = 1;
        message = "unsupported IPP version";
    }
    else
    {
        PrinterRecord printer;
        if (!resolvePrinter(http.path, req, printer))
        {
            status = ipp::CLIENT_ERROR_NOT_FOUND;
            message = "printer not found";
        }
        else
        {
            const std::string uri = baseUri(http) + "/ipp/" + printer.id;
            statusMessage_.clear();
            LOG_D(TAG, "%s for %s", ipp::operationName(req.operation), printer.id.c_str());
            switch (req.operation)
            {
            case ipp::OP_GET_PRINTER_ATTRIBUTES:
                opGetPrinterAttributes(req, printer, uri, groups);
                break;
            case ipp::OP_PRINT_JOB:
                status = opPrintJob(req, printer, uri, body, leftover, groups);
                break;
            case ipp::OP_VALIDATE_JOB:
                status = opValidateJob(req, groups);
                break;
            case ipp::OP_GET_JOB_ATTRIBUTES:
                status = opGetJobAttributes(req, printer, uri, groups);
                break;
            case ipp::OP_GET_JOBS:
                status = opGetJobs(req, printer, uri, groups);
                break;
            case ipp::OP_CANCEL_JOB:
                status = opCancelJob(req, printer);
                break;
            case ipp::OP_IDENTIFY_PRINTER:
                status = opIdentifyPrinter(printer);
                break;
            default:
                status = ipp::SERVER_ERROR_OPERATION_NOT_SUPPORTED;
                statusMessage_ = "operation not supported";
                break;
            }
            message = statusMessage_;
        }
    }

    if (status >= 0x0400)
    {
        SystemStats::instance().ippErrors++;
        LOG_W(TAG, "%s failed: 0x%04x %s", ipp::operationName(req.operation), status, message.c_str());
    }

    ipp::Writer out;
    out.header(major, minor, status, req.requestId);
    out.group(ipp::TAG_OPERATION);
    out.charset("attributes-charset", "utf-8");
    out.language("attributes-natural-language", "en");
    if (!message.empty())
        out.text("status-message", message);
    out.data().insert(out.data().end(), groups.data().begin(), groups.data().end());
    out.end();
    return std::move(out.data());
}

bool IppServer::formatSupported(const std::string &format)
{
    return format.empty() || format == "application/pwg-raster" || format == "application/octet-stream" ||
           format == "image/pwg-raster";
}

uint32_t IppServer::requestedJobId(const ipp::Request &req)
{
    const int32_t id = req.getInt("job-id", 0);
    if (id > 0)
        return static_cast<uint32_t>(id);
    const std::string uri = req.getString("job-uri");
    const size_t slash = uri.rfind('/');
    if (slash != std::string::npos)
        return static_cast<uint32_t>(strtoul(uri.c_str() + slash + 1, nullptr, 10));
    return 0;
}

// ===========================================================================
// Operations
// ===========================================================================
void IppServer::opGetPrinterAttributes(const ipp::Request &req, const PrinterRecord &printer,
                                       const std::string &uri, ipp::Writer &w)
{
    const std::vector<std::string> requested = req.getStrings("requested-attributes");
    const bool all = requested.empty() ||
                     std::any_of(requested.begin(), requested.end(), [](const std::string &s) {
                         return s == "all" || s == "printer-description" || s == "job-template";
                     });
    auto want = [&](const char *name) {
        return all || std::find(requested.begin(), requested.end(), name) != requested.end();
    };

    const PrinterProfile *profile = PrinterProfiles::findByName(printer.profileName);
    if (!profile)
        profile = &PrinterProfiles::defaultProfile();
    const bool busy = jobs_->printerBusy(printer.mac);
    const bool accepting = jobs_->pendingCount() < jobs_->capacity();
    const std::string host = hostWithoutPort(uri.substr(6, uri.find('/', 6) - 6));
    const uint32_t uptime = PrinterRegistry::nowSeconds();

    w.group(ipp::TAG_PRINTER);

    // --- Identification ---------------------------------------------------
    if (want("printer-uri-supported"))
        w.uri("printer-uri-supported", uri);
    if (want("uri-security-supported"))
        w.keyword("uri-security-supported", "none");
    if (want("uri-authentication-supported"))
        w.keyword("uri-authentication-supported", "none");
    if (want("printer-name"))
        w.name("printer-name", printer.displayName());
    if (want("printer-info"))
        w.text("printer-info", printer.displayName());
    if (want("printer-location"))
        w.text("printer-location", "BLE " + printer.mac);
    if (want("printer-make-and-model"))
        w.text("printer-make-and-model", "CatBridge Cat Printer (" + profile->profileName + ")");
    if (want("printer-device-id"))
        w.text("printer-device-id", "MFG:CatBridge;MDL:Cat Printer;CMD:PWGRaster;CLS:PRINTER;");
    if (want("printer-uuid"))
        w.uri("printer-uuid", "urn:uuid:" + printerUuid(printer.id));
    if (want("printer-more-info"))
        w.uri("printer-more-info", "http://" + host + "/");
    if (want("printer-firmware-name"))
        w.name("printer-firmware-name", "CatBridge");
    if (want("printer-firmware-string-version"))
        w.text("printer-firmware-string-version", CATBRIDGE_VERSION);
    if (want("printer-kind"))
        w.keywords("printer-kind", {"receipt", "roll"});

    // --- State ------------------------------------------------------------
    if (want("printer-state"))
        w.enumValue("printer-state", busy ? ipp::PRINTER_PROCESSING : ipp::PRINTER_IDLE);
    if (want("printer-state-reasons"))
        w.keyword("printer-state-reasons", printer.online ? "none" : "offline-report");
    if (want("printer-state-message"))
        w.text("printer-state-message",
               printer.online ? (busy ? "Printing" : "Ready") : "Printer is switched off or out of BLE range");
    if (want("printer-is-accepting-jobs"))
        w.boolean("printer-is-accepting-jobs", accepting);
    if (want("queued-job-count"))
    {
        int32_t queued = 0;
        for (const auto &j : jobs_->list())
            if (j.printerMac == printer.mac && !j.finished())
                ++queued;
        w.integer("queued-job-count", queued);
    }
    if (want("printer-up-time"))
        w.integer("printer-up-time", ippTime(uptime));
    if (want("printer-config-change-time"))
        w.integer("printer-config-change-time", 1);
    if (want("printer-state-change-time"))
        w.integer("printer-state-change-time", ippTime(printer.lastSeen));

    // --- Protocol ---------------------------------------------------------
    if (want("ipp-versions-supported"))
        w.keywords("ipp-versions-supported", {"1.1", "2.0"});
    if (want("ipp-features-supported"))
        w.keywords("ipp-features-supported", {"ipp-everywhere"});
    if (want("operations-supported"))
        w.integers(ipp::TAG_ENUM, "operations-supported",
                   {ipp::OP_PRINT_JOB, ipp::OP_VALIDATE_JOB, ipp::OP_CANCEL_JOB, ipp::OP_GET_JOB_ATTRIBUTES,
                    ipp::OP_GET_JOBS, ipp::OP_GET_PRINTER_ATTRIBUTES, ipp::OP_IDENTIFY_PRINTER});
    if (want("charset-configured"))
        w.charset("charset-configured", "utf-8");
    if (want("charset-supported"))
        w.charset("charset-supported", "utf-8");
    if (want("natural-language-configured"))
        w.language("natural-language-configured", "en");
    if (want("generated-natural-language-supported"))
        w.language("generated-natural-language-supported", "en");
    if (want("compression-supported"))
        w.keyword("compression-supported", "none");
    if (want("pdl-override-supported"))
        w.keyword("pdl-override-supported", "attempted");
    if (want("multiple-document-jobs-supported"))
        w.boolean("multiple-document-jobs-supported", false);
    if (want("multiple-operation-time-out"))
        w.integer("multiple-operation-time-out", 60);
    if (want("which-jobs-supported"))
        w.keywords("which-jobs-supported", {"completed", "not-completed"});
    if (want("job-ids-supported"))
        w.boolean("job-ids-supported", true);
    if (want("identify-actions-default"))
        w.keyword("identify-actions-default", "display");
    if (want("identify-actions-supported"))
        w.keyword("identify-actions-supported", "display");

    // --- Document formats -------------------------------------------------
    if (want("document-format-default"))
        w.mimeType("document-format-default", "application/octet-stream");
    if (want("document-format-supported"))
        w.strings(ipp::TAG_MIME_TYPE, "document-format-supported",
                  {"application/pwg-raster", "application/octet-stream"});
    if (want("pwg-raster-document-type-supported"))
        w.keywords("pwg-raster-document-type-supported", {"black_1", "sgray_8"});
    if (want("pwg-raster-document-resolution-supported"))
        w.resolution("pwg-raster-document-resolution-supported", kDpi, kDpi);
    if (want("pwg-raster-document-sheet-back"))
        w.keyword("pwg-raster-document-sheet-back", "normal");

    // --- Job template -----------------------------------------------------
    if (want("job-creation-attributes-supported"))
        w.keywords("job-creation-attributes-supported",
                   {"copies", "media", "media-col", "orientation-requested", "print-color-mode", "print-quality",
                    "printer-resolution", "sides"});
    if (want("copies-default"))
        w.integer("copies-default", 1);
    if (want("copies-supported"))
        w.range("copies-supported", 1, 1);
    if (want("color-supported"))
        w.boolean("color-supported", false);
    if (want("print-color-mode-default"))
        w.keyword("print-color-mode-default", "monochrome");
    if (want("print-color-mode-supported"))
        w.keywords("print-color-mode-supported", {"auto", "monochrome"});
    if (want("print-quality-default"))
        w.enumValue("print-quality-default", 4);
    if (want("print-quality-supported"))
        w.integers(ipp::TAG_ENUM, "print-quality-supported", {3, 4, 5});
    if (want("printer-resolution-default"))
        w.resolution("printer-resolution-default", kDpi, kDpi);
    if (want("printer-resolution-supported"))
        w.resolution("printer-resolution-supported", kDpi, kDpi);
    if (want("sides-default"))
        w.keyword("sides-default", "one-sided");
    if (want("sides-supported"))
        w.keyword("sides-supported", "one-sided");
    if (want("orientation-requested-default"))
        w.enumValue("orientation-requested-default", 3);
    if (want("orientation-requested-supported"))
        w.integers(ipp::TAG_ENUM, "orientation-requested-supported", {3});
    if (want("job-sheets-default"))
        w.name("job-sheets-default", "none");
    if (want("job-sheets-supported"))
        w.name("job-sheets-supported", "none");
    if (want("output-bin-default"))
        w.keyword("output-bin-default", "face-up");
    if (want("output-bin-supported"))
        w.keyword("output-bin-supported", "face-up");

    // --- Media ------------------------------------------------------------
    std::vector<std::string> mediaNames;
    for (const auto &m : kMedia)
        mediaNames.emplace_back(m.keyword);
    if (want("media-default"))
        w.keyword("media-default", kDefaultMedia.keyword);
    if (want("media-supported"))
        w.keywords("media-supported", mediaNames);
    if (want("media-ready"))
        w.keyword("media-ready", kDefaultMedia.keyword);
    if (want("media-col-default"))
        writeMediaCol(w, "media-col-default", kDefaultMedia);
    if (want("media-col-ready"))
        writeMediaCol(w, "media-col-ready", kDefaultMedia);
    if (want("media-col-database"))
    {
        bool first = true;
        for (const auto &m : kMedia)
        {
            writeMediaCol(w, first ? "media-col-database" : "", m);
            first = false;
        }
    }
    if (want("media-col-supported"))
        w.keywords("media-col-supported", {"media-bottom-margin", "media-left-margin", "media-right-margin",
                                           "media-size", "media-source", "media-top-margin", "media-type"});
    if (want("media-size-supported"))
    {
        bool first = true;
        for (const auto &m : kMedia)
        {
            w.beginCollection(first ? "media-size-supported" : "");
            writeMediaSize(w, m);
            w.endCollection();
            first = false;
        }
    }
    for (const char *margin : {"media-bottom-margin-supported", "media-left-margin-supported",
                               "media-right-margin-supported", "media-top-margin-supported"})
        if (want(margin))
            w.integer(margin, 0);
    if (want("media-source-supported"))
        w.keyword("media-source-supported", "main");
    if (want("media-type-supported"))
        w.keyword("media-type-supported", "stationery");
}

uint16_t IppServer::opValidateJob(const ipp::Request &req, ipp::Writer &w)
{
    const std::string format = req.getString("document-format");
    if (!formatSupported(format))
    {
        w.group(ipp::TAG_UNSUPPORTED_GROUP);
        w.mimeType("document-format", format);
        statusMessage_ = "document format not supported";
        return ipp::CLIENT_ERROR_DOCUMENT_FORMAT_NOT_SUPPORTED;
    }
    const std::string compression = req.getString("compression", "none");
    if (compression != "none")
    {
        w.group(ipp::TAG_UNSUPPORTED_GROUP);
        w.keyword("compression", compression);
        statusMessage_ = "compression not supported";
        return ipp::CLIENT_ERROR_ATTRIBUTES_OR_VALUES_NOT_SUPPORTED;
    }
    return ipp::STATUS_OK;
}

uint16_t IppServer::opPrintJob(const ipp::Request &req, const PrinterRecord &printer, const std::string &uri,
                               BodyReader &body, std::vector<uint8_t> &leftover, ipp::Writer &w)
{
    const uint16_t valid = opValidateJob(req, w);
    if (valid != ipp::STATUS_OK)
        return valid;

    if (jobs_->pendingCount() >= jobs_->capacity())
    {
        statusMessage_ = "print queue full";
        return ipp::SERVER_ERROR_BUSY;
    }

    const PrinterProfile *profile = PrinterProfiles::findByName(printer.profileName);
    const BridgeConfig bridge = config_->bridge();

    raster::PwgJobConverter::Options options;
    options.width = profile ? profile->printWidthPx : 384;
    options.trimBlankRows = bridge.trimBlankRows;
    options.maxOutputBytes = settings_.maxJobBytes;
    raster::PwgJobConverter converter(options);

    const uint32_t start = millis();
    size_t received = leftover.size();
    bool ok = leftover.empty() || converter.feed(leftover.data(), leftover.size());
    std::vector<uint8_t>().swap(leftover);

    uint8_t buf[1024];
    while (ok)
    {
        const size_t n = body.read(buf, sizeof(buf));
        if (!n)
            break;
        received += n;
        ok = converter.feed(buf, n);
        watchdog::feed();
    }
    if (body.error())
    {
        statusMessage_ = "connection error while receiving document";
        return ipp::CLIENT_ERROR_BAD_REQUEST;
    }
    if (ok)
        ok = converter.finish();
    if (!ok)
    {
        statusMessage_ = converter.error();
        return converter.overBudget() ? ipp::CLIENT_ERROR_REQUEST_ENTITY_TOO_LARGE
                                      : ipp::CLIENT_ERROR_DOCUMENT_FORMAT_ERROR;
    }

    raster::MonoBitmap &bitmap = converter.bitmap();
    LOG_I(TAG, "Received %u bytes, %u page(s) -> %u rows for %s in %u ms", static_cast<unsigned>(received),
          static_cast<unsigned>(converter.pages()), static_cast<unsigned>(bitmap.height), printer.id.c_str(),
          static_cast<unsigned>(millis() - start));

    PrintJob job;
    job.printerMac = printer.mac;
    job.name = req.getString("job-name", "IPP job");
    job.user = req.getString("requesting-user-name", "anonymous");
    if (bitmap.height == 0)
    {
        job.type = JobType::FeedPaper; // blank document: only feed paper
    }
    else
    {
        job.type = JobType::Bitmap;
        job.widthPx = bitmap.width;
        job.heightPx = bitmap.height;
        job.payload = std::move(bitmap.data);
    }

    std::string error;
    const uint32_t id = jobs_->submit(std::move(job), &error);
    if (!id)
    {
        statusMessage_ = error;
        return ipp::SERVER_ERROR_BUSY;
    }

    JobStatus status;
    jobs_->getStatus(id, status);
    w.group(ipp::TAG_JOB);
    writeJobAttributes(w, status, uri, {"job-id", "job-uri", "job-state", "job-state-reasons", "job-state-message"});
    return ipp::STATUS_OK;
}

void IppServer::writeJobAttributes(ipp::Writer &w, const JobStatus &job, const std::string &printerUri,
                                   const std::vector<std::string> &requested) const
{
    const bool all = requested.empty() || std::find(requested.begin(), requested.end(), "all") != requested.end() ||
                     std::find(requested.begin(), requested.end(), "job-description") != requested.end();
    auto want = [&](const char *name) {
        return all || std::find(requested.begin(), requested.end(), name) != requested.end();
    };

    if (want("job-id"))
        w.integer("job-id", static_cast<int32_t>(job.id));
    if (want("job-uri"))
        w.uri("job-uri", printerUri + "/" + std::to_string(job.id));
    if (want("job-printer-uri"))
        w.uri("job-printer-uri", printerUri);
    if (want("job-name"))
        w.name("job-name", job.name);
    if (want("job-originating-user-name"))
        w.name("job-originating-user-name", job.user.empty() ? "anonymous" : job.user);
    if (want("job-state"))
        w.enumValue("job-state", ippJobState(job.state));
    if (want("job-state-reasons"))
        w.keyword("job-state-reasons", ippJobStateReason(job.state));
    if (want("job-state-message"))
        w.text("job-state-message", job.message.empty() ? jobStateName(job.state) : job.message);
    if (want("job-printer-up-time"))
        w.integer("job-printer-up-time", ippTime(PrinterRegistry::nowSeconds()));
    if (want("time-at-creation"))
        w.integer("time-at-creation", ippTime(job.createdAt));
    if (want("time-at-processing"))
    {
        if (job.startedAt)
            w.integer("time-at-processing", ippTime(job.startedAt));
        else
            w.noValue("time-at-processing");
    }
    if (want("time-at-completed"))
    {
        if (job.completedAt)
            w.integer("time-at-completed", ippTime(job.completedAt));
        else
            w.noValue("time-at-completed");
    }
    if (want("job-impressions-completed"))
        w.integer("job-impressions-completed", job.state == JobState::Completed ? 1 : 0);
    if (want("job-k-octets"))
        w.integer("job-k-octets", static_cast<int32_t>((job.bytes + 1023) / 1024));
}

uint16_t IppServer::opGetJobAttributes(const ipp::Request &req, const PrinterRecord &printer,
                                       const std::string &uri, ipp::Writer &w)
{
    JobStatus job;
    const uint32_t id = requestedJobId(req);
    if (!id || !jobs_->getStatus(id, job) || job.printerMac != printer.mac)
    {
        statusMessage_ = "job not found";
        return ipp::CLIENT_ERROR_NOT_FOUND;
    }
    w.group(ipp::TAG_JOB);
    writeJobAttributes(w, job, uri, req.getStrings("requested-attributes"));
    return ipp::STATUS_OK;
}

uint16_t IppServer::opGetJobs(const ipp::Request &req, const PrinterRecord &printer, const std::string &uri,
                              ipp::Writer &w)
{
    const std::string which = req.getString("which-jobs", "not-completed");
    if (which != "completed" && which != "not-completed" && which != "all")
    {
        w.group(ipp::TAG_UNSUPPORTED_GROUP);
        w.keyword("which-jobs", which);
        statusMessage_ = "which-jobs value not supported";
        return ipp::CLIENT_ERROR_ATTRIBUTES_OR_VALUES_NOT_SUPPORTED;
    }
    std::vector<std::string> requested = req.getStrings("requested-attributes");
    if (requested.empty())
        requested = {"job-id", "job-uri"};
    const int32_t limit = req.getInt("limit", 0);

    int32_t count = 0;
    auto jobs = jobs_->list();
    // Most recent first, as clients usually expect.
    for (auto it = jobs.rbegin(); it != jobs.rend(); ++it)
    {
        if (it->printerMac != printer.mac)
            continue;
        const bool finished = it->finished();
        if ((which == "completed" && !finished) || (which == "not-completed" && finished))
            continue;
        if (limit > 0 && count >= limit)
            break;
        w.group(ipp::TAG_JOB);
        writeJobAttributes(w, *it, uri, requested);
        ++count;
    }
    return ipp::STATUS_OK;
}

uint16_t IppServer::opCancelJob(const ipp::Request &req, const PrinterRecord &printer)
{
    JobStatus job;
    const uint32_t id = requestedJobId(req);
    if (!id || !jobs_->getStatus(id, job) || job.printerMac != printer.mac)
    {
        statusMessage_ = "job not found";
        return ipp::CLIENT_ERROR_NOT_FOUND;
    }
    if (job.finished() || !jobs_->cancel(id))
    {
        statusMessage_ = "job already finished";
        return ipp::CLIENT_ERROR_NOT_POSSIBLE;
    }
    return ipp::STATUS_OK;
}

uint16_t IppServer::opIdentifyPrinter(const PrinterRecord &printer)
{
    std::string error;
    if (!jobs_->identifyPrinter(printer.mac, &error))
    {
        statusMessage_ = error;
        return ipp::SERVER_ERROR_BUSY;
    }
    return ipp::STATUS_OK;
}
