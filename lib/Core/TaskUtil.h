/**
 * @file TaskUtil.h
 * @brief Portable FreeRTOS task creation for single- and dual-core ESP32 variants.
 *
 * The bridge pins radio/printing work to core 1 and network work to core 0 on
 * dual-core chips (ESP32-S3). Single-core chips (ESP32-C3) only have core 0;
 * pinning to core 1 there would fail, so the core is clamped.
 */
#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace taskutil
{
    /** Map a preferred core to one that exists on this chip. */
    inline BaseType_t core(BaseType_t preferred)
    {
        if (preferred == tskNO_AFFINITY)
            return preferred;
        return preferred < static_cast<BaseType_t>(portNUM_PROCESSORS) ? preferred : 0;
    }

    /** xTaskCreatePinnedToCore() with the core clamped to the available cores. */
    inline bool create(TaskFunction_t fn, const char *name, uint32_t stackSize, void *arg, UBaseType_t priority,
                       TaskHandle_t *handle, BaseType_t preferredCore)
    {
        return xTaskCreatePinnedToCore(fn, name, stackSize, arg, priority, handle, core(preferredCore)) == pdPASS;
    }
} // namespace taskutil
