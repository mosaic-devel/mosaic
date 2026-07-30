#pragma once

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

// The ONE conversion between Mosaic's UTF-8 `std::string` paths and `std::filesystem::path`.
//
// Mosaic carries paths as UTF-8 byte strings everywhere -- they come out of the JSON settings
// file, the `.mosaic` container, the recent-files list, drag-and-drop and the FLTK file chooser,
// all of which are UTF-8 by contract. `std::filesystem::path`'s value type, however, is a
// PLATFORM choice, and the implicit `path(std::string)` constructor converts through the
// platform's *narrow* encoding:
//
//   * POSIX (Linux, macOS): `path` is byte-based, and the narrow encoding is the identity. Both
//     functions below are therefore exactly `path(std::string)` and `p.string()` -- the two calls
//     every existing call site already makes. This is the reason a helper can be introduced this
//     late in the project's life with zero risk: the Linux and macOS builds come out BYTE FOR BYTE
//     as they are today, and no behaviour on either platform can change.
//   * Windows: `path` is `wchar_t`-based, and `path(std::string)` decodes the bytes in the
//   process's
//     ACTIVE CODE PAGE (still a legacy ANSI page for most users, not UTF-8). A folder named
//     "Übungen" or "写真" arrives mangled, so the file cannot be opened and the user gets a
//     "could not save" for a path that is perfectly valid. That is the bug this exists to prevent.
//
// The portable crossing is `char8_t`: `std::filesystem::path`'s constructor from a `std::u8string`
// (and `native()`-side `u8string()` accessor) is *defined* to treat the input as UTF-8 and to
// transcode to/from the native encoding, on every platform. We deliberately do NOT use
// `std::filesystem::u8path`, which did the same job pre-C++20 but is DEPRECATED in C++20 precisely
// because the `char8_t` overloads replaced it -- using it would warn under -Wdeprecated, and the
// project builds with -Werror.
//
// ⚠ SCOPE. This header introduces the correct tool; a FULL CALL-SITE AUDIT OF THE CODEBASE IS NOT
// PART OF THE CHANGE THAT ADDED IT. Only the call sites in the settings/i18n/text-resource paths
// and the `.mosaic` write path were converted. Everywhere else still constructs paths from
// `std::string` implicitly, which is correct on POSIX and code-page-dependent on Windows. New code
// should use these two functions; old code is converted opportunistically, per subsystem, as each
// is exercised on Windows.
namespace mosaic::common {

// A std::filesystem::path from a UTF-8 byte string, on every platform.
[[nodiscard]] std::filesystem::path pathFromUtf8(std::string_view utf8);

// The UTF-8 byte string for a path, on every platform.
[[nodiscard]] std::string utf8FromPath(const std::filesystem::path& p);

// A stdio stream for a UTF-8 path -- `std::fopen` spelled so it survives Windows. `mode` is an
// ordinary fopen mode string ("wb", "r+b", ...) and is ASCII by definition. Returns nullptr exactly
// when fopen would; the caller still owns the handle and still calls std::fclose.
//
// This exists because the image codecs hand a FILE* to libpng/libjpeg/libtiff/giflib, which is a C
// API taking a stream rather than a path -- so `std::filesystem::path`'s wide-aware overloads
// (which is what the std::ifstream call sites use instead) are not available to them. On Windows
// the only entry point that can open a path outside the active code page is `_wfopen`, and passing
// UTF-8 bytes to narrow `fopen` is precisely why "export a PNG into an accented folder" failed.
[[nodiscard]] std::FILE* fopenUtf8(std::string_view utf8, const char* mode);

} // namespace mosaic::common
