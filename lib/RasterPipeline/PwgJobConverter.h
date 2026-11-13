/**
 * @file PwgJobConverter.h
 * @brief Streaming conversion of a PWG raster document into a printer bitmap.
 *
 * Glues the raster pipeline together:
 *
 *   PWG raster bytes ─► PwgRasterDecoder ─► toGray() ─► MonoRasterizer ─► MonoBitmap
 *                       (one line buffer)   (one line)  (scale to 384 px,
 *                                                        Floyd–Steinberg,
 *                                                        1-bit pack)
 *
 * Input is consumed in arbitrary chunks straight from the network; only one
 * source line and a few scaled rows are buffered at any time. The output is
 * the 1-bit printer bitmap (48 bytes per row), which is ~24x smaller than an
 * 8-bit grayscale page at the same width. A byte budget protects the heap.
 *
 * Pure C++ (no Arduino dependencies) so it can be unit tested on the host.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "MonoBitmap.h"
#include "MonoRasterizer.h"
#include "PwgRasterDecoder.h"

namespace raster
{

    class PwgJobConverter
    {
    public:
        struct Options
        {
            uint16_t width = 384;          ///< Printer width in dots
            bool trimBlankRows = true;     ///< Remove leading/trailing white rows of every page
            bool dither = true;            ///< Floyd–Steinberg (false = threshold)
            uint32_t pageGapRows = 24;     ///< Blank rows between pages (3 mm)
            size_t maxOutputBytes = 160 * 1024; ///< Output budget (rows * 48 bytes)
            uint32_t maxPages = 50;        ///< Upper bound on pages per job
        };

        explicit PwgJobConverter(const Options &options);

        /** Feed document bytes. @return false once the document is invalid or over budget. */
        bool feed(const uint8_t *data, size_t len);

        /** Signal end of document. @return true if at least one complete page was converted. */
        bool finish();

        /** Resulting bitmap (all pages stacked). */
        MonoBitmap &bitmap() { return bitmap_; }

        uint32_t pages() const { return decoder_.pagesCompleted(); }
        bool failed() const { return !error_.empty() || decoder_.failed(); }
        /** Human readable error (empty when OK). */
        std::string error() const { return error_.empty() ? decoder_.error() : error_; }
        /** True when the failure is due to the byte budget (maps to "request entity too large"). */
        bool overBudget() const { return overBudget_; }

    private:
        Options opt_;
        PwgRasterDecoder decoder_;
        MonoRasterizer rasterizer_;
        MonoBitmap bitmap_;
        PwgPageHeader header_;
        std::vector<uint8_t> gray_;
        std::string error_;
        bool overBudget_ = false;
    };

} // namespace raster
