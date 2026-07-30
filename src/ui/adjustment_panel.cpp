#include "ui/adjustment_panel.hpp"

#include "common/i18n.hpp"
#include "ui/curve_editor.hpp"    // the S34 Curves row edits a core::brush::Curve directly
#include "ui/gradient_flyout.hpp" // the S34-a Gradient Map row opens the host's shared bubble
#include "ui/paint_chip.hpp"      // ... on this preview chip (the Fill dialog's control)
#include "ui/tone_wheel.hpp"
#include "ui/scrub_slider.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <FL/Enumerations.H>
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Image_Surface.H>
#include <FL/Fl_Menu_Item.H> // FL_MENU_DIVIDER (grouped Choice dropdowns)
#include <FL/Fl_RGB_Image.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace mosaic::ui {

namespace {

constexpr int kPanelW = 380;
constexpr int kPadX = 14;
constexpr int kHeaderH = 40;
constexpr int kRowH = 26;
constexpr int kRowGap = 6;
constexpr int kFieldW = 160; // control column; the label takes the rest of the row
constexpr int kFooterH = 40; // the Reset row
constexpr int kWheelD = 96;  // Color Balance tone-band wheel diameter
constexpr int kWheelCapH = 16; // the Shadows/Midtones/Highlights caption under each wheel
constexpr int kHistoH = 56 + 9;  // Levels/Threshold histogram body + its marker lane
constexpr int kRampH = 14 + 9;   // Levels output ramp + its marker lane
constexpr int kCurveH = 190;     // the Curves plot (S34); the channel picker rides a normal row

Fl_Color toFl(common::Color8 c) {
    return fl_rgb_color(c.r, c.g, c.b);
}

// A Choice descriptor may reorder/group its dropdown for display (desc.choiceOrder) while the
// bag keeps storing the option INDEX. These map between the two: a dropdown POSITION (the add()
// order the widget's value() returns) and the STORED value (the enum index the bag holds). With
// no choiceOrder the two are identical, so every other Choice descriptor is unaffected.
int choiceStoredValue(const core::AdjustmentParamDesc& d, int pos) {
    if (d.choiceOrder == nullptr)
        return pos;
    if (pos < 0 || pos >= d.choiceCount)
        return 0;
    return d.choiceOrder[pos];
}
int choiceDisplayPos(const core::AdjustmentParamDesc& d, int stored) {
    if (d.choiceOrder == nullptr)
        return stored;
    for (int i = 0; i < d.choiceCount; ++i)
        if (d.choiceOrder[i] == stored)
            return i;
    return 0;
}

// A control's binding: the closure to run when it fires (FLTK callbacks are C thunks + a void*).
struct Binding {
    std::function<void()> fn;
};
void controlThunk(Fl_Widget*, void* b) {
    if (auto* bb = static_cast<Binding*>(b))
        bb->fn();
}

// The Levels/Threshold strip (S32 pro controls): a histogram of the adjustment's BACKDROP (or a
// black-to-white ramp for the output row) with draggable triangle markers beneath it. The panel
// owns the marker semantics (which param each index writes, ordering constraints, the gamma
// marker's log placement); this widget only reports "marker i moved to t in [0,1]".
class HistoStrip : public Fl_Widget {
public:
    enum class Body : std::uint8_t { Histogram, Ramp };
    static constexpr int kMarkerLane = 9; // px below the body reserved for the triangles

    HistoStrip(int X, int Y, int W, int H, Body body) : Fl_Widget(X, Y, W, H), m_body(body) {}

    void setGroundColor(common::Color8 c) { m_ground = c; }
    void setHistogram(std::vector<float> bins) { // 256 values, 0..1 (sqrt-normalized by caller)
        m_bins = std::move(bins);
        redraw();
    }
    void setMarkers(std::vector<double> pos01) {
        m_markers = std::move(pos01);
        redraw();
    }
    [[nodiscard]] const std::vector<double>& markers() const noexcept { return m_markers; }
    void setOnMarker(std::function<void(int idx, double pos01)> cb) {
        m_onMarker = std::move(cb);
    }

protected:
    void draw() override {
        const Palette& pal = activePalette();
        fl_color(toFl(m_ground)); // erase the whole box first ([[mosaic-ui-gotchas]])
        fl_rectf(x(), y(), w(), h());
        const int bx = x();
        const int bw = w();
        const int bh = h() - kMarkerLane;
        if (m_body == Body::Histogram) {
            fl_color(toFl(pal.controlBg));
            fl_rectf(bx, y(), bw, bh);
            fl_color(toFl(pal.textMuted));
            if (!m_bins.empty()) {
                for (int px = 0; px < bw - 2; ++px) {
                    const std::size_t bin =
                        std::min<std::size_t>(m_bins.size() - 1,
                                              static_cast<std::size_t>(px) * m_bins.size() /
                                                  std::max(1, bw - 2));
                    const int bar = static_cast<int>(std::lround(m_bins[bin] * (bh - 4)));
                    if (bar > 0)
                        fl_yxline(bx + 1 + px, y() + bh - 2, y() + bh - 2 - bar);
                }
            }
            fl_color(toFl(pal.border));
            fl_rect(bx, y(), bw, bh);
        } else { // Ramp: the output row -- black to white, so the markers read as levels
            for (int px = 0; px < bw - 2; ++px) {
                const int v = 255 * px / std::max(1, bw - 3);
                fl_color(fl_rgb_color(static_cast<std::uint8_t>(v)));
                fl_yxline(bx + 1 + px, y() + 1, y() + bh - 2);
            }
            fl_color(toFl(pal.border));
            fl_rect(bx, y(), bw, bh);
        }
        // Markers: triangles pointing up into the body, filled by their own position (dark at
        // the left, light at the right -- the Levels black/gamma/white handle language).
        for (const double m : m_markers) {
            const int mx = bx + 1 + static_cast<int>(std::lround(m * (bw - 3)));
            const int top = y() + bh;
            const auto shade = static_cast<std::uint8_t>(std::lround(40 + m * 175));
            fl_color(fl_rgb_color(shade));
            fl_polygon(mx, top, mx - 4, top + kMarkerLane - 1, mx + 4, top + kMarkerLane - 1);
            fl_color(toFl(pal.text));
            fl_loop(mx, top, mx - 4, top + kMarkerLane - 1, mx + 4, top + kMarkerLane - 1);
        }
    }

    int handle(int event) override {
        // Claim the whole PUSH/DRAG/RELEASE gesture ([[mosaic-ui-gotchas]]).
        switch (event) {
            case FL_PUSH: {
                if (Fl::event_button() != FL_LEFT_MOUSE || m_markers.empty())
                    return 0;
                const double t = posAt(Fl::event_x());
                m_drag = 0;
                for (int i = 1; i < static_cast<int>(m_markers.size()); ++i)
                    if (std::abs(m_markers[static_cast<std::size_t>(i)] - t) <
                        std::abs(m_markers[static_cast<std::size_t>(m_drag)] - t))
                        m_drag = i;
                dragTo(t);
                return 1;
            }
            case FL_DRAG:
                if (m_drag >= 0)
                    dragTo(posAt(Fl::event_x()));
                return 1;
            case FL_RELEASE:
                m_drag = -1;
                return 1;
            default:
                return Fl_Widget::handle(event);
        }
    }

private:
    [[nodiscard]] double posAt(int ex) const {
        return std::clamp((ex - (x() + 1.0)) / std::max(1, w() - 3), 0.0, 1.0);
    }
    void dragTo(double t) {
        if (m_drag < 0 || m_drag >= static_cast<int>(m_markers.size()))
            return;
        m_markers[static_cast<std::size_t>(m_drag)] = t;
        redraw();
        if (m_onMarker)
            m_onMarker(m_drag, t);
    }

    Body m_body;
    common::Color8 m_ground{0, 0, 0, 255};
    std::vector<float> m_bins;
    std::vector<double> m_markers;
    int m_drag = -1;
    std::function<void(int, double)> m_onMarker;
};

// The backdrop's four 256-bin histograms, alpha-weighted: [0] the W3C luma the Levels/Threshold
// strips have always drawn, then [1..3] the raw R/G/B channels -- indexed by core::CurveChannel,
// so the Curves plot can show the distribution of the very channel its curve edits (S34-a). One
// pass over the image fills all four, which is what makes the per-channel version as cheap as the
// luma-only one it replaces. Each is sqrt-normalized against its OWN peak, so a dominant sky
// doesn't flatten every other bar into invisibility and one channel's clipping does not squash
// the others.
using HistogramSet = std::array<std::vector<float>, core::kCurveChannelCount>;

HistogramSet backdropHistograms(const common::Image& img) {
    HistogramSet bins;
    for (std::vector<float>& b : bins)
        b.assign(256, 0.0f);
    if (img.empty())
        return bins;
    for (std::size_t p = 0; p < img.rgba.size(); p += 4) {
        const double a = img.rgba[p + 3] / 255.0;
        if (a <= 0.0)
            continue;
        const double lum = 0.3 * img.rgba[p] + 0.59 * img.rgba[p + 1] + 0.11 * img.rgba[p + 2];
        bins[0][static_cast<std::size_t>(std::clamp(lum, 0.0, 255.0))] += static_cast<float>(a);
        for (std::size_t ch = 0; ch < 3; ++ch)
            bins[ch + 1][img.rgba[p + ch]] += static_cast<float>(a);
    }
    for (std::vector<float>& b : bins) {
        const float mx = *std::max_element(b.begin(), b.end());
        if (mx > 0.0f)
            for (float& v : b)
                v = std::sqrt(v / mx);
    }
    return bins;
}

// The Curves plot's histogram ink. The composite (luma) curve draws in the palette's muted text,
// a per-channel curve in a desaturated version of its own channel -- the bars sit at low coverage,
// so a tint is what says which distribution you are looking at, without a legend.
common::Color8 curveHistogramTint(int channel) {
    switch (static_cast<core::CurveChannel>(channel)) {
    case core::CurveChannel::Red:
        return {206, 92, 92, 255};
    case core::CurveChannel::Green:
        return {86, 176, 92, 255};
    case core::CurveChannel::Blue:
        return {92, 124, 214, 255};
    default:
        return activePalette().textMuted;
    }
}

// A schema-clamped 8-bit level read: the Photo Filter custom colour's three rows are stored in
// levels, and a hostile bag must not paint the swatch with a negative channel.
std::uint8_t levelByte(double v) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0, 255.0)));
}

} // namespace

struct AdjustmentPanel::State {
    // One generated row per schema entry, in schema order: the slider or checkbox, paired with
    // its descriptor so syncValues can push a drifted bag back into the right control.
    struct Row {
        const core::AdjustmentParamDesc* desc = nullptr;
        ScrubSlider* slider = nullptr; // Scalar rows
        CheckBox* check = nullptr;     // Toggle rows
        Dropdown* choice = nullptr;    // Choice rows (the bag stores the option index)
        Dial* dial = nullptr;          // Angle rows (a rotary knob over the same scalar)
        Fl_Box* dialReadout = nullptr; // the "45 deg" caption right of an Angle dial
        Fl_Box* caption = nullptr;     // the row's left-hand label (so the row can be hidden whole)
    };
    std::vector<Row> rows;
    // Color Balance's tone-band wheels (replace its nine scalar rows): each edits the
    // <prefix>_cr/_mg/_yb key triple through the plane mapping, preserving the mean.
    struct Wheel {
        ToneWheel* wheel = nullptr;
        const char* prefix = nullptr; // "shadows" / "midtones" / "highlights"
    };
    std::vector<Wheel> wheels;
    HistoStrip* histo = nullptr; // Levels/Threshold input strip (markers own the scalars)
    HistoStrip* ramp = nullptr;  // Levels output row
    // Curves (S34): the plot + its channel picker. `shownChannel` is which of the four curves
    // the plot currently holds (mirrors the bag's "channel" row); `curveGesture` bumps whenever
    // a NEW gesture starts, so one drag = one undo step but two drags do not merge.
    CurveEditor* curve = nullptr;
    Dropdown* curveChannel = nullptr;
    int shownChannel = 0;
    int curveGesture = 0;
    // Gradient Map (S34-a): the ramp preview chip. Its gradient is NOT a schema row -- like the
    // curve knots it lives in the same name->double bag as indexed stops -- so the chip reads and
    // writes it through core::adjustmentGradientMap / setAdjustmentGradientMap.
    PaintChip* gradientChip = nullptr;
    int gradientGesture = 0;
    // Photo Filter (S34-a): the effective filter colour, clickable only under the Custom preset.
    SwatchChip* filterSwatch = nullptr;
    // The backdrop's four histograms (luma, R, G, B), refreshed when the panel changes target --
    // the Levels/Threshold strips take [0] and the Curves plot takes the channel it is showing.
    // `curveHistoChannel` is which one the plot currently holds (-1 = none pushed yet).
    HistogramSet histograms;
    int curveHistoChannel = -1;
    std::vector<std::unique_ptr<Binding>> bindings;
    std::map<std::string, double> lastBag; // the values currently shown (drift detector)
    bool syncing = false;                  // guard: value-sets during sync must not fire edits
};

AdjustmentPanel::AdjustmentPanel() : Popover(kPanelW, 100), m_state(std::make_unique<State>()) {
    setPinned(true); // survives canvas/chrome clicks, like the Type panel; Esc still closes
    end();           // Popover's ctor leaves the group open; build() manages its own begin/end
}

AdjustmentPanel::~AdjustmentPanel() = default;

void AdjustmentPanel::setPlacementProviders(std::function<common::Rect()> region) {
    setCornerPlacement(Corner::BottomRight, std::move(region));
}

void AdjustmentPanel::build(core::AdjustmentKind kind) {
    const Palette& pal = activePalette();
    clear(); // drop the previous kind's rows (never reached from a row's own callback)
    resizable(nullptr); // clear() re-arms proportional scaling; the layout is fixed (the LE rule)
    m_state->rows.clear();
    m_state->wheels.clear();
    m_state->bindings.clear();
    m_state->lastBag.clear();
    // clear() just FREED these child widgets; drop the raw pointers so a kind WITHOUT a histogram
    // (Exposure, a blur, ...) leaves them null, not dangling. Only the Levels/Threshold layouts
    // below re-set them. reflect()'s histogram refresh guards on histo != nullptr alone (no kind
    // check), so a stale non-null pointer here is a use-after-free the next time it reflects.
    m_state->histo = nullptr;
    m_state->ramp = nullptr;
    m_state->curve = nullptr;
    m_state->curveChannel = nullptr;
    m_state->gradientChip = nullptr;
    m_state->filterSwatch = nullptr;
    m_state->curveHistoChannel = -1; // the plot is gone; the next sync re-pushes its histogram
    // A ramp bubble anchored to the chip we are about to free would be pointing at freed memory
    // (and showing the previous kind's gradient over the new kind's rows).
    if (m_gradientFlyout != nullptr && m_gradientFlyout->shown())
        m_gradientFlyout->hide();

    // The pro layouts (user 2026-07-17: "we can do better than 9 sliders"): Color Balance takes
    // three tone-band wheels + the luminosity toggle; Levels takes the histogram with
    // black/gamma/white handles + the output ramp; Threshold takes the histogram with one level
    // handle; Curves takes the curve plot + its own channel picker. Their schema rows are owned
    // by those controls; everything else stays generic.
    const bool wheelLayout = kind == core::AdjustmentKind::ColorBalance;
    const bool levelsLayout = kind == core::AdjustmentKind::Levels;
    const bool thresholdLayout = kind == core::AdjustmentKind::Threshold;
    const bool curvesLayout = kind == core::AdjustmentKind::Curves;
    const bool gradientLayout = kind == core::AdjustmentKind::GradientMap;
    const bool photoFilterLayout = kind == core::AdjustmentKind::PhotoFilter;
    const bool customScalars = wheelLayout || levelsLayout || thresholdLayout;
    // Curves owns its ONE row (a Choice, not a Scalar) as well as the plot, so the generic-row
    // filter is a predicate rather than the plain "custom layouts own the scalars" test. Photo
    // Filter owns exactly its three color_* rows: they stay schema-declared (clamping, seeding
    // and Reset all come free) but read as ONE swatch instead of three sliders.
    const auto ownedByCustom = [&](const core::AdjustmentParamDesc& d) {
        if (curvesLayout)
            return true;
        if (photoFilterLayout && std::strncmp(d.key, "color_", 6) == 0)
            return true;
        return customScalars && d.type == core::AdjustmentParamType::Scalar;
    };
    const auto schema = core::adjustmentParamSchema(kind);
    int genericRows = 0;
    for (const core::AdjustmentParamDesc& d : schema)
        if (!ownedByCustom(d))
            ++genericRows;
    int contentH = kHeaderH + genericRows * (kRowH + kRowGap) + kFooterH;
    if (wheelLayout)
        contentH += kWheelD + kWheelCapH + kRowGap;
    if (levelsLayout)
        contentH += kHistoH + kRowGap + kRampH + kRowGap;
    if (thresholdLayout)
        contentH += kHistoH + kRowGap;
    if (curvesLayout)
        contentH += kRowH + kRowGap + kCurveH + kRowGap;
    if (gradientLayout || photoFilterLayout) // one extra row: the ramp chip / the filter swatch
        contentH += kRowH + kRowGap;
    setBaseSize(kPanelW, contentH);

    begin();

    // ---- Header: the kind's display name (the panel edits the ACTIVE layer, so no layer name) --
    auto* head = new Fl_Box(kPadX, 10, kPanelW - 2 * kPadX, 20);
    head->copy_label(std::string(core::adjustmentKindName(kind)).c_str());
    head->box(FL_NO_BOX);
    head->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    head->labelfont(FL_HELVETICA_BOLD);
    head->labelsize(13);
    head->labelcolor(toFl(pal.text));

    // ---- Schema-generated rows (label left, control right) ----
    const int fieldLeft = kPanelW - kPadX - kFieldW;
    int cy = kHeaderH;
    const auto bind = [&](std::function<void()> fn) {
        auto b = std::make_unique<Binding>();
        b->fn = std::move(fn);
        Binding* raw = b.get();
        m_state->bindings.push_back(std::move(b));
        return raw;
    };
    if (wheelLayout) {
        // Three wheels across the row, captions beneath; each writes its band's key triple
        // through the plane mapping with the mean preserved (a hand-authored equal shift on all
        // three sliders survives a wheel drag).
        static constexpr const char* kBands[3] = {"shadows", "midtones", "highlights"};
        static constexpr const char* kBandLabels[3] = {"Shadows", "Midtones", "Highlights"};
        const int cellW = (kPanelW - 2 * kPadX - 2 * kRowGap) / 3;
        for (int b = 0; b < 3; ++b) {
            const int bx = kPadX + b * (cellW + kRowGap);
            auto* wheel = new ToneWheel(bx + (cellW - kWheelD) / 2, cy, kWheelD, kWheelD);
            wheel->setGroundColor(pal.panelBg);
            wheel->setOnChange([this, prefix = kBands[b]](double px, double py) {
                if (m_state->syncing)
                    return;
                const std::string cr = std::string(prefix) + "_cr";
                const std::string mg = std::string(prefix) + "_mg";
                const std::string yb = std::string(prefix) + "_yb";
                const auto get = [&](const std::string& k) {
                    const auto it = m_state->lastBag.find(k);
                    return it == m_state->lastBag.end() ? 0.0 : it->second;
                };
                const double mean = (get(cr) + get(mg) + get(yb)) / 3.0;
                core::ColorBalanceTriple t =
                    core::colorBalanceFromPlane({px * 100.0, py * 100.0}, mean);
                t.cr = std::clamp(t.cr, -100.0, 100.0);
                t.mg = std::clamp(t.mg, -100.0, 100.0);
                t.yb = std::clamp(t.yb, -100.0, 100.0);
                m_state->lastBag[cr] = t.cr;
                m_state->lastBag[mg] = t.mg;
                m_state->lastBag[yb] = t.yb;
                if (m_onEdit)
                    m_onEdit(std::string("adjust:wheel_") + prefix,
                             [cr, mg, yb, t](std::map<std::string, double>& bag) {
                                 bag[cr] = t.cr;
                                 bag[mg] = t.mg;
                                 bag[yb] = t.yb;
                             });
            });
            m_state->wheels.push_back({wheel, kBands[b]});
            auto* cap = new Fl_Box(bx, cy + kWheelD, cellW, kWheelCapH, nullptr);
            cap->copy_label(_(kBandLabels[b]));
            cap->box(FL_NO_BOX);
            cap->align(FL_ALIGN_CENTER | FL_ALIGN_INSIDE);
            cap->labelfont(FL_HELVETICA);
            cap->labelsize(11);
            cap->labelcolor(toFl(pal.textMuted));
        }
        cy += kWheelD + kWheelCapH + kRowGap;
    }
    // A schema-defaulted read of the working bag, for the custom controls' cross-param math.
    const auto bagGet = [this, kind](const char* key) {
        const auto it = m_state->lastBag.find(key);
        if (it != m_state->lastBag.end())
            return it->second;
        const core::AdjustmentParamDesc* d = core::adjustmentParamDesc(kind, key);
        return d != nullptr ? d->def : 0.0;
    };
    if (levelsLayout) {
        // The input strip: black / gamma / white handles on the backdrop's histogram. The gamma
        // handle sits where the input maps to half output (t = 0.5^gamma), the classic Levels
        // placement, so dragging it toward black brightens.
        auto* hs = new HistoStrip(kPadX, cy, kPanelW - 2 * kPadX, kHistoH,
                                  HistoStrip::Body::Histogram);
        hs->setGroundColor(pal.panelBg);
        hs->setOnMarker([this, hs, bagGet](int idx, double t) {
            if (m_state->syncing)
                return;
            double b = bagGet("in_black");
            double w = bagGet("in_white");
            double g = bagGet("gamma");
            const char* key = "in_black";
            double v = t;
            if (idx == 0) {
                v = b = std::min(t, w - 0.02);
            } else if (idx == 2) {
                key = "in_white";
                v = w = std::max(t, b + 0.02);
            } else {
                key = "gamma";
                const double rel = std::clamp((t - b) / std::max(1e-4, w - b), 0.01, 0.99);
                v = g = std::clamp(std::log(rel) / std::log(0.5), 0.1, 10.0);
            }
            m_state->lastBag[key] = v;
            // Re-place all three from the constrained values so handles never cross visually.
            hs->setMarkers({b, b + (w - b) * std::pow(0.5, g), w});
            if (m_onEdit)
                m_onEdit(std::string("adjust:") + key,
                         [key, v](std::map<std::string, double>& bag) { bag[key] = v; });
        });
        m_state->histo = hs;
        cy += kHistoH + kRowGap;
        // The output row: a black-to-white ramp with the out_black / out_white handles.
        auto* rs = new HistoStrip(kPadX, cy, kPanelW - 2 * kPadX, kRampH, HistoStrip::Body::Ramp);
        rs->setGroundColor(pal.panelBg);
        rs->setOnMarker([this, rs, bagGet](int idx, double t) {
            if (m_state->syncing)
                return;
            double ob = bagGet("out_black");
            double ow = bagGet("out_white");
            const char* key = idx == 0 ? "out_black" : "out_white";
            // The output pair may INVERT (ob > ow is a legal negative ramp), so no ordering
            // constraint -- each handle owns its own value.
            const double v = t;
            (idx == 0 ? ob : ow) = v;
            m_state->lastBag[key] = v;
            rs->setMarkers({ob, ow});
            if (m_onEdit)
                m_onEdit(std::string("adjust:") + key,
                         [key, v](std::map<std::string, double>& bag) { bag[key] = v; });
        });
        m_state->ramp = rs;
        cy += kRampH + kRowGap;
    }
    if (thresholdLayout) {
        // The histogram with the single cut handle: you place the threshold where the
        // distribution actually splits, which is the whole point of the control.
        auto* hs = new HistoStrip(kPadX, cy, kPanelW - 2 * kPadX, kHistoH,
                                  HistoStrip::Body::Histogram);
        hs->setGroundColor(pal.panelBg);
        hs->setOnMarker([this](int, double t) {
            if (m_state->syncing)
                return;
            m_state->lastBag["level"] = t;
            if (m_onEdit)
                m_onEdit("adjust:level",
                         [t](std::map<std::string, double>& bag) { bag["level"] = t; });
        });
        m_state->histo = hs;
        cy += kHistoH + kRowGap;
    }
    if (curvesLayout) {
        // The channel picker, then the plot. Curves is the one kind whose parameter is not a
        // number: its four channel curves live in the SAME name->double bag as indexed knots
        // (core::adjustmentCurve / setAdjustmentCurve), so this row streams through the ordinary
        // funnel and undo/coalescing/.mosaic round-tripping all work unchanged
        // (docs/adjustment-layers.md §2.1). The picker is hand-built rather than schema-generated
        // because switching channel must RE-SEED the plot in the same callback -- the generic
        // Choice row only writes the bag.
        auto* cap = new Fl_Box(kPadX, cy, fieldLeft - kPadX - 8, kRowH, nullptr);
        cap->copy_label(_("Channel"));
        cap->box(FL_NO_BOX);
        cap->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        cap->labelfont(FL_HELVETICA);
        cap->labelsize(12);
        cap->labelcolor(toFl(pal.textMuted));
        auto* dd = new Dropdown(fieldLeft, cy, kFieldW, kRowH);
        if (const core::AdjustmentParamDesc* cd = core::adjustmentParamDesc(kind, "channel")) {
            for (int i = 0; i < cd->choiceCount; ++i)
                dd->add(_(cd->choices[i]), 0, nullptr, nullptr, 0); // plain words, no '/' parsing
        }
        dd->value(0);
        dd->callback(controlThunk, bind([this, dd] {
                         if (m_state->syncing)
                             return;
                         const int ch = std::clamp(dd->value(), 0, core::kCurveChannelCount - 1);
                         m_state->shownChannel = ch;
                         m_state->lastBag["channel"] = static_cast<double>(ch);
                         if (m_state->curve != nullptr) {
                             m_state->curve->setCurve(core::adjustmentCurve(
                                 m_state->lastBag, static_cast<core::CurveChannel>(ch)));
                             // ... and its histogram, in the SAME callback for the same reason:
                             // the plot must show the new channel before the next reflect().
                             m_state->curveHistoChannel = ch;
                             m_state->curve->setHistogram(
                                 m_state->histograms[static_cast<std::size_t>(ch)],
                                 curveHistogramTint(ch));
                         }
                         if (m_onEdit)
                             m_onEdit("adjust:channel",
                                      [ch](std::map<std::string, double>& bag) {
                                          bag["channel"] = static_cast<double>(ch);
                                      });
                     }));
        m_state->curveChannel = dd;
        cy += kRowH + kRowGap;
        auto* ce = new CurveEditor(kPadX, cy, kPanelW - 2 * kPadX, kCurveH);
        ce->setCellColor(pal.panelBg);
        ce->onChanged([this](const core::brush::Curve& c) {
            if (m_state->syncing)
                return;
            const auto ch = static_cast<core::CurveChannel>(
                std::clamp(m_state->shownChannel, 0, core::kCurveChannelCount - 1));
            // ONE undo step per gesture. FLTK reports the frames of a point drag as FL_DRAG, so
            // anything else (the push that starts a drag, an added/removed point, a corner
            // toggle) begins a new coalesce id -- a slider run's semantics, on a curve.
            if (Fl::event() != FL_DRAG)
                ++m_state->curveGesture;
            core::setAdjustmentCurve(m_state->lastBag, ch, c);
            if (m_onEdit) {
                m_onEdit("adjust:curve" + std::to_string(static_cast<int>(ch)) + ":" +
                             std::to_string(m_state->curveGesture),
                         [ch, c](std::map<std::string, double>& bag) {
                             core::setAdjustmentCurve(bag, ch, c);
                         });
            }
        });
        m_state->curve = ce;
        cy += kCurveH + kRowGap;
    }
    if (gradientLayout) {
        // The ramp row, ABOVE the generic "Reverse" toggle because the ramp is the control and
        // the toggle is a modifier of it. Same storage story as Curves' knots: the gradient is
        // indexed stops in the SAME name->double bag (core::adjustmentGradientMap /
        // setAdjustmentGradientMap), so undo, per-gesture coalescing and the .mosaic round trip
        // all work unchanged (docs/adjustment-layers.md §2.6). The chip is a preview + a button;
        // the editing surface is the host's shared GradientFlyout, opened on it.
        auto* cap = new Fl_Box(kPadX, cy, fieldLeft - kPadX - 8, kRowH, nullptr);
        cap->copy_label(_("Gradient"));
        cap->box(FL_NO_BOX);
        cap->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        cap->labelfont(FL_HELVETICA);
        cap->labelsize(12);
        cap->labelcolor(toFl(pal.textMuted));
        auto* chip = new PaintChip(fieldLeft, cy, kFieldW, kRowH);
        chip->setGroundColor(pal.panelBg);
        chip->setPaint(core::defaultGradientMap());
        chip->setOnClick([this] { openGradientFlyout(); });
        m_state->gradientChip = chip;
        cy += kRowH + kRowGap;
    }
    for (const core::AdjustmentParamDesc& d : schema) {
        if (ownedByCustom(d))
            continue; // the wheels / histogram handles / curve plot above own these rows
        State::Row row;
        row.desc = &d;
        const auto caption = [&] {
            auto* cap = new Fl_Box(kPadX, cy, fieldLeft - kPadX - 8, kRowH, nullptr);
            cap->copy_label(_(d.label));
            cap->box(FL_NO_BOX);
            cap->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
            cap->labelfont(FL_HELVETICA);
            cap->labelsize(12);
            cap->labelcolor(toFl(pal.textMuted));
            return cap;
        };
        if (d.type == core::AdjustmentParamType::Toggle) {
            auto* cb = new CheckBox(fieldLeft, cy, kFieldW, kRowH, _(d.label));
            cb->setGroundColor(pal.panelBg);
            cb->setOnToggle([this, key = d.key](bool on) {
                if (m_state->syncing)
                    return;
                const double v = on ? 1.0 : 0.0;
                m_state->lastBag[key] = v;
                if (m_onEdit)
                    m_onEdit(std::string("adjust:") + key,
                             [key, v](std::map<std::string, double>& bag) { bag[key] = v; });
            });
            row.check = cb;
        } else if (d.type == core::AdjustmentParamType::Choice) {
            row.caption = caption();
            auto* dd = new Dropdown(fieldLeft, cy, kFieldW, kRowH);
            // Add the options in the descriptor's DISPLAY order (identity when it has none) with a
            // group separator below each family; the STORED value stays the option index, so the
            // widget's position is mapped through choiceStoredValue/choiceDisplayPos everywhere.
            for (int pos = 0; pos < d.choiceCount; ++pos) {
                const int stored = choiceStoredValue(d, pos);
                const int flags = (d.choiceDivider != nullptr && d.choiceDivider[pos])
                                      ? FL_MENU_DIVIDER
                                      : 0;
                dd->add(_(d.choices[stored]), 0, nullptr, nullptr,
                        flags); // plain words -- no '/' path parsing risk
            }
            dd->value(choiceDisplayPos(d, static_cast<int>(std::clamp(d.def, d.min, d.max))));
            dd->callback(controlThunk, bind([this, dd, desc = &d, key = d.key] {
                             if (m_state->syncing)
                                 return;
                             const double v = choiceStoredValue(*desc, dd->value());
                             m_state->lastBag[key] = v;
                             if (m_onEdit)
                                 m_onEdit(std::string("adjust:") + key,
                                          [key, v](std::map<std::string, double>& bag) {
                                              bag[key] = v;
                                          });
                         }));
            row.choice = dd;
        } else if (d.type == core::AdjustmentParamType::Angle) {
            // A rotary knob for an angle-valued scalar (the blur family's direction/rotation): a
            // square dial in the field area + a degrees readout to its right, mirroring the
            // layer-effects Direction/Angle dials. The bag stores the raw math angle unchanged, so
            // setZeroOffset(90) is what turns the needle to the true blur direction (0 = +x = right)
            // without touching the serialized value; the dial funnels edits through the SAME
            // coalesce id + undo path as the ScrubSlider it replaces.
            row.caption = caption();
            auto* dial = new Dial(fieldLeft, cy, kRowH, kRowH);
            dial->range(d.min, d.max);
            dial->step(d.step);
            dial->setCellColor(pal.panelBg);
            dial->setZeroOffset(90.0); // value 0 (0 = +x) points the needle at 3 o'clock
            dial->setDefaultValue(d.def);
            dial->when(FL_WHEN_CHANGED);
            dial->value(d.def);
            auto* ro = new Fl_Box(fieldLeft + kRowH + 8, cy, kFieldW - kRowH - 8, kRowH, nullptr);
            ro->box(FL_NO_BOX);
            ro->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
            ro->labelfont(FL_HELVETICA);
            ro->labelsize(12);
            ro->labelcolor(toFl(pal.text));
            const auto writeDeg = [](Fl_Box* b, double deg, const char* suffix) {
                char buf[24];
                std::snprintf(buf, sizeof(buf), "%d%s", static_cast<int>(std::lround(deg)), suffix);
                b->copy_label(buf);
            };
            writeDeg(ro, dial->value(), d.suffix);
            dial->callback(controlThunk,
                           bind([this, dial, ro, writeDeg, suffix = d.suffix, key = d.key] {
                               if (m_state->syncing)
                                   return;
                               const double v = dial->value();
                               m_state->lastBag[key] = v;
                               writeDeg(ro, v, suffix);
                               ro->redraw();
                               if (m_onEdit)
                                   m_onEdit(std::string("adjust:") + key,
                                            [key, v](std::map<std::string, double>& bag) {
                                                bag[key] = v;
                                            });
                           }));
            row.dial = dial;
            row.dialReadout = ro;
        } else {
            row.caption = caption();
            auto* s = new ScrubSlider(fieldLeft, cy, kFieldW, kRowH);
            s->range(d.min, d.max);
            s->step(d.step);
            s->setSuffix(d.suffix);
            s->setCellColor(pal.panelBg);
            s->setDefaultValue(d.def);
            s->setRuler(m_ruler);
            s->value(d.def);
            s->when(FL_WHEN_CHANGED);
            s->callback(controlThunk, bind([this, s, key = d.key] {
                            if (m_state->syncing)
                                return;
                            const double v = s->value();
                            m_state->lastBag[key] = v;
                            if (m_onEdit)
                                m_onEdit(std::string("adjust:") + key,
                                         [key, v](std::map<std::string, double>& bag) {
                                             bag[key] = v;
                                         });
                        }));
            // Hue/Saturation reads professionally with value-ramp tracks: the hue slider carries
            // the spectrum (red centred at 0), saturation a gray-to-vivid ramp, lightness a
            // black-to-white ramp.
            if (kind == core::AdjustmentKind::HueSaturation) {
                const std::string_view key = d.key;
                if (key == "hue") {
                    s->setTrackFill([](double t) { return wheelHue(t - 0.5, 1.0); });
                } else if (key == "saturation") {
                    s->setTrackFill([](double t) {
                        const common::Color8 vivid = wheelHue(0.0, 1.0);
                        const auto mix = [&](std::uint8_t v) {
                            return static_cast<std::uint8_t>(std::lround(128 + (v - 128) * t));
                        };
                        return common::Color8{mix(vivid.r), mix(vivid.g), mix(vivid.b), 255};
                    });
                } else if (key == "lightness") {
                    s->setTrackFill([](double t) {
                        const auto v = static_cast<std::uint8_t>(std::lround(255.0 * t));
                        return common::Color8{v, v, v, 255};
                    });
                }
            }
            row.slider = s;
        }
        m_state->rows.push_back(row);
        cy += kRowH + kRowGap;
        // Photo Filter's colour sits directly under the preset picker it belongs to, which is why
        // it is emitted from inside the loop rather than before or after it. It always shows the
        // EFFECTIVE filter colour (a preset's own, or the Custom rows'), and it is clickable only
        // under Custom -- clicking it while a named preset is chosen would offer to edit a colour
        // the maths is not using. syncValues() owns both of those, live.
        if (photoFilterLayout && std::strcmp(d.key, "filter") == 0) {
            auto* cap = new Fl_Box(kPadX, cy, fieldLeft - kPadX - 8, kRowH, nullptr);
            cap->copy_label(_("Filter color"));
            cap->box(FL_NO_BOX);
            cap->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
            cap->labelfont(FL_HELVETICA);
            cap->labelsize(12);
            cap->labelcolor(toFl(pal.textMuted));
            auto* sw = new SwatchChip(fieldLeft, cy, kFieldW, kRowH);
            sw->setGroundColor(pal.panelBg);
            sw->setInteractive(false); // syncValues turns it on under the Custom preset
            sw->setOnClick([this] { openFilterColor(); });
            m_state->filterSwatch = sw;
            cy += kRowH + kRowGap;
        }
    }

    // ---- Reset: re-seed the schema defaults as ONE ordinary undoable edit ----
    auto* reset = new FlatButton(kPanelW - kPadX - 88, cy + (kFooterH - 28) / 2 - 2, 88, 28,
                                 _("Reset"));
    reset->callback(controlThunk, bind([this, kind] {
                        if (m_onEdit)
                            m_onEdit("adjust:reset", [kind](std::map<std::string, double>& bag) {
                                for (const core::AdjustmentParamDesc& d :
                                     core::adjustmentParamSchema(kind))
                                    bag[d.key] = d.def;
                                // Curves' knots are not schema rows, so re-seeding the schema
                                // cannot restore them: Reset ERASES them, which is the identity
                                // (absent == identity, core/adjustments.hpp).
                                if (kind == core::AdjustmentKind::Curves)
                                    core::clearAdjustmentCurves(bag);
                                // Same shape for Gradient Map's stops, and the same one-line fix:
                                // absent spells the DEFAULT black-to-white ramp there (§2.6), so
                                // erasing them IS "back to the factory ramp".
                                if (kind == core::AdjustmentKind::GradientMap)
                                    core::clearAdjustmentGradientMap(bag);
                            });
                        // The funnel's edit lands in the document; the host's per-frame reflect
                        // syncs the controls from the fresh bag (no manual sync here).
                    }));

    end();
    m_built = true;
    m_blendDirty = true;
}

void AdjustmentPanel::syncValues(const std::map<std::string, double>& bag) {
    m_state->syncing = true;
    // Grayscale's "Grays" palette is meaningless for the 1-bit Dithered method, so that row is
    // hidden while Dithered is selected (and restored otherwise). syncValues runs on every bag
    // change (reflect()), so the row tracks the method live. Read the method through the schema so
    // an absent/out-of-range bag entry resolves to the same value the compositor would.
    bool hideGrays = false;
    if (m_kind == core::AdjustmentKind::Grayscale) {
        if (const core::AdjustmentParamDesc* md = core::adjustmentParamDesc(m_kind, "method")) {
            const auto mit = bag.find("method");
            const double mv =
                mit == bag.end() ? md->def : std::clamp(mit->second, md->min, md->max);
            hideGrays = static_cast<int>(std::lround(mv)) ==
                        static_cast<int>(core::GrayscaleMethod::Dithered);
        }
    }
    for (const State::Row& r : m_state->rows) {
        const auto it = bag.find(r.desc->key);
        const double v =
            it == bag.end() ? r.desc->def : std::clamp(it->second, r.desc->min, r.desc->max);
        if (r.slider != nullptr)
            r.slider->value(v);
        if (r.check != nullptr)
            r.check->setChecked(v >= 0.5);
        if (r.choice != nullptr) // the bag stores the enum index; the dropdown shows its position
            r.choice->value(choiceDisplayPos(*r.desc, static_cast<int>(std::lround(v))));
        if (m_kind == core::AdjustmentKind::Grayscale && std::strcmp(r.desc->key, "grays") == 0) {
            for (Fl_Widget* w : {static_cast<Fl_Widget*>(r.caption),
                                 static_cast<Fl_Widget*>(r.slider)})
                if (w != nullptr)
                    hideGrays ? w->hide() : w->show();
        }
        if (r.dial != nullptr) { // undo/redo + canvas-gizmo edits move the dial and its readout
            r.dial->value(v);
            if (r.dialReadout != nullptr) {
                char buf[24];
                std::snprintf(buf, sizeof(buf), "%d%s", static_cast<int>(std::lround(v)),
                              r.desc->suffix);
                r.dialReadout->copy_label(buf);
                r.dialReadout->redraw();
            }
        }
    }
    for (const State::Wheel& wb : m_state->wheels) {
        const auto get = [&](const std::string& k) {
            const auto it = bag.find(k);
            return it == bag.end() ? 0.0 : std::clamp(it->second, -100.0, 100.0);
        };
        const std::string prefix(wb.prefix);
        const core::ColorBalancePoint p = core::colorBalanceToPlane(
            {get(prefix + "_cr"), get(prefix + "_mg"), get(prefix + "_yb")});
        wb.wheel->setValue(p.x / 100.0, p.y / 100.0);
    }
    const auto get = [&](const char* key) {
        const auto it = bag.find(key);
        if (it != bag.end())
            return it->second;
        const core::AdjustmentParamDesc* d = core::adjustmentParamDesc(m_kind, key);
        return d != nullptr ? d->def : 0.0;
    };
    if (m_state->histo != nullptr && m_kind == core::AdjustmentKind::Levels) {
        const double b = std::clamp(get("in_black"), 0.0, 1.0);
        const double w = std::clamp(get("in_white"), 0.0, 1.0);
        const double g = std::clamp(get("gamma"), 0.1, 10.0);
        m_state->histo->setMarkers({b, b + (w - b) * std::pow(0.5, g), w});
        if (m_state->ramp != nullptr)
            m_state->ramp->setMarkers({std::clamp(get("out_black"), 0.0, 1.0),
                                       std::clamp(get("out_white"), 0.0, 1.0)});
    } else if (m_state->histo != nullptr && m_kind == core::AdjustmentKind::Threshold) {
        m_state->histo->setMarkers({std::clamp(get("level"), 0.0, 1.0)});
    }
    if (m_state->curve != nullptr) {
        // Undo/redo (and Reset) move the knots under us: re-seed the plot from the bag's curve
        // for whichever channel the bag says we are on. setCurve deliberately does NOT fire the
        // change callback, so this can never loop back into the funnel.
        const int ch = std::clamp(static_cast<int>(std::lround(get("channel"))), 0,
                                  core::kCurveChannelCount - 1);
        m_state->shownChannel = ch;
        if (m_state->curveChannel != nullptr)
            m_state->curveChannel->value(ch);
        m_state->curve->setCurve(
            core::adjustmentCurve(bag, static_cast<core::CurveChannel>(ch)));
        // The plot's backdrop histogram follows the channel (S34-a). Pushed only when the shown
        // channel actually changes: syncValues runs on every bag change, and a live point drag
        // must not re-upload 256 bins (and re-damage the widget) per frame.
        if (m_state->curveHistoChannel != ch) {
            m_state->curveHistoChannel = ch;
            m_state->curve->setHistogram(
                m_state->histograms[static_cast<std::size_t>(ch)], curveHistogramTint(ch));
        }
    }
    if (m_state->gradientChip != nullptr) // undo/redo/Reset move the ramp under the chip too
        m_state->gradientChip->setPaint(core::adjustmentGradientMap(bag));
    if (m_state->filterSwatch != nullptr) {
        // The swatch always shows what the compositor will actually use, and is live only where
        // there is something to edit: a named preset owns its colour, Custom owns the three rows.
        const auto preset = static_cast<core::PhotoFilterPreset>(std::clamp(
            static_cast<int>(std::lround(get("filter"))), 0, core::kPhotoFilterPresetCount - 1));
        const bool custom = preset == core::PhotoFilterPreset::Custom;
        m_state->filterSwatch->setColour(
            custom ? common::Color8{levelByte(get("color_r")), levelByte(get("color_g")),
                                    levelByte(get("color_b")), 255}
                   : core::photoFilterPresetColor(preset));
        m_state->filterSwatch->setInteractive(custom && static_cast<bool>(m_onEditColor));
        m_state->filterSwatch->copy_tooltip(custom ? _("Click to choose the filter color")
                                                   : nullptr);
    }
    m_state->lastBag = bag;
    m_state->syncing = false;
    m_blendDirty = true; // control faces changed; a faded panel must re-blend
}

void AdjustmentPanel::reflect(const core::AdjustmentLayer& layer) {
    const bool rebuilt = !m_built || layer.adjustmentKind() != m_kind;
    const bool retargeted = layer.id() != m_target;
    m_target = layer.id();
    if (rebuilt) {
        m_kind = layer.adjustmentKind();
        build(m_kind);
    }
    // The histograms show the BACKDROP the adjustment grades: refreshed when the panel starts
    // showing a different layer (its scope differs), not per frame -- while you drag the panel's
    // own handles the backdrop cannot change. Only computed for the layouts that draw one (the
    // Levels/Threshold strips and, since S34-a, the Curves plot).
    if ((rebuilt || retargeted) && m_backdrop &&
        (m_state->histo != nullptr || m_state->curve != nullptr)) {
        m_state->histograms = backdropHistograms(m_backdrop());
        m_state->curveHistoChannel = -1; // force the plot's histogram to be (re)pushed below
        if (m_state->histo != nullptr)
            m_state->histo->setHistogram(m_state->histograms[0]); // luma, as it always was
    }
    // `retargeted` joins the drift test since S34-a: a fresh target re-reads the backdrop
    // histograms, and the plot needs a sync to take the new one even when the two layers' bags
    // happen to be equal.
    if (rebuilt || retargeted || layer.params() != m_state->lastBag)
        syncValues(layer.params());
    if (rebuilt && shown())
        reanchor(); // the row count (and so the height) changed under an open panel
}

void AdjustmentPanel::openGradientFlyout() {
    if (m_gradientFlyout == nullptr || m_state->gradientChip == nullptr)
        return;
    if (m_gradientFlyout->shownForAnchor(m_state->gradientChip)) { // a re-click toggles it shut
        m_gradientFlyout->hide();
        return;
    }
    // Re-pointed on every open, so sharing the host's editor with another opener is safe (the
    // ImageOpsPanel rule). The flyout edits STOPS + spread; a gradient map has no geometry, so
    // only the stops are taken and everything else stays the fixed ramp shape (§2.6).
    m_gradientFlyout->setOnChange([this](const core::vec::Gradient& g) {
        if (m_state->syncing)
            return;
        core::vec::Gradient ramp = core::defaultGradientMap();
        ramp.stops = g.stops;
        // ONE undo step per gesture, the Curves rule: FLTK reports the frames of a stop drag as
        // FL_DRAG, so anything else (a colour edit, an added/removed stop, the push that starts a
        // drag) begins a new coalesce id -- a slider run's semantics, on a ramp.
        if (Fl::event() != FL_DRAG)
            ++m_state->gradientGesture;
        core::setAdjustmentGradientMap(m_state->lastBag, ramp);
        if (m_state->gradientChip != nullptr)
            m_state->gradientChip->setPaint(ramp);
        if (m_onEdit) {
            m_onEdit("adjust:gradient:" + std::to_string(m_state->gradientGesture),
                     [ramp](std::map<std::string, double>& bag) {
                         core::setAdjustmentGradientMap(bag, ramp);
                     });
        }
    });
    m_gradientFlyout->openFor(m_state->gradientChip,
                              core::adjustmentGradientMap(m_state->lastBag));
}

void AdjustmentPanel::openFilterColor() {
    if (!m_onEditColor || m_state->filterSwatch == nullptr)
        return;
    m_onEditColor(m_state->filterSwatch, m_state->filterSwatch->colour());
}

void AdjustmentPanel::setPickedColor(common::Color8 c) {
    // The host's pick router hands every colour to whichever panel is up; only Photo Filter has
    // one to take.
    if (m_kind != core::AdjustmentKind::PhotoFilter || m_state->filterSwatch == nullptr)
        return;
    const double r = c.r;
    const double g = c.g;
    const double b = c.b;
    // Picking a colour IS choosing Custom. Writing the three colour rows while the dropdown still
    // said "Warming (85)" would leave the swatch showing a colour the maths does not use, so the
    // preset moves with them -- one edit, one undo step, no intermediate lying state.
    const auto custom = static_cast<double>(core::PhotoFilterPreset::Custom);
    m_state->lastBag["filter"] = custom;
    m_state->lastBag["color_r"] = r;
    m_state->lastBag["color_g"] = g;
    m_state->lastBag["color_b"] = b;
    m_state->filterSwatch->setColour(c);
    if (m_onEdit) {
        m_onEdit("adjust:filter_color", [r, g, b, custom](std::map<std::string, double>& bag) {
            bag["filter"] = custom;
            bag["color_r"] = r;
            bag["color_g"] = g;
            bag["color_b"] = b;
        });
    }
}

void AdjustmentPanel::hide() {
    // An editor bubble anchored to one of our rows must not outlive the panel on screen (the
    // Fill dialog's rule) -- it would float over the canvas with nothing behind it.
    if (m_gradientFlyout != nullptr && m_gradientFlyout->shown())
        m_gradientFlyout->hide();
    Popover::hide();
}

void AdjustmentPanel::openFor(const Fl_Widget* anchor) {
    // This panel is opened by SELECTION, not by a click on it, so the show() must not move the
    // keyboard focus: stealing it silences whatever the user was typing into (round 4).
    Fl_Widget* focus = Fl::focus();
    showAnchored(anchor); // corner placement drives geometry; the anchor is for bookkeeping
    if (focus != nullptr)
        Fl::focus(focus);
}

void AdjustmentPanel::reapplyTheme() {
    Popover::reapplyTheme();
    if (m_built) { // regenerate the rows in the new palette, keeping the shown values
        const std::map<std::string, double> bag = m_state->lastBag;
        build(m_kind);
        syncValues(bag);
    }
}

void AdjustmentPanel::setFade(double opacity) {
    const double f = std::clamp(opacity, 0.25, 1.0);
    if (f == m_fade)
        return;
    m_fade = f;
    m_blendDirty = true; // the fade level is baked into the blend
    if (f >= 1.0) {      // back to opaque: drop the cache, the ordinary draw path resumes
        m_blend.clear();
        m_blendRW = 0;
        m_blendRH = 0;
    }
    redraw();
}

void AdjustmentPanel::refreshFadeBlend() {
    // Called by the HOST (onFrame), never from draw(): creating an Fl_Image_Surface inside a
    // draw() corrupts the active graphics context. Renders the ground + each CHILD into the
    // surface (never the window itself -- an Fl_Window snapshots BLACK, the settings-dialog
    // trap), blends over the host's reconstruction of the canvas beneath, caches the result
    // for draw() to blit.
    if (m_fade >= 1.0 || !m_under || !shown())
        return;
    common::Image under = m_under(w(), h());
    if (under.empty() || under.width == 0 || under.height == 0)
        return;
    Fl_Image_Surface surf(w(), h());
    Fl_Surface_Device::push_current(&surf);
    const Palette& pal = activePalette();
    fl_color(toFl(pal.panelBg)); // the Popover plain-panel ground + hairline
    fl_rectf(0, 0, w(), h());
    fl_color(toFl(pal.border));
    fl_rect(0, 0, w(), h());
    // The deltas are REQUIRED: Fl_Widget_Surface::draw subtracts a non-window widget's own
    // x()/y() from the surface origin, so without them every child renders at the surface's
    // TOP-LEFT (the round-4 "controls pushed to the top-left, only Reset visible" bug).
    // Skip hidden children: Fl_Widget_Surface::draw calls the child's own draw() directly and
    // (unlike Fl_Group::draw_children) never consults visible(), so an un-guarded loop would bake
    // a hidden row -- Grayscale's "Grays" under the Dithered method -- back into the faded cache
    // even though the opaque draw path hides it correctly.
    for (int i = 0; i < children(); ++i)
        if (child(i)->visible())
            surf.draw(child(i), child(i)->x(), child(i)->y());
    Fl_Surface_Device::pop_current();
    for (int i = 0; i < children(); ++i)
        child(i)->clear_damage(); // they were "drawn" (offscreen); stop re-damage loops
    const std::unique_ptr<Fl_RGB_Image> panelImg(surf.image());
    if (!panelImg || panelImg->d() < 3)
        return;
    // The surface reads back at DEVICE resolution under a scaled screen (the drag-ghost trap):
    // blend at raster size, sampling the logical-sized under image nearest; draw() scales back.
    const int rw = panelImg->w();
    const int rh = panelImg->h();
    const int depth = panelImg->d();
    const int stride = panelImg->ld() != 0 ? panelImg->ld() : rw * depth;
    const auto* src = reinterpret_cast<const unsigned char*>(panelImg->data()[0]);
    m_blend.assign(static_cast<std::size_t>(rw) * rh * 3, 0);
    const double a = m_fade;
    for (int j = 0; j < rh; ++j) {
        const auto uy = static_cast<std::uint32_t>(
            std::min<long>(under.height - 1, static_cast<long>(j) * under.height / rh));
        for (int i = 0; i < rw; ++i) {
            const auto ux = static_cast<std::uint32_t>(
                std::min<long>(under.width - 1, static_cast<long>(i) * under.width / rw));
            const std::size_t up = (static_cast<std::size_t>(uy) * under.width + ux) * 4;
            const std::size_t sp = static_cast<std::size_t>(j) * stride +
                                   static_cast<std::size_t>(i) * depth;
            const std::size_t dp = (static_cast<std::size_t>(j) * rw + i) * 3;
            for (int ch = 0; ch < 3; ++ch) {
                m_blend[dp + ch] = static_cast<unsigned char>(std::lround(
                    src[sp + ch] * a + under.rgba[up + ch] * (1.0 - a)));
            }
        }
    }
    m_blendRW = rw;
    m_blendRH = rh;
    m_blendDirty = false;
    redraw();
}

void AdjustmentPanel::drawContent() {
    if (m_fade < 1.0 && !m_blend.empty() && m_blendRW > 0 && m_blendRH > 0) {
        // Blit the cached blend (RGB triples -- depth 3 on purpose: a depth-4 fl_draw_image is
        // interpreted inconsistently across surfaces, the round-3 magenta artifact).
        Fl_RGB_Image out(m_blend.data(), m_blendRW, m_blendRH, 3);
        out.scale(w(), h(), /*proportional=*/0, /*can_expand=*/1);
        out.draw(0, 0);
        return;
    }
    Popover::drawContent(); // opaque (or the blend isn't built yet -- the host refreshes it next frame)
}

} // namespace mosaic::ui
