/**
 * @file JobQueue.cpp
 * @brief Implementation of the FreeRTOS print queue and worker task.
 */
#include "JobQueue.h"

#include <Arduino.h>

#include "BleArbiter.h"
#include "CatPrinterDriver.h"
#include "ConfigManager.h"
#include "Logging.h"
#include "MacUtil.h"
#include "PrinterProfile.h"
#include "PrinterRegistry.h"
#include "SystemStats.h"
#include "TextRenderer.h"
#include "Watchdog.h"

static const char *TAG = "jobs";

const char *jobTypeName(JobType type)
{
    switch (type)
    {
    case JobType::Bitmap:
        return "bitmap";
    case JobType::Identify:
        return "identify";
    case JobType::TestPage:
        return "test-page";
    case JobType::FeedPaper:
        return "feed";
    case JobType::RawPackets:
        return "raw";
    }
    return "unknown";
}

const char *jobStateName(JobState state)
{
    switch (state)
    {
    case JobState::Pending:
        return "pending";
    case JobState::Processing:
        return "processing";
    case JobState::Completed:
        return "completed";
    case JobState::Failed:
        return "failed";
    case JobState::Cancelled:
        return "cancelled";
    }
    return "unknown";
}

bool JobQueue::begin(PrinterRegistry *registry, ConfigManager *config, const Settings &settings)
{
    registry_ = registry;
    config_ = config;
    settings_ = settings;
    if (settings_.queueSize == 0)
        settings_.queueSize = 1;
    queue_ = xQueueCreate(settings_.queueSize, sizeof(PrintJob *));
    if (!queue_)
    {
        LOG_E(TAG, "Unable to create job queue");
        return false;
    }
    LOG_I(TAG, "Job queue ready (%u slots, %u KiB payload budget, %u attempts)", settings_.queueSize,
          static_cast<unsigned>(settings_.maxQueuedBytes / 1024), settings_.maxRetries);
    return true;
}

void JobQueue::applySettings(const Settings &settings)
{
    RtosLock lock(mutex_);
    settings_.maxRetries = settings.maxRetries ? settings.maxRetries : 1;
    settings_.retryDelayMs = settings.retryDelayMs;
}

bool JobQueue::startTask(UBaseType_t priority, BaseType_t core, uint32_t stackSize)
{
    if (!queue_)
        return false;
    return xTaskCreatePinnedToCore(taskEntry, "job_worker", stackSize, this, priority, &task_, core) == pdPASS;
}

void JobQueue::addListener(Listener listener)
{
    RtosLock lock(mutex_);
    listeners_.push_back(std::move(listener));
}

// ---------------------------------------------------------------------------
// Producers
// ---------------------------------------------------------------------------
uint32_t JobQueue::submit(PrintJob &&job, std::string *error)
{
    auto reject = [&](const char *reason) -> uint32_t {
        SystemStats::instance().jobsRejected++;
        LOG_W(TAG, "Job for %s rejected: %s", job.printerMac.c_str(), reason);
        if (error)
            *error = reason;
        return 0;
    };

    if (!queue_)
        return reject("queue not initialised");

    job.printerMac = macutil::normalize(job.printerMac);
    PrinterRecord record;
    if (job.printerMac.empty() || !registry_ || !registry_->get(job.printerMac, record))
        return reject("unknown printer");

    if (job.type == JobType::Bitmap)
    {
        const size_t rowBytes = (job.widthPx + 7u) / 8u;
        if (job.heightPx == 0 || job.payload.size() != rowBytes * job.heightPx)
            return reject("invalid bitmap dimensions");
    }

    const size_t bytes = job.payload.size();
    if (queuedBytes_ + bytes > settings_.maxQueuedBytes)
        return reject("queue memory budget exceeded");
    if (uxQueueSpacesAvailable(queue_) == 0)
        return reject("queue full");

    job.id = nextId_++;
    if (job.id == 0) // wrap-around
        job.id = nextId_++;
    job.createdAt = PrinterRegistry::nowSeconds();
    job.attempts = 0;
    if (job.name.empty())
        job.name = std::string(jobTypeName(job.type)) + "-" + std::to_string(job.id);

    JobStatus status;
    status.id = job.id;
    status.printerMac = job.printerMac;
    status.name = job.name;
    status.user = job.user;
    status.type = job.type;
    status.createdAt = job.createdAt;
    status.bytes = static_cast<uint32_t>(bytes);
    status.rowsTotal = job.type == JobType::Bitmap ? job.heightPx : 0;

    PrintJob *heapJob = new (std::nothrow) PrintJob(std::move(job));
    if (!heapJob)
        return reject("out of memory");

    {
        RtosLock lock(mutex_);
        status_.push_back(status);
        trimHistory();
    }
    queuedBytes_ += bytes;

    if (xQueueSend(queue_, &heapJob, 0) != pdTRUE)
    {
        queuedBytes_ -= bytes;
        {
            RtosLock lock(mutex_);
            for (auto it = status_.begin(); it != status_.end(); ++it)
                if (it->id == heapJob->id)
                {
                    status_.erase(it);
                    break;
                }
        }
        // Restore the caller's job object for diagnostics.
        job = std::move(*heapJob);
        delete heapJob;
        return reject("queue full");
    }

    SystemStats::instance().jobsQueued++;
    LOG_I(TAG, "Job %u (%s, %u bytes) queued for %s", static_cast<unsigned>(status.id), jobTypeName(status.type),
          static_cast<unsigned>(bytes), status.printerMac.c_str());
    notify(status);
    return status.id;
}

uint32_t JobQueue::identifyPrinter(const std::string &mac, std::string *error)
{
    PrintJob job;
    job.printerMac = mac;
    job.type = JobType::Identify;
    job.name = "identify";
    return submit(std::move(job), error);
}

uint32_t JobQueue::printTestPage(const std::string &mac, std::string *error)
{
    PrintJob job;
    job.printerMac = mac;
    job.type = JobType::TestPage;
    job.name = "test-page";
    return submit(std::move(job), error);
}

bool JobQueue::cancel(uint32_t id)
{
    bool found = false;
    JobStatus snapshot;
    {
        RtosLock lock(mutex_);
        for (auto &s : status_)
        {
            if (s.id != id)
                continue;
            if (s.state == JobState::Pending)
            {
                s.state = JobState::Cancelled;
                s.completedAt = PrinterRegistry::nowSeconds();
                s.message = "cancelled before processing";
                snapshot = s;
                found = true;
            }
            else if (s.state == JobState::Processing)
            {
                cancelRequested_ = id;
                found = true;
            }
            break;
        }
    }
    if (found && snapshot.id)
        notify(snapshot);
    if (found)
        LOG_I(TAG, "Cancel requested for job %u", static_cast<unsigned>(id));
    return found;
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------
bool JobQueue::getStatus(uint32_t id, JobStatus &out) const
{
    RtosLock lock(mutex_);
    for (const auto &s : status_)
        if (s.id == id)
        {
            out = s;
            return true;
        }
    return false;
}

std::vector<JobStatus> JobQueue::list() const
{
    RtosLock lock(mutex_);
    return std::vector<JobStatus>(status_.begin(), status_.end());
}

size_t JobQueue::pendingCount() const
{
    return queue_ ? uxQueueMessagesWaiting(queue_) : 0;
}

bool JobQueue::printerBusy(const std::string &mac) const
{
    RtosLock lock(mutex_);
    for (const auto &s : status_)
        if (s.printerMac == mac && (s.state == JobState::Pending || s.state == JobState::Processing))
            return true;
    return false;
}

void JobQueue::updateStatus(uint32_t id, const std::function<void(JobStatus &)> &fn)
{
    JobStatus snapshot;
    {
        RtosLock lock(mutex_);
        for (auto &s : status_)
            if (s.id == id)
            {
                fn(s);
                snapshot = s;
                break;
            }
    }
    if (snapshot.id)
        notify(snapshot);
}

void JobQueue::notify(const JobStatus &status)
{
    std::vector<Listener> listeners;
    {
        RtosLock lock(mutex_);
        listeners = listeners_;
    }
    for (auto &l : listeners)
        l(status);
}

void JobQueue::trimHistory()
{
    // Drop the oldest *finished* entries beyond the history size. Pending and
    // processing entries are bounded by the queue length and always kept.
    while (status_.size() > settings_.historySize + settings_.queueSize + 1)
    {
        auto it = status_.begin();
        while (it != status_.end() && !it->finished())
            ++it;
        if (it == status_.end())
            break;
        status_.erase(it);
    }
}

// ---------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------
void JobQueue::taskEntry(void *arg)
{
    static_cast<JobQueue *>(arg)->run();
}

void JobQueue::run()
{
    watchdog::subscribe();
    LOG_I(TAG, "Worker task started");
    for (;;)
    {
        PrintJob *job = nullptr;
        if (xQueueReceive(queue_, &job, pdMS_TO_TICKS(1000)) == pdTRUE && job)
        {
            process(job);
            queuedBytes_ -= job->payload.size();
            delete job; // releases the payload (memory leak prevention)
        }
        watchdog::feed();
    }
}

void JobQueue::process(PrintJob *job)
{
    JobStatus current;
    if (getStatus(job->id, current) && current.state == JobState::Cancelled)
    {
        LOG_I(TAG, "Skipping cancelled job %u", static_cast<unsigned>(job->id));
        if (job->onComplete)
            job->onComplete(current);
        return;
    }

    active_ = job->id;
    cancelRequested_ = 0;
    const uint32_t started = PrinterRegistry::nowSeconds();
    updateStatus(job->id, [&](JobStatus &s) {
        s.state = JobState::Processing;
        s.startedAt = started;
    });

    std::string error;
    bool success = false;
    uint8_t maxAttempts;
    uint32_t retryDelay;
    {
        RtosLock lock(mutex_);
        maxAttempts = settings_.maxRetries ? settings_.maxRetries : 1;
        retryDelay = settings_.retryDelayMs;
    }

    while (job->attempts < maxAttempts)
    {
        job->attempts++;
        updateStatus(job->id, [&](JobStatus &s) { s.attempts = job->attempts; });

        bool retryable = true;
        success = execute(*job, error, retryable);
        if (success || !retryable || cancelRequested_ == job->id)
            break;
        if (job->attempts < maxAttempts)
        {
            SystemStats::instance().jobRetries++;
            LOG_W(TAG, "Job %u attempt %u failed (%s), retrying", static_cast<unsigned>(job->id), static_cast<unsigned>(job->attempts), error.c_str());
            // Wait in small steps so the watchdog stays fed and cancel requests are honoured.
            const uint32_t waitMs = retryDelay * job->attempts;
            for (uint32_t waited = 0; waited < waitMs && cancelRequested_ != job->id; waited += 250)
            {
                vTaskDelay(pdMS_TO_TICKS(250));
                watchdog::feed();
            }
        }
    }

    const bool cancelled = cancelRequested_ == job->id;
    const JobState finalState = success ? JobState::Completed : (cancelled ? JobState::Cancelled : JobState::Failed);
    if (success)
    {
        SystemStats::instance().jobsPrinted++;
        LOG_I(TAG, "Job %u completed on %s", static_cast<unsigned>(job->id), job->printerMac.c_str());
    }
    else if (!cancelled)
    {
        SystemStats::instance().jobsFailed++;
        LOG_E(TAG, "Job %u failed: %s", static_cast<unsigned>(job->id), error.c_str());
    }
    else
    {
        LOG_I(TAG, "Job %u cancelled", static_cast<unsigned>(job->id));
    }

    JobStatus finalStatus;
    updateStatus(job->id, [&](JobStatus &s) {
        s.state = finalState;
        s.completedAt = PrinterRegistry::nowSeconds();
        if (!success)
            s.message = cancelled ? "cancelled" : error;
        finalStatus = s;
    });
    {
        RtosLock lock(mutex_);
        trimHistory();
    }
    active_ = 0;
    cancelRequested_ = 0;

    if (job->onComplete)
        job->onComplete(finalStatus);
}

bool JobQueue::execute(PrintJob &job, std::string &error, bool &retryable)
{
    PrinterRecord record;
    if (!registry_->get(job.printerMac, record))
    {
        error = "printer removed from registry";
        retryable = false;
        return false;
    }

    const PrinterProfile *profile = PrinterProfiles::findByName(record.profileName);
    if (!profile)
        profile = &PrinterProfiles::defaultProfile();

    const ProtocolConfig proto = config_ ? config_->protocol() : ProtocolConfig{};
    CatPrinterDriver::Options options;
    options.chunkSize = proto.chunkSize;
    options.packetDelayMs = proto.packetDelayMs;
    catgfx::PrintSettings settings;
    settings.energy = proto.energy;
    settings.speed = proto.speed;

    // Only one BLE session at a time: wait (feeding the watchdog) until the scanner releases the radio.
    bool acquired = false;
    for (int i = 0; i < 30 && !acquired; ++i)
    {
        watchdog::feed();
        acquired = BleArbiter::instance().acquire(pdMS_TO_TICKS(1000));
    }
    if (!acquired)
    {
        error = "BLE radio busy";
        return false;
    }
    struct RadioGuard
    {
        ~RadioGuard() { BleArbiter::instance().release(); }
    } radioGuard;

    CatPrinterDriver driver; // destructor guarantees disconnect
    driver.setOptions(options);

    if (!driver.connect(record.mac, record.addrType, *profile))
    {
        error = CatPrinterDriver::errorString(driver.lastError());
        retryable = driver.lastError() != CatPrinterDriver::Error::UnsupportedProtocol;
        return false;
    }
    registry_->touch(record.mac); // a successful connection proves the printer is alive

    uint32_t rowsSent = 0;
    auto progress = [&](uint32_t done, uint32_t total) {
        rowsSent = done;
        updateStatus(job.id, [&](JobStatus &s) {
            s.rowsDone = done;
            s.rowsTotal = total;
        });
        watchdog::feed();
        return cancelRequested_ != job.id;
    };

    bool ok = false;
    switch (job.type)
    {
    case JobType::Bitmap:
    {
        raster::MonoBitmap bmp(job.widthPx);
        bmp.height = job.heightPx;
        bmp.data.swap(job.payload); // borrow the payload without copying
        ok = driver.printBitmap(bmp, settings, proto.feedAfterJobLines, progress);
        job.payload.swap(bmp.data);
        break;
    }
    case JobType::Identify:
        ok = driver.identify(record.displayName(), record.mac, settings);
        break;
    case JobType::TestPage:
    {
        std::vector<std::string> lines = {
            "CatBridge test page",
            record.displayName(),
            record.mac,
            "Profile: " + record.profileName,
            "RSSI: " + std::to_string(record.rssi) + " dBm",
            "MTU: " + std::to_string(driver.mtu()),
            "Energy: " + std::to_string(proto.energy) + " Speed: " + std::to_string(proto.speed),
        };
        raster::MonoBitmap page = raster::buildTestPage(lines, profile->printWidthPx);
        ok = driver.printBitmap(page, settings, proto.feedAfterJobLines, progress);
        break;
    }
    case JobType::FeedPaper:
        ok = driver.feedPaper(job.feedLines ? job.feedLines : proto.feedAfterJobLines);
        break;
    case JobType::RawPackets:
        ok = driver.sendRaw(job.payload.data(), job.payload.size());
        break;
    }

    if (!ok)
    {
        error = CatPrinterDriver::errorString(driver.lastError());
        // Never re-send a partially printed bitmap: that would duplicate output.
        retryable = rowsSent == 0 && job.type != JobType::RawPackets &&
                    driver.lastError() != CatPrinterDriver::Error::Cancelled;
    }
    driver.disconnect(); // disconnect immediately after completion
    return ok;
}
