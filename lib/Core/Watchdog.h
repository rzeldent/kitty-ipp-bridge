/**
 * @file Watchdog.h
 * @brief Thin wrapper around the ESP-IDF Task Watchdog Timer (TWDT).
 *
 * Every long running bridge task subscribes itself and feeds the watchdog in
 * its main loop. Long blocking operations (BLE scan windows, printing a long
 * page) feed the watchdog from inside their loops. If a task hangs for longer
 * than the configured timeout, the TWDT panics and the chip reboots, which is
 * the desired behaviour for an unattended appliance.
 */
#pragma once

#include <cstdint>

namespace watchdog
{
    /** Reconfigure the TWDT timeout (seconds) and enable panic on expiry. */
    void begin(uint32_t timeoutSeconds);

    /** Subscribe the calling task. Safe to call multiple times. */
    void subscribe();

    /** Unsubscribe the calling task (e.g. before it deletes itself). */
    void unsubscribe();

    /** Feed the watchdog for the calling task (no-op if not subscribed). */
    void feed();
} // namespace watchdog
