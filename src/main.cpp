/**
 * @file main.cpp
 * @brief CatPrinter IPP Bridge - firmware entry point.
 *
 * Wires the subsystems together and starts the FreeRTOS tasks:
 *
 * | Task          | Owner            | Core | Prio | Purpose                                         |
 * |---------------|------------------|------|------|-------------------------------------------------|
 * | ble_scan      | BleDiscovery     | 1    | 2    | Periodic active BLE scan, UUID matching          |
 * | registry      | main.cpp         | 1    | 2    | Applies discovery events, offline time-outs      |
 * | job_worker    | JobQueue         | 1    | 3    | Connects to printers and prints queued jobs      |
 * | ipp           | IppServer        | 0    | 2    | IPP/HTTP server on port 631, mDNS maintenance    |
 * | web_ui        | WebUI            | 0    | 1    | Pushes live updates to dashboard clients (SSE)   |
 * | monitor       | main.cpp         | 0    | 1    | Heap monitoring, Wi-Fi supervision               |
 * | async_tcp     | AsyncTCP library | -    | -    | Serves the REST API / dashboard                  |
 *
 * See docs/ARCHITECTURE.md for diagrams.
 */
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <esp_system.h>

#include "BleDiscovery.h"
#include "ConfigManager.h"
#include "IppServer.h"
#include "JobQueue.h"
#include "Logging.h"
#include "MacUtil.h"
#include "PrinterProfile.h"
#include "PrinterRegistry.h"
#include "SystemStats.h"
#include "Watchdog.h"
#include "WebUI.h"

#ifndef CATBRIDGE_VERSION
#define CATBRIDGE_VERSION "dev"
#endif
#ifndef CATBRIDGE_HOSTNAME
#define CATBRIDGE_HOSTNAME "catbridge"
#endif

static const char *TAG = "main";

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------
namespace
{
    constexpr uint32_t kWatchdogTimeoutSec = 60;     ///< TWDT timeout for subscribed tasks
    constexpr uint32_t kWifiConnectTimeoutMs = 20000; ///< Before falling back to the setup AP
    constexpr uint32_t kMonitorPeriodMs = 5000;
    constexpr uint32_t kHeapLogPeriodMs = 300000;     ///< Periodic heap report
    constexpr uint32_t kLowHeapBytes = 16 * 1024;     ///< Below this the bridge is considered starving
    constexpr uint8_t kLowHeapRestartChecks = 6;      ///< Consecutive low readings before a restart (30 s)
    constexpr size_t kJobBudgetNoPsram = 160 * 1024;  ///< Bitmap bytes held in internal RAM
    constexpr size_t kJobBudgetPsram = 2 * 1024 * 1024;
} // namespace

// ---------------------------------------------------------------------------
// Subsystems (static lifetime)
// ---------------------------------------------------------------------------
static SerialLogSink gSerialLog;
static RingBufferLogSink gRingLog(80);
static ConfigManager gConfig;
static PrinterRegistry gRegistry;
static BleDiscovery gDiscovery;
static JobQueue gJobs;
static IppServer gIpp;
static WebUI gWeb;

static std::string gHostname = CATBRIDGE_HOSTNAME;

// ---------------------------------------------------------------------------
// Settings mapping
// ---------------------------------------------------------------------------
static BleDiscovery::Settings discoverySettings(const BridgeConfig &b)
{
    BleDiscovery::Settings s;
    s.scanIntervalMs = static_cast<uint32_t>(b.scanIntervalSec) * 1000;
    s.scanDurationMs = static_cast<uint32_t>(b.scanDurationSec) * 1000;
    s.probeUnknownDevices = b.probeUnknownDevices;
    return s;
}

static JobQueue::Settings jobSettings(const BridgeConfig &b)
{
    JobQueue::Settings s;
    s.queueSize = b.queueSize;
    s.maxRetries = b.maxRetries;
    s.maxQueuedBytes = psramFound() ? kJobBudgetPsram : kJobBudgetNoPsram;
    return s;
}

/** Apply settings changed through the REST API without a reboot. */
static void applyConfig()
{
    const BridgeConfig b = gConfig.bridge();
    Logger::instance().setLevel(static_cast<LogLevel>(b.logLevel));
    gDiscovery.applySettings(discoverySettings(b));
    gRegistry.setOfflineTimeout(b.offlineTimeoutSec);
    gJobs.applySettings(jobSettings(b));
}

// ---------------------------------------------------------------------------
// Wi-Fi
// ---------------------------------------------------------------------------
static void onWifiEvent(arduino_event_id_t event, arduino_event_info_t info)
{
    switch (event)
    {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        LOG_I("wifi", "Connected, IP %s", WiFi.localIP().toString().c_str());
        gIpp.requestMdnsRestart(); // re-announce on the new interface address
        break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
        SystemStats::instance().wifiReconnects++;
        LOG_W("wifi", "Disconnected (reason %u), reconnecting", info.wifi_sta_disconnected.reason);
        break;
    case ARDUINO_EVENT_WIFI_AP_START:
        gIpp.requestMdnsRestart();
        break;
    default:
        break;
    }
}

static void startAccessPoint(const NetworkConfig &net)
{
    const std::string mac = WiFi.macAddress().c_str();
    const std::string ssid = "CatBridge-" + macutil::shortSuffix(mac);
    WiFi.mode(net.ssid.empty() ? WIFI_AP : WIFI_AP_STA);
    WiFi.softAP(ssid.c_str(), net.apPassword.c_str());
    LOG_W("wifi", "Setup access point '%s' active, open http://%s/", ssid.c_str(),
          WiFi.softAPIP().toString().c_str());
}

static void startWifi()
{
    const NetworkConfig net = gConfig.network();
    gHostname = net.hostname.empty() ? CATBRIDGE_HOSTNAME : net.hostname;

    WiFi.onEvent(onWifiEvent);
    WiFi.persistent(false);
    WiFi.setHostname(gHostname.c_str());
    WiFi.setAutoReconnect(true);

    if (net.ssid.empty())
    {
        LOG_W("wifi", "No Wi-Fi configured");
        startAccessPoint(net);
        return;
    }

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(true); // modem sleep is required for Wi-Fi/BLE coexistence
    WiFi.begin(net.ssid.c_str(), net.password.c_str());
    LOG_I("wifi", "Connecting to '%s'", net.ssid.c_str());

    const uint32_t start = millis();
    while (!WiFi.isConnected() && millis() - start < kWifiConnectTimeoutMs)
        delay(250);

    if (!WiFi.isConnected())
    {
        LOG_W("wifi", "Connection failed, keeping retries in the background");
        startAccessPoint(net);
    }
    else
    {
        configTime(0, 0, "pool.ntp.org", "time.nist.gov"); // optional wall clock for the UI
    }
}

// ---------------------------------------------------------------------------
// Printer Registry task: discovery events -> registry, offline time-outs
// ---------------------------------------------------------------------------
static void registryTask(void *)
{
    watchdog::subscribe();
    const auto &profiles = PrinterProfiles::all();
    uint32_t lastCheck = 0;
    for (;;)
    {
        watchdog::feed();
        DiscoveryEvent ev;
        while (gDiscovery.receive(ev, pdMS_TO_TICKS(1000)))
        {
            const std::string profile =
                ev.profileIndex < profiles.size() ? profiles[ev.profileIndex].profileName : std::string();
            gRegistry.upsert(ev.mac, ev.deviceName, ev.rssi, ev.addrType, profile);
            watchdog::feed();
        }
        if (millis() - lastCheck >= 1000)
        {
            lastCheck = millis();
            gRegistry.checkTimeouts();
        }
    }
}

// ---------------------------------------------------------------------------
// Monitor task: heap monitoring, Wi-Fi supervision
// ---------------------------------------------------------------------------
static void monitorTask(void *)
{
    watchdog::subscribe();
    SystemStats &stats = SystemStats::instance();
    uint32_t lastHeapLog = 0;
    uint32_t staConnectedSince = 0;
    uint8_t lowHeapCount = 0;

    for (;;)
    {
        watchdog::feed();
        const uint32_t freeHeap = ESP.getFreeHeap();
        if (freeHeap < stats.minFreeHeap)
            stats.minFreeHeap = freeHeap;

        if (millis() - lastHeapLog >= kHeapLogPeriodMs)
        {
            lastHeapLog = millis();
            LOG_I(TAG, "Heap free=%u min=%u maxBlock=%u psram=%u | printers=%u online=%u | jobs ok=%u fail=%u",
                  static_cast<unsigned>(freeHeap), static_cast<unsigned>(ESP.getMinFreeHeap()),
                  static_cast<unsigned>(ESP.getMaxAllocHeap()), static_cast<unsigned>(ESP.getFreePsram()),
                  static_cast<unsigned>(gRegistry.size()), static_cast<unsigned>(gRegistry.onlineCount()),
                  static_cast<unsigned>(stats.jobsPrinted.load()), static_cast<unsigned>(stats.jobsFailed.load()));
        }

        // Starvation guard: a fragmented / leaking heap is recovered by a clean restart,
        // but never while a job is being printed.
        if (freeHeap < kLowHeapBytes)
        {
            if (++lowHeapCount == 1)
                LOG_W(TAG, "Low heap: %u bytes", static_cast<unsigned>(freeHeap));
            if (lowHeapCount >= kLowHeapRestartChecks && gJobs.activeJob() == 0)
            {
                LOG_E(TAG, "Heap exhausted, restarting");
                delay(200);
                ESP.restart();
            }
        }
        else
            lowHeapCount = 0;

        // Turn the setup AP off once the station connection is stable again.
        if (WiFi.isConnected())
        {
            if (!staConnectedSince)
                staConnectedSince = millis();
            if ((WiFi.getMode() & WIFI_MODE_AP) && millis() - staConnectedSince > 60000)
            {
                LOG_I("wifi", "Station connected, disabling setup access point");
                WiFi.softAPdisconnect(true);
                WiFi.mode(WIFI_STA);
                gIpp.requestMdnsRestart();
            }
        }
        else
            staConnectedSince = 0;

        vTaskDelay(pdMS_TO_TICKS(kMonitorPeriodMs));
    }
}

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------
void setup()
{
    Serial.begin(115200);
    delay(200);

    Logger &log = Logger::instance();
    log.addSink(&gSerialLog);
    log.addSink(&gRingLog);

    LOG_I(TAG, "CatPrinter IPP Bridge %s starting (reset reason %d, heap %u, psram %u)", CATBRIDGE_VERSION,
          static_cast<int>(esp_reset_reason()), static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getPsramSize()));

    watchdog::begin(kWatchdogTimeoutSec);

    if (!gConfig.begin())
        LOG_E(TAG, "Configuration storage unavailable, using defaults");
    const BridgeConfig bridge = gConfig.bridge();
    log.setLevel(static_cast<LogLevel>(bridge.logLevel));

    // ---- Network -----------------------------------------------------------
    startWifi();

    // ---- BLE ---------------------------------------------------------------
    NimBLEDevice::init("CatBridge");
    NimBLEDevice::setPower(9); // dBm, best range for sleeping printers
    NimBLEDevice::setMTU(247);

    gRegistry.begin(&gConfig, bridge.offlineTimeoutSec);

    if (!gDiscovery.begin(discoverySettings(bridge)) || !gDiscovery.startTask())
        LOG_E(TAG, "BLE discovery failed to start");

    if (xTaskCreatePinnedToCore(registryTask, "registry", 4096, nullptr, 2, nullptr, 1) != pdPASS)
        LOG_E(TAG, "Registry task failed to start");

    // ---- Print queue ---------------------------------------------------------
    if (!gJobs.begin(&gRegistry, &gConfig, jobSettings(bridge)) || !gJobs.startTask())
        LOG_E(TAG, "Job queue failed to start");

    // ---- IPP -----------------------------------------------------------------
    IppServer::Settings ipp;
    ipp.hostname = gHostname;
    ipp.maxJobBytes = psramFound() ? kJobBudgetPsram : kJobBudgetNoPsram;
    if (!gIpp.begin(&gRegistry, &gJobs, &gConfig, ipp) || !gIpp.startTask())
        LOG_E(TAG, "IPP server failed to start");

    // ---- Web UI / REST -------------------------------------------------------
    WebUI::Context ctx;
    ctx.registry = &gRegistry;
    ctx.jobs = &gJobs;
    ctx.config = &gConfig;
    ctx.discovery = &gDiscovery;
    ctx.ipp = &gIpp;
    ctx.logs = &gRingLog;
    ctx.onConfigChanged = applyConfig;
    if (!gWeb.begin(ctx, ipp.httpPort) || !gWeb.startTask())
        LOG_E(TAG, "Web UI failed to start");

    if (xTaskCreatePinnedToCore(monitorTask, "monitor", 4096, nullptr, 1, nullptr, 0) != pdPASS)
        LOG_E(TAG, "Monitor task failed to start");

    LOG_I(TAG, "Bridge ready: http://%s.local/  ipp://%s.local:%u/ipp/print", gHostname.c_str(), gHostname.c_str(),
          ipp.port);
}

void loop()
{
    // All work happens in dedicated tasks; the Arduino loop task just idles.
    vTaskDelay(pdMS_TO_TICKS(1000));
}
