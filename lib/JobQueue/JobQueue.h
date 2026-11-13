/**
 * @file JobQueue.h
 * @brief FreeRTOS print queue with a single worker task.
 *
 * All BLE printing goes through this queue so that exactly one printer is
 * active at a time. Producers (IPP server, Web UI, REST API) submit jobs and
 * receive a job id; a dedicated worker task processes them in FIFO order:
 *
 *   submit() ──► FreeRTOS queue (PrintJob*) ──► worker task
 *                                                  │ acquire BLE radio lock
 *                                                  │ connect ─► print ─► disconnect
 *                                                  │ release radio lock
 *                                                  └► status table + callbacks
 *
 * Memory: a job owns its payload. Bitmap payloads are 1-bit packed rows (48
 * bytes per row at 384 px). On PSRAM boards large allocations are served
 * from PSRAM automatically by the Arduino core. The total number of queued
 * payload bytes is bounded (Settings::maxQueuedBytes) to protect the heap,
 * and the number of jobs by Settings::queueSize (overflow protection).
 */
#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "RtosMutex.h"

class PrinterRegistry;
class ConfigManager;

/** Kind of work carried by a PrintJob. */
enum class JobType : uint8_t
{
    Bitmap,     ///< payload = packed 1-bit rows (widthPx x heightPx, MSB first, 1 = black)
    Identify,   ///< Print "CAT PRINTER IDENTIFY" + feed 20 mm (payload unused)
    TestPage,   ///< Print a generated test page (payload unused)
    FeedPaper,  ///< Feed paper by feedLines (payload unused)
    RawPackets, ///< payload = pre-encoded protocol bytes, sent verbatim
};

/** Life cycle of a job. */
enum class JobState : uint8_t
{
    Pending,
    Processing,
    Completed,
    Failed,
    Cancelled,
};

const char *jobTypeName(JobType type);
const char *jobStateName(JobState state);

/** Snapshot of a job as reported by the status API. */
struct JobStatus
{
    uint32_t id = 0;
    std::string printerMac;
    std::string name;
    std::string user;
    JobType type = JobType::Bitmap;
    JobState state = JobState::Pending;
    uint8_t attempts = 0;
    uint32_t createdAt = 0;   ///< Uptime seconds
    uint32_t startedAt = 0;   ///< Uptime seconds (0 = not started)
    uint32_t completedAt = 0; ///< Uptime seconds (0 = not finished)
    uint32_t bytes = 0;       ///< Payload size
    uint32_t rowsTotal = 0;
    uint32_t rowsDone = 0;
    std::string message; ///< Error description for failed jobs

    bool finished() const
    {
        return state == JobState::Completed || state == JobState::Failed || state == JobState::Cancelled;
    }
};

/** A unit of work for the worker task. */
struct PrintJob
{
    std::string printerMac;       ///< Target printer (canonical MAC)
    std::vector<uint8_t> payload; ///< Job data, interpretation depends on type

    JobType type = JobType::Bitmap;
    uint16_t widthPx = 384;  ///< Bitmap width
    uint32_t heightPx = 0;   ///< Bitmap height (rows)
    uint16_t feedLines = 0;  ///< Extra feed for FeedPaper jobs
    std::string name;        ///< Job name (IPP job-name or generated)
    std::string user;        ///< Requesting user
    uint32_t id = 0;         ///< Assigned by JobQueue::submit()
    uint8_t attempts = 0;    ///< Attempts performed so far
    uint32_t createdAt = 0;  ///< Uptime seconds

    /** Invoked from the worker task once the job reached a final state. */
    std::function<void(const JobStatus &)> onComplete;
};

class JobQueue
{
public:
    struct Settings
    {
        uint8_t queueSize = 8;            ///< Maximum queued (not yet processing) jobs
        uint8_t maxRetries = 3;           ///< Attempts per job (1 = no retry)
        uint32_t retryDelayMs = 3000;     ///< Base delay between attempts (multiplied by attempt)
        size_t maxQueuedBytes = 160 * 1024; ///< Upper bound of payload bytes held by the queue
        size_t historySize = 32;          ///< Finished jobs kept for status queries
    };

    using Listener = std::function<void(const JobStatus &)>;

    bool begin(PrinterRegistry *registry, ConfigManager *config, const Settings &settings);
    bool startTask(UBaseType_t priority = 3, BaseType_t core = 1, uint32_t stackSize = 8192);

    /** Update retry settings at run time (queue length is fixed after begin()). */
    void applySettings(const Settings &settings);

    /**
     * Queue a job.
     * @param job   Job to queue; ownership of the payload moves into the queue.
     * @param error Optional reason when the job is rejected.
     * @return Job id (> 0) or 0 when rejected (queue full, too large, unknown printer).
     */
    uint32_t submit(PrintJob &&job, std::string *error = nullptr);

    /** Queue an identification job (connect, print "CAT PRINTER IDENTIFY", feed 20 mm, disconnect). */
    uint32_t identifyPrinter(const std::string &mac, std::string *error = nullptr);

    /** Queue a generated test page. */
    uint32_t printTestPage(const std::string &mac, std::string *error = nullptr);

    /** Cancel a pending or running job. */
    bool cancel(uint32_t id);

    bool getStatus(uint32_t id, JobStatus &out) const;
    std::vector<JobStatus> list() const;

    /** Jobs waiting in the queue (excluding the active one). */
    size_t pendingCount() const;
    /** Id of the job being processed, 0 when idle. */
    uint32_t activeJob() const { return active_; }
    /** Active or pending job for a printer? (used for IPP printer-state). */
    bool printerBusy(const std::string &mac) const;
    size_t queuedBytes() const { return queuedBytes_; }
    size_t capacity() const { return settings_.queueSize; }

    /** Register a listener for job state changes (called from various tasks, keep it short). */
    void addListener(Listener listener);

private:
    static void taskEntry(void *arg);
    void run();
    void process(PrintJob *job);
    bool execute(PrintJob &job, std::string &error, bool &retryable);
    void updateStatus(uint32_t id, const std::function<void(JobStatus &)> &fn);
    void notify(const JobStatus &status);
    void trimHistory();

    Settings settings_;
    PrinterRegistry *registry_ = nullptr;
    ConfigManager *config_ = nullptr;
    QueueHandle_t queue_ = nullptr;
    TaskHandle_t task_ = nullptr;
    mutable RtosMutex mutex_;
    std::deque<JobStatus> status_;
    std::vector<Listener> listeners_;
    std::atomic<uint32_t> nextId_{1};
    std::atomic<uint32_t> active_{0};
    std::atomic<uint32_t> cancelRequested_{0};
    std::atomic<size_t> queuedBytes_{0};
};
