/**
 * @file MonoBitmap.h
 * @brief Minimal 1-bit bitmap container used as print job payload.
 *
 * Layout: rows of rowBytes() bytes, MSB first (bit 7 = left-most pixel),
 * 1 = black. This is the canonical format exchanged between the IPP server,
 * the job queue and the printer driver.
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace raster
{

    struct MonoBitmap
    {
        uint16_t width = 384;      ///< Pixels per row
        uint32_t height = 0;       ///< Number of rows
        std::vector<uint8_t> data; ///< height * rowBytes() bytes

        explicit MonoBitmap(uint16_t w = 384) : width(w) {}

        size_t rowBytes() const { return (width + 7u) / 8u; }

        const uint8_t *row(uint32_t y) const { return data.data() + static_cast<size_t>(y) * rowBytes(); }
        uint8_t *row(uint32_t y) { return data.data() + static_cast<size_t>(y) * rowBytes(); }

        /** Append one packed row (rowBytes() bytes). */
        void appendRow(const uint8_t *packed)
        {
            data.insert(data.end(), packed, packed + rowBytes());
            ++height;
        }

        /** Append @p n white rows. */
        void appendBlank(uint32_t n)
        {
            data.resize(data.size() + static_cast<size_t>(n) * rowBytes(), 0);
            height += n;
        }

        void setPixel(uint32_t x, uint32_t y, bool black)
        {
            if (x >= width || y >= height)
                return;
            uint8_t &b = row(y)[x / 8];
            const uint8_t mask = static_cast<uint8_t>(0x80u >> (x % 8));
            b = black ? static_cast<uint8_t>(b | mask) : static_cast<uint8_t>(b & ~mask);
        }

        bool pixel(uint32_t x, uint32_t y) const
        {
            if (x >= width || y >= height)
                return false;
            return (row(y)[x / 8] & (0x80u >> (x % 8))) != 0;
        }
    };

} // namespace raster
