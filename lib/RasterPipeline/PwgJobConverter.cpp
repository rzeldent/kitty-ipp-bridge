/**
 * @file PwgJobConverter.cpp
 * @brief Streaming conversion of a PWG raster document into a printer bitmap.
 */
#include "PwgJobConverter.h"

namespace raster
{

    PwgJobConverter::PwgJobConverter(const Options &options) : opt_(options), bitmap_(options.width)
    {
        // Printer pages are narrow; refuse absurd dimensions early (1200 dpi A3 fits).
        decoder_.setLimits(16000, 60000);

        decoder_.onPageStart([this](const PwgPageHeader &h, uint32_t index) {
            std::string reason;
            if (!isSupported(h, &reason))
            {
                error_ = "unsupported raster: " + reason;
                return false;
            }
            if (index >= opt_.maxPages)
            {
                error_ = "too many pages";
                return false;
            }
            header_ = h;
            gray_.assign(h.width, 0xFF);
            if (index > 0 && bitmap_.height > 0)
                bitmap_.appendBlank(opt_.pageGapRows);

            MonoRasterizer::Options ro;
            ro.outWidth = opt_.width;
            ro.trimBlankRows = opt_.trimBlankRows;
            ro.dither = opt_.dither;
            rasterizer_.beginPage(h.width, h.height, ro, [this](const uint8_t *row, size_t) {
                if (overBudget_)
                    return;
                if ((static_cast<size_t>(bitmap_.height) + 1) * bitmap_.rowBytes() > opt_.maxOutputBytes)
                {
                    overBudget_ = true;
                    error_ = "document exceeds memory budget";
                    return;
                }
                bitmap_.appendRow(row);
            });
            return true;
        });

        decoder_.onLine([this](const uint8_t *line, uint32_t) {
            if (overBudget_)
                return;
            toGray(header_, line, gray_.data());
            rasterizer_.pushLine(gray_.data());
        });

        decoder_.onPageEnd([this](uint32_t) { rasterizer_.endPage(); });
    }

    bool PwgJobConverter::feed(const uint8_t *data, size_t len)
    {
        if (failed())
            return false;
        if (!decoder_.feed(data, len))
        {
            if (error_.empty())
                error_ = decoder_.error();
            return false;
        }
        return !overBudget_;
    }

    bool PwgJobConverter::finish()
    {
        if (failed())
            return false;
        if (!decoder_.finish())
        {
            error_ = decoder_.error().empty() ? "truncated document" : decoder_.error();
            return false;
        }
        if (decoder_.pagesCompleted() == 0)
        {
            error_ = "document contains no pages";
            return false;
        }
        return true;
    }

} // namespace raster
