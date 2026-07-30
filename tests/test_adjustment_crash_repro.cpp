// Repro harness for the intermittent adjustment-layer crash (user: "sometimes while adding two
// adjustment layers/filters the program crashes; sometimes while an adjustment layer is present").
// Drives the REAL classes the host drives -- the compositor scope walk, the dock's scope-preview
// thumbnails, and the pinned AdjustmentPanel (faded and rebuilt/retargeted between two adjustment
// layers, driven headlessly -- see the case below) -- exactly as the app_window frame loop does, so
// ASan catches any UAF / dangling-context along those paths deterministically.
#include "ui/adjustment_panel.hpp"
#include "ui/layer_panel.hpp"

#include "core/adjustments.hpp"
#include "core/commands.hpp"
#include "core/document.hpp"
#include "render/compositor.hpp"

#include <doctest/doctest.h>

#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Image_Surface.H>

#include <algorithm>
#include <cstdlib>
#include <map>
#include <string>

using namespace mosaic;

namespace {

// A raster layer with a real gradient so the compositor's scope walk does honest work.
std::unique_ptr<core::RasterLayer> paintedRaster(core::Document& doc) {
    auto r = doc.makeRaster("bg", doc.width(), doc.height());
    common::Image& img = r->image();
    for (std::uint32_t y = 0; y < img.height; ++y)
        for (std::uint32_t x = 0; x < img.width; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
            img.rgba[p + 0] = static_cast<std::uint8_t>(x * 255 / std::max<std::uint32_t>(1, img.width));
            img.rgba[p + 1] = static_cast<std::uint8_t>(y * 255 / std::max<std::uint32_t>(1, img.height));
            img.rgba[p + 2] = 128;
            img.rgba[p + 3] = 255;
        }
    return r;
}

core::LayerId addAdjustment(core::Document& doc, core::AdjustmentKind kind) {
    auto layer = doc.makeAdjustment(std::string(core::adjustmentKindName(kind)), kind);
    core::seedAdjustmentDefaults(*layer);
    if (layer->params().contains("center_x")) {
        layer->params()["center_x"] = doc.width() * 0.5;
        layer->params()["center_y"] = doc.height() * 0.5;
    }
    const core::LayerId id = layer->id();
    doc.commands().push(std::make_unique<core::AddLayerCommand>(doc.root().id(),
                                                               doc.root().childCount(),
                                                               std::move(layer)));
    return id;
}

} // namespace

// The compositor's scope walk with TWO stacked adjustments (one spatial), through preview + backdrop
// + a full document composite -- the "an adjustment layer is present" recomposite path.
TEST_CASE("two stacked adjustments composite/preview/backdrop without corruption") {
    core::Document doc(96, 72);
    doc.commands().push(std::make_unique<core::AddLayerCommand>(doc.root().id(), 0, paintedRaster(doc)));
    const core::LayerId a = addAdjustment(doc, core::AdjustmentKind::Levels);
    const core::LayerId b = addAdjustment(doc, core::AdjustmentKind::GaussianBlur);

    for (const core::LayerId id : {a, b}) {
        auto* adj = doc.find(id)->as<core::AdjustmentLayer>();
        REQUIRE(adj != nullptr);
        (void)render::adjustmentPreview(*adj, doc.width(), doc.height(), 32, 24);
        (void)render::adjustmentBackdrop(*adj, doc.width(), doc.height(), 32, 24);
    }
    (void)render::composite(doc);

    // Undo the top adjustment, redo it -- the scope walk must stay valid across tree mutation.
    doc.commands().undo();
    (void)render::composite(doc);
    doc.commands().redo();
    (void)render::composite(doc);
    CHECK(doc.find(b) != nullptr);
}

// The dock's scope-preview thumbnails (adjustmentScopeRevision + render::adjustmentPreview) with two
// adjustment layers, retargeting the active layer between them across many refreshes.
TEST_CASE("layer-panel scope-preview thumbnails hold across two adjustments") {
    core::Document doc(80, 60);
    doc.commands().push(std::make_unique<core::AddLayerCommand>(doc.root().id(), 0, paintedRaster(doc)));
    const core::LayerId a = addAdjustment(doc, core::AdjustmentKind::Exposure);
    const core::LayerId b = addAdjustment(doc, core::AdjustmentKind::Levels);

    Fl_Double_Window parent(0, 0, 400, 400);
    parent.begin();
    auto* dock = new ui::LayerPanel(0, 0, 300, 380);
    parent.end();
    dock->setDocument(&doc);

    for (int i = 0; i < 8; ++i) {
        dock->setActive(i % 2 == 0 ? a : b);
        dock->refresh();
        dock->refreshThumbnails();
        // Drift a param on the active adjustment (the scope revision must pick it up + rebuild).
        auto* adj = doc.find(i % 2 == 0 ? a : b)->as<core::AdjustmentLayer>();
        adj->params()["exposure"] = 0.1 * i;
        dock->refreshThumbnails();
    }
    CHECK(doc.find(a) != nullptr);
}

// The pinned editor as the host drives it: faded (the offscreen blend), rebuilt/retargeted between
// two adjustment layers of DIFFERENT and SAME kind, with undo/redo drift and a real draw each
// cycle. This is the round-3/round-4 fade-blend + rebuild interaction.
//
// HEADLESS on purpose. This case used to parent.show() a real 640x480 top-level and drive draw()
// through Fl::check(); on a live compositor (KWin / XWayland) that pops a visible window that stays
// mapped and wedges the process in futex_wait -- "the spike window" -- deadlocking the whole suite.
// refreshFadeBlend() only runs while shown(), and shown() cannot be true without a mapped window,
// so its fade path was never reachable without one. Instead we drive the SAME child-render
// machinery it uses (adjustment_panel.cpp: each CHILD into an Fl_Image_Surface -- the window itself
// snapshots BLACK) offscreen, and keep the reflect/rebuild/retarget/undo lifetimes running with no
// window at all -- so ASan walks the primary UAF paths on every run (the panel is never mapped),
// while the offscreen draw is gated like every other render test (needs a display; FLTK/X11 leak on
// teardown under LSan, so it is skipped under ASan).
TEST_CASE("adjustment panel: fade blend + rebuild/retarget across two layers (headless)") {
    core::Document doc(120, 90);
    doc.commands().push(std::make_unique<core::AddLayerCommand>(doc.root().id(), 0, paintedRaster(doc)));
    const core::LayerId levelsA = addAdjustment(doc, core::AdjustmentKind::Levels);
    const core::LayerId levelsB = addAdjustment(doc, core::AdjustmentKind::Levels);
    const core::LayerId exposure = addAdjustment(doc, core::AdjustmentKind::Exposure);
    const core::LayerId blur = addAdjustment(doc, core::AdjustmentKind::GaussianBlur);

    // A child of an UNSHOWN parent, exactly the MainWindow child order -- but the parent is NEVER
    // show()n (that is the spike window). Both destruct at scope exit, having never been mapped.
    Fl_Double_Window parent(0, 0, 640, 480);
    parent.begin();
    auto* panel = new ui::AdjustmentPanel();
    parent.end();

    panel->setPlacementProviders([] { return common::Rect{0, 0, 640, 480}; });
    // The backdrop provider = render::adjustmentBackdrop for the panel's current target (the host's).
    panel->setBackdropProvider([&]() -> common::Image {
        core::Layer* l = doc.find(panel->target());
        auto* adj = l != nullptr ? l->as<core::AdjustmentLayer>() : nullptr;
        if (adj == nullptr)
            return {};
        return render::adjustmentBackdrop(*adj, doc.width(), doc.height(), 40, 30);
    });
    // The under-image provider = a panel-sized raster (the host samples the composite; a solid fill
    // exercises the same blend-buffer math without needing the canvas).
    panel->setUnderProvider([](int w, int h) -> common::Image {
        if (w <= 0 || h <= 0)
            return {};
        common::Image img(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
        for (std::size_t p = 0; p < img.rgba.size(); ++p)
            img.rgba[p] = static_cast<std::uint8_t>(p * 7);
        return img;
    });
    panel->setOnEdit([&](const std::string& /*id*/,
                         std::function<void(std::map<std::string, double>&)> mutate) {
        core::Layer* l = doc.find(panel->target());
        auto* adj = l != nullptr ? l->as<core::AdjustmentLayer>() : nullptr;
        if (adj == nullptr)
            return;
        std::map<std::string, double> next = adj->params();
        mutate(next);
        doc.commands().push(std::make_unique<core::SetAdjustmentParamsCommand>(adj->id(),
                                                                              std::move(next),
                                                                              "Edit", 0));
    });

    // What refreshFadeBlend() does, minus the shown() gate: render each CHILD (never the window --
    // an Fl_Window snapshots BLACK) into an Fl_Image_Surface, so a dangling child/target pointer
    // after a retarget is dereferenced right here. Gated like every render test: it needs a display,
    // and FLTK/X11 internals leak on teardown under LSan, so under ASan we skip it and rely on the
    // window-free reflect/rebuild/retarget/undo walk above for the UAF coverage.
    const auto shootPanel = [&] {
#if defined(__SANITIZE_ADDRESS__) ||                                                               \
    (defined(__has_feature) && __has_feature(address_sanitizer)) // NOLINT
        return;
#endif
        if (std::getenv("DISPLAY") == nullptr && std::getenv("WAYLAND_DISPLAY") == nullptr)
            return;
        Fl_Image_Surface surf(std::max(1, panel->w()), std::max(1, panel->h()));
        Fl_Surface_Device::push_current(&surf);
        for (int i = 0; i < panel->children(); ++i)
            surf.draw(panel->child(i), panel->child(i)->x(), panel->child(i)->y());
        Fl_Surface_Device::pop_current();
        for (int i = 0; i < panel->children(); ++i)
            panel->child(i)->clear_damage(); // they were "drawn" offscreen; stop re-damage loops
    };

    // Open on the first adjustment (host: reflect then place; here just reflect -- no show).
    auto* first = doc.find(levelsA)->as<core::AdjustmentLayer>();
    panel->reflect(*first);
    shootPanel();

    const core::LayerId order[] = {levelsA, levelsB, exposure, blur, exposure, levelsB, levelsA, blur};
    for (int cycle = 0; cycle < 3; ++cycle) {
        for (const core::LayerId id : order) {
            auto* adj = doc.find(id)->as<core::AdjustmentLayer>();
            REQUIRE(adj != nullptr);
            panel->reflect(*adj);            // retarget (+ rebuild when the kind changes)
            panel->setFade(0.45);            // the host fades an occluding panel (state only, unshown)
            shootPanel();                    // draw the retargeted children offscreen
            // undo/redo param drift the host re-syncs through reflect()
            adj->params()["gamma"] = 1.0 + 0.01 * cycle;
            panel->reflect(*adj);
            panel->setFade(1.0);             // back to opaque: drops the fade cache
            shootPanel();
        }
    }
    CHECK(doc.find(blur) != nullptr);
}
