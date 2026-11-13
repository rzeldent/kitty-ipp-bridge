#include "PwgRasterDecoder.h"

#include <algorithm>
#include <cstring>

namespace raster
{
    namespace
    {
        uint32_t be32(const uint8_t *p)
        {
            return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
                   (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
        }

        bool isGraySpace(uint32_t cs) { return cs == CSPACE_W || cs == CSPACE_SW || cs == CSPACE_K; }
        bool isRgbSpace(uint32_t cs) { return cs == CSPACE_RGB || cs == CSPACE_SRGB || cs == CSPACE_ADOBE_RGB; }
    } // namespace

    bool isSupported(const PwgPageHeader &h, std::string *reason)
    {
        auto bad = [&](const char *msg) {
            if (reason)
                *reason = msg;
            return false;
        };
        if (h.width == 0 || h.height == 0)
            return bad("empty page");
        if (h.colorOrder != 0)
            return bad("only chunky color order is supported");
        if (isGraySpace(h.colorSpace))
        {
            if (!(h.bitsPerColor == 1 || h.bitsPerColor == 8 || h.bitsPerColor == 16) || h.bitsPerPixel != h.bitsPerColor)
                return bad("unsupported gray bit depth");
        }
        else if (isRgbSpace(h.colorSpace))
        {
            if (!(h.bitsPerColor == 8 || h.bitsPerColor == 16) || h.bitsPerPixel != h.bitsPerColor * 3)
                return bad("unsupported RGB bit depth");
        }
        else
        {
            return bad("unsupported color space");
        }
        const uint64_t expected = (static_cast<uint64_t>(h.width) * h.bitsPerPixel + 7) / 8;
        if (h.bytesPerLine != expected)
            return bad("inconsistent bytes per line");
        return true;
    }

    void toGray(const PwgPageHeader &h, const uint8_t *line, uint8_t *gray)
    {
        const uint32_t w = h.width;
        if (isGraySpace(h.colorSpace))
        {
            const bool inkIsOne = h.colorSpace == CSPACE_K; // K: 1/255 = black; W/sW: 255 = white
            if (h.bitsPerColor == 1)
            {
                for (uint32_t x = 0; x < w; ++x)
                {
                    const bool bit = (line[x >> 3] & (0x80 >> (x & 7))) != 0;
                    gray[x] = (bit != inkIsOne) ? 255 : 0;
                }
            }
            else
            {
                const uint32_t step = h.bitsPerColor / 8; // 1 or 2 (big endian, use MSB)
                for (uint32_t x = 0; x < w; ++x)
                {
                    const uint8_t v = line[x * step];
                    gray[x] = inkIsOne ? static_cast<uint8_t>(255 - v) : v;
                }
            }
        }
        else
        {
            const uint32_t step = h.bitsPerColor / 8; // bytes per component
            const uint32_t px = step * 3;
            for (uint32_t x = 0; x < w; ++x)
            {
                const uint8_t *p = line + x * px;
                const uint32_t r = p[0], g = p[step], b = p[2 * step];
                // ITU-R BT.601 luma, fixed point
                gray[x] = static_cast<uint8_t>((r * 77 + g * 150 + b * 29) >> 8);
            }
        }
    }

    PwgRasterDecoder::PwgRasterDecoder() { reset(); }

    void PwgRasterDecoder::reset()
    {
        state_ = State::Sync;
        error_.clear();
        header_.clear();
        header_.reserve(kHeaderSize);
        line_buf_.clear();
        page_ = PwgPageHeader{};
        unitBytes_ = 1;
        lineRepeat_ = linePos_ = y_ = runCount_ = literalRemaining_ = pixelPos_ = pages_ = syncPos_ = 0;
    }

    bool PwgRasterDecoder::fail(const std::string &message)
    {
        state_ = State::Error;
        error_ = message;
        return false;
    }

    bool PwgRasterDecoder::parseHeader()
    {
        const uint8_t *h = header_.data();
        PwgPageHeader p;
        p.hwResolutionX = be32(h + 276);
        p.hwResolutionY = be32(h + 280);
        p.width = be32(h + 372);
        p.height = be32(h + 376);
        p.bitsPerColor = be32(h + 384);
        p.bitsPerPixel = be32(h + 388);
        p.bytesPerLine = be32(h + 392);
        p.colorOrder = be32(h + 396);
        p.colorSpace = be32(h + 400);
        p.numColors = be32(h + 420);
        p.totalPageCount = be32(h + 452);

        if (p.width == 0 || p.height == 0 || p.width > maxWidth_ || p.height > maxHeight_)
            return fail("page dimensions out of range");
        if (p.bitsPerPixel == 0 || p.bitsPerPixel > 64)
            return fail("invalid bits per pixel");
        const uint64_t expected = (static_cast<uint64_t>(p.width) * p.bitsPerPixel + 7) / 8;
        if (p.bytesPerLine != expected)
            return fail("inconsistent bytes per line");

        page_ = p;
        unitBytes_ = p.bitsPerPixel < 8 ? 1 : p.bitsPerPixel / 8;
        if (unitBytes_ > sizeof(pixel_))
            return fail("pixel too large");
        line_buf_.assign(p.bytesPerLine, 0);
        y_ = 0;

        if (pageStart_ && !pageStart_(page_, pages_))
            return fail("page rejected");
        state_ = State::LineRepeat;
        return true;
    }

    void PwgRasterDecoder::writeRepeat(uint32_t count)
    {
        const uint32_t total = page_.bytesPerLine;
        for (uint32_t i = 0; i < count && linePos_ < total; ++i)
        {
            const uint32_t n = std::min(unitBytes_, total - linePos_);
            memcpy(&line_buf_[linePos_], pixel_, n);
            linePos_ += n;
        }
    }

    bool PwgRasterDecoder::lineComplete()
    {
        // Deliver the line (plus repeats), then advance to next line or page.
        const uint32_t copies = std::min(lineRepeat_, page_.height - y_);
        for (uint32_t i = 0; i < copies; ++i)
        {
            if (line_)
                line_(line_buf_.data(), y_);
            ++y_;
        }
        if (y_ >= page_.height)
        {
            if (pageEnd_)
                pageEnd_(pages_);
            ++pages_;
            header_.clear();
            state_ = State::Header;
        }
        else
        {
            state_ = State::LineRepeat;
        }
        return true;
    }

    bool PwgRasterDecoder::feed(const uint8_t *data, size_t len)
    {
        size_t i = 0;
        while (i < len)
        {
            switch (state_)
            {
            case State::Error:
                return false;

            case State::Sync:
            {
                static const char kSync[4] = {'R', 'a', 'S', '2'};
                if (data[i++] != static_cast<uint8_t>(kSync[syncPos_]))
                    return fail("missing PWG sync word");
                if (++syncPos_ == 4)
                {
                    header_.clear();
                    state_ = State::Header;
                }
                break;
            }

            case State::Header:
            {
                const size_t need = kHeaderSize - header_.size();
                const size_t take = std::min(need, len - i);
                header_.insert(header_.end(), data + i, data + i + take);
                i += take;
                if (header_.size() == kHeaderSize && !parseHeader())
                    return false;
                break;
            }

            case State::LineRepeat:
                lineRepeat_ = static_cast<uint32_t>(data[i++]) + 1;
                linePos_ = 0;
                state_ = State::Control;
                break;

            case State::Control:
            {
                const uint8_t c = data[i++];
                if (c < 128)
                {
                    runCount_ = static_cast<uint32_t>(c) + 1;
                    pixelPos_ = 0;
                    state_ = State::RepeatPixel;
                }
                else
                {
                    literalRemaining_ = (257u - c) * unitBytes_;
                    state_ = State::Literal;
                }
                break;
            }

            case State::RepeatPixel:
            {
                pixel_[pixelPos_++] = data[i++];
                if (pixelPos_ == unitBytes_)
                {
                    writeRepeat(runCount_);
                    if (linePos_ >= page_.bytesPerLine)
                        lineComplete();
                    else
                        state_ = State::Control;
                }
                break;
            }

            case State::Literal:
            {
                const uint32_t room = page_.bytesPerLine - linePos_;
                const size_t take = std::min<size_t>(std::min<size_t>(literalRemaining_, len - i), room);
                if (take == 0)
                    return fail("literal run exceeds line");
                memcpy(&line_buf_[linePos_], data + i, take);
                linePos_ += static_cast<uint32_t>(take);
                literalRemaining_ -= static_cast<uint32_t>(take);
                i += take;
                if (literalRemaining_ == 0)
                {
                    if (linePos_ >= page_.bytesPerLine)
                        lineComplete();
                    else
                        state_ = State::Control;
                }
                else if (linePos_ >= page_.bytesPerLine)
                {
                    return fail("literal run exceeds line");
                }
                break;
            }
            }
        }
        return state_ != State::Error;
    }

    bool PwgRasterDecoder::finish()
    {
        if (state_ == State::Error)
            return false;
        if (state_ == State::Header && header_.empty() && pages_ > 0)
            return true;
        if (pages_ == 0)
            return fail(state_ == State::Sync ? "empty document" : "truncated document");
        return fail("truncated page");
    }

} // namespace raster
