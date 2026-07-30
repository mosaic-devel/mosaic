#pragma once

// A byte-exact ZIP writer for tests (test_zip.cpp, test_bundle.cpp): every field is under the
// test's control, so a hostile case corrupts exactly one thing and the assertion says which rule
// caught it. The corruption knobs lie in exactly one of the two header copies, so a test can
// prove WHICH copy the reader trusts (sizes must come from the central directory -- the
// RGBA_brushes.bundle data-descriptor lesson).

#include <doctest/doctest.h>
#include <zlib.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace ziptest {

inline void putU16(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(static_cast<std::uint8_t>(x & 0xFF));
    v.push_back(static_cast<std::uint8_t>(x >> 8));
}

inline void putU32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    putU16(v, static_cast<std::uint16_t>(x & 0xFFFF));
    putU16(v, static_cast<std::uint16_t>(x >> 16));
}

inline void putBytes(std::vector<std::uint8_t>& v, const void* p, std::size_t n) {
    const auto* b = static_cast<const std::uint8_t*>(p);
    v.insert(v.end(), b, b + n);
}

[[nodiscard]] inline std::vector<std::uint8_t> rawDeflate(const std::vector<std::uint8_t>& in) {
    z_stream zs{};
    REQUIRE(deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) ==
            Z_OK);
    std::vector<std::uint8_t> out(deflateBound(&zs, static_cast<uLong>(in.size())));
    zs.next_in = const_cast<Bytef*>(in.data());
    zs.avail_in = static_cast<uInt>(in.size());
    zs.next_out = out.data();
    zs.avail_out = static_cast<uInt>(out.size());
    REQUIRE(deflate(&zs, Z_FINISH) == Z_STREAM_END);
    out.resize(out.size() - zs.avail_out);
    deflateEnd(&zs);
    return out;
}

struct TestEntry {
    std::string name;
    std::vector<std::uint8_t> data;
    bool deflate = false;

    // Corruption knobs -- each lies about exactly one field, in exactly one header copy.
    std::uint16_t flags = 0;         // written to both headers
    bool zeroLocalSizes = false;     // data-descriptor shape: local crc/sizes = 0
    std::uint16_t localExtraLen = 0; // extra bytes present ONLY in the local header
    int methodOverride = -1;         // central-directory method field
    long long centralUncompressedOverride = -1;
    long long centralCrcOverride = -1;
    bool corruptData = false; // flip a byte of the stored/deflated payload
};

[[nodiscard]] inline std::vector<std::uint8_t> buildZip(const std::vector<TestEntry>& entries,
                                                        std::size_t commentLen = 0) {
    std::vector<std::uint8_t> out;
    struct Placed {
        std::uint32_t offset = 0;
        std::uint32_t crc = 0;
        std::vector<std::uint8_t> payload;
    };
    std::vector<Placed> placed;

    for (const TestEntry& e : entries) {
        Placed p;
        p.offset = static_cast<std::uint32_t>(out.size());
        p.crc = e.data.empty()
                    ? 0
                    : static_cast<std::uint32_t>(
                          ::crc32(0, e.data.data(), static_cast<uInt>(e.data.size())));
        p.payload = e.deflate ? rawDeflate(e.data) : e.data;
        if (e.corruptData && !p.payload.empty())
            p.payload[p.payload.size() / 2] ^= 0x5A;

        putU32(out, 0x04034b50); // local header
        putU16(out, 20);         // version needed
        putU16(out, e.flags);
        putU16(out, e.deflate ? 8 : 0);
        putU32(out, 0); // dos time/date
        putU32(out, e.zeroLocalSizes ? 0 : p.crc);
        putU32(out, e.zeroLocalSizes ? 0 : static_cast<std::uint32_t>(p.payload.size()));
        putU32(out, e.zeroLocalSizes ? 0 : static_cast<std::uint32_t>(e.data.size()));
        putU16(out, static_cast<std::uint16_t>(e.name.size()));
        putU16(out, e.localExtraLen);
        putBytes(out, e.name.data(), e.name.size());
        out.insert(out.end(), e.localExtraLen, 0xEE);
        putBytes(out, p.payload.data(), p.payload.size());
        placed.push_back(std::move(p));
    }

    const std::uint32_t centralOffset = static_cast<std::uint32_t>(out.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const TestEntry& e = entries[i];
        const Placed& p = placed[i];
        putU32(out, 0x02014b50);
        putU16(out, 20); // version made by
        putU16(out, 20); // version needed
        putU16(out, e.flags);
        putU16(out, e.methodOverride >= 0 ? static_cast<std::uint16_t>(e.methodOverride)
                                          : (e.deflate ? 8 : 0));
        putU32(out, 0); // dos time/date
        putU32(out, e.centralCrcOverride >= 0 ? static_cast<std::uint32_t>(e.centralCrcOverride)
                                              : p.crc);
        putU32(out, static_cast<std::uint32_t>(p.payload.size()));
        putU32(out, e.centralUncompressedOverride >= 0
                        ? static_cast<std::uint32_t>(e.centralUncompressedOverride)
                        : static_cast<std::uint32_t>(e.data.size()));
        putU16(out, static_cast<std::uint16_t>(e.name.size()));
        putU16(out, 0); // central extra
        putU16(out, 0); // comment
        putU16(out, 0); // disk start
        putU16(out, 0); // internal attrs
        putU32(out, 0); // external attrs
        putU32(out, p.offset);
        putBytes(out, e.name.data(), e.name.size());
    }
    const std::uint32_t centralSize = static_cast<std::uint32_t>(out.size()) - centralOffset;

    putU32(out, 0x06054b50);
    putU16(out, 0); // disk number
    putU16(out, 0); // central start disk
    putU16(out, static_cast<std::uint16_t>(entries.size()));
    putU16(out, static_cast<std::uint16_t>(entries.size()));
    putU32(out, centralSize);
    putU32(out, centralOffset);
    putU16(out, static_cast<std::uint16_t>(commentLen));
    out.insert(out.end(), commentLen, 'c');
    return out;
}

[[nodiscard]] inline std::vector<std::uint8_t> bytesOf(std::string_view s) {
    return {s.begin(), s.end()};
}

// Locate the one EOCD record (for tests that patch its fields).
[[nodiscard]] inline std::size_t eocdAt(const std::vector<std::uint8_t>& zip) {
    for (std::size_t i = zip.size() - 22;; --i) {
        if (zip[i] == 0x50 && zip[i + 1] == 0x4b && zip[i + 2] == 0x05 && zip[i + 3] == 0x06)
            return i;
        REQUIRE(i != 0);
    }
}

} // namespace ziptest
