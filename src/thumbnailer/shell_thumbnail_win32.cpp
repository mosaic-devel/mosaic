// The Windows half of the .mosaic thumbnailer (S57): an IThumbnailProvider SHELL EXTENSION, so
// Explorer's icon views, the common file dialogs, the Details pane and the Photos-style preview all
// show a document's own picture instead of a generic file icon. This is the THIRD instance of one
// design, not a new one. Linux is two separate mechanisms -- a freedesktop thumbnailer binary a
// file manager spawns once per file (main.cpp) and a KIO plugin for Dolphin -- and macOS is a Quick
// Look thumbnail extension the system loads out-of-process from the app bundle
// (quicklook_macos.mm). Like both of those, this reads ONLY the newest PRVW chunk (a verified
// linear scan that decompresses no tile content) and links mosaic_io + mosaic_common. No FLTK, no
// Vulkan, no fontconfig, no display, no GPU.
//
// WHY AN IN-PROCESS COM DLL. Windows has no spawn-a-helper-per-file thumbnailer protocol either,
// and nothing resembling KIO's plugin host. The shell's supported mechanism is a COM class
// implementing IThumbnailProvider, registered against the file extension, which the shell
// instantiates inside a process of ITS choosing -- normally the isolated thumbnail host (a
// dllhost.exe surrogate), and Explorer itself when a machine has process isolation disabled. That
// is why the io+common-only rule is load-bearing here rather than merely tidy: this code is a guest
// in somebody else's address space, and dragging the text engine, the vector model or the
// compositor into Explorer would be inexcusable. It is also why nothing here logs: spdlog would
// mean a second dependency and a log file written from a shell host, and a thumbnail that fails
// must fail silently by returning a failure HRESULT -- which is exactly the vocabulary the shell
// already reads.
//
// A file that predates previews (pre-S48-b) has no PRVW, and compositing one here would mean
// linking the document model plus a compositor into a shell extension -- exactly the dependency
// this whole family exists to avoid. The deliberate answer matches both siblings: no preview, no
// thumbnail, and the shell falls back to the document icon. One Save in a current Mosaic embeds a
// preview and the file thumbnails forever after. The thumbnail is therefore as sharp as the stored
// 256 px PRVW and no sharper.
//
// ⚠ Compile/link-verified only; there is no Windows machine in the loop. The runtime unknowns are
// collected at the end of packaging/windows/README.md.

// ⚠ INCLUDE ORDER IS LOAD-BEARING, and it is the reason this file breaks the house convention of
// putting project headers first. <initguid.h> does nothing but `#define INITGUID`, which switches
// every DEFINE_GUID in every SDK header included AFTER it from a declaration into a
// __declspec(selectany) DEFINITION. That is how this TU obtains IID_IThumbnailProvider without
// retyping it: MinGW's `uuid` import library does NOT carry that IID (checked with nm), while
// <thumbcache.h> spells it out, so letting the SDK header define it removes the single most
// commonly mistyped constant in this mechanism from the diff entirely. IID_IUnknown and
// IID_IClassFactory are unaffected -- in C++ mode mingw-w64 declares those as plain `EXTERN_C const
// IID`, never as a DEFINE_GUID, so they always come from `uuid`, which is already in MinGW's
// default link line. Should a project header ever start pulling <windows.h>, the GUIDs silently
// become declarations again -- and the failure is a LOUD unresolved symbol at link time, not a
// runtime surprise.
#include <initguid.h>

#include <windows.h>

#include <objbase.h>    // IClassFactory, CoTaskMemAlloc, StringFromGUID2
#include <propsys.h>    // IInitializeWithStream
#include <shlobj.h>     // SHChangeNotify -- the (un)registration cache flush
#include <thumbcache.h> // IThumbnailProvider, WTS_ALPHATYPE, WTS_E_FAILEDEXTRACTION

#include "common/image.hpp"
#include "io/mosaic/preview.hpp"

#include <atomic>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <new>
#include <optional>
#include <string>
#include <vector>

// The coclass GUID. Freshly minted for this handler and NOTHING ELSE may reuse it: a CLSID is the
// name the registry, the MSI and the shell all know this object by, so changing it after a release
// orphans the keys of every installation that already exists. Under INITGUID (above) this is the
// definition, not a declaration.
//
// {986B1E67-2A97-4460-A8ED-071F760F3647}
DEFINE_GUID(CLSID_MosaicThumbnailProvider, 0x986b1e67, 0x2a97, 0x4460, 0xa8, 0xed, 0x07, 0x1f, 0x76,
            0x0f, 0x36, 0x47);

namespace {

// The module's live-object count, gating DllCanUnloadNow. Every provider and every class factory
// holds one, as does an outstanding LockServer. relaxed ordering is enough: the count is only ever
// compared against zero by the loader, which has already serialised itself against the object
// teardowns through its own loader lock.
std::atomic<long> g_moduleObjects{0};

// The DLL's own module handle, captured in DllMain -- DllRegisterServer needs it to write its own
// path into the registry, and there is no other honest way to learn it (a hard-coded install
// directory is exactly the thing a per-user or portable install breaks).
HINSTANCE g_module = nullptr;

// Upper bound on how much of a .mosaic file this handler will buffer.
//
// ⚠ A DELIBERATE, WINDOWS-ONLY DEVIATION from readNewestPreview() and from both sibling handlers,
// which read whole files unconditionally. The PRVW scan needs the file as one buffer, and there is
// no index that would let it seek; on Linux that costs a short-lived helper process its own private
// peak RSS, and on macOS a sandboxed per-file extension. Here the buffer is allocated inside a
// LONG-LIVED, SHARED shell host that is also thumbnailing everything else on the machine, so an
// unbounded read is somebody else's memory. Past the cap the file simply gets the document icon --
// the same outcome as a pre-S48-b file, and a cap this generous is only reachable by a document far
// larger than any Mosaic has been tested with.
constexpr std::uint64_t kMaxSourceBytes = 512ull * 1024ull * 1024ull;

// Staging block for the read loop, HEAP allocated rather than a local array: 64 KiB is nothing on a
// heap and a lot on the stack of a shell worker thread. Large enough that a multi-megabyte document
// is a handful of IStream::Read calls.
constexpr std::size_t kReadBlock = 1u << 16;

// ---- the file -> preview read -------------------------------------------------------------------

// The whole stream as bytes, or nullopt when it cannot be read or exceeds kMaxSourceBytes.
//
// Sized from Stat() only as a RESERVE HINT, never as the loop bound: a shell IStream may be a
// network or virtual-shell stream whose cbSize is unknown (0), and one that lies short would
// silently truncate the scan -- so the loop runs until Read reports no more bytes. STATFLAG_NONAME
// is not an optimisation but a correctness point: the default flag makes Stat allocate pwcsName
// with CoTaskMemAlloc, and every caller that forgets the matching CoTaskMemFree leaks it.
std::optional<std::vector<std::uint8_t>> readWholeStream(IStream* stream) {
    if (stream == nullptr)
        return std::nullopt;

    // Nothing in the IInitializeWithStream contract promises the stream arrives at offset 0, and a
    // scan that starts mid-file finds no chunk magic at all.
    const LARGE_INTEGER start{};
    if (FAILED(stream->Seek(start, STREAM_SEEK_SET, nullptr)))
        return std::nullopt;

    std::vector<std::uint8_t> bytes;
    STATSTG stat{};
    if (SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME))) {
        if (stat.cbSize.QuadPart > kMaxSourceBytes)
            return std::nullopt; // refuse before allocating anything, not after
        if (stat.cbSize.QuadPart > 0)
            bytes.reserve(static_cast<std::size_t>(stat.cbSize.QuadPart));
    }

    std::vector<std::uint8_t> block(kReadBlock);
    for (;;) {
        ULONG got = 0;
        const HRESULT hr = stream->Read(block.data(), static_cast<ULONG>(block.size()), &got);
        // S_FALSE with a short count is the documented end of stream; a hard failure is a torn read
        // and must not be mistaken for one, because a truncated buffer scans as a preview-less
        // file.
        if (FAILED(hr))
            return std::nullopt;
        if (got == 0)
            break;
        if (bytes.size() + got > kMaxSourceBytes)
            return std::nullopt; // a stream that outgrew what Stat promised
        bytes.insert(bytes.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(got));
        if (hr == S_FALSE)
            break;
    }
    return bytes;
}

// ---- the preview -> HBITMAP conversion ----------------------------------------------------------

// `img` as the 32-bit top-down DIB section the shell requires, or nullptr.
//
// TWO conversions happen here and both are easy to get subtly wrong:
//
//   ROW ORDER. A BITMAPINFOHEADER with a POSITIVE biHeight describes a bottom-up DIB, which is the
//   Windows default and the opposite of every image buffer in this program. The NEGATIVE height
//   below is what makes CreateDIBSection hand back top-down rows, so common::Image's row 0 is the
//   bitmap's row 0 and no flip is needed.
//
//   ALPHA. common::Image is straight-alpha RGBA; a 32-bit DIB the shell composites is BGRA with
//   PREMULTIPLIED alpha (that is what WTSAT_ARGB means to the thumbnail host). Premultiplying later
//   is not an option -- the shell would read the unmultiplied colour as too bright wherever the
//   document is transparent, which is the identical requirement the macOS sibling meets for
//   CGImageCreate, with the identical rounding.
HBITMAP createShellDib(const mosaic::common::Image& img) {
    const std::size_t w = img.width;
    const std::size_t h = img.height;
    if (w == 0 || h == 0 || img.rgba.size() < w * h * 4)
        return nullptr;
    // biWidth/biHeight are LONG, and the negation below must not overflow. Unreachable for a real
    // preview (kPreviewEdge is 256) but the reader's own cap is 4096, not 2^31.
    if (w > static_cast<std::size_t>(LONG_MAX) || h > static_cast<std::size_t>(LONG_MAX))
        return nullptr;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = static_cast<LONG>(w);
    bi.bmiHeader.biHeight = -static_cast<LONG>(h); // negative: top-down rows
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    // A DIB SECTION specifically, not a compatible bitmap: the thumbnail host reads the bits
    // directly and documents this as the only accepted HBITMAP shape. No DC is needed for a 32-bit
    // BI_RGB section, so the first argument stays null rather than borrowing the screen's.
    HBITMAP bitmap = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (bitmap == nullptr || pixels == nullptr) {
        if (bitmap != nullptr)
            DeleteObject(bitmap);
        return nullptr;
    }

    // 32 bits per pixel makes every row a multiple of 4 bytes already, so the DIB's stride is
    // exactly w * 4 and there is no padding to skip.
    auto* dst = static_cast<std::uint8_t*>(pixels);
    const std::uint8_t* src = img.rgba.data();
    for (std::size_t i = 0; i < w * h; ++i) {
        const std::uint32_t a = src[4 * i + 3];
        dst[4 * i + 0] = static_cast<std::uint8_t>((src[4 * i + 2] * a + 127) / 255); // B
        dst[4 * i + 1] = static_cast<std::uint8_t>((src[4 * i + 1] * a + 127) / 255); // G
        dst[4 * i + 2] = static_cast<std::uint8_t>((src[4 * i + 0] * a + 127) / 255); // R
        dst[4 * i + 3] = static_cast<std::uint8_t>(a);
    }
    return bitmap;
}

// ---- the coclass --------------------------------------------------------------------------------

// IInitializeWithStream, NOT IInitializeWithFile -- and the reason is that mosaic::io offers both
// spellings, so the choice costs nothing. readNewestPreview() takes a path, but its own
// implementation is a two-liner over newestPreviewInFile(), which takes a std::span of the file's
// bytes; feeding that span from an IStream is a read loop, not an adapter layer. Given a free
// choice, the stream interface is strictly better: it is what the shell prefers, it works for items
// that have no filesystem path at all (network locations, virtual shell namespaces), and it is the
// only one that functions when the thumbnail host runs as a low-privilege isolated process, which
// on a default Windows 10/11 install is the normal case -- an IInitializeWithFile handler in that
// host is handed a path it may not be permitted to open.
class MosaicThumbnailProvider final : public IInitializeWithStream, public IThumbnailProvider {
public:
    MosaicThumbnailProvider() noexcept { g_moduleObjects.fetch_add(1, std::memory_order_relaxed); }

    MosaicThumbnailProvider(const MosaicThumbnailProvider&) = delete;
    MosaicThumbnailProvider& operator=(const MosaicThumbnailProvider&) = delete;

    // ---- IUnknown ----
    //
    // ⚠ Both bases derive from IUnknown non-virtually, so this object contains TWO IUnknown
    // subobjects and a bare cast to IUnknown* is ambiguous. Every conversion therefore names the
    // base it goes through, and IID_IUnknown deliberately always answers with the SAME one, because
    // COM identity requires that QueryInterface(IID_IUnknown) return one fixed pointer for the
    // lifetime of the object.
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr)
            return E_POINTER;
        *ppv = nullptr;
        if (IsEqualIID(riid, IID_IUnknown))
            *ppv = static_cast<IUnknown*>(static_cast<IInitializeWithStream*>(this));
        else if (IsEqualIID(riid, IID_IInitializeWithStream))
            *ppv = static_cast<IInitializeWithStream*>(this);
        else if (IsEqualIID(riid, IID_IThumbnailProvider))
            *ppv = static_cast<IThumbnailProvider*>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(m_refs.fetch_add(1, std::memory_order_relaxed) + 1);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        // acq_rel so every write this thread made to the object happens-before the destructor the
        // LAST releaser runs -- the standard refcount fence, and not theoretical here: the shell is
        // free to marshal a reference to another thread.
        const long left = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (left == 0)
            delete this;
        return static_cast<ULONG>(left);
    }

    // ---- IInitializeWithStream ----
    HRESULT STDMETHODCALLTYPE Initialize(IStream* stream, DWORD grfMode) override {
        (void)grfMode; // read-only either way: this handler never writes to the item
        if (stream == nullptr)
            return E_INVALIDARG;
        // The documented answer to a second Initialize on one instance; the shell relies on it to
        // tell "already busy" apart from a broken handler.
        if (m_stream != nullptr)
            return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        m_stream = stream;
        m_stream->AddRef();
        return S_OK;
    }

    // ---- IThumbnailProvider ----
    HRESULT STDMETHODCALLTYPE GetThumbnail(UINT cx, HBITMAP* phbmp,
                                          WTS_ALPHATYPE* pdwAlpha) override {
        if (phbmp == nullptr || pdwAlpha == nullptr)
            return E_POINTER;
        *phbmp = nullptr;
        *pdwAlpha = WTSAT_UNKNOWN;
        if (m_stream == nullptr)
            return E_UNEXPECTED; // GetThumbnail before Initialize
        if (cx == 0)
            return E_INVALIDARG;

        // ⚠ NO EXCEPTION MAY CROSS THIS FRAME. Everything below is ordinary C++ that allocates --
        // the file buffer, the decoded preview, the downscale -- and a std::bad_alloc unwinding
        // into the shell's C vtable call is not a failed thumbnail, it is a dead thumbnail host
        // taking every other pending thumbnail on the machine with it. The catch-all is the COM
        // boundary doing its job, not defensiveness.
        try {
            const auto bytes = readWholeStream(m_stream);
            if (!bytes.has_value())
                return WTS_E_FAILEDEXTRACTION;

            const auto preview = mosaic::io::native::newestPreviewInFile(*bytes);
            if (!preview.has_value())
                return WTS_E_FAILEDEXTRACTION; // pre-S48-b, or damage took every copy

            // `cx` is the requested MAXIMUM EDGE, and downscalePreview fits the longest edge to it
            // while preserving aspect -- and never upscales, which is the behaviour wanted here
            // rather than a limitation to work around: a 256 px PRVW blown up into a 768 px tile
            // looks worse than a small sharp one, and the shell scales an undersized bitmap into
            // its slot without complaint. Area-averaged and alpha-weighted, so the same downscale
            // the Linux binary and the New-Document dialog's cards show.
            const mosaic::common::Image scaled = mosaic::io::native::downscalePreview(*preview, cx);
            HBITMAP bitmap = createShellDib(scaled);
            if (bitmap == nullptr)
                return WTS_E_FAILEDEXTRACTION;

            // Ownership of the bitmap passes to the caller here; nothing in this object may touch
            // it again.
            *phbmp = bitmap;
            *pdwAlpha = WTSAT_ARGB; // premultiplied BGRA, per createShellDib
            return S_OK;
        } catch (const std::bad_alloc&) {
            return E_OUTOFMEMORY;
        } catch (...) {
            return WTS_E_FAILEDEXTRACTION;
        }
    }

private:
    // Private and non-virtual on purpose. IUnknown has no virtual destructor, so a `delete` through
    // a base pointer would be undefined -- keeping the destructor unreachable from outside means
    // the only way to destroy this object is Release()'s `delete this`, where the static type is
    // already the most derived one.
    ~MosaicThumbnailProvider() {
        if (m_stream != nullptr)
            m_stream->Release();
        g_moduleObjects.fetch_sub(1, std::memory_order_relaxed);
    }

    std::atomic<long> m_refs{1}; // born referenced: the creator owns the first count
    IStream* m_stream = nullptr; // borrowed from the shell, held with an AddRef until teardown
};

// The class factory. One coclass, so no dispatch: DllGetClassObject has already matched the CLSID
// by the time anything here runs.
class MosaicClassFactory final : public IClassFactory {
public:
    MosaicClassFactory() noexcept { g_moduleObjects.fetch_add(1, std::memory_order_relaxed); }

    MosaicClassFactory(const MosaicClassFactory&) = delete;
    MosaicClassFactory& operator=(const MosaicClassFactory&) = delete;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr)
            return E_POINTER;
        *ppv = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory))
            *ppv = static_cast<IClassFactory*>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(m_refs.fetch_add(1, std::memory_order_relaxed) + 1);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const long left = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (left == 0)
            delete this;
        return static_cast<ULONG>(left);
    }

    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (ppv == nullptr)
            return E_POINTER;
        *ppv = nullptr;
        // Aggregation is a COM feature this object does not implement, and the spec is explicit
        // that a non-aggregatable class must say so rather than ignore the outer pointer.
        if (outer != nullptr)
            return CLASS_E_NOAGGREGATION;

        auto* provider = new (std::nothrow) MosaicThumbnailProvider();
        if (provider == nullptr)
            return E_OUTOFMEMORY;
        // The constructor's reference is this function's; QueryInterface takes the caller's, so the
        // Release below is the handover, not a leak.
        const HRESULT hr = provider->QueryInterface(riid, ppv);
        provider->Release();
        return hr;
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
        // The module count and the server lock are deliberately the same counter: both answer the
        // one question DllCanUnloadNow asks.
        if (lock)
            g_moduleObjects.fetch_add(1, std::memory_order_relaxed);
        else
            g_moduleObjects.fetch_sub(1, std::memory_order_relaxed);
        return S_OK;
    }

private:
    ~MosaicClassFactory() { g_moduleObjects.fetch_sub(1, std::memory_order_relaxed); }

    std::atomic<long> m_refs{1};
};

// ---- registration -------------------------------------------------------------------------------
//
// ⚠ THE MSI IS THE REAL REGISTRATION PATH. packaging/windows/mosaic.wxs writes these keys
// declaratively, which is what makes an uninstall (and a rollback of a failed install) actually
// remove them. DllRegisterServer / DllUnregisterServer exist for two other reasons: they make the
// extension testable by hand with `regsvr32` on a machine that has no installer, and they are the
// executable, always-in-sync statement of what the key layout IS -- a table in a comment drifts,
// and the GUIDs below are read out of the compiled constants rather than retyped, so the two
// spellings of this handler's identity cannot disagree.

// A GUID in the registry's `{XXXXXXXX-XXXX-...}` spelling. StringFromGUID2 rather than a literal is
// the whole point: the CLSID key and the IThumbnailProvider handler key are both derived from the
// same constants the vtables use, so a typo in a 32-hex-digit string -- which registers a handler
// nothing will ever ask for and fails completely silently -- is not expressible here.
struct GuidText {
    wchar_t text[40]{}; // 38 characters plus NUL; 40 is the documented buffer size
    bool ok = false;

    explicit GuidText(const GUID& guid) noexcept {
        // ⚠ The COUNT is checked, not merely the buffer, and the reason is worth the two lines: an
        // empty `text` would compose the subkey L"CLSID\\" -- which names the CLSID HIVE ITSELF.
        // Writing that is harmless; handing it to DllUnregisterServer's RegDeleteTreeW would delete
        // every registered COM class on the machine. Unreachable with a 40-wchar buffer (the call
        // always returns 39, the braced string plus its NUL), guarded because that failure would
        // not announce itself.
        ok = StringFromGUID2(guid, text, 40) == 39;
    }
};

bool writeRegString(const std::wstring& subKey, const wchar_t* name, const wchar_t* value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, subKey.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    const auto bytes = static_cast<DWORD>((std::wcslen(value) + 1) * sizeof(wchar_t));
    const LSTATUS rc = RegSetValueExW(key, name, 0, REG_SZ,
                                      reinterpret_cast<const BYTE*>(value), bytes);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

// Two paths joined -- there is no PathCombineW here because that would pull in shlwapi for string
// concatenation, and every input is a registry subkey this file wrote itself.
std::wstring joinKey(const std::wstring& a, const std::wstring& b) { return a + L'\\' + b; }

// `rest` under the per-user class store. NOT HKEY_CLASSES_ROOT: HKCR is a merged READ view of
// HKLM\Software\Classes and HKCU\Software\Classes, and a WRITE through it lands in the HKLM half --
// machine-wide, needing elevation, and a second registration that would then shadow or be shadowed
// by the installer's. packaging/windows/mosaic.wxs installs per-user and writes its association
// rows to HKCU\Software\Classes; these keys go exactly where the MSI's do, so the hand-registered
// extension exercises the same layout the shipped one produces. (A per-machine build would move
// both to HKLM together -- the same two-line flip mosaic.wxs documents for its own rows.)
std::wstring classesKey(const std::wstring& rest) { return L"Software\\Classes\\" + rest; }

} // namespace

// ---- the exported DLL entry points --------------------------------------------------------------
//
// ⚠ ALL FOUR MUST LEAVE THE DLL UNDECORATED, and shell_thumbnail_win32.def is the ONLY thing that
// arranges it -- there is deliberately no __declspec(dllexport) here. Three findings, each verified
// against the real toolchains rather than assumed, decide it that way:
//
//   * The .def alone is sufficient on BOTH targets. A module-definition file is a plain linker
//   input
//     for MinGW (which is why CMake's CMAKE_LINK_DEF_FILE_FLAG is empty on Windows-GNU) and reaches
//     lld through -Xlinker /DEF: on llvm-mingw; a test link with each produced an export table
//     holding exactly these four names, undecorated, and nothing else.
//   * dllexport would BREAK the clang build. <combaseapi.h> already declares DllGetClassObject and
//     DllCanUnloadNow without it, and adding the attribute on the definition is
//     -Wdll-attribute-on-redeclaration, i.e. an error under -Werror on the aarch64 toolchain. GCC
//     has no such diagnostic, so this is precisely the class of defect that ships to one arch only.
//   * The .def is not merely one of two ways to say it -- it is also what STOPS the auto-export.
//     Given no exports at all, GNU ld falls back to exporting every global symbol in the image,
//     which for a DLL that statically links mosaic_io means an export table full of Mosaic
//     internals.
//
// Decoration itself is a non-issue on this project's Windows targets (both arches are 64-bit, where
// STDAPI's __stdcall carries no name mangling), but the failure mode if any of this were wrong is
// the worst kind: the DLL loads, and CoCreateInstance answers "class not registered" without ever
// hinting that the entry point was simply invisible.

// Only one coclass lives here, so the CLSID match is the whole dispatch.
extern "C" HRESULT STDAPICALLTYPE DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    if (ppv == nullptr)
        return E_POINTER;
    *ppv = nullptr;
    if (!IsEqualCLSID(rclsid, CLSID_MosaicThumbnailProvider))
        return CLASS_E_CLASSNOTAVAILABLE;

    auto* factory = new (std::nothrow) MosaicClassFactory();
    if (factory == nullptr)
        return E_OUTOFMEMORY;
    const HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

extern "C" HRESULT STDAPICALLTYPE DllCanUnloadNow(void) {
    return g_moduleObjects.load(std::memory_order_acquire) == 0 ? S_OK : S_FALSE;
}

// The manual escape hatch: `regsvr32 MosaicThumbnail.dll`, and it needs NO elevation, because every
// key it writes is under HKCU\Software\Classes -- see classesKey() for why that is the right root
// rather than the HKEY_CLASSES_ROOT every regsvr32 tutorial reaches for.
extern "C" HRESULT STDAPICALLTYPE DllRegisterServer(void) {
    wchar_t dllPath[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(g_module, dllPath, MAX_PATH);
    // A truncated path would register a server that cannot be found. ERROR_INSUFFICIENT_BUFFER is
    // how GetModuleFileNameW reports that on Windows 10, but a `len == MAX_PATH` answer means the
    // same thing on any release, so both are refused.
    if (len == 0 || len >= MAX_PATH)
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);

    const GuidText clsid(CLSID_MosaicThumbnailProvider);
    const GuidText handler(IID_IThumbnailProvider);
    if (!clsid.ok || !handler.ok)
        return E_UNEXPECTED;

    const std::wstring clsidKey = classesKey(joinKey(L"CLSID", clsid.text));
    if (!writeRegString(clsidKey, nullptr, L"Mosaic Thumbnail Handler"))
        return E_ACCESSDENIED;
    const std::wstring inproc = joinKey(clsidKey, L"InprocServer32");
    if (!writeRegString(inproc, nullptr, dllPath))
        return E_ACCESSDENIED;
    // Apartment, not Both. The shell's thumbnail-handler contract asks for Apartment, and it is
    // also what this object actually is: its refcount is atomic, but one instance is driven as
    // Initialize-then-GetThumbnail with no locking between them, and only an STA guarantees those
    // two calls are serialised. Declaring Both would tell COM it may call in from any MTA thread it
    // likes, which buys nothing here -- the shell already parallelises by creating one instance per
    // file -- and costs the only synchronisation this object relies on.
    if (!writeRegString(inproc, L"ThreadingModel", L"Apartment"))
        return E_ACCESSDENIED;

    // The handler association, written in BOTH of the two places the shell looks, deliberately:
    //
    //   SystemFileAssociations\.mosaic  is the durable one. The shell resolves ShellEx handlers
    //       through the ProgId FIRST, so a key under .mosaic\ShellEx is shadowed the moment the
    //       user sets some other program as the default for .mosaic -- and a .mosaic file should
    //       still show its own picture even when it opens in something else. This branch is not
    //       reached through the ProgId at all, so it survives that.
    //   .mosaic\ShellEx  is the classic location every sample and every troubleshooting guide
    //   names.
    //       Last in the lookup order and therefore harmless, kept because a machine whose
    //       SystemFileAssociations tree has been mangled still thumbnails.
    //
    // Neither is registered against the ProgId (Mosaic.Document) itself: mosaic.wxs owns that key,
    // and a handler nailed to it would silently stop working on exactly the association change
    // above.
    const std::wstring shellEx = classesKey(joinKey(L".mosaic\\ShellEx", handler.text));
    const std::wstring sysShellEx =
        classesKey(joinKey(L"SystemFileAssociations\\.mosaic\\ShellEx", handler.text));
    if (!writeRegString(sysShellEx, nullptr, clsid.text))
        return E_ACCESSDENIED;
    if (!writeRegString(shellEx, nullptr, clsid.text))
        return E_ACCESSDENIED;

    // Flush the shell's association cache so a running Explorer notices without a logoff. It does
    // NOT clear the thumbnail cache -- an already-cached generic icon for a .mosaic file survives
    // this, which is why packaging/windows/README.md documents the cache wipe separately.
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

extern "C" HRESULT STDAPICALLTYPE DllUnregisterServer(void) {
    const GuidText clsid(CLSID_MosaicThumbnailProvider);
    const GuidText handler(IID_IThumbnailProvider);
    if (!clsid.ok || !handler.ok)
        return E_UNEXPECTED; // see GuidText: a blank name here would delete the CLSID hive

    // Only the three keys this handler created, and none of their parents: `.mosaic` and its ProgId
    // are the installer's, and a thumbnail handler that removed the file association on the way out
    // would be a genuinely destructive bug. RegDeleteTreeW because the CLSID key has a child.
    const std::wstring clsidKey = classesKey(joinKey(L"CLSID", clsid.text));
    const std::wstring shellEx = classesKey(joinKey(L".mosaic\\ShellEx", handler.text));
    const std::wstring sysShellEx =
        classesKey(joinKey(L"SystemFileAssociations\\.mosaic\\ShellEx", handler.text));

    // ERROR_FILE_NOT_FOUND is success here: unregistering something already absent is the state the
    // caller asked for, and returning a failure would make an uninstall look broken.
    const LSTATUS a = RegDeleteTreeW(HKEY_CURRENT_USER, clsidKey.c_str());
    const LSTATUS b = RegDeleteTreeW(HKEY_CURRENT_USER, shellEx.c_str());
    const LSTATUS c = RegDeleteTreeW(HKEY_CURRENT_USER, sysShellEx.c_str());
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    for (const LSTATUS rc : {a, b, c}) {
        if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND)
            return HRESULT_FROM_WIN32(static_cast<DWORD>(rc));
    }
    return S_OK;
}

// Not exported (DllMainCRTStartup calls it), and it does exactly two things. Capturing the module
// handle is the one DllRegisterServer cannot do without. DisableThreadLibraryCalls is the load-time
// courtesy that matters most in a shell extension: Explorer creates and destroys a great many
// threads, and without this every one of them would enter this DLL under the loader lock for a
// notification it has no use for.
extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
