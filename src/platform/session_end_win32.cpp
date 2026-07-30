#include "platform/session_end.hpp"

// Windows implementation of platform/session_end.hpp: answer the OS's shutdown/log-off query
// instead of letting DefWindowProc grant permission behind the user's back. The WHY -- and why this
// is pointedly not a dialog -- is in the header; this file is the mechanism.
//
// ⚠⚠ THE MECHANISM PROBLEM, and the reason this TU exists at all. **WM_QUERYENDSESSION is SENT, not
// posted.** It is delivered straight into the window procedure from inside SendMessage; it never
// enters a thread message queue, so it never appears in PeekMessage -- and Fl::add_system_handler,
// which is a hook on FLTK's PeekMessage-based pump, cannot see it. (This is the identical trap
// tablet_win32.cpp records for WM_TABLET_QUERYSYSTEMGESTURESTATUS, where it is the reason
// press-and-hold is left enabled.) A system handler also could not do the job even if it saw the
// message: it can only swallow one, and what is needed here is to RETURN A VALUE for it.
//
// So the only way in is to SUBCLASS the top-level HWND -- SetWindowLongPtrW(GWLP_WNDPROC) with the
// previous procedure kept and CallWindowProcW'd for everything we do not handle. Three properties
// that discipline demands, all of them load-bearing rather than defensive:
//
//   * the chain must stay intact. We keep FLTK's procedure and forward every message, including the
//     two we act on where forwarding is safe, and we restore it on WM_NCDESTROY -- with the one
//     exception of a LATER subclass sitting on top of ours (see detach()).
//   * installation must be IDEMPOTENT. Subclassing twice would make the second hook's `prev` the
//     first hook, and then unhooking in any order but the exact reverse leaves a dangling link.
//     A second install on the same window re-points the existing hook instead.
//   * the procedure must survive RE-ENTRANCY. The predicate below is application code running
//     inside an OS send; anything it does can bring us back in here, up to and including the
//     window's own destruction. Every callback is therefore made with the guard held by shared_ptr
//     and its state re-checked afterwards.

// spdlog FIRST, before anything drags in <windows.h> (FL/platform.H does): windows.h defines a pile
// of bare macros and fmt's headers are the usual casualty. The same ordering rule as
// file_dialog_win32.cpp, for the same reason.
#include "common/log.hpp"

#include <FL/Fl_Window.H>
#include <FL/platform.H> // fl_win32_xid() -> the window's HWND (and the <windows.h> this TU needs)

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Named even though FL/platform.H has already dragged it in: every Win32 call below is OURS, not
// FLTK's, and an FLTK that stopped leaking the system header would otherwise break this file at a
// distance (the same note native_window_win32.cpp carries).
#include <windows.h>

namespace mosaic::platform {
namespace {

spdlog::logger& plog() {
    static const auto logger = common::log::category("platform");
    return *logger;
}

// ---- user32: the shutdown-block reason API, resolved at RUNTIME ---------------------------------
//
// ShutdownBlockReason{Create,Destroy} have been in user32 since Vista -- far below Mosaic's Windows
// 10 1809 floor -- and the mingw-w64 import library exports both, so linking them statically would
// be correct on every Windows this build targets. They are resolved anyway, for the reason
// tablet_win32.cpp paid to learn: **a missing import is not an error the loader reports, it is a
// process that dies before main().** This session already shipped exactly that crash
// (USER32.dll.SetWindowFeedbackSetting, under Wine) over a function that only suppressed some pen
// animations. The Wine in this tree does export both of these -- as soft stubs that FIXME once, set
// ERROR_CALL_NOT_IMPLEMENTED and return FALSE -- so today's static import would survive; a Wine
// that had them as hard `@ stub` entries instead would not, and a courtesy feature must never be
// able to stop Mosaic from starting. Resolving costs eight lines and removes the question rather
// than answering it, which is the standing policy for this whole directory.
//
// Degrading is graceful in the one direction that matters: with no reason API we still return FALSE
// and still block, and Windows names the application on its blocking screen instead of our
// sentence. Losing the sentence is a worse explanation; losing the block would be lost work.
//
// user32.dll is unconditionally already mapped into any GUI process, so GetModuleHandleW is right
// here rather than LoadLibraryW: no reference to own, nothing to free, and no chance of picking up
// a second copy from a search path.

// The GetProcAddress -> generic-function-pointer hop, because FARPROC cast straight to a real
// signature trips -Wcast-function-type and routing it through void* trips -Wpedantic. The full
// reasoning is written out once, on wintab::resolve in tablet_win32.cpp; this is a two-line copy
// rather than a shared header the whole directory would have to include.
template <class Fn>
void resolveProc(HMODULE dll, const char* symbol, Fn& out) noexcept {
    using Generic = void(WINAPI*)();
    const Generic generic = reinterpret_cast<Generic>(GetProcAddress(dll, symbol));
    out = reinterpret_cast<Fn>(generic);
}

struct BlockReasonApi {
    bool tried = false;
    BOOL(WINAPI* create)(HWND, LPCWSTR) = nullptr;
    BOOL(WINAPI* destroy)(HWND) = nullptr;
};

// Not thread-safe by construction and it does not need to be: every caller runs on the UI thread,
// inside the window procedure (the same single-threaded contract the rest of this directory has).
const BlockReasonApi& blockReasonApi() {
    static BlockReasonApi a;
    if (!a.tried) {
        a.tried = true;
        if (HMODULE u32 = GetModuleHandleW(L"user32.dll"); u32 != nullptr) {
            resolveProc(u32, "ShutdownBlockReasonCreate", a.create);
            resolveProc(u32, "ShutdownBlockReasonDestroy", a.destroy);
        }
    }
    return a;
}

// ---- the reason string --------------------------------------------------------------------------

std::wstring toWide(const std::string& utf8) {
    if (utf8.empty())
        return {};
    // Measured with an EXPLICIT byte count rather than -1, so the NUL stays out of the result and
    // the wstring's size() is its length (the same boundary file_dialog_win32.cpp documents).
    const int chars =
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    if (chars <= 0)
        return {};
    std::wstring out(static_cast<std::size_t>(chars), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), out.data(), chars);
    return out;
}

// The already-translated sentence in the shape Windows shows it in: UTF-16, ONE line, bounded.
std::wstring blockReasonText(const std::string& utf8) {
    std::wstring wide = toWide(utf8);
    // The blocking screen gives each application a single row. A newline or tab that survived
    // translation would be drawn as a stray box there rather than wrapping, so fold the lot to
    // spaces -- cheaper than trusting 74 catalogs to have kept the sentence on one line.
    for (wchar_t& c : wide) {
        if (c == L'\r' || c == L'\n' || c == L'\t')
            c = L' ';
    }
    // Windows truncates past MAX_STR_BLOCKREASON itself, so this is not about avoiding a failure --
    // it is about WHERE the cut lands. Doing it here keeps it on a character boundary: a cut that
    // fell between the two halves of a surrogate pair would leave a lone unit at the end of the
    // sentence, which is a replacement glyph in every language that needs a pair (CJK extensions,
    // and emoji if a translator ever reaches for one).
    if (wide.size() >= static_cast<std::size_t>(MAX_STR_BLOCKREASON)) {
        wide.resize(static_cast<std::size_t>(MAX_STR_BLOCKREASON) - 1);
        if (!wide.empty() && wide.back() >= 0xD800 && wide.back() <= 0xDBFF)
            wide.pop_back(); // a high surrogate whose partner did not fit
    }
    return wide;
}

// ---- the guard ---------------------------------------------------------------------------------

struct Guard {
    HWND hwnd = nullptr;
    WNDPROC prev = nullptr; // FLTK's procedure -- the rest of the chain
    SessionEndQuery hasUnsavedWork;
    SessionEndNotify onSessionEnd;
    std::wstring reason;
    bool blocking = false;      // a block reason is registered on `hwnd` right now
    bool inQuery = false;       // re-entrancy latch for WM_QUERYENDSESSION
    LRESULT queryAnswer = TRUE; // what the outermost query answered (a nested one repeats it)
    bool notified = false;      // onSessionEnd has already run for this session-end attempt
    bool detached = false;      // stopped answering: nothing here may act on `hwnd` again
};

// Function-local static so there is no construction-order question, and a plain vector because the
// count is the number of top-level Mosaic windows -- one, today. Held by shared_ptr so a guard can
// be dropped from the table while a stack frame is still standing on it (see guardProc).
std::vector<std::shared_ptr<Guard>>& guards() {
    static std::vector<std::shared_ptr<Guard>> all;
    return all;
}

std::shared_ptr<Guard> findGuard(HWND hwnd) {
    for (const std::shared_ptr<Guard>& g : guards()) {
        if (g->hwnd == hwnd)
            return g;
    }
    return nullptr;
}

// Deliberately NOT GWLP_USERDATA, which would be the obvious way to reach the guard from the
// procedure: that slot belongs to whoever created the window, and FLTK's Windows driver is free to
// start using it in any release. A table lookup over a one-element vector costs a pointer compare.
LRESULT CALLBACK guardProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Register the reason with the shell, so the blocking screen can name it. Called only immediately
// before answering FALSE: Windows reads the reason (ShutdownBlockReasonQuery) after the query
// returns, and there is no second chance to supply one.
void setBlockReason(Guard& g) {
    if (g.blocking || g.reason.empty())
        return; // Create fails on a window that already has a reason -- do not spend the call
    const auto create = blockReasonApi().create;
    if (create == nullptr)
        return; // no reason API: block anyway, unnamed (see the user32 note above)
    g.blocking = create(g.hwnd, g.reason.c_str()) != FALSE;
}

// Stop naming a reason. Windows keeps a registered reason until it is destroyed, so this has to run
// on every path back out of blocking -- a cancelled shutdown, a document that got saved before the
// next query, and teardown -- or a later shutdown with nothing unsaved would still list Mosaic.
void clearBlockReason(Guard& g) {
    if (!g.blocking)
        return;
    g.blocking = false;
    if (const auto destroy = blockReasonApi().destroy; destroy != nullptr)
        destroy(g.hwnd);
}

// Stop answering for this window, and put FLTK's procedure back if ours is still the installed one.
void detach(const std::shared_ptr<Guard>& g) {
    if (g->detached)
        return;
    g->detached = true;
    clearBlockReason(*g);

    // ⚠ Only unhook while OURS is on top. If something subclassed the window AFTER us, that hook
    // holds our procedure as its `prev` and will keep calling it; ripping ours out of the window
    // would not stop those calls, it would only orphan the link. Leaving it installed is the lesser
    // evil -- with `detached` set our procedure is a pure pass-through -- and the table entry has
    // to stay with it, because it is what still owns `prev`.
    const auto installed = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(g->hwnd, GWLP_WNDPROC));
    if (installed != &guardProc)
        return;
    SetWindowLongPtrW(g->hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g->prev));

    std::vector<std::shared_ptr<Guard>>& all = guards();
    all.erase(std::remove(all.begin(), all.end(), g), all.end());
}

// ---- WM_QUERYENDSESSION ------------------------------------------------------------------------

LRESULT onQueryEndSession(const std::shared_ptr<Guard>& g, HWND hwnd, UINT msg, WPARAM wParam,
                          LPARAM lParam) {
    // Re-entrancy. The predicate is application code running inside an OS send; if anything under
    // it pumps and the message arrives again, asking twice could answer twice with two different
    // answers -- and Windows takes the FIRST return as the application's word. Repeat it instead.
    if (g->inQuery)
        return g->queryAnswer;

    const ULONG_PTR flags = static_cast<ULONG_PTR>(lParam);

    // ENDSESSION_CRITICAL: the machine is going down whatever anybody returns here -- Windows
    // documents a critical shutdown as unblockable, and a FALSE is simply ignored. Putting a
    // "Mosaic is preventing shutdown" screen in front of a user with no way to act on it is worse
    // than not asking, so drop any standing reason and get out of the way. WM_ENDSESSION still
    // follows, and that is where the work is made recoverable.
    if ((flags & ENDSESSION_CRITICAL) != 0) {
        clearBlockReason(*g);
        g->queryAnswer = CallWindowProcW(g->prev, hwnd, msg, wParam, lParam);
        return g->queryAnswer;
    }

    bool unsaved = false;
    g->inQuery = true;
    try {
        if (g->hasUnsavedWork)
            unsaved = g->hasUnsavedWork();
    } catch (...) {
        // An exception must not cross a window-procedure frame -- USER32's is not unwindable -- so
        // it is caught here whatever it was. This is also the one place where guessing has a safe
        // direction: assume there IS work. A needless blocking screen costs the user one click; the
        // other way round costs them their edits.
        unsaved = true;
        plog().error("session-end guard: the unsaved-work predicate threw; assuming unsaved work");
    }
    g->inQuery = false;

    if (g->detached) {
        // The predicate tore the window down under us. There is nothing left to protect, and
        // nothing left to hang a reason on.
        g->queryAnswer = TRUE;
        return g->queryAnswer;
    }

    if (!unsaved) {
        clearBlockReason(*g);
        // Forwarded rather than answered TRUE outright: this handler owns only Mosaic's half of the
        // answer, and swallowing the message would silence anything else in the chain. FLTK 1.4.5
        // does not handle it (verified), so this reaches DefWindowProc -- whose answer is TRUE.
        g->queryAnswer = CallWindowProcW(g->prev, hwnd, msg, wParam, lParam);
        return g->queryAnswer;
    }

    setBlockReason(*g);
    plog().info("session end refused: unsaved work{}",
                g->blocking ? "" : " (no reason registered -- Windows will name the app instead)");
    // FALSE is the whole point of the file. Windows now shows its own blocking screen, names our
    // reason on it, and leaves the decision with the user.
    g->queryAnswer = FALSE;
    return g->queryAnswer;
}

// ---- WM_ENDSESSION -----------------------------------------------------------------------------

void onEndSession(const std::shared_ptr<Guard>& g, WPARAM wParam) {
    if (wParam == FALSE) {
        // Not ending after all: somebody refused (possibly us) and the user backed out. Stop
        // blocking and re-arm, because they may edit on and try again later in the same run.
        clearBlockReason(*g);
        g->notified = false;
        return;
    }
    if (g->notified)
        return; // at most once per session end
    g->notified = true;
    clearBlockReason(*g); // the session is ending; there is nothing left to block

    // ⚠ THE LAST CALL. The process is terminated from here without unwinding -- no destructor of
    // ours runs, so this is the only chance to make the work recoverable. And it must be FAST: the
    // same few-second budget applies, after which Windows kills us mid-write.
    //
    // What it must NOT do is written on SessionEndNotify in the header and is worth repeating at
    // the call site: it must not write the user's document (a session end is not a Save), and it
    // must not DISCARD the recovery journal. An OS-terminated process is an unclean exit by
    // construction, and the journal surviving is what lets the next launch offer the restore.
    const ULONGLONG start = GetTickCount64();
    try {
        if (g->onSessionEnd)
            g->onSessionEnd();
    } catch (...) {
        plog().error("session-end guard: the session-end callback threw");
    }
    // Logged unconditionally, and with the elapsed time, because this is the one code path that
    // cannot be reproduced without really shutting Windows down: this line is the only evidence
    // that it ran at all and that it fitted in the budget. "returned after", not "finished in" --
    // the catch above means it may have given up part way.
    plog().info("session ending: recovery sync returned after {} ms", GetTickCount64() - start);
}

// ---- the subclass procedure --------------------------------------------------------------------

LRESULT CALLBACK guardProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Held by shared_ptr for the whole call: the callbacks below are application code, and one of
    // them reaching back in (the window's own destruction, removeSessionEndGuard) can drop this
    // entry from the table while we are still standing on it.
    const std::shared_ptr<Guard> g = findGuard(hwnd);
    if (!g) {
        // Unreachable by construction -- an entry is only ever removed together with the subclass
        // that would have brought us here -- but a window procedure has no way to fail, and
        // DefWindowProc is the only answer available without a `prev` to forward to.
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    const WNDPROC prev = g->prev; // captured up front: detach() below clears our claim, not this

    if (!g->detached) {
        switch (msg) {
        case WM_QUERYENDSESSION:
            return onQueryEndSession(g, hwnd, msg, wParam, lParam);
        case WM_ENDSESSION:
            // Acted on, then forwarded: WM_ENDSESSION is a notification with no answer to give, so
            // there is nothing here to swallow. DefWindowProc does nothing with it and FLTK does
            // not handle it, but the chain stays honest for whoever comes next.
            onEndSession(g, wParam);
            break;
        case WM_NCDESTROY:
            // The last message a window ever receives, and so the one place a manual subclass can
            // unhook itself: the HWND is still addressable here, and after this there will be no
            // further call to notice. Unhook BEFORE forwarding, so FLTK's own WM_NCDESTROY teardown
            // is the last thing to touch the window.
            detach(g);
            return CallWindowProcW(prev, hwnd, msg, wParam, lParam);
        default:
            break;
        }
    }
    return CallWindowProcW(prev, hwnd, msg, wParam, lParam);
}

// The top-level HWND behind an FLTK window, or null when there is none yet.
//
// ⚠ Only a TOP-LEVEL window is sent WM_QUERYENDSESSION. Mosaic's canvas, menu bar, menu popups and
// tool popovers are all FLTK SUB-windows, which are real child HWNDs on this backend -- a guard
// installed on one of those would compile, install cleanly, and never once be asked. GA_ROOT is the
// top-level ancestor, and it is the window itself for the main window the call site passes.
HWND topLevelHandle(Fl_Window* win) {
    if (win == nullptr)
        return nullptr;
    // FLTK creates the HWND inside Fl_Window::show(), so this is null exactly when the window has
    // not been shown yet.
    HWND hwnd = fl_win32_xid(win);
    if (hwnd == nullptr)
        return nullptr;
    if (HWND root = GetAncestor(hwnd, GA_ROOT); root != nullptr)
        hwnd = root;
    return hwnd;
}

} // namespace

void installSessionEndGuard(Fl_Window* win, SessionEndQuery hasUnsavedWork,
                            SessionEndNotify onSessionEnd, const std::string& reason) {
    HWND hwnd = topLevelHandle(win);
    if (hwnd == nullptr) {
        // Nothing retries. The call site is the line after show(), and a window that is not shown
        // there is a wiring bug rather than a transient state -- so say so and carry on, because a
        // missing shutdown prompt must not be a failure to start.
        plog().warn("session-end guard: no native window yet; a shutdown will not be questioned");
        return;
    }

    // A hide()/show() cycle DESTROYS the HWND and creates a new one (FLTK's Windows driver calls
    // DestroyWindow from Fl_Window::hide()), which can leave an entry pointing at a handle that no
    // longer exists -- our procedure died with its window, so there is nothing to restore and
    // nothing to keep. Swept here rather than on a timer: install is the only moment that cares.
    std::vector<std::shared_ptr<Guard>>& all = guards();
    all.erase(std::remove_if(all.begin(), all.end(),
                             [](const std::shared_ptr<Guard>& g) {
                                 return IsWindow(g->hwnd) == FALSE;
                             }),
              all.end());

    std::wstring wide = blockReasonText(reason);

    // IDEMPOTENT (see the header of this file for what a double subclass costs). Re-pointing also
    // covers a re-arm: a guard that was detached but is still in the table is one whose procedure
    // is STILL installed -- detach() only erases the entry when it managed to restore -- so
    // clearing the flag puts it straight back into service without touching the window at all.
    if (const std::shared_ptr<Guard> existing = findGuard(hwnd)) {
        existing->hasUnsavedWork = std::move(hasUnsavedWork);
        existing->onSessionEnd = std::move(onSessionEnd);
        existing->reason = std::move(wide);
        existing->detached = false;
        return;
    }

    auto g = std::make_shared<Guard>();
    g->hwnd = hwnd;
    g->hasUnsavedWork = std::move(hasUnsavedWork);
    g->onSessionEnd = std::move(onSessionEnd);
    g->reason = std::move(wide);
    // In the table BEFORE the hook goes in. Our procedure finds its guard by HWND, so a message
    // dispatched between the two lines would find none and fall through to DefWindowProc -- losing
    // FLTK's own handling of it. Removed again below if the hook fails.
    all.push_back(g);

    SetLastError(0);
    const LONG_PTR prev =
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&guardProc));
    if (prev == 0) {
        // A real window always had a procedure, so zero is a failure rather than "there was none".
        plog().warn("session-end guard: could not subclass the window (error {}); a shutdown will "
                    "not be questioned",
                    GetLastError());
        all.pop_back();
        return;
    }
    g->prev = reinterpret_cast<WNDPROC>(prev);
    plog().info("session-end guard installed on the main window");
}

void removeSessionEndGuard(Fl_Window* win) {
    HWND hwnd = topLevelHandle(win);
    if (hwnd == nullptr)
        return;
    // The shared_ptr is what makes this safe: detach() drops the entry from the table, and the
    // local copy is what keeps the Guard alive until this returns.
    if (const std::shared_ptr<Guard> g = findGuard(hwnd))
        detach(g);
}

} // namespace mosaic::platform
