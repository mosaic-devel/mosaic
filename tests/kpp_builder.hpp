#pragma once

// A minimal synthetic .kpp writer for tests (test_kpp_mapper.cpp, test_brush_library.cpp): a
// PNG skeleton whose `version` and `preset` ride as tEXt chunks -- tEXt only, so no deflate
// stream is needed and the bytes stay hand-checkable. There is deliberately no IDAT: readKpp
// never decodes the raster, and the icon path is exercised with real fixtures instead.

#include <cstdint>
#include <string>
#include <vector>

namespace kpptest {

[[nodiscard]] inline std::uint32_t crc32(const std::uint8_t* p, std::size_t n) {
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c ^ 0xFFFFFFFFu;
}

inline void be32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int s = 24; s >= 0; s -= 8)
        out.push_back(static_cast<std::uint8_t>(v >> s));
}

[[nodiscard]] inline std::vector<std::uint8_t> syntheticKpp(const std::string& version,
                                                            const std::string& presetXml,
                                                            bool withVersion = true,
                                                            bool withPreset = true) {
    std::vector<std::uint8_t> png = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    const auto chunk = [&png](const char type[5], const std::string& data) {
        be32(png, static_cast<std::uint32_t>(data.size()));
        std::vector<std::uint8_t> body(type, type + 4);
        body.insert(body.end(), data.begin(), data.end());
        png.insert(png.end(), body.begin(), body.end());
        be32(png, crc32(body.data(), body.size()));
    };
    chunk("IHDR", std::string("\x00\x00\x00\x01\x00\x00\x00\x01\x08\x06\x00\x00\x00", 13));
    if (withVersion)
        chunk("tEXt", "version" + std::string(1, '\0') + version);
    if (withPreset)
        chunk("tEXt", "preset" + std::string(1, '\0') + presetXml);
    chunk("IEND", "");
    return png;
}

} // namespace kpptest
