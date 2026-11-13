#include "TextRenderer.h"

#include "MonoRasterizer.h"

namespace raster
{
    namespace
    {
        // Classic 5x7 ASCII font (0x20..0x7E), column major, bit 0 = top row.
        // This glyph set is widely distributed in the public domain.
        const uint8_t kFont5x7[95][5] = {
            {0x00, 0x00, 0x00, 0x00, 0x00}, // ' '
            {0x00, 0x00, 0x5F, 0x00, 0x00}, // !
            {0x00, 0x07, 0x00, 0x07, 0x00}, // "
            {0x14, 0x7F, 0x14, 0x7F, 0x14}, // #
            {0x24, 0x2A, 0x7F, 0x2A, 0x12}, // $
            {0x23, 0x13, 0x08, 0x64, 0x62}, // %
            {0x36, 0x49, 0x55, 0x22, 0x50}, // &
            {0x00, 0x05, 0x03, 0x00, 0x00}, // '
            {0x00, 0x1C, 0x22, 0x41, 0x00}, // (
            {0x00, 0x41, 0x22, 0x1C, 0x00}, // )
            {0x08, 0x2A, 0x1C, 0x2A, 0x08}, // *
            {0x08, 0x08, 0x3E, 0x08, 0x08}, // +
            {0x00, 0x50, 0x30, 0x00, 0x00}, // ,
            {0x08, 0x08, 0x08, 0x08, 0x08}, // -
            {0x00, 0x60, 0x60, 0x00, 0x00}, // .
            {0x20, 0x10, 0x08, 0x04, 0x02}, // /
            {0x3E, 0x51, 0x49, 0x45, 0x3E}, // 0
            {0x00, 0x42, 0x7F, 0x40, 0x00}, // 1
            {0x42, 0x61, 0x51, 0x49, 0x46}, // 2
            {0x21, 0x41, 0x45, 0x4B, 0x31}, // 3
            {0x18, 0x14, 0x12, 0x7F, 0x10}, // 4
            {0x27, 0x45, 0x45, 0x45, 0x39}, // 5
            {0x3C, 0x4A, 0x49, 0x49, 0x30}, // 6
            {0x01, 0x71, 0x09, 0x05, 0x03}, // 7
            {0x36, 0x49, 0x49, 0x49, 0x36}, // 8
            {0x06, 0x49, 0x49, 0x29, 0x1E}, // 9
            {0x00, 0x36, 0x36, 0x00, 0x00}, // :
            {0x00, 0x56, 0x36, 0x00, 0x00}, // ;
            {0x08, 0x14, 0x22, 0x41, 0x00}, // <
            {0x14, 0x14, 0x14, 0x14, 0x14}, // =
            {0x00, 0x41, 0x22, 0x14, 0x08}, // >
            {0x02, 0x01, 0x51, 0x09, 0x06}, // ?
            {0x32, 0x49, 0x79, 0x41, 0x3E}, // @
            {0x7E, 0x11, 0x11, 0x11, 0x7E}, // A
            {0x7F, 0x49, 0x49, 0x49, 0x36}, // B
            {0x3E, 0x41, 0x41, 0x41, 0x22}, // C
            {0x7F, 0x41, 0x41, 0x22, 0x1C}, // D
            {0x7F, 0x49, 0x49, 0x49, 0x41}, // E
            {0x7F, 0x09, 0x09, 0x09, 0x01}, // F
            {0x3E, 0x41, 0x49, 0x49, 0x7A}, // G
            {0x7F, 0x08, 0x08, 0x08, 0x7F}, // H
            {0x00, 0x41, 0x7F, 0x41, 0x00}, // I
            {0x20, 0x40, 0x41, 0x3F, 0x01}, // J
            {0x7F, 0x08, 0x14, 0x22, 0x41}, // K
            {0x7F, 0x40, 0x40, 0x40, 0x40}, // L
            {0x7F, 0x02, 0x0C, 0x02, 0x7F}, // M
            {0x7F, 0x04, 0x08, 0x10, 0x7F}, // N
            {0x3E, 0x41, 0x41, 0x41, 0x3E}, // O
            {0x7F, 0x09, 0x09, 0x09, 0x06}, // P
            {0x3E, 0x41, 0x51, 0x21, 0x5E}, // Q
            {0x7F, 0x09, 0x19, 0x29, 0x46}, // R
            {0x46, 0x49, 0x49, 0x49, 0x31}, // S
            {0x01, 0x01, 0x7F, 0x01, 0x01}, // T
            {0x3F, 0x40, 0x40, 0x40, 0x3F}, // U
            {0x1F, 0x20, 0x40, 0x20, 0x1F}, // V
            {0x3F, 0x40, 0x38, 0x40, 0x3F}, // W
            {0x63, 0x14, 0x08, 0x14, 0x63}, // X
            {0x07, 0x08, 0x70, 0x08, 0x07}, // Y
            {0x61, 0x51, 0x49, 0x45, 0x43}, // Z
            {0x00, 0x7F, 0x41, 0x41, 0x00}, // [
            {0x02, 0x04, 0x08, 0x10, 0x20}, // backslash
            {0x00, 0x41, 0x41, 0x7F, 0x00}, // ]
            {0x04, 0x02, 0x01, 0x02, 0x04}, // ^
            {0x40, 0x40, 0x40, 0x40, 0x40}, // _
            {0x00, 0x01, 0x02, 0x04, 0x00}, // `
            {0x20, 0x54, 0x54, 0x54, 0x78}, // a
            {0x7F, 0x48, 0x44, 0x44, 0x38}, // b
            {0x38, 0x44, 0x44, 0x44, 0x20}, // c
            {0x38, 0x44, 0x44, 0x48, 0x7F}, // d
            {0x38, 0x54, 0x54, 0x54, 0x18}, // e
            {0x08, 0x7E, 0x09, 0x01, 0x02}, // f
            {0x0C, 0x52, 0x52, 0x52, 0x3E}, // g
            {0x7F, 0x08, 0x04, 0x04, 0x78}, // h
            {0x00, 0x44, 0x7D, 0x40, 0x00}, // i
            {0x20, 0x40, 0x44, 0x3D, 0x00}, // j
            {0x7F, 0x10, 0x28, 0x44, 0x00}, // k
            {0x00, 0x41, 0x7F, 0x40, 0x00}, // l
            {0x7C, 0x04, 0x18, 0x04, 0x78}, // m
            {0x7C, 0x08, 0x04, 0x04, 0x78}, // n
            {0x38, 0x44, 0x44, 0x44, 0x38}, // o
            {0x7C, 0x14, 0x14, 0x14, 0x08}, // p
            {0x08, 0x14, 0x14, 0x18, 0x7C}, // q
            {0x7C, 0x08, 0x04, 0x04, 0x08}, // r
            {0x48, 0x54, 0x54, 0x54, 0x20}, // s
            {0x04, 0x3F, 0x44, 0x40, 0x20}, // t
            {0x3C, 0x40, 0x40, 0x20, 0x7C}, // u
            {0x1C, 0x20, 0x40, 0x20, 0x1C}, // v
            {0x3C, 0x40, 0x30, 0x40, 0x3C}, // w
            {0x44, 0x28, 0x10, 0x28, 0x44}, // x
            {0x0C, 0x50, 0x50, 0x50, 0x3C}, // y
            {0x44, 0x64, 0x54, 0x4C, 0x44}, // z
            {0x00, 0x08, 0x36, 0x41, 0x00}, // {
            {0x00, 0x00, 0x7F, 0x00, 0x00}, // |
            {0x00, 0x41, 0x36, 0x08, 0x00}, // }
            {0x08, 0x04, 0x08, 0x10, 0x08}, // ~
        };

        const uint8_t *glyph(char c)
        {
            unsigned char u = static_cast<unsigned char>(c);
            if (u < 0x20 || u > 0x7E)
                u = '?';
            return kFont5x7[u - 0x20];
        }

        /** Split text on '\n' and wrap to @p maxChars. */
        std::vector<std::string> layout(const std::string &text, size_t maxChars)
        {
            std::vector<std::string> lines;
            std::string current;
            auto flush = [&]() {
                lines.push_back(current);
                current.clear();
            };
            for (char c : text)
            {
                if (c == '\n')
                {
                    flush();
                    continue;
                }
                if (maxChars && current.size() >= maxChars)
                {
                    // Prefer breaking at the last space.
                    size_t sp = current.rfind(' ');
                    if (sp != std::string::npos && sp > 0)
                    {
                        std::string rest = current.substr(sp + 1);
                        current.resize(sp);
                        flush();
                        current = rest;
                    }
                    else
                    {
                        flush();
                    }
                }
                current.push_back(c);
            }
            if (!current.empty() || lines.empty())
                flush();
            return lines;
        }
    } // namespace

    size_t TextRenderer::charsPerLine(uint16_t width, uint8_t scale)
    {
        if (scale == 0)
            scale = 1;
        return width / (kAdvance * scale);
    }

    void TextRenderer::drawText(MonoBitmap &bmp, const std::string &text, uint8_t scale, bool center)
    {
        if (scale == 0)
            scale = 1;
        const size_t maxChars = charsPerLine(bmp.width, scale);
        for (const std::string &line : layout(text, maxChars))
        {
            const uint32_t top = bmp.height;
            bmp.appendBlank(kLineHeight * scale);
            const uint32_t textWidth = static_cast<uint32_t>(line.size()) * kAdvance * scale;
            uint32_t x0 = center && textWidth < bmp.width ? (bmp.width - textWidth) / 2 : 0;
            for (size_t i = 0; i < line.size(); ++i)
            {
                const uint8_t *g = glyph(line[i]);
                for (int col = 0; col < kGlyphWidth; ++col)
                    for (int r = 0; r < kGlyphHeight; ++r)
                        if (g[col] & (1u << r))
                            for (int sy = 0; sy < scale; ++sy)
                                for (int sx = 0; sx < scale; ++sx)
                                    bmp.setPixel(x0 + (i * kAdvance + col) * scale + sx,
                                                 top + scale /* top padding */ + r * scale + sy, true);
            }
        }
    }

    void TextRenderer::drawRule(MonoBitmap &bmp, uint32_t thickness)
    {
        const uint32_t top = bmp.height;
        bmp.appendBlank(thickness);
        for (uint32_t y = top; y < bmp.height; ++y)
            for (uint32_t x = 0; x < bmp.width; ++x)
                bmp.setPixel(x, y, true);
    }

    MonoBitmap buildIdentifyPage(const std::string &name, const std::string &mac, uint16_t width)
    {
        MonoBitmap bmp(width);
        TextRenderer::drawRule(bmp, 4);
        bmp.appendBlank(8);
        TextRenderer::drawText(bmp, "CAT PRINTER IDENTIFY", 3);
        bmp.appendBlank(8);
        if (!name.empty())
            TextRenderer::drawText(bmp, name, 2);
        if (!mac.empty())
            TextRenderer::drawText(bmp, mac, 2);
        bmp.appendBlank(8);
        TextRenderer::drawRule(bmp, 4);
        return bmp;
    }

    MonoBitmap buildTestPage(const std::vector<std::string> &lines, uint16_t width)
    {
        MonoBitmap bmp(width);
        TextRenderer::drawRule(bmp, 3);
        TextRenderer::drawText(bmp, "CATBRIDGE TEST PAGE", 3);
        TextRenderer::drawRule(bmp, 3);
        bmp.appendBlank(6);
        for (const auto &l : lines)
            TextRenderer::drawText(bmp, l, 2, false);
        bmp.appendBlank(8);

        // Horizontal gray ramp, rendered through the real dithering pipeline.
        MonoRasterizer rasterizer;
        MonoRasterizer::Options opt;
        opt.outWidth = width;
        opt.trimBlankRows = false;
        const uint32_t rampHeight = 64;
        rasterizer.beginPage(width, rampHeight, opt, [&](const uint8_t *row, size_t) { bmp.appendRow(row); });
        std::vector<uint8_t> ramp(width);
        for (uint32_t x = 0; x < width; ++x)
            ramp[x] = static_cast<uint8_t>(x * 255 / (width - 1));
        for (uint32_t y = 0; y < rampHeight; ++y)
            rasterizer.pushLine(ramp.data());
        rasterizer.endPage();
        bmp.appendBlank(8);

        // Checkerboard and vertical lines to check alignment of the print head.
        const uint32_t top = bmp.height;
        bmp.appendBlank(48);
        for (uint32_t y = 0; y < 24; ++y)
            for (uint32_t x = 0; x < width; ++x)
                bmp.setPixel(x, top + y, ((x / 8) + (y / 8)) % 2 == 0);
        for (uint32_t y = 24; y < 48; ++y)
            for (uint32_t x = 0; x < width; x += 16)
                bmp.setPixel(x, top + y, true);
        bmp.appendBlank(4);
        TextRenderer::drawRule(bmp, 3);
        return bmp;
    }

} // namespace raster
