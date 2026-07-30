#include "core/brush/bitmap_tip.hpp"
#include "core/brush/stroke_state.hpp"
#include "io/brush/tip_io.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

// The bitmap tip-file readers (io/brush/tip_io.hpp). The fixtures are the REAL files the docs'
// §3.6 traps were verified against: vegetal.gbr pins the double-inversion rule; fairy-dust.gih
// declares 4 cells and ships 1; A_bamboo-leaves.gih declares rank0:5 over 3 cells so integer
// division parks it on cell 0 forever; hair.png is the mirror-image convention check. ABR has no
// CC-0 corpus, so its cases are hand-built buffers -- which is fitting, since the traps there
// are structural (computed-brush skips, PackBits bounds, the v6 8BIM walk).
namespace {

using namespace mosaic::io::brush;
namespace cb = mosaic::core::brush;

std::vector<std::uint8_t> fixture(const char* name) {
    const std::string path = std::string(MOSAIC_FIXTURE_DIR) + "/" + name;
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    REQUIRE_MESSAGE(f.good(), path);
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(f.tellg()));
    f.seekg(0);
    REQUIRE(f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()))
                .good());
    return buf;
}

// Grey value of pixel i of a frame (tip-image convention: white = no paint).
std::uint8_t grey(const cb::TipFrame& f, std::size_t i) { return f.rgba[i * 4]; }

void push16(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(static_cast<std::uint8_t>(x >> 8));
    v.push_back(static_cast<std::uint8_t>(x));
}
void push32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    push16(v, static_cast<std::uint16_t>(x >> 16));
    push16(v, static_cast<std::uint16_t>(x));
}

} // namespace

TEST_CASE("tip_io: vegetal.gbr -- a real v2 mask, and the raw byte IS coverage") {
    const auto buf = fixture("brush/vegetal.gbr");
    std::string err;
    const auto tip = readGbr(buf.data(), buf.size(), &err);
    REQUIRE_MESSAGE(tip.has_value(), err);

    CHECK(tip->name == "tree flowers speedpainting strokes");
    REQUIRE(tip->frames.size() == 1);
    CHECK(tip->frames[0].width == 42);
    CHECK(tip->frames[0].height == 38);
    CHECK(tip->spacing == 0.6); // stored as 60, a percentage
    CHECK(tip->sourceKind == cb::TipSourceKind::Mask);
    CHECK(tip->defaultApplication == cb::TipApplication::AlphaMask);
    CHECK(!tip->hasColorAndTransparency);

    // §3.6.1: the file's raw bytes span 0..248 as COVERAGE, so the TIP IMAGE must span 7..255 --
    // bright exactly where the raw file is dark. A reader that skips the inversion (or applies
    // it twice) puts the extremes at the wrong ends.
    std::uint8_t lo = 255, hi = 0;
    bool opaque = true;
    const cb::TipFrame& f = tip->frames[0];
    for (std::size_t i = 0; i < std::size_t(42) * 38; ++i) {
        lo = std::min(lo, grey(f, i));
        hi = std::max(hi, grey(f, i));
        opaque = opaque && f.rgba[i * 4 + 3] == 255;
    }
    CHECK(lo == 7); // 255 - 248
    CHECK(hi == 255);
    CHECK(opaque);
}

TEST_CASE("tip_io: fairy-dust.gih -- ncells is a claim, not a count") {
    const auto buf = fixture("brush/fairy-dust.gih");
    std::string err;
    const auto tip = readGih(buf.data(), buf.size(), &err);
    REQUIRE_MESSAGE(tip.has_value(), err);

    // Declares 4 (twice over -- the leading count and the parasite agree), contains exactly one
    // cell; the file simply ends. Trusting either claim reads off the end of the buffer.
    CHECK(tip->name == "GIMP Brush Pipe");
    CHECK(tip->declaredCells == 4);
    REQUIRE(tip->frames.size() == 1);
    CHECK(tip->droppedFrames == 3);
    CHECK(tip->frames[0].width == 128);
    CHECK(tip->spacing == 0.2); // the (only) cell's 20 %
    CHECK(tip->hose.dim == 1);
    CHECK(tip->hose.rank[0] == 4);
    CHECK(tip->hose.selection[0] == cb::FrameSelection::Random);
    // The five parasite keys nobody reads were present and ignored, not rejected.

    // End to end: the mixed-radix index is taken modulo the LOADED count, so every random draw
    // in 0..3 folds home to the one real cell.
    cb::BitmapTip bitmap(tip->frames, tip->defaultApplication, tip->sourceKind, {}, tip->hose);
    REQUIRE(bitmap.frameCount() == 1);
    cb::HoseState hose;
    cb::StrokeState st;
    cb::StrokeInput in;
    st.begin(in, 7);
    hose.beginStroke();
    for (int d = 0; d < 32; ++d) {
        st.beginDab();
        CHECK(hose.selectFrame(tip->hose, bitmap.frameCount(), st, in) == 0);
    }
}

TEST_CASE("tip_io: A_bamboo-leaves.gih -- integer division parks the hose on cell 0") {
    const auto buf = fixture("brush/A_bamboo-leaves.gih");
    const auto tip = readGih(buf.data(), buf.size());
    REQUIRE(tip.has_value());

    // Three real cells, rank0:5. stride = 3/5 = 0 in integer arithmetic, so every draw lands on
    // cell 0 -- and nothing sanitizes it, because the guard only fires for incremental/angular.
    REQUIRE(tip->frames.size() == 3);
    CHECK(tip->declaredCells == 3);
    CHECK(tip->droppedFrames == 0);
    CHECK(tip->hose.rank[0] == 5); // NOT sanitized away
    CHECK(tip->hose.selection[0] == cb::FrameSelection::Random);

    cb::HoseState hose;
    cb::StrokeState st;
    cb::StrokeInput in;
    st.begin(in, 11);
    for (int d = 0; d < 32; ++d) {
        st.beginDab();
        CHECK(hose.selectFrame(tip->hose, 3, st, in) == 0);
    }
}

TEST_CASE("tip_io: hair.png -- a PNG is already a tip image; no inversion") {
    const auto buf = fixture("brush/hair.png");
    std::string err;
    const auto tip = readPngTip(buf.data(), buf.size(), &err);
    REQUIRE_MESSAGE(tip.has_value(), err);

    CHECK(tip->frames[0].width == 114);
    CHECK(tip->frames[0].height == 111);
    CHECK(tip->sourceKind == cb::TipSourceKind::Mask);
    CHECK(tip->defaultApplication == cb::TipApplication::AlphaMask);
    CHECK(!tip->hasColorAndTransparency);
    CHECK(tip->spacing == 0.25); // no brush_spacing chunk: the embedded default stands

    // The file is a 255 field with the hairs cut into it -- mostly WHITE, dark strokes sparse.
    // Read with "the" inversion, the histogram flips.
    std::size_t light = 0, dark = 0;
    const cb::TipFrame& f = tip->frames[0];
    for (std::size_t i = 0; i < std::size_t(114) * 111; ++i)
        (grey(f, i) >= 128 ? light : dark)++;
    CHECK(light > dark);
    CHECK(dark > 0);
}

TEST_CASE("tip_io: a coloured, transparent PNG is an Image tip") {
    // The io test fixture: 4x4 RGBA with a transparent corner and colour content.
    const auto buf = fixture("sample.png");
    const auto tip = readPngTip(buf.data(), buf.size());
    REQUIRE(tip.has_value());
    CHECK(tip->sourceKind == cb::TipSourceKind::Image);
    CHECK(tip->defaultApplication == cb::TipApplication::LightnessMap);
    CHECK(tip->hasColorAndTransparency);
}

TEST_CASE("tip_io: brush_spacing and brush_name text chunks are honoured") {
    // Splice a tEXt into hair.png just before IEND (the last 12 bytes). CRC over type+data.
    auto png = fixture("brush/hair.png");
    const auto crc32 = [](const std::uint8_t* p, std::size_t n) {
        std::uint32_t c = 0xFFFFFFFFu;
        for (std::size_t i = 0; i < n; ++i) {
            c ^= p[i];
            for (int k = 0; k < 8; ++k)
                c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
        return c ^ 0xFFFFFFFFu;
    };
    const std::string data1 = std::string("brush_spacing") + '\0' + "0.5";
    const std::string data2 = std::string("brush_name") + '\0' + "spliced";
    std::vector<std::uint8_t> insert;
    for (const std::string& d : {data1, data2}) {
        push32(insert, static_cast<std::uint32_t>(d.size()));
        std::vector<std::uint8_t> body = {'t', 'E', 'X', 't'};
        body.insert(body.end(), d.begin(), d.end());
        insert.insert(insert.end(), body.begin(), body.end());
        push32(insert, crc32(body.data(), body.size()));
    }
    png.insert(png.end() - 12, insert.begin(), insert.end());

    const auto tip = readPngTip(png.data(), png.size());
    REQUIRE(tip.has_value());
    CHECK(tip->spacing == 0.5); // a FRACTION, unlike GBR's percentage
    CHECK(tip->name == "spliced");
}

TEST_CASE("tip_io: truncated and hostile GBR/GIH inputs fail clean") {
    const auto gbr = fixture("brush/vegetal.gbr");

    SUBCASE("GBR cut mid-pixel-data") {
        const std::vector<std::uint8_t> cut(gbr.begin(), gbr.end() - 100);
        CHECK(!readGbr(cut.data(), cut.size()).has_value());
    }
    SUBCASE("GBR with a lying header size") {
        auto lying = gbr;
        lying[3] = 0xFF; // header_size low byte: name would run far past the file
        CHECK(!readGbr(lying.data(), lying.size()).has_value());
    }
    SUBCASE("GIH whose count line is garbage") {
        const std::string doc = "name\nnot-a-number ncells:1\n";
        CHECK(!readGih(reinterpret_cast<const std::uint8_t*>(doc.data()), doc.size())
                   .has_value());
    }
    SUBCASE("GIH with no newline at all") {
        const std::string doc(8192, 'x');
        CHECK(!readGih(reinterpret_cast<const std::uint8_t*>(doc.data()), doc.size())
                   .has_value());
    }
    SUBCASE("empty") {
        CHECK(!readGbr(nullptr, 0).has_value());
        CHECK(!readGih(nullptr, 0).has_value());
    }
}

// ------------------------------------------------------------------------------------------------
// ABR: hand-built buffers.

namespace {

// One sampled body at depth 8: bounds, depth, compression, then the payload.
void pushSampleBody(std::vector<std::uint8_t>& v, std::uint32_t w, std::uint32_t h,
                    std::uint8_t compression, const std::vector<std::uint8_t>& payload) {
    push32(v, 0);
    push32(v, 0);
    push32(v, h); // bottom
    push32(v, w); // right
    push16(v, 8); // depth
    v.push_back(compression);
    v.insert(v.end(), payload.begin(), payload.end());
}

} // namespace

TEST_CASE("tip_io: ABR v1 -- a computed brush is skipped, the sampled one behind it survives") {
    std::vector<std::uint8_t> abr;
    push16(abr, 1); // version
    push16(abr, 2); // count

    // Brush 1: computed (type 1). The reference's skip for this case is broken and loses the
    // rest of the file; ours must not.
    push16(abr, 1);
    push32(abr, 6);
    for (int i = 0; i < 6; ++i)
        abr.push_back(0xEE);

    // Brush 2: sampled 2x2, raw. Body: 6 discard + 9 discard + bounds/depth/compression + 4 px.
    std::vector<std::uint8_t> body(6, 0);
    body.insert(body.end(), 9, 0);
    pushSampleBody(body, 2, 2, 0, {0, 128, 255, 32});
    push16(abr, 2);
    push32(abr, static_cast<std::uint32_t>(body.size()));
    abr.insert(abr.end(), body.begin(), body.end());

    int dropped = -1;
    std::string err;
    const auto tips = readAbr(abr.data(), abr.size(), &err, &dropped);
    REQUIRE_MESSAGE(tips.has_value(), err);
    REQUIRE(tips->size() == 1);
    CHECK(dropped == 1);
    const TipFile& tip = (*tips)[0];
    CHECK(tip.frames[0].width == 2);
    CHECK(tip.frames[0].height == 2);
    // An ABR sample byte is coverage, like a GBR's: inverted on the way in.
    CHECK(grey(tip.frames[0], 0) == 255);
    CHECK(grey(tip.frames[0], 2) == 0);
    CHECK(tip.sourceKind == cb::TipSourceKind::Mask);
    CHECK(!tip.hasColorAndTransparency);
}

TEST_CASE("tip_io: ABR v2 -- UCS-2 name and PackBits scanlines") {
    std::vector<std::uint8_t> abr;
    push16(abr, 2); // version
    push16(abr, 1); // count

    std::vector<std::uint8_t> body(6, 0);
    // name "AB": u32 count, then u16 units
    push32(body, 2);
    push16(body, 'A');
    push16(body, 'B');
    body.insert(body.end(), 9, 0);
    // 4x4, RLE: each scanline compresses [10,10,10,10] as F D 0A (repeat 10 four times) = 2 bytes
    std::vector<std::uint8_t> payload;
    for (int y = 0; y < 4; ++y)
        push16(payload, 2); // compressed scanline lengths
    for (int y = 0; y < 4; ++y) {
        payload.push_back(0xFD); // -3: repeat next byte 4 times
        payload.push_back(10);
    }
    pushSampleBody(body, 4, 4, 1, payload);
    push16(abr, 2);
    push32(abr, static_cast<std::uint32_t>(body.size()));
    abr.insert(abr.end(), body.begin(), body.end());

    const auto tips = readAbr(abr.data(), abr.size());
    REQUIRE(tips.has_value());
    REQUIRE(tips->size() == 1);
    CHECK((*tips)[0].name == "AB");
    for (std::size_t i = 0; i < 16; ++i)
        CHECK(grey((*tips)[0].frames[0], i) == 245); // 255 - 10
}

TEST_CASE("tip_io: ABR v6.1 -- the 8BIM walk finds samp behind other sections") {
    std::vector<std::uint8_t> abr;
    push16(abr, 6); // version
    push16(abr, 1); // subversion

    // A decoy section first.
    const char* desc = "8BIMdesc";
    abr.insert(abr.end(), desc, desc + 8);
    push32(abr, 4);
    push32(abr, 0xDEADBEEF);

    // Then the sample section with one brush: 37 key bytes + 10 (v6.1) + body, padded to 4.
    const char* samp = "8BIMsamp";
    abr.insert(abr.end(), samp, samp + 8);
    std::vector<std::uint8_t> brush(37, 0);
    brush.insert(brush.end(), 10, 0);
    pushSampleBody(brush, 2, 1, 0, {0, 255});
    std::vector<std::uint8_t> section;
    push32(section, static_cast<std::uint32_t>(brush.size()));
    section.insert(section.end(), brush.begin(), brush.end());
    while (section.size() % 4 != 0)
        section.push_back(0);
    push32(abr, static_cast<std::uint32_t>(section.size()));
    abr.insert(abr.end(), section.begin(), section.end());

    const auto tips = readAbr(abr.data(), abr.size());
    REQUIRE(tips.has_value());
    REQUIRE(tips->size() == 1);
    CHECK(grey((*tips)[0].frames[0], 0) == 255);
    CHECK(grey((*tips)[0].frames[0], 1) == 0);
}

TEST_CASE("tip_io: ABR rejects what it does not speak") {
    std::string err;
    std::vector<std::uint8_t> v5;
    push16(v5, 5);
    push16(v5, 1);
    CHECK(!readAbr(v5.data(), v5.size(), &err).has_value());

    std::vector<std::uint8_t> v63;
    push16(v63, 6);
    push16(v63, 3); // unsupported subversion
    CHECK(!readAbr(v63.data(), v63.size(), &err).has_value());

    CHECK(!readAbr(nullptr, 0, &err).has_value());

    // An all-computed file is a SUCCESS with zero tips and the drop counted.
    std::vector<std::uint8_t> computed;
    push16(computed, 1);
    push16(computed, 1);
    push16(computed, 1); // type: computed
    push32(computed, 2);
    push16(computed, 0);
    int dropped = 0;
    const auto tips = readAbr(computed.data(), computed.size(), &err, &dropped);
    REQUIRE(tips.has_value());
    CHECK(tips->empty());
    CHECK(dropped == 1);
}

TEST_CASE("tip_io: ABR PackBits writes are bounded by the frame, not the stream") {
    // A hostile scanline that claims far more repeats than the frame holds: 2x2 frame, one
    // scanline encoding 128 repeated bytes. The decode must clip at the frame's 4 pixels.
    std::vector<std::uint8_t> abr;
    push16(abr, 1);
    push16(abr, 1);

    std::vector<std::uint8_t> body(6, 0);
    body.insert(body.end(), 9, 0);
    std::vector<std::uint8_t> payload;
    push16(payload, 2); // row 0: 2 compressed bytes
    push16(payload, 2); // row 1
    payload.push_back(0x81); // -127: repeat next byte 128 times
    payload.push_back(200);
    payload.push_back(0x81);
    payload.push_back(200);
    pushSampleBody(body, 2, 2, 1, payload);
    push16(abr, 2);
    push32(abr, static_cast<std::uint32_t>(body.size()));
    abr.insert(abr.end(), body.begin(), body.end());

    const auto tips = readAbr(abr.data(), abr.size());
    REQUIRE(tips.has_value());
    REQUIRE(tips->size() == 1); // decoded, clipped, no overflow (ASan is the real judge here)
    CHECK(grey((*tips)[0].frames[0], 0) == 55); // 255 - 200
}

TEST_CASE("tip_io: parasite edge rules -- garbage dim, the sanitize guard, key order") {
    // A minimal one-cell GIH built by hand: 28-byte v2 header, no name byte beyond the NUL,
    // 1x1 pixel. The parasite is where the case lives.
    const auto gihWith = [](const std::string& parasite) {
        std::string doc = "synthetic\n1 " + parasite + "\n";
        std::vector<std::uint8_t> v(doc.begin(), doc.end());
        push32(v, 29); // header_size: 28 + the name's NUL
        push32(v, 2);  // version
        push32(v, 1);  // width
        push32(v, 1);  // height
        push32(v, 1);  // bytes
        const char* magic = "GIMP";
        v.insert(v.end(), magic, magic + 4);
        push32(v, 20); // spacing %
        v.push_back(0);   // the name's NUL
        v.push_back(200); // the pixel
        return v;
    };

    SUBCASE("garbage dim means 1, not 0 -- sel0 still lands") {
        const auto v = gihWith("ncells:1 dim:abc rank0:1 sel0:random");
        const auto tip = readGih(v.data(), v.size());
        REQUIRE(tip.has_value());
        CHECK(tip->hose.dim == 1);
        CHECK(tip->hose.selection[0] == cb::FrameSelection::Random);
    }
    SUBCASE("rank 0 under incremental degrades to constant (the ONE sanitize rule)") {
        const auto v = gihWith("ncells:1 dim:1 rank0:0 sel0:incremental");
        const auto tip = readGih(v.data(), v.size());
        REQUIRE(tip.has_value());
        CHECK(tip->hose.selection[0] == cb::FrameSelection::Constant);
    }
    SUBCASE("rank 0 under random is NOT sanitized") {
        const auto v = gihWith("ncells:1 dim:1 rank0:0 sel0:random");
        const auto tip = readGih(v.data(), v.size());
        REQUIRE(tip.has_value());
        CHECK(tip->hose.selection[0] == cb::FrameSelection::Random);
    }
    SUBCASE("sel0 arriving BEFORE dim is validated against dim 0 and discarded") {
        const auto v = gihWith("ncells:1 sel0:random dim:1 rank0:1");
        const auto tip = readGih(v.data(), v.size());
        REQUIRE(tip.has_value());
        CHECK(tip->hose.dim == 1);
        // The producer validates indices against dim AS IT STANDS mid-string.
        CHECK(tip->hose.selection[0] == cb::FrameSelection::Constant);
    }
    SUBCASE("unread parasite keys are ignored, not rejected") {
        const auto v = gihWith("ncells:1 dim:1 rank0:1 sel0:random cols:1 rows:1 step:100 "
                               "placement:constant cellwidth:1 cellheight:1 future:thing");
        REQUIRE(readGih(v.data(), v.size()).has_value());
    }
}
