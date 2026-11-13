/**
 * @file WebUI.cpp
 * @brief Web dashboard and REST API implementation.
 */
#include "WebUI.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <esp_system.h>

#include <cctype>
#include <cstring>
#include <ctime>
#include <type_traits>

#include "BleDiscovery.h"
#include "ConfigManager.h"
#include "IppServer.h"
#include "JobQueue.h"
#include "Logging.h"
#include "MacUtil.h"
#include "PrinterRegistry.h"
#include "SystemStats.h"
#include "WebUiPage.h"

#ifndef CATBRIDGE_VERSION
#define CATBRIDGE_VERSION "dev"
#endif

static const char *TAG = "web";

namespace
{
    constexpr size_t kMaxBody = 2048;
    constexpr size_t kMaxAliasLen = 48;
    constexpr const char *kJson = "application/json";

    // ---------------------------------------------------------------------
    // Request body handling: collected into request->_tempObject (freed by the library).
    // ---------------------------------------------------------------------
    void collectBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
    {
        if (total > kMaxBody)
            return; // oversized bodies are ignored; the handler reports "invalid JSON"
        if (index == 0)
        {
            request->_tempObject = malloc(total + 1);
            if (!request->_tempObject)
                return;
        }
        if (!request->_tempObject || index + len > total)
            return;
        memcpy(static_cast<uint8_t *>(request->_tempObject) + index, data, len);
        if (index + len == total)
            static_cast<char *>(request->_tempObject)[total] = '\0';
    }

    /** Parse the JSON body. Empty bodies yield an empty object. */
    bool parseBody(AsyncWebServerRequest *request, JsonDocument &doc)
    {
        const char *body = static_cast<const char *>(request->_tempObject);
        if (!body || !*body)
        {
            doc.to<JsonObject>();
            return request->contentLength() == 0;
        }
        return !deserializeJson(doc, body) && doc.is<JsonObject>();
    }

    void sendJson(AsyncWebServerRequest *request, int code, const JsonDocument &doc)
    {
        AsyncResponseStream *res = request->beginResponseStream(kJson);
        res->setCode(code);
        res->addHeader("Cache-Control", "no-store");
        serializeJson(doc, *res);
        request->send(res);
    }

    void sendError(AsyncWebServerRequest *request, int code, const std::string &message)
    {
        JsonDocument doc;
        doc["ok"] = false;
        doc["error"] = message;
        sendJson(request, code, doc);
    }

    void sendOk(AsyncWebServerRequest *request, uint32_t jobId = 0)
    {
        JsonDocument doc;
        doc["ok"] = true;
        if (jobId)
            doc["jobId"] = jobId;
        sendJson(request, jobId ? 202 : 200, doc);
    }

    /** Remove control characters and limit length (aliases end up in mDNS and IPP names). */
    std::string sanitizeAlias(const std::string &in)
    {
        std::string out;
        for (char c : in)
        {
            if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F)
                continue;
            out.push_back(c);
        }
        while (!out.empty() && out.back() == ' ')
            out.pop_back();
        size_t start = out.find_first_not_of(' ');
        out = start == std::string::npos ? std::string() : out.substr(start);
        if (out.size() > kMaxAliasLen)
        {
            out.resize(kMaxAliasLen);
            // Do not cut a UTF-8 sequence in half.
            while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80)
                out.pop_back();
            if (!out.empty() && (static_cast<unsigned char>(out.back()) & 0x80))
                out.pop_back();
        }
        return out;
    }

    bool validHostname(const std::string &h)
    {
        if (h.empty() || h.size() > 32 || h.front() == '-' || h.back() == '-')
            return false;
        for (char c : h)
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-'))
                return false;
        return true;
    }

    std::string formatUptime(uint32_t seconds)
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lud %02lu:%02lu:%02lu", static_cast<unsigned long>(seconds / 86400),
                 static_cast<unsigned long>((seconds / 3600) % 24), static_cast<unsigned long>((seconds / 60) % 60),
                 static_cast<unsigned long>(seconds % 60));
        return buf;
    }

    /** Wall clock time if NTP synchronised, else 0. */
    time_t wallClock()
    {
        const time_t now = time(nullptr);
        return now > 1700000000 ? now : 0;
    }

    void printerToJson(JsonObject o, const PrinterRecord &r, IppServer *ipp, JobQueue *jobs)
    {
        const uint32_t now = PrinterRegistry::nowSeconds();
        o["mac"] = r.mac;
        o["id"] = r.id;
        o["alias"] = r.alias;
        o["name"] = r.displayName();
        o["deviceName"] = r.deviceName;
        o["rssi"] = r.rssi;
        o["online"] = r.online;
        o["lastSeen"] = r.lastSeen;
        o["lastSeenAgo"] = r.lastSeen ? static_cast<int32_t>(now - r.lastSeen) : -1;
        const time_t wall = wallClock();
        if (wall && r.lastSeen)
            o["lastSeenEpoch"] = static_cast<uint32_t>(wall - (now - r.lastSeen));
        o["firstSeen"] = r.firstSeen;
        o["profile"] = r.profileName;
        if (ipp)
            o["ippUri"] = ipp->printerUri(r.id);
        if (jobs)
            o["busy"] = jobs->printerBusy(r.mac);
    }

    void jobToJson(JsonObject o, const JobStatus &j)
    {
        o["id"] = j.id;
        o["printer"] = j.printerMac;
        o["name"] = j.name;
        o["user"] = j.user;
        o["type"] = jobTypeName(j.type);
        o["state"] = jobStateName(j.state);
        o["attempts"] = j.attempts;
        o["createdAt"] = j.createdAt;
        o["startedAt"] = j.startedAt;
        o["completedAt"] = j.completedAt;
        o["bytes"] = j.bytes;
        o["rowsTotal"] = j.rowsTotal;
        o["rowsDone"] = j.rowsDone;
        if (!j.message.empty())
            o["message"] = j.message;
    }

    /** Split "/api/printers/{key}/{action}" into key and action. */
    void splitRoute(const String &url, const char *prefix, std::string &key, std::string &action)
    {
        std::string rest = url.c_str() + strlen(prefix);
        while (!rest.empty() && rest.back() == '/')
            rest.pop_back();
        const size_t slash = rest.find('/');
        key = rest.substr(0, slash);
        action = slash == std::string::npos ? std::string() : rest.substr(slash + 1);
    }
} // namespace

// ===========================================================================
// Setup
// ===========================================================================
bool WebUI::begin(const Context &context, uint16_t port)
{
    ctx_ = context;
    queue_ = xQueueCreate(24, sizeof(UiEvent));
    server_ = new AsyncWebServer(port);
    events_ = new AsyncEventSource("/api/events");
    if (!queue_ || !server_ || !events_)
    {
        LOG_E(TAG, "Web UI allocation failed");
        return false;
    }

    // Producers -> queue (never block, never log from these callbacks).
    ctx_.registry->addListener([this](RegistryEvent event, const PrinterRecord &r) {
        UiEvent e{};
        e.kind = UiEvent::Printer;
        strlcpy(e.mac, r.mac.c_str(), sizeof(e.mac));
        strlcpy(e.text, registryEventName(event), sizeof(e.text));
        post(e);
    });
    ctx_.jobs->addListener([this](const JobStatus &j) {
        UiEvent e{};
        e.kind = UiEvent::Job;
        e.jobId = j.id;
        post(e);
    });
    if (ctx_.logs)
        ctx_.logs->setListener([this](const std::string &line) {
            UiEvent e{};
            e.kind = UiEvent::Log;
            strlcpy(e.text, line.c_str(), sizeof(e.text));
            post(e);
        });

    registerRoutes();
    server_->addHandler(events_);
    server_->begin();
    LOG_I(TAG, "Web UI listening on port %u", port);
    return true;
}

bool WebUI::startTask(UBaseType_t priority, BaseType_t core, uint32_t stackSize)
{
    return xTaskCreatePinnedToCore(taskEntry, "web_ui", stackSize, this, priority, &task_, core) == pdPASS;
}

void WebUI::post(const UiEvent &event)
{
    if (!queue_ || xQueueSend(queue_, &event, 0) != pdTRUE)
        droppedEvents_++;
}

void WebUI::registerRoutes()
{
    AsyncWebServer &s = *server_;

    s.on(AsyncURIMatcher::exact("/"), HTTP_GET, [](AsyncWebServerRequest *request) {
        SystemStats::instance().httpRequests++;
        AsyncWebServerResponse *res = request->beginResponse(
            200, "text/html", reinterpret_cast<const uint8_t *>(kIndexHtml), strlen(kIndexHtml));
        res->addHeader("Cache-Control", "no-cache");
        request->send(res);
    });

    s.on(AsyncURIMatcher::exact("/api/printers"), HTTP_GET,
         [this](AsyncWebServerRequest *request) { handlePrinters(request); });

    s.on(AsyncURIMatcher::prefix("/api/printers/"), HTTP_GET | HTTP_POST | HTTP_DELETE,
         [this](AsyncWebServerRequest *request) { handlePrinterRoute(request); }, nullptr, collectBody);

    s.on(AsyncURIMatcher::exact("/api/jobs"), HTTP_GET,
         [this](AsyncWebServerRequest *request) { handleJobs(request); });

    s.on(AsyncURIMatcher::prefix("/api/jobs/"), HTTP_GET | HTTP_POST,
         [this](AsyncWebServerRequest *request) { handleJobRoute(request); });

    s.on(AsyncURIMatcher::exact("/api/stats"), HTTP_GET,
         [this](AsyncWebServerRequest *request) { handleStats(request); });

    s.on(AsyncURIMatcher::exact("/api/config"), HTTP_GET,
         [this](AsyncWebServerRequest *request) { handleGetConfig(request); });

    s.on(AsyncURIMatcher::exact("/api/config"), HTTP_POST,
         [this](AsyncWebServerRequest *request) { handlePostConfig(request); }, nullptr, collectBody);

    s.on(AsyncURIMatcher::exact("/api/network"), HTTP_POST,
         [this](AsyncWebServerRequest *request) { handlePostNetwork(request); }, nullptr, collectBody);

    s.on(AsyncURIMatcher::exact("/api/logs"), HTTP_GET,
         [this](AsyncWebServerRequest *request) { handleLogs(request); });

    s.on(AsyncURIMatcher::exact("/api/scan"), HTTP_POST, [this](AsyncWebServerRequest *request) {
        SystemStats::instance().httpRequests++;
        if (ctx_.discovery)
            ctx_.discovery->requestScan();
        sendOk(request);
    });

    s.on(AsyncURIMatcher::exact("/api/reboot"), HTTP_POST, [this](AsyncWebServerRequest *request) {
        SystemStats::instance().httpRequests++;
        LOG_W(TAG, "Reboot requested via API");
        rebootAtMs_ = millis() + 1000;
        if (rebootAtMs_ == 0)
            rebootAtMs_ = 1;
        sendOk(request);
    });

    s.onNotFound([](AsyncWebServerRequest *request) {
        if (request->url().startsWith("/api/"))
            sendError(request, 404, "not found");
        else
            request->redirect("/"); // captive-portal friendly in AP mode
    });
}

// ===========================================================================
// Printers
// ===========================================================================
void WebUI::handlePrinters(AsyncWebServerRequest *request)
{
    SystemStats::instance().httpRequests++;
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (const PrinterRecord &r : ctx_.registry->list())
        printerToJson(arr.add<JsonObject>(), r, ctx_.ipp, ctx_.jobs);
    sendJson(request, 200, doc);
}

void WebUI::handlePrinterRoute(AsyncWebServerRequest *request)
{
    SystemStats::instance().httpRequests++;
    std::string key, action;
    splitRoute(request->url(), "/api/printers/", key, action);

    PrinterRecord printer;
    if (!ctx_.registry->find(key, printer))
    {
        sendError(request, 404, "printer not found");
        return;
    }

    const WebRequestMethod method = request->method();

    if (action.empty())
    {
        if (method == HTTP_GET)
        {
            JsonDocument doc;
            printerToJson(doc.to<JsonObject>(), printer, ctx_.ipp, ctx_.jobs);
            sendJson(request, 200, doc);
        }
        else if (method == HTTP_DELETE)
        {
            ctx_.registry->remove(printer.mac);
            sendOk(request);
        }
        else
            sendError(request, 405, "method not allowed");
        return;
    }

    if (method != HTTP_POST)
    {
        sendError(request, 405, "method not allowed");
        return;
    }

    if (action == "alias")
    {
        JsonDocument body;
        std::string alias;
        if (parseBody(request, body) && body["alias"].is<const char *>())
            alias = body["alias"].as<const char *>();
        else if (request->hasParam("alias", true))
            alias = request->getParam("alias", true)->value().c_str();
        else if (request->hasParam("alias"))
            alias = request->getParam("alias")->value().c_str();
        else
        {
            sendError(request, 400, "expected JSON body {\"alias\":\"...\"}");
            return;
        }
        alias = sanitizeAlias(alias);
        if (!ctx_.registry->setAlias(printer.mac, alias))
        {
            sendError(request, 404, "printer not found");
            return;
        }
        ctx_.registry->get(printer.mac, printer);
        JsonDocument doc;
        doc["ok"] = true;
        printerToJson(doc["printer"].to<JsonObject>(), printer, ctx_.ipp, ctx_.jobs);
        sendJson(request, 200, doc);
        return;
    }

    std::string error;
    uint32_t jobId = 0;
    if (action == "identify")
        jobId = ctx_.jobs->identifyPrinter(printer.mac, &error);
    else if (action == "print-test")
        jobId = ctx_.jobs->printTestPage(printer.mac, &error);
    else if (action == "feed")
    {
        JsonDocument body;
        int mm = 10;
        if (parseBody(request, body) && body["mm"].is<int>())
            mm = body["mm"].as<int>();
        if (mm < 1 || mm > 100)
        {
            sendError(request, 400, "mm must be between 1 and 100");
            return;
        }
        PrintJob job;
        job.printerMac = printer.mac;
        job.type = JobType::FeedPaper;
        job.feedLines = static_cast<uint16_t>(mm * 8);
        job.name = "feed";
        jobId = ctx_.jobs->submit(std::move(job), &error);
    }
    else
    {
        sendError(request, 404, "unknown action");
        return;
    }

    if (!jobId)
        sendError(request, 503, error.empty() ? "unable to queue job" : error);
    else
        sendOk(request, jobId);
}

// ===========================================================================
// Jobs
// ===========================================================================
void WebUI::handleJobs(AsyncWebServerRequest *request)
{
    SystemStats::instance().httpRequests++;
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    const auto jobs = ctx_.jobs->list();
    for (auto it = jobs.rbegin(); it != jobs.rend(); ++it)
        jobToJson(arr.add<JsonObject>(), *it);
    sendJson(request, 200, doc);
}

void WebUI::handleJobRoute(AsyncWebServerRequest *request)
{
    SystemStats::instance().httpRequests++;
    std::string key, action;
    splitRoute(request->url(), "/api/jobs/", key, action);
    const uint32_t id = static_cast<uint32_t>(strtoul(key.c_str(), nullptr, 10));
    JobStatus status;
    if (!id || !ctx_.jobs->getStatus(id, status))
    {
        sendError(request, 404, "job not found");
        return;
    }
    if (action.empty() && request->method() == HTTP_GET)
    {
        JsonDocument doc;
        jobToJson(doc.to<JsonObject>(), status);
        sendJson(request, 200, doc);
    }
    else if (action == "cancel" && request->method() == HTTP_POST)
    {
        if (ctx_.jobs->cancel(id))
            sendOk(request);
        else
            sendError(request, 409, "job already finished");
    }
    else
        sendError(request, 404, "unknown action");
}

// ===========================================================================
// Statistics
// ===========================================================================
std::string WebUI::statsJson() const
{
    SystemStats &st = SystemStats::instance();
    const uint32_t uptime = PrinterRegistry::nowSeconds();

    JsonDocument doc;
    // Contract fields (string values as specified).
    doc["uptime"] = formatUptime(uptime);
    doc["heapFree"] = std::to_string(ESP.getFreeHeap());
    doc["jobsPrinted"] = std::to_string(st.jobsPrinted.load());
    doc["printersDiscovered"] = std::to_string(ctx_.registry->size());
    doc["jobsFailed"] = std::to_string(st.jobsFailed.load());

    // Extended numeric details.
    JsonObject d = doc["details"].to<JsonObject>();
    d["firmware"] = CATBRIDGE_VERSION;
    d["uptimeSeconds"] = uptime;
    d["heapFree"] = ESP.getFreeHeap();
    d["heapMinFree"] = ESP.getMinFreeHeap();
    d["heapMaxAlloc"] = ESP.getMaxAllocHeap();
    d["psramSize"] = ESP.getPsramSize();
    d["psramFree"] = ESP.getFreePsram();
    d["printersOnline"] = ctx_.registry->onlineCount();
    d["ippPrintersAdvertised"] = ctx_.ipp ? ctx_.ipp->advertisedPrinters() : 0;
    d["jobsQueued"] = st.jobsQueued.load();
    d["jobsRejected"] = st.jobsRejected.load();
    d["jobRetries"] = st.jobRetries.load();
    d["jobsPending"] = ctx_.jobs->pendingCount();
    d["jobActive"] = ctx_.jobs->activeJob();
    d["queueCapacity"] = ctx_.jobs->capacity();
    d["queuedBytes"] = ctx_.jobs->queuedBytes();
    d["bleScans"] = st.bleScans.load();
    d["bleProbes"] = st.bleProbes.load();
    d["bleConnectFailures"] = st.bleConnectFailures.load();
    d["ippRequests"] = st.ippRequests.load();
    d["ippErrors"] = st.ippErrors.load();
    d["httpRequests"] = st.httpRequests.load();
    d["mdnsRestarts"] = st.mdnsRestarts.load();
    d["wifiReconnects"] = st.wifiReconnects.load();
    d["wifiRssi"] = WiFi.isConnected() ? WiFi.RSSI() : 0;
    d["ip"] = WiFi.isConnected() ? WiFi.localIP().toString().c_str() : WiFi.softAPIP().toString().c_str();
    d["resetReason"] = static_cast<int>(esp_reset_reason());
    d["sseClients"] = events_ ? events_->count() : 0;
    d["sseDropped"] = droppedEvents_.load();
    if (ctx_.discovery)
    {
        d["scanning"] = ctx_.discovery->scanning();
        d["lastScanDevices"] = ctx_.discovery->lastScanDevices();
        d["lastScanMatches"] = ctx_.discovery->lastScanMatches();
    }

    std::string out;
    serializeJson(doc, out);
    return out;
}

void WebUI::handleStats(AsyncWebServerRequest *request)
{
    SystemStats::instance().httpRequests++;
    AsyncWebServerResponse *res = request->beginResponse(200, kJson, statsJson().c_str());
    res->addHeader("Cache-Control", "no-store");
    request->send(res);
}

void WebUI::handleLogs(AsyncWebServerRequest *request)
{
    SystemStats::instance().httpRequests++;
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    if (ctx_.logs)
        for (const auto &line : ctx_.logs->snapshot())
            arr.add(line);
    sendJson(request, 200, doc);
}

// ===========================================================================
// Configuration
// ===========================================================================
void WebUI::handleGetConfig(AsyncWebServerRequest *request)
{
    SystemStats::instance().httpRequests++;
    const BridgeConfig b = ctx_.config->bridge();
    const ProtocolConfig p = ctx_.config->protocol();
    const NetworkConfig n = ctx_.config->network();

    JsonDocument doc;
    JsonObject jb = doc["bridge"].to<JsonObject>();
    jb["scanIntervalSec"] = b.scanIntervalSec;
    jb["scanDurationSec"] = b.scanDurationSec;
    jb["offlineTimeoutSec"] = b.offlineTimeoutSec;
    jb["probeUnknownDevices"] = b.probeUnknownDevices;
    jb["queueSize"] = b.queueSize;
    jb["maxRetries"] = b.maxRetries;
    jb["logLevel"] = b.logLevel;
    jb["trimBlankRows"] = b.trimBlankRows;

    JsonObject jp = doc["protocol"].to<JsonObject>();
    jp["energy"] = p.energy;
    jp["speed"] = p.speed;
    jp["feedAfterJobLines"] = p.feedAfterJobLines;
    jp["chunkSize"] = p.chunkSize;
    jp["packetDelayMs"] = p.packetDelayMs;

    JsonObject jn = doc["network"].to<JsonObject>();
    jn["ssid"] = n.ssid;
    jn["passwordSet"] = !n.password.empty();
    jn["hostname"] = n.hostname;
    jn["apMode"] = !WiFi.isConnected() && (WiFi.getMode() & WIFI_MODE_AP);
    sendJson(request, 200, doc);
}

void WebUI::handlePostConfig(AsyncWebServerRequest *request)
{
    SystemStats::instance().httpRequests++;
    JsonDocument body;
    if (!parseBody(request, body))
    {
        sendError(request, 400, "invalid JSON");
        return;
    }

    BridgeConfig b = ctx_.config->bridge();
    ProtocolConfig p = ctx_.config->protocol();
    std::string error;

    // Range checked assignment helper.
    auto take = [&](JsonVariantConst v, const char *name, long lo, long hi, auto &field) {
        if (v.isNull() || !error.empty())
            return;
        if (!v.is<long>() || v.as<long>() < lo || v.as<long>() > hi)
        {
            error = std::string(name) + " must be between " + std::to_string(lo) + " and " + std::to_string(hi);
            return;
        }
        field = static_cast<std::remove_reference_t<decltype(field)>>(v.as<long>());
    };
    auto takeBool = [&](JsonVariantConst v, const char *name, bool &field) {
        if (v.isNull() || !error.empty())
            return;
        if (!v.is<bool>())
        {
            error = std::string(name) + " must be a boolean";
            return;
        }
        field = v.as<bool>();
    };

    JsonVariantConst jb = body["bridge"];
    take(jb["scanIntervalSec"], "scanIntervalSec", 10, 3600, b.scanIntervalSec);
    take(jb["scanDurationSec"], "scanDurationSec", 1, 30, b.scanDurationSec);
    take(jb["offlineTimeoutSec"], "offlineTimeoutSec", 30, 86400, b.offlineTimeoutSec);
    takeBool(jb["probeUnknownDevices"], "probeUnknownDevices", b.probeUnknownDevices);
    take(jb["queueSize"], "queueSize", 1, 32, b.queueSize);
    take(jb["maxRetries"], "maxRetries", 1, 10, b.maxRetries);
    take(jb["logLevel"], "logLevel", 0, 3, b.logLevel);
    takeBool(jb["trimBlankRows"], "trimBlankRows", b.trimBlankRows);

    JsonVariantConst jp = body["protocol"];
    take(jp["energy"], "energy", 0, 0xFFFF, p.energy);
    take(jp["speed"], "speed", 1, 255, p.speed);
    take(jp["feedAfterJobLines"], "feedAfterJobLines", 0, 800, p.feedAfterJobLines);
    take(jp["chunkSize"], "chunkSize", 20, 512, p.chunkSize);
    take(jp["packetDelayMs"], "packetDelayMs", 0, 100, p.packetDelayMs);

    if (error.empty() && b.scanDurationSec >= b.scanIntervalSec)
        error = "scanDurationSec must be shorter than scanIntervalSec";
    if (!error.empty())
    {
        sendError(request, 400, error);
        return;
    }

    if (!ctx_.config->setBridge(b) || !ctx_.config->setProtocol(p))
    {
        sendError(request, 500, "failed to persist configuration");
        return;
    }
    if (ctx_.onConfigChanged)
        ctx_.onConfigChanged();
    LOG_I(TAG, "Configuration updated via API");
    handleGetConfig(request);
}

void WebUI::handlePostNetwork(AsyncWebServerRequest *request)
{
    SystemStats::instance().httpRequests++;
    JsonDocument body;
    if (!parseBody(request, body))
    {
        sendError(request, 400, "invalid JSON");
        return;
    }
    NetworkConfig n = ctx_.config->network();
    if (body["ssid"].is<const char *>())
    {
        const std::string ssid = body["ssid"].as<const char *>();
        if (ssid.size() > 32)
        {
            sendError(request, 400, "ssid too long");
            return;
        }
        n.ssid = ssid;
    }
    if (body["password"].is<const char *>())
    {
        const std::string pw = body["password"].as<const char *>();
        if (!pw.empty() && (pw.size() < 8 || pw.size() > 63))
        {
            sendError(request, 400, "password must be 8-63 characters");
            return;
        }
        n.password = pw;
    }
    if (body["hostname"].is<const char *>())
    {
        const std::string host = body["hostname"].as<const char *>();
        if (!validHostname(host))
        {
            sendError(request, 400, "hostname may contain letters, digits and '-' (max 32)");
            return;
        }
        n.hostname = host;
    }
    if (body["apPassword"].is<const char *>())
    {
        const std::string pw = body["apPassword"].as<const char *>();
        if (pw.size() < 8 || pw.size() > 63)
        {
            sendError(request, 400, "apPassword must be 8-63 characters");
            return;
        }
        n.apPassword = pw;
    }
    if (!ctx_.config->setNetwork(n))
    {
        sendError(request, 500, "failed to persist network settings");
        return;
    }
    LOG_I(TAG, "Network settings updated (reboot required)");
    JsonDocument doc;
    doc["ok"] = true;
    doc["rebootRequired"] = true;
    sendJson(request, 200, doc);
}

// ===========================================================================
// Web UI task: forwards queued notifications to SSE clients
// ===========================================================================
void WebUI::taskEntry(void *arg)
{
    static_cast<WebUI *>(arg)->run();
}

void WebUI::run()
{
    uint32_t lastStats = 0;
    for (;;)
    {
        UiEvent e;
        if (xQueueReceive(queue_, &e, pdMS_TO_TICKS(500)) == pdTRUE && events_->count() > 0)
        {
            JsonDocument doc;
            std::string payload;
            const char *name = nullptr;
            switch (e.kind)
            {
            case UiEvent::Printer:
            {
                PrinterRecord r;
                if (strcmp(e.text, registryEventName(RegistryEvent::Removed)) == 0 || !ctx_.registry->get(e.mac, r))
                {
                    doc["mac"] = e.mac;
                    doc["removed"] = true;
                }
                else
                    printerToJson(doc.to<JsonObject>(), r, ctx_.ipp, ctx_.jobs);
                doc["event"] = e.text;
                name = "printer";
                break;
            }
            case UiEvent::Job:
            {
                JobStatus j;
                if (!ctx_.jobs->getStatus(e.jobId, j))
                    break;
                jobToJson(doc.to<JsonObject>(), j);
                name = "job";
                break;
            }
            case UiEvent::Log:
                payload = e.text;
                name = "log";
                break;
            }
            if (name)
            {
                if (payload.empty())
                    serializeJson(doc, payload);
                events_->send(payload.c_str(), name, millis());
            }
        }

        if (millis() - lastStats > 5000)
        {
            lastStats = millis();
            if (events_->count() > 0)
                events_->send(statsJson().c_str(), "stats", millis());
        }

        const uint32_t rebootAt = rebootAtMs_;
        if (rebootAt && static_cast<int32_t>(millis() - rebootAt) >= 0)
        {
            Serial.println("[web] restarting");
            Serial.flush();
            ESP.restart();
        }
    }
}
