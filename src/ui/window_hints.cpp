#include "ui/window_hints.hpp"

#include "common/image.hpp"
#include "common/image_svg.hpp"
#include "common/log.hpp"
#include "platform/wayland_dialog.hpp"
#include "platform/wayland_toplevel_icon.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <assets/app_icon_svg.hpp> // generated: mosaic::assets::app_icon_svg[]
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mosaic::ui {
namespace {

// The dialog hints have to OUTLIVE the call that creates them -- destroying an xdg_dialog_v1 is how
// the hint is withdrawn -- so they are parked here, one per window.
//
// Nothing needs to erase an entry promptly: the protocol says an xdg_dialog_v1 whose xdg_toplevel
// has been destroyed simply "becomes inert", so a stale one is harmless rather than a dangling
// reference. The sweep below keeps the map bounded anyway, which matters because a dialog that is
// opened and closed repeatedly (the confirm-on-quit, say) would otherwise accumulate one proxy per
// open for the life of the process.
std::unordered_map<Fl_Window*, std::unique_ptr<platform::WaylandDialogHint>>& dialogHints() {
    static std::unordered_map<Fl_Window*, std::unique_ptr<platform::WaylandDialogHint>> map;
    return map;
}

// ⚠ Deliberately does NOT dereference a key. Several of these dialogs are rebuilt per open
// (`m_fillDialog`, `m_layerEffectsDialog`, `m_textureGenDialog`, ... are reset on their
// unique_ptr), so a key can be a DANGLING Fl_Window* by the time the sweep runs -- asking it
// `shown()` would be a use-after-free. FLTK's own shown-window list is the safe oracle: walk it,
// compare ADDRESSES only, and drop every entry that is not on it. A stale pointer that happens to
// match a live window's address is harmless -- that entry is replaced on that window's next show()
// anyway.
void sweepClosedDialogs() {
    std::unordered_set<const Fl_Window*> live;
    for (Fl_Window* w = Fl::first_window(); w != nullptr; w = Fl::next_window(w))
        live.insert(w);
    auto& map = dialogHints();
    for (auto it = map.begin(); it != map.end();) {
        it = live.contains(it->first) ? std::next(it) : map.erase(it);
    }
}

// Every toplevel the watcher has already handled. Same pointer-only discipline as the sweep above:
// never dereferenced, only compared, because a dialog rebuilt per open leaves a dangling key.
std::unordered_set<Fl_Window*>& handledToplevels() {
    static std::unordered_set<Fl_Window*> set;
    return set;
}

// Fl::first_window()/next_window() enumerate exactly the SHOWN windows, which is what makes this
// work: a window that closes drops off the list and is forgotten, so if it is shown again it counts
// as new -- and it genuinely is, since FLTK builds it a fresh xdg_toplevel.
void hintWatcher(void* /*unused*/) {
    std::unordered_set<Fl_Window*> live;
    for (Fl_Window* w = Fl::first_window(); w != nullptr; w = Fl::next_window(w))
        live.insert(w);
    std::erase_if(handledToplevels(), [&live](Fl_Window* w) { return !live.contains(w); });

    for (Fl_Window* w : live) {
        // Sub-windows (the Vulkan canvas, the rulers, popovers) are subsurfaces with no
        // xdg_toplevel of their own. applyToplevelHints would no-op on them anyway; skipping is
        // just cheaper on a callback that runs every pass of the event loop.
        if (w->parent() != nullptr)
            continue;
        if (!handledToplevels().insert(w).second)
            continue; // already done
        applyToplevelHints(w);
    }
}

} // namespace

void installToplevelHintWatcher() {
    static bool installed = false;
    if (installed)
        return;
    installed = true;
    Fl::add_check(hintWatcher);
}

void applyToplevelHints(Fl_Window* win) {
    if (win == nullptr || win->shown() == 0)
        return;

    // One spec, built once: `name` is the .desktop's Icon= (the path a compositor prefers when the
    // app IS installed and the theme can resolve it), `raster` is the fallback that carries actual
    // pixels -- the only thing that works for an uninstalled binary such as an AppImage.
    static const platform::WaylandIconSpec spec{
        .name = "mosaic", .raster = [](int edge) {
            std::string err;
            common::Image img = common::rasterizeSvg(
                mosaic::assets::app_icon_svg, mosaic::assets::app_icon_svg_size, edge, edge, &err);
            if (img.empty())
                common::log::category("ui")->warn("app icon at {}px: {}", edge, err);
            return img;
        }};
    platform::applyWaylandToplevelIcon(win, spec);

    // modal()/non_modal() is FLTK's own test for "give this toplevel a parent", and xdg-dialog-v1
    // does nothing without one -- so this condition deliberately mirrors it rather than guessing.
    if (win->modal() == 0 && win->non_modal() == 0)
        return;
    sweepClosedDialogs();
    // Replaced unconditionally, not created-if-absent: every call follows a fresh show(), and a
    // re-shown FLTK window is a NEW xdg_toplevel -- a hint built against the previous one has gone
    // inert and would leave the reopened dialog un-hinted. Assigning destroys the old hint.
    dialogHints()[win] = std::make_unique<platform::WaylandDialogHint>(win, win->modal() != 0);
}

} // namespace mosaic::ui
