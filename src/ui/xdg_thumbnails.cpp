#include "ui/xdg_thumbnails.hpp"

#include "io/brush/md5.hpp"
#include "io/io.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <system_error>

namespace mosaic::ui {

std::string fileUriFor(const std::string& absolutePath) {
    // RFC 3986 path-legal set, matching what GLib/Qt leave unescaped when they build the cache
    // key. Everything else (spaces, UTF-8 bytes, '#', '?', '%') percent-encodes.
    static constexpr char kAllowed[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
        "-._~!$&'()*+,;=:@/";
    std::string uri = "file://";
    for (const char c : absolutePath) {
        if (std::strchr(kAllowed, c) != nullptr && c != '\0') {
            uri += c;
        } else {
            static constexpr char kHex[] = "0123456789ABCDEF";
            const auto b = static_cast<unsigned char>(c);
            uri += '%';
            uri += kHex[b >> 4];
            uri += kHex[b & 15];
        }
    }
    return uri;
}

std::string xdgThumbnailName(const std::string& fileUri) {
    return io::brush::md5Hex(reinterpret_cast<const std::uint8_t*>(fileUri.data()),
                             fileUri.size()) +
           ".png";
}

std::filesystem::path xdgThumbnailRoot() {
    if (const char* cache = std::getenv("XDG_CACHE_HOME"); cache != nullptr && cache[0] != '\0')
        return std::filesystem::path(cache) / "thumbnails";
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0')
        return std::filesystem::path(home) / ".cache" / "thumbnails";
    return {};
}

std::optional<common::Image> loadXdgThumbnail(const std::string& absolutePath) {
    const std::filesystem::path root = xdgThumbnailRoot();
    if (root.empty())
        return std::nullopt;
    const std::string name = xdgThumbnailName(fileUriFor(absolutePath));

    std::error_code ec;
    const auto sourceTime = std::filesystem::last_write_time(absolutePath, ec);
    if (ec)
        return std::nullopt; // no source, no thumbnail

    // Largest first: the card downscales, never upscales.
    for (const char* bucket : {"large", "x-large", "normal"}) {
        const std::filesystem::path candidate = root / bucket / name;
        const auto thumbTime = std::filesystem::last_write_time(candidate, ec);
        if (ec || thumbTime < sourceTime) // missing, or older than the file = stale
            continue;
        if (auto img = io::loadImage(candidate.string()))
            return img;
    }
    return std::nullopt;
}

} // namespace mosaic::ui
