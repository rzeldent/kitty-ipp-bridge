/**
 * @file SystemStats.h
 * @brief Process wide counters exposed through GET /api/stats.
 *
 * Counters are lock-free atomics so that every subsystem can update them from
 * any task (or the AsyncTCP callback context) without additional locking.
 */
#pragma once

#include <atomic>
#include <cstdint>

struct SystemStats
{
    std::atomic<uint32_t> jobsPrinted{0};      ///< Jobs completed successfully
    std::atomic<uint32_t> jobsFailed{0};       ///< Jobs that failed after all retries
    std::atomic<uint32_t> jobsQueued{0};       ///< Jobs accepted into the queue
    std::atomic<uint32_t> jobsRejected{0};     ///< Jobs rejected (queue full, too large, ...)
    std::atomic<uint32_t> jobRetries{0};       ///< Individual retry attempts
    std::atomic<uint32_t> bleScans{0};         ///< Completed BLE scan windows
    std::atomic<uint32_t> bleProbes{0};        ///< Temporary service discovery connections
    std::atomic<uint32_t> bleConnectFailures{0}; ///< Failed print connections
    std::atomic<uint32_t> ippRequests{0};      ///< IPP operations handled
    std::atomic<uint32_t> ippErrors{0};        ///< IPP requests answered with an error status
    std::atomic<uint32_t> httpRequests{0};     ///< REST/Web UI requests
    std::atomic<uint32_t> mdnsRestarts{0};     ///< mDNS responder (re)starts
    std::atomic<uint32_t> wifiReconnects{0};   ///< Wi-Fi reconnect events
    std::atomic<uint32_t> minFreeHeap{UINT32_MAX}; ///< Lowest free heap observed by the monitor

    /** Global instance. */
    static SystemStats &instance()
    {
        static SystemStats stats;
        return stats;
    }
};
