#include "platform/file_dialog.hpp"

// macOS implementation of platform/file_dialog.hpp (PLAN.md S58), used INSTEAD of file_dialog.cpp
// on Apple (see platform/CMakeLists.txt). There is no XDG Desktop Portal / kdialog / sd-bus on
// macOS; Fl_Native_File_Chooser already maps to the native Cocoa NSOpenPanel / NSSavePanel, which
// IS the system dialog we want. So this file is just the Fl_Native_File_Chooser path -- the same
// fallback the Linux build keeps for when the portal is unreachable.

#include <FL/Fl.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_Window.H>

#include <string>
#include <vector>

namespace mosaic::platform {
namespace {

// FLTK filter syntax: "Label\t*.{png,jpg}\nLabel2\t*". Collapse each FileFilter's globs into one
// brace pattern (or "*" if it matches everything). (Same rule as the Linux nativeFallback path.)
std::string fltkFilterString(const std::vector<FileFilter>& filters) {
    std::string out;
    for (const FileFilter& f : filters) {
        if (f.globs.empty())
            continue;
        std::string exts;
        bool matchAll = false;
        for (const std::string& g : f.globs) {
            if (g == "*") {
                matchAll = true;
                break;
            }
            const std::string ext = (g.rfind("*.", 0) == 0) ? g.substr(2) : g;
            if (!exts.empty())
                exts += ',';
            exts += ext;
        }
        // A SINGLE extension must be "*.ext", not "*.{ext}": with USE_FILTER_EXT the macOS
        // Fl_Native_File_Chooser parses the brace form wrong and leaks it into the saved name
        // ("untitled.mosaic}.mosaic"). Braces are only for a multi-extension set.
        std::string pattern;
        if (matchAll)
            pattern = "*";
        else if (exts.find(',') == std::string::npos)
            pattern = "*." + exts;
        else
            pattern = "*.{" + exts + "}";
        if (!out.empty())
            out += '\n';
        out += f.name + "\t" + pattern;
    }
    return out;
}

// >0 while a dialog is on the stack (see fileDialogInFlight). Fl_Native_File_Chooser::show() is
// app-modal and blocking on macOS, so this is only briefly set, but the app's quit guard consults
// it exactly as on Linux.
int g_dialogDepth = 0;

struct DialogGuard {
    Fl_Window* parent;
    explicit DialogGuard(Fl_Window* p) : parent(p) {
        ++g_dialogDepth;
        if (parent != nullptr && parent->shown() != 0)
            parent->deactivate();
    }
    ~DialogGuard() {
        if (parent != nullptr && parent->shown() != 0)
            parent->activate();
        --g_dialogDepth;
    }
    DialogGuard(const DialogGuard&) = delete;
    DialogGuard& operator=(const DialogGuard&) = delete;
};

std::optional<std::string> run(const FileDialogRequest& req, bool save) {
    DialogGuard guard(req.parent);
    Fl_Native_File_Chooser chooser;
    chooser.title(req.title.c_str());
    chooser.type(save ? Fl_Native_File_Chooser::BROWSE_SAVE_FILE
                      : Fl_Native_File_Chooser::BROWSE_FILE);
    if (save)
        chooser.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM |
                        Fl_Native_File_Chooser::USE_FILTER_EXT);
    const std::string filter = fltkFilterString(req.filters);
    if (!filter.empty())
        chooser.filter(filter.c_str());
    if (!req.startFolder.empty())
        chooser.directory(req.startFolder.c_str());
    if (save && !req.suggestedName.empty())
        chooser.preset_file(req.suggestedName.c_str());
    if (chooser.show() != 0) // 1 = cancel, -1 = error
        return std::nullopt;
    const char* picked = chooser.filename();
    if (picked == nullptr || picked[0] == '\0')
        return std::nullopt;
    return std::string(picked);
}

} // namespace

void initNativeFileDialog() {
    // No-op on macOS: Fl_Native_File_Chooser is already the native Cocoa panel.
}

bool fileDialogInFlight() noexcept {
    return g_dialogDepth > 0;
}

bool fileDialogGrabsInput() noexcept {
    // Same answer as fileDialogInFlight() here. The Linux build has to distinguish the two because
    // its last-resort backend is an in-process FLTK window that needs its own events; on macOS the
    // only backend is the Cocoa NSOpenPanel/NSSavePanel, which is app-modal and never routes
    // through Fl::handle() at all.
    return g_dialogDepth > 0;
}

std::optional<std::string> showSaveDialog(const FileDialogRequest& req) {
    return run(req, /*save=*/true);
}

std::optional<std::string> showOpenDialog(const FileDialogRequest& req) {
    return run(req, /*save=*/false);
}

} // namespace mosaic::platform
