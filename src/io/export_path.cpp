#include "io/export_path.hpp"

#include "common/fs_path.hpp" // pathFromUtf8 / utf8FromPath: these strings are UTF-8 paths

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
// See src/io/mosaic/save.cpp: the toolchain defines NOMINMAX, and redefining it is -Werror.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h> // SHGetKnownFolderPath: Windows has no XDG user-dirs file to read
#endif

namespace mosaic::io {
namespace {

// The directory part of `path`, or "" when it has none. Never "." -- a path with no directory
// part has no directory, and inventing "." is how the process working directory gets in.
[[nodiscard]] std::string parentOf(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    if (slash == std::string_view::npos)
        return {};
    if (slash == 0)
        return std::string(path.substr(0, 1)); // "/name" -> "/"
    // A Windows drive root keeps its separator: "C:\shot.png" -> "C:\", never "C:", because "C:" is
    // the CURRENT DIRECTORY on drive C -- a different place, and one isAbsolutePath rightly refuses
    // (a root name with no root directory). Without this the export folder for a document at a
    // drive root would be dropped and the next rule would answer. Unreachable on POSIX: only an
    // absolute candidate is ever handed to parentOf, and "C:\..." is not absolute there.
    if (slash == 2 && path[1] == ':')
        return std::string(path.substr(0, slash + 1));
    return std::string(path.substr(0, slash));
}

// The first ABSOLUTE candidate, or "". The absoluteness test is the whole point: a relative
// candidate is dropped, not resolved.
[[nodiscard]] std::string firstAbsolute(std::initializer_list<std::string_view> candidates) {
    for (const std::string_view c : candidates)
        if (isAbsolutePath(c))
            return std::string(c);
    return {};
}

[[nodiscard]] std::string stemOf(std::string_view path) {
    const std::string name = fileNameOf(path);
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot == 0)
        return name; // ".hidden" is a name, not an extension
    return name.substr(0, dot);
}

// "png" and ".png" both mean the same thing; store the dotted form once.
[[nodiscard]] std::string dottedExtension(std::string_view ext) {
    if (ext.empty())
        return {};
    return ext.front() == '.' ? std::string(ext) : ("." + std::string(ext));
}

[[nodiscard]] std::string joinPath(std::string_view dir, std::string_view name) {
    if (dir.empty())
        return std::string(name);
    // The separator the HOST writes. Both are always ACCEPTED on input (find_last_of("/\\")
    // throughout), but these strings are shown to the user -- the picker's file-name field, the
    // recent-files menu, the window title -- and "C:\Users\me/shot.png" is legal to every Win32 API
    // and wrong to every Windows user.
#if defined(_WIN32)
    constexpr char kSep = '\\';
#else
    constexpr char kSep = '/';
#endif
    std::string out(dir);
    if (out.back() != '/' && out.back() != '\\')
        out += kSep;
    out += name;
    return out;
}

#if defined(_WIN32)

// One environment variable, read from the WIDE environment. std::getenv hands back the CRT's narrow
// copy of it, transcoded through the ACTIVE CODE PAGE -- so "C:\Users\Zoë\..." arrives as CP-1252
// bytes that are not UTF-8, and every path in Mosaic is UTF-8 by convention. _wgetenv reads that
// same variable with no transcode in it, and utf8FromPath does the one lossless conversion.
[[nodiscard]] std::string envString(const wchar_t* key) {
    const wchar_t* v = ::_wgetenv(key);
    if (v == nullptr || *v == L'\0')
        return {};
    return common::utf8FromPath(std::filesystem::path(v));
}

// One known folder as UTF-8. This is the Windows answer to the XDG user-dirs file, and it is not
// optional politeness: Pictures and Documents are redirectable per user, and OneDrive redirects
// them by default on a great many machines, so a hard-coded "%USERPROFILE%\Pictures" would point
// the picker at a folder the user stopped using years ago. KF_FLAG_DEFAULT: report where it IS
// and create nothing -- userDirOrHome() below requires existence before it will use the answer, and
// the §0 rule keeps Mosaic from creating directories in user space uninvited.
[[nodiscard]] std::string knownFolderUtf8(REFKNOWNFOLDERID id) {
    PWSTR wide = nullptr;
    std::string out;
    if (SUCCEEDED(::SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &wide)) && wide != nullptr)
        out = common::utf8FromPath(std::filesystem::path(wide));
    ::CoTaskMemFree(wide); // required even on failure, and nullptr-safe
    return out;
}

#else

[[nodiscard]] std::string envString(const char* key) {
    const char* v = std::getenv(key);
    return (v != nullptr && *v != '\0') ? std::string(v) : std::string{};
}

#endif

// Which OS user folder is being asked for. One enum rather than a string key because the two
// platforms name these folders in incompatible ways -- an XDG variable on one, a KNOWNFOLDERID GUID
// on the other -- and osUserDir() below is the single place that translation happens.
enum class UserFolder : std::uint8_t { Pictures, Documents };

#if !defined(_WIN32)

// One "XDG_<NAME>_DIR=..." record out of the XDG user-dirs file. The format is a shell fragment;
// only the two spellings xdg-user-dirs itself writes are honoured ("$HOME/Pictures" and an
// absolute path), because guessing at arbitrary shell is worse than falling through to $HOME.
[[nodiscard]] std::string xdgUserDir(const char* key, const std::string& home) {
    if (std::string direct = envString(key); !direct.empty()) {
        if (direct.rfind("$HOME", 0) == 0 && !home.empty())
            direct = home + direct.substr(5);
        if (isAbsolutePath(direct))
            return direct;
    }
    std::string configHome = envString("XDG_CONFIG_HOME");
    if (configHome.empty() && !home.empty())
        configHome = home + "/.config";
    if (configHome.empty())
        return {};
    std::ifstream in(configHome + "/user-dirs.dirs");
    if (!in)
        return {};
    const std::string prefix = std::string(key) + "=";
    std::string line;
    while (std::getline(in, line)) {
        // Skip leading blanks and comments.
        const std::size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#')
            continue;
        line.erase(0, first);
        if (line.rfind(prefix, 0) != 0)
            continue;
        std::string value = line.substr(prefix.size());
        while (!value.empty() && (value.back() == '\r' || value.back() == '\n' ||
                                  value.back() == ' ' || value.back() == '\t'))
            value.pop_back();
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            value = value.substr(1, value.size() - 2);
        if (value.rfind("$HOME", 0) == 0 && !home.empty())
            value = home + value.substr(5);
        if (isAbsolutePath(value))
            return value;
    }
    return {};
}

#endif

// The OS's own answer for a user folder, or "". POSIX reads the XDG user-dirs file; Windows asks
// the shell, because there is no such file there and the folders move (see knownFolderUtf8).
[[nodiscard]] std::string osUserDir(UserFolder which, [[maybe_unused]] const std::string& home) {
#if defined(_WIN32)
    return knownFolderUtf8(which == UserFolder::Pictures ? FOLDERID_Pictures : FOLDERID_Documents);
#else
    return xdgUserDir(which == UserFolder::Pictures ? "XDG_PICTURES_DIR" : "XDG_DOCUMENTS_DIR",
                      home);
#endif
}

// The OS dir if it resolves to a real directory, else <home>/<fallbackName> when THAT exists,
// else home. Existence matters here (and only here): pointing the picker at a folder the user
// deleted is worse than opening in their home.
[[nodiscard]] std::string userDirOrHome(UserFolder which, const char* fallbackName) {
    const std::string home = userHomeDir();
    std::error_code ec;
    // pathFromUtf8 before every is_directory: on Windows a narrow string is decoded in the active
    // code page, so a redirected Pictures under an accented account name would be tested for
    // existence under the wrong name and always answer "no".
    if (const std::string os = osUserDir(which, home);
        !os.empty() && std::filesystem::is_directory(common::pathFromUtf8(os), ec))
        return os;
    if (!home.empty()) {
        // joinPath, not a hard-coded '/': this string is shown to the user, so it wants the host's
        // separator. On POSIX joinPath separates with '/' too, so the result is the same string.
        const std::string guess = joinPath(home, fallbackName);
        if (std::filesystem::is_directory(common::pathFromUtf8(guess), ec))
            return guess;
    }
    return home;
}

} // namespace

bool isAbsolutePath(std::string_view path) {
    if (path.empty())
        return false;
    // std::filesystem answers the host's own rule, which is what the picker will be judged by:
    //   POSIX    a leading '/'.
    //   Windows  a root NAME plus a root DIRECTORY -- "C:\x" and "\\server\share\x" qualify;
    //            "C:x" (relative to the current directory ON drive C), "\x" (the current drive's
    //            root, so which drive depends on the process) and "/x" do NOT, and every one of
    //            those is exactly the kind of half-rooted path this policy exists to refuse.
    // pathFromUtf8, not the string_view constructor: identical on POSIX, and on Windows the only
    // decode that cannot turn a non-ASCII name into a differently-shaped one.
    return common::pathFromUtf8(path).is_absolute();
}

std::string fileNameOf(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    if (slash == std::string_view::npos)
        return std::string(path);
    return std::string(path.substr(slash + 1));
}

std::string exportStartFolder(const ExportPathInputs& in) {
    // Only ever a directory we were HANDED: the last export's, the document's, the OS location,
    // home. Nothing here can synthesize the process working directory.
    const std::string lastDir = isAbsolutePath(in.lastExportPath) ? parentOf(in.lastExportPath)
                                                                  : std::string{};
    const std::string docDir =
        isAbsolutePath(in.documentPath) ? parentOf(in.documentPath) : std::string{};
    return firstAbsolute({lastDir, docDir, in.fallbackDir, in.homeDir});
}

ExportSeed seedExportTarget(const ExportPathInputs& in, std::string_view stem,
                            std::string_view extension) {
    ExportSeed seed;
    seed.folder = exportStartFolder(in);
    seed.reExport = isAbsolutePath(in.lastExportPath);

    // The remembered export's own stem wins: you named that file, we keep the name.
    std::string base = seed.reExport ? stemOf(in.lastExportPath) : std::string{};
    if (base.empty())
        base = std::string(stem);
    if (base.empty())
        base = "untitled";

    seed.name = base + dottedExtension(extension);
    seed.fullPath = joinPath(seed.folder, seed.name);
    return seed;
}

std::string resolveExportPath(std::string_view typed, std::string_view folder) {
    if (typed.empty())
        return {};
    if (isAbsolutePath(typed))
        return std::string(typed);
    if (folder.empty())
        return {}; // relative, and nowhere honest to resolve it against
    return joinPath(folder, typed);
}

std::string withExtension(std::string_view path, std::string_view ext) {
    const std::string want = dottedExtension(ext);
    if (want.empty())
        return std::string(path);
    const std::string name = fileNameOf(path);
    const std::size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot != 0) {
        std::string have = name.substr(dot);
        for (char& c : have)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::string lowered = want;
        for (char& c : lowered)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (have == lowered)
            return std::string(path);
    }
    return std::string(path) + want;
}

std::string userHomeDir() {
#if defined(_WIN32)
    // Windows first, and in THIS order deliberately. USERPROFILE is the shell's own answer and the
    // one every other Windows program agrees with; the known folder is the authority when the
    // environment was stripped (a service, a scheduled task, a login without a profile). HOME comes
    // last because MSYS2, Git Bash and Cygwin all set it to a POSIX-shaped "/home/name" that names
    // nothing to Win32 -- isAbsolutePath already declines that (root directory, no root name), so
    // it falls through rather than pointing at a directory that does not exist. A HOME that IS a
    // real Windows path still works, which is what a user who set it deliberately wants.
    if (std::string profile = envString(L"USERPROFILE"); isAbsolutePath(profile))
        return profile;
    if (std::string known = knownFolderUtf8(FOLDERID_Profile); isAbsolutePath(known))
        return known;
    if (std::string home = envString(L"HOME"); isAbsolutePath(home))
        return home;
#else
    if (std::string home = envString("HOME"); isAbsolutePath(home))
        return home;
#endif
    return {};
}

std::string userPicturesDir() { return userDirOrHome(UserFolder::Pictures, "Pictures"); }

std::string userDocumentsDir() { return userDirOrHome(UserFolder::Documents, "Documents"); }

ExportPathInputs exportPathInputs(std::string lastExportPath, std::string documentPath) {
    ExportPathInputs in;
    in.lastExportPath = std::move(lastExportPath);
    in.documentPath = std::move(documentPath);
    in.fallbackDir = userPicturesDir();
    in.homeDir = userHomeDir();
    return in;
}

ExportPathInputs documentPathInputs(std::string documentPath) {
    ExportPathInputs in;
    in.documentPath = std::move(documentPath);
    in.fallbackDir = userDocumentsDir();
    in.homeDir = userHomeDir();
    return in;
}

} // namespace mosaic::io
