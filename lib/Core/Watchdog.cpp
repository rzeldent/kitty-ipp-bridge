#include "Watchdog.h"

#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace watchdog
{
    void begin(uint32_t timeoutSeconds)
    {
        esp_task_wdt_config_t cfg = {};
        cfg.timeout_ms = timeoutSeconds * 1000;
        cfg.idle_core_mask = 0; // Idle tasks are not monitored: BLE/Wi-Fi bursts may starve them briefly
        cfg.trigger_panic = true;
        // Arduino-ESP32 already initialises the TWDT; reconfigure it, or
        // initialise it if it was disabled in the SDK configuration.
        if (esp_task_wdt_reconfigure(&cfg) != ESP_OK)
            esp_task_wdt_init(&cfg);
    }

    void subscribe()
    {
        if (esp_task_wdt_status(nullptr) != ESP_OK)
            esp_task_wdt_add(nullptr);
    }

    void unsubscribe()
    {
        if (esp_task_wdt_status(nullptr) == ESP_OK)
            esp_task_wdt_delete(nullptr);
    }

    void feed()
    {
        if (esp_task_wdt_status(nullptr) == ESP_OK)
            esp_task_wdt_reset();
    }
} // namespace watchdog
