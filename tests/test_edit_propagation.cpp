// DOES THE USER'S WORK REACH THE CANVAS? -- the axis this suite could not see.
//
// The rest of the suite asks two questions about pixels: are they RIGHT (goldens, GPU/CPU parity,
// per-lane unit tests) and what do they COST (test_composite_budget.cpp). Both interrogate ONE
// composite of ONE document. Neither asks the question every user actually asks, which is about
// the SECOND composite:
//
//     I changed something. Did the canvas change?
//
// That gap shipped in 0.3.2. A performance change moved the frame's text/texture cache refresh
// ahead of the recomposite drain, which was correct on cost and wrong on plumbing: the refresh
// names the band it re-rendered EXACTLY ONCE, the earlier call consumed that report, the drain's
// call found the caches current and reported nothing, and the region composite patched an empty
// rect. Documents with 3D type OPENED perfectly -- the first composite is unconditional -- and then
// no edit to the type ever appeared again. Every existing test passed, because every existing test
// composites once.
//
// So the assertions here are about the DELTA, and each edit is put through the same three:
//
//   1. WORK HAPPENED.       The composite after the edit differs from the composite before it.
//                           An edit that changes nothing on screen is the bug, whatever the model
//                           says about itself.
//   2. THE REPORT IS HONEST. Every pixel that changed lies inside the rect the pipeline said would
//                           change (a command's dirtyRegion, unioned with what the pre-composite
//                           cache refresh reported). Under-reporting is how work becomes invisible
//                           on the region path even when the renderer is perfect.
//   3. THE SHORTCUT AGREES. Patching the old composite with compositeRegion() over that rect
//                           reproduces the full recomposite. This is the path the app actually
//                           takes for an interactive edit; a divergence here means the canvas and
//                           an export disagree about the same document.
//
// ...plus a fourth, run over every case at once at the bottom: UNDO PUTS IT BACK, pixel for pixel.
//
// ⚠ WHAT THIS FILE CANNOT REACH. The 0.3.2 defect itself lived in ui/app_window.cpp, in a private
// method of a class with no header, and nothing here would have caught it. That seam is held by
// the compiler instead: `ensureTextCaches()` is [[nodiscard]] and every caller but the full
// composite routes its report through one `settleTextCaches()` wrapper. What this file holds is
// the contract UNDERNEATH that seam -- that the report exists, covers what moved, and can be
// composited from -- for every kind of edit the application can make, so the day a renderer or a
// cache-validity key stops honouring it, a test says so instead of a user.
//
// ⚠ AND IT IS A MATRIX, ON PURPOSE. The 3D-type regression is one row. Brushing, vector objects,
// adjustments, layer effects, the texture generators, masks and layer chrome are the others, and
// they are here because the failure was never about text: it was about an edit and a composite
// losing track of each other, which any of them can do.

#include "common/geometry.hpp"
#include "common/image.hpp"
#include "core/command.hpp"
#include "core/commands.hpp"
#include "core/document.hpp"
#include "core/layer.hpp"
#include "core/layer_effects.hpp"
#include "core/text/extrude.hpp"
#include "core/text/shaping.hpp"
#include "core/text/text_layer_render.hpp"
#include "core/text/text_model.hpp"
#include "core/texture/texture_layer_render.hpp"
#include "core/texture/texture_params.hpp"
#include "core/vector/geometry.hpp"
#include "core/vector/object.hpp"
#include "core/vector/paint.hpp"
#include "platform/font_db.hpp"
#include "render/compositor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <doctest/doctest.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace core = mosaic::core;
namespace render = mosaic::render;
namespace text = mosaic::core::text;
namespace texture = mosaic::core::texture;
namespace vec = mosaic::core::vec;
using mosaic::common::Affine2D;
using mosaic::common::Color8;
using mosaic::common::ColorF;
using mosaic::common::Image;
using mosaic::common::Rect;

// Small enough that a case costs milliseconds, big enough that a 3D solid, a bevel and a blur have
// somewhere to land. Every fixture is this size.
constexpr std::uint32_t kW = 320;
constexpr std::uint32_t kH = 240;

// ---------------------------------------------------------------------------------------------
// The frame, in miniature
// ---------------------------------------------------------------------------------------------

// The app's pre-composite settle (ui/app_window.cpp `ensureTextCaches`), reproduced here in the
// same order and with the same hand-back: text pixel caches, then texture-generator caches, both
// reporting the DOCUMENT-space band they visibly changed. A test that composited without this
// would be testing a document whose text layers have no pixels.
struct Settle {
    text::TextShaper shaper;
    mosaic::platform::FontDB fonts;

    Rect operator()(core::Document& doc) {
        Rect dirty{};
        text::refreshTextCaches(doc, shaper, fonts, core::kInvalidLayerId, /*liveDrag=*/false,
                                &dirty, /*draftEditing=*/false);
        Rect texDirty{};
        texture::refreshTextureCaches(doc, &texDirty);
        return dirty.united(texDirty);
    }

    // Fonts vary by machine and a font-less sandbox must still pass the non-text rows, so the text
    // cases gate on this exactly as test_text_render.cpp does.
    [[nodiscard]] bool haveFonts() {
        if (fonts.families().empty())
            return false;
        text::FontRef r;
        r.family = fonts.defaultFamily();
        return fonts.resolve(r).has_value();
    }
};

render::CompositeOptions plainOpts() {
    render::CompositeOptions o;
    o.checkerboard = false; // a checker under the alpha would make every diff a diff of the checker
    return o;
}

Image compositeAll(const core::Document& doc) {
    const render::CompositeResult r = render::composite(doc, plainOpts(), render::Backend::Cpu);
    REQUIRE(r.ok);
    return r.image;
}

// The bounding box of every pixel where `a` and `b` differ, in document pixels; empty when the two
// images are identical. Exact, not tolerant: both sides come from the same deterministic CPU
// compositor over the same document, so any difference at all is a real difference.
Rect diffBounds(const Image& a, const Image& b) {
    REQUIRE(a.width == b.width);
    REQUIRE(a.height == b.height);
    long x0 = static_cast<long>(a.width), y0 = static_cast<long>(a.height), x1 = -1, y1 = -1;
    for (std::uint32_t y = 0; y < a.height; ++y) {
        for (std::uint32_t x = 0; x < a.width; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * a.width + x) * 4;
            if (a.rgba[p] != b.rgba[p] || a.rgba[p + 1] != b.rgba[p + 1] ||
                a.rgba[p + 2] != b.rgba[p + 2] || a.rgba[p + 3] != b.rgba[p + 3]) {
                x0 = std::min<long>(x0, static_cast<long>(x));
                y0 = std::min<long>(y0, static_cast<long>(y));
                x1 = std::max<long>(x1, static_cast<long>(x));
                y1 = std::max<long>(y1, static_cast<long>(y));
            }
        }
    }
    if (x1 < 0)
        return {};
    return {static_cast<double>(x0), static_cast<double>(y0), static_cast<double>(x1 - x0 + 1),
            static_cast<double>(y1 - y0 + 1)};
}

// Whether `inner` lies inside `outer` once both are snapped outward to whole pixels. The reported
// rect is allowed to be generous -- over-reporting costs a bigger patch, never a wrong one -- so
// this is the only direction worth asserting.
bool covers(const Rect& outer, const Rect& inner) {
    if (inner.empty())
        return true;
    return std::floor(outer.x) <= inner.x && std::floor(outer.y) <= inner.y &&
           std::ceil(outer.right()) >= inner.right() && std::ceil(outer.bottom()) >= inner.bottom();
}

// Copy `patch` (the result of a compositeRegion at floor(roi)) into a copy of `base` -- the app's
// `patchComposite`, which is what makes a region composite show up on screen.
Image patched(const Image& base, const Image& patch, const Rect& roi) {
    Image out = base;
    const auto ox = static_cast<long>(std::floor(std::max(0.0, roi.x)));
    const auto oy = static_cast<long>(std::floor(std::max(0.0, roi.y)));
    for (std::uint32_t y = 0; y < patch.height; ++y) {
        const long dy = oy + static_cast<long>(y);
        if (dy < 0 || dy >= static_cast<long>(out.height))
            continue;
        for (std::uint32_t x = 0; x < patch.width; ++x) {
            const long dx = ox + static_cast<long>(x);
            if (dx < 0 || dx >= static_cast<long>(out.width))
                continue;
            const std::size_t s = (static_cast<std::size_t>(y) * patch.width + x) * 4;
            const std::size_t d = (static_cast<std::size_t>(dy) * out.width + dx) * 4;
            std::copy_n(&patch.rgba[s], 4, &out.rgba[d]);
        }
    }
    return out;
}

// WHERE the canvas patch's rectangle comes from -- which is a property of the EDIT, not of the
// harness, and the reason this is a parameter rather than a guess:
//
//   Settle      the app queues an UNNAMED region (ui/app_window.cpp `requestTextRecomposite`, an
//               empty seed) and the rect comes entirely from what the pre-composite cache refresh
//               reports. Every text edit takes this path, and it is the one that shipped broken.
//   Command     the edit names its own document-space rect (`Command::dirtyRegion`) -- the brush
//               and inpaint paths -- and the settle band is unioned in.
//   Whole       the command declares nullopt, the honest "unknown, assume the canvas" fallback
//               that structural and chrome edits take, and the app recomposites everything. Axes 2
//               and 3 still run, but over the whole canvas: what they check here is that
//               compositeRegion agrees with composite, not that the rect was tight.
//
// ⚠ Getting this wrong makes the test PASS for the wrong reason, which is how the first draft of
// this file failed to catch an injected under-reporting defect: `SetTextCommand` names no rect, so
// defaulting to the whole canvas made axes 2 and 3 vacuous on precisely the rows they exist for.
enum class Reported { Settle, Command, Whole };

// Run ONE edit through the frame the app runs, and hold it to the three axes at the top of this
// file. `edit` returns the command it pushed (already applied by the stack) so the harness can ask
// it where it claims to have changed pixels.
//
// Returns the composite from BEFORE the edit, so a caller can close the loop on undo.
Image checkEditReaches(std::string_view what, core::Document& doc, Settle& settle,
                       const std::function<core::Command*()>& edit, Reported source) {
    INFO("edit: " << std::string(what));

    settle(doc); // the document must be settled BEFORE, or the first composite renders the caches
                 // and the second one gets a report that belongs to the open, not to the edit
    const Image before = compositeAll(doc);

    core::Command* cmd = edit();
    const std::optional<Rect> claimed = cmd != nullptr ? cmd->dirtyRegion(doc) : std::nullopt;
    const Rect settled = settle(doc);
    const Rect whole{0, 0, static_cast<double>(doc.width()), static_cast<double>(doc.height())};

    // The rect the canvas patch would actually be built from. This is the only information the
    // region path has to work with, so it is the only thing worth asserting against.
    Rect reported{};
    switch (source) {
    case Reported::Settle:
        CHECK_MESSAGE(!claimed.has_value(),
                      "this edit now names its own rect; the case should say Reported::Command: "
                          << std::string(what));
        reported = settled;
        break;
    case Reported::Command:
        REQUIRE_MESSAGE(claimed.has_value(),
                        "this edit stopped naming a rect, so the canvas patch lost its scope: "
                            << std::string(what));
        reported = claimed->united(settled);
        break;
    case Reported::Whole:
        reported = whole.united(settled);
        break;
    }

    const Image after = compositeAll(doc);

    // 1. WORK HAPPENED.
    const Rect changed = diffBounds(before, after);
    CHECK_MESSAGE(!changed.empty(),
                  "the edit changed no pixel on the canvas: " << std::string(what));
    if (changed.empty())
        return before; // the rest would assert on nothing

    // 2. THE REPORT IS HONEST.
    INFO("changed  = " << changed.x << "," << changed.y << " " << changed.w << "x" << changed.h);
    INFO("reported = " << reported.x << "," << reported.y << " " << reported.w << "x"
                       << reported.h);
    CHECK_MESSAGE(!reported.empty(),
                  "nothing named the band that changed, so the canvas would never be patched: "
                      << std::string(what));
    CHECK_MESSAGE(covers(reported, changed),
                  "pixels changed outside the reported dirty rect: " << std::string(what));

    // 3. THE SHORTCUT AGREES.
    if (!reported.empty()) {
        const render::CompositeResult roi =
            render::compositeRegion(doc, reported, plainOpts(), render::Backend::Cpu);
        REQUIRE(roi.ok);
        CHECK_MESSAGE(patched(before, roi.image, reported) == after,
                      "the region patch and the full recomposite disagree: " << std::string(what));
    }
    return before;
}

// The fourth axis, applied to a document that has just been edited: undo must restore the exact
// pixels the edit started from. Cheap to check here and expensive to discover in a file.
void checkUndoRestores(std::string_view what, core::Document& doc, Settle& settle,
                       const Image& before) {
    INFO("undo: " << std::string(what));
    doc.commands().undo();
    settle(doc);
    CHECK_MESSAGE(compositeAll(doc) == before,
                  "undo did not restore the canvas: " << std::string(what));
}

// ---------------------------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------------------------

// A backdrop, so no case is compositing an edit over nothing: an edit that is invisible because
// the canvas is empty would pass axis 1 for the wrong reason.
void addBackdrop(core::Document& doc) {
    auto back = doc.makeRaster("Back", kW, kH);
    for (std::uint32_t y = 0; y < kH; ++y)
        for (std::uint32_t x = 0; x < kW; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * kW + x) * 4;
            back->image().rgba[p + 0] = static_cast<std::uint8_t>(30 + (x * 160) / kW);
            back->image().rgba[p + 1] = static_cast<std::uint8_t>(40 + (y * 150) / kH);
            back->image().rgba[p + 2] = 110;
            back->image().rgba[p + 3] = 255;
        }
    back->invalidateContentBounds();
    doc.root().addOnTop(std::move(back));
}

std::unique_ptr<core::Document> makeDoc() {
    auto doc = std::make_unique<core::Document>(kW, kH);
    addBackdrop(*doc);
    return doc;
}

text::CharStyle styleOf(const std::string& family, float size, ColorF fill) {
    text::CharStyle s;
    s.setSolidFill(fill);
    s.sizePx = size;
    s.font.family = family;
    return s;
}

// A text layer carrying `block`, placed away from the canvas edge so a 3D solid's projection and a
// shadow's spread both have room to grow into without being clipped by the canvas.
core::LayerId addText(core::Document& doc, text::TextBlock block) {
    auto layer = doc.makeText("Type");
    layer->setBlock(std::move(block));
    layer->setTransform(Affine2D::translation(60.0, 140.0));
    const core::LayerId id = layer->id();
    doc.root().addOnTop(std::move(layer));
    return id;
}

core::LayerId addVector(core::Document& doc) {
    auto v = doc.makeVector("Shape");
    vec::Object o;
    vec::RectShape r;
    r.size = {90.0, 70.0};
    o.geometry = vec::ParametricShape{r};
    o.fill = vec::SolidPaint{ColorF{0.9f, 0.35f, 0.2f, 1.0f}};
    v->setObject(std::move(o));
    v->setTransform(Affine2D::translation(70.0, 60.0));
    const core::LayerId id = v->id();
    doc.root().addOnTop(std::move(v));
    return id;
}

core::LayerId addRaster(core::Document& doc) {
    auto r = doc.makeRaster("Paint", kW, kH);
    r->image().fill(Color8{0, 0, 0, 0});
    r->invalidateContentBounds();
    const core::LayerId id = r->id();
    doc.root().addOnTop(std::move(r));
    return id;
}

// The block every 3D case extrudes: short (one solid, fast), big enough to read.
text::TextBlock make3dBlock(const std::string& family) {
    text::TextBlock b =
        // The colour is the RUN's, not the material's (§10.4): a 3D solid shades with the same
        // paint the flat block would fill with.
        text::makeBlock("Ag", styleOf(family, 56.0f, ColorF{0.80f, 0.55f, 0.20f, 1.0f}));
    text::Extrude e;
    e.depth = 14.0f;
    e.bevelFront.size = 2.0f;
    b.extrude = e;
    return b;
}

// Push a command through the stack (which applies it) and hand it back for its dirtyRegion.
core::Command* push(core::Document& doc, std::unique_ptr<core::Command> cmd) {
    core::Command* raw = cmd.get();
    doc.commands().push(std::move(cmd));
    return raw;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// 3D type -- the 0.3.2 row
// ---------------------------------------------------------------------------------------------

TEST_CASE("every 3D type parameter reaches the canvas") {
    // THE REGRESSION THIS FILE WAS WRITTEN FOR, stated at the level the core can hold: a document
    // opened with 3D type composited perfectly and then never changed again, because the edit and
    // the composite lost track of each other. Here the loss would have to be in the cache-validity
    // key -- a parameter the renderer consumes but `refreshTextCache` does not compare, so the
    // stale solid is served back. Each row below is a knob a user can turn in the 3D panel, and
    // each must move pixels.
    Settle settle;
    if (!settle.haveFonts())
        return;
    const std::string family = settle.fonts.defaultFamily();

    struct Knob {
        const char* what;
        void (*mutate)(text::Extrude&);
    };
    // Deliberately one knob per FACE of the pipeline -- mesh (depth, bevel), camera (orientation,
    // perspective), shading (lighting, ambient, material) -- so a lane that stops being consulted
    // is named by the row that fails rather than by a single "3D changed" assertion.
    static const Knob knobs[] = {
        {"extrude depth", [](text::Extrude& e) { e.depth = 40.0f; }},
        {"front bevel size", [](text::Extrude& e) { e.bevelFront.size = 6.0f; }},
        {"front bevel profile",
         [](text::Extrude& e) {
             e.bevelFront.size = 6.0f;
             e.bevelFront.profile = text::Bevel::Profile::Concave;
         }},
        {"orientation",
         [](text::Extrude& e) {
             e.orientation = mosaic::common::Quat::fromAxisAngle({0.0, 1.0, 0.0}, 0.5);
         }},
        {"perspective FOV", [](text::Extrude& e) { e.perspective = 55.0f; }},
        {"material metalness",
         [](text::Extrude& e) {
             e.material.metalness = 1.0f;
             e.material.roughness = 0.1f;
         }},
        {"lighting toggle", [](text::Extrude& e) { e.lightingEnabled = false; }},
        {"key light direction",
         [](text::Extrude& e) { e.lights.at(0).direction = {-0.6, -0.4, -0.7}; }},
        {"ambient", [](text::Extrude& e) { e.ambient = ColorF{0.9f, 0.2f, 0.2f, 1.0f}; }},
    };

    for (const Knob& knob : knobs) {
        auto doc = makeDoc();
        const core::LayerId id = addText(*doc, make3dBlock(family));

        const Image before = checkEditReaches(
            knob.what, *doc, settle,
            [&]() -> core::Command* {
                auto* layer = doc->find(id)->as<core::TextLayer>();
                REQUIRE(layer != nullptr);
                text::TextBlock next = layer->block();
                REQUIRE(next.extrude.has_value());
                knob.mutate(*next.extrude);
                return push(*doc, std::make_unique<core::SetTextCommand>(id, std::move(next)));
            },
            Reported::Settle);
        checkUndoRestores(knob.what, *doc, settle, before);
    }
}

TEST_CASE("turning 3D on and off reaches the canvas") {
    // The two transitions the panel's master switch makes. Flat->solid grows the layer's extent, so
    // it is also the case where a dirty band that reported only the OLD pixels would leave the new
    // solid's overhang stale on screen.
    Settle settle;
    if (!settle.haveFonts())
        return;
    const std::string family = settle.fonts.defaultFamily();

    {
        auto doc = makeDoc();
        text::TextBlock flat =
            text::makeBlock("Ag", styleOf(family, 56.0f, ColorF{0.95f, 0.95f, 0.95f, 1.0f}));
        const core::LayerId id = addText(*doc, flat);
        const Image before = checkEditReaches(
            "enable 3D", *doc, settle,
            [&]() -> core::Command* {
                text::TextBlock next = doc->find(id)->as<core::TextLayer>()->block();
                text::Extrude e;
                e.depth = 18.0f;
                next.extrude = e;
                return push(*doc, std::make_unique<core::SetTextCommand>(id, std::move(next)));
            },
            Reported::Settle);
        checkUndoRestores("enable 3D", *doc, settle, before);
    }
    {
        auto doc = makeDoc();
        const core::LayerId id = addText(*doc, make3dBlock(family));
        const Image before = checkEditReaches(
            "disable 3D", *doc, settle,
            [&]() -> core::Command* {
                text::TextBlock next = doc->find(id)->as<core::TextLayer>()->block();
                next.extrude.reset();
                return push(*doc, std::make_unique<core::SetTextCommand>(id, std::move(next)));
            },
            Reported::Settle);
        checkUndoRestores("disable 3D", *doc, settle, before);
    }
}

TEST_CASE("an edit that SHRINKS the text repaints what it vacated") {
    // The half of the dirty-band contract that a growing edit cannot test. `refreshTextCaches`
    // accumulates the OLD cached extent as well as the new one precisely because pixels the text
    // no longer covers still have to be repainted -- and a report of only the new extent is right
    // about every pixel it names while leaving a ghost of the previous glyphs on the canvas.
    //
    // Every row here makes the layer's footprint SMALLER, so the changed area is largely OUTSIDE
    // the new extent. Axis 2 fails if the vacated band goes unreported; axis 3 fails because the
    // patch then leaves the ghost behind.
    Settle settle;
    if (!settle.haveFonts())
        return;
    const std::string family = settle.fonts.defaultFamily();

    SUBCASE("deleting characters") {
        auto doc = makeDoc();
        const core::LayerId id =
            addText(*doc, text::makeBlock("Wide Text", styleOf(family, 44.0f, ColorF{0, 0, 0, 1})));
        const Image before = checkEditReaches(
            "delete characters", *doc, settle,
            [&]() -> core::Command* {
                return push(*doc, std::make_unique<core::SetTextCommand>(
                                      id, text::makeBlock(
                                              "W", styleOf(family, 44.0f, ColorF{0, 0, 0, 1}))));
            },
            Reported::Settle);
        checkUndoRestores("delete characters", *doc, settle, before);
    }
    SUBCASE("shrinking the type size") {
        auto doc = makeDoc();
        const core::LayerId id =
            addText(*doc, text::makeBlock("Big", styleOf(family, 72.0f, ColorF{0, 0, 0, 1})));
        const Image before = checkEditReaches(
            "shrink type size", *doc, settle,
            [&]() -> core::Command* {
                return push(*doc, std::make_unique<core::SetTextCommand>(
                                      id, text::makeBlock(
                                              "Big", styleOf(family, 18.0f, ColorF{0, 0, 0, 1}))));
            },
            Reported::Settle);
        checkUndoRestores("shrink type size", *doc, settle, before);
    }
    SUBCASE("collapsing the extrusion") {
        auto doc = makeDoc();
        text::TextBlock deep = make3dBlock(family);
        deep.extrude->depth = 60.0f;
        deep.extrude->orientation = mosaic::common::Quat::fromAxisAngle({0.0, 1.0, 0.0}, 0.6);
        const core::LayerId id = addText(*doc, deep);
        const Image before = checkEditReaches(
            "collapse the extrusion", *doc, settle,
            [&]() -> core::Command* {
                text::TextBlock next = doc->find(id)->as<core::TextLayer>()->block();
                next.extrude->depth = 2.0f;
                return push(*doc, std::make_unique<core::SetTextCommand>(id, std::move(next)));
            },
            Reported::Settle);
        checkUndoRestores("collapse the extrusion", *doc, settle, before);
    }
}

TEST_CASE("a 3D block's overlay effects reach the canvas") {
    // An extruded block CONSUMES the colour/gradient/pattern overlays per face (S30-e), which is
    // why they are part of the pixel cache's validity key rather than the 2D effect pass's
    // business. A key that forgot them would serve the previously baked solid forever -- the exact
    // shape of the 0.3.2 failure, one layer down.
    Settle settle;
    if (!settle.haveFonts())
        return;

    auto doc = makeDoc();
    const core::LayerId id = addText(*doc, make3dBlock(settle.fonts.defaultFamily()));

    const Image before = checkEditReaches(
        "3D colour overlay", *doc, settle,
        [&]() -> core::Command* {
            core::LayerEffects fx;
            fx.colorOverlay.enabled = true;
            fx.colorOverlay.paint = vec::SolidPaint{ColorF{0.1f, 0.9f, 0.3f, 1.0f}};
            fx.colorOverlay.opacity = 1.0f;
            return push(*doc, std::make_unique<core::SetLayerEffectsCommand>(id, std::move(fx)));
        },
        Reported::Whole);
    checkUndoRestores("3D colour overlay", *doc, settle, before);
}

TEST_CASE("a reflection-env swap re-renders the mirror without reading as a content edit") {
    // TWO assertions that used to be in conflict, and the conflict shipped as an infinite loop.
    //
    // The reflect-canvas mirror is a snapshot of the document BELOW a 3D block, built by the app
    // and sampled by the extrude lanes. It is render support -- no undo, no effect on the layer's
    // silhouette -- but it is IN the pixels, so installing one has to re-render the pixel cache.
    // setReflectionEnv got that by calling invalidateContentBounds(), i.e. by claiming the document
    // content had changed.
    //
    // It had not, and the app's reflectStackFingerprint believed it: that fingerprint mixes every
    // OTHER layer's contentRevision to decide whether a mirror is stale, so on a document with TWO
    // reflect-canvas layers, building A's mirror staled B's, whose rebuild staled A's, forever --
    // a full-canvas recomposite every 0.3 s settle. Measured on the S60 fixture: ten rebuilds in
    // 150 s, no sign of stopping, each dragging a 16 s composite behind it. Two mirrors facing each
    // other have no fixed point; the loop has to be truncated at one bounce.
    //
    // So: the pixels must move, and contentRevision must not.
    Settle settle;
    if (!settle.haveFonts())
        return;

    auto doc = makeDoc();
    text::TextBlock block = make3dBlock(settle.fonts.defaultFamily());
    block.extrude->material.metalness = 1.0f; // a mirror needs something to mirror WITH
    block.extrude->material.roughness = 0.0f;
    block.extrude->reflectCanvas = true;
    block.extrude->orientation = mosaic::common::Quat::fromAxisAngle({1.0, 0.0, 0.0}, 0.9);
    const core::LayerId id = addText(*doc, block);

    settle(*doc);
    auto* layer = doc->find(id)->as<core::TextLayer>();
    REQUIRE(layer != nullptr);
    const Image before = compositeAll(*doc);
    const std::uint64_t revBefore = layer->contentRevision();

    // A saturated mirror, so its arrival is unmistakable in the composited pixels.
    mosaic::common::ImageF env(16, 16);
    for (std::size_t i = 0; i < env.rgba.size(); i += 4) {
        env.rgba[i + 0] = 1.0f; // red
        env.rgba[i + 3] = 1.0f;
    }
    layer->setReflectionEnv(std::move(env), Affine2D::scaling(0.05, 0.05));

    CHECK_FALSE(layer->cacheCurrent()); // ... the pixels are now stale and must re-render
    const Rect band = settle(*doc);
    const Image after = compositeAll(*doc);

    CHECK_MESSAGE(diffBounds(before, after).empty() == false,
                  "the mirror never reached the canvas");
    CHECK_MESSAGE(!band.empty(), "the re-render reported no band, so a region pass would miss it");
    CHECK_MESSAGE(layer->contentRevision() == revBefore,
                  "installing a mirror moved the CONTENT revision -- this is the mirror loop");
}

// ---------------------------------------------------------------------------------------------
// Flat type
// ---------------------------------------------------------------------------------------------

TEST_CASE("typing and restyling reach the canvas") {
    Settle settle;
    if (!settle.haveFonts())
        return;
    const std::string family = settle.fonts.defaultFamily();

    struct Row {
        const char* what;
        void (*mutate)(text::TextBlock&, const std::string& family);
    };
    static const Row rows[] = {
        {"a keystroke",
         [](text::TextBlock& b, const std::string& f) {
             b = text::makeBlock("Hello!", styleOf(f, 40.0f, ColorF{0, 0, 0, 1}));
         }},
        {"type size",
         [](text::TextBlock& b, const std::string& f) {
             b = text::makeBlock("Hello", styleOf(f, 68.0f, ColorF{0, 0, 0, 1}));
         }},
        {"fill colour",
         [](text::TextBlock& b, const std::string& f) {
             b = text::makeBlock("Hello", styleOf(f, 40.0f, ColorF{0.95f, 0.2f, 0.1f, 1.0f}));
         }},
        {"baseline bend", [](text::TextBlock& b, const std::string&) { b.bend = 0.6f; }},
        {"anti-alias mode",
         [](text::TextBlock& b, const std::string&) { b.aa = text::AntiAlias::None; }},
    };

    for (const Row& row : rows) {
        auto doc = makeDoc();
        const core::LayerId id =
            addText(*doc, text::makeBlock("Hello", styleOf(family, 40.0f, ColorF{0, 0, 0, 1})));
        const Image before = checkEditReaches(
            row.what, *doc, settle,
            [&]() -> core::Command* {
                text::TextBlock next = doc->find(id)->as<core::TextLayer>()->block();
                row.mutate(next, family);
                return push(*doc, std::make_unique<core::SetTextCommand>(id, std::move(next)));
            },
            Reported::Settle);
        checkUndoRestores(row.what, *doc, settle, before);
    }
}

// ---------------------------------------------------------------------------------------------
// Brushing
// ---------------------------------------------------------------------------------------------

TEST_CASE("a brush dab reaches the canvas") {
    // The one edit that ALREADY names a tight rect (SetLayerPixelsCommand's region form is what the
    // stroke path pushes), so its axis-2 assertion is the strict one in this file: the claim is a
    // real bounding box and nothing may change outside it.
    Settle settle;
    auto doc = makeDoc();
    const core::LayerId id = addRaster(*doc);

    constexpr long kOx = 120, kOy = 90;
    Image dab(24, 24);
    for (std::uint32_t y = 0; y < dab.height; ++y)
        for (std::uint32_t x = 0; x < dab.width; ++x) {
            const double dx = double(x) - 11.5, dy = double(y) - 11.5;
            const double d = std::sqrt(dx * dx + dy * dy);
            const auto a =
                static_cast<std::uint8_t>(std::clamp(255.0 * (1.0 - d / 12.0), 0.0, 255.0));
            const std::size_t p = (static_cast<std::size_t>(y) * dab.width + x) * 4;
            dab.rgba[p + 0] = 240;
            dab.rgba[p + 1] = 30;
            dab.rgba[p + 2] = 60;
            dab.rgba[p + 3] = a;
        }

    const Image before = checkEditReaches(
        "brush dab", *doc, settle,
        [&]() -> core::Command* {
            return push(*doc, std::make_unique<core::SetLayerPixelsCommand>(id, dab, kOx, kOy));
        },
        Reported::Command);
    checkUndoRestores("brush dab", *doc, settle, before);
}

TEST_CASE("a brush dab on a TRANSFORMED layer reaches the canvas where the layer is") {
    // Same edit, on a layer whose transform means its pixel space and the document's are not the
    // same space. dirtyRegion must report the DOCUMENT projection of the painted rect; reporting
    // the layer-local one would patch a band the paint is not in, and the dab would go missing on
    // screen while being perfectly present in the file.
    Settle settle;
    auto doc = makeDoc();
    auto layer = doc->makeRaster("Paint", 200, 200);
    layer->image().fill(Color8{0, 0, 0, 0});
    layer->invalidateContentBounds();
    layer->setTransform(Affine2D::trs({40.0, 30.0}, 0.4, {1.3, 1.3}));
    const core::LayerId id = layer->id();
    doc->root().addOnTop(std::move(layer));

    Image dab(20, 20);
    dab.fill(Color8{20, 220, 90, 255});

    const Image before = checkEditReaches(
        "dab on a rotated layer", *doc, settle,
        [&]() -> core::Command* {
            return push(*doc, std::make_unique<core::SetLayerPixelsCommand>(id, dab, 60, 70));
        },
        Reported::Command);
    checkUndoRestores("dab on a rotated layer", *doc, settle, before);
}

// ---------------------------------------------------------------------------------------------
// Vector
// ---------------------------------------------------------------------------------------------

TEST_CASE("vector object edits reach the canvas") {
    Settle settle;

    struct Row {
        const char* what;
        void (*mutate)(vec::Object&);
    };
    static const Row rows[] = {
        {"fill colour",
         [](vec::Object& o) { o.fill = vec::SolidPaint{ColorF{0.15f, 0.45f, 0.95f, 1.0f}}; }},
        {"geometry size",
         [](vec::Object& o) {
             vec::RectShape r;
             r.size = {150.0, 40.0};
             o.geometry = vec::ParametricShape{r};
         }},
        {"corner radius",
         [](vec::Object& o) {
             o.geometry = vec::ParametricShape{vec::RectShape::uniform({90.0, 70.0}, 24.0)};
         }},
        {"stroke",
         [](vec::Object& o) {
             o.stroke.enabled = true; // ⚠ a Stroke defaults to DISABLED with NoPaint; setting the
             o.stroke.paint =         //   width alone draws nothing, which axis 1 catches
                 vec::SolidPaint{ColorF{0.05f, 0.05f, 0.05f, 1.0f}};
             o.stroke.width = 9.0;
         }},
    };

    for (const Row& row : rows) {
        auto doc = makeDoc();
        const core::LayerId id = addVector(*doc);
        const Image before = checkEditReaches(
            row.what, *doc, settle,
            [&]() -> core::Command* {
                vec::Object next = *doc->find(id)->as<core::VectorLayer>()->object();
                row.mutate(next);
                return push(*doc,
                            std::make_unique<core::SetVectorObjectCommand>(id, std::move(next)));
            },
            Reported::Whole);
        checkUndoRestores(row.what, *doc, settle, before);
    }
}

// ---------------------------------------------------------------------------------------------
// Adjustments
// ---------------------------------------------------------------------------------------------

TEST_CASE("adjustment parameter edits reach the canvas") {
    // Both families: a COLOUR adjustment (per-pixel) and a SPATIAL one (reads the backdrop's
    // neighbourhood, so the compositor routes it through the blur branch and the region machinery
    // has to account for its reach). The spatial row is the one that would catch a region pass
    // that patched only the slider's own footprint and left a halo of stale blur around it.
    Settle settle;

    struct Row {
        const char* what;
        core::AdjustmentKind kind;
        const char* key;
        double from;
        double to;
    };
    static const Row rows[] = {
        {"brightness", core::AdjustmentKind::BrightnessContrast, "brightness", 0.0, 0.6},
        {"contrast", core::AdjustmentKind::BrightnessContrast, "contrast", 0.0, 0.8},
        {"exposure", core::AdjustmentKind::Exposure, "exposure", 0.0, 1.5},
        {"hue", core::AdjustmentKind::HueSaturation, "hue", 0.0, 90.0},
        {"posterize levels", core::AdjustmentKind::Posterize, "levels", 8.0, 2.0},
        {"gaussian blur radius", core::AdjustmentKind::GaussianBlur, "radius", 1.0, 9.0},
    };

    for (const Row& row : rows) {
        auto doc = makeDoc();
        auto adj = doc->makeAdjustment("Adjust", row.kind);
        adj->params()[row.key] = row.from;
        const core::LayerId id = adj->id();
        doc->root().addOnTop(std::move(adj));

        const Image before = checkEditReaches(
            row.what, *doc, settle,
            [&]() -> core::Command* {
                auto params = doc->find(id)->as<core::AdjustmentLayer>()->params();
                params[row.key] = row.to;
                return push(*doc, std::make_unique<core::SetAdjustmentParamsCommand>(
                                      id, std::move(params)));
            },
            Reported::Whole);
        checkUndoRestores(row.what, *doc, settle, before);
    }
}

// ---------------------------------------------------------------------------------------------
// TexGen
// ---------------------------------------------------------------------------------------------

TEST_CASE("texture-generator parameter edits reach the canvas") {
    // The other renderer-filled pixel cache, and the one that rides the SAME pre-composite seam the
    // text caches do (`refreshTextureCaches` is the second half of the app's settle). Its report is
    // therefore lost by exactly the same mistakes; a params edit that does not re-render is the
    // 0.3.2 failure wearing the other hat.
    Settle settle;

    struct Row {
        const char* what;
        void (*mutate)(texture::TextureParams&);
    };
    static const Row rows[] = {
        {"seed", [](texture::TextureParams& p) { p.seed = 12345; }},
        {"scale", [](texture::TextureParams& p) { p.scale = 3.0; }},
        {"paper tint",
         [](texture::TextureParams& p) {
             std::get<texture::PaperParams>(p.spec).tint = ColorF{0.2f, 0.5f, 0.35f, 1.0f};
         }},
        {"paper roughness",
         [](texture::TextureParams& p) {
             std::get<texture::PaperParams>(p.spec).roughness = 0.95;
         }},
        {"raking light angle",
         [](texture::TextureParams& p) {
             std::get<texture::PaperParams>(p.spec).lightAzimuthDeg = 90.0;
         }},
    };

    for (const Row& row : rows) {
        auto doc = makeDoc();
        auto tex =
            doc->makeTexture("Paper", texture::defaultTextureParams(texture::Generator::Paper));
        const core::LayerId id = tex->id();
        doc->root().addOnTop(std::move(tex));

        const Image before = checkEditReaches(
            row.what, *doc, settle,
            [&]() -> core::Command* {
                texture::TextureParams next = doc->find(id)->as<core::TextureLayer>()->params();
                row.mutate(next);
                return push(*doc, std::make_unique<core::SetTextureCommand>(id, std::move(next)));
            },
            Reported::Settle);
        checkUndoRestores(row.what, *doc, settle, before);
    }
}

TEST_CASE("switching the texture generator reaches the canvas") {
    Settle settle;
    auto doc = makeDoc();
    auto tex = doc->makeTexture("Paper", texture::defaultTextureParams(texture::Generator::Paper));
    const core::LayerId id = tex->id();
    doc->root().addOnTop(std::move(tex));

    const Image before = checkEditReaches(
        "generator switch", *doc, settle,
        [&]() -> core::Command* {
            return push(*doc, std::make_unique<core::SetTextureCommand>(
                                  id, texture::defaultTextureParams(texture::Generator::Marble)));
        },
        Reported::Settle);
    checkUndoRestores("generator switch", *doc, settle, before);
}

// ---------------------------------------------------------------------------------------------
// Layer effects
// ---------------------------------------------------------------------------------------------

TEST_CASE("layer-effect edits reach the canvas") {
    // Effects grow a layer's footprint OUTSIDE its own pixels, which is what makes them worth their
    // own row: a dirty rect derived from the layer's content bounds rather than its effects bounds
    // is right about every pixel it names and silently wrong about the shadow.
    Settle settle;

    struct Row {
        const char* what;
        void (*mutate)(core::LayerEffects&);
    };
    static const Row rows[] = {
        {"drop shadow on",
         [](core::LayerEffects& fx) {
             core::ShadowEffect s;
             s.enabled = true;
             s.size = 10.0f;
             s.distance = 8.0f;
             fx.dropShadows.push_back(s);
         }},
        {"outer glow on",
         [](core::LayerEffects& fx) {
             fx.outerGlow.enabled = true;
             fx.outerGlow.size = 12.0f;
             fx.outerGlow.paint = vec::SolidPaint{ColorF{1.0f, 0.9f, 0.3f, 1.0f}};
         }},
        {"stroke on",
         [](core::LayerEffects& fx) {
             core::StrokeEffect s;
             s.enabled = true;
             s.width = 5.0f;
             s.paint = vec::SolidPaint{ColorF{0.0f, 0.0f, 0.0f, 1.0f}};
             fx.strokes.push_back(s);
         }},
        {"bevel on",
         [](core::LayerEffects& fx) {
             fx.bevel.enabled = true;
             fx.bevel.size = 8.0f;
         }},
        {"fill opacity", [](core::LayerEffects& fx) { fx.fillOpacity = 0.35f; }},
    };

    for (const Row& row : rows) {
        auto doc = makeDoc();
        const core::LayerId id = addVector(*doc);
        const Image before = checkEditReaches(
            row.what, *doc, settle,
            [&]() -> core::Command* {
                core::LayerEffects fx;
                row.mutate(fx);
                return push(*doc,
                            std::make_unique<core::SetLayerEffectsCommand>(id, std::move(fx)));
            },
            Reported::Whole);
        checkUndoRestores(row.what, *doc, settle, before);
    }
}

// ---------------------------------------------------------------------------------------------
// Layer chrome
// ---------------------------------------------------------------------------------------------

TEST_CASE("layer chrome edits reach the canvas") {
    // Opacity, blend mode, visibility, transform, mask. None of these touches a pixel the layer
    // owns, which is exactly why they are easy to leave out of an invalidation path -- and why a
    // user who drags an opacity slider and sees nothing has the same bug as a user typing into 3D
    // text.
    Settle settle;

    SUBCASE("opacity") {
        auto doc = makeDoc();
        const core::LayerId id = addVector(*doc);
        const Image before = checkEditReaches(
            "opacity", *doc, settle,
            [&]() -> core::Command* {
                return push(*doc, std::make_unique<core::SetOpacityCommand>(id, 0.35f));
            },
            Reported::Whole);
        checkUndoRestores("opacity", *doc, settle, before);
    }
    SUBCASE("blend mode") {
        auto doc = makeDoc();
        const core::LayerId id = addVector(*doc);
        const Image before = checkEditReaches(
            "blend mode", *doc, settle,
            [&]() -> core::Command* {
                return push(*doc, std::make_unique<core::SetBlendModeCommand>(
                                      id, core::BlendMode::Multiply));
            },
            Reported::Whole);
        checkUndoRestores("blend mode", *doc, settle, before);
    }
    SUBCASE("visibility") {
        auto doc = makeDoc();
        const core::LayerId id = addVector(*doc);
        const Image before = checkEditReaches(
            "visibility", *doc, settle,
            [&]() -> core::Command* {
                return push(*doc, std::make_unique<core::SetVisibleCommand>(id, false));
            },
            Reported::Whole);
        checkUndoRestores("visibility", *doc, settle, before);
    }
    SUBCASE("transform") {
        auto doc = makeDoc();
        const core::LayerId id = addVector(*doc);
        const Image before = checkEditReaches(
            "transform", *doc, settle,
            [&]() -> core::Command* {
                return push(*doc, std::make_unique<core::SetTransformCommand>(
                                      id, Affine2D::trs({140.0, 110.0}, 0.35, {1.4, 1.4})));
            },
            Reported::Whole);
        checkUndoRestores("transform", *doc, settle, before);
    }
}

TEST_CASE("a mask edit reaches the canvas") {
    Settle settle;
    auto doc = makeDoc();
    auto layer = doc->makeRaster("Paint", kW, kH);
    layer->image().fill(Color8{220, 60, 40, 255});
    layer->invalidateContentBounds();
    const core::LayerId id = layer->id();
    doc->root().addOnTop(std::move(layer));

    core::RasterMask mask(kW, kH, 255);
    for (std::uint32_t y = 0; y < kH / 2; ++y)
        for (std::uint32_t x = 0; x < kW; ++x)
            mask.coverage[static_cast<std::size_t>(y) * kW + x] = 0;

    const Image before = checkEditReaches(
        "layer mask", *doc, settle,
        [&]() -> core::Command* {
            return push(*doc, std::make_unique<core::SetLayerMaskCommand>(id, mask));
        },
        Reported::Whole);
    checkUndoRestores("layer mask", *doc, settle, before);
}

// ---------------------------------------------------------------------------------------------
// The frame's contract, on a document that has all of it at once
// ---------------------------------------------------------------------------------------------

TEST_CASE("the pre-composite settle reports its band EXACTLY ONCE") {
    // The invariant the 0.3.2 defect broke, asserted directly. `refreshTextCaches` /
    // `refreshTextureCaches` re-render a stale cache and report the band they changed; a SECOND
    // call in the same frame finds the caches current and reports nothing. That is correct and
    // deliberate -- and it is precisely why the first caller must not throw the report away.
    //
    // If this test ever fails by reporting a band twice, the app is doing redundant work. If a
    // future refactor makes the report idempotent instead, delete the [[nodiscard]] gymnastics in
    // ui/app_window.cpp along with this case -- but until then, this is the property the whole
    // region path is built on, and it deserves to be written down somewhere it is checked.
    Settle settle;
    if (!settle.haveFonts())
        return;

    auto doc = makeDoc();
    const core::LayerId id = addText(*doc, make3dBlock(settle.fonts.defaultFamily()));
    settle(*doc); // the initial render

    text::TextBlock next = doc->find(id)->as<core::TextLayer>()->block();
    next.extrude->depth = 44.0f;
    doc->commands().push(std::make_unique<core::SetTextCommand>(id, std::move(next)));

    const Rect first = settle(*doc);
    CHECK_FALSE(first.empty()); // the edit re-rendered: the band is named

    const Rect second = settle(*doc);
    CHECK(second.empty()); // ...and it is named ONCE. A caller that drops `first` has dropped it.
}
