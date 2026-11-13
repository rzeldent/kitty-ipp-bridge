/**
 * @file IppMessage.h
 * @brief Minimal IPP/1.1 + IPP/2.0 message encoding and decoding (RFC 8010).
 *
 * Only what a printer needs is implemented:
 *  - IppRequestParser: incremental parser for the request header and
 *    attribute groups. Once complete, the remaining bytes of the HTTP body
 *    are the document data (streamed separately).
 *  - IppWriter: builder for responses, including collections
 *    (media-col-database etc.) required by IPP Everywhere clients.
 *
 * Pure C++ without Arduino dependencies; covered by host unit tests.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ipp
{

    // ---- Operation ids ------------------------------------------------------
    enum Operation : uint16_t
    {
        OP_PRINT_JOB = 0x0002,
        OP_VALIDATE_JOB = 0x0004,
        OP_CREATE_JOB = 0x0005,
        OP_SEND_DOCUMENT = 0x0006,
        OP_CANCEL_JOB = 0x0008,
        OP_GET_JOB_ATTRIBUTES = 0x0009,
        OP_GET_JOBS = 0x000A,
        OP_GET_PRINTER_ATTRIBUTES = 0x000B,
    };

    // ---- Status codes ------------------------------------------------------
    enum Status : uint16_t
    {
        STATUS_OK = 0x0000,
        STATUS_OK_IGNORED_OR_SUBSTITUTED = 0x0001,
        CLIENT_ERROR_BAD_REQUEST = 0x0400,
        CLIENT_ERROR_FORBIDDEN = 0x0401,
        CLIENT_ERROR_NOT_FOUND = 0x0406,
        CLIENT_ERROR_NOT_POSSIBLE = 0x0404,
        CLIENT_ERROR_REQUEST_ENTITY_TOO_LARGE = 0x0409,
        CLIENT_ERROR_DOCUMENT_FORMAT_NOT_SUPPORTED = 0x040A,
        CLIENT_ERROR_ATTRIBUTES_OR_VALUES_NOT_SUPPORTED = 0x040B,
        CLIENT_ERROR_DOCUMENT_FORMAT_ERROR = 0x040F,
        SERVER_ERROR_INTERNAL_ERROR = 0x0500,
        SERVER_ERROR_OPERATION_NOT_SUPPORTED = 0x0501,
        SERVER_ERROR_VERSION_NOT_SUPPORTED = 0x0503,
        SERVER_ERROR_NOT_ACCEPTING_JOBS = 0x0506,
        SERVER_ERROR_BUSY = 0x0507,
    };

    // ---- Delimiter and value tags -------------------------------------------
    enum Tag : uint8_t
    {
        TAG_OPERATION = 0x01,
        TAG_JOB = 0x02,
        TAG_END = 0x03,
        TAG_PRINTER = 0x04,
        TAG_UNSUPPORTED_GROUP = 0x05,

        TAG_UNSUPPORTED_VALUE = 0x10,
        TAG_UNKNOWN = 0x12,
        TAG_NO_VALUE = 0x13,

        TAG_INTEGER = 0x21,
        TAG_BOOLEAN = 0x22,
        TAG_ENUM = 0x23,

        TAG_STRING = 0x30,
        TAG_DATE = 0x31,
        TAG_RESOLUTION = 0x32,
        TAG_RANGE = 0x33,
        TAG_BEGIN_COLLECTION = 0x34,
        TAG_TEXT_LANG = 0x35,
        TAG_NAME_LANG = 0x36,
        TAG_END_COLLECTION = 0x37,

        TAG_TEXT = 0x41,
        TAG_NAME = 0x42,
        TAG_KEYWORD = 0x44,
        TAG_URI = 0x45,
        TAG_URI_SCHEME = 0x46,
        TAG_CHARSET = 0x47,
        TAG_LANGUAGE = 0x48,
        TAG_MIME_TYPE = 0x49,
        TAG_MEMBER_NAME = 0x4A,
    };

    // ---- Job states ----------------------------------------------------------
    enum JobState : int32_t
    {
        JOB_PENDING = 3,
        JOB_PENDING_HELD = 4,
        JOB_PROCESSING = 5,
        JOB_PROCESSING_STOPPED = 6,
        JOB_CANCELED = 7,
        JOB_ABORTED = 8,
        JOB_COMPLETED = 9,
    };

    // ---- Printer states ------------------------------------------------------
    enum PrinterState : int32_t
    {
        PRINTER_IDLE = 3,
        PRINTER_PROCESSING = 4,
        PRINTER_STOPPED = 5,
    };

    /** One top-level attribute (collection members are skipped). */
    struct Attribute
    {
        uint8_t group = 0;               ///< Delimiter tag of the containing group
        uint8_t valueTag = 0;            ///< Tag of the first value
        std::string name;
        std::vector<std::string> values; ///< Raw value bytes, one entry per value

        /** First value interpreted as string (empty if none). */
        std::string asString() const { return values.empty() ? std::string() : values.front(); }
        /** First value as big-endian 32 bit integer (integer/enum). */
        int32_t asInt(int32_t fallback = 0) const;
        bool asBool(bool fallback = false) const;
    };

    struct Request
    {
        uint8_t versionMajor = 0;
        uint8_t versionMinor = 0;
        uint16_t operation = 0;
        uint32_t requestId = 0;
        std::vector<Attribute> attributes;

        const Attribute *find(const std::string &name, uint8_t group = 0) const;
        std::string getString(const std::string &name, const std::string &fallback = std::string()) const;
        int32_t getInt(const std::string &name, int32_t fallback = 0) const;
        /** All string values of a (possibly multi-valued) attribute. */
        std::vector<std::string> getStrings(const std::string &name) const;
    };

    /**
     * @brief Incremental IPP request parser.
     *
     * Call feed() with consecutive chunks of the HTTP body. It returns the
     * number of bytes consumed; once complete() is true, all following bytes
     * (including the unconsumed tail of the last chunk) are document data.
     */
    class RequestParser
    {
    public:
        explicit RequestParser(size_t maxAttributeBytes = 16384) : maxBytes_(maxAttributeBytes) {}

        /** Returns bytes consumed from @p data. Check error()/complete() afterwards. */
        size_t feed(const uint8_t *data, size_t len);

        bool complete() const { return complete_; }
        bool error() const { return !error_.empty(); }
        const std::string &errorMessage() const { return error_; }
        const Request &request() const { return request_; }

        void reset();

    private:
        bool tryParse();

        std::vector<uint8_t> buf_;
        size_t maxBytes_;
        bool complete_ = false;
        std::string error_;
        Request request_;
    };

    /** Builder for IPP response messages. */
    class Writer
    {
    public:
        Writer() { buf_.reserve(1024); }

        /** Response header (version 2.0 unless the request used 1.x). */
        void header(uint8_t major, uint8_t minor, uint16_t statusOrOp, uint32_t requestId);
        /** Group delimiter (TAG_OPERATION, TAG_PRINTER, TAG_JOB, ...). */
        void group(uint8_t tag);
        /** End-of-attributes tag. */
        void end();

        // Single valued attributes (pass an empty name to add another value
        // to the previous attribute, i.e. a "1setOf").
        void string(uint8_t tag, const std::string &name, const std::string &value);
        void strings(uint8_t tag, const std::string &name, const std::vector<std::string> &values);
        void integer(const std::string &name, int32_t value);
        void integers(uint8_t tag, const std::string &name, const std::vector<int32_t> &values);
        void enumValue(const std::string &name, int32_t value);
        void boolean(const std::string &name, bool value);
        void range(const std::string &name, int32_t lower, int32_t upper);
        void resolution(const std::string &name, int32_t x, int32_t y, uint8_t units = 3 /* dpi */);
        void noValue(const std::string &name);

        // Convenience wrappers for common tags
        void keyword(const std::string &n, const std::string &v) { string(TAG_KEYWORD, n, v); }
        void keywords(const std::string &n, const std::vector<std::string> &v) { strings(TAG_KEYWORD, n, v); }
        void text(const std::string &n, const std::string &v) { string(TAG_TEXT, n, v); }
        void name(const std::string &n, const std::string &v) { string(TAG_NAME, n, v); }
        void uri(const std::string &n, const std::string &v) { string(TAG_URI, n, v); }
        void mimeType(const std::string &n, const std::string &v) { string(TAG_MIME_TYPE, n, v); }
        void charset(const std::string &n, const std::string &v) { string(TAG_CHARSET, n, v); }
        void language(const std::string &n, const std::string &v) { string(TAG_LANGUAGE, n, v); }

        // Collections
        void beginCollection(const std::string &name);
        void memberName(const std::string &member);
        void endCollection();

        const std::vector<uint8_t> &data() const { return buf_; }
        std::vector<uint8_t> &data() { return buf_; }

    private:
        void raw(uint8_t tag, const std::string &name, const uint8_t *value, size_t len);
        void u16(uint16_t v);
        void u32(uint32_t v);

        std::vector<uint8_t> buf_;
    };

    /** Human readable operation name, for logging. */
    const char *operationName(uint16_t op);

} // namespace ipp
