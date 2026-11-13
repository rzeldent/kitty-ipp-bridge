/**
 * @file RtosMutex.h
 * @brief Small RAII helpers around FreeRTOS mutexes.
 *
 * All shared state in the bridge (printer registry, job status table,
 * configuration, BLE stack access) is protected with FreeRTOS mutexes. These
 * helpers make the locking exception-safe and keep call sites short:
 *
 * @code
 *   RtosMutex mutex;
 *   {
 *       RtosLock lock(mutex);
 *       // ... critical section ...
 *   }
 * @endcode
 */
#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

/**
 * @brief Recursive FreeRTOS mutex with priority inheritance.
 *
 * A recursive mutex is used so that a public method holding the lock can call
 * another public method of the same object without dead-locking.
 */
class RtosMutex
{
public:
    RtosMutex() : handle_(xSemaphoreCreateRecursiveMutex()) {}
    ~RtosMutex()
    {
        if (handle_)
            vSemaphoreDelete(handle_);
    }

    RtosMutex(const RtosMutex &) = delete;
    RtosMutex &operator=(const RtosMutex &) = delete;

    /** Take the mutex, waiting at most @p ticks. Returns true on success. */
    bool lock(TickType_t ticks = portMAX_DELAY) { return xSemaphoreTakeRecursive(handle_, ticks) == pdTRUE; }

    /** Release the mutex. Must be called by the owning task. */
    void unlock() { xSemaphoreGiveRecursive(handle_); }

private:
    SemaphoreHandle_t handle_;
};

/**
 * @brief Scoped lock for RtosMutex (similar to std::lock_guard).
 *
 * When constructed with a timeout, check owns() before touching shared state.
 */
class RtosLock
{
public:
    explicit RtosLock(RtosMutex &m, TickType_t ticks = portMAX_DELAY) : mutex_(m), owns_(m.lock(ticks)) {}
    ~RtosLock()
    {
        if (owns_)
            mutex_.unlock();
    }

    RtosLock(const RtosLock &) = delete;
    RtosLock &operator=(const RtosLock &) = delete;

    /** True if the mutex was acquired. */
    bool owns() const { return owns_; }

private:
    RtosMutex &mutex_;
    bool owns_;
};
