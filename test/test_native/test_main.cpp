/**
 * Host unit tests for the hardware independent libraries:
 *   - CatProtocol    (packet framing, CRC, bit order, flow control)
 *   - IppProtocol    (request parsing, response encoding)
 *   - RasterPipeline (PWG decoding, scaling/dithering, text rendering)
 *
 * Run with: pio test -e native
 */
#include <unity.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "CatProtocol.h"
#include "IppMessage.h"
#include "MonoBitmap.h"
#include "MonoRasterizer.h"
#include "PwgRasterDecoder.h"
#include "TextRenderer.h"

void setUp() {}
void tearDown() {}

static std::string hex(const std::vector<uint8_t> &v)
{
    static const char *d = "0123456789abcdef";
    std::string s;
    for (uint8_t b : v)
    {
        s.push_back(d[b >> 4]);
        s.push_back(d[b & 15]);
    }
    return s;
}

// ---------------------------------------------------------------------------
// CatProtocol
// ---------------------------------------------------------------------------
static void test_crc8_known_packets()
{
    std::vector<uint8_t> out;
    const uint8_t quality = 0x33;
    catgfx::LegacyEncoder::packet(out, catgfx::LegacyEncoder::CMD_QUALITY, &quality, 1);
    TEST_ASSERT_EQUAL_STRING("5178a40001003399ff", hex(out).c_str());

    out.clear();
    const uint8_t zero = 0;
    catgfx::LegacyEncoder::packet(out, catgfx::LegacyEncoder::CMD_GET_STATE, &zero, 1);
    TEST_ASSERT_EQUAL_STRING("5178a30001000000ff", hex(out).c_str());
}

static void test_lattice_start_packet()
{
    catgfx::LegacyEncoder enc;
    std::vector<uint8_t> out;
    catgfx::PrintSettings s;
    enc.beginJob(out, s);
    const std::string h = hex(out);
    TEST_ASSERT_TRUE(h.find("5178a6000b00aa551738445f5f5f44382ca1ff") != std::string::npos);
}

static void test_bitmap_row_bit_order()
{
    catgfx::LegacyEncoder enc(384);
    std::vector<uint8_t> row(48, 0);
    row[0] = 0x80;  // left-most pixel black (MSB first)
    row[47] = 0x01; // right-most pixel black
    std::vector<uint8_t> out;
    enc.bitmapRow(out, row.data(), row.size());
    TEST_ASSERT_EQUAL(48 + 8, out.size());
    TEST_ASSERT_EQUAL_HEX8(0xA2, out[2]);
    TEST_ASSERT_EQUAL_HEX8(48, out[4]);
    TEST_ASSERT_EQUAL_HEX8(0x01, out[6]); // LSB first on the wire
    TEST_ASSERT_EQUAL_HEX8(0x80, out[6 + 47]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, out.back());
}

static void test_feed_splits_large_values()
{
    catgfx::LegacyEncoder enc;
    std::vector<uint8_t> out;
    enc.feed(out, 300);
    // 255 + 45 lines -> two packets of 10 bytes
    TEST_ASSERT_EQUAL(20, out.size());
    TEST_ASSERT_EQUAL_HEX8(0xA1, out[2]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, out[6]);
    TEST_ASSERT_EQUAL_HEX8(45, out[16]);
}

static void test_flow_control()
{
    catgfx::LegacyEncoder enc;
    const uint8_t pause[] = {0x51, 0x78, 0xAE, 0x01, 0x01, 0x00, 0x10, 0x70, 0xFF};
    const uint8_t resume[] = {0x51, 0x78, 0xAE, 0x01, 0x01, 0x00, 0x00, 0x00, 0xFF};
    const uint8_t other[] = {0x51, 0x78, 0xA3, 0x01, 0x03, 0x00, 0x00, 0x0C, 0x10, 0x00, 0xFF};
    TEST_ASSERT_TRUE(enc.parseNotification(pause, sizeof(pause)) == catgfx::FlowControl::Pause);
    TEST_ASSERT_TRUE(enc.parseNotification(resume, sizeof(resume)) == catgfx::FlowControl::Resume);
    TEST_ASSERT_TRUE(enc.parseNotification(other, sizeof(other)) == catgfx::FlowControl::None);
}

static void test_encoder_factory()
{
    TEST_ASSERT_NOT_NULL(catgfx::createEncoder(catgfx::ProtocolFamily::CatGfxLegacy).get());
    TEST_ASSERT_NULL(catgfx::createEncoder(catgfx::ProtocolFamily::CatGfxV2).get());
}

// ---------------------------------------------------------------------------
// IppProtocol
// ---------------------------------------------------------------------------
static std::vector<uint8_t> buildPrintJobRequest()
{
    ipp::Writer w;
    w.header(2, 0, ipp::OP_PRINT_JOB, 42);
    w.group(ipp::TAG_OPERATION);
    w.charset("attributes-charset", "utf-8");
    w.language("attributes-natural-language", "en");
    w.uri("printer-uri", "ipp://catbridge.local/ipp/aabbccddeeff");
    w.name("requesting-user-name", "alice");
    w.mimeType("document-format", "application/pwg-raster");
    w.group(ipp::TAG_JOB);
    w.beginCollection("media-col");
    w.memberName("media-size");
    w.beginCollection("");
    w.memberName("x-dimension");
    w.integer("", 5800);
    w.endCollection();
    w.endCollection();
    w.integer("copies", 1);
    w.keywords("job-sheets", {"none", "none"});
    w.end();
    return w.data();
}

static void test_ipp_parse_complete_request()
{
    std::vector<uint8_t> msg = buildPrintJobRequest();
    const size_t attrLen = msg.size();
    const char doc[] = "RaS2-document";
    msg.insert(msg.end(), doc, doc + sizeof(doc) - 1);

    ipp::RequestParser p;
    const size_t consumed = p.feed(msg.data(), msg.size());
    TEST_ASSERT_TRUE(p.complete());
    TEST_ASSERT_FALSE(p.error());
    TEST_ASSERT_EQUAL(attrLen, consumed);

    const ipp::Request &r = p.request();
    TEST_ASSERT_EQUAL(2, r.versionMajor);
    TEST_ASSERT_EQUAL_HEX16(ipp::OP_PRINT_JOB, r.operation);
    TEST_ASSERT_EQUAL(42, r.requestId);
    TEST_ASSERT_EQUAL_STRING("application/pwg-raster", r.getString("document-format").c_str());
    TEST_ASSERT_EQUAL_STRING("alice", r.getString("requesting-user-name").c_str());
    TEST_ASSERT_EQUAL(1, r.getInt("copies"));
    TEST_ASSERT_EQUAL(2, r.getStrings("job-sheets").size());
    // Collection members must not leak as top level attributes
    TEST_ASSERT_NULL(r.find("x-dimension"));
    TEST_ASSERT_NOT_NULL(r.find("media-col"));
}

static void test_ipp_parse_byte_by_byte()
{
    std::vector<uint8_t> msg = buildPrintJobRequest();
    const size_t attrLen = msg.size();
    msg.push_back('X');
    ipp::RequestParser p;
    size_t total = 0;
    size_t i = 0;
    for (; i < msg.size() && !p.complete(); ++i)
        total += p.feed(&msg[i], 1);
    TEST_ASSERT_TRUE(p.complete());
    TEST_ASSERT_EQUAL(attrLen, total);
    TEST_ASSERT_EQUAL(attrLen, i);
}

static void test_ipp_parse_rejects_garbage()
{
    // Attribute before any group delimiter
    std::vector<uint8_t> msg = {2, 0, 0, 0x0B, 0, 0, 0, 1, 0x47, 0, 1, 'a', 0, 1, 'b', 3};
    ipp::RequestParser p;
    p.feed(msg.data(), msg.size());
    TEST_ASSERT_TRUE(p.error());

    // Attribute section larger than the configured limit
    ipp::RequestParser limited(32);
    std::vector<uint8_t> big(64, 0x01);
    big[0] = 2;
    big[1] = 0;
    limited.feed(big.data(), big.size());
    TEST_ASSERT_TRUE(limited.error());
}

static void test_ipp_writer_encoding()
{
    ipp::Writer w;
    w.header(1, 1, ipp::STATUS_OK, 7);
    w.group(ipp::TAG_PRINTER);
    w.integer("queued-job-count", 3);
    w.end();
    TEST_ASSERT_EQUAL_STRING("0101000000000007"
                             "04"
                             "21"
                             "0010"
                             "717565756564"         // "queued"
                             "2d6a6f622d636f756e74" // "-job-count"
                             "0004"
                             "00000003"
                             "03",
                             hex(w.data()).c_str());
}

// ---------------------------------------------------------------------------
// RasterPipeline
// ---------------------------------------------------------------------------
static void put32(std::vector<uint8_t> &h, size_t off, uint32_t v)
{
    h[off] = static_cast<uint8_t>(v >> 24);
    h[off + 1] = static_cast<uint8_t>(v >> 16);
    h[off + 2] = static_cast<uint8_t>(v >> 8);
    h[off + 3] = static_cast<uint8_t>(v);
}

static std::vector<uint8_t> pwgHeader(uint32_t w, uint32_t h, uint32_t bpc, uint32_t bpp, uint32_t cs)
{
    std::vector<uint8_t> hdr(1796, 0);
    memcpy(hdr.data(), "PwgRaster", 9);
    put32(hdr, 276, 203);
    put32(hdr, 280, 203);
    put32(hdr, 372, w);
    put32(hdr, 376, h);
    put32(hdr, 384, bpc);
    put32(hdr, 388, bpp);
    put32(hdr, 392, (w * bpp + 7) / 8);
    put32(hdr, 400, cs);
    put32(hdr, 420, bpp / bpc);
    return hdr;
}

static void test_pwg_decode_sgray_compressed()
{
    std::vector<uint8_t> s = {'R', 'a', 'S', '2'};
    auto hdr = pwgHeader(8, 4, 8, 8, raster::CSPACE_SW);
    s.insert(s.end(), hdr.begin(), hdr.end());
    // line 0 (+1 repeat): 4 x 0x00, 4 x 0xFF
    s.insert(s.end(), {0x01, 0x03, 0x00, 0x03, 0xFF});
    // line 2: 8 literal pixels (257 - 249 = 8)
    s.insert(s.end(), {0x00, 249, 0, 32, 64, 96, 128, 160, 192, 224});
    // line 3: all white
    s.insert(s.end(), {0x00, 0x07, 0xFF});

    raster::PwgRasterDecoder dec;
    std::vector<std::vector<uint8_t>> lines;
    raster::PwgPageHeader seen;
    dec.onPageStart([&](const raster::PwgPageHeader &h, uint32_t) {
        seen = h;
        return raster::isSupported(h);
    });
    dec.onLine([&](const uint8_t *line, uint32_t) { lines.emplace_back(line, line + 8); });

    // Feed in awkward chunk sizes to exercise the state machine.
    for (size_t i = 0; i < s.size(); i += 7)
        TEST_ASSERT_TRUE(dec.feed(s.data() + i, std::min<size_t>(7, s.size() - i)));
    TEST_ASSERT_TRUE(dec.finish());
    TEST_ASSERT_EQUAL(1, dec.pagesCompleted());
    TEST_ASSERT_EQUAL(8, seen.width);
    TEST_ASSERT_EQUAL(4, lines.size());
    const uint8_t l0[] = {0, 0, 0, 0, 255, 255, 255, 255};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(l0, lines[0].data(), 8);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(l0, lines[1].data(), 8);
    TEST_ASSERT_EQUAL_UINT8(224, lines[2][7]);
    TEST_ASSERT_EQUAL_UINT8(255, lines[3][0]);
}

static void test_pwg_decode_multi_page_and_rgb()
{
    std::vector<uint8_t> s = {'R', 'a', 'S', '2'};
    for (int page = 0; page < 2; ++page)
    {
        auto hdr = pwgHeader(2, 1, 8, 24, raster::CSPACE_SRGB);
        s.insert(s.end(), hdr.begin(), hdr.end());
        // one line: 2 literal RGB pixels: red, white
        s.insert(s.end(), {0x00, 255, 255, 0, 0, 255, 255, 255});
    }
    raster::PwgRasterDecoder dec;
    raster::PwgPageHeader hdr;
    std::vector<uint8_t> gray;
    dec.onPageStart([&](const raster::PwgPageHeader &h, uint32_t) {
        hdr = h;
        return raster::isSupported(h);
    });
    dec.onLine([&](const uint8_t *line, uint32_t) {
        uint8_t g[2];
        raster::toGray(hdr, line, g);
        gray.push_back(g[0]);
        gray.push_back(g[1]);
    });
    TEST_ASSERT_TRUE(dec.feed(s.data(), s.size()));
    TEST_ASSERT_TRUE(dec.finish());
    TEST_ASSERT_EQUAL(2, dec.pagesCompleted());
    TEST_ASSERT_EQUAL(4, gray.size());
    TEST_ASSERT_UINT8_WITHIN(2, 76, gray[0]); // luma of pure red
    TEST_ASSERT_EQUAL_UINT8(255, gray[1]);
}

static void test_pwg_rejects_bad_streams()
{
    raster::PwgRasterDecoder dec;
    const uint8_t bad[] = {'%', 'P', 'D', 'F'};
    TEST_ASSERT_FALSE(dec.feed(bad, sizeof(bad)));
    TEST_ASSERT_TRUE(dec.failed());

    // Truncated page
    raster::PwgRasterDecoder dec2;
    std::vector<uint8_t> s = {'R', 'a', 'S', '2'};
    auto hdr = pwgHeader(8, 2, 8, 8, raster::CSPACE_SW);
    s.insert(s.end(), hdr.begin(), hdr.end());
    s.insert(s.end(), {0x00, 0x07, 0xFF});
    TEST_ASSERT_TRUE(dec2.feed(s.data(), s.size()));
    TEST_ASSERT_FALSE(dec2.finish());

    // Unsupported colour space (CMYK = 6)
    raster::PwgPageHeader h;
    h.width = 1;
    h.height = 1;
    h.bitsPerColor = 8;
    h.bitsPerPixel = 32;
    h.bytesPerLine = 4;
    h.colorSpace = 6;
    TEST_ASSERT_FALSE(raster::isSupported(h));
}

static void test_rasterizer_downscale_black()
{
    raster::MonoRasterizer r;
    raster::MonoRasterizer::Options opt;
    std::vector<std::vector<uint8_t>> rows;
    r.beginPage(768, 100, opt, [&](const uint8_t *row, size_t n) { rows.emplace_back(row, row + n); });
    std::vector<uint8_t> black(768, 0);
    for (int y = 0; y < 100; ++y)
        r.pushLine(black.data());
    r.endPage();
    TEST_ASSERT_EQUAL(50, rows.size());
    for (const auto &row : rows)
    {
        TEST_ASSERT_EQUAL(48, row.size());
        for (uint8_t b : row)
            TEST_ASSERT_EQUAL_HEX8(0xFF, b);
    }
}

static void test_rasterizer_trims_blank_rows()
{
    raster::MonoRasterizer r;
    raster::MonoRasterizer::Options opt;
    uint32_t count = 0;
    r.beginPage(384, 30, opt, [&](const uint8_t *, size_t) { ++count; });
    std::vector<uint8_t> white(384, 255), black(384, 0);
    for (int y = 0; y < 30; ++y)
        r.pushLine((y == 10 || y == 12) ? black.data() : white.data());
    r.endPage();
    // Leading 10 and trailing 17 rows removed, interior blank row kept.
    TEST_ASSERT_EQUAL(3, count);

    opt.trimBlankRows = false;
    count = 0;
    r.beginPage(384, 30, opt, [&](const uint8_t *, size_t) { ++count; });
    for (int y = 0; y < 30; ++y)
        r.pushLine(white.data());
    r.endPage();
    TEST_ASSERT_EQUAL(30, count);
}

static void test_rasterizer_dither_midgray()
{
    raster::MonoRasterizer r;
    raster::MonoRasterizer::Options opt;
    opt.trimBlankRows = false;
    uint32_t black = 0, total = 0;
    r.beginPage(192, 50, opt, [&](const uint8_t *row, size_t n) {
        for (size_t i = 0; i < n; ++i)
            for (int b = 0; b < 8; ++b)
            {
                black += (row[i] >> b) & 1;
                ++total;
            }
    });
    std::vector<uint8_t> gray(192, 128);
    for (int y = 0; y < 50; ++y)
        r.pushLine(gray.data());
    r.endPage();
    TEST_ASSERT_EQUAL(100 * 384, total); // upscaled 2x in both directions
    const double ratio = static_cast<double>(black) / total;
    TEST_ASSERT_TRUE(ratio > 0.40 && ratio < 0.60);
}

static void test_identify_page()
{
    raster::MonoBitmap bmp = raster::buildIdentifyPage("Desk Label Printer", "aa:bb:cc:dd:ee:ff");
    TEST_ASSERT_EQUAL(384, bmp.width);
    TEST_ASSERT_TRUE(bmp.height > 60);
    TEST_ASSERT_EQUAL(bmp.height * 48, bmp.data.size());
    size_t ink = 0;
    for (uint8_t b : bmp.data)
        ink += b ? 1 : 0;
    TEST_ASSERT_TRUE(ink > 100);

    raster::MonoBitmap test = raster::buildTestPage({"line one", "line two"});
    TEST_ASSERT_TRUE(test.height > 0);
    TEST_ASSERT_EQUAL(test.height * 48, test.data.size());
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_crc8_known_packets);
    RUN_TEST(test_lattice_start_packet);
    RUN_TEST(test_bitmap_row_bit_order);
    RUN_TEST(test_feed_splits_large_values);
    RUN_TEST(test_flow_control);
    RUN_TEST(test_encoder_factory);
    RUN_TEST(test_ipp_parse_complete_request);
    RUN_TEST(test_ipp_parse_byte_by_byte);
    RUN_TEST(test_ipp_parse_rejects_garbage);
    RUN_TEST(test_ipp_writer_encoding);
    RUN_TEST(test_pwg_decode_sgray_compressed);
    RUN_TEST(test_pwg_decode_multi_page_and_rgb);
    RUN_TEST(test_pwg_rejects_bad_streams);
    RUN_TEST(test_rasterizer_downscale_black);
    RUN_TEST(test_rasterizer_trims_blank_rows);
    RUN_TEST(test_rasterizer_dither_midgray);
    RUN_TEST(test_identify_page);
    return UNITY_END();
}
