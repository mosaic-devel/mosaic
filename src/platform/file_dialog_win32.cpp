#include "platform/file_dialog.hpp"

// Windows implementation of platform/file_dialog.hpp (PLAN.md S57), used INSTEAD of file_dialog.cpp
// on Win32 (see platform/CMakeLists.txt) -- the same substitution the Cocoa sibling makes on Apple.
// There is no XDG Desktop Portal, no kdialog and no sd-bus here. The desktop's real file dialog is
// the shell's COMMON ITEM DIALOG (IFileOpenDialog / IFileSaveDialog), and we drive it directly
// rather than through Fl_Native_File_Chooser: FLTK's Windows driver still calls the LEGACY
// GetOpenFileName common dialog, which is the visibly-1990s picker with no places bar, no library
// or OneDrive support and no per-monitor DPI awareness -- the same "this is the wrong dialog"
// complaint that started this module on Linux.
//
// ⚠ BEHAVIOUR DIFFERENCE FROM THE LINUX BACKEND, recorded where it will be found. The header
// promises "NEVER BLOCKS THE MAIN LOOP"; this backend does block it. IFileDialog::Show is a
// synchronous, in-process, modal call that runs its OWN message loop until the user answers, and
// there is no non-blocking form of it. What that promise was actually protecting, though, the
// shell's loop provides itself: it keeps dispatching WM_PAINT / WM_SIZE / WM_CLOSE to our windows
// for as long as the picker is up, so the app stays painted, stays answerable to the window
// manager, and does not replay a burst of queued input afterwards -- precisely the list of failures
// that made "the export picker freezes the whole program" a bug on Linux. What IS lost is Mosaic's
// own timers: the frame timer does not tick, so the canvas holds its last frame instead of
// animating. Accepted deliberately -- the alternative (IFileDialogEvents plus a hand-rolled
// nested pump) buys an animating canvas behind a modal dialog at the price of exactly the
// re-entrancy this module's whole history was spent designing out.

// spdlog FIRST, before anything drags in <windows.h> (FL/platform.H does, via FL/win32.H):
// windows.h defines a pile of bare macros -- ERROR, small/near/far, and with UNICODE a rename of
// every -A/-W entry point -- and fmt's headers are the usual casualty. Including it while the
// preprocessor is still clean costs nothing and removes the question.
#include "common/log.hpp"

#include <FL/Fl_Window.H>
#include <FL/platform.H> // fl_xid -> the window's HWND (and the <windows.h> this TU needs)

#include <shobjidl.h> // IFileDialog / IShellItem / COMDLG_FILTERSPEC / SHCreateItemFromParsingName

#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <string>
#include <vector>

namespace mosaic::platform {

namespace {

spdlog::logger& plog() {
    static const auto logger = common::log::category("platform");
    return *logger;
}

// ---- UTF-8 <-> UTF-16 --------------------------------------------------------------------------
//
// Mosaic holds every path as UTF-8 std::string; every Win32 entry point this file calls takes
// UTF-16. These two are the whole boundary, and they are the reason the build defines UNICODE: the
// CRT's narrow forms would go through the ACTIVE CODE PAGE instead, which cannot represent most of
// what a user may name a folder and silently mangles the rest.

std::wstring toWide(const std::string& utf8) {
    if (utf8.empty())
        return {};
    // Measured with an EXPLICIT byte count rather than -1, so the NUL stays out of the result and
    // the wstring's size() is its length: a wstring carrying a trailing NUL would put one inside
    // the dialog's file-name field.
    const int chars =
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    if (chars <= 0)
        return {};
    std::wstring out(static_cast<std::size_t>(chars), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), out.data(), chars);
    return out;
}

std::string fromWide(const wchar_t* wide) {
    if (wide == nullptr || wide[0] == L'\0')
        return {};
    const int len = static_cast<int>(std::wcslen(wide));
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, len, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0)
        return {};
    std::string out(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, len, out.data(), bytes, nullptr, nullptr);
    return out;
}

// One environment variable, as UTF-8. GetEnvironmentVariableW rather than getenv() for the same
// reason as above -- and rather than _wgetenv(), whose wide environment block is initialised lazily
// by the CRT and is empty in a program that does not enter through wmain().
std::string environmentValue(const wchar_t* name) {
    const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0)
        return {}; // unset, or empty -- the same thing to every caller here
    std::wstring buf(needed, L'\0');
    // The sizing call counts the NUL; the filling call does not report it. A second answer that no
    // longer fits means the variable changed under us, which is not worth a retry loop.
    const DWORD written = GetEnvironmentVariableW(name, buf.data(), needed);
    if (written == 0 || written >= needed)
        return {};
    buf.resize(written);
    return fromWide(buf.c_str());
}

// ---- COM ownership -----------------------------------------------------------------------------

// CoInitializeEx / CoUninitialize as an RAII pair, with the refcount rule spelled out because
// getting it wrong is a silent theft of somebody else's apartment reference:
//
//   S_OK     we initialised the apartment. Ours to uninitialise.
//   S_FALSE  the apartment was ALREADY initialised on this thread -- but the call still took a
//            reference, so it must still be paired with CoUninitialize. This is the normal case:
//            FLTK's Windows driver calls OleInitialize() for drag-and-drop when it opens the
//            display, so COM is up long before the first picker.
//   RPC_E_CHANGED_MODE  the thread is already an MTA and our apartment request was REFUSED. NO
//            reference was taken; calling CoUninitialize here would decrement whoever did
//            initialise it, and a spurious CoUninitialize is how a process ends up tearing COM
//            down under a component that is still using it. So: proceed (the thread has usable
//            COM either way -- the dialog objects are created in-proc and the shell marshals what
//            it must) but do NOT uninitialise.
//
// Any other failure is fatal for this call: there is no dialog without COM.
class ComApartment {
public:
    ComApartment() noexcept
        : m_hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {}
    ~ComApartment() {
        if (m_hr == S_OK || m_hr == S_FALSE)
            CoUninitialize();
    }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

    [[nodiscard]] bool usable() const noexcept {
        return SUCCEEDED(m_hr) || m_hr == RPC_E_CHANGED_MODE;
    }
    [[nodiscard]] HRESULT result() const noexcept { return m_hr; }

private:
    HRESULT m_hr;
};

// Minimal COM interface ownership -- the job BusHandle/SlotHandle do for sd-bus in file_dialog.cpp:
// no early return may leak an interface, and the order of teardown must not be something a future
// edit has to remember. Deliberately not a general-purpose smart pointer (no copy, no reset, no
// detach): every use here is "create it, drive it, drop it".
template <typename T> class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() {
        if (m_p != nullptr)
            m_p->Release();
    }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    // Two out-parameter spellings, because COM has two conventions and mixing them up is a compile
    // error at best: the riid-taking factories (CoCreateInstance, SHCreateItemFromParsingName) want
    // an untyped `void**`, while an interface method that can only ever hand back one type
    // (IFileDialog::GetResult) is honestly typed. The cast lives here so no call site spells it.
    [[nodiscard]] void** put() noexcept { return reinterpret_cast<void**>(&m_p); }
    [[nodiscard]] T** address() noexcept { return &m_p; }
    [[nodiscard]] T* get() const noexcept { return m_p; }
    T* operator->() const noexcept { return m_p; }
    explicit operator bool() const noexcept { return m_p != nullptr; }

private:
    T* m_p = nullptr;
};

// ---- request -> dialog ------------------------------------------------------------------------

// req.filters as the shell wants them. Two things differ from every other backend:
//
//   * Windows joins the globs of ONE type with ';' ("*.png;*.jpg"), where FLTK uses a brace group
//     and the portal a list of separate patterns.
//   * req.mimeTypes is IGNORED, and there is nothing to compensate for. The shell matches purely on
//     EXTENSION, so the header's NB -- KDE's picker applying a filter's mime list INSTEAD of its
//     globs, which once hid every .mosaic file from the combined type -- has no Windows analogue.
//
// COMDLG_FILTERSPEC only BORROWS its two strings, so they must outlive the Show() call; this owns
// them. The array is therefore filled only once both string vectors have stopped growing, and the
// type is non-copyable so it can never be moved either: a std::wstring short enough for the
// small-string buffer keeps its characters INSIDE the object, so a reallocation or a move silently
// invalidates every c_str() already written into an entry -- and "Images" is short enough.
class FilterSpecs {
public:
    FilterSpecs() = default;
    FilterSpecs(const FilterSpecs&) = delete;
    FilterSpecs& operator=(const FilterSpecs&) = delete;

    void build(const std::vector<FileFilter>& filters) {
        for (const FileFilter& f : filters) {
            if (f.globs.empty())
                continue; // no globs describes no file set: dropped, as on every other backend
            std::string joined;
            for (const std::string& g : f.globs) {
                if (!joined.empty())
                    joined += ';';
                joined += g;
            }
            m_names.push_back(toWide(f.name));
            m_specs.push_back(toWide(joined));
        }
        m_entries.reserve(m_names.size());
        for (std::size_t i = 0; i < m_names.size(); ++i)
            m_entries.push_back(COMDLG_FILTERSPEC{m_names[i].c_str(), m_specs[i].c_str()});
    }

    [[nodiscard]] bool empty() const noexcept { return m_entries.empty(); }
    [[nodiscard]] UINT count() const noexcept { return static_cast<UINT>(m_entries.size()); }
    [[nodiscard]] const COMDLG_FILTERSPEC* data() const noexcept { return m_entries.data(); }

private:
    std::vector<std::wstring> m_names;
    std::vector<std::wstring> m_specs;
    std::vector<COMDLG_FILTERSPEC> m_entries; // borrows from m_names / m_specs
};

// "*.png" -> "png"; "" for a glob that names no extension ("*", "*.*", anything without a dot).
// SetDefaultExtension wants it WITHOUT the dot.
std::string extensionFromGlob(const std::string& glob) {
    const std::size_t dot = glob.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= glob.size())
        return {};
    const std::string ext = glob.substr(dot + 1);
    return ext == "*" ? std::string{} : ext;
}

// The last component of a path, or the whole string when it has no separator -- the same rule as
// detail::baseNameOf on Linux, restated here because the detail helpers are POSIX-shaped and the
// Windows TU implements none of them (see the guard in file_dialog.hpp). SetFileName takes a BASE
// NAME: a path handed to it is pasted whole into the dialog's name field, which is one of the bugs
// the header's note about baseNameOf exists to remember.
std::string baseName(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Absoluteness by the HOST's rule -- a drive letter or a UNC prefix, never a leading '/'. Asked of
// std::filesystem deliberately, because that is the same question io::isAbsolutePath asks and the
// picker must be judged by the rule the export-path policy already used.
//
// Built on the wide form rather than on common::pathFromUtf8 (the project's canonical UTF-8 ->
// path crossing) for two reasons: this TU's real string boundary is the Win32 -W entry points, so
// toWide() has to exist here anyway and a second conversion mechanism beside it would only be
// something to keep in sync; and path's char8_t constructor transcodes, so it can THROW on
// ill-formed input, while path(std::wstring) is a copy of the native type and cannot. A start
// folder is not worth an exception escaping into a UI callback.
bool isAbsoluteWindowsPath(const std::string& path) {
    if (path.empty())
        return false;
    return std::filesystem::path(toWide(path)).is_absolute();
}

// The ABSOLUTE directory to open the picker in: req.startFolder when it is absolute, else the user
// profile directory. NEVER the process working directory -- the export-path policy in
// io/export_path.hpp exists for exactly that reason, and platform/ does not depend on io/, so the
// last-resort rule is spelled out again here just as it is in the Linux TU.
std::string absoluteStartFolder(const FileDialogRequest& req) {
    if (isAbsoluteWindowsPath(req.startFolder))
        return req.startFolder;
    // USERPROFILE is Windows' $HOME. HOME is looked at first only because a POSIX-ish shell (MSYS2,
    // Git Bash) sets it, and when it does it is what the user means; when it holds a Unix-shaped
    // "/home/u" the absoluteness test rejects it on the way back out.
    if (const std::string home = environmentValue(L"HOME"); isAbsoluteWindowsPath(home))
        return home;
    if (const std::string profile = environmentValue(L"USERPROFILE");
        isAbsoluteWindowsPath(profile))
        return profile;
    return {};
}

// The HWND to own the dialog. nullptr is valid and simply means unowned -- the picker still opens,
// it just is not stacked over, centred on, or modal to anything of ours. Every caller in the app
// passes a real top-level window, which is what a Win32 modal owner must be.
HWND ownerWindow(Fl_Window* win) {
    if (win == nullptr || win->shown() == 0)
        return nullptr;
    return fl_xid(win);
}

// ---- modality bookkeeping ----------------------------------------------------------------------

// >0 while run() is on the stack. A counter, not a bool, so the balance survives even if the
// re-entrancy refusal in run() is ever removed. Read via fileDialogInFlight().
int g_dialogDepth = 0;

// Raises "a picker is up" for its lifetime -- the same shape as detail::InFlightScope on Linux,
// file-local because this TU does not implement the detail namespace.
struct InFlightScope {
    InFlightScope() noexcept { ++g_dialogDepth; }
    ~InFlightScope() { --g_dialogDepth; }
    InFlightScope(const InFlightScope&) = delete;
    InFlightScope& operator=(const InFlightScope&) = delete;
};

std::optional<std::string> run(const FileDialogRequest& req, bool save) {
    // ONE picker at a time, enforced rather than assumed (the header's re-entrancy refusal). Show()
    // pumps its own message loop, which dispatches to our windows: a timeout, an fd handler or a
    // worker's completion callback that reaches a menu action can get back in here while a dialog
    // is already on screen, and two stacked pickers cannot keep the bookkeeping below straight.
    if (g_dialogDepth > 0) {
        plog().warn("file dialog: refusing a second picker while one is already up");
        return std::nullopt;
    }
    // ... and that is the ONLY guard raised here. The parent window is deliberately not
    // deactivate()d the way the Linux and macOS backends do it: Show(owner) disables the owner
    // window itself, in this process, synchronously -- there is no compositor that may decline the
    // request, which is the whole reason Linux needs a modality it enforces rather than asks for.
    // Greying our own chrome on top of that would only make Mosaic the one Windows application
    // whose window dims behind a file dialog.
    const InFlightScope inFlight;

    const ComApartment com;
    if (!com.usable()) {
        plog().warn("file dialog: CoInitializeEx failed (hr={:#010x}); no picker",
                    static_cast<unsigned long>(com.result()));
        return std::nullopt;
    }

    // Two CLSIDs, one interface. The CLSID is what matters -- it selects the save-shaped or
    // open-shaped dialog together with its default options -- while everything driven below lives
    // on IFileDialog, the base both of them derive from. Asking for IID_IFileDialog directly keeps
    // the pointer honestly typed; requesting IID_IFileSaveDialog into an IFileDialog* would work
    // only by COM's single-inheritance layout, which is not a thing to lean on for no gain.
    //
    // ⚠ The GUIDs are the CLSID_*/IID_* CONSTANTS that <shobjidl.h> declares and the MinGW `uuid`
    // import library defines. MSVC's __uuidof operator is not the portable spelling (mingw-w64 does
    // emulate it via __mingw_uuidof, but llvm-mingw and MSVC disagree about the corners and there
    // is nothing to gain by finding out where).
    ComPtr<IFileDialog> dialog;
    const HRESULT created =
        CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr,
                         CLSCTX_INPROC_SERVER, IID_IFileDialog, dialog.put());
    if (FAILED(created) || !dialog) {
        plog().warn("file dialog: CoCreateInstance({}) failed (hr={:#010x})",
                    save ? "FileSaveDialog" : "FileOpenDialog",
                    static_cast<unsigned long>(created));
        return std::nullopt;
    }

    // Add to the shell's own defaults instead of replacing them (GetOptions-modify-SetOptions is
    // the documented pattern): the save CLSID already arrives with FOS_OVERWRITEPROMPT and the open
    // one with FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST, and a bare SetOptions drops whatever else it
    // came with. Both are re-asserted anyway, because they are contract (the header: showOpenDialog
    // "chooses an EXISTING file"; every other backend sets SAVEAS_CONFIRM) and not a default to
    // inherit by luck.
    //
    // FOS_FORCEFILESYSTEM is the one bit that is not cosmetic. Without it the user can choose a
    // shell item that has no path at all -- a search result, a device under "This PC", an
    // unhydrated OneDrive placeholder -- and the SIGDN_FILESYSPATH query below then fails on a
    // selection the user believes they made. With it the dialog refuses those items in its own UI,
    // where refusing is something the user can act on.
    FILEOPENDIALOGOPTIONS options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        options |= FOS_FORCEFILESYSTEM;
        if (save)
            options |= FOS_OVERWRITEPROMPT;
        else
            options |= FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST;
        dialog->SetOptions(options);
    }

    // The setters' HRESULTs are deliberately not checked one by one: each one that fails costs a
    // cosmetic detail (a default title, an unselected type) and none of them can make the dialog
    // wrong enough to be worth refusing to show it. Only creating the object and reading the answer
    // are load-bearing, and both are checked.
    const std::wstring title = toWide(req.title);
    if (!title.empty())
        dialog->SetTitle(title.c_str());
    // Empty accept label = the system's own button text ("Open" / "Save"), which is the header's
    // contract and also the only spelling that stays localised without us translating it.
    const std::wstring accept = toWide(req.acceptLabel);
    if (!accept.empty())
        dialog->SetOkButtonLabel(accept.c_str());

    FilterSpecs specs;
    specs.build(req.filters);
    if (!specs.empty()) {
        dialog->SetFileTypes(specs.count(), specs.data());
        // The FIRST filter is the request's default type (the header). SetFileTypeIndex is
        // ONE-BASED: 0 is not "the first entry", it is an out-of-range index the shell rejects, and
        // a rejected call leaves the dropdown wherever the shell last remembered it -- the classic
        // off-by-one here, and a silent one.
        dialog->SetFileTypeIndex(1);
        // ... and a user who types a bare "shot" still gets an extension. Taken from the default
        // type's first glob, matching what the type dropdown shows. The FLTK backends get this from
        // USE_FILTER_EXT; on Windows it is this call or nothing.
        if (save && !req.filters.front().globs.empty()) {
            const std::wstring ext = toWide(extensionFromGlob(req.filters.front().globs.front()));
            if (!ext.empty())
                dialog->SetDefaultExtension(ext.c_str());
        }
    }

    // A BASE NAME, never a path (see baseName above). Open ignores it, exactly as the header says.
    if (save) {
        const std::wstring name = toWide(baseName(req.suggestedName));
        if (!name.empty())
            dialog->SetFileName(name.c_str());
    }

    // SetFolder, not SetDefaultFolder: it forces the dialog to open where Mosaic's export-path
    // policy says, which is the point of passing startFolder to every backend ("so they cannot
    // disagree"). Microsoft's advice to prefer SetDefaultFolder is about not overriding the shell's
    // most-recently-used folder -- but io::exportStartFolder already prefers this document's last
    // export directory, so the sticky behaviour that advice protects is the behaviour we are
    // implementing, only per document instead of per application.
    //
    // An unresolvable folder leaves the call unmade rather than sending something relative. That is
    // safe here in a way it is not on Linux: the Common Item Dialog falls back to its own
    // remembered folder (else the Documents library) and never to the process working directory,
    // unlike FLTK's kdialog driver, which synthesises getcwd().
    if (const std::string folder = absoluteStartFolder(req); !folder.empty()) {
        const std::wstring wide = toWide(folder);
        ComPtr<IShellItem> item;
        const HRESULT bound =
            SHCreateItemFromParsingName(wide.c_str(), nullptr, IID_IShellItem, item.put());
        if (SUCCEEDED(bound) && item)
            dialog->SetFolder(item.get());
    }

    const HWND owner = ownerWindow(req.parent);
    plog().info("file dialog: shell {} owner={:#x}", save ? "IFileSaveDialog" : "IFileOpenDialog",
                reinterpret_cast<std::uintptr_t>(owner));

    // Blocking and modal; see the note at the top of this file. Returns only once the user has
    // answered.
    const HRESULT shown = dialog->Show(owner);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        // Cancel is an ANSWER, not an error: nothing to log and nothing to report.
        return std::nullopt;
    }
    if (FAILED(shown)) {
        plog().warn("file dialog: IFileDialog::Show failed (hr={:#010x})",
                    static_cast<unsigned long>(shown));
        return std::nullopt;
    }

    ComPtr<IShellItem> result;
    if (FAILED(dialog->GetResult(result.address())) || !result) {
        plog().warn("file dialog: the picker returned OK with no item");
        return std::nullopt;
    }

    // SIGDN_FILESYSPATH is the only display name that is a real path (the others are for showing to
    // a human). FOS_FORCEFILESYSTEM above is what makes it reliably available. The wchar_t* comes
    // out of the COM task allocator, so it is freed with CoTaskMemFree and not delete[].
    PWSTR wpath = nullptr;
    if (FAILED(result->GetDisplayName(SIGDN_FILESYSPATH, &wpath)) || wpath == nullptr) {
        plog().warn("file dialog: the chosen item has no filesystem path");
        return std::nullopt;
    }
    std::string path = fromWide(wpath);
    CoTaskMemFree(wpath);
    if (path.empty())
        return std::nullopt; // undecodable: a cancel beats handing back a bad path
    return path;
}

} // namespace

void initNativeFileDialog() {
    // No-op on Windows. It exists because the header's Phase 0 exists: on Linux this sharpens the
    // Fl_Native_File_Chooser FALLBACK (making it use kdialog on KDE instead of the GTK picker), and
    // there is no fallback here at all -- run() either gets the shell's own dialog or gets nothing.
    // Kept as a call the app makes unconditionally, so start-up has no per-platform branch.
}

bool fileDialogInFlight() noexcept {
    return g_dialogDepth > 0;
}

bool fileDialogGrabsInput() noexcept {
    // ALWAYS FALSE, and that is the whole point rather than an omission. This predicate asks the
    // narrow question "does a picker in ANOTHER PROCESS own the user's input, so our own global
    // event dispatch must swallow it?" -- and on Windows the answer can never be yes. The Common
    // Item Dialog is created in-proc with CLSCTX_INPROC_SERVER and made modal by Win32 itself:
    // Show(owner) disables the owner window for its duration, and FLTK's own set_modal() chain
    // covers the app's other windows. Modality here is enforced by the OS, not requested of a
    // compositor that the xdg-dialog spec permits to ignore it.
    //
    // Returning true would add nothing and risk everything the header warns about: the input guard
    // in app_window swallows FL_PUSH/FL_KEYBOARD/FL_SHORTCUT for every window in the process, and
    // the dialog's own message loop is what dispatches to our windows while it is up. A guard left
    // raised over an in-process modal dialog is a lockup with no way out but SIGKILL, which is
    // exactly the Fl_File_Chooser trap the header describes -- so the safe direction is not to
    // raise it at all. fileDialogInFlight() stays true either way, which is what the quit guard
    // (a WM_CLOSE arriving mid-dialog) actually needs.
    return false;
}

std::optional<std::string> showSaveDialog(const FileDialogRequest& req) {
    return run(req, /*save=*/true);
}

std::optional<std::string> showOpenDialog(const FileDialogRequest& req) {
    return run(req, /*save=*/false);
}

} // namespace mosaic::platform
