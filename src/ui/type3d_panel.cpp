#include "ui/type3d_panel.hpp"

#include "ui/cursor_apply.hpp"
#include "ui/gizmo_canvas.hpp"
#include "ui/scrub_slider.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include "common/geometry3d.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

// The 3D popup (docs/type-tool.md §8.4, S30-d). The viewport widget renders the edited block
// through the host's renderTextF hook (the full pipeline, GPU lane included) and overlays the
// manipulation gizmos: three constrained rotation rings (model X/Y/Z, classic RGB), the depth
// handle riding the solid's +z axis, the bevel knob, and the light-direction sphere. Every drag
// streams coalesced block edits through the same funnel as the sliders, so the CANVAS updates
// live and undo captures one step per gesture.
namespace mosaic::ui {
namespace {

namespace txt = core::text;
using common::Quat;
using common::Vec2;
using common::Vec3;

constexpr int kPanelW = 300;
constexpr int kPad = 12;
constexpr int kScrollW = 15;  // reserved right gutter for the ScrollView's vertical scrollbar
constexpr int kRowH = 24;
constexpr int kRowGap = 7;
constexpr int kLabelW = 92;
constexpr int kInnerW = kPanelW - 2 * kPad - kScrollW;
constexpr int kFieldLeft = kPad + kLabelW;
constexpr int kFieldW = kPanelW - kPad - kScrollW - kFieldLeft;
constexpr int kViewH = 190;
constexpr double kPi = 3.14159265358979323846;

// No Lighting toggle: unlit was a fullbright silhouette that muted every material control -- no
// genuine trade-off, so no checkbox ([[mosaic-no-toggle-for-strictly-better]], user 2026-07-03).
// The model field stays (tests exercise the flat lane); the panel always keeps lighting on.
enum class Role {
    Enable, Depth, BevelSize, BevelProfile, Perspective, ReflectCanvas, ReflectSides, OverlayWrap,
    Preset, Metalness, Roughness, LightPower,
};

const char* coalesceId(Role r) {
    switch (r) {
    case Role::Enable: return "extrude:on";
    case Role::Depth: return "extrude:depth";
    case Role::BevelSize: return "extrude:bevelSize";
    case Role::BevelProfile: return "extrude:bevelProfile";
    case Role::Perspective: return "extrude:perspective";
    case Role::ReflectCanvas: return "extrude:reflect";
    case Role::ReflectSides: return "extrude:reflectSides";
    case Role::OverlayWrap: return "extrude:overlayWrap";
    case Role::Preset: return "extrude:preset";
    case Role::Metalness: return "extrude:metalness";
    case Role::Roughness: return "extrude:roughness";
    case Role::LightPower: return "extrude:lightPower";
    }
    return "";
}

// Material presets (round 3, "should we add any other options?"): one pick sets the whole finish.
// The metals SET the colour too -- a metal's reflectance IS its colour (gold is gold); the
// dielectric finishes (Plastic / Matte) keep whatever colour is chosen, they are just surface
// qualities. The dropdown reads "Custom" whenever the sliders do not match a preset.
struct MaterialPreset {
    const char* name;
    float metal, rough;
    bool setsAlbedo;
    float r, g, b;
};
constexpr MaterialPreset kPresets[] = {
    {"Chrome", 1.0f, 0.04f, true, 0.95f, 0.96f, 0.97f},
    {"Gold", 1.0f, 0.15f, true, 1.00f, 0.77f, 0.34f},
    {"Copper", 1.0f, 0.18f, true, 0.95f, 0.54f, 0.43f},
    {"Steel", 1.0f, 0.35f, true, 0.77f, 0.78f, 0.80f},
    {"Plastic", 0.0f, 0.35f, false, 0.0f, 0.0f, 0.0f},
    {"Matte", 0.0f, 0.90f, false, 0.0f, 0.0f, 0.0f},
};

// The preset the material currently equals (dropdown index; 0 = "Custom").
int presetIndexFor(const txt::Extrude& e) {
    const auto& m = e.material;
    for (std::size_t i = 0; i < std::size(kPresets); ++i) {
        const MaterialPreset& p = kPresets[i];
        const auto close = [](float a, float b) { return std::abs(a - b) <= 0.005f; };
        if (!close(m.metalness, p.metal) || !close(m.roughness, p.rough)) continue;
        if (p.setsAlbedo &&
            (!close(m.albedo.r, p.r) || !close(m.albedo.g, p.g) || !close(m.albedo.b, p.b)))
            continue;
        return static_cast<int>(i) + 1;
    }
    return 0;
}

Fl_Color toFl(common::Color8 c) { return fl_rgb_color(c.r, c.g, c.b); }
common::Color8 to8(common::ColorF c) {
    const auto q = [](float v) {
        return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
    };
    return {q(c.r), q(c.g), q(c.b), 255};
}

struct Binding {
    Type3dPanel* self = nullptr;
    Role role = Role::Enable;
};
void onControlCb(Fl_Widget*, void* v) {
    auto* b = static_cast<Binding*>(v);
    b->self->applyControl(static_cast<int>(b->role));
}

// The software-AA gizmo canvas moved to ui/gizmo_canvas.hpp (S55-f: the Texture Generator's
// preview pane is its second consumer); the class and its rationale live there.

}  // namespace

// ---------------------------------------------------------------------------------------------
// The live viewport + gizmos
// ---------------------------------------------------------------------------------------------
class Extrude3dViewport : public Fl_Widget {
public:
    Extrude3dViewport(int X, int Y, int W, int H, Type3dPanel* owner)
        : Fl_Widget(X, Y, W, H), m_owner(owner) {
        copy_tooltip("Drag to orbit; rings rotate one axis; square = depth, diamond = bevel; "
                     "the sphere aims the light");
    }

    void setImage(common::Image img) {
        m_img = std::move(img);  // composed with the gizmos in draw() (GizmoCanvas)
        redraw();
    }

protected:
    void draw() override {
        const Palette& pal = activePalette();
        const auto& cur = m_owner->m_current;
        if (!m_owner->m_hasSession) {
            fl_rectf(x(), y(), w(), h(), toFl(pal.controlBg));
            drawHint("Edit a text object to sculpt it");
            return;
        }
        if (!cur) {
            fl_rectf(x(), y(), w(), h(), toFl(pal.controlBg));
            drawHint("Enable 3D extrusion below");
            return;
        }
        // Compose ground + preview + AA gizmos into one opaque image, then blit (round 2: the
        // fl_line gizmos were jagged; SDF-coverage strokes + hover highlight replace them).
        GizmoCanvas gc(w(), h(), pal.controlBg);
        if (!m_img.rgba.empty())
            gc.blitImage(m_img, (w() - static_cast<int>(m_img.width)) / 2,
                         (h() - static_cast<int>(m_img.height)) / 2);
        drawGizmos(gc, *cur);
        Fl_RGB_Image blit(gc.data(), w(), h(), 4);
        blit.draw(x(), y());
        fl_rect(x(), y(), w(), h(), toFl(pal.border));
    }

    int handle(int event) override {
        const auto& cur = m_owner->m_current;
        switch (event) {
        case FL_ENTER:
        case FL_MOVE: {
            // Live affordance: light up whatever a press HERE would grab, and shape the cursor
            // (hand = a handle/ring/lamp, move = free orbit). grabAt is the single truth for
            // both, so the highlight can never lie about the drag target.
            Grab over = Grab::None;
            if (m_owner->m_hasSession && cur) over = grabAt(eventPos(), *cur);
            if (over != m_hover) {
                m_hover = over;
                redraw();
            }
            if (window() != nullptr) {
                // ⚠ The free-orbit affordance is NOT the stock FL_CURSOR_MOVE on Wayland: FLTK
                // resolves that by the Xcursor name `move`, which breeze symlinks to `dnd-move` --
                // a closed grabbing hand that points ~10 px from where it clicks, and that would
                // read here as the SAME hand the ring/lamp handles show. ui::MoveCursor keeps the
                // orbit's four-way arrow distinct from the handles' hand (docs/wayland.md §2.5).
                if (over == Grab::None)
                    window()->cursor(FL_CURSOR_DEFAULT);
                else if (over == Grab::Trackball)
                    m_moveCursor.apply(window());
                else
                    window()->cursor(FL_CURSOR_HAND);
            }
            return 1;  // own belowmouse so the tooltip shows
        }
        case FL_LEAVE:
            if (m_hover != Grab::None) {
                m_hover = Grab::None;
                redraw();
            }
            if (window() != nullptr) window()->cursor(FL_CURSOR_DEFAULT);
            return 1;
        case FL_PUSH: {
            if (!m_owner->m_hasSession || !cur) return 1;
            m_last = eventPos();
            m_grab = grabAt(m_last, *cur);
            // The gesture's ACCUMULATOR: drag math composes on this local copy, event by event.
            // Composing on m_current instead silently dropped every event but the last between
            // frames (reflect() refreshes m_current only once per frame, so each event rebased on
            // the same stale value) -- the "drag miles to rotate" bug (user report 2026-07-03).
            m_drag = *cur;
            redraw();  // the grabbed element stays lit for the whole gesture
            return 1;
        }
        case FL_DRAG: {
            if (m_grab == Grab::None || !cur) return 1;
            const Vec2 p = eventPos();
            const Vec2 d = p - m_last;
            m_last = p;
            dragBy(d, p);
            return 1;
        }
        case FL_RELEASE:
            m_grab = Grab::None;
            m_hover = m_owner->m_hasSession && cur ? grabAt(eventPos(), *cur) : Grab::None;
            redraw();
            return 1;
        default:
            return Fl_Widget::handle(event);
        }
    }

private:
    enum class Grab { None, Trackball, RingX, RingY, RingZ, Depth, Bevel, Light };

    struct RingPt {
        Vec2 p;         // widget-local screen point
        double z = 0.0; // rotated depth (+ toward the viewer) -- back-half segments draw dimmed
    };

    [[nodiscard]] Vec2 eventPos() const {
        return {static_cast<double>(Fl::event_x()), static_cast<double>(Fl::event_y())};
    }
    [[nodiscard]] Vec2 center() const { return {x() + w() * 0.5, y() + h() * 0.5}; }
    [[nodiscard]] double gizmoR() const { return std::min(w(), h()) * 0.36; }
    [[nodiscard]] Vec2 lightCenter() const { return {x() + w() - 26.0, y() + h() - 26.0}; }
    static constexpr double kLightR = 17.0;

    void drawHint(const char* text) {
        const Palette& pal = activePalette();
        fl_color(toFl(pal.textMuted));
        fl_font(FL_HELVETICA, 12);
        fl_draw(text, x(), y(), w(), h(), FL_ALIGN_CENTER);
        fl_rect(x(), y(), w(), h(), toFl(pal.border));
    }

    // A model-axis ring's screen polyline (+ per-point depth) under the current orientation.
    [[nodiscard]] std::vector<RingPt> ringPoints(const Quat& q, int axis) const {
        std::vector<RingPt> pts;
        pts.reserve(49);
        const Vec2 c = center();
        const double R = gizmoR();
        for (int i = 0; i <= 48; ++i) {
            const double a = 2.0 * kPi * i / 48.0;
            Vec3 v;
            if (axis == 0) v = {0.0, std::cos(a), std::sin(a)};       // ring about X: YZ plane
            else if (axis == 1) v = {std::cos(a), 0.0, std::sin(a)};  // about Y: XZ plane
            else v = {std::cos(a), std::sin(a), 0.0};                 // about Z: XY plane
            const Vec3 r = q.rotate(v);
            pts.push_back({{c.x + r.x * R, c.y + r.y * R}, r.z});
        }
        return pts;
    }

    [[nodiscard]] Vec2 depthTip(const Quat& q) const {
        const Vec3 a = q.rotate({0.0, 0.0, 1.0});
        const Vec2 c = center();
        return {c.x + a.x * gizmoR() * 1.28, c.y + a.y * gizmoR() * 1.28};
    }
    [[nodiscard]] Vec2 bevelKnob(const Quat& q) const {
        const Vec3 a = q.rotate({0.0, -0.8, 0.55});  // the front face's top edge, roughly
        const Vec2 c = center();
        return {c.x + a.x * gizmoR(), c.y + a.y * gizmoR()};
    }

    void drawGizmos(GizmoCanvas& gc, const txt::Extrude& e) {
        const Palette& pal = activePalette();
        const Grab lit = m_grab != Grab::None ? m_grab : m_hover;  // gesture wins over hover
        const Vec2 off{static_cast<double>(x()), static_cast<double>(y())};  // widget -> canvas
        const common::Color8 axisCol[3] = {
            {235, 96, 96, 255}, {104, 208, 104, 255}, {108, 152, 246, 255}};
        const Grab ringGrab[3] = {Grab::RingX, Grab::RingY, Grab::RingZ};
        for (int axis = 0; axis < 3; ++axis) {
            const bool hot = lit == ringGrab[axis];
            const std::vector<RingPt> pts = ringPoints(e.orientation, axis);
            const double width = hot ? 2.6 : 1.6;
            for (std::size_t i = 1; i < pts.size(); ++i) {
                // The ring's far half recedes: draw it thin and faint so the near half (the part
                // you would actually grab) reads in front of the solid, depth and all.
                const bool back = pts[i - 1].z + pts[i].z < 0.0;
                gc.stroke(pts[i - 1].p - off, pts[i].p - off, back ? width * 0.75 : width,
                          axisCol[axis], back ? (hot ? 0.45f : 0.28f) : (hot ? 1.0f : 0.85f));
            }
        }
        // Depth handle: a stem from the centre along the solid's +z with a square knob.
        const bool depthHot = lit == Grab::Depth;
        const Vec2 c = center() - off;
        const Vec2 tip = depthTip(e.orientation) - off;
        gc.stroke(c, tip, depthHot ? 2.4 : 1.6, pal.accent, depthHot ? 1.0f : 0.85f);
        gc.fillSquare(tip, depthHot ? 6.0 : 4.5, pal.accent, 1.0f);
        // Bevel knob: a diamond riding the front-top edge.
        const bool bevelHot = lit == Grab::Bevel;
        gc.fillDiamond(bevelKnob(e.orientation) - off, bevelHot ? 7.5 : 6.0, pal.text, 1.0f);
        // Light sphere: the dot = where the lamp sits on the near hemisphere.
        if (e.lightingEnabled && !e.lights.empty()) {
            const bool lightHot = lit == Grab::Light;
            const Vec2 lc = lightCenter() - off;
            gc.strokeCircle(lc, kLightR, lightHot ? 2.2 : 1.4, pal.border,
                            lightHot ? 1.0f : 0.8f);
            const Vec3 lamp = (-e.lights[0].direction).normalized();
            gc.fillDisc({lc.x + lamp.x * kLightR, lc.y + lamp.y * kLightR},
                        lightHot ? 5.5 : 4.5, {255, 220, 90, 255}, 1.0f);
        }
    }

    [[nodiscard]] static double distToPolyline(Vec2 p, const std::vector<RingPt>& pts) {
        double best = 1e9;
        for (std::size_t i = 1; i < pts.size(); ++i) {
            const Vec2 a = pts[i - 1].p, b = pts[i].p;
            const Vec2 ab = b - a;
            const double len2 = ab.dot(ab);
            const double t = len2 > 0.0 ? std::clamp((p - a).dot(ab) / len2, 0.0, 1.0) : 0.0;
            const Vec2 q = a + ab * t;
            best = std::min(best, (p - q).length());
        }
        return best;
    }

    [[nodiscard]] Grab grabAt(Vec2 p, const txt::Extrude& e) const {
        if ((p - depthTip(e.orientation)).length() <= 9.0) return Grab::Depth;
        if ((p - bevelKnob(e.orientation)).length() <= 9.0) return Grab::Bevel;
        if (e.lightingEnabled && !e.lights.empty() &&
            (p - lightCenter()).length() <= kLightR + 4.0)
            return Grab::Light;
        const Grab rings[3] = {Grab::RingX, Grab::RingY, Grab::RingZ};
        double best = 6.0;
        Grab hit = Grab::Trackball;
        for (int axis = 0; axis < 3; ++axis) {
            const double d = distToPolyline(p, ringPoints(e.orientation, axis));
            if (d < best) {
                best = d;
                hit = rings[axis];
            }
        }
        return hit;
    }

    // All drag math reads AND writes m_drag (the press-time copy): each event composes on the
    // previous event's value, decoupled from the per-frame reflect() round-trip.
    void dragBy(Vec2 d, Vec2 p) {
        switch (m_grab) {
        case Grab::Trackball: {
            const double len = d.length();
            if (len <= 0.0) return;
            const Vec3 axis = Vec3{d.y, d.x, 0.0}.normalized();
            m_drag.orientation =
                (Quat::fromAxisAngle(axis, len * 0.011) * m_drag.orientation).normalized();
            const Quat q = m_drag.orientation;
            m_owner->commitExtrude("extrude:orientation",
                                   [q](txt::Extrude& ex) { ex.orientation = q; });
            break;
        }
        case Grab::RingX:
        case Grab::RingY:
        case Grab::RingZ: {
            const int axis = m_grab == Grab::RingX ? 0 : m_grab == Grab::RingY ? 1 : 2;
            const Vec2 c = center();
            const Vec2 prev = p - d;
            const double a0 = std::atan2(prev.y - c.y, prev.x - c.x);
            const double a1 = std::atan2(p.y - c.y, p.x - c.x);
            double da = a1 - a0;
            while (da > kPi) da -= 2.0 * kPi;
            while (da < -kPi) da += 2.0 * kPi;
            const Vec3 model = axis == 0 ? Vec3{1, 0, 0} : axis == 1 ? Vec3{0, 1, 0} : Vec3{0, 0, 1};
            const Vec3 world = m_drag.orientation.rotate(model);
            // The ring reads as a dial about its world axis: pointer angle maps directly when the
            // axis faces the viewer, mirrored when it faces away.
            const double sign = world.z >= 0.0 ? 1.0 : -1.0;
            m_drag.orientation =
                (Quat::fromAxisAngle(world, da * sign) * m_drag.orientation).normalized();
            const Quat q = m_drag.orientation;
            m_owner->commitExtrude("extrude:orientation",
                                   [q](txt::Extrude& ex) { ex.orientation = q; });
            break;
        }
        case Grab::Depth: {
            const Vec3 a = m_drag.orientation.rotate({0.0, 0.0, 1.0});
            Vec2 dir{a.x, a.y};
            const double l = dir.length();
            if (l < 0.05) return;  // the axis points at the viewer: no screen direction to ride
            dir = dir * (1.0 / l);
            // Gain rides the viewport's fit scale (design px per screen px): the solid then grows
            // roughly WITH the pointer instead of a fixed crawl on a zoomed-out headline.
            const double gain =
                std::clamp(1.0 / std::max(m_owner->m_viewScale, 1e-3), 0.8, 6.0);
            m_drag.depth = std::clamp(
                m_drag.depth + static_cast<float>(d.dot(dir) * gain), 0.5f, 500.0f);
            const float depth = m_drag.depth;
            m_owner->commitExtrude("extrude:depth",
                                   [depth](txt::Extrude& ex) { ex.depth = depth; });
            break;
        }
        case Grab::Bevel: {
            // A fine control on purpose: the useful range is a handful of px (the mesh saturates
            // it at the local stroke width, so overshoot is harmless -- the slider does coarse).
            m_drag.bevelFront.size = std::clamp(
                m_drag.bevelFront.size - static_cast<float>(d.y) * 0.1f, 0.0f, 100.0f);
            const float size = m_drag.bevelFront.size;
            m_owner->commitExtrude("extrude:bevelSize",
                                   [size](txt::Extrude& ex) { ex.bevelFront.size = size; });
            break;
        }
        case Grab::Light: {
            const Vec2 lc = lightCenter();
            const double nx = std::clamp((p.x - lc.x) / kLightR, -0.95, 0.95);
            const double ny = std::clamp((p.y - lc.y) / kLightR, -0.95, 0.95);
            const double planar = nx * nx + ny * ny;
            // The lamp rides the sphere's near hemisphere; it always shines INTO the scene (-z).
            const Vec3 lamp = Vec3{nx, ny, std::sqrt(std::max(0.1, 1.0 - planar))}.normalized();
            const Vec3 dir = -lamp;  // absolute (from pointer position): no accumulation needed
            m_owner->commitExtrude("extrude:light",
                                   [dir](txt::Extrude& ex) {
                                if (ex.lights.empty()) ex.lights.push_back(txt::kDefaultKeyLight);
                                ex.lights[0].direction = dir;
                            });
            break;
        }
        case Grab::None:
            break;
        }
    }

    Type3dPanel* m_owner;
    common::Image m_img;
    Grab m_grab = Grab::None;
    Grab m_hover = Grab::None;  // what a press would grab right now (drawn highlighted)
    Vec2 m_last{};
    txt::Extrude m_drag;  // the active gesture's accumulator (copied from m_current at FL_PUSH)
    MoveCursor m_moveCursor;  // the orbit affordance; Wayland substitution + its rasterized cache
};

// ---------------------------------------------------------------------------------------------
// The panel
// ---------------------------------------------------------------------------------------------
struct Type3dPanel::State {
    std::vector<std::unique_ptr<Binding>> bindings;
    ScrollView* scroll = nullptr;
    Extrude3dViewport* viewport = nullptr;
    CheckBox* enable = nullptr;
    CheckBox* reflect = nullptr;
    CheckBox* reflectSides = nullptr;
    CheckBox* overlayWrap = nullptr;
    Dropdown* profile = nullptr;
    Dropdown* preset = nullptr;
    SwatchChip* albedoChip = nullptr;
    ScrubSlider* depth = nullptr;
    ScrubSlider* bevel = nullptr;
    ScrubSlider* perspective = nullptr;
    ScrubSlider* metalness = nullptr;
    ScrubSlider* roughness = nullptr;
    ScrubSlider* lightPower = nullptr;
    std::vector<Fl_Widget*> needsExtrude;  // greyed while 3D is off / no session
};

Type3dPanel::Type3dPanel() : Popover(kPanelW, 555), m_state(std::make_unique<State>()) {
    setPinned(true);  // like the Type panel: survives canvas clicks, closes via its button/tool switch
}
Type3dPanel::~Type3dPanel() = default;

void Type3dPanel::setScrubRuler(ScrubRuler* ruler) {
    m_scrubRuler = ruler;
    if (m_state->viewport != nullptr) build();
}

void Type3dPanel::setPlacementProviders(std::function<common::Rect()> region,
                                        std::function<std::optional<common::Rect>()> avoid) {
    m_region = region;  // resizeToContent clamps the footprint's height to the same region
    setCornerPlacement(Corner::BottomRight, std::move(region), std::move(avoid));
}

void Type3dPanel::reapplyTheme() {
    Popover::reapplyTheme();
    build();
}

// The single write path every control and gizmo funnels through: read-modify-write the block's
// optional Extrude (guarded: the controls are greyed when 3D is off, so a missing value means a
// stale event -- dropped). `overrideId` lets the gizmos coalesce under their own ids.
void Type3dPanel::applyControl(int roleInt) {
    if (m_reflecting) return;
    const Role role = static_cast<Role>(roleInt);
    switch (role) {
    case Role::Enable: {
        const bool on = m_state->enable != nullptr && m_state->enable->checked();
        if (m_onBlockEdit)
            m_onBlockEdit(coalesceId(role), [on](txt::TextBlock& b) {
                if (on && !b.extrude) b.extrude = txt::Extrude{};
                if (!on) b.extrude.reset();
            });
        break;
    }
    case Role::Depth:
        commitExtrude(coalesceId(role), [v = static_cast<float>(m_state->depth->value())](
                                            txt::Extrude& e) { e.depth = v; });
        break;
    case Role::BevelSize:
        commitExtrude(coalesceId(role), [v = static_cast<float>(m_state->bevel->value())](
                                            txt::Extrude& e) { e.bevelFront.size = v; });
        break;
    case Role::BevelProfile:
        commitExtrude(coalesceId(role), [v = m_state->profile->value()](txt::Extrude& e) {
            e.bevelFront.profile = static_cast<txt::Bevel::Profile>(std::max(0, v));
        });
        break;
    case Role::Perspective:
        commitExtrude(coalesceId(role), [v = static_cast<float>(m_state->perspective->value())](
                                            txt::Extrude& e) { e.perspective = v; });
        break;
    case Role::ReflectCanvas:
        commitExtrude(coalesceId(role), [on = m_state->reflect->checked()](txt::Extrude& e) {
            e.reflectCanvas = on;
        });
        break;
    case Role::ReflectSides:
        commitExtrude(coalesceId(role), [on = m_state->reflectSides->checked()](txt::Extrude& e) {
            e.reflectSidesOnly = on;
        });
        break;
    case Role::OverlayWrap:
        commitExtrude(coalesceId(role), [on = m_state->overlayWrap->checked()](txt::Extrude& e) {
            e.overlayWrapSides = on;
        });
        break;
    case Role::Preset: {
        const int v = m_state->preset->value();
        if (v <= 0) break;  // "Custom" is a readout, not an action
        const MaterialPreset p = kPresets[static_cast<std::size_t>(v - 1)];
        commitExtrude(coalesceId(role), [p](txt::Extrude& e) {
            e.material.metalness = p.metal;
            e.material.roughness = p.rough;
            if (p.setsAlbedo) e.material.albedo = {p.r, p.g, p.b, 1.0f};
        });
        break;
    }
    case Role::LightPower:
        commitExtrude(coalesceId(role),
                      [v = static_cast<float>(m_state->lightPower->value())](txt::Extrude& e) {
                          if (e.lights.empty()) e.lights.push_back(txt::kDefaultKeyLight);
                          e.lights[0].intensity = v;
                      });
        break;
    case Role::Metalness:
        commitExtrude(coalesceId(role), [v = static_cast<float>(m_state->metalness->value())](
                                            txt::Extrude& e) { e.material.metalness = v; });
        break;
    case Role::Roughness:
        commitExtrude(coalesceId(role), [v = static_cast<float>(m_state->roughness->value())](
                                            txt::Extrude& e) { e.material.roughness = v; });
        break;
    }
}

void Type3dPanel::build() {
    const Palette& pal = activePalette();
    // Normalize the window to its base footprint BEFORE laying out children: a HIDDEN popover is
    // group-stretched by every main-window resize (nothing re-pins it until it is shown), and
    // building design-coordinate children inside that stretched box bakes an inconsistent resize
    // baseline -- the show-time restore to the base size then proportionally SHRINKS the fresh
    // controls (user 2026-07-16: "3d shrinks them to about a quarter of the panel's size").
    resizable(nullptr);
    Fl_Double_Window::resize(x(), y(), m_baseW, m_baseH);
    clear();             // ⚠ resets resizable() to the group and re-baselines the resize math...
    resizable(nullptr);  // ...so pin it null again until the scroll exists below
    *m_state = State{};
    begin();
    // The Type panel's scroll pattern: content in a ScrollView so a short window scrolls the
    // panel instead of overflowing the canvas (S30-e feedback: "does not support scrolling").
    // VERTICAL_ALWAYS keeps the scrollbar gutter occupied so the row layout never jumps.
    auto* sv = new ScrollView(1, 1, kPanelW - 2, h() - 2);
    sv->type(Fl_Scroll::VERTICAL_ALWAYS);
    sv->box(FL_FLAT_BOX);
    sv->color(toFl(pal.panelBg));
    sv->begin();
    auto* content = new Fl_Group(1, 1, kPanelW - 2 - kScrollW, 4000);
    // An Fl_Group's default resizable() is itself: the size() that trims the provisional 4000px
    // to the real content height would proportionally scale every row -- pin it null.
    content->resizable(nullptr);
    content->begin();
    m_state->scroll = sv;

    int cy = kPad;
    const auto bind = [&](Role role) {
        auto b = std::make_unique<Binding>();
        b->self = this;
        b->role = role;
        Binding* raw = b.get();
        m_state->bindings.push_back(std::move(b));
        return raw;
    };
    const auto caption = [&](const char* text, const char* tip) {
        auto* box = new Fl_Box(kPad, cy, kLabelW, kRowH);
        box->copy_label(text);
        box->box(FL_NO_BOX);
        box->labelfont(FL_HELVETICA);
        box->labelsize(12);
        box->labelcolor(toFl(pal.textMuted));
        box->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        if (tip != nullptr) box->copy_tooltip(tip);
        m_state->needsExtrude.push_back(box);
        return box;
    };
    const auto sliderRow = [&](const char* label, Role role, double min, double max, double step,
                               const char* suffix, const char* tip) {
        caption(label, tip);
        auto* s = new ScrubSlider(kFieldLeft, cy, kFieldW, kRowH);
        s->range(min, max);
        s->step(step);
        s->setSuffix(suffix);
        s->setCellColor(pal.panelBg);
        s->setRuler(m_scrubRuler);
        s->when(FL_WHEN_CHANGED);
        s->callback(onControlCb, bind(role));
        if (tip != nullptr) s->copy_tooltip(tip);
        m_state->needsExtrude.push_back(s);
        cy += kRowH + kRowGap;
        return s;
    };

    // Enable toggle (the one control alive without an Extrude).
    m_state->enable = new CheckBox(kPad, cy, kInnerW, kRowH, "3D extrusion", [this](bool) {
        if (!m_reflecting) applyControl(static_cast<int>(Role::Enable));
    });
    m_state->enable->setGroundColor(pal.panelBg);
    m_state->enable->copy_tooltip("Extrude the text into a solid (docs: one depth per object)");
    cy += kRowH + kRowGap;

    // The live viewport.
    m_state->viewport = new Extrude3dViewport(kPad, cy, kInnerW, kViewH, this);
    cy += kViewH + kRowGap;

    m_state->depth = sliderRow("Depth", Role::Depth, 0.5, 500, 0.5, "px",
                               "Extrusion depth (the solid's thickness)");
    m_state->bevel = sliderRow("Bevel", Role::BevelSize, 0, 100, 0.5, "px",
                               "Front bevel size (0 = sharp edge)");
    {
        caption("Profile", "Front bevel profile");
        auto* d = new Dropdown(kFieldLeft, cy, kFieldW, kRowH);
        d->add("Flat");
        d->add("Round");
        d->add("Convex");
        d->add("Concave");
        d->callback(onControlCb, bind(Role::BevelProfile));
        d->copy_tooltip("Front bevel profile");
        m_state->profile = d;
        m_state->needsExtrude.push_back(d);
        cy += kRowH + kRowGap;
    }
    m_state->perspective = sliderRow("Perspective", Role::Perspective, 0, 120, 0.5, "\xC2\xB0",
                                     "Camera field of view: 0 = flat orthographic, higher = drama");

    // Reflect canvas + its Sides-only refinement share the row (the second gates on the first).
    m_state->reflect =
        new CheckBox(kPad, cy, kInnerW / 2 - 4, kRowH, "Reflect canvas", [this](bool) {
            if (!m_reflecting) applyControl(static_cast<int>(Role::ReflectCanvas));
        });
    m_state->reflect->setGroundColor(pal.panelBg);
    m_state->reflect->copy_tooltip(
        "Metallic reflections mirror the canvas content behind the solid");
    m_state->needsExtrude.push_back(m_state->reflect);
    m_state->reflectSides = new CheckBox(kPad + kInnerW / 2 + 4, cy, kInnerW / 2 - 4, kRowH,
                                         "Sides only", [this](bool) {
                                             if (!m_reflecting)
                                                 applyControl(static_cast<int>(Role::ReflectSides));
                                         });
    m_state->reflectSides->setGroundColor(pal.panelBg);
    m_state->reflectSides->copy_tooltip(
        "Only the extruded sides (walls and bevels) mirror the canvas; "
        "the face keeps the studio finish");
    cy += kRowH + kRowGap;

    // §12 (S30-e): a Layer-Effects overlay paints the FRONT face by default; this wraps it onto
    // the extruded sides too.
    m_state->overlayWrap =
        new CheckBox(kPad, cy, kInnerW, kRowH, "Wrap effects onto sides", [this](bool) {
            if (!m_reflecting) applyControl(static_cast<int>(Role::OverlayWrap));
        });
    m_state->overlayWrap->setGroundColor(pal.panelBg);
    m_state->overlayWrap->copy_tooltip(
        "A Layer-Effects colour/gradient/pattern overlay paints the front face; "
        "check to wrap it around the whole solid (walls, bevels and the back)");
    m_state->needsExtrude.push_back(m_state->overlayWrap);
    cy += kRowH + kRowGap;

    {
        caption("Preset", "Material presets: metals set their own colour, Plastic/Matte keep yours");
        auto* d = new Dropdown(kFieldLeft, cy, kFieldW, kRowH);
        d->add("Custom");
        for (const MaterialPreset& p : kPresets) d->add(p.name);
        d->callback(onControlCb, bind(Role::Preset));
        d->copy_tooltip("Material presets: metals set their own colour, Plastic/Matte keep yours");
        m_state->preset = d;
        m_state->needsExtrude.push_back(d);
        cy += kRowH + kRowGap;
    }
    m_state->metalness = sliderRow("Metalness", Role::Metalness, 0, 1, 0.01, "",
                                   "0 = dielectric (plastic), 1 = metal");
    m_state->roughness = sliderRow("Roughness", Role::Roughness, 0, 1, 0.01, "",
                                   "0 = polished (tight highlight), 1 = matte");
    m_state->lightPower = sliderRow("Intensity", Role::LightPower, 0, 3, 0.05, "",
                                    "Key light strength (the lamp on the little sphere)");

    {
        caption("Material", "The solid's colour");
        auto* chip = new SwatchChip(kFieldLeft, cy, kFieldW, kRowH);
        chip->setGroundColor(pal.panelBg);
        chip->setInteractive(true);
        chip->copy_tooltip("Material colour \xE2\x80\x94 click to edit");
        chip->setOnClick([this, chip] {
            if (m_onEditColor && m_hasSession && m_current)
                m_onEditColor(chip, m_current->material.albedo);
        });
        m_state->albedoChip = chip;
        m_state->needsExtrude.push_back(chip);
        cy += kRowH + kRowGap;
    }

    m_contentH = cy + kPad - kRowGap;
    content->size(content->w(), m_contentH);
    content->end();
    sv->end();
    end();
    resizable(sv);  // height changes stretch the scroll; the rows never scale
    resizeToContent();
    redraw();
}

// Set the footprint to fit the content, clamped to the region (the canvas) so the panel never
// exceeds it -- the ScrollView takes any overflow. Reanchors when already shown.
void Type3dPanel::resizeToContent() {
    int availH = 600;
    if (m_region) {
        const common::Rect reg = m_region();
        if (!reg.empty())
            availH = static_cast<int>(std::lround(reg.h)) - 24;
    }
    const int H = std::clamp(m_contentH + 2, 160, std::max(160, availH));
    setBaseSize(kPanelW, H);
    if (shown())
        reanchor();
}

void Type3dPanel::commitExtrude(const char* id, std::function<void(txt::Extrude&)> mutate) {
    if (m_reflecting || !m_onBlockEdit) return;
    m_onBlockEdit(id, [mutate = std::move(mutate)](txt::TextBlock& b) {
        if (!b.extrude) return;  // 3D turned off mid-gesture: a stale event, dropped
        mutate(*b.extrude);
    });
}

void Type3dPanel::reflect(const std::optional<txt::Extrude>& ex, bool hasSession) {
    if (m_state->viewport == nullptr) return;
    m_reflecting = true;
    m_current = ex;
    m_hasSession = hasSession;

    const txt::Extrude e = ex.value_or(txt::Extrude{});
    m_state->enable->setChecked(ex.has_value());
    if (hasSession)
        m_state->enable->activate();
    else
        m_state->enable->deactivate();
    m_state->depth->value(e.depth);
    m_state->bevel->value(e.bevelFront.size);
    m_state->profile->value(static_cast<int>(e.bevelFront.profile));
    m_state->perspective->value(e.perspective);
    m_state->reflect->setChecked(e.reflectCanvas);
    m_state->reflectSides->setChecked(e.reflectSidesOnly);
    m_state->overlayWrap->setChecked(e.overlayWrapSides);
    m_state->preset->value(presetIndexFor(e));
    m_state->metalness->value(e.material.metalness);
    m_state->roughness->value(e.material.roughness);
    m_state->lightPower->value(e.lights.empty() ? txt::kDefaultKeyLight.intensity
                                                : e.lights[0].intensity);
    if (m_state->albedoChip != nullptr)
        m_state->albedoChip->setColour(to8(e.material.albedo));
    const bool active = hasSession && ex.has_value();
    for (Fl_Widget* w : m_state->needsExtrude) {
        if (active)
            w->activate();
        else
            w->deactivate();
    }
    // "Sides only" refines Reflect canvas, so it additionally gates on it (not in needsExtrude).
    if (active && e.reflectCanvas)
        m_state->reflectSides->activate();
    else
        m_state->reflectSides->deactivate();
    for (ScrubSlider* s :
         {m_state->depth, m_state->bevel, m_state->perspective, m_state->metalness,
          m_state->roughness, m_state->lightPower})
        s->redraw();

    // Re-render the viewport through the host pipeline (GPU lane and all).
    if (active && m_renderPreview) {
        m_state->viewport->setImage(m_renderPreview(e, m_state->viewport->w() - 8,
                                                    m_state->viewport->h() - 8, &m_viewScale));
    } else {
        m_state->viewport->setImage({});
    }
    m_reflecting = false;
}

void Type3dPanel::toggle(const Fl_Widget* anchor) {
    if (m_state->viewport == nullptr) build();
    if (shownFor(anchor)) {
        hide();
        return;
    }
    resizeToContent();  // re-clamp to the current canvas height before placing
    showAnchored(anchor);
}

}  // namespace mosaic::ui
