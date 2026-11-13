/**
 * @file PwgRasterDecoder.h
 * @brief Streaming decoder for PWG Raster (PWG 5102.4, "application/pwg-raster").
 *
 * The decoder is a push parser: feed() it arbitrary sized chunks straight
 * from the network and it invokes callbacks for every page header and every
 * decoded raster line. Only one uncompressed line is ever held in memory, so
 * multi-megabyte pages are decoded with a few kilobytes of RAM.
 *
 * Stream layout:
 * @verbatim
 *   "RaS2" | page header (1796 bytes) | compressed lines | page header | ...
 * @endverbatim
 *
 * Line compression: a line-repeat byte (n+1 copies of the line) followed by
 * packets: 0..127 = repeat next pixel (n+1) times, 128..255 = (257-n)
 * literal pixels follow. A "pixel" is ceil(bitsPerPixel/8) bytes.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace raster
{

    /** Colour spaces we understand (values from the CUPS/PWG header). */
    enum PwgColorSpace : uint32_t
    {
        CSPACE_W = 0,         ///< Luminance (DeviceGray)
        CSPACE_RGB = 1,       ///< DeviceRGB
        CSPACE_K = 3,         ///< Black (1 = ink)
        CSPACE_SW = 18,       ///< sGray
        CSPACE_SRGB = 19,     ///< sRGB
        CSPACE_ADOBE_RGB = 20 ///< Adobe RGB
    };

    /** Subset of the 1796 byte page header relevant for printing. */
    struct PwgPageHeader
    {
        uint32_t hwResolutionX = 0;
        uint32_t hwResolutionY = 0;
        uint32_t width = 0;        ///< cupsWidth (pixels)
        uint32_t height = 0;       ///< cupsHeight (lines)
        uint32_t bitsPerColor = 0; ///< cupsBitsPerColor
        uint32_t bitsPerPixel = 0; ///< cupsBitsPerPixel
        uint32_t bytesPerLine = 0; ///< cupsBytesPerLine
        uint32_t colorOrder = 0;   ///< cupsColorOrder (0 = chunky)
        uint32_t colorSpace = 0;   ///< cupsColorSpace
        uint32_t numColors = 0;    ///< cupsNumColors
        uint32_t totalPageCount = 0;
    };

    /** True if the header describes a format convertible by toGray(). */
    bool isSupported(const PwgPageHeader &h, std::string *reason = nullptr);

    /**
     * Convert one decoded line to 8-bit luminance (0 = black, 255 = white).
     * @param gray output buffer of h.width bytes
     */
    void toGray(const PwgPageHeader &h, const uint8_t *line, uint8_t *gray);

    class PwgRasterDecoder
    {
    public:
        static constexpr size_t kHeaderSize = 1796;

        /** Return false to abort decoding (e.g. unsupported page). */
        using PageStartFn = std::function<bool(const PwgPageHeader &header, uint32_t pageIndex)>;
        /** Called once per line (repeated lines are delivered repeatedly). */
        using LineFn = std::function<void(const uint8_t *line, uint32_t y)>;
        using PageEndFn = std::function<void(uint32_t pageIndex)>;

        PwgRasterDecoder();

        void onPageStart(PageStartFn fn) { pageStart_ = std::move(fn); }
        void onLine(LineFn fn) { line_ = std::move(fn); }
        void onPageEnd(PageEndFn fn) { pageEnd_ = std::move(fn); }

        /** Limits protecting the device from absurd headers. */
        void setLimits(uint32_t maxWidth, uint32_t maxHeight)
        {
            maxWidth_ = maxWidth;
            maxHeight_ = maxHeight;
        }

        /** Feed a chunk of the stream. Returns false once an error occurred. */
        bool feed(const uint8_t *data, size_t len);

        /** Call at end of stream. Returns true if at least one page decoded completely. */
        bool finish();

        bool failed() const { return state_ == State::Error; }
        const std::string &error() const { return error_; }
        uint32_t pagesCompleted() const { return pages_; }

        /** Reset to the initial state (expecting the sync word). */
        void reset();

    private:
        enum class State : uint8_t
        {
            Sync,
            Header,
            LineRepeat,
            Control,
            RepeatPixel,
            Literal,
            Error
        };

        bool fail(const std::string &message);
        bool parseHeader();
        void writeRepeat(uint32_t count);
        bool lineComplete();

        State state_ = State::Sync;
        std::string error_;
        std::vector<uint8_t> header_;
        std::vector<uint8_t> line_buf_;
        PwgPageHeader page_;
        uint32_t unitBytes_ = 1;
        uint32_t lineRepeat_ = 0;
        uint32_t linePos_ = 0;
        uint32_t y_ = 0;
        uint32_t runCount_ = 0;
        uint32_t literalRemaining_ = 0;
        uint8_t pixel_[8] = {};
        uint32_t pixelPos_ = 0;
        uint32_t pages_ = 0;
        uint32_t syncPos_ = 0;
        uint32_t maxWidth_ = 10000;
        uint32_t maxHeight_ = 100000;

        PageStartFn pageStart_;
        LineFn line_;
        PageEndFn pageEnd_;
    };

} // namespace raster
