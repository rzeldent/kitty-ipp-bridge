/**
 * @file WebUI.h
 * @brief Web dashboard (Bootstrap 5 single page app) and JSON REST API.
 *
 * Served by ESPAsyncWebServer on port 80:
 *
 *   GET    /                               Dashboard (single page application)
 *   GET    /api/printers                   List printers
 *   GET    /api/printers/{mac}             Single printer
 *   POST   /api/printers/{mac}/alias       {"alias":"Desk Label Printer"}
 *   POST   /api/printers/{mac}/identify    Queue identification print
 *   POST   /api/printers/{mac}/print-test  Queue test page
 *   POST   /api/printers/{mac}/feed        {"mm":10} feed paper
 *   DELETE /api/printers/{mac}             Forget printer
 *   GET    /api/jobs                       Recent jobs
 *   POST   /api/jobs/{id}/cancel           Cancel job
 *   GET    /api/stats                      Statistics
 *   GET    /api/config  POST /api/config   Bridge + protocol settings
 *   POST   /api/network                    Wi-Fi / host name (reboot required)
 *   POST   /api/scan                       Trigger a BLE scan now
 *   GET    /api/logs                       Recent log lines
 *   POST   /api/reboot                     Restart the bridge
 *   GET    /api/events                     Server-Sent Events (printer/job/log/stats)
 *
 * {mac} accepts "aa:bb:cc:dd:ee:ff", "aa-bb-..." or the printer id "aabbccddeeff".
 * See docs/REST_API.md for request/response examples.
 *
 * Request handlers run in the AsyncTCP task and only touch thread-safe
 * components. Push notifications are funnelled through a FreeRTOS queue and
 * emitted by the dedicated "web_ui" task, so producers (registry, job worker,
 * logger) never block on network I/O.
 */
#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

class AsyncWebServer;
class AsyncEventSource;
class AsyncWebServerRequest;
class PrinterRegistry;
class JobQueue;
class ConfigManager;
class BleDiscovery;
class IppServer;
class RingBufferLogSink;

class WebUI
{
public:
    /** Components used by the API (all must outlive the WebUI). */
    struct Context
    {
        PrinterRegistry *registry = nullptr;
        JobQueue *jobs = nullptr;
        ConfigManager *config = nullptr;
        BleDiscovery *discovery = nullptr;
        IppServer *ipp = nullptr;
        RingBufferLogSink *logs = nullptr;
        /** Invoked after settings changed through the API (apply at run time). */
        std::function<void()> onConfigChanged;
    };

    bool begin(const Context &context, uint16_t port = 80);
    bool startTask(UBaseType_t priority = 1, BaseType_t core = 0, uint32_t stackSize = 6144);

private:
    /** Notification forwarded to SSE clients by the Web UI task. */
    struct UiEvent
    {
        enum Kind : uint8_t
        {
            Printer,
            Job,
            Log,
        } kind;
        char mac[18];
        uint32_t jobId;
        char text[136];
    };

    static void taskEntry(void *arg);
    void run();
    void post(const UiEvent &event);
    void registerRoutes();

    void handlePrinters(AsyncWebServerRequest *request);
    void handlePrinterRoute(AsyncWebServerRequest *request);
    void handleJobs(AsyncWebServerRequest *request);
    void handleJobRoute(AsyncWebServerRequest *request);
    void handleStats(AsyncWebServerRequest *request);
    void handleGetConfig(AsyncWebServerRequest *request);
    void handlePostConfig(AsyncWebServerRequest *request);
    void handlePostNetwork(AsyncWebServerRequest *request);
    void handleLogs(AsyncWebServerRequest *request);

    std::string statsJson() const;

    Context ctx_;
    AsyncWebServer *server_ = nullptr;
    AsyncEventSource *events_ = nullptr;
    QueueHandle_t queue_ = nullptr;
    TaskHandle_t task_ = nullptr;
    std::atomic<uint32_t> droppedEvents_{0};
    std::atomic<uint32_t> rebootAtMs_{0}; ///< Non-zero: restart scheduled at this millis() value
};
