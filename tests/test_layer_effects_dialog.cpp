#include "ui/layer_effects_dialog.hpp"

#include <doctest/doctest.h>

#include <optional>
#include <variant>

#include "common/image.hpp"
#include "core/layer_effects.hpp"

using namespace mosaic;

namespace {
// Records what the dialog pushes back to its host, so the seed -> working -> host wiring can be
// checked headlessly (the dialog is built but never shown, like the Type 3D panel test).
struct Recorder {
    int applyCount = 0;
    std::optional<core::LayerEffects> lastApply;
    ui::LayerEffectsHost host() {
        ui::LayerEffectsHost h;
        h.applyLive = [this](const std::optional<core::LayerEffects>& fx) {
            ++applyCount;
            lastApply = fx;
        };
        h.renderPreview = [](int, int) { return ui::PreviewContent{}; };
        h.commit = [](const std::optional<core::LayerEffects>&) {};
        h.foreground = [] { return common::Color8{0, 0, 0, 255}; };
        return h;
    }
};
}  // namespace

TEST_CASE("Layer Effects dialog builds and seeds its working copy from the model") {
    Recorder rec;
    ui::LayerEffectsDialog dlg(rec.host());
    CHECK(dlg.w() == 760);
    CHECK(dlg.h() == 580);

    // No effects -> the layer stays effect-less (nullopt applied live).
    dlg.seed("Background", std::nullopt);
    CHECK(rec.applyCount >= 1);
    CHECK_FALSE(rec.lastApply.has_value());

    // A stroked layer seeds the working copy; seed applies it live so the canvas shows it.
    core::LayerEffects fx;
    core::StrokeEffect s;
    s.width = 5.0f;
    s.align = core::StrokeEffect::Align::Outside;
    s.enabled = true;
    fx.strokes.push_back(s);
    fx.fillOpacity = 0.5f;
    dlg.seed("Shape", fx);
    REQUIRE(rec.lastApply.has_value());
    REQUIRE(rec.lastApply->strokes.size() == 1);
    CHECK(rec.lastApply->strokes[0].width == doctest::Approx(5.0f));
    CHECK(rec.lastApply->fillOpacity == doctest::Approx(0.5f));
}

TEST_CASE("Layer Effects dialog builds the overlay + gradient panels (LE-c) without crashing") {
    Recorder rec;
    ui::LayerEffectsDialog dlg(rec.host());

    // A stack exercising every LE-c control path: a gradient stroke, an enabled colour overlay, and an
    // enabled gradient overlay -- rebuildStack() must build the kind dropdowns, paint chips and panels.
    core::LayerEffects fx;
    core::StrokeEffect s;
    s.width = 4.0f;
    s.enabled = true;
    s.paint = core::vec::Gradient{core::vec::GradientType::Radial,
                                  {{0.0, {1, 0, 0, 1}}, {1.0, {0, 0, 1, 1}}},
                                  common::Affine2D::identity(),
                                  core::vec::SpreadMethod::Pad};
    fx.strokes.push_back(s);
    fx.colorOverlay.paint = core::vec::SolidPaint{{0, 1, 0, 1}};
    fx.colorOverlay.enabled = true;
    fx.gradientOverlay.paint = core::vec::Gradient{core::vec::GradientType::Linear,
                                                   {{0.0, {1, 1, 0, 1}}, {1.0, {0, 1, 1, 1}}},
                                                   common::Affine2D::identity(),
                                                   core::vec::SpreadMethod::Reflect};
    fx.gradientOverlay.enabled = true;

    dlg.seed("Styled", fx);
    REQUIRE(rec.lastApply.has_value());
    CHECK(rec.lastApply->colorOverlay.enabled);
    CHECK(rec.lastApply->gradientOverlay.enabled);
    REQUIRE(rec.lastApply->strokes.size() == 1);
    CHECK(std::holds_alternative<core::vec::Gradient>(rec.lastApply->strokes[0].paint));

    // Re-seeding to an empty stack clears everything back to no effects.
    dlg.seed("Plain", std::nullopt);
    CHECK_FALSE(rec.lastApply.has_value());
}
