#include "platform/file_dialog.hpp"

#include "common/log.hpp"
#include "platform/native_window.hpp" // nativeSurfaceHandle (X11 xid without dragging in Xlib here)
#include "platform/wayland_foreign.hpp" // the native-Wayland xdg_foreign parent handle

#include <FL/Fl.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_Window.H>

#include <systemd/sd-bus.h>

#include <fcntl.h>    // FD_CLOEXEC / O_NONBLOCK on the kdialog pipe
#include <signal.h>   // kill (POSIX; not something <csignal> promises)
#include <spawn.h>    // posix_spawnp -- see the note on runKdialog for why NOT fork()+execvp()
#include <sys/wait.h> // waitpid for the directly-spawned kdialog
#include <unistd.h>   // pipe / read / close

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// The environment posix_spawnp hands the child. Declared here rather than leaned on from
// <unistd.h>, whose declaration of it is feature-test-macro gated.
extern "C" char** environ;

namespace mosaic::platform {

namespace {

spdlog::logger& plog() {
    static const auto logger = common::log::category("platform");
    return *logger;
}

// $HOME, or "" when unset. Deliberately NOT getcwd(): the process working directory is wherever
// the binary happened to be launched from and is never a place the user meant to write to (the
// export-path policy in io/export_path.hpp exists for exactly this reason). platform/ does not
// depend on io/, so the last-resort rule is spelled out again here rather than shared.
std::string homeDirectory() {
    const char* home = std::getenv("HOME");
    return (home != nullptr && home[0] == '/') ? std::string(home) : std::string{};
}

} // namespace

// ---- pure helpers (declared in the header's detail namespace, so tests can pin them) -----------

namespace detail {

std::string baseNameOf(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// This absoluteness is not cosmetic. kdialog parses its start-location argument with
// QUrl::fromUserInput(), which checks QDir::isAbsolutePath() FIRST and otherwise falls through to
// its "the user typed a web address" heuristic -- so a relative "asdf.png" comes back as
// http://asdf.png and lands in the dialog's file-name field (user report, X11 + KDE). An absolute
// path takes the QUrl::fromLocalFile() branch and cannot be mistaken for anything.
std::string absoluteStartFolder(const FileDialogRequest& req, const std::string& fallbackHome) {
    if (!req.startFolder.empty() && req.startFolder.front() == '/') // this TU is the POSIX one
        return req.startFolder;
    return fallbackHome;
}

// Handles the optional host component and percent-encoding; portals may hand back a
// /run/user/<uid>/doc/... FUSE path, which is a real path we can open.
std::string uriToPath(const std::string& uri) {
    std::string s = uri;
    const std::string scheme = "file://";
    if (s.rfind(scheme, 0) == 0) {
        s.erase(0, scheme.size());
        const std::size_t slash = s.find('/'); // strip an optional host before the path
        if (slash == std::string::npos)
            return {};
        s.erase(0, slash);
    }
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const int hi = hex(s[i + 1]);
            const int lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

// Collapse each FileFilter's globs into one brace pattern (or "*" if it matches everything).
std::string fltkFilterString(const std::vector<FileFilter>& filters) {
    std::string out;
    for (const FileFilter& f : filters) {
        if (f.globs.empty())
            continue;
        std::string pattern;
        bool matchAll = false;
        std::string exts;
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
        pattern = matchAll ? "*" : ("*.{" + exts + "}");
        if (!out.empty())
            out += '\n';
        out += f.name + "\t" + pattern;
    }
    return out;
}

std::string kdialogFilter(const std::vector<FileFilter>& filters) {
    std::string out;
    for (const FileFilter& f : filters) {
        if (f.globs.empty())
            continue;
        std::string globs;
        for (const std::string& g : f.globs) {
            if (!globs.empty())
                globs += ' ';
            globs += g;
        }
        if (!out.empty())
            out += '\n';
        out += globs + '|' + f.name;
    }
    return out;
}

// kdialog hands its positional argument to QUrl::fromUserInput(). That function tries, in order:
// an absolute path (-> QUrl::fromLocalFile, the branch we want), then -- with no working
// directory supplied, which is kdialog's case -- the "the user typed a web address" heuristic,
// which prepends "http://" to anything that parses as a host. So the old code's
//   save && startFolder.empty()  ->  startArg = "asdf.png"
// produced QUrl("http://asdf.png"), and Plasma's picker showed exactly that in its file-name
// field (user report, X11 + KDE). The other old spelling, ".", is no better: QUrl::fromUserInput
// (".") is an EMPTY url, so the start location was silently dropped -- and it meant the process
// working directory in the first place, which is never where an export belongs.
//
// Both spellings are gone: the folder comes from absoluteStartFolder() (the app's export-path
// policy, else $HOME), and the file name is appended as a base name for a save. A trailing '/'
// on the open form marks it unambiguously as a directory.
std::string kdialogStartArgument(const FileDialogRequest& req, bool save,
                                 const std::string& fallbackHome) {
    std::string arg = absoluteStartFolder(req, fallbackHome);
    if (arg.empty())
        arg = "/"; // absolute above all else: never "." and never a bare name
    if (arg.back() != '/')
        arg += '/';
    if (save)
        arg += baseNameOf(req.suggestedName); // "" is fine: the directory alone is still absolute
    return arg;
}

} // namespace detail

namespace {

// ---- small helpers -----------------------------------------------------------------------------

bool runningKde() {
    const char* xdg = std::getenv("XDG_CURRENT_DESKTOP");
    if (xdg != nullptr && std::strstr(xdg, "KDE") != nullptr)
        return true;
    return std::getenv("KDE_FULL_SESSION") != nullptr;
}

// The X11/XWayland parent-window identifier the portal wants: "x11:<hex-xid>". Native Wayland needs a
// zxdg_exporter_v2 "wayland:<handle>" instead -- runPortal obtains that from WaylandForeignExport and
// only falls through to this helper (which returns "" off X11) for the x11 path. "" is valid either
// way: the dialog still opens, just un-parented. Since S59-a Mosaic defaults to NATIVE WAYLAND
// (preferWaylandBackendIfUnset), so the "wayland:<handle>" path is now the common one and this
// helper serves a pure-Xorg session or the FLTK_BACKEND=x11 escape hatch -- it used to be the other
// way round. See docs/wayland.md.
std::string parentWindowHandle(Fl_Window* win) {
    if (win == nullptr || win->shown() == 0)
        return {};
    NativeSurfaceHandle nh;
    std::string err;
    if (!nativeSurfaceHandle(win, nh, err) || nh.system != WindowSystem::X11 || nh.window == nullptr)
        return {};
    const auto xid = reinterpret_cast<std::uintptr_t>(nh.window);
    char buf[32];
    std::snprintf(buf, sizeof buf, "x11:%lx", static_cast<unsigned long>(xid));
    return buf;
}

// The ABSOLUTE directory to open the picker in, with $HOME as the last resort.
std::string startFolderFor(const FileDialogRequest& req) {
    return detail::absoluteStartFolder(req, homeDirectory());
}

// ---- nested-wait machinery ---------------------------------------------------------------------
//
// Everything below exists because BOTH out-of-process pickers (the XDG portal dialog and our
// directly spawned kdialog) are waited for from a NESTED Fl::wait() loop rather than by blocking
// the thread. A main loop that stops turning is an application that has stopped existing as far as
// the user can tell: no expose handling, no resize, no frame timer (so no Vulkan present -- the
// canvas is whatever was last on screen), no autosave journal tick, no async-job poll, and every
// click and keystroke piling up unread in the X11 socket to be replayed in a burst afterwards.
//
// A nested loop is re-entrant by construction, so it needs three guarantees, one per helper here:
// nothing it dispatches may reach back into the app (EventSwallow), every descriptor it registers
// comes back off on every exit path including a throw (FdWatch), and a turn of the loop can never
// cost zero time (pumpFltkOnce).

// A turn of a nested wait. BOUNDED on purpose: a bare Fl::wait() returns instantly, for ever, in
// two states that are easy to reach -- no window left to wait on, and a descriptor that poll()
// reports as POLLNVAL because its owner closed it behind FLTK's back. Either turns the loop below
// into a 100% CPU spin that the user experiences as a dead, unkillable-looking app. 20 Hz of
// polling costs nothing next to the 60 Hz frame timer this loop exists to keep running.
constexpr double kPumpIntervalSec = 0.05;

void pumpFltkOnce() {
    Fl::wait(kPumpIntervalSec);
}

// Swallow every FLTK event for the object's lifetime.
//
// THE RE-ENTRANCY RULE FOR THIS FILE. The nested pump dispatches everything the app would normally
// act on: a menu pick, a second Ctrl+O, Escape (FLTK routes it to the modal window's callback --
// for the Export modal that is doCancel() -> hide()), the window-manager close button (FL_CLOSE), a
// file drop. Any of those re-enters the app underneath a picker that is still on screen: at best a
// second nested loop and a second D-Bus connection, at worst a parent window torn down while this
// stack frame still holds a pointer to it.
//
// Fl::event_dispatch() replaces Fl::handle()'s delivery step wholesale, so a dispatcher that
// returns 0 drops every window event while leaving timeouts, Fl::add_fd handlers and Fl::flush()
// -- i.e. redraws and the frame timer -- running. That is exactly the "modal, but still painting"
// shape wanted here, and it is FLTK's own idiom for it (Fl_Native_File_Chooser_Kdialog.cxx installs
// the same swallow-all dispatcher while it pumps).
//
// NOT installed around the Fl_Native_File_Chooser fallback: FLTK's last-resort backend is its own
// in-process Fl_File_Chooser window, which needs its events (see fileDialogGrabsInput).
struct EventSwallow {
    Fl_Event_Dispatch previous;
    EventSwallow() : previous(Fl::event_dispatch()) { Fl::event_dispatch(&swallow); }
    ~EventSwallow() { Fl::event_dispatch(previous); }
    EventSwallow(const EventSwallow&) = delete;
    EventSwallow& operator=(const EventSwallow&) = delete;
    static int swallow(int /*event*/, Fl_Window* /*win*/) { return 0; }
};

// Fl::add_fd / Fl::remove_fd as an RAII pair. FLTK keeps a raw descriptor NUMBER in its poll set;
// leaving one behind after its owner closed it is not a leak but a live fault -- poll() reports
// POLLNVAL on it every turn (spin), and the moment the number is recycled by anything else in the
// process FLTK starts invoking our handler on somebody else's descriptor.
struct FdWatch {
    int fd;
    FdWatch(int f, Fl_FD_Handler cb, void* data) : fd(f) { Fl::add_fd(fd, cb, data); }
    ~FdWatch() { Fl::remove_fd(fd); }
    FdWatch(const FdWatch&) = delete;
    FdWatch& operator=(const FdWatch&) = delete;
};

// ---- Fl_Native_File_Chooser fallback ----------------------------------------------------------

// >0 while an IN-PROCESS FLTK chooser may be on screen (see fileDialogGrabsInput).
int g_inProcessChooserDepth = 0;

struct InProcessChooserScope {
    InProcessChooserScope() { ++g_inProcessChooserDepth; }
    ~InProcessChooserScope() { --g_inProcessChooserDepth; }
    InProcessChooserScope(const InProcessChooserScope&) = delete;
    InProcessChooserScope& operator=(const InProcessChooserScope&) = delete;
};

std::optional<std::string> nativeFallback(const FileDialogRequest& req, bool save) {
    // Fl_Native_File_Chooser picks its own backend: GTK, then zenity, then kdialog, then -- when
    // none of those exists -- FLTK's own Fl_File_Chooser, an ordinary FLTK window in THIS process.
    // Input must not be blocked while that one is up or it is unusable, so the flag the app's
    // input guard reads is dropped for the duration. fileDialogInFlight() (the QUIT guard) stays
    // raised: FLTK's chooser is modal, but the app still must not tear its own windows down here.
    const InProcessChooserScope inProcess;

    Fl_Native_File_Chooser chooser;
    chooser.title(req.title.c_str());
    chooser.type(save ? Fl_Native_File_Chooser::BROWSE_SAVE_FILE
                      : Fl_Native_File_Chooser::BROWSE_FILE);
    if (save)
        chooser.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM |
                        Fl_Native_File_Chooser::USE_FILTER_EXT);
    const std::string filter = detail::fltkFilterString(req.filters);
    if (!filter.empty())
        chooser.filter(filter.c_str());
    // ALWAYS set a directory, even when the caller gave none. FLTK's own kdialog driver builds its
    // start location as (directory ? directory : getcwd()) + "/" + preset_file
    // (Fl_Native_File_Chooser_Kdialog.cxx), so leaving it unset is precisely how the process
    // working directory became the default export folder. The GTK driver is well-behaved but takes
    // the same value happily.
    const std::string dir = startFolderFor(req);
    if (!dir.empty())
        chooser.directory(dir.c_str());
    const std::string name = detail::baseNameOf(req.suggestedName);
    if (save && !name.empty())
        chooser.preset_file(name.c_str());
    if (chooser.show() != 0) // 1 = cancel, -1 = error
        return std::nullopt;
    const char* picked = chooser.filename();
    if (picked == nullptr || picked[0] == '\0')
        return std::nullopt;
    return std::string(picked);
}

// ---- XDG Desktop Portal (org.freedesktop.portal.FileChooser) -----------------------------------

enum class PortalStatus { Ok, Cancelled, Unavailable };
struct PortalResult {
    PortalStatus status = PortalStatus::Unavailable;
    std::string path;
};

// State shared with the async signal handlers.
struct ResponseState {
    bool done = false;
    bool broken = false;        // the reply can NEVER arrive now -- stop waiting for it
    std::uint32_t response = 1; // 0 = ok, 1 = cancelled, 2 = other
    std::string uri;
};

// sd-bus ownership, so that no early return can leak a connection, a match slot or a message --
// and so that the ORDER of teardown is not something a future edit has to remember.
struct BusHandle {
    sd_bus* bus = nullptr;
    BusHandle() = default;
    ~BusHandle() { sd_bus_unref(bus); }
    BusHandle(const BusHandle&) = delete;
    BusHandle& operator=(const BusHandle&) = delete;
};
struct SlotHandle {
    sd_bus_slot* slot = nullptr;
    SlotHandle() = default;
    ~SlotHandle() { sd_bus_slot_unref(slot); }
    SlotHandle(const SlotHandle&) = delete;
    SlotHandle& operator=(const SlotHandle&) = delete;
};
struct MessageHandle {
    sd_bus_message* msg = nullptr;
    MessageHandle() = default;
    ~MessageHandle() { sd_bus_message_unref(msg); }
    MessageHandle(const MessageHandle&) = delete;
    MessageHandle& operator=(const MessageHandle&) = delete;
};

// Append one "{sv}" option entry with a string value.
void appendStringOption(sd_bus_message* m, const char* key, const char* value) {
    sd_bus_message_open_container(m, 'e', "sv");
    sd_bus_message_append(m, "s", key);
    sd_bus_message_open_container(m, 'v', "s");
    sd_bus_message_append(m, "s", value);
    sd_bus_message_close_container(m);
    sd_bus_message_close_container(m);
}

// Append one "{sv}" option entry whose value is a portal PATH: the FileChooser spec types
// current_folder / current_file as "ay" -- a NUL-TERMINATED byte array, not a string and not a
// URI. (Getting this wrong is silent: the portal simply ignores an option it cannot read.)
void appendPathOption(sd_bus_message* m, const char* key, const std::string& path) {
    sd_bus_message_open_container(m, 'e', "sv");
    sd_bus_message_append(m, "s", key);
    sd_bus_message_open_container(m, 'v', "ay");
    sd_bus_message_append_array(m, 'y', path.c_str(), path.size() + 1); // include the NUL
    sd_bus_message_close_container(m);
    sd_bus_message_close_container(m);
}

void appendBoolOption(sd_bus_message* m, const char* key, bool value) {
    sd_bus_message_open_container(m, 'e', "sv");
    sd_bus_message_append(m, "s", key);
    sd_bus_message_open_container(m, 'v', "b");
    const int v = value ? 1 : 0;
    sd_bus_message_append(m, "b", v);
    sd_bus_message_close_container(m);
    sd_bus_message_close_container(m);
}

// One filter struct: (sa(us)) -- (name, [(type, pattern)]) with type 0 = glob, 1 = mime.
void appendFilterStruct(sd_bus_message* m, const FileFilter& f) {
    sd_bus_message_open_container(m, 'r', "sa(us)");
    sd_bus_message_append(m, "s", f.name.c_str());
    sd_bus_message_open_container(m, 'a', "(us)");
    for (const std::string& g : f.globs) {
        sd_bus_message_open_container(m, 'r', "us");
        sd_bus_message_append(m, "us", static_cast<std::uint32_t>(0), g.c_str());
        sd_bus_message_close_container(m);
    }
    for (const std::string& mt : f.mimeTypes) {
        sd_bus_message_open_container(m, 'r', "us");
        sd_bus_message_append(m, "us", static_cast<std::uint32_t>(1), mt.c_str());
        sd_bus_message_close_container(m);
    }
    sd_bus_message_close_container(m); // (us) array
    sd_bus_message_close_container(m); // struct
}

// The "filters" option: a(sa(us)), the dialog's type dropdown.
void appendFiltersOption(sd_bus_message* m, const std::vector<FileFilter>& filters) {
    sd_bus_message_open_container(m, 'e', "sv");
    sd_bus_message_append(m, "s", "filters");
    sd_bus_message_open_container(m, 'v', "a(sa(us))");
    sd_bus_message_open_container(m, 'a', "(sa(us))");
    for (const FileFilter& f : filters)
        appendFilterStruct(m, f);
    sd_bus_message_close_container(m); // a(sa(us))
    sd_bus_message_close_container(m); // variant
    sd_bus_message_close_container(m); // dict entry
}

// The "current_filter" option: (sa(us)), the type selected at open. Always sent (as the request's
// FIRST filter): without it KDE's picker treats the list as default-less and synthesizes its own
// "All Supported Files" union entry on top of ours (KFileFilterCombo::setFilters adds it whenever
// 2+ filters arrive with an empty default -- a set default suppresses it), which duplicated the
// combined type we already provide. GNOME's picker simply honours it as the initial selection.
void appendCurrentFilterOption(sd_bus_message* m, const FileFilter& f) {
    sd_bus_message_open_container(m, 'e', "sv");
    sd_bus_message_append(m, "s", "current_filter");
    sd_bus_message_open_container(m, 'v', "(sa(us))");
    appendFilterStruct(m, f);
    sd_bus_message_close_container(m); // variant
    sd_bus_message_close_container(m); // dict entry
}

// The Request.Response signal handler: (u response, a{sv} results); pull results["uris"][0].
int onResponse(sd_bus_message* m, void* userdata, sd_bus_error* /*retError*/) {
    auto* st = static_cast<ResponseState*>(userdata);
    if (st->done)
        return 0; // both matches (predicted + returned path) are live; take the first answer only
    if (sd_bus_message_read(m, "u", &st->response) < 0) {
        st->done = true;
        return 0;
    }
    if (st->response == 0 && sd_bus_message_enter_container(m, 'a', "{sv}") > 0) {
        while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
            const char* key = nullptr;
            sd_bus_message_read(m, "s", &key);
            if (key != nullptr && std::strcmp(key, "uris") == 0 &&
                sd_bus_message_enter_container(m, 'v', "as") > 0) {
                if (sd_bus_message_enter_container(m, 'a', "s") > 0) {
                    const char* uri = nullptr;
                    if (sd_bus_message_read(m, "s", &uri) > 0 && uri != nullptr)
                        st->uri = uri; // the first (only, for single-select) URI
                    sd_bus_message_exit_container(m);
                }
                sd_bus_message_exit_container(m); // variant
            } else {
                sd_bus_message_skip(m, "v");
            }
            sd_bus_message_exit_container(m); // dict entry
        }
        sd_bus_message_exit_container(m); // array
    }
    st->done = true;
    return 0;
}

// NameOwnerChanged for org.freedesktop.portal.Desktop with an empty new owner: the portal service
// is gone. Whatever dialog it had on screen went with it, and the Response we are waiting for can
// never be emitted -- so without this the nested pump has no way of ever learning that and waits
// for ever. (This is one of the two "the app just locks up with a picker that isn't there any
// more" paths; the other is a dead connection, handled in drainBus.)
int onPortalVanished(sd_bus_message* m, void* userdata, sd_bus_error* /*retError*/) {
    auto* st = static_cast<ResponseState*>(userdata);
    const char* name = nullptr;
    const char* oldOwner = nullptr;
    const char* newOwner = nullptr;
    if (sd_bus_message_read(m, "sss", &name, &oldOwner, &newOwner) < 0)
        return 0;
    if (name == nullptr || std::strcmp(name, "org.freedesktop.portal.Desktop") != 0)
        return 0;
    if (newOwner == nullptr || newOwner[0] == '\0') {
        st->broken = true;
        st->done = true;
    }
    return 0;
}

// Dispatch every message sd-bus has for us. Returns false when the connection is finished with.
//
// That return value is the difference between "the picker was cancelled" and "the app is wedged at
// 100% CPU". On a peer disconnect sd-bus CLOSES the connection's descriptor -- while FLTK is still
// polling it. poll() then reports POLLNVAL on that descriptor on every single turn, so the wait
// below returns instantly for ever while sd_bus_process keeps answering -ECONNRESET. Swallowing
// that error (which the first version of this file did) is an unbreakable busy-spin waiting for a
// reply that cannot exist.
bool drainBus(sd_bus* bus, ResponseState& st) {
    for (;;) {
        const int r = sd_bus_process(bus, nullptr);
        if (r < 0) {
            st.broken = true;
            st.done = true;
            return false;
        }
        if (r == 0 || st.done)
            break;
    }
    if (sd_bus_is_open(bus) <= 0) {
        st.broken = true;
        st.done = true;
        return false;
    }
    return true;
}

struct BusPump {
    sd_bus* bus = nullptr;
    ResponseState* state = nullptr;
};

// Drain the bus whenever its fd is readable (registered with Fl::add_fd for the nested wait).
void busFdReady(FL_SOCKET /*fd*/, void* data) {
    auto* p = static_cast<BusPump*>(data);
    drainBus(p->bus, *p->state);
}

PortalResult runPortal(const FileDialogRequest& req, bool save) {
    PortalResult result;

    BusHandle bus;
    if (sd_bus_open_user(&bus.bus) < 0 || bus.bus == nullptr)
        return result; // no session bus -> Unavailable

    const char* unique = nullptr;
    if (sd_bus_get_unique_name(bus.bus, &unique) < 0 || unique == nullptr)
        return result;

    // The Request object path is predictable: /.../request/<SENDER>/<TOKEN>, SENDER = our unique
    // name with the leading ':' dropped and '.' -> '_'. Subscribe on it BEFORE the call so the
    // Response can never race ahead of the match.
    std::string sender = (unique[0] == ':') ? unique + 1 : unique;
    for (char& c : sender)
        if (c == '.')
            c = '_';
    static unsigned counter = 0;
    const std::string token = "mosaic" + std::to_string(counter++);
    const std::string predictedPath =
        "/org/freedesktop/portal/desktop/request/" + sender + "/" + token;

    ResponseState state;
    SlotHandle predictedMatch;
    if (sd_bus_match_signal(bus.bus, &predictedMatch.slot, "org.freedesktop.portal.Desktop",
                            predictedPath.c_str(), "org.freedesktop.portal.Request", "Response",
                            onResponse, &state) < 0)
        return result;

    // ... and watch the portal's bus name, so a portal that dies (or restarts under a new owner)
    // ends the wait instead of stranding it for ever. See onPortalVanished. Best-effort: a bus
    // that will not take this match is one we are about to fail on anyway.
    SlotHandle ownerMatch;
    sd_bus_add_match(bus.bus, &ownerMatch.slot,
                     "type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',"
                     "interface='org.freedesktop.DBus',member='NameOwnerChanged',"
                     "arg0='org.freedesktop.portal.Desktop'",
                     onPortalVanished, &state);

    MessageHandle call;
    if (sd_bus_message_new_method_call(bus.bus, &call.msg, "org.freedesktop.portal.Desktop",
                                       "/org/freedesktop/portal/desktop",
                                       "org.freedesktop.portal.FileChooser",
                                       save ? "SaveFile" : "OpenFile") < 0)
        return result;

    // Parent the dialog to our window so the compositor stacks + modals it. Native Wayland needs an
    // xdg_foreign handle (WaylandForeignExport -- kept alive for the whole call, since destroying it
    // revokes the handle); X11/XWayland uses x11:<xid>. Empty = unparented (a floating window, the
    // pre-fix behaviour). Logged so a "still not modal" report can be traced to the exact path.
    WaylandForeignExport wlParent(req.parent);
    std::string parent = wlParent.handle();
    if (parent.empty())
        parent = parentWindowHandle(req.parent); // the x11:<xid> path
    plog().info("file dialog: portal {} modal=true parent='{}'", save ? "SaveFile" : "OpenFile",
                parent.empty() ? "(none)" : parent.c_str());
    sd_bus_message_append(call.msg, "ss", parent.c_str(), req.title.c_str());
    sd_bus_message_open_container(call.msg, 'a', "{sv}");
    appendStringOption(call.msg, "handle_token", token.c_str());
    if (!req.acceptLabel.empty()) {
        const std::string mnemonic = "_" + req.acceptLabel; // portal underscore-mnemonic
        appendStringOption(call.msg, "accept_label", mnemonic.c_str());
    }
    appendBoolOption(call.msg, "modal", true);
    // current_name is a BASE NAME ("shot.png"), never a path and never a URI -- the portal pastes
    // it into the dialog's name field verbatim.
    if (const std::string name = detail::baseNameOf(req.suggestedName); save && !name.empty())
        appendStringOption(call.msg, "current_name", name.c_str());
    // ... and current_folder is where that name lands. Sending it keeps all three picker paths
    // (portal, kdialog, native chooser) obeying the ONE export-path policy instead of each falling
    // back to its own remembered-or-cwd default.
    if (const std::string dir = startFolderFor(req); !dir.empty())
        appendPathOption(call.msg, "current_folder", dir);
    if (!req.filters.empty()) {
        appendFiltersOption(call.msg, req.filters);
        appendCurrentFilterOption(call.msg, req.filters.front()); // the first filter IS the default
    }
    sd_bus_message_close_container(call.msg);

    // The method returns the Request handle immediately (the dialog result arrives later via the
    // Response signal), so this synchronous call is quick; 0 = sd-bus's default method timeout,
    // which is what bounds it if the portal is installed but wedged.
    sd_bus_error err = SD_BUS_ERROR_NULL;
    MessageHandle reply;
    const int rc = sd_bus_call(bus.bus, call.msg, 0, &err, &reply.msg);
    if (rc < 0) { // portal backend missing / method failed -> fall back to the native chooser
        plog().info("file dialog: portal {} refused ({})", save ? "SaveFile" : "OpenFile",
                    err.message != nullptr ? err.message : std::strerror(-rc));
        sd_bus_error_free(&err);
        return result; // Unavailable
    }
    sd_bus_error_free(&err);

    // The path predicted above is a CONVENTION, not a guarantee. The portal spec requires the
    // caller to compare it against the handle the method actually returned and to subscribe to
    // that one when they differ -- a backend that hands back a different path would otherwise emit
    // its Response on an object nobody is listening to, and the pump below would wait for a signal
    // that is never coming (a hard freeze with the picker sitting right there on screen). Both
    // matches stay installed until the dialog is done, so there is no instant in which neither is.
    SlotHandle returnedMatch;
    const char* handle = nullptr; // points into `reply`, which outlives the wait below
    if (reply.msg != nullptr && sd_bus_message_read(reply.msg, "o", &handle) >= 0 &&
        handle != nullptr && predictedPath != handle) {
        plog().info("file dialog: portal returned request path '{}' (predicted '{}'); subscribing "
                    "to the returned one too",
                    handle, predictedPath);
        sd_bus_match_signal(bus.bus, &returnedMatch.slot, "org.freedesktop.portal.Desktop", handle,
                            "org.freedesktop.portal.Request", "Response", onResponse, &state);
    }

    // Pump the connection until the Response arrives. Drain anything already queued first, then --
    // if still pending -- drive the bus fd from a nested FLTK loop so the app keeps painting (and
    // keeps answering the window manager) while the dialog is up.
    //
    // There is deliberately NO wall-clock cap on this wait, and adding one would be a bug, not a
    // safety net: a picker legitimately stays open for as long as the user is browsing, and a
    // timeout that fires under them would cancel a dialog they are still using. What the loop is
    // bounded by instead is every way the answer can become impossible, each closed above or in
    // drainBus: the connection dying, the portal losing its bus name, the descriptor being revoked
    // out from under FLTK, and the Response being emitted on a Request path we never subscribed to.
    BusPump pump{bus.bus, &state};
    if (drainBus(bus.bus, state) && !state.done) {
        const int fd = sd_bus_get_fd(bus.bus);
        if (fd < 0) {
            state.broken = true;
        } else {
            EventSwallow swallow; // nothing dispatched in here may re-enter the app
            FdWatch watch(fd, busFdReady, &pump);
            while (!state.done) {
                pumpFltkOnce();
                // Re-drain unconditionally rather than trusting fd readability alone: sd-bus can
                // hold a fully-received message in its own buffer with nothing left to read on the
                // socket, in which case the descriptor never becomes readable again and a purely
                // fd-driven loop would sleep on an answer it already has.
                if (!drainBus(bus.bus, state))
                    break;
            }
        }
    }

    // An answer we already have beats anything that happened to the connection afterwards (the
    // portal legitimately drops off the bus right after replying), so decode first.
    if (state.response == 0 && !state.uri.empty()) {
        result.path = detail::uriToPath(state.uri);
        // An undecodable URI: treat as a cancel rather than handing back a bad path.
        result.status = result.path.empty() ? PortalStatus::Cancelled : PortalStatus::Ok;
        return result;
    }
    if (state.broken) {
        // No Request.Close is attempted here on purpose: it is a round-trip to a service we have
        // just established is gone, and D-Bus would try to auto-START it and then block us for the
        // default method timeout -- re-freezing the main loop in the middle of the recovery path.
        plog().warn("file dialog: the portal connection ended while the picker was up; treating "
                    "it as a cancel rather than waiting for a reply that cannot arrive");
    }
    result.status = PortalStatus::Cancelled;
    return result;
}

// ---- kdialog, spawned directly for an X11-modal transient (KDE) --------------------------------
//
// On X11/XWayland the XDG portal's file dialog is a Wayland window that KWin will NOT parent to our
// X11 window (it floats, non-modal -- verified: portal parent='x11:<xid>' + modal=true is ignored).
// A directly-spawned kdialog FORCED onto the xcb (X11) Qt platform, given `--attach <xid>`, IS a
// real X11-modal transient (verified via WM_TRANSIENT_FOR + _NET_WM_STATE_MODAL on the resulting
// window) -- an X11-to-X11 transient the WM honours. So on X11 + KDE we prefer it over the portal.
// (Native Wayland keeps the portal: there the wayland:<handle> parent works, see WaylandForeignExport.)

// The window's X11 id (client window), or 0 when not on the X11 backend / not shown.
unsigned long x11WindowId(Fl_Window* win) {
    if (win == nullptr || win->shown() == 0)
        return 0;
    NativeSurfaceHandle nh;
    std::string err;
    if (!nativeSurfaceHandle(win, nh, err) || nh.system != WindowSystem::X11 || nh.window == nullptr)
        return 0;
    return reinterpret_cast<std::uintptr_t>(nh.window);
}

enum class KdialogOutcome { Ok, Cancelled, Failed };

// What the nested pump accumulates from kdialog's stdout.
struct KdialogPump {
    std::string out;
    bool finished = false; // end-of-file (or an unrecoverable error) on the pipe
};

void kdialogFdReady(FL_SOCKET fd, void* data) {
    auto* p = static_cast<KdialogPump*>(data);
    char buf[512];
    for (;;) {
        const ssize_t n = ::read(static_cast<int>(fd), buf, sizeof buf);
        if (n > 0) {
            p->out.append(buf, static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0) { // EOF: every write end is closed, so kdialog is done with us
            p->finished = true;
            return;
        }
        if (errno == EINTR)
            continue; // a signal, not an answer -- the blocking version STOPPED reading here and
                      // silently returned a truncated path
        if (errno == EAGAIN) // == EWOULDBLOCK on Linux; drained for now, poll() will call us again
            return;
        p->finished = true; // a real error on the pipe: nothing more is coming
        return;
    }
}

// After EOF, how long we will keep turning the loop waiting for kdialog to actually exit before
// killing it. EOF means it closed its stdout, i.e. it is already on its way out -- but "it will be
// gone in a moment" is precisely the reasoning that produced the freeze this file is fixing, so
// the reap is bounded too.
constexpr int kKdialogReapTimeoutSec = 5;

// Spawn kdialog, optionally `--attach`ed to X11 window `xid` (0 = no attach) so the WM makes it a
// modal transient, then wait for it WITHOUT blocking the FLTK loop. Failed = kdialog missing /
// could not start -> the caller falls back to the portal.
//
// TWO FAILURE MODES ARE DESIGNED OUT HERE; both produced the reported "the export picker locks up
// and crashes, freezing the whole program". X11 + KDE was the DEFAULT desktop path when that was
// reported (Mosaic pinned FLTK to X11, so a Plasma Wayland session landed here through XWayland),
// which is why this was the branch the user actually hit. Since S59-a the default is NATIVE WAYLAND
// (preferWaylandBackendIfUnset), so a Plasma session now takes the portal path instead and this
// branch serves a pure-Xorg / FLTK_BACKEND=x11 KDE user -- see the gate in run() and
// docs/wayland.md.
//
// (1) IT MUST NOT BLOCK THE MAIN LOOP. The first version sat in a blocking read() + waitpid() for
//     the ENTIRE life of the picker -- however many minutes the user spends browsing. Mosaic
//     serviced nothing at all in that window: no expose, no resize, no frame timer (so no Vulkan
//     present), no journal tick, no job poll, with input queueing up unread in the X11 socket and
//     replayed in a burst afterwards. That is the "freezes the whole program" report, and it was
//     the default path. Meanwhile file_dialog.hpp promised the opposite ("Does not block the FLTK
//     loop") -- only the portal branch ever kept that promise. So the pipe is non-blocking, driven
//     from Fl::add_fd inside a nested Fl::wait() pump, exactly like the portal branch and like
//     FLTK's own kdialog driver.
//
// (2) IT MUST NOT fork(). The old child called setenv() between fork() and exec() to force
//     QT_QPA_PLATFORM=xcb. POSIX allows only async-signal-safe functions there when the parent is
//     multithreaded, and setenv() is not one: it takes the environment lock and mallocs. Mosaic
//     spawns from a process full of worker threads -- the Export modal's own encode job is running
//     on one while its [...] button is being pressed. A child that wedges between fork and exec
//     never execs, so its stdout is never closed, so the parent's read() waits for EOF that cannot
//     come: a permanent freeze with no timeout anywhere in the path. posix_spawnp runs NO user code
//     in the child; the amended environment is built here, in the parent, and handed over.
KdialogOutcome runKdialog(const FileDialogRequest& req, bool save, unsigned long xid,
                          std::string& outPath) {
    char attach[32];
    std::snprintf(attach, sizeof attach, "0x%lx", xid);
    const std::string filter = detail::kdialogFilter(req.filters);
    const std::string startArg = detail::kdialogStartArgument(req, save, homeDirectory());

    std::vector<std::string> args{"kdialog"};
    if (xid != 0) {
        args.push_back("--attach");
        args.push_back(attach);
    }
    if (!req.title.empty()) {
        args.push_back("--title");
        args.push_back(req.title);
    }
    args.push_back(save ? "--getsavefilename" : "--getopenfilename");
    args.push_back(startArg);
    if (!filter.empty())
        args.push_back(filter);
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (std::string& s : args)
        argv.push_back(s.data());
    argv.push_back(nullptr);

    // The child's environment, amended in the PARENT (see note (2) above).
    std::vector<std::string> envStrings;
    for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
        if (xid != 0 && std::strncmp(*e, "QT_QPA_PLATFORM=", 16) == 0)
            continue; // replaced just below
        envStrings.emplace_back(*e);
    }
    if (xid != 0)
        envStrings.emplace_back("QT_QPA_PLATFORM=xcb"); // force X11 so --attach works
    std::vector<char*> envp;
    envp.reserve(envStrings.size() + 1);
    for (std::string& s : envStrings)
        envp.push_back(s.data());
    envp.push_back(nullptr);

    int fds[2];
    if (pipe(fds) != 0)
        return KdialogOutcome::Failed;
    // Both ends close-on-exec: the child needs only the dup2'd copy on stdout (dup2 clears
    // FD_CLOEXEC on the new descriptor). A write end leaked into any other child would hold the
    // pipe open past kdialog's exit and the read loop would never see EOF -- the same wedge as (2).
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    // ... and the read end is non-blocking, because the fd handler drains it from inside the FLTK
    // loop and must never be the thing that stops the loop turning.
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);

    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        close(fds[0]);
        close(fds[1]);
        return KdialogOutcome::Failed;
    }
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    pid_t pid = 0;
    const int spawnRc = posix_spawnp(&pid, "kdialog", &actions, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]); // the parent must not keep a write end open, or EOF never arrives
    if (spawnRc != 0) {
        close(fds[0]);
        plog().warn("kdialog could not be started ({}); portal fallback", std::strerror(spawnRc));
        return KdialogOutcome::Failed;
    }

    KdialogPump pump;
    int status = 0;
    bool reaped = false;
    {
        EventSwallow swallow; // modal: the app keeps painting, but nothing can be driven behind it
        {
            FdWatch watch(fds[0], kdialogFdReady, &pump);
            while (!pump.finished)
                pumpFltkOnce();
        } // remove_fd BEFORE the close below -- FLTK must never poll a descriptor we have released
        close(fds[0]);

        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(kKdialogReapTimeoutSec);
        for (;;) {
            const pid_t r = waitpid(pid, &status, WNOHANG);
            if (r == pid) {
                reaped = true;
                break;
            }
            if (r < 0) {
                if (errno == EINTR)
                    continue;
                break; // ECHILD: something already reaped it -- judge by the output instead
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                plog().warn("kdialog closed its output but did not exit; killing it");
                kill(pid, SIGKILL);
                while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
                }
                break; // deliberately NOT `reaped`: the status is the kill, not kdialog's verdict
            }
            pumpFltkOnce();
        }
    }

    std::string out = pump.out;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();

    if (!reaped || !WIFEXITED(status)) {
        // No usable exit code (auto-reaped, or we had to kill it). Judge by what it printed:
        // kdialog writes the chosen path on success and nothing at all on cancel.
        if (!out.empty()) {
            outPath = out;
            return KdialogOutcome::Ok;
        }
        return KdialogOutcome::Cancelled;
    }
    const int code = WEXITSTATUS(status);
    if (code == 0) {
        if (out.empty())
            return KdialogOutcome::Cancelled;
        outPath = out;
        return KdialogOutcome::Ok;
    }
    if (code == 1)
        return KdialogOutcome::Cancelled; // the user pressed Cancel
    // 127 = exec failed; anything else = kdialog started but errored (e.g. no xcb Qt plugin).
    // Either way, log the code and let the caller try the portal.
    plog().warn("kdialog exited with code {} ({}); portal fallback", code,
                code == 127 ? "not found" : "startup/other error");
    return KdialogOutcome::Failed;
}

// >0 while run() (below) is on the stack. A counter, not a bool, so the balance survives even if
// the re-entrancy refusal in run() is ever removed. Read via fileDialogInFlight().
int g_dialogDepth = 0;

// RAII: makes the picker MODAL for the duration of run(). Both wait paths pump a nested FLTK loop
// so the canvas keeps painting -- but that also keeps the parent window live enough to reach the
// quit path, and quitting hides the parent out from under the nested loop, which then spins forever
// on a reply that never arrives (the reported freeze). Deactivating the parent blocks every
// in-widget interaction (menus, tools, a second Open) regardless of how well the compositor honours
// the portal's own modal-parent hint; the quit guard in app_window backstops the one path that
// bypasses widget-active state (the window-manager close button -> FL_CLOSE).
//
// The parent is held through an Fl_Widget_Tracker, not a bare pointer: this object outlives a
// nested event loop, and FLTK processes its Fl::delete_widget() queue inside Fl::flush(). A parent
// deleted in there would leave the destructor below calling shown() on freed memory.
struct DialogGuard {
    Fl_Window* parent;
    Fl_Widget_Tracker tracker;
    explicit DialogGuard(Fl_Window* p) : parent(p), tracker(p) {
        ++g_dialogDepth;
        if (parent != nullptr && parent->shown() != 0)
            parent->deactivate();
    }
    ~DialogGuard() {
        --g_dialogDepth;
        if (parent != nullptr && tracker.exists() != 0 && parent->shown() != 0)
            parent->activate();
    }
    DialogGuard(const DialogGuard&) = delete;
    DialogGuard& operator=(const DialogGuard&) = delete;
};

std::optional<std::string> run(const FileDialogRequest& req, bool save) {
    // ONE picker at a time, enforced rather than assumed. Every wait below runs a nested Fl::wait()
    // loop; a second dialog opened from inside one would stack a second nested loop and a second
    // DialogGuard whose destructor re-ACTIVATES the parent while the first picker is still on
    // screen. EventSwallow already stops FLTK delivering the events that could do that, so this
    // only catches a caller reaching us some other way (an fd handler, a timeout, a worker's
    // completion callback) -- but it is the invariant the rest of this file is written against.
    if (g_dialogDepth > 0) {
        plog().warn("file dialog: refusing a second picker while one is already up");
        return std::nullopt;
    }
    DialogGuard guard(req.parent);
    // X11/XWayland + KDE: the portal picker cannot be made modal to our X11 window, but a directly
    // spawned, X11-forced kdialog CAN (see runKdialog). Prefer it; fall through to the
    // portal only if kdialog is missing / cannot start.
    //
    // ⚠ SINCE S59-a THIS GATE IS FALSE FOR A DEFAULT PLASMA SESSION. The backend pin now chooses
    // native Wayland (docs/wayland.md), so a KDE user reaches the portal branch below with a real
    // zxdg_exporter_v2 "wayland:<handle>" parent -- which is exactly the condition
    // docs/export-system-plan.md §8 named for RETIRING this fork ("it can be dropped once Mosaic
    // gains a native-Wayland canvas: then the wayland:<handle> path is the norm on Plasma and the
    // XWayland mismatch disappears"). It is kept until that has been verified on a real session,
    // because the check is interactive by nature and because this is still the only modal picker a
    // pure-Xorg / FLTK_BACKEND=x11 KDE user gets. Removing it is a live decision, not a leftover.
    if (activeBackend() == WindowSystem::X11 && runningKde()) {
        if (const unsigned long xid = x11WindowId(req.parent)) {
            std::string path;
            const KdialogOutcome oc = runKdialog(req, save, xid, path);
            plog().info("file dialog: kdialog --attach 0x{:x} modal transient -> {}", xid,
                        oc == KdialogOutcome::Ok          ? "ok"
                        : oc == KdialogOutcome::Cancelled ? "cancelled"
                                                          : "failed (falling back to portal)");
            if (oc == KdialogOutcome::Ok)
                return path;
            if (oc == KdialogOutcome::Cancelled)
                return std::nullopt;
            // Failed -> fall through to the portal below.
        }
    }
    const PortalResult portal = runPortal(req, save);
    switch (portal.status) {
    case PortalStatus::Ok:
        return portal.path;
    case PortalStatus::Cancelled:
        return std::nullopt;
    case PortalStatus::Unavailable:
        break;
    }
    // Portal unreachable. On KDE, drive kdialog OURSELVES (unattached: we are here because the
    // X11+KDE attach path above did not apply) rather than through Fl_Native_File_Chooser's
    // kdialog driver, whose command builder seeds the start location from getcwd() when no
    // directory is set, drops it entirely for an Open, and passes it to the shell unquoted.
    // Ours is always one absolute path. Off KDE -- and when kdialog is simply missing -- the
    // native chooser (GTK) takes over, and it honours directory()/preset_file() properly.
    if (runningKde()) {
        std::string path;
        const KdialogOutcome oc = runKdialog(req, save, /*xid=*/0, path);
        plog().info("file dialog: portal unavailable -> kdialog (unattached) -> {}",
                    oc == KdialogOutcome::Ok          ? "ok"
                    : oc == KdialogOutcome::Cancelled ? "cancelled"
                                                      : "failed (native chooser next)");
        if (oc == KdialogOutcome::Ok)
            return path;
        if (oc == KdialogOutcome::Cancelled)
            return std::nullopt;
    }
    plog().info("file dialog: XDG portal unavailable -> native fallback (Fl_Native_File_Chooser)");
    return nativeFallback(req, save); // portal unreachable: the Fl_Native_File_Chooser path
}

} // namespace

namespace detail {

InFlightScope::InFlightScope() noexcept {
    ++g_dialogDepth;
}

InFlightScope::~InFlightScope() noexcept {
    --g_dialogDepth;
}

} // namespace detail

void initNativeFileDialog() {
    if (runningKde())
        Fl::option(Fl::OPTION_FNFC_USES_KDIALOG, true);
}

bool fileDialogInFlight() noexcept {
    return g_dialogDepth > 0;
}

bool fileDialogGrabsInput() noexcept {
    return g_dialogDepth > 0 && g_inProcessChooserDepth == 0;
}

std::optional<std::string> showSaveDialog(const FileDialogRequest& req) {
    return run(req, /*save=*/true);
}

std::optional<std::string> showOpenDialog(const FileDialogRequest& req) {
    return run(req, /*save=*/false);
}

} // namespace mosaic::platform
