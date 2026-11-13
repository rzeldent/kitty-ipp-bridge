#include "IppMessage.h"

namespace ipp
{
    namespace
    {
        uint16_t be16(const uint8_t *p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
        uint32_t be32(const uint8_t *p)
        {
            return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
                   (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
        }
    } // namespace

    // -----------------------------------------------------------------------
    // Attribute / Request helpers
    // -----------------------------------------------------------------------
    int32_t Attribute::asInt(int32_t fallback) const
    {
        if (values.empty() || values.front().size() != 4)
            return fallback;
        return static_cast<int32_t>(be32(reinterpret_cast<const uint8_t *>(values.front().data())));
    }

    bool Attribute::asBool(bool fallback) const
    {
        if (values.empty() || values.front().size() != 1)
            return fallback;
        return values.front()[0] != 0;
    }

    const Attribute *Request::find(const std::string &name, uint8_t group) const
    {
        for (const auto &a : attributes)
            if (a.name == name && (group == 0 || a.group == group))
                return &a;
        return nullptr;
    }

    std::string Request::getString(const std::string &name, const std::string &fallback) const
    {
        const Attribute *a = find(name);
        return a && !a->values.empty() ? a->values.front() : fallback;
    }

    int32_t Request::getInt(const std::string &name, int32_t fallback) const
    {
        const Attribute *a = find(name);
        return a ? a->asInt(fallback) : fallback;
    }

    std::vector<std::string> Request::getStrings(const std::string &name) const
    {
        const Attribute *a = find(name);
        return a ? a->values : std::vector<std::string>();
    }

    // -----------------------------------------------------------------------
    // RequestParser
    // -----------------------------------------------------------------------
    void RequestParser::reset()
    {
        buf_.clear();
        complete_ = false;
        error_.clear();
        request_ = Request{};
    }

    size_t RequestParser::feed(const uint8_t *data, size_t len)
    {
        if (complete_ || error())
            return 0;
        const size_t prev = buf_.size();
        buf_.insert(buf_.end(), data, data + len);
        if (!tryParse())
        {
            if (!error() && buf_.size() > maxBytes_)
                error_ = "attribute section too large";
            return error() ? 0 : len;
        }
        // tryParse() leaves the number of attribute bytes in buf_.size()
        const size_t consumed = buf_.size() - prev;
        buf_.clear();
        buf_.shrink_to_fit();
        return consumed;
    }

    bool RequestParser::tryParse()
    {
        const size_t n = buf_.size();
        const uint8_t *b = buf_.data();
        if (n < 9)
            return false;

        Request req;
        req.versionMajor = b[0];
        req.versionMinor = b[1];
        req.operation = be16(b + 2);
        req.requestId = be32(b + 4);

        size_t pos = 8;
        uint8_t group = 0;
        int depth = 0; // collection nesting depth (members are skipped)
        while (pos < n)
        {
            const uint8_t tag = b[pos];
            if (tag == TAG_END)
            {
                if (depth != 0)
                {
                    error_ = "unterminated collection";
                    return false;
                }
                request_ = std::move(req);
                buf_.resize(pos + 1); // report attribute section length
                complete_ = true;
                return true;
            }
            if (tag < 0x10)
            {
                group = tag;
                ++pos;
                continue;
            }
            size_t p = pos + 1;
            if (p + 2 > n)
                return false;
            const uint16_t nameLen = be16(b + p);
            p += 2;
            if (p + nameLen + 2 > n)
                return false;
            std::string name(reinterpret_cast<const char *>(b + p), nameLen);
            p += nameLen;
            const uint16_t valueLen = be16(b + p);
            p += 2;
            if (p + valueLen > n)
                return false;
            std::string value(reinterpret_cast<const char *>(b + p), valueLen);
            p += valueLen;
            pos = p;

            if (group == 0)
            {
                error_ = "attribute outside of group";
                return false;
            }

            if (depth > 0)
            {
                if (tag == TAG_BEGIN_COLLECTION)
                    ++depth;
                else if (tag == TAG_END_COLLECTION)
                    --depth;
                continue;
            }
            if (tag == TAG_END_COLLECTION)
            {
                error_ = "unexpected end of collection";
                return false;
            }

            if (nameLen > 0)
            {
                Attribute a;
                a.group = group;
                a.valueTag = tag;
                a.name = std::move(name);
                if (tag != TAG_BEGIN_COLLECTION)
                    a.values.push_back(std::move(value));
                req.attributes.push_back(std::move(a));
            }
            else if (!req.attributes.empty() && tag != TAG_BEGIN_COLLECTION)
            {
                req.attributes.back().values.push_back(std::move(value));
            }
            if (tag == TAG_BEGIN_COLLECTION)
                depth = 1;
        }
        return false;
    }

    // -----------------------------------------------------------------------
    // Writer
    // -----------------------------------------------------------------------
    void Writer::u16(uint16_t v)
    {
        buf_.push_back(static_cast<uint8_t>(v >> 8));
        buf_.push_back(static_cast<uint8_t>(v));
    }

    void Writer::u32(uint32_t v)
    {
        buf_.push_back(static_cast<uint8_t>(v >> 24));
        buf_.push_back(static_cast<uint8_t>(v >> 16));
        buf_.push_back(static_cast<uint8_t>(v >> 8));
        buf_.push_back(static_cast<uint8_t>(v));
    }

    void Writer::raw(uint8_t tag, const std::string &name, const uint8_t *value, size_t len)
    {
        buf_.push_back(tag);
        u16(static_cast<uint16_t>(name.size()));
        buf_.insert(buf_.end(), name.begin(), name.end());
        u16(static_cast<uint16_t>(len));
        if (len)
            buf_.insert(buf_.end(), value, value + len);
    }

    void Writer::header(uint8_t major, uint8_t minor, uint16_t statusOrOp, uint32_t requestId)
    {
        buf_.push_back(major);
        buf_.push_back(minor);
        u16(statusOrOp);
        u32(requestId);
    }

    void Writer::group(uint8_t tag) { buf_.push_back(tag); }
    void Writer::end() { buf_.push_back(TAG_END); }

    void Writer::string(uint8_t tag, const std::string &name, const std::string &value)
    {
        // IPP limits most string values to 1023 octets; never exceed 16 bit lengths.
        const size_t len = value.size() > 0xFFFF ? 0xFFFF : value.size();
        raw(tag, name, reinterpret_cast<const uint8_t *>(value.data()), len);
    }

    void Writer::strings(uint8_t tag, const std::string &name, const std::vector<std::string> &values)
    {
        for (size_t i = 0; i < values.size(); ++i)
            string(tag, i == 0 ? name : std::string(), values[i]);
    }

    void Writer::integer(const std::string &name, int32_t value)
    {
        const uint8_t v[4] = {static_cast<uint8_t>(value >> 24), static_cast<uint8_t>(value >> 16),
                              static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
        raw(TAG_INTEGER, name, v, 4);
    }

    void Writer::integers(uint8_t tag, const std::string &name, const std::vector<int32_t> &values)
    {
        for (size_t i = 0; i < values.size(); ++i)
        {
            const int32_t value = values[i];
            const uint8_t v[4] = {static_cast<uint8_t>(value >> 24), static_cast<uint8_t>(value >> 16),
                                  static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
            raw(tag, i == 0 ? name : std::string(), v, 4);
        }
    }

    void Writer::enumValue(const std::string &name, int32_t value)
    {
        const uint8_t v[4] = {static_cast<uint8_t>(value >> 24), static_cast<uint8_t>(value >> 16),
                              static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
        raw(TAG_ENUM, name, v, 4);
    }

    void Writer::boolean(const std::string &name, bool value)
    {
        const uint8_t v = value ? 1 : 0;
        raw(TAG_BOOLEAN, name, &v, 1);
    }

    void Writer::range(const std::string &name, int32_t lower, int32_t upper)
    {
        uint8_t v[8];
        for (int i = 0; i < 4; ++i)
        {
            v[i] = static_cast<uint8_t>(lower >> (24 - 8 * i));
            v[4 + i] = static_cast<uint8_t>(upper >> (24 - 8 * i));
        }
        raw(TAG_RANGE, name, v, 8);
    }

    void Writer::resolution(const std::string &name, int32_t x, int32_t y, uint8_t units)
    {
        uint8_t v[9];
        for (int i = 0; i < 4; ++i)
        {
            v[i] = static_cast<uint8_t>(x >> (24 - 8 * i));
            v[4 + i] = static_cast<uint8_t>(y >> (24 - 8 * i));
        }
        v[8] = units;
        raw(TAG_RESOLUTION, name, v, 9);
    }

    void Writer::noValue(const std::string &name) { raw(TAG_NO_VALUE, name, nullptr, 0); }

    void Writer::beginCollection(const std::string &name) { raw(TAG_BEGIN_COLLECTION, name, nullptr, 0); }

    void Writer::memberName(const std::string &member)
    {
        raw(TAG_MEMBER_NAME, std::string(), reinterpret_cast<const uint8_t *>(member.data()), member.size());
    }

    void Writer::endCollection() { raw(TAG_END_COLLECTION, std::string(), nullptr, 0); }

    const char *operationName(uint16_t op)
    {
        switch (op)
        {
        case OP_PRINT_JOB:
            return "Print-Job";
        case OP_VALIDATE_JOB:
            return "Validate-Job";
        case OP_CREATE_JOB:
            return "Create-Job";
        case OP_SEND_DOCUMENT:
            return "Send-Document";
        case OP_CANCEL_JOB:
            return "Cancel-Job";
        case OP_GET_JOB_ATTRIBUTES:
            return "Get-Job-Attributes";
        case OP_GET_JOBS:
            return "Get-Jobs";
        case OP_GET_PRINTER_ATTRIBUTES:
            return "Get-Printer-Attributes";
        default:
            return "Unknown";
        }
    }

} // namespace ipp
