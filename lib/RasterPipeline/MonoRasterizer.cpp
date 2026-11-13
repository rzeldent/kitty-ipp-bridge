#include "MonoRasterizer.h"

#include <algorithm>
#include <cstring>

namespace raster
{

    void MonoRasterizer::beginPage(uint32_t srcWidth, uint32_t srcHeight, const Options &options, RowFn sink)
    {
        opt_ = options;
        if (opt_.outWidth == 0)
            opt_.outWidth = 384;
        sink_ = std::move(sink);
        srcWidth_ = srcWidth ? srcWidth : 1;
        srcHeight_ = srcHeight;
        const uint32_t w = opt_.outWidth;
        dstHeight_ = static_cast<uint32_t>(std::max<uint64_t>(1, static_cast<uint64_t>(srcHeight) * w / srcWidth_));
        srcY_ = dstY_ = emitted_ = accLines_ = pendingBlank_ = 0;
        seenInk_ = false;

        acc_.assign(w, 0);
        scaled_.assign(w, 0);
        errCur_.assign(w + 2, 0);
        errNext_.assign(w + 2, 0);
        packed_.assign((w + 7) / 8, 0);

        if (srcWidth_ >= w)
        {
            // Box filter: precompute how many source pixels fall into each output column.
            hcount_.assign(w, 0);
            for (uint32_t x = 0; x < srcWidth_; ++x)
                ++hcount_[static_cast<uint64_t>(x) * w / srcWidth_];
            hsum_.assign(w, 0);
        }
        else
        {
            hcount_.clear();
            hsum_.clear();
        }
    }

    void MonoRasterizer::pushLine(const uint8_t *gray)
    {
        if (srcY_ >= srcHeight_)
            return;
        const uint32_t w = opt_.outWidth;

        if (srcWidth_ >= w)
        {
            std::fill(hsum_.begin(), hsum_.end(), 0);
            for (uint32_t x = 0; x < srcWidth_; ++x)
                hsum_[static_cast<uint64_t>(x) * w / srcWidth_] += gray[x];
            for (uint32_t d = 0; d < w; ++d)
                acc_[d] += hcount_[d] ? hsum_[d] / hcount_[d] : 255;
        }
        else
        {
            for (uint32_t d = 0; d < w; ++d)
                acc_[d] += gray[static_cast<uint64_t>(d) * srcWidth_ / w];
        }
        ++accLines_;

        const uint32_t outEnd = static_cast<uint32_t>(static_cast<uint64_t>(srcY_ + 1) * dstHeight_ / srcHeight_);
        if (outEnd > dstY_)
        {
            for (uint32_t d = 0; d < w; ++d)
                scaled_[d] = static_cast<uint16_t>(acc_[d] / accLines_);
            while (dstY_ < outEnd)
            {
                ditherAndOutput(scaled_.data());
                ++dstY_;
            }
            std::fill(acc_.begin(), acc_.end(), 0);
            accLines_ = 0;
        }
        ++srcY_;
    }

    void MonoRasterizer::endPage()
    {
        // A truncated page may leave accumulated lines behind: flush them.
        if (accLines_ > 0 && dstY_ < dstHeight_)
        {
            for (uint32_t d = 0; d < opt_.outWidth; ++d)
                scaled_[d] = static_cast<uint16_t>(acc_[d] / accLines_);
            ditherAndOutput(scaled_.data());
            ++dstY_;
            accLines_ = 0;
        }
        // Trailing blank rows are simply never delivered when trimming.
        pendingBlank_ = 0;
    }

    void MonoRasterizer::ditherAndOutput(const uint16_t *row)
    {
        const int w = static_cast<int>(opt_.outWidth);
        const bool ltr = (dstY_ & 1u) == 0; // serpentine scan reduces directional artefacts
        std::fill(errNext_.begin(), errNext_.end(), 0);
        std::fill(packed_.begin(), packed_.end(), 0);
        bool blank = true;

        for (int i = 0; i < w; ++i)
        {
            const int x = ltr ? i : w - 1 - i;
            int v = static_cast<int>(row[x]);
            if (opt_.dither)
                v += errCur_[x + 1];
            const int out = v < opt_.threshold ? 0 : 255;
            if (out == 0)
            {
                packed_[x >> 3] |= static_cast<uint8_t>(0x80u >> (x & 7));
                blank = false;
            }
            if (opt_.dither)
            {
                const int e = v - out;
                const int fwd = ltr ? x + 1 : x - 1;
                const int back = ltr ? x - 1 : x + 1;
                // Indices are offset by one guard pixel on each side.
                errCur_[fwd + 1] = static_cast<int16_t>(errCur_[fwd + 1] + e * 7 / 16);
                errNext_[back + 1] = static_cast<int16_t>(errNext_[back + 1] + e * 3 / 16);
                errNext_[x + 1] = static_cast<int16_t>(errNext_[x + 1] + e * 5 / 16);
                errNext_[fwd + 1] = static_cast<int16_t>(errNext_[fwd + 1] + e / 16);
            }
        }
        errCur_.swap(errNext_);
        deliver(packed_.data(), blank);
    }

    void MonoRasterizer::deliver(const uint8_t *packed, bool blank)
    {
        const size_t rowBytes = packed_.size();
        if (!opt_.trimBlankRows)
        {
            if (sink_)
                sink_(packed, rowBytes);
            ++emitted_;
            return;
        }
        if (blank)
        {
            if (seenInk_)
                ++pendingBlank_; // may be interior white space: hold until we know
            return;              // leading blank rows are dropped
        }
        seenInk_ = true;
        if (pendingBlank_)
        {
            std::vector<uint8_t> zero(rowBytes, 0);
            for (; pendingBlank_ > 0; --pendingBlank_)
            {
                if (sink_)
                    sink_(zero.data(), rowBytes);
                ++emitted_;
            }
        }
        if (sink_)
            sink_(packed, rowBytes);
        ++emitted_;
    }

} // namespace raster
