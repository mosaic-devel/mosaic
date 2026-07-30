#include "common/fs_path.hpp"

namespace mosaic::common {

std::filesystem::path pathFromUtf8(std::string_view utf8) {
#if defined(_WIN32)
    // `path`'s char8_t constructor is the only one specified to read its input as UTF-8 and
    // transcode to the native UTF-16; the char overload would decode in the active code page. The
    // copy is a per-character static_cast rather than a reinterpret_cast of the buffer: char8_t is
    // a DISTINCT type from char in C++20, so aliasing one array as the other is not something the
    // standard blesses, and a path-length copy costs nothing on any path Mosaic handles.
    std::u8string u8;
    u8.reserve(utf8.size());
    for (const char c : utf8)
        u8.push_back(static_cast<char8_t>(c));
    return std::filesystem::path(u8);
#else
    // POSIX: `path` IS bytes, so this is the identity -- literally the `path(std::string)` call
    // every existing call site already makes, which is why adopting the helper cannot change the
    // Linux or macOS build's behaviour.
    return std::filesystem::path(std::string(utf8));
#endif
}

std::string utf8FromPath(const std::filesystem::path& p) {
#if defined(_WIN32)
    const std::u8string u8 = p.u8string(); // native UTF-16 -> UTF-8, per the standard
    std::string out;
    out.reserve(u8.size());
    for (const char8_t c : u8)
        out.push_back(static_cast<char>(c));
    return out;
#else
    return p.string(); // the identity, as above
#endif
}

std::FILE* fopenUtf8(std::string_view utf8, const char* mode) {
#if defined(_WIN32)
    // Both arguments must be wide: _wfopen has no narrow mode overload. The mode string is ASCII by
    // definition (the C standard enumerates the letters), so widening it character by character is
    // exact rather than a transcoding.
    const std::filesystem::path p = pathFromUtf8(utf8);
    std::wstring wmode;
    wmode.reserve(4);
    for (const char* m = mode; *m != '\0'; ++m)
        wmode.push_back(static_cast<wchar_t>(*m));
    return ::_wfopen(p.c_str(), wmode.c_str());
#else
    // POSIX: paths ARE bytes, so this is the plain fopen every call site used before. The
    // std::string is needed only because string_view carries no terminator.
    return std::fopen(std::string(utf8).c_str(), mode);
#endif
}

} // namespace mosaic::common
