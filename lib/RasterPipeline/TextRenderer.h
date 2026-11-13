/**
 * @file TextRenderer.h
 * @brief Renders text and diagnostic pages into 1-bit bitmaps.
 *
 * Used for the "identify" job ("CAT PRINTER IDENTIFY") and the test page.
 * Glyphs come from a classic public domain 5x7 font, scaled by an integer
 * factor so the output is readable on a 203 dpi thermal head.
 */
#pragma once

#include <string>
#include <vector>

#include "MonoBitmap.h"

namespace raster
{

    class TextRenderer
    {
    public:
        static constexpr int kGlyphWidth = 5;
        static constexpr int kGlyphHeight = 7;
        static constexpr int kAdvance = 6;     ///< Glyph width + 1 column spacing
        static constexpr int kLineHeight = 10; ///< Glyph height + 3 rows spacing

        /** Characters per line for a given width and scale. */
        static size_t charsPerLine(uint16_t width, uint8_t scale);

        /**
         * Append text to @p bmp. Lines longer than the bitmap width are wrapped.
         * @param scale  integer magnification (1 = 6x10 px per character cell)
         * @param center centre every line horizontally
         */
        static void drawText(MonoBitmap &bmp, const std::string &text, uint8_t scale, bool center = true);

        /** Append a horizontal line of @p thickness rows. */
        static void drawRule(MonoBitmap &bmp, uint32_t thickness = 2);
    };

    /** Bitmap printed by the identify action: large banner plus details. */
    MonoBitmap buildIdentifyPage(const std::string &name, const std::string &mac, uint16_t width = 384);

    /** Diagnostic test page with text, gray ramp (dithering) and patterns. */
    MonoBitmap buildTestPage(const std::vector<std::string> &lines, uint16_t width = 384);

} // namespace raster
