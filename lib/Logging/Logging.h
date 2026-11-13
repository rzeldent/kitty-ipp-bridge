/**
 * @file Logging.h
 * @brief Thread-safe, level based logging subsystem with pluggable sinks.
 *
 * Usage:
 * @code
 *   LOG_I("ble", "Discovered %s rssi=%d", mac.c_str(), rssi);
 * @endcode
 *
 * Every log entry is formatted once and dispatched to all registered sinks.
 * Two sinks are provided:
 *  - SerialLogSink:     prints to the UART console
 *  - RingBufferLogSink: keeps the last N entries in RAM (served by
 *                       GET /api/logs and streamed to the Web UI)
 *
 * Additional sinks (syslog, file, MQTT...) can be added by implementing
 * LogSink and calling Logger::addSink().
 */
#pragma once

#include <Arduino.h>

#include <cstdarg>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "RtosMutex.h"

/** Severity levels, ordered by importance. */
enum class LogLevel : uint8_t
{
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3,
    None = 4 ///< Disable logging entirely
};

/** Short, fixed-width level name ("DEBUG", "INFO", ...). */
const char *logLevelName(LogLevel level);

/** Parse a level name (case-insensitive). Returns @p fallback on error. */
LogLevel logLevelFromString(const std::string &name, LogLevel fallback = LogLevel::Info);

/** A single formatted log line. */
struct LogEntry
{
    uint32_t timestampMs; ///< millis() at creation
    LogLevel level;
    const char *tag;     ///< Static string identifying the subsystem
    const char *message; ///< Formatted message (valid only during write())
};

/** Destination for log entries. Implementations must be fast and non-blocking. */
class LogSink
{
public:
    virtual ~LogSink() = default;
    virtual void write(const LogEntry &entry) = 0;
};

/** Writes log lines to the Arduino Serial port. */
class SerialLogSink : public LogSink
{
public:
    void write(const LogEntry &entry) override;
};

/**
 * @brief Keeps the most recent log lines in memory.
 *
 * Also offers a listener hook which the Web UI uses to push lines to browsers
 * using Server-Sent Events.
 */
class RingBufferLogSink : public LogSink
{
public:
    explicit RingBufferLogSink(size_t capacity = 64);

    void write(const LogEntry &entry) override;

    /** Copy all buffered lines (oldest first). */
    std::vector<std::string> snapshot() const;

    /** Optional listener invoked for every new line (keep it non-blocking!). */
    void setListener(std::function<void(const std::string &)> listener);

private:
    mutable RtosMutex mutex_;
    std::vector<std::string> lines_;
    size_t capacity_;
    size_t next_ = 0;
    bool wrapped_ = false;
    std::function<void(const std::string &)> listener_;
};

/** Central logger (singleton). */
class Logger
{
public:
    static Logger &instance();

    void setLevel(LogLevel level) { level_ = level; }
    LogLevel level() const { return level_; }

    /** Register an additional sink. Sinks must outlive the logger. */
    void addSink(LogSink *sink);

    /** printf-style logging. */
    void log(LogLevel level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
    void vlog(LogLevel level, const char *tag, const char *fmt, va_list args);

    bool enabled(LogLevel level) const { return level >= level_ && level_ != LogLevel::None; }

private:
    Logger() = default;
    RtosMutex mutex_;
    std::vector<LogSink *> sinks_;
    volatile LogLevel level_ = LogLevel::Info;
};

/** Format a log entry into a single line "[  12.345] INFO  tag: message". */
std::string formatLogLine(const LogEntry &entry);

#define LOG_D(tag, ...) Logger::instance().log(LogLevel::Debug, tag, __VA_ARGS__)
#define LOG_I(tag, ...) Logger::instance().log(LogLevel::Info, tag, __VA_ARGS__)
#define LOG_W(tag, ...) Logger::instance().log(LogLevel::Warn, tag, __VA_ARGS__)
#define LOG_E(tag, ...) Logger::instance().log(LogLevel::Error, tag, __VA_ARGS__)
