#include <charconv>
#include <clocale>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#ifdef __GLIBC__
#include <malloc.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h> // _NSGetExecutablePath, to locate the bundled MoltenVK ICD (S58)
#endif

#include "bench.hpp"
#include "common/fs_path.hpp"
#include "common/i18n.hpp"
#include "common/image.hpp"
#include "common/log.hpp"
#include "common/profiler.hpp"
#include "common/settings.hpp"
#include "common/version.hpp"
#include "core/core.hpp"
#include "core/document.hpp"
#include "core/texture/texture_layer_render.hpp"
#include "core/texture/texture_render.hpp"
#include "io/io.hpp"
#include "platform/platform.hpp"
#include "platform/system_sound.hpp"
#include "render/compositor.hpp"
#include "render/gpu_caps.hpp"
#include "render/gpu_policy.hpp"
#include "render/render.hpp"
#include "ui/app_window.hpp"
#include "ui/theme.hpp"
#include "ui/ui.hpp"

#if defined(_WIN32)
// ⚠ AFTER the project headers, unlike every other system include in this file. <windows.h> drags in
// wingdi.h and winuser.h, which #define ordinary words -- RGB, TRANSPARENT, OPAQUE, DrawText,
// LoadImage, GetObject -- as macros, and any project header parsed *after* it would have those
// names rewritten out from under it. (std::min/std::max are already safe: the toolchain file
// defines NOMINMAX globally.)
#include <windows.h>
#endif

namespace {

#if defined(_WIN32)
// ---- Windows start-up plumbing (PLAN.md S57) --------------------------------
// Two things a cross-compiled Windows GUI executable has to do for itself before anything else
// runs.

// The directory holding mosaic.exe. Empty if Windows will not say (it always will, for the running
// image). Everything the payload ships is located relative to this, so an unzipped portable copy
// works from any folder -- the same reason the macOS block below derives its paths from
// _NSGetExecutablePath instead of trusting a compiled-in prefix.
//
// A near-twin of the executablePath() helper in common/settings.cpp, which is file-local there. Not
// worth a public header entry for two callers on one platform; if a third appears, promoting that
// one is the fix rather than a third copy.
std::filesystem::path exeDirWin32() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        // GetModuleFileNameW is the one Win32 path call that signals truncation by returning the
        // buffer size instead of the size it needed, so there is nothing to do but keep doubling.
        // Not theoretical: the manifest declares longPathAware, so paths past MAX_PATH are real.
        if (buf.size() >= 32768) return {};  // the long-path ceiling; give up rather than spin
        buf.resize(buf.size() * 2);
    }
    return std::filesystem::path(buf).parent_path();
}

// Borrow the launching console, if there is one, so `mosaic --version` actually prints.
//
// src/app/CMakeLists.txt links this program into the WINDOWS subsystem (-mwindows) so that opening
// a document from Explorer does not also flash a console window. The price is that such a process
// starts with NO standard handles at all: run `mosaic --version` from cmd.exe and every printf
// succeeds into nothing, silently. It is not a bug the CONSOLE subsystem would fix either -- that
// trades a silent command line for a black window behind the GUI for the life of the app. Windows
// simply makes you pick, and then hands you AttachConsole to un-pick it at run time.
//
// AttachConsole(ATTACH_PARENT_PROCESS) succeeds when the parent owns a console (cmd, PowerShell, a
// CI runner) and fails when it does not (Explorer, the Start menu, a file association). On failure
// we do NOTHING, which is exactly right for a GUI launch. In particular not AllocConsole(), which
// would create the window the subsystem choice just bought us.
//
// Two consequences that cannot be fixed from inside the process, and are worth knowing before
// reading a confusing terminal:
//   * cmd.exe does not WAIT for a GUI-subsystem child, so the shell prints its next prompt
//     immediately and our output lands after it. The text is all there, just interleaved.
//     `start /wait mosaic --version` orders it properly.
//   * The console output code page is left alone on purpose. Setting it to UTF-8 would outlive this
//     process -- it belongs to the console, not to us -- so a run of `mosaic --version` would leave
//     the user's shell in a code page it did not ask for. Mosaic's console output is ASCII apart
//     from file names in error messages, which is the smaller sin.
void attachParentConsole() {
    // Snapshot what the process was GIVEN, before AttachConsole can install console handles of its
    // own. `mosaic --bench composite > results.txt` arrives here with a real file handle on stdout,
    // and reopening that on CONOUT$ would throw the user's redirection away -- which is precisely
    // the case the headless harness depends on.
    const bool hadOut = ::GetStdHandle(STD_OUTPUT_HANDLE) != nullptr;
    const bool hadErr = ::GetStdHandle(STD_ERROR_HANDLE) != nullptr;
    const bool hadIn = ::GetStdHandle(STD_INPUT_HANDLE) != nullptr;
    if (::AttachConsole(ATTACH_PARENT_PROCESS) == 0) return;  // no parent console: stay silent

    // Attaching fixes the process's Win32 handles but not the C runtime's FILE objects, which were
    // constructed at start-up around handles that did not exist. freopen re-points them IN PLACE,
    // and because it mutates the same FILE object rather than returning a new one, std::cout and
    // std::cerr -- which hold `stdout`/`stderr` inside their stdio-synced stream buffers -- follow
    // along with no further work. Everything this file prints goes through iostreams, so that
    // detail is the whole reason a plain freopen is enough here.
    if (!hadOut) (void)std::freopen("CONOUT$", "w", stdout);
    if (!hadErr) (void)std::freopen("CONOUT$", "w", stderr);
    if (!hadIn) (void)std::freopen("CONIN$", "r", stdin);
}

// Point the bundled fontconfig at the bundled config tree (<exedir>/etc/fonts).
//
// Mosaic uses fontconfig on Windows exactly as it does on Linux (platform/font_db.cpp) rather than
// growing a DirectWrite backend, so the payload has to carry fontconfig's *rules* as well as its
// DLL. The path compiled into libfontconfig-1.dll is the LINUX CROSS-BUILD PREFIX, a directory that
// will never exist on a user's machine; FONTCONFIG_PATH names the directory holding fonts.conf and
// is the documented override. The shipped fonts.conf is itself relocatable -- it reaches the font
// directory and the cache through fontconfig's own WINDOWSFONTDIR and
// LOCAL_APPDATA_FONTCONFIG_CACHE tokens, and includes `conf.d` by a RELATIVE path resolved against
// its own directory -- so pointing at the directory is all that is needed.
//
// If this fails, fontconfig falls back to a minimal configuration compiled into the DLL, which does
// still scan C:\Windows\Fonts: the app finds real fonts, but without conf.d it loses the alias
// rules that turn a generic family ("sans-serif", "monospace") into an installed one. That is a
// safety net, not the plan.
//
// Never overrides a value the user set. Same rule, and the same executable-relative derivation, as
// the __APPLE__ VK_ICD_FILENAMES block in main() below.
void pointFontconfigAtPayload() {
    if (const char* f = std::getenv("FONTCONFIG_FILE"); f != nullptr && *f != '\0') return;
    if (const char* p = std::getenv("FONTCONFIG_PATH"); p != nullptr && *p != '\0') return;
    const std::filesystem::path dir = exeDirWin32();
    if (dir.empty()) return;
    const std::filesystem::path fonts = dir / "etc" / "fonts";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(fonts / "fonts.conf", ec))
        return; // build tree, not a payload
    // _putenv_s, not SetEnvironmentVariableW: fontconfig reads this with getenv(), i.e. out of the
    // C runtime's own copy of the environment, which SetEnvironmentVariable does not touch. Both
    // mosaic.exe and libfontconfig-1.dll bind the same UCRT, so one _putenv_s is visible to both.
    // (common/i18n.cpp sets $LANGUAGE the same way, for the same reason.)
    (void)::_putenv_s("FONTCONFIG_PATH", mosaic::common::utf8FromPath(fonts).c_str());
}
#endif  // _WIN32

void printVersion() { std::cout << mosaic::common::buildInfo() << '\n'; }

void printHelp(std::string_view argv0) {
    std::cout << "Usage: " << argv0 << " [options] [file]\n\n"
              << "With no options, Mosaic launches its GUI (requires a display).\n"
              << "A file argument is opened at start-up: a .mosaic document, or any image Mosaic\n"
              << "can decode. It is positional and takes no flag, because that is how a desktop\n"
              << "hands a file to a program -- dropping it on the icon, or \"Open with\" -- on\n"
              << "Windows, macOS and Linux alike. Use -- before a filename that starts with '-'.\n\n"
              << "Options:\n"
              << "  -v, --version          Print version information and exit\n"
              << "  -h, --help             Print this help and exit\n"
              << "      --headless         Run the headless op-runner (no GUI)\n"
              << "      --composite-demo   Composite a built-in demo document (no GUI) and exit\n"
              << "      --bench SCENARIO   Run a headless, deterministic benchmark and print a\n"
                 "                         table (no GUI). Omit SCENARIO to list them.\n"
              << "      --bench-iterations N  Samples per benchmark case (default: per scenario)\n"
              << "      --gui-frames N      Launch the GUI, render N frames, then exit (smoke test)\n"
              << "      --log-level LEVEL   trace|debug|info|warn|error|critical|off "
                 "(default: from settings)\n"
              << "      --log-file PATH     Also append log output to PATH\n"
              << "      --config PATH       Use PATH instead of the default settings file\n"
              << "      --profile           Collect per-operation timings in this build "
                 "(also MOSAIC_PROFILE=1)\n"
              << "      --cpu               Run every compute lane on the CPU: no GPU compute\n"
                 "                          objects are built (the window is still presented\n"
                 "                          through Vulkan). Overrides the saved Rendering\n"
                 "                          preference for this run. Also MOSAIC_CPU_ONLY=1\n"
              << "      --gpu, --gpu-compute  Allow the GPU compute lanes for this run even if\n"
                 "                          the saved preference says CPU-only\n"
              << "      --device WHICH      Pick the Vulkan device by index or by part of its\n"
                 "                          name, case-insensitively (e.g. --device 1, --device\n"
                 "                          nvidia). The start-up log lists every device it\n"
                 "                          found, with its index. A selector that matches\n"
                 "                          nothing falls back to the automatic pick and says\n"
                 "                          so. Also MOSAIC_DEVICE\n"
              << "\nHeadless options:\n"
              << "      --width N          Image width  (default 64)\n"
              << "      --height N         Image height (default 64)\n"
              << "      --clear R,G,B[,A]  Fill color, components 0-255 (default 64,128,192,255)\n"
              << "      --texture GEN      Generate a texture layer (sky|paper|grass|wood|\n"
                 "                         marble|stone|canvas|metal) and composite it through\n"
                 "                         the document pipeline (S55-a)\n"
              << "      --seed N           Texture generator seed (default 0; deterministic)\n"
              << "      --export PATH      Write the rendered image as a PPM file\n"
              << "                         (--cpu / --gpu / --gpu-compute above also pick the\n"
                 "                          headless render backend; default: auto)\n";
}

bool parseUint(std::string_view s, std::uint32_t& out) {
    std::uint32_t v = 0;
    const auto* end = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(s.data(), end, v);
    if (ec != std::errc{} || ptr != end) return false;
    out = v;
    return true;
}

bool parseU64(std::string_view s, std::uint64_t& out) {
    std::uint64_t v = 0;
    const auto* end = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(s.data(), end, v);
    if (ec != std::errc{} || ptr != end) return false;
    out = v;
    return true;
}

// Parse "R,G,B" or "R,G,B,A" (each 0-255).
bool parseColor(std::string_view s, mosaic::common::Color8& out) {
    std::uint32_t comp[4] = {0, 0, 0, 255};
    int n = 0;
    std::size_t start = 0;
    while (n < 4) {
        const std::size_t comma = s.find(',', start);
        const std::size_t len = (comma == std::string_view::npos) ? std::string_view::npos
                                                                   : comma - start;
        std::uint32_t v = 0;
        if (!parseUint(s.substr(start, len), v) || v > 255) return false;
        comp[n++] = v;
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    if (n < 3) return false;
    out = {static_cast<std::uint8_t>(comp[0]), static_cast<std::uint8_t>(comp[1]),
           static_cast<std::uint8_t>(comp[2]), static_cast<std::uint8_t>(comp[3])};
    return true;
}

int runHeadless(std::uint32_t w, std::uint32_t h, mosaic::common::Color8 color,
                mosaic::render::Backend backend, const std::string& exportPath) {
    namespace render = mosaic::render;
    const render::RenderResult r = render::renderSolid(w, h, color, backend);
    if (!r.ok) {
        std::cerr << "[headless] render failed: " << r.error << '\n';
        return 1;
    }
    std::cout << "[headless] rendered " << r.image.width << 'x' << r.image.height << " via "
              << render::backendName(r.usedBackend) << " (validation errors: " << r.validationErrors
              << ")\n";
    const auto& px = r.image.rgba;
    std::cout << "[headless] pixel(0,0) = " << +px[0] << ',' << +px[1] << ',' << +px[2] << ','
              << +px[3] << '\n';
    if (!exportPath.empty()) {
        std::string err;
        if (!mosaic::common::writePpm(r.image, exportPath, &err)) {
            std::cerr << "[headless] export failed: " << err << '\n';
            return 1;
        }
        std::cout << "[headless] wrote " << exportPath << '\n';
    }
    return r.validationErrors > 0 ? 3 : 0;
}

// The S55-a texture op: build a document holding one texture-generator layer, run the cache
// refresh pass, composite through the standard pipeline, optionally export. Exercises the whole
// params -> refreshTextureCaches -> compositor chain headlessly (docs/texture-generator.md §2),
// and is the golden-render regeneration path.
int runTextureOp(const std::string& generator, std::uint64_t seed, std::uint32_t w,
                 std::uint32_t h, mosaic::render::Backend backend, const std::string& exportPath) {
    namespace core = mosaic::core;
    namespace texture = mosaic::core::texture;
    namespace render = mosaic::render;
    std::optional<texture::Generator> gen;
    for (int i = 0; i < texture::kGeneratorCount; ++i) {
        const auto g = static_cast<texture::Generator>(i);
        if (generator == texture::generatorTraits(g).token) {
            gen = g;
            break;
        }
    }
    if (!gen) {
        std::cerr << "[texture] unknown generator '" << generator << "' (expected";
        for (int i = 0; i < texture::kGeneratorCount; ++i)
            std::cerr << (i == 0 ? " " : "|")
                      << texture::generatorTraits(static_cast<texture::Generator>(i)).token;
        std::cerr << ")\n";
        return 2;
    }
    core::Document doc(w, h);
    texture::TextureParams params = texture::defaultTextureParams(*gen);
    params.seed = seed;
    doc.root().addOnTop(doc.makeTexture(texture::generatorName(*gen), std::move(params)));
    texture::refreshTextureCaches(doc);
    render::CompositeOptions opts;
    opts.checkerboard = true; // PPM carries no alpha: a clouds-only sky shows its transparency
    const render::CompositeResult r = render::composite(doc, opts, backend);
    if (!r.ok) {
        std::cerr << "[texture] composite failed: " << r.error << '\n';
        return 1;
    }
    std::cout << "[texture] generated " << generator << " (seed " << seed << ") " << r.image.width
              << 'x' << r.image.height << " via " << render::backendName(r.usedBackend)
              << " (validation errors: " << r.validationErrors << ")\n";
    if (!exportPath.empty()) {
        std::string err;
        if (!mosaic::common::writePpm(r.image, exportPath, &err)) {
            std::cerr << "[texture] export failed: " << err << '\n';
            return 1;
        }
        std::cout << "[texture] wrote " << exportPath << '\n';
    }
    return r.validationErrors > 0 ? 3 : 0;
}

// Composite the built-in demo document (no GPU/display required) and optionally export it. This
// exercises the document model -> compositor pipeline end-to-end, the way the headless op-runner
// (PLAN §3.15) will once a file/op format exists (S41/S48).
int runCompositeDemo(const std::string& exportPath, mosaic::render::Backend backend) {
    namespace render = mosaic::render;
    const auto doc = render::makeCompositorDemo();
    render::CompositeOptions opts;
    opts.checkerboard = true;  // flatten over the checkerboard so transparency is visible
    const render::CompositeResult r = render::composite(*doc, opts, backend);
    if (!r.ok) {
        std::cerr << "[composite] failed: " << r.error << '\n';
        return 1;
    }
    std::cout << "[composite] composited " << r.image.width << 'x' << r.image.height << " ("
              << doc->layerCount() << " layers) via " << render::backendName(r.usedBackend)
              << " (validation errors: " << r.validationErrors << ")\n";
    const auto px = [&](std::uint32_t x, std::uint32_t y) {
        const std::size_t p = (static_cast<std::size_t>(y) * r.image.width + x) * 4;
        std::cout << "[composite] pixel(" << x << ',' << y << ") = " << +r.image.rgba[p] << ','
                  << +r.image.rgba[p + 1] << ',' << +r.image.rgba[p + 2] << ','
                  << +r.image.rgba[p + 3] << '\n';
    };
    px(0, 0);    // transparent border -> checkerboard
    px(32, 32);  // center -> blue * red, screened green, inverted
    if (!exportPath.empty()) {
        std::string err;
        if (!mosaic::common::writePpm(r.image, exportPath, &err)) {
            std::cerr << "[composite] export failed: " << err << '\n';
            return 1;
        }
        std::cout << "[composite] wrote " << exportPath << '\n';
    }
    return 0;
}

}  // namespace

// Print the collected per-operation timings (S60-alpha). The Timing Profiler WINDOW stays
// debug-only by product decision, so without this a release `--profile` run would collect into a
// singleton nothing ever reads. Goes to stderr, not the log, so a benchmark run can capture it
// without the log level mattering.
void dumpProfile() {
    const auto rows = mosaic::common::Profiler::instance().snapshot();
    if (rows.empty())
        return;
    // i18n setup (FLTK/gettext) has already moved LC_NUMERIC, so %f would emit the user's decimal
    // separator -- a comma here, which silently breaks any harness parsing these numbers. Pin the
    // C numeric locale for the dump and restore it: this output is data, not UI.
    const char* prevNumeric = std::setlocale(LC_NUMERIC, nullptr);
    const std::string saved = prevNumeric != nullptr ? prevNumeric : "C";
    std::setlocale(LC_NUMERIC, "C");
    std::fprintf(stderr, "\n%-34s %-4s %8s %8s %8s %8s %8s\n", "operation", "lane", "count",
                 "last ms", "min ms", "avg ms", "max ms");
    for (const auto& r : rows) {
        // CPU / GPU / DEV -- DEV rows are real device time from a timestamp query, and a DEV row
        // beside the GPU row of the same name is the submit-vs-device comparison (S60-a).
        std::fprintf(stderr, "%-34s %-4s %8llu %8.2f %8.2f %8.2f %8.2f\n", r.name.c_str(),
                     mosaic::common::laneName(r.lane),
                     static_cast<unsigned long long>(r.count), r.last, r.min, r.avg, r.max);
    }
    std::setlocale(LC_NUMERIC, saved.c_str());
}

int main(int argc, char** argv) {
    namespace common = mosaic::common;
#if defined(_WIN32)
    // FIRST, before anything can print: a -mwindows process has no stdout until it asks for one, so
    // every diagnostic below this line -- including `--version` and `--help` -- would be lost.
    attachParentConsole();
    // Before any font, text or FLTK call, all of which reach fontconfig on the first query.
    pointFontconfigAtPayload();
#endif
    // MOSAIC_PROFILE=1 is the env twin of --profile: it reaches code that runs before argument
    // parsing, and it survives into a harness that does not control the command line (S60-alpha).
    if (const char* p = std::getenv("MOSAIC_PROFILE"); p != nullptr && *p == '1')
        mosaic::common::Profiler::setEnabled(true);
#ifdef __APPLE__
    // Inside a .app bundle the Vulkan loader must be pointed at the bundled MoltenVK ICD
    // (Contents/Resources/vulkan/icd.d/MoltenVK_icd.json) -- there is no system-wide Vulkan on
    // macOS. Set it before any Vulkan call, derived from the executable path so the bundle stays
    // relocatable, and never override a value the user set themselves (S58).
    if (std::getenv("VK_ICD_FILENAMES") == nullptr && std::getenv("VK_DRIVER_FILES") == nullptr) {
        char exePath[4096];
        std::uint32_t sz = sizeof(exePath);
        if (_NSGetExecutablePath(exePath, &sz) == 0) {
            const std::filesystem::path icd =
                std::filesystem::path(exePath).parent_path().parent_path() // Contents/MacOS -> Contents
                / "Resources" / "vulkan" / "icd.d" / "MoltenVK_icd.json";
            std::error_code ec;
            if (std::filesystem::exists(icd, ec)) {
                setenv("VK_ICD_FILENAMES", icd.c_str(), 0);
                setenv("VK_DRIVER_FILES", icd.c_str(), 0);
            }
        }
    }
#endif
#ifdef __GLIBC__
    // Document-sized pixel buffers (a 1080p ImageF is ~32 MiB) sit just under glibc's dynamic
    // mmap-threshold ceiling, so every composite frame munmapped and re-faulted ~100 MB of
    // pages — measurably ~30% of a composite (S15.x perf pass). Raising the threshold keeps
    // those buffers heap-resident across frames. The real fix (buffer pooling / dirty tiles)
    // is the S60 perf session.
    mallopt(M_MMAP_THRESHOLD, 256 * 1024 * 1024);
#endif
    common::i18n::init();  // best-effort; strings stay English until a catalog is installed
    common::i18n::initDomain("motivate");  // the Annoyances one-liners' own catalog (Settings)

    bool headless = false;
    bool compositeDemo = false;
    // The S60 measurement harness (bench.hpp): --bench SCENARIO, and --bench-iterations N with
    // 0 meaning "the scenario's own default".
    std::string benchScenario;
    std::uint32_t benchIterations = 0;
    std::string textureArg;        // --texture GEN: the S55-a headless texture op
    std::uint64_t textureSeed = 0; // --seed N
    std::uint32_t width = 64;
    std::uint32_t height = 64;
    common::Color8 color{64, 128, 192, 255};
    mosaic::render::Backend backend = mosaic::render::Backend::Auto;
    // The GPU-use opinion this command line expressed, if any (render/gpu_policy.hpp). Distinct
    // from `backend`, which only steers the HEADLESS render ops: this one decides whether the GUI's
    // compute lanes may build Vulkan objects at all. `--cpu` sets both, because a user who typed
    // "--cpu" meant it in both senses; `--gpu`/`--gpu-compute` also un-stick a saved CPU-only
    // preference for this run, so an explicit GPU request cannot be refused by a settings file.
    mosaic::render::GpuUseOverride gpuFlag = mosaic::render::GpuUseOverride::None;
    // --device: which Vulkan device this run should use, as an index or part of a name (S60-f).
    // Empty = no opinion, which is a different answer from "auto" the moment MOSAIC_DEVICE also
    // has a say -- see render::decideDeviceSelector.
    std::string deviceArg;
    std::string exportPath;
    int guiFrames = 0;
    std::string logLevelArg;   // empty => fall back to settings / default
    std::string logFileArg;    // empty => stderr only
    std::string configPathArg; // empty => default per-user settings path
    std::vector<std::string> openPathArgs; // positional: documents to open at start-up
    bool endOfOptions = false; // everything after a bare "--" is a filename

    auto needValue = [&](int& i, std::string_view name) -> const char* {
        if (i + 1 >= argc) {
            std::cerr << "Option " << name << " requires a value\n";
            return nullptr;
        }
        return argv[++i];
    };

    // POSITIONAL file arguments, deliberately without a flag: a desktop hands a program its files
    // by putting the paths in argv -- dropping them on the .exe or the dock icon, "Open with..."
    // over a multi-selection, an xdg-mime association, a shell glob -- and it never invents a switch
    // to do it. Anything that is not an option, and everything after a bare "--", is a file. Each
    // opens in its own tab (S49); before tabs existed this refused a second path.
    const auto takePath = [&](std::string_view arg) -> bool {
        openPathArgs.emplace_back(arg);
        return true;
    };

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (endOfOptions) {
            if (!takePath(arg))
                return 2;
            continue;
        }
        if (arg == "--") {
            endOfOptions = true;
            continue;
        }
        if (!arg.starts_with("-")) {
            if (!takePath(arg))
                return 2;
            continue;
        }
        if (arg == "-v" || arg == "--version") {
            printVersion();
            return 0;
        }
        if (arg == "-h" || arg == "--help") {
            printHelp(argv[0]);
            return 0;
        }
        if (arg == "--headless") {
            headless = true;
        } else if (arg == "--profile") {
            // Turn on per-operation timing COLLECTION in any build (S60-alpha). The Help-menu
            // FPS readout and Timing Profiler window stay debug-only; this is the half that lets
            // the RELEASE build -- the one the user runs -- be measured at all.
            mosaic::common::Profiler::setEnabled(true);
        } else if (arg == "--composite-demo") {
            compositeDemo = true;
        } else if (arg == "--bench") {
            // A bare --bench (or one followed by another option) lists the scenarios and exits
            // non-zero, rather than failing with "requires a value" and leaving the user to guess.
            if (i + 1 >= argc || argv[i + 1][0] == '-') {
                std::cerr << "Option --bench requires a scenario.\n\n";
                mosaic::app::printBenchScenarios();
                return 2;
            }
            benchScenario = argv[++i];
        } else if (arg == "--bench-iterations") {
            const char* v = needValue(i, arg);
            if (!v || !parseUint(v, benchIterations) || benchIterations == 0) {
                std::cerr << "Invalid --bench-iterations (expected a positive integer)\n";
                return 2;
            }
        } else if (arg == "--cpu") {
            backend = mosaic::render::Backend::Cpu;
            gpuFlag = mosaic::render::GpuUseOverride::ForceCpuOnly;
        } else if (arg == "--gpu") {
            backend = mosaic::render::Backend::Gpu;
            gpuFlag = mosaic::render::GpuUseOverride::ForceAuto;
        } else if (arg == "--gpu-compute") {
            backend = mosaic::render::Backend::GpuCompute;
            gpuFlag = mosaic::render::GpuUseOverride::ForceAuto;
        } else if (arg == "--device") {
            const char* v = needValue(i, arg);
            if (!v || *v == '\0') {
                std::cerr << "Invalid --device (expected an index or part of a device name)\n";
                return 2;
            }
            deviceArg = v;
        } else if (arg == "--width") {
            const char* v = needValue(i, arg);
            if (!v || !parseUint(v, width)) {
                std::cerr << "Invalid --width\n";
                return 2;
            }
        } else if (arg == "--height") {
            const char* v = needValue(i, arg);
            if (!v || !parseUint(v, height)) {
                std::cerr << "Invalid --height\n";
                return 2;
            }
        } else if (arg == "--clear") {
            const char* v = needValue(i, arg);
            if (!v || !parseColor(v, color)) {
                std::cerr << "Invalid --clear (expected R,G,B[,A], each 0-255)\n";
                return 2;
            }
        } else if (arg == "--texture") {
            const char* v = needValue(i, arg);
            if (!v) return 2;
            textureArg = v;
        } else if (arg == "--seed") {
            const char* v = needValue(i, arg);
            if (!v || !parseU64(v, textureSeed)) {
                std::cerr << "Invalid --seed (expected a non-negative integer)\n";
                return 2;
            }
        } else if (arg == "--export") {
            const char* v = needValue(i, arg);
            if (!v) return 2;
            exportPath = v;
        } else if (arg == "--log-level") {
            const char* v = needValue(i, arg);
            if (!v || !common::log::parseLevel(v)) {
                std::cerr << "Invalid --log-level (expected "
                             "trace|debug|info|warn|error|critical|off)\n";
                return 2;
            }
            logLevelArg = v;
        } else if (arg == "--log-file") {
            const char* v = needValue(i, arg);
            if (!v) return 2;
            logFileArg = v;
        } else if (arg == "--config") {
            const char* v = needValue(i, arg);
            if (!v) return 2;
            configPathArg = v;
        } else if (arg == "--gui-frames") {
            const char* v = needValue(i, arg);
            std::uint32_t n = 0;
            if (!v || !parseUint(v, n)) {
                std::cerr << "Invalid --gui-frames (expected a non-negative integer)\n";
                return 2;
            }
            guiFrames = static_cast<int>(n);
        } else {
            std::cerr << "Unknown option: " << arg << "\n\n";
            printHelp(argv[0]);
            return 2;
        }
    }

    // ---- Settings + logging -------------------------------------------------
    const std::filesystem::path configPath =
        configPathArg.empty() ? common::defaultSettingsPath() : std::filesystem::path(configPathArg);

    std::string settingsErr;
    bool settingsExisted = false;
    const common::Settings settings = common::loadSettings(configPath, &settingsErr, &settingsExisted);

    // CLI --log-level wins over the persisted level; both fall back to Info.
    const std::string levelStr = logLevelArg.empty() ? settings.logLevel : logLevelArg;
    const common::log::Level level =
        common::log::parseLevel(levelStr).value_or(common::log::Level::Info);

    std::string logInitErr;
    common::log::init({.level = level, .file = logFileArg, .color = true}, &logInitErr);

    const auto appLog = common::log::category("app");
    appLog->info("{}", common::buildInfo());
    if (!logInitErr.empty()) appLog->warn("{}", logInitErr);
    if (!settingsErr.empty()) appLog->warn("settings: {}", settingsErr);
    appLog->debug("config: {} (theme={}, logLevel={})",
                  settingsExisted ? configPath.string() : std::string("built-in defaults"),
                  settings.theme, settings.logLevel);

    // ---- GPU-use policy (S60-b item 14; render/gpu_policy.hpp) --------------------------------
    // Decided ONCE, here, before anything can build a lane -- the bench, the headless ops, the
    // composite demo and the GUI all pass through this line. The precedence (flag, then env, then
    // the saved preference) lives in the pure `decideGpuPolicy` so it can be tested without a
    // command line, a settings file or a device.
    {
        namespace render = mosaic::render;
        const render::GpuPolicy policy =
            render::decideGpuPolicy(gpuFlag, render::gpuUseOverrideFromEnv(),
                                    settings.renderingMode == "cpu-only");
        render::setGpuPolicy(policy);
        // Only the non-default is worth a line. The default build says nothing new and does
        // nothing new -- that is the point of it being the default.
        if (!policy.allowsComputeLane())
            appLog->info("CPU-only compute: no GPU compute lane will be built (presentation is "
                         "unaffected)");
    }

    // ---- WHICH device (S60-f; render/gpu_caps.hpp) --------------------------------------------
    // Same precedence and the same reasoning as the block above: the flag is a one-run override
    // and outranks MOSAIC_DEVICE. Set here, before anything can enumerate -- the bench, the
    // headless ops, the composite demo, every compute lane and the window all read the
    // process-wide selector, because `VulkanContext::shared()` has a dozen callers and threading a
    // selector through all of them would be this global with more spelling.
    {
        namespace render = mosaic::render;
        render::DeviceSelector selector =
            render::decideDeviceSelector(deviceArg, render::deviceSelectorFromEnv().text);
        if (!selector.empty())
            appLog->info("gpu: device selector \"{}\" (from {})", selector.text,
                         selector.fromEnvironment ? "MOSAIC_DEVICE" : "--device");
        render::setDeviceSelector(std::move(selector));
    }

    if (!openPathArgs.empty() && (compositeDemo || headless || !benchScenario.empty()))
        appLog->warn("ignoring {} path(s): the headless, composite-demo and bench modes open no "
                     "document",
                     openPathArgs.size());

    // The S60 measurement harness (docs/s60-performance-plan.md §8.2): headless, deterministic,
    // no window and no event loop. Runs before every GUI decision so it works without a display.
    // With --profile it also dumps the per-operation table, so one run yields both views.
    if (!benchScenario.empty()) {
        const int rc = mosaic::app::runBench(benchScenario, benchIterations);
        if (mosaic::common::Profiler::enabled())
            dumpProfile();
        return rc;
    }

    if (compositeDemo) {
        return runCompositeDemo(exportPath, backend);
    }

    if (headless) {
        if (!textureArg.empty())
            return runTextureOp(textureArg, textureSeed, width, height, backend, exportPath);
        return runHeadless(width, height, color, backend, exportPath);
    }

    // First GUI run with no settings file yet: write the defaults so the file exists and is
    // discoverable/hand-editable (and so the S51 settings UI has something to update).
    if (!settingsExisted && !configPath.empty()) {
        std::string saveErr;
        if (common::saveSettings(settings, configPath, &saveErr)) {
            appLog->info("wrote default settings to {}", configPath.string());
        } else {
            appLog->warn("could not write settings to {}: {}", configPath.string(), saveErr);
        }
    }

    // Arm the host's dialog alert sounds. The subsystem defaults to SILENT and this is its only
    // caller, so linking mosaic::platform into a test binary, the thumbnailer or any headless tool
    // can never make the machine beep -- notably `ctest`, where the AskOrTell suite presents a dozen
    // faces. Deliberately after the headless/bench returns above, for the same reason. There is no
    // in-app on/off: each backend routes through the preference the OS already owns (the XDG
    // event-sounds switch, the Windows sound scheme, the macOS alert setting).
    mosaic::platform::enableSystemSounds(true);

    // Default: launch the GUI shell (FLTK window + Vulkan canvas), themed per settings.
    const mosaic::ui::ThemeMode themeMode =
        mosaic::ui::parseThemeMode(settings.theme).value_or(mosaic::ui::ThemeMode::System);
    const int rc = mosaic::ui::runApp(mosaic::ui::RunOptions{.openPaths = std::move(openPathArgs),
                                                     .autoQuitFrames = guiFrames,
                                                     .themeMode = themeMode,
                                                     .pickerSurface = settings.pickerSurface,
                                                     .settingsPath = configPath,
                                                     .recentColors = settings.recentColors,
                                                     .recentFiles = settings.recentFiles,
                                                     .recentSizes = settings.recentSizes,
                                                     .dockWidth = settings.dockWidth,
                                                     .brushPresetHeight =
                                                         settings.brushPresetHeight,
                                                     .brushPreset = settings.brushPreset,
                                                     .eraserPreset = settings.eraserPreset,
                                                     .brushPresetDisplay =
                                                         settings.brushPresetDisplay,
                                                     .workingProfile = settings.workingProfile,
                                                     .cmykProfile = settings.cmykProfile,
                                                     .units = common::resolveUnits(settings.units),
                                                     .cropSwitchToolAfterApply =
                                                         settings.cropSwitchToolAfterApply,
                                                     .cropInitialFraming =
                                                         settings.cropInitialFraming,
                                                     .cropClearSelectionOnLeave =
                                                         settings.cropClearSelectionOnLeave,
                                                     .multiSelectionEdits =
                                                         settings.multiSelectionEdits,
                                                     .lassoSmooth = settings.lassoSmooth,
                                                     .selectBrushAddByDefault =
                                                         settings.selectBrushAddByDefault,
                                                     .eraserSizeFollowsBrush =
                                                         settings.eraserSizeFollowsBrush,
                                                     .overlayLineStyle =
                                                         settings.overlayLineStyle,
                                                     .featherIndicator =
                                                         settings.featherIndicator,
                                                     .antsCirculate = settings.antsCirculate,
                                                     .iconPack = settings.iconPack,
                                                     .motivationalLines =
                                                         settings.motivationalLines,
                                                     .showUnsavedDuration =
                                                         settings.showUnsavedDuration,
                                                     .unsavedIncludeSeconds =
                                                         settings.unsavedIncludeSeconds,
                                                     .spellCheck = settings.spellCheck,
                                                     .spellCheckAllCaps = settings.spellCheckAllCaps,
                                                     .textLanguage = settings.textLanguage,
                                                     .emojiFont = settings.emojiFont,
                                                     .inpaintBackend = settings.inpaintBackend,
                                                     .inpaintPreset = settings.inpaintPreset,
                                                     .inpaintParams = settings.inpaintParams,
                                                     .tabletPressureCurve =
                                                         settings.tabletPressureCurve,
                                                     .tabletPressureMin = settings.tabletPressureMin,
                                                     .tabletPressureMax = settings.tabletPressureMax,
                                                     .tabletTiltOffsetDegrees =
                                                         settings.tabletTiltOffsetDegrees,
                                                     .tabletSpeedMax = settings.tabletSpeedMax,
                                                     .tabletSpeedWindowMs =
                                                         settings.tabletSpeedWindowMs,
                                                     .brushSmoothing = settings.brushSmoothing,
                                                     // Already applied process-wide by
                                                     // setDeviceSelector() above; carried here so
                                                     // the window can report what was asked for.
                                                     .gpuDevice = deviceArg});
    if (mosaic::common::Profiler::enabled())
        dumpProfile();
    return rc;
}
