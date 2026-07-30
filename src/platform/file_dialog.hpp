#pragma once

#include <optional>
#include <string>
#include <vector>

class Fl_Window; // forward-declared: this header stays free of FLTK/D-Bus includes.

// platform::FileDialog -- the one native file dialog that powers File->Open, Quick Export -> PNG,
// the Export modal's [...] button, and (later) Save. FLTK's own Fl_Native_File_Chooser defaults to
// the GTK picker on every desktop (its zenity/kdialog backends are opt-in), which is the "wrong
// dialog" the user hit. We route through the XDG Desktop Portal instead -- the desktop's real
// file dialog on X11/XWayland + native Wayland + GNOME + KDE -- and keep the native chooser as the
// fallback when the portal is unreachable (Export & I/O plan, section 8).
//
// ONE HEADER, THREE BACKENDS, exactly one of which platform/CMakeLists.txt compiles:
// file_dialog.cpp is the Linux one described above, file_dialog_macos.cpp the Cocoa panels (S58),
// and file_dialog_win32.cpp the Windows shell's Common Item Dialog (S57). Everything below is the
// contract all three honour; where one of them cannot, it says so here and again in its own file.
namespace mosaic::platform {

// One entry in a dialog's type filter, e.g. "PNG image" -> *.png / image/png. The portal takes
// both shell globs and MIME types; the Fl_Native_File_Chooser fallback uses the globs.
//
// NB: only add mimeTypes when they describe the SAME file set as the globs. KDE's picker applies
// the mime list INSTEAD of the globs when a filter carries both (KFileWidget's filter-changed
// path checks mimePatterns first and never reaches the glob name-filter) -- so a filter mixing
// image mimes with a *.mosaic glob showed no .mosaic files at all (user report 2026-07-16).
struct FileFilter {
    std::string name;                   // human label, "PNG image"
    std::vector<std::string> globs;     // {"*.png"} (at least one)
    std::vector<std::string> mimeTypes; // {"image/png"} (optional; portal only; see NB above)
};

struct FileDialogRequest {
    std::string title = "Open";      // dialog window title
    std::string acceptLabel;         // primary-button text ("Export"); empty = the system default
    // Save only: the pre-filled file NAME ("untitled.png"). A base name, not a path and not a
    // URI -- every backend pastes it straight into the dialog's name field. A path handed here is
    // reduced to its last component rather than shown whole.
    std::string suggestedName;
    // The directory to open in. MUST be absolute; a relative value (or none) falls back to $HOME,
    // never to the process working directory -- see io::exportStartFolder, which is what the app
    // resolves this from. Passed to all three backends (portal `current_folder`, kdialog's start
    // argument, Fl_Native_File_Chooser::directory) so they cannot disagree.
    std::string startFolder;
    // Type filters. The FIRST one is the dialog's default type -- sent to the portal as
    // current_filter, which both selects it and stops KDE's picker synthesizing its own
    // "All Supported Files" union entry on top of the list.
    std::vector<FileFilter> filters;
    Fl_Window* parent = nullptr;     // transient-for parent, so the dialog stacks/centres correctly
};

// Phase 0 (call once at start-up, before the first dialog): make the Fl_Native_File_Chooser
// FALLBACK use kdialog on KDE instead of the default GTK picker. A no-op off KDE. Safe to always
// call. The portal (Phase 1) is still the primary path; this only sharpens the fallback.
void initNativeFileDialog();

// True while a showOpen/showSaveDialog call is on the stack -- i.e. its reply is being pumped
// through a nested FLTK loop and the parent window is deactivated behind the picker. The app's
// quit paths consult this to stay MODAL behind the dialog: hiding the parent out from under that
// nested loop leaves it spinning on a reply that can never come (user report: quitting while a
// picker is open froze the app hard, only pkill escaped). A no-op wrapper so callers need not
// know how the flag is stored.
[[nodiscard]] bool fileDialogInFlight() noexcept;

// True while an OUT-OF-PROCESS picker (the XDG portal dialog, or our directly spawned kdialog)
// owns the user's input -- the narrower question a global input guard must ask.
//
// It is NOT the same as fileDialogInFlight(). The last-resort backend is FLTK's OWN Fl_File_Chooser,
// an in-process FLTK window (Fl_Native_File_Chooser falls back to it when GTK, zenity and kdialog
// are all absent). Swallowing input on fileDialogInFlight() would leave THAT chooser on screen,
// modal, and completely dead -- a total lockup with no way out but SIGKILL. Input may only be
// blocked while the dialog the user is looking at belongs to another process.
//
// The three backends therefore answer differently, and the difference is the whole reason this is a
// separate predicate: Linux says yes for the portal/kdialog pickers and no for FLTK's own chooser,
// macOS says yes (the Cocoa panel is app-modal and never reaches Fl::handle()), and WINDOWS ALWAYS
// SAYS NO -- there the shell's dialog is created in-process and Show() disables the owner window
// itself, so modality is the OS's, and raising the guard on top of it would only risk the
// in-process lockup described above for no gain.
[[nodiscard]] bool fileDialogGrabsInput() noexcept;

// Show a modal "save/export" dialog and return the chosen path (URI-decoded, absolute), or nullopt
// if the user cancelled. On X11 + KDE that is a directly spawned `kdialog --attach` (the only
// picker the window manager will make a real modal transient of our X11 window); otherwise the XDG
// portal; and the native chooser when neither is reachable.
//
// NEVER BLOCKS THE MAIN LOOP. Every one of those waits is pumped through Fl::add_fd + a nested
// Fl::wait(), so the canvas keeps painting, timers keep firing and the window keeps behaving like a
// live application for however long the picker is up. (The X11+KDE branch used to block the thread
// outright, which is what "the export picker freezes the whole program" was.) Input to our own
// windows is blocked for the duration instead (see fileDialogGrabsInput), so "modal" is something
// we enforce rather than merely ask of the compositor.
//
// ⚠ THAT PARAGRAPH IS TRUE OF THE LINUX BACKEND ONLY. The macOS and Windows pickers are
// in-process, app-modal and synchronous: they run the OS's own event loop, which keeps our windows
// painted and answering the window manager -- the part of the promise that mattered, and the reason
// neither needs a nested pump -- but Mosaic's own timers do not fire, so the canvas holds its last
// frame instead of animating. Neither needs to enforce modality itself either (see
// fileDialogGrabsInput); the OS disables the owner window for them.
//
// A SECOND CALL WHILE ONE IS UP IS REFUSED (returns nullopt immediately): the nested pump makes
// re-entrancy reachable, and two stacked pickers cannot keep the modality bookkeeping straight.
[[nodiscard]] std::optional<std::string> showSaveDialog(const FileDialogRequest& req);

// As showSaveDialog, but chooses an EXISTING file to open (suggestedName is ignored).
[[nodiscard]] std::optional<std::string> showOpenDialog(const FileDialogRequest& req);

#if !defined(__APPLE__) && !defined(_WIN32)
// The pure, side-effect-free halves of the Linux picker, exposed ONLY so tests/test_file_dialog.cpp
// can pin them: every one of them has already shipped a user-visible bug (the "http://asdf.png"
// start location, the CWD default, a path pasted into a name field). The portal round-trip and the
// spawned kdialog are NOT testable headlessly -- they need a session bus and a desktop -- so they
// stay file-local and the user's interactive pass is what covers them. Neither sibling TU
// implements any of this, hence the guard: macOS is Fl_Native_File_Chooser only, and Windows drives
// the shell's own IFileDialog, whose inputs are not these. (Nor could they be shared. Every helper
// here answers a POSIX question -- an absolute path starts with '/', a chosen file arrives as a
// file:// URI, a filter is a brace group or a kdialog pipe string -- and the Windows answers to all
// three differ, so a shared "detail" would only be a name two platforms disagree under.)
namespace detail {

// The last component of a path ("asdf.png" out of "/home/u/asdf.png"), or the whole string when it
// has no separator. The portal's `current_name` and the native chooser's preset_file both want a
// BASE NAME, never a path and never a URI.
[[nodiscard]] std::string baseNameOf(const std::string& path);

// The ABSOLUTE directory to open the picker in: req.startFolder when it is absolute, else
// `fallbackHome` (the caller passes $HOME). NEVER the process working directory -- see the long
// note in the .cpp for what a relative value does to kdialog.
[[nodiscard]] std::string absoluteStartFolder(const FileDialogRequest& req,
                                              const std::string& fallbackHome);

// Decode a file:// URI (as returned by the portal) into a local filesystem path.
[[nodiscard]] std::string uriToPath(const std::string& uri);

// FLTK filter syntax: "Label\t*.{png,jpg}\nLabel2\t*".
[[nodiscard]] std::string fltkFilterString(const std::vector<FileFilter>& filters);

// kdialog filter syntax: "<space-separated globs>|<label>", one per line.
[[nodiscard]] std::string kdialogFilter(const std::vector<FileFilter>& filters);

// kdialog's positional start-location argument, which MUST come out absolute.
[[nodiscard]] std::string kdialogStartArgument(const FileDialogRequest& req, bool save,
                                               const std::string& fallbackHome);

// Raises the "a picker is up" flag for its lifetime -- exactly what showSave/showOpenDialog do
// around their wait. Public only so a headless test can prove that the RE-ENTRANCY REFUSAL works
// (a second picker asked for while one is up must return nullopt without spawning anything).
struct InFlightScope {
    InFlightScope() noexcept;
    ~InFlightScope() noexcept;
    InFlightScope(const InFlightScope&) = delete;
    InFlightScope& operator=(const InFlightScope&) = delete;
};

} // namespace detail
#endif // !__APPLE__ && !_WIN32

} // namespace mosaic::platform
