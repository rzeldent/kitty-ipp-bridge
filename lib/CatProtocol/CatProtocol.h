/**
 * @file CatProtocol.h
 * @brief Hardware independent packet encoders for BLE Cat Printers.
 *
 * The BLE transport (CatPrinterDriver) is deliberately ignorant of the packet
 * format. Everything that is printer-protocol specific lives behind the
 * ProtocolEncoder interface so new printer generations ("CatGFX" variants)
 * can be added without touching the transport, job queue or IPP code.
 *
 * Implemented today:
 *  - ProtocolFamily::CatGfxLegacy: the widely used "0x51 0x78" framed
 *    protocol spoken by GB01/GB02/GB03/GT01/MX05/MX06/MX08/YT01 style
 *    printers (384 dots, 203 dpi, service 0xAE30, write char 0xAE01).
 *
 * Reserved for future work (see docs/CATGFX_TODO.md):
 *  - ProtocolFamily::CatGfxV2: newer firmware (e.g. MXW01) that uses a
 *    separate data characteristic and different framing.
 *
 * Bitmap convention used throughout the bridge: 1 bit per pixel, rows packed
 * MSB first (bit 7 = left-most pixel), 1 = black. Encoders convert to the
 * printer native bit order.
 *
 * This library has no Arduino dependencies and is covered by host unit tests.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace catgfx
{

    /** Known protocol families. */
    enum class ProtocolFamily : uint8_t
    {
        CatGfxLegacy = 0, ///< 0x51 0x78 framed command protocol
        CatGfxV2 = 1,     ///< Placeholder for next generation protocol (not implemented)
    };

    /** Human readable family name. */
    const char *familyName(ProtocolFamily family);

    /** Per-job tuning parameters passed to the encoder. */
    struct PrintSettings
    {
        uint16_t energy = 0x3000; ///< Thermal energy (darkness)
        uint8_t speed = 32;       ///< Motor speed while printing bitmaps
    };

    /** Flow-control state reported by the printer through notifications. */
    enum class FlowControl : uint8_t
    {
        None,   ///< Notification unrelated to flow control
        Pause,  ///< Printer buffer full: stop sending
        Resume, ///< Printer ready for more data
    };

    /** CRC-8 (polynomial 0x07, init 0x00) used by the legacy protocol. */
    uint8_t crc8(const uint8_t *data, size_t len);

    /**
     * @brief Abstract printer protocol encoder.
     *
     * All methods append bytes to @p out. The driver sends @p out in MTU sized
     * chunks; packet boundaries do not need to align with BLE writes.
     */
    class ProtocolEncoder
    {
    public:
        virtual ~ProtocolEncoder() = default;

        virtual ProtocolFamily family() const = 0;

        /** Native print width in pixels (e.g. 384). */
        virtual uint16_t printWidth() const = 0;

        /** Commands sent once after connecting, before any bitmap data. */
        virtual void beginJob(std::vector<uint8_t> &out, const PrintSettings &settings) = 0;

        /**
         * Encode one bitmap row.
         * @param row       packed MSB-first row, 1 = black
         * @param rowBytes  bytes in @p row (printWidth()/8 expected, shorter rows are padded)
         */
        virtual void bitmapRow(std::vector<uint8_t> &out, const uint8_t *row, size_t rowBytes) = 0;

        /** Advance the paper by @p lines dot rows (8 lines = 1 mm at 203 dpi). */
        virtual void feed(std::vector<uint8_t> &out, uint16_t lines) = 0;

        /** Commands sent after the last row (end of job). */
        virtual void endJob(std::vector<uint8_t> &out) = 0;

        /** Request a status notification from the printer (optional). */
        virtual void requestStatus(std::vector<uint8_t> &out) = 0;

        /** Interpret a notification received from the printer. */
        virtual FlowControl parseNotification(const uint8_t *data, size_t len) const = 0;
    };

    /**
     * @brief Encoder for the classic "0x51 0x78" Cat Printer protocol.
     *
     * Packet layout:
     * @verbatim
     *   0x51 0x78 CMD 0x00 LEN_LO LEN_HI DATA[LEN] CRC8(DATA) 0xFF
     * @endverbatim
     */
    class LegacyEncoder : public ProtocolEncoder
    {
    public:
        // Command bytes
        static constexpr uint8_t CMD_RETRACT = 0xA0;
        static constexpr uint8_t CMD_FEED = 0xA1;
        static constexpr uint8_t CMD_PRINT_ROW = 0xA2;
        static constexpr uint8_t CMD_GET_STATE = 0xA3;
        static constexpr uint8_t CMD_QUALITY = 0xA4;
        static constexpr uint8_t CMD_LATTICE = 0xA6;
        static constexpr uint8_t CMD_FLOW_CONTROL = 0xAE;
        static constexpr uint8_t CMD_ENERGY = 0xAF;
        static constexpr uint8_t CMD_SPEED = 0xBD;
        static constexpr uint8_t CMD_DRAW_MODE = 0xBE;

        explicit LegacyEncoder(uint16_t widthPx = 384) : width_(widthPx) {}

        ProtocolFamily family() const override { return ProtocolFamily::CatGfxLegacy; }
        uint16_t printWidth() const override { return width_; }

        void beginJob(std::vector<uint8_t> &out, const PrintSettings &settings) override;
        void bitmapRow(std::vector<uint8_t> &out, const uint8_t *row, size_t rowBytes) override;
        void feed(std::vector<uint8_t> &out, uint16_t lines) override;
        void endJob(std::vector<uint8_t> &out) override;
        void requestStatus(std::vector<uint8_t> &out) override;
        FlowControl parseNotification(const uint8_t *data, size_t len) const override;

        /** Append one framed packet. Exposed for raw/diagnostic use and tests. */
        static void packet(std::vector<uint8_t> &out, uint8_t cmd, const uint8_t *data, size_t len);

    private:
        uint16_t width_;
    };

    /**
     * Create an encoder for a protocol family.
     * @return nullptr if the family is not implemented yet.
     */
    std::unique_ptr<ProtocolEncoder> createEncoder(ProtocolFamily family, uint16_t widthPx = 384);

} // namespace catgfx
