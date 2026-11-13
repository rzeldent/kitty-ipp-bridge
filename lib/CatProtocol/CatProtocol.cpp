#include "CatProtocol.h"

namespace catgfx
{

    const char *familyName(ProtocolFamily family)
    {
        switch (family)
        {
        case ProtocolFamily::CatGfxLegacy:
            return "catgfx-legacy";
        case ProtocolFamily::CatGfxV2:
            return "catgfx-v2";
        }
        return "unknown";
    }

    uint8_t crc8(const uint8_t *data, size_t len)
    {
        uint8_t crc = 0;
        for (size_t i = 0; i < len; ++i)
        {
            crc ^= data[i];
            for (int b = 0; b < 8; ++b)
                crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x07) : static_cast<uint8_t>(crc << 1);
        }
        return crc;
    }

    // -----------------------------------------------------------------------
    // LegacyEncoder
    // -----------------------------------------------------------------------
    void LegacyEncoder::packet(std::vector<uint8_t> &out, uint8_t cmd, const uint8_t *data, size_t len)
    {
        out.reserve(out.size() + len + 8);
        out.push_back(0x51);
        out.push_back(0x78);
        out.push_back(cmd);
        out.push_back(0x00);
        out.push_back(static_cast<uint8_t>(len & 0xFF));
        out.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        if (len)
            out.insert(out.end(), data, data + len);
        out.push_back(crc8(data, len));
        out.push_back(0xFF);
    }

    void LegacyEncoder::beginJob(std::vector<uint8_t> &out, const PrintSettings &settings)
    {
        // Quality: 0x33 = 200 dpi "normal" quality.
        const uint8_t quality = 0x33;
        packet(out, CMD_QUALITY, &quality, 1);

        // Lattice start marker (fixed magic sequence used by the vendor app).
        static const uint8_t latticeStart[] = {0xAA, 0x55, 0x17, 0x38, 0x44, 0x5F, 0x5F, 0x5F, 0x44, 0x38, 0x2C};
        packet(out, CMD_LATTICE, latticeStart, sizeof(latticeStart));

        const uint8_t energy[] = {static_cast<uint8_t>(settings.energy & 0xFF),
                                  static_cast<uint8_t>(settings.energy >> 8)};
        packet(out, CMD_ENERGY, energy, sizeof(energy));

        const uint8_t speed = settings.speed;
        packet(out, CMD_SPEED, &speed, 1);

        // Draw mode: 0x00 = image (bitmap), 0x01 = text.
        const uint8_t mode = 0x00;
        packet(out, CMD_DRAW_MODE, &mode, 1);
    }

    void LegacyEncoder::bitmapRow(std::vector<uint8_t> &out, const uint8_t *row, size_t rowBytes)
    {
        // The printer expects LSB-first bit order (bit 0 = left-most pixel).
        uint8_t native[96];
        const size_t nativeBytes = width_ / 8 > sizeof(native) ? sizeof(native) : width_ / 8;
        for (size_t i = 0; i < nativeBytes; ++i)
        {
            uint8_t b = i < rowBytes ? row[i] : 0;
            b = static_cast<uint8_t>((b & 0xF0) >> 4 | (b & 0x0F) << 4);
            b = static_cast<uint8_t>((b & 0xCC) >> 2 | (b & 0x33) << 2);
            b = static_cast<uint8_t>((b & 0xAA) >> 1 | (b & 0x55) << 1);
            native[i] = b;
        }
        packet(out, CMD_PRINT_ROW, native, nativeBytes);
    }

    void LegacyEncoder::feed(std::vector<uint8_t> &out, uint16_t lines)
    {
        // The printer accepts at most 0xFF lines per packet reliably; split larger feeds.
        while (lines)
        {
            uint16_t n = lines > 0xFF ? 0xFF : lines;
            const uint8_t data[] = {static_cast<uint8_t>(n & 0xFF), static_cast<uint8_t>(n >> 8)};
            packet(out, CMD_FEED, data, sizeof(data));
            lines = static_cast<uint16_t>(lines - n);
        }
    }

    void LegacyEncoder::endJob(std::vector<uint8_t> &out)
    {
        static const uint8_t latticeEnd[] = {0xAA, 0x55, 0x17, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x17};
        packet(out, CMD_LATTICE, latticeEnd, sizeof(latticeEnd));
        requestStatus(out);
    }

    void LegacyEncoder::requestStatus(std::vector<uint8_t> &out)
    {
        const uint8_t zero = 0x00;
        packet(out, CMD_GET_STATE, &zero, 1);
    }

    FlowControl LegacyEncoder::parseNotification(const uint8_t *data, size_t len) const
    {
        // Flow control notification: 51 78 AE 01 01 00 <state> <crc> FF
        //   state 0x10 = buffer full (pause), 0x00 = ready (resume)
        if (len < 9 || data[0] != 0x51 || data[1] != 0x78 || data[2] != CMD_FLOW_CONTROL)
            return FlowControl::None;
        return data[6] == 0x10 ? FlowControl::Pause : FlowControl::Resume;
    }

    std::unique_ptr<ProtocolEncoder> createEncoder(ProtocolFamily family, uint16_t widthPx)
    {
        switch (family)
        {
        case ProtocolFamily::CatGfxLegacy:
            return std::unique_ptr<ProtocolEncoder>(new LegacyEncoder(widthPx));
        case ProtocolFamily::CatGfxV2:
            // TODO(catgfx): implement next generation protocol, see docs/CATGFX_TODO.md
            return nullptr;
        }
        return nullptr;
    }

} // namespace catgfx
