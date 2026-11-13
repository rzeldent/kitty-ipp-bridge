#include "Logging.h"

#include <strings.h>

namespace
{
    constexpr size_t kMaxMessageLength = 256;
}

const char *logLevelName(LogLevel level)
{
    switch (level)
    {
    case LogLevel::Debug:
        return "DEBUG";
    case LogLevel::Info:
        return "INFO";
    case LogLevel::Warn:
        return "WARN";
    case LogLevel::Error:
        return "ERROR";
    default:
        return "NONE";
    }
}

LogLevel logLevelFromString(const std::string &name, LogLevel fallback)
{
    static const LogLevel levels[] = {LogLevel::Debug, LogLevel::Info, LogLevel::Warn, LogLevel::Error, LogLevel::None};
    for (LogLevel l : levels)
        if (strcasecmp(name.c_str(), logLevelName(l)) == 0)
            return l;
    return fallback;
}

std::string formatLogLine(const LogEntry &entry)
{
    char prefix[48];
    snprintf(prefix, sizeof(prefix), "[%6lu.%03lu] %-5s %s: ",
             static_cast<unsigned long>(entry.timestampMs / 1000),
             static_cast<unsigned long>(entry.timestampMs % 1000),
             logLevelName(entry.level), entry.tag);
    std::string line(prefix);
    line += entry.message;
    return line;
}

// ---------------------------------------------------------------------------
// SerialLogSink
// ---------------------------------------------------------------------------
void SerialLogSink::write(const LogEntry &entry)
{
    std::string line = formatLogLine(entry);
    Serial.println(line.c_str());
}

// ---------------------------------------------------------------------------
// RingBufferLogSink
// ---------------------------------------------------------------------------
RingBufferLogSink::RingBufferLogSink(size_t capacity) : capacity_(capacity ? capacity : 1)
{
    lines_.resize(capacity_);
}

void RingBufferLogSink::write(const LogEntry &entry)
{
    std::string line = formatLogLine(entry);
    std::function<void(const std::string &)> listener;
    {
        RtosLock lock(mutex_);
        lines_[next_] = line;
        next_ = (next_ + 1) % capacity_;
        if (next_ == 0)
            wrapped_ = true;
        listener = listener_;
    }
    if (listener)
        listener(line);
}

std::vector<std::string> RingBufferLogSink::snapshot() const
{
    RtosLock lock(mutex_);
    std::vector<std::string> out;
    if (wrapped_)
    {
        out.reserve(capacity_);
        for (size_t i = 0; i < capacity_; ++i)
            out.push_back(lines_[(next_ + i) % capacity_]);
    }
    else
    {
        out.assign(lines_.begin(), lines_.begin() + next_);
    }
    return out;
}

void RingBufferLogSink::setListener(std::function<void(const std::string &)> listener)
{
    RtosLock lock(mutex_);
    listener_ = std::move(listener);
}

// ---------------------------------------------------------------------------
// Logger
// ---------------------------------------------------------------------------
Logger &Logger::instance()
{
    static Logger logger;
    return logger;
}

void Logger::addSink(LogSink *sink)
{
    if (!sink)
        return;
    RtosLock lock(mutex_);
    sinks_.push_back(sink);
}

void Logger::log(LogLevel level, const char *tag, const char *fmt, ...)
{
    if (!enabled(level))
        return;
    va_list args;
    va_start(args, fmt);
    vlog(level, tag, fmt, args);
    va_end(args);
}

void Logger::vlog(LogLevel level, const char *tag, const char *fmt, va_list args)
{
    if (!enabled(level))
        return;
    char buffer[kMaxMessageLength];
    vsnprintf(buffer, sizeof(buffer), fmt, args);

    LogEntry entry{millis(), level, tag ? tag : "-", buffer};

    // The lock serialises output so lines from different tasks never interleave.
    RtosLock lock(mutex_);
    for (LogSink *sink : sinks_)
        sink->write(entry);
}
