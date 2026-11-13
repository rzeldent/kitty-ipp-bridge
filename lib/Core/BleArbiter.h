/**
 * @file BleArbiter.h
 * @brief Serialises access to the BLE radio between scanner and print driver.
 *
 * The NimBLE host can technically scan and connect at the same time, but Cat
 * Printers are cheap devices that frequently fail to connect while the central
 * is scanning. The bridge therefore guarantees that only one "BLE activity"
 * (a scan window, a verification probe, or a print job) is active at a time.
 *
 * The scanner uses a short timeout and simply skips a cycle when a job is
 * running; the job worker waits for the current scan window to finish.
 */
#pragma once

#include "RtosMutex.h"

class BleArbiter
{
public:
    /** Global instance shared by BleDiscovery and CatPrinterDriver. */
    static BleArbiter &instance()
    {
        static BleArbiter arbiter;
        return arbiter;
    }

    /** Acquire exclusive radio access. */
    bool acquire(TickType_t ticks) { return mutex_.lock(ticks); }

    /** Release radio access. */
    void release() { mutex_.unlock(); }

private:
    BleArbiter() = default;
    RtosMutex mutex_;
};

/** Scoped BLE radio ownership. */
class BleRadioLock
{
public:
    explicit BleRadioLock(TickType_t ticks = portMAX_DELAY) : owns_(BleArbiter::instance().acquire(ticks)) {}
    ~BleRadioLock()
    {
        if (owns_)
            BleArbiter::instance().release();
    }
    BleRadioLock(const BleRadioLock &) = delete;
    BleRadioLock &operator=(const BleRadioLock &) = delete;

    bool owns() const { return owns_; }

private:
    bool owns_;
};
