#include "ui/brush_preset_panel.hpp"

#include "common/i18n.hpp"
#include "common/log.hpp"
#include "core/brush/stroke_preview.hpp" // the card's stroke, laid by the REAL engine
#include "io/brush/kpp.hpp"     // readKppIcon: a LOOSE .kpp carries its own raster
#include "io/brush/library.hpp"
#include "io/brush/preset.hpp"
#include "io/brush/preset_brush.hpp" // presetBrushParams: once per preset, never per render
#include "io/brush/preset_json.hpp"  // readMbpIcon: ... and so does a loose .mbp
#include "ui/brush_presets.hpp"
#include "ui/icons.hpp"
#include "ui/theme.hpp"

#include <FL/Fl.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/Fl_Scroll.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <utility>

namespace mosaic::ui {
namespace {

// A LOOSE preset's raster (ui/brush_presets.hpp): the file itself is the PNG, so its icon comes out
// of the same walker the library uses -- just without an archive around it. Nullopt (with *error
// set) for anything unreadable, which the caller caches as a MISS exactly like a broken `.kpp`.
[[nodiscard]] std::optional<common::Image> loadLoosePresetIcon(const std::string& path,
                                                               std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error != nullptr)
            *error = "cannot open " + path;
        return std::nullopt;
    }
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                          std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        if (error != nullptr)
            *error = "empty file";
        return std::nullopt;
    }
    std::string ext = std::filesystem::path(path).extension().string();
    for (char& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == ".kpp")
        return io::brush::readKppIcon(bytes.data(), bytes.size(), error);
    return io::brush::readMbpIcon(bytes.data(), bytes.size(), error);
}

// The section header (title + the "12 of 117" readout) and the search row above the grid.
constexpr int kHeaderH = 26;
constexpr int kSearchH = 24;
constexpr int kSearchGap = 6; // search row -> grid
constexpr int kClearW = 20;   // the clear-filter button, shown only while the filter is non-empty
constexpr int kEditW = 46;    // the "Edit…" button -- always there, greyed when nothing is editable

// ⚠ The dock's left-edge resize band (== RightDock::splitterWidth()). NOTHING clickable may share
// those pixels -- the same rule the layer list obeys by insetting its rows past it. The grid's own
// padding then lands column 0 at kLeftInset, which is where the title and the search box start too,
// so the section reads as one column rather than three margins.
constexpr int kGrabBand = 5;
constexpr int kGridPad = 6;
constexpr int kLeftInset = kGrabBand + kGridPad;
constexpr int kRightPad = 8;

// Cells. The grid derives its column count from a MINIMUM width and spreads the leftover, so a
// dock dragged to any width still fills edge to edge.
constexpr int kGap = 6;
// 76, not 72: at the default 280 px dock that is still three columns, but each is now wide enough
// for "Default round" -- the FIRST cell in the grid -- to read without an ellipsis.
constexpr int kMinCellW = 76;
constexpr int kMaxCols = 8;
constexpr int kCellInset = 4; // cell edge -> thumbnail
constexpr int kLabelH = 15;   // the name line under the thumbnail
constexpr int kLabelGap = 3;

Fl_Color toFl(common::Color8 c) {
    return fl_rgb_color(c.r, c.g, c.b);
}

// The two fidelity inks (§6.4). Fixed rather than palette tokens: the palette has no warning colour,
// and these mean the same thing in both themes. Amber = approximated (something was dropped), red =
// substituted (Mosaic has no engine for the source paintop and imported its nearest pixel-brush kin).
constexpr common::Color8 kApproxInk{214, 158, 46, 255};
constexpr common::Color8 kSubstInk{198, 84, 72, 255};

// The Approximated presets whose SOURCE PAINTOP has no engine here at all, as opposed to merely
// having lost some options. Null for the rest. See tooltipFor -- these need the specific
// sentence, and the paintop ids are the mapper's own (io/brush/mapper.cpp). `colorsmudge` left
// this list when the smudge engine landed (§6.6c): its presets really smear now, and whatever
// they still drop (Scatter, Texture) is listed like any pixel brush's.
[[nodiscard]] const char* missingEngineNote(std::string_view paintop) {
    if (paintop == "spraybrush")
        return _("No spray engine yet: this stamps its tip. It does not scatter particles.");
    if (paintop == "filter")
        return _("No filter-brush engine yet: this paints colour. It does not filter.");
    return nullptr;
}

// Cards (docs/brushes.md §8.2). One per row: the tip icon on the left, and the long strip carrying a
// stroke of that very brush filling the rest, with the name above it.
// ⚠ The tip icon has NO size of its own: it is a square as tall as the strip, sharing its top and its
// bottom. Two boxes side by side that ALMOST line up read as a mistake.
constexpr int kCardStrokeH = 58;  // the stroke strip's height -- and the tip icon's square
constexpr int kCardNameH = 14;    // the name line above the strip
constexpr int kCardGap = 8;       // icon -> strip

// ⚠⚠ THE STRIP'S RENDER WIDTH IS QUANTIZED TO THIS, AND IT IS A PERFORMANCE CONTRACT, NOT TIDINESS.
// A stroke preview costs ~1.7 ms through the real engine -- a thousand times a box filter. A strip
// whose rendered width tracked the dock's pixel for pixel would re-render every visible card on
// EVERY FRAME of a width drag, which is exactly the lag the dock was just dug out of (and would be a
// far more expensive version of it). Rendering at the next bucket up and CROPPING to fit costs one
// re-render per 32 px of drag instead of one per pixel.
//
// ⚠ CROPPED, NEVER SCALED. Scaling a stroke horizontally would make the brush lie about its own
// size, which is the one thing the preview exists to tell the truth about.
constexpr int kStrokeBucket = 32;

// The largest dab the strip will draw. §8.3 rules that a preview renders at TRUE BRUSH SCALE and
// lets a big brush overflow its box; this is a ceiling on that, and the only dishonesty in the card.
// A preset's authored diameter runs to 1000 px, and a 1000 px tip rasterizes a MEGAPIXEL MASK PER
// DAB into a strip that cannot tell one blob from a bigger blob anyway.
//
// ⚠ IT IS SET AGAINST kCardStrokeH, NOT CHOSEN FREELY. The preview path insets itself by the brush's
// RADIUS so the stroke cannot hang over the edges of its box (core/brush/stroke_preview.hpp) -- which
// means a ceiling anywhere near the strip's own height leaves the S-curve no room to swing and
// flattens it into a fat straight sausage. A stroke has to look like a stroke: 28 in 58 leaves the
// curve about 8 px of margin at its widest, and that is what buys the taper, the bend and the edge.
constexpr double kStrokeMaxDiameter = 28.0;

// The tab strip.
constexpr int kTabH = 22;
constexpr int kTabGap = 2;
constexpr int kTabPadX = 9;      // text inset inside a tab
constexpr int kTabFontSize = 11;
constexpr int kTabStripGap = 5;  // strip -> search row
constexpr int kTabFadeW = 14;    // the "there is more this way" gradient at a scrollable edge

// px of travel before a press becomes a scroll rather than a click -- the tab strip's rule, and the
// grid's too now that both scroll by direct drag.
constexpr int kDragSlop = 3;
constexpr double kFlingTickS = 1.0 / 60.0; // the fling timer's cadence (its step is analytic anyway)

} // namespace

// ---- Pure grid maths -------------------------------------------------------------------------

const char* presetDisplayModeKey(PresetDisplayMode mode) noexcept {
    return mode == PresetDisplayMode::Cards ? "cards" : "grid";
}

PresetDisplayMode presetDisplayModeFromKey(std::string_view key) noexcept {
    // ⚠ The DEFAULT is Cards, so anything unrecognised -- including the empty string a settings file
    // written before this existed carries -- lands on Cards, not on Grid.
    return key == "grid" ? PresetDisplayMode::Grid : PresetDisplayMode::Cards;
}

PresetGridMetrics presetGridMetrics(int viewWidth, PresetDisplayMode mode) {
    PresetGridMetrics m;
    m.mode = mode;
    const int avail = std::max(24, viewWidth - 2 * kGridPad);

    if (mode == PresetDisplayMode::Cards) {
        // One card per row, full width. The card is a ROW, not a tile: its whole point is the long
        // strip, and a strip only reads as a stroke if it is long.
        //
        // ⚠ The tip icon is the SAME BOX as the strip -- same height, same top, same bottom. Two boxes
        // side by side that ALMOST line up read as a mistake, and they were: the icon was centred
        // against the whole cell while the strip hung below the name line.
        m.cols = 1;
        m.cellW = avail;
        m.thumb = kCardStrokeH;
        m.cellH = kCardNameH + kCardStrokeH + 2 * kCellInset;
        return m;
    }

    m.cols = std::clamp((avail + kGap) / (kMinCellW + kGap), 1, kMaxCols);
    m.cellW = std::max(24, (avail - (m.cols - 1) * kGap) / m.cols);
    m.thumb = std::max(8, m.cellW - 2 * kCellInset);
    m.cellH = m.thumb + 2 * kCellInset + kLabelGap + kLabelH;
    return m;
}

int presetGridContentHeight(int cellCount, const PresetGridMetrics& m) {
    if (cellCount <= 0 || m.cols <= 0)
        return 0;
    const int rows = (cellCount + m.cols - 1) / m.cols;
    return 2 * kGridPad + rows * m.cellH + (rows - 1) * kGap;
}

PresetCellRect presetCellRect(int slot, const PresetGridMetrics& m, int originX, int originY) {
    PresetCellRect r;
    if (slot < 0 || m.cols <= 0)
        return r;
    const int col = slot % m.cols;
    const int row = slot / m.cols;
    r.x = originX + kGridPad + col * (m.cellW + kGap);
    r.y = originY + kGridPad + row * (m.cellH + kGap);
    r.w = m.cellW;
    r.h = m.cellH;
    return r;
}

PresetCellRect presetThumbRect(const PresetCellRect& cell, const PresetGridMetrics& m) {
    PresetCellRect r;
    r.w = m.thumb;
    r.h = m.thumb;
    if (m.mode == PresetDisplayMode::Cards) {
        r.x = cell.x + kCellInset;                 // hard left: the strip takes the rest of the row
        r.y = cell.y + kCellInset + kCardNameH;    // ... and its top is the STRIP's top (see below)
    } else {
        r.x = cell.x + (cell.w - m.thumb) / 2;  // centred over the cell's own label
        r.y = cell.y + kCellInset;
    }
    return r;
}

PresetCellRect presetStrokeRect(const PresetCellRect& cell, const PresetGridMetrics& m) {
    PresetCellRect r;
    if (m.mode != PresetDisplayMode::Cards)
        return r; // a grid has no strip

    const PresetCellRect th = presetThumbRect(cell, m);
    r.x = th.x + th.w + kCardGap;
    r.y = cell.y + kCellInset + kCardNameH;
    r.w = std::max(0, (cell.x + cell.w - kCellInset) - r.x);
    r.h = kCardStrokeH;
    return r;
}

core::brush::StrokePreviewStyle presetStrokeStyle(const Palette& pal, bool eraser) {
    core::brush::StrokePreviewStyle style;
    style.maxDiameter = kStrokeMaxDiameter;

    if (eraser) {
        // ⚠⚠ AN ERASER'S PAPER IS A SLAB OF PAINT, and it has to be. An eraser can only take paper
        // AWAY, and what shows through the hole is the DOCK -- so paper made of the dock's own ground
        // (which is exactly what the dark theme's paper is, below) would carve a panel-coloured hole
        // in a panel-coloured card: perfectly invisible. The muted ink IS the paper here, and the
        // eraser removes it to reveal the dock. That reads in BOTH themes; white-paper-on-a-dark-dock
        // only ever read in one, and it read by accident.
        style.paper = pal.textMuted;
        style.paper.a = 255;
        style.ink = pal.textMuted;
        return style;
    }

    if (pal.dark) {
        // The card belongs to the dock it sits in. A white slab per row on a dark UI is a row of
        // lightboxes, and it hurts to look at.
        style.paper = pal.panelBg;
        style.ink = pal.textMuted;
    } else {
        style.paper = common::Color8{255, 255, 255, 255}; // black on white: right here, and kept
        style.ink = common::Color8{0, 0, 0, 255};
    }
    style.paper.a = 255; // OPAQUE, always: see stroke_preview.hpp
    style.ink.a = 255;
    return style;
}

int presetStrokeRenderWidth(int stripWidth) {
    if (stripWidth <= 0)
        return 0;
    // The next bucket UP, so the rendered strip always covers the strip it is cropped into. See
    // kStrokeBucket: this is what keeps a width drag from re-rendering the engine on every frame.
    return ((stripWidth + kStrokeBucket - 1) / kStrokeBucket) * kStrokeBucket;
}

int presetSlotAt(int localX, int localY, int cellCount, const PresetGridMetrics& m) {
    if (cellCount <= 0 || m.cols <= 0)
        return -1;
    const int gx = localX - kGridPad;
    const int gy = localY - kGridPad;
    if (gx < 0 || gy < 0)
        return -1;
    const int strideX = m.cellW + kGap;
    const int strideY = m.cellH + kGap;
    const int col = gx / strideX;
    const int row = gy / strideY;
    if (col >= m.cols)
        return -1;
    if (gx - col * strideX >= m.cellW || gy - row * strideY >= m.cellH)
        return -1; // in a gutter: the gaps between cells belong to nobody
    const int slot = row * m.cols + col;
    return slot < cellCount ? slot : -1;
}

// ---- Pure fling maths ------------------------------------------------------------------------

PresetFlingStep presetFlingStep(double velocity, double dt) {
    PresetFlingStep out;
    out.velocity = velocity;
    if (dt <= 0.0)
        return out;
    // v(t) = v0 * e^(-t/tau); the travel is its integral. Analytic on purpose: two 16 ms ticks and
    // one 32 ms tick land on the SAME position, so a stalled timer delays a fling but never
    // lengthens it.
    const double decay = std::exp(-dt / kPresetFlingTau);
    out.dx = velocity * kPresetFlingTau * (1.0 - decay);
    out.velocity = velocity * decay;
    return out;
}

bool presetFlingDead(double velocity) noexcept {
    return std::abs(velocity) < kPresetFlingDeadV;
}

void PresetFlingTracker::push(double timeS, double pos) noexcept {
    m_ring[m_count % kCap] = {timeS, pos};
    ++m_count;
}

double PresetFlingTracker::releaseVelocity(double timeS) const noexcept {
    const std::size_t have = std::min(m_count, kCap);
    if (have < 2)
        return 0.0;
    // The oldest sample still inside the window, measured from the RELEASE (they are pushed in
    // time order, so the walk backward stops at the first stale one).
    const Sample& newest = m_ring[(m_count - 1) % kCap];
    const Sample* oldest = &newest;
    for (std::size_t back = 2; back <= have; ++back) {
        const Sample& s = m_ring[(m_count - back) % kCap];
        if (timeS - s.t > kPresetFlingWindowS)
            break;
        oldest = &s;
    }
    // span == 0 is every release that must be dead, and there are two: a single recent sample, and
    // the HOLD -- the pointer stopped moving before it let go, so even the newest sample is outside
    // the window and nothing older can be in it either. That is the line between "throw the list"
    // and "put it down", and it is what a finger on glass does.
    const double span = newest.t - oldest->t;
    if (span <= 0.0)
        return 0.0;
    return std::clamp((newest.pos - oldest->pos) / span, -kPresetFlingMaxV, kPresetFlingMaxV);
}

namespace {
std::optional<double> g_presetNowForTest;
} // namespace

double presetUiNow() {
    if (g_presetNowForTest)
        return *g_presetNowForTest;
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

void presetUiSetNowForTest(std::optional<double> seconds) {
    g_presetNowForTest = seconds;
}

// ---- Pure name handling ----------------------------------------------------------------------

namespace {
bool isSeparator(unsigned char c) {
    return c == '_' || c == '-' || c == '(' || c == ')' || c == '.' || c == ',' ||
           std::isspace(c) != 0;
}
} // namespace

std::string normalizePresetName(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (isSeparator(c)) {
            if (!out.empty() && out.back() != ' ')
                out.push_back(' ');
            continue;
        }
        out.push_back(static_cast<char>(std::tolower(c))); // ASCII fold; other bytes pass through
    }
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

bool presetMatchesQuery(std::string_view name, std::string_view query) {
    const std::string q = normalizePresetName(query);
    if (q.empty())
        return true;
    const std::string n = normalizePresetName(name);
    std::size_t at = 0;
    while (at < q.size()) {
        const std::size_t end = q.find(' ', at);
        const std::string_view token =
            std::string_view(q).substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (!token.empty() && n.find(token) == std::string::npos)
            return false; // EVERY token must land: "knife wet" finds `i)_Wet_Knife` too
        if (end == std::string::npos)
            break;
        at = end + 1;
    }
    return true;
}

std::vector<int> filterPresetIndices(const std::vector<std::string>& names,
                                     std::string_view query) {
    std::vector<int> out;
    out.reserve(names.size());
    for (std::size_t i = 0; i < names.size(); ++i)
        if (presetMatchesQuery(names[i], query))
            out.push_back(static_cast<int>(i));
    return out;
}

std::string presetDisplayName(std::string_view name) {
    // Drop the corpus's sort prefix ("a)_Eraser_Circle" -> "Eraser Circle") and un-snake the rest.
    // The grid is sorted by the RAW name, so the families stay contiguous without wearing their
    // letter on every tile; the tooltip still carries the exact name, and the search still matches
    // it (normalizePresetName sees both forms the same way).
    std::size_t start = 0;
    if (name.size() > 2 && name[1] == ')')
        start = 2;
    std::string out;
    out.reserve(name.size() - start);
    for (std::size_t i = start; i < name.size(); ++i) {
        const char ch = name[i];
        const char c = (ch == '_') ? ' ' : ch;
        if (c == ' ' && (out.empty() || out.back() == ' '))
            continue;
        out.push_back(c);
    }
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out.empty() ? std::string(name) : out;
}

// ---- The taxonomy ----------------------------------------------------------------------------

PresetGroup presetGroupOf(std::string_view name) noexcept {
    // The corpus's convention is a single letter, a close paren and an underscore: `i)_Wet_Knife`.
    // Anything else is somebody's own brush and answers to no letter.
    if (name.size() < 3 || name[1] != ')' || name[2] != '_')
        return PresetGroup::Other;
    switch (name[0]) {
    case 'a':
        return PresetGroup::Erasers;
    case 'b':
        return PresetGroup::Basics;
    case 'c':
    case 'd':
    case 'e':
        return PresetGroup::Draw;
    case 'f':
    case 'g':
    case 'h':
    case 'j':
        return PresetGroup::Paint;
    case 'i':
    case 'k':
        return PresetGroup::Blend;
    case 'l':
    case 'x':
        return PresetGroup::Effects;
    case 't':
    case 'u':
    case 'v':
    case 'w':
        return PresetGroup::Special;
    case 'y':
    case 'z':
        return PresetGroup::Texture;
    default:
        return PresetGroup::Other; // m) .. s) are unused upstream, left for user brushes
    }
}

std::string presetTabLabel(PresetTab tab) {
    switch (tab) {
    case PresetTab::All:
        return _("All");
    case PresetTab::Basics:
        return _("Basics");
    case PresetTab::Draw:
        return _("Draw");
    case PresetTab::Paint:
        return _("Paint");
    case PresetTab::Blend:
        return _("Blend");
    case PresetTab::Texture:
        return _("Texture");
    case PresetTab::Effects:
        return _("Effects");
    case PresetTab::Special:
        return _("Special");
    case PresetTab::User:
        return _("User");
    }
    return {};
}

bool presetTabAdmits(PresetTab tab, PresetGroup group, bool userInstalled) noexcept {
    switch (tab) {
    case PresetTab::All:
        return true;
    case PresetTab::User:
        return userInstalled;
    case PresetTab::Basics:
        return group == PresetGroup::Basics;
    case PresetTab::Draw:
        return group == PresetGroup::Draw;
    case PresetTab::Paint:
        return group == PresetGroup::Paint;
    case PresetTab::Blend:
        return group == PresetGroup::Blend;
    case PresetTab::Texture:
        return group == PresetGroup::Texture;
    case PresetTab::Effects:
        return group == PresetGroup::Effects;
    case PresetTab::Special:
        // ⚠ Special is also the CATCH-ALL, and that is the point. `Other` is an imported brush that
        // follows no naming convention; `Erasers` reaches here only for a preset named `a)_...` that
        // does NOT actually erase (the corpus split is by `eraserMode`, not by the letter, so the two
        // can disagree and a preset must not fall between them). A preset that is in the library and
        // in NO tab is a preset the user cannot find -- worse than one that is filed oddly.
        return group == PresetGroup::Special || group == PresetGroup::Other ||
               group == PresetGroup::Erasers;
    }
    return false;
}

std::vector<PresetTabCounts> visiblePresetTabs(const std::vector<PresetGroup>& groups,
                                               const std::vector<bool>& userInstalled) {
    constexpr PresetTab kOrder[] = {PresetTab::All,     PresetTab::Basics,  PresetTab::Draw,
                                    PresetTab::Paint,   PresetTab::Blend,   PresetTab::Texture,
                                    PresetTab::Effects, PresetTab::Special, PresetTab::User};
    std::vector<PresetTabCounts> out;
    for (const PresetTab tab : kOrder) {
        int n = 0;
        for (std::size_t i = 0; i < groups.size(); ++i) {
            const bool mine = i < userInstalled.size() && userInstalled[i];
            if (presetTabAdmits(tab, groups[i], mine))
                ++n;
        }
        // An empty tab is a dead affordance -- and the User tab in particular must not advertise a
        // room the user has never put anything in. `All` is the exception only in that it cannot be
        // empty while anything else is not.
        if (n > 0)
            out.push_back({tab, n});
    }
    return out;
}

// ---- Pure thumbnail scaling ------------------------------------------------------------------

common::Image presetThumbnail(const common::Image& src, int box, common::Color8 ground) {
    if (box < 1)
        box = 1;
    common::Image out(static_cast<std::uint32_t>(box), static_cast<std::uint32_t>(box));
    out.fill(ground);
    if (src.empty())
        return out;

    const double scale = std::min(static_cast<double>(box) / src.width,
                                  static_cast<double>(box) / src.height);
    const int dw = std::clamp(static_cast<int>(std::lround(src.width * scale)), 1, box);
    const int dh = std::clamp(static_cast<int>(std::lround(src.height * scale)), 1, box);
    const int ox = (box - dw) / 2;
    const int oy = (box - dh) / 2;

    for (int dy = 0; dy < dh; ++dy) {
        const auto y0 = static_cast<std::uint32_t>(static_cast<double>(dy) * src.height / dh);
        auto y1 = static_cast<std::uint32_t>(static_cast<double>(dy + 1) * src.height / dh);
        y1 = std::clamp(y1, y0 + 1, src.height);
        for (int dx = 0; dx < dw; ++dx) {
            const auto x0 = static_cast<std::uint32_t>(static_cast<double>(dx) * src.width / dw);
            auto x1 = static_cast<std::uint32_t>(static_cast<double>(dx + 1) * src.width / dw);
            x1 = std::clamp(x1, x0 + 1, src.width);

            // Box filter over the source footprint, in PREMULTIPLIED space (averaging straight
            // colour across a transparent edge drags the transparent pixels' colour into the mix).
            double pr = 0.0, pg = 0.0, pb = 0.0, pa = 0.0;
            int n = 0;
            for (std::uint32_t sy = y0; sy < y1; ++sy) {
                const std::uint8_t* row = &src.rgba[(static_cast<std::size_t>(sy) * src.width) * 4];
                for (std::uint32_t sx = x0; sx < x1; ++sx) {
                    const std::uint8_t* p = row + static_cast<std::size_t>(sx) * 4;
                    const double a = p[3] / 255.0;
                    pr += p[0] * a;
                    pg += p[1] * a;
                    pb += p[2] * a;
                    pa += a;
                    ++n;
                }
            }
            if (n == 0)
                continue;
            const double inv = 1.0 / n;
            const double a = pa * inv;
            const auto over = [&](double premul, std::uint8_t under) {
                return static_cast<std::uint8_t>(
                    std::lround(std::clamp(premul * inv + under * (1.0 - a), 0.0, 255.0)));
            };
            std::uint8_t* d =
                &out.rgba[((static_cast<std::size_t>(oy + dy) * box) + (ox + dx)) * 4];
            d[0] = over(pr, ground.r);
            d[1] = over(pg, ground.g);
            d[2] = over(pb, ground.b);
            d[3] = 255;
        }
    }
    return out;
}

// ---- The search field ------------------------------------------------------------------------

// A themed Fl_Input with a placeholder. Fl_Input has none, and an unlabelled box above a grid is a
// mystery; the whole reason this panel beats the combobox it replaces is that you can TYPE at it.
// Escape clears the filter, Enter picks the first match (a search you have to reach for the mouse to
// finish is half a search).
class PresetSearchInput : public TextInput {
public:
    explicit PresetSearchInput(BrushPresetPanel* panel) : TextInput(0, 0, 10, 10), m_panel(panel) {
        box(MOSAIC_INPUT_BOX);
        textsize(12);
        when(FL_WHEN_CHANGED);
        applyStyle();
        // NOTE: do NOT clear_visible_focus() -- despite the name it makes take_focus() bail out, and
        // the field would never receive the keyboard (the RenameInput lesson, layer_panel.cpp).
    }

    // Fl_Input's stock colours are FLTK's, not ours -- an unstyled field is a white slab in a dark
    // dock. Cached, so a runtime re-theme has to re-apply them (the ThemeSubscription contract).
    void applyStyle() {
        const Palette& pal = activePalette();
        color(toFl(pal.controlBg));
        textcolor(toFl(pal.text));
        cursor_color(toFl(pal.text));
        selection_color(toFl(pal.accent));
    }

protected:
    void draw() override {
        TextInput::draw(); // erases the field and paints the value + cursor
        if (size() != 0 || Fl::focus() == this)
            return;
        const Palette& pal = activePalette();
        fl_font(textfont(), textsize());
        fl_color(toFl(pal.textMuted));
        fl_draw(_("Search presets"), x() + Fl::box_dx(box()) + 2, y(), w(), h(),
                FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    }

    int handle(int event) override {
        if (event == FL_KEYBOARD) {
            const int key = Fl::event_key();
            if (key == FL_Escape && size() != 0) {
                m_panel->setFilter(std::string());
                return 1;
            }
            if (key == FL_Enter || key == FL_KP_Enter) {
                const std::vector<int>& slots = m_panel->grid()->slots();
                if (!slots.empty())
                    m_panel->pick(slots.front());
                return 1;
            }
        }
        return TextInput::handle(event);
    }

private:
    BrushPresetPanel* m_panel;
};

namespace {
void cbSearch(Fl_Widget*, void* p) {
    static_cast<BrushPresetPanel*>(p)->onFilterEdited();
}
void cbClear(Fl_Widget*, void* p) {
    static_cast<BrushPresetPanel*>(p)->setFilter(std::string());
}
void cbEdit(Fl_Widget*, void* p) {
    auto* panel = static_cast<BrushPresetPanel*>(p);
    panel->requestEdit(panel->selected());
}
} // namespace

// ---- PresetTabStrip --------------------------------------------------------------------------

PresetTabStrip::PresetTabStrip(int X, int Y, int W, int H, BrushPresetPanel* panel)
    : Fl_Widget(X, Y, W, H), m_panel(panel) {}

PresetTabStrip::~PresetTabStrip() {
    Fl::remove_timeout(cbFling, this); // the timer must not fire into a freed widget
}

void PresetTabStrip::setTabs(std::vector<PresetTabCounts> tabs) {
    m_tabs = std::move(tabs);
    m_hover = -1;
    m_scroll = 0;
    stopFling(); // the scroll was just reset under it; a stale fling would scroll a new corpus
    // The active tab may not exist in the new corpus (switch to the Eraser and "Texture" is gone).
    // Falling back to All is the only answer that cannot show an empty grid.
    const bool stillThere = std::any_of(m_tabs.begin(), m_tabs.end(),
                                        [&](const PresetTabCounts& t) { return t.tab == m_active; });
    if (!stillThere)
        m_active = PresetTab::All;
    redraw();
}

void PresetTabStrip::setActive(PresetTab tab) {
    m_active = tab;
    scrollActiveIntoView();
    redraw();
}

PresetCellRect PresetTabStrip::tabRect(std::size_t i) const {
    PresetCellRect r;
    if (i >= m_tabs.size())
        return r;
    fl_font(FL_HELVETICA, kTabFontSize);
    int x = 0;
    for (std::size_t k = 0; k <= i; ++k) {
        const std::string label = presetTabLabel(m_tabs[k].tab);
        const int w = static_cast<int>(std::ceil(fl_width(label.c_str()))) + 2 * kTabPadX;
        if (k == i) {
            r.x = x;
            r.w = w;
            r.y = 0;
            r.h = kTabH;
            return r;
        }
        x += w + kTabGap;
    }
    return r;
}

int PresetTabStrip::contentWidth() const {
    if (m_tabs.empty())
        return 0;
    const PresetCellRect last = tabRect(m_tabs.size() - 1);
    return last.x + last.w;
}

int PresetTabStrip::maxScroll() const {
    return std::max(0, contentWidth() - w());
}

void PresetTabStrip::scrollBy(int dx) {
    const int next = std::clamp(m_scroll + dx, 0, maxScroll());
    if (next != m_scroll) {
        m_scroll = next;
        redraw();
    }
}

void PresetTabStrip::startFling(double velocity) {
    if (presetFlingDead(velocity) || maxScroll() <= 0)
        return;
    m_flingV = velocity;
    m_flingPos = m_scroll;
    if (!m_flinging) {
        m_flinging = true;
        Fl::add_timeout(kFlingTickS, cbFling, this);
    }
}

void PresetTabStrip::stopFling() {
    if (!m_flinging)
        return;
    m_flinging = false;
    Fl::remove_timeout(cbFling, this);
}

void PresetTabStrip::cbFling(void* self) {
    auto* strip = static_cast<PresetTabStrip*>(self);
    // A fixed dt, not a measured one: the step is analytic (see presetFlingStep), so a late tick
    // only delays the fling -- and a fixed step is one the tests can replay exactly.
    strip->flingTick(kFlingTickS);
    if (strip->m_flinging)
        Fl::repeat_timeout(kFlingTickS, cbFling, self);
}

void PresetTabStrip::flingTick(double dt) {
    if (!m_flinging)
        return;
    const PresetFlingStep step = presetFlingStep(m_flingV, dt);
    m_flingV = step.velocity;
    m_flingPos += step.dx;
    const int max = maxScroll();
    // Ran off an end: there is nothing left to move, so the fling folds rather than integrating an
    // invisible overshoot it would have to play back.
    const bool hitEdge = m_flingPos <= 0.0 || m_flingPos >= max;
    const int next = std::clamp(static_cast<int>(std::lround(m_flingPos)), 0, max);
    if (next != m_scroll) {
        m_scroll = next;
        redraw();
    }
    if (hitEdge || presetFlingDead(m_flingV))
        stopFling();
}

void PresetTabStrip::scrollActiveIntoView() {
    for (std::size_t i = 0; i < m_tabs.size(); ++i) {
        if (m_tabs[i].tab != m_active)
            continue;
        const PresetCellRect r = tabRect(i);
        if (r.x < m_scroll)
            m_scroll = r.x;
        else if (r.x + r.w > m_scroll + w())
            m_scroll = r.x + r.w - w();
        m_scroll = std::clamp(m_scroll, 0, maxScroll());
        return;
    }
}

std::optional<PresetTab> PresetTabStrip::tabAt(int localX) const {
    const int contentX = localX + m_scroll;
    for (std::size_t i = 0; i < m_tabs.size(); ++i) {
        const PresetCellRect r = tabRect(i);
        if (contentX >= r.x && contentX < r.x + r.w)
            return m_tabs[i].tab;
    }
    return std::nullopt; // the gaps between tabs belong to nobody
}

void PresetTabStrip::draw() {
    const Palette& pal = activePalette();
    fl_color(toFl(pal.panelBg));
    fl_rectf(x(), y(), w(), h()); // ⚠ erase the whole widget: draw() must own every pixel it claims

    fl_push_clip(x(), y(), w(), h());
    fl_font(FL_HELVETICA, kTabFontSize);
    for (std::size_t i = 0; i < m_tabs.size(); ++i) {
        const PresetCellRect r = tabRect(i);
        const int tx = x() + r.x - m_scroll;
        if (tx + r.w < x() || tx > x() + w())
            continue; // off-strip: arithmetic, not a clip test
        const bool active = m_tabs[i].tab == m_active;
        const bool hot = static_cast<int>(i) == m_hover;

        if (active) {
            fl_color(toFl(pal.accent));
            fl_rectf(tx, y() + r.y, r.w, r.h);
        } else if (hot) {
            fl_color(toFl(pal.controlBg));
            fl_rectf(tx, y() + r.y, r.w, r.h);
        }
        fl_color(toFl(active ? pal.onAccent : pal.textMuted));
        const std::string label = presetTabLabel(m_tabs[i].tab);
        fl_draw(label.c_str(), tx, y() + r.y, r.w, r.h, FL_ALIGN_CENTER);
    }

    // The scroll affordance, and it appears exactly when it is TRUE: a small arrow at whichever edge
    // still has tabs beyond it. Deliberately NOT a scrollbar (a horizontal bar under a 22 px strip is
    // more chrome than content), and deliberately not a glyph in a label -- FLTK's label drawing has
    // no Unicode here, which is a trap this codebase has already fallen into once.
    const auto arrow = [&](int cx, bool pointsRight) {
        // Cover the tab it overlaps, so the arrow is never drawn on top of a half-cut word.
        fl_color(toFl(pal.panelBg));
        fl_rectf(cx - kTabFadeW / 2, y(), kTabFadeW, h());
        fl_color(toFl(pal.textMuted));
        const int mid = y() + h() / 2;
        const int dx = pointsRight ? 3 : -3;
        fl_begin_polygon();
        fl_vertex(cx - dx, mid - 4);
        fl_vertex(cx + dx, mid);
        fl_vertex(cx - dx, mid + 4);
        fl_end_polygon();
    };
    if (m_scroll > 0)
        arrow(x() + kTabFadeW / 2, /*pointsRight=*/false);
    if (m_scroll < maxScroll())
        arrow(x() + w() - kTabFadeW / 2, /*pointsRight=*/true);
    fl_pop_clip();
}

int PresetTabStrip::handle(int event) {
    switch (event) {
    case FL_ENTER:
        return 1;
    case FL_LEAVE:
        if (m_hover != -1) {
            m_hover = -1;
            redraw();
        }
        return 1;
    case FL_MOVE: {
        int hit = -1;
        const int lx = Fl::event_x() - x() + m_scroll;
        for (std::size_t i = 0; i < m_tabs.size(); ++i) {
            const PresetCellRect r = tabRect(i);
            if (lx >= r.x && lx < r.x + r.w) {
                hit = static_cast<int>(i);
                break;
            }
        }
        if (hit != m_hover) {
            m_hover = hit;
            redraw();
        }
        return 1;
    }
    case FL_PUSH:
        stopFling(); // a touch catches the strip mid-flight, exactly like a finger on glass
        m_dragFrom = Fl::event_x();
        m_dragScroll = m_scroll;
        m_dragged = false;
        m_flingTracker.reset();
        m_flingTracker.push(presetUiNow(), Fl::event_x());
        return 1;
    case FL_DRAG:
        if (m_dragFrom >= 0) {
            m_flingTracker.push(presetUiNow(), Fl::event_x());
            const int travel = Fl::event_x() - m_dragFrom;
            if (std::abs(travel) >= kDragSlop)
                m_dragged = true; // ⚠ a drag that MOVED is a scroll, and must not also pick the tab
            if (m_dragged) {
                m_scroll = std::clamp(m_dragScroll - travel, 0, maxScroll());
                redraw();
            }
        }
        return 1;
    case FL_RELEASE: {
        const bool wasDrag = m_dragged;
        m_dragFrom = -1;
        m_dragged = false;
        if (wasDrag) {
            // The content moves AGAINST the pointer (scroll = start - travel), so its velocity is
            // the pointer's, negated.
            startFling(-m_flingTracker.releaseVelocity(presetUiNow()));
            return 1;
        }
        if (const std::optional<PresetTab> hit = tabAt(Fl::event_x() - x()); hit && m_panel != nullptr)
            m_panel->pickTab(*hit);
        return 1;
    }
    case FL_MOUSEWHEEL:
        // A vertical wheel over a horizontal strip scrolls it sideways -- most mice have no
        // horizontal wheel, and a wheel that did nothing here would just look broken.
        if (maxScroll() > 0) {
            stopFling(); // the wheel took the strip over; the fling must not fight it for position
            scrollBy((Fl::event_dy() + Fl::event_dx()) * 24);
            return 1;
        }
        return 0; // nothing to scroll: let the grid below have it
    default:
        break;
    }
    return Fl_Widget::handle(event);
}

// ---- PresetGrid ------------------------------------------------------------------------------

PresetGrid::PresetGrid(int X, int Y, int W, int H, BrushPresetPanel* panel)
    : Fl_Widget(X, Y, W, H), m_panel(panel) {}

PresetGrid::~PresetGrid() {
    Fl::remove_timeout(cbFling, this); // the timer must not fire into a freed widget
}

void PresetGrid::setSlots(std::vector<int> slots) {
    m_slots = std::move(slots);
    m_hover = -1;
    stopFling(); // the panel resets the scroll for a new result set; a stale fling would undo it
    redraw();
}

// ---- Drag-to-scroll + fling (the tab strip's mechanic, turned vertical) ------------------------

int PresetGrid::listScrollMax() const {
    const ScrollView* sv = m_panel != nullptr ? m_panel->scroll() : nullptr;
    if (sv == nullptr)
        return 0;
    return std::max(0, h() - sv->h()); // the grid IS the content: its height is the content height
}

void PresetGrid::scrollListTo(int target) {
    ScrollView* sv = m_panel != nullptr ? m_panel->scroll() : nullptr;
    if (sv == nullptr)
        return;
    const int next = std::clamp(target, 0, listScrollMax());
    if (next != sv->yposition())
        sv->scroll_to(0, next);
}

void PresetGrid::startFling(double velocity) {
    if (presetFlingDead(velocity) || listScrollMax() <= 0)
        return;
    const ScrollView* sv = m_panel != nullptr ? m_panel->scroll() : nullptr;
    m_flingV = velocity;
    m_flingPos = sv != nullptr ? sv->yposition() : 0;
    if (!m_flinging) {
        m_flinging = true;
        Fl::add_timeout(kFlingTickS, cbFling, this);
    }
}

void PresetGrid::stopFling() {
    if (!m_flinging)
        return;
    m_flinging = false;
    Fl::remove_timeout(cbFling, this);
}

void PresetGrid::cbFling(void* self) {
    auto* grid = static_cast<PresetGrid*>(self);
    // A fixed dt, not a measured one: the step is analytic (see presetFlingStep), so a late tick
    // only delays the fling -- and a fixed step is one the tests can replay exactly.
    grid->flingTick(kFlingTickS);
    if (grid->m_flinging)
        Fl::repeat_timeout(kFlingTickS, cbFling, self);
}

void PresetGrid::flingTick(double dt) {
    if (!m_flinging)
        return;
    const ScrollView* sv = m_panel != nullptr ? m_panel->scroll() : nullptr;
    if (sv == nullptr) {
        stopFling();
        return;
    }
    // Someone else moved the list mid-flight (the scrollbar's own drag): their scroll wins, and the
    // fling folds rather than fighting them for the position every tick.
    if (std::abs(sv->yposition() - static_cast<int>(std::lround(m_flingPos))) > 1) {
        stopFling();
        return;
    }
    const PresetFlingStep step = presetFlingStep(m_flingV, dt);
    m_flingV = step.velocity;
    m_flingPos += step.dx;
    const int max = listScrollMax();
    const bool hitEdge = m_flingPos <= 0.0 || m_flingPos >= max;
    scrollListTo(static_cast<int>(std::lround(m_flingPos)));
    if (hitEdge || presetFlingDead(m_flingV))
        stopFling();
}

int PresetGrid::slotOf(int presetIndex) const {
    for (std::size_t i = 0; i < m_slots.size(); ++i)
        if (m_slots[i] == presetIndex)
            return static_cast<int>(i);
    return -1;
}

void PresetGrid::draw() {
    const Palette& pal = activePalette();
    // ⚠ Erase the WHOLE widget. A child of an Fl_Scroll that leaves any of its own rect unpainted
    // shows the content that scrolled past it -- FLTK never clears behind a child.
    fl_color(toFl(pal.panelBg));
    fl_rectf(x(), y(), w(), h());

    for (int slot = 0; slot < static_cast<int>(m_slots.size()); ++slot) {
        const PresetCellRect r = presetCellRect(slot, m_metrics, x(), y());
        // ⚠ THE LAZY-DECODE GATE. Only a cell that is actually on screen draws, and only a cell that
        // draws asks the panel for its thumbnail -- which is the ONE place an icon is ever decoded.
        if (!fl_not_clipped(r.x, r.y, r.w, r.h))
            continue;
        drawCell(slot, r);
    }
}

void PresetGrid::drawCell(int slot, const PresetCellRect& r) {
    const Palette& pal = activePalette();
    const int index = m_slots[static_cast<std::size_t>(slot)];
    const bool selected = index == m_panel->selected();
    const bool hovered = slot == m_hover;

    // ⚠ NEITHER state fills the cell any more. Selection used to lay a controlActive slab under the
    // whole row -- the very slab the hover fill was thrown out for -- so both states are RINGS now:
    // 2 px of accent means "selected", 2 px of the pressed-control grey means "under the cursor",
    // and the two differ by ink alone. The dense grid keeps its HOVER fill (a small tile tinting is
    // a highlight, not a slab); its selection is the ring, same as the cards'.
    if (!selected && hovered) {
        if (m_metrics.mode == PresetDisplayMode::Cards) {
            fl_color(toFl(pal.controlActive));
            fl_rect(r.x, r.y, r.w, r.h);
            fl_rect(r.x + 1, r.y + 1, r.w - 2, r.h - 2);
        } else {
            fl_color(toFl(pal.controlHover));
            fl_rectf(r.x, r.y, r.w, r.h);
        }
    }

    const PresetCellRect tr = presetThumbRect(r, m_metrics);
    const int t = tr.w;
    const int tx = tr.x;
    const int ty = tr.y;

    const common::Image* pixels = m_panel->thumbnailPixels(index);
    if (Fl_RGB_Image* img = m_panel->thumbnailFor(index); img != nullptr) {
        img->draw(tx, ty);
    } else if (index < 0) {
        // "Default round" = NO preset: the engine's own analytic circle. Draw exactly that -- a soft
        // round dab -- rather than borrow another brush's picture for it.
        const auto ground = [&](int, int) { return pal.panelBg; };
        const double cx = tx + t / 2.0;
        const double cy = ty + t / 2.0;
        drawAAPrims(tx, ty, t, t, ground,
                    {{cx, cy, t * 0.30, 0.0, pal.textMuted},
                     {cx, cy, t * 0.30 + 2.0, 1.0, pal.textMuted}});
    } else {
        // A preset whose icon would not decode still gets a cell: an empty hole in the grid reads as
        // a bug in the panel, not as a fault in one .kpp. A hollow ring, so it is not mistaken for
        // the round tip above.
        const auto ground = [&](int, int) { return pal.panelBg; };
        const double cx = tx + t / 2.0;
        const double cy = ty + t / 2.0;
        drawAAPrims(tx, ty, t, t, ground, {{cx, cy, t * 0.26, 1.5, pal.border}});
    }

    // TWO BADGES, TWO CORNERS. The fidelity badge (§6.4) is a DOT in the thumbnail's bottom-right
    // corner -- a dot and not a letter, because 106 of the 117 shipped presets are Approximated or
    // Substituted and a glyph on nearly every tile would be noise. The USER badge is a RING in the
    // top-left. Both blit BEFORE the framing hairline; see the note where the frame is drawn.
    if (index >= 0) {
        // `under` reads the thumbnail's own pixels, so a badge's anti-aliased rim blends into the
        // icon it sits on instead of ringing itself with the panel colour. Shared by both badges.
        const auto under = [&](int px, int py) -> common::Color8 {
            if (pixels == nullptr || pixels->empty())
                return pal.panelBg;
            const int lx = std::clamp(px - tx, 0, static_cast<int>(pixels->width) - 1);
            const int ly = std::clamp(py - ty, 0, static_cast<int>(pixels->height) - 1);
            const std::uint8_t* q =
                &pixels->rgba[((static_cast<std::size_t>(ly) * pixels->width) + lx) * 4];
            return {q[0], q[1], q[2], 255};
        };
        if (const io::brush::LibraryPreset* p = m_panel->presetAt(index); p != nullptr) {
            const io::brush::PresetFidelity f = p->preset.provenance.fidelity;
            if (f != io::brush::PresetFidelity::Exact) {
                const bool subst = f == io::brush::PresetFidelity::Substituted;
                const double bcx = tx + t - 7.0;
                const double bcy = ty + t - 7.0;
                // Only the dot's own patch is rendered + blitted -- not the whole thumbnail square.
                drawAAPrims(static_cast<int>(bcx) - 6, static_cast<int>(bcy) - 6, 13, 13, under,
                            {{bcx, bcy, 5.0, 0.0, pal.panelBg}, // a quiet backing disc
                             {bcx, bcy, 3.5, 0.0, subst ? kSubstInk : kApproxInk}});
            }
        }
        // ⭐ THE USER BADGE (feedback round 1): "this one is yours". It has to be told apart from the
        // fidelity dot at a glance, so it differs on all three axes a 13 px mark has -- the OPPOSITE
        // corner (top-left, where the fidelity dot can never be), the ACCENT ink (never amber or
        // red, which mean "something was lost"), and a RING rather than a filled dot. Colour alone
        // would not have been enough: the two would read as one badge in two moods.
        if (m_panel->userInstalled(index)) {
            const double ucx = tx + 7.0;
            const double ucy = ty + 7.0;
            drawAAPrims(static_cast<int>(ucx) - 6, static_cast<int>(ucy) - 6, 13, 13, under,
                        {{ucx, ucy, 5.0, 0.0, pal.panelBg}, // the same quiet backing disc
                         {ucx, ucy, 3.6, 1.8, pal.accent}}); // ... under a RING, not a disc
        }
    }

    // ⚠ THE FRAME IS PAINTED LAST, AND THE ORDER IS LOAD-BEARING (962053b). Both badges are OPAQUE
    // 13x13 blits whose patches reach the thumbnail's first/last row and column -- exactly where the
    // hairline runs -- and their ground is the thumbnail's PIXELS, which know nothing of a frame
    // painted over them. A badge drawn after the frame re-lays the icon across the frame's corner
    // and bites ~12 px out of two edges. The frame is the outermost thing in the square, so it is
    // painted like one. (The user badge doubled the number of corners this can go wrong in.)
    fl_color(toFl(pal.border)); // a hairline, so a pale icon does not float on the panel ground
    fl_rect(tx, ty, t, t);

    if (selected) { // the selection ring: 2 px of accent around the whole cell
        fl_color(toFl(pal.accent));
        fl_rect(r.x, r.y, r.w, r.h);
        fl_rect(r.x + 1, r.y + 1, r.w - 2, r.h - 2);
    }

    if (m_metrics.mode == PresetDisplayMode::Cards) {
        drawCardStroke(index, r);
        return;
    }

    fl_font(FL_HELVETICA, 11);
    fl_color(toFl(selected ? pal.text : pal.textMuted));
    // ⚠ fl_draw does NOT clip to the box it is given: a long name would run straight over the next
    // cell. Ellipsize first, always.
    const std::string label =
        ellipsizeToWidth(presetDisplayName(m_panel->nameAt(index)), r.w - 4);
    fl_draw(label.c_str(), r.x + 2, ty + t + kLabelGap, r.w - 4, kLabelH, FL_ALIGN_CENTER);
}

// The card's long rectangle: the NAME, and under it a stroke of this very brush, laid by the real
// engine (core/brush/stroke_preview.hpp). This is the mode's whole reason to exist -- a tip icon
// tells you what a brush's PICTURE looks like, which is a question nobody has; a stroke tells you
// what MARK it makes, which is the only question anyone has.
void PresetGrid::drawCardStroke(int index, const PresetCellRect& r) {
    const Palette& pal = activePalette();
    const bool selected = index == m_panel->selected();
    const PresetCellRect sr = presetStrokeRect(r, m_metrics);
    if (sr.w <= 0 || sr.h <= 0)
        return;

    fl_font(FL_HELVETICA, 11);
    fl_color(toFl(selected ? pal.text : pal.textMuted));
    const std::string label = ellipsizeToWidth(presetDisplayName(m_panel->nameAt(index)), sr.w);
    fl_draw(label.c_str(), sr.x, r.y + kCellInset, sr.w, kCardNameH, FL_ALIGN_LEFT);

    // ⚠ RENDERED WIDE, DRAWN CROPPED -- never scaled. The strip is rendered at the next 32 px bucket
    // up (so a width drag re-renders once per bucket instead of once per pixel, at ~1.7 ms a card)
    // and the middle of it is blitted into the strip. Scaling it to fit would stretch the stroke and
    // make the brush lie about its own size, which is the one thing the card exists to be honest
    // about.
    const int renderW = presetStrokeRenderWidth(sr.w);
    if (Fl_RGB_Image* img = m_panel->strokePreviewFor(index, renderW, sr.h); img != nullptr) {
        const int cx = (renderW - sr.w) / 2;
        // The preview carries ALPHA where an eraser carved the paper away, so it composites over the
        // cell rather than replacing it -- which is exactly how an eraser's card should read.
        img->draw(sr.x, sr.y, sr.w, sr.h, cx, 0);
    }

    fl_color(toFl(pal.border)); // the strip is a little canvas: frame it like the tip icon
    fl_rect(sr.x, sr.y, sr.w, sr.h);
}

void PresetGrid::setHover(int slot) {
    if (slot == m_hover)
        return;
    m_hover = slot;
    if (slot < 0) {
        tooltip(nullptr);
    } else {
        const std::string tip = m_panel->tooltipFor(m_slots[static_cast<std::size_t>(slot)]);
        copy_tooltip(tip.c_str());
    }
    redraw();
}

void PresetGrid::moveCursor(int delta) {
    if (m_slots.empty())
        return;
    const int cur = slotOf(m_panel->selected());
    // Nothing selected (or the selection is filtered out): the first arrow lands on the first cell.
    const int next = cur < 0 ? 0
                             : std::clamp(cur + delta, 0, static_cast<int>(m_slots.size()) - 1);
    m_panel->pick(m_slots[static_cast<std::size_t>(next)]);
}

int PresetGrid::handle(int event) {
    switch (event) {
    case FL_ENTER:
        return 1;
    case FL_MOVE:
        setHover(presetSlotAt(Fl::event_x() - x(), Fl::event_y() - y(),
                              static_cast<int>(m_slots.size()), m_metrics));
        return 1;
    case FL_LEAVE:
        setHover(-1);
        return 1;
    case FL_FOCUS:
    case FL_UNFOCUS:
        return 1; // take the keyboard so the arrows walk the grid instead of scrolling it
    case FL_PUSH: {
        if (Fl::event_button() != FL_LEFT_MOUSE)
            return 1;
        take_focus();
        stopFling(); // a touch catches the list mid-flight, exactly like a finger on glass
        // ⚠ The pick moved to FL_RELEASE: a press is not yet a click, because a press that MOVES is
        // a drag-scroll of the list (the tab strip's rule, turned vertical) and must not also pick
        // the cell it happened to start over.
        m_pressX = Fl::event_x();
        m_pressY = Fl::event_y();
        m_pressScroll = m_panel->scroll() != nullptr ? m_panel->scroll()->yposition() : 0;
        m_dragged = false;
        m_pressDouble = Fl::event_clicks() > 0;
        m_flingTracker.reset();
        m_flingTracker.push(presetUiNow(), Fl::event_y());
        return 1;
    }
    case FL_DRAG:
        if (m_pressY >= 0) {
            m_flingTracker.push(presetUiNow(), Fl::event_y());
            const int travel = Fl::event_y() - m_pressY;
            if (std::abs(travel) >= kDragSlop)
                m_dragged = true;
            if (m_dragged) {
                setHover(-1); // the content is moving; a highlight chasing it under a still
                              // cursor would be noise
                scrollListTo(m_pressScroll - travel);
            }
        }
        return 1;
    case FL_RELEASE: {
        const bool wasPress = m_pressY >= 0;
        const bool wasDrag = m_dragged;
        const bool wasDouble = m_pressDouble;
        const int pressX = m_pressX;
        const int pressY = m_pressY;
        m_pressX = -1;
        m_pressY = -1;
        m_dragged = false;
        m_pressDouble = false;
        if (!wasPress)
            return 1;
        if (wasDrag) {
            // The content moves AGAINST the pointer (scroll = start - travel), so its velocity is
            // the pointer's, negated. A release that had STOPPED moving flings nothing.
            startFling(-m_flingTracker.releaseVelocity(presetUiNow()));
            return 1;
        }
        // The press coords, not the release's: inside the slop the two differ by a couple of px,
        // and the cell you pressed is the cell you meant.
        const int slot = presetSlotAt(pressX - x(), pressY - y(),
                                      static_cast<int>(m_slots.size()), m_metrics);
        if (slot >= 0) {
            const int index = m_slots[static_cast<std::size_t>(slot)];
            // A double-click SELECTS FIRST and then opens the editor: the second click's cell is
            // the one you meant to edit, and the editor is seeded from a selection, not from a
            // click. (Index -1 -- Default round -- selects and stops; there is nothing to edit.)
            m_panel->pick(index);
            if (wasDouble)
                m_panel->requestEdit(index);
        }
        return 1;
    }
    case FL_MOUSEWHEEL:
        stopFling(); // the wheel took the list over; the fling must not fight it for position
        break;       // ... and the wheel itself falls through to the ScrollView, as before
    case FL_KEYBOARD:
        switch (Fl::event_key()) {
        case FL_Left:
            moveCursor(-1);
            return 1;
        case FL_Right:
            moveCursor(+1);
            return 1;
        case FL_Up:
            moveCursor(-std::max(1, m_metrics.cols));
            return 1;
        case FL_Down:
            moveCursor(+std::max(1, m_metrics.cols));
            return 1;
        case FL_Home:
            moveCursor(-static_cast<int>(m_slots.size()));
            return 1;
        case FL_End:
            moveCursor(+static_cast<int>(m_slots.size()));
            return 1;
        default:
            break;
        }
        break;
    default:
        break;
    }
    return Fl_Widget::handle(event); // FL_MOUSEWHEEL falls through to the ScrollView, as it must
}

// ---- BrushPresetPanel ------------------------------------------------------------------------

BrushPresetPanel::BrushPresetPanel(int X, int Y, int W, int H) : Panel(X, Y, W, H) {
    borderEdges(EdgeLeft); // this section's slice of the canvas|dock junction (the dock's splitter
                           // strip owns the hairline above us)
    const Palette& pal = activePalette();
    begin();

    m_search = new PresetSearchInput(this);
    m_search->callback(cbSearch, this);
    m_search->tooltip(_("Filter the presets by name"));

    m_clearButton = new IconButton(0, 0, kClearW, kSearchH, Icon::Close);
    m_clearButton->callback(cbClear, this);
    m_clearButton->tooltip(_("Clear the filter"));
    m_clearButton->hide(); // only while there IS a filter

    // The modal editor's launch point (§8.3). A button and not a menu item: the menu tree is
    // all-or-nothing across the catalogs, and a control that lives beside the thing it edits is
    // where a user looks for it anyway. A double-click on a card opens the same dialog.
    m_editButton = new FlatButton(0, 0, kEditW, kSearchH, _("Edit…"));
    m_editButton->labelsize(11);
    m_editButton->callback(cbEdit, this);
    m_editButton->tooltip(_("Edit the selected brush preset"));
    m_editButton->deactivate(); // nothing is selected yet, and Default round is not editable

    m_tabs = new PresetTabStrip(0, 0, 10, kTabH, this);
    m_tabs->tooltip(_("Filter by kind. Drag or scroll the strip to see them all."));

    m_scroll = new ScrollView(0, 0, 10, 10);
    m_scroll->type(Fl_Scroll::VERTICAL);
    m_scroll->box(FL_NO_BOX);
    m_scroll->color(toFl(pal.panelBg));
    m_scroll->begin();
    m_grid = new PresetGrid(0, 0, 10, 10, this);
    m_scroll->end();

    end();
    resizable(nullptr); // layoutChildren() places every child; see resize()
    rebuildSlots();
}

void BrushPresetPanel::setStore(const BrushPresetStore* store) {
    m_store = store;
    m_names = store != nullptr ? store->names() : std::vector<std::string>{};
    m_icons.clear(); // a new store re-numbers the presets: both caches are keyed by INDEX
    m_thumbs.clear();

    // The taxonomy is derived ONCE per store, not per keystroke: it is a property of the library.
    m_groups.clear();
    m_userInstalled.clear();
    m_groups.reserve(m_names.size());
    m_userInstalled.reserve(m_names.size());
    for (std::size_t i = 0; i < m_names.size(); ++i) {
        m_groups.push_back(presetGroupOf(m_names[i]));
        m_userInstalled.push_back(store != nullptr && store->isUserPreset(static_cast<int>(i)));
    }

    rebuildTabs();
    rebuildSlots();
}

bool BrushPresetPanel::inCorpus(int index) const {
    if (index < 0)
        return true; // "Default round" = NO preset, and BOTH tools have a plain round nib (§8.4)
    const io::brush::LibraryPreset* p = presetAt(index);
    if (p == nullptr)
        return false;
    // ⚠ SEMANTIC, not by name. A preset erases because it carries CompositeOp=erase, which the mapper
    // already turns into StrokeMode::Erase. The `a)_` prefix is a filing convention that HAPPENS to
    // agree on the three shipped ones; it is not the fact.
    const bool erases = p->preset.eraserMode;
    return (m_corpus == PresetCorpus::Eraser) == erases;
}

void BrushPresetPanel::rebuildTabs() {
    if (m_tabs == nullptr)
        return;
    // Only the presets the CURRENT corpus admits get a vote on which tabs exist -- otherwise the
    // Eraser would sprout a "Texture" tab with nothing behind it.
    std::vector<PresetGroup> groups;
    std::vector<bool> mine;
    for (std::size_t i = 0; i < m_groups.size(); ++i) {
        if (!inCorpus(static_cast<int>(i)))
            continue;
        groups.push_back(m_groups[i]);
        mine.push_back(i < m_userInstalled.size() && m_userInstalled[i]);
    }
    m_tabs->setTabs(visiblePresetTabs(groups, mine));
    m_tab = m_tabs->active(); // setTabs may have fallen back to All
}

void BrushPresetPanel::setCorpus(PresetCorpus corpus) {
    if (m_corpus == corpus)
        return;
    m_corpus = corpus;
    // ⚠ Index -1 is the ONE cache key whose MEANING depends on the corpus: the Brush's Default round
    // paints and the Eraser's carves. Both caches are keyed by index alone, so the two would hand each
    // other the wrong picture -- an eraser card showing a stroke of paint. Evict just that key; every
    // other index means the same preset in either corpus.
    m_params.erase(-1);
    m_strokes.erase(-1);
    rebuildTabs();
    rebuildSlots();
}

void BrushPresetPanel::setTab(PresetTab tab) {
    if (m_tabs != nullptr)
        m_tabs->setActive(tab);
    m_tab = m_tabs != nullptr ? m_tabs->active() : tab;
    rebuildSlots();
}

void BrushPresetPanel::pickTab(PresetTab tab) {
    setTab(tab);
}

const io::brush::LibraryPreset* BrushPresetPanel::presetAt(int index) const {
    if (m_store == nullptr || index < 0)
        return nullptr;
    const std::vector<io::brush::LibraryPreset>& all = m_store->presets();
    if (static_cast<std::size_t>(index) >= all.size())
        return nullptr;
    return &all[static_cast<std::size_t>(index)];
}

std::string BrushPresetPanel::nameAt(int index) const {
    if (index < 0)
        return _("Default round");
    if (static_cast<std::size_t>(index) < m_names.size())
        return m_names[static_cast<std::size_t>(index)];
    return {};
}

bool BrushPresetPanel::userInstalled(int index) const {
    return index >= 0 && static_cast<std::size_t>(index) < m_userInstalled.size() &&
           m_userInstalled[static_cast<std::size_t>(index)];
}

std::string BrushPresetPanel::tooltipFor(int index) const {
    if (index < 0)
        return std::string(_("Default round")) + "\n" +
               _("No preset: the engine's own plain round tip.");

    const io::brush::LibraryPreset* p = presetAt(index);
    if (p == nullptr)
        return {};
    std::string tip = p->preset.name;
    // The USER badge, in words. It is the first line after the name because it changes what the
    // preset IS for -- a brush you made, which Save overwrites in place and Delete can remove.
    if (userInstalled(index)) {
        tip += "\n";
        tip += _("Your own preset (in your brushes folder).");
    }
    const io::brush::PresetProvenance& prov = p->preset.provenance;
    switch (prov.fidelity) {
    case io::brush::PresetFidelity::Exact:
        break;
    case io::brush::PresetFidelity::Approximated:
        tip += "\n";
        // ⚠ APPROXIMATED IS NOT ALWAYS THE SAME SIZE OF LIE, and the generic line understated three
        // whole families badly enough to read as a bug report. For most presets it means what it
        // says -- some OPTIONS were dropped, the rest paints. But for the three source paintops
        // below Mosaic has NO ENGINE AT ALL: the preset imports as a plain pixel brush wearing that
        // paintop's tip. A Blender that does not blend, a spray that does not scatter. That is 21 of
        // the 117, and a user is owed the specific sentence, not the general one.
        if (const char* missing = missingEngineNote(prov.sourcePaintop))
            tip += missing;
        else
            tip += _("Approximated: some of this preset's options have no engine here yet.");
        break;
    case io::brush::PresetFidelity::Substituted:
        tip += "\n";
        tip += _("Substituted: imported as its nearest pixel-brush kin.");
        if (!prov.sourcePaintop.empty())
            tip += " (" + prov.sourcePaintop + ")";
        break;
    }
    if (!prov.droppedOptions.empty()) {
        tip += "\n";
        tip += _("Dropped:");
        const std::size_t shown = std::min<std::size_t>(prov.droppedOptions.size(), 6);
        for (std::size_t i = 0; i < shown; ++i)
            tip += (i == 0 ? " " : ", ") + prov.droppedOptions[i];
        if (prov.droppedOptions.size() > shown)
            tip += ", ...";
    }
    return tip;
}

void BrushPresetPanel::setSelected(int index) {
    if (index == m_selected)
        return;
    m_selected = index;
    syncEditButton();
    if (m_grid != nullptr) {
        const int slot = m_grid->slotOf(index);
        if (slot >= 0)
            scrollCellIntoView(slot);
        m_grid->redraw();
    }
    redraw();
}

void BrushPresetPanel::pick(int presetIndex) {
    if (presetIndex != m_selected) {
        m_selected = presetIndex;
        syncEditButton();
        if (m_grid != nullptr) {
            const int slot = m_grid->slotOf(presetIndex);
            if (slot >= 0)
                scrollCellIntoView(slot);
            m_grid->redraw();
        }
        redraw();
    }
    if (m_onSelect)
        m_onSelect(presetIndex); // the HOST resolves it: select() once, never per stroke
}

void BrushPresetPanel::syncEditButton() {
    if (m_editButton == nullptr)
        return;
    // Editable == there is a real preset behind the cell. Index -1 is the engine's own analytic
    // circle: no option table, no tip file, nothing to save over.
    if (presetAt(m_selected) != nullptr)
        m_editButton->activate();
    else
        m_editButton->deactivate();
}

void BrushPresetPanel::requestEdit(int index) {
    if (index < 0 || presetAt(index) == nullptr)
        return; // Default round, or a stale index: nothing to open the editor on
    if (m_onEdit)
        m_onEdit(index);
}

void BrushPresetPanel::refreshStore() {
    const BrushPresetStore* store = m_store;
    // ⚠ Both caches are keyed by INDEX, and a save or an import re-numbers the corpus from wherever
    // it inserted. Dropping them whole is the honest move: the alternative is a per-index fixup
    // that has to know exactly where the insertion happened, which is a rule in two places.
    m_icons.clear();
    m_thumbs.clear();
    m_params.clear();
    m_strokes.clear();
    m_store = nullptr;
    setStore(store); // re-derives names, groups, provenance and the tabs from the store as it stands
    syncEditButton();
}

void BrushPresetPanel::setFilter(const std::string& text) {
    if (text == m_filter)
        return;
    m_filter = text;
    if (m_search != nullptr && m_search->value() != m_filter)
        m_search->value(m_filter.c_str());
    rebuildSlots();
}

void BrushPresetPanel::onFilterEdited() {
    if (m_search == nullptr)
        return;
    const std::string next = m_search->value() != nullptr ? m_search->value() : "";
    if (next == m_filter)
        return;
    m_filter = next;
    rebuildSlots();
}

int BrushPresetPanel::matchCount() const {
    if (m_grid == nullptr)
        return 0;
    int n = 0;
    for (const int index : m_grid->slots())
        if (index >= 0) // the Default-round cell is not one of the library's presets
            ++n;
    return n;
}

int BrushPresetPanel::totalCount() const {
    return static_cast<int>(m_names.size());
}

int BrushPresetPanel::corpusCount() const {
    // How many presets the ACTIVE corpus and tab hold, ignoring the search. This is the denominator
    // the header shows, and it is the number "12 of ..." has to be able to reach.
    int n = 0;
    for (std::size_t i = 0; i < m_names.size(); ++i) {
        if (!inCorpus(static_cast<int>(i)))
            continue;
        const PresetGroup group = i < m_groups.size() ? m_groups[i] : PresetGroup::Other;
        const bool mine = i < m_userInstalled.size() && m_userInstalled[i];
        if (presetTabAdmits(m_tab, group, mine))
            ++n;
    }
    return n;
}

// The filter re-runs, the grid is re-laid, and NOT ONE widget is created or destroyed -- the grid is
// a single widget precisely so a keystroke cannot churn 118 children (and so it can never free a
// widget an event is still running inside, the HistoryPanel lesson).
void BrushPresetPanel::rebuildSlots() {
    if (m_grid == nullptr)
        return;
    std::vector<int> slots;
    // Slot 0: "Default round" = NO preset, whenever the filter admits it. It is a real cell because
    // "put it back the way it was" must be a click, not a mystery, and it is FIRST wherever it
    // appears -- it is where every brush starts.
    //
    // It rides on All and on BASICS (the user's call, and it is right: the plain round nib is the most
    // basic thing in the library, and Basics is the tab the dock opens on). Not on the other media
    // tabs -- it belongs to no medium, and repeating it in seven places would be noise in seven
    // places.
    if ((m_tab == PresetTab::All || m_tab == PresetTab::Basics) &&
        presetMatchesQuery(_("Default round"), m_filter))
        slots.push_back(-1);
    for (const int i : filterPresetIndices(m_names, m_filter)) {
        if (!inCorpus(i))
            continue;
        const PresetGroup group = static_cast<std::size_t>(i) < m_groups.size()
                                      ? m_groups[static_cast<std::size_t>(i)]
                                      : PresetGroup::Other;
        const bool mine = static_cast<std::size_t>(i) < m_userInstalled.size() &&
                          m_userInstalled[static_cast<std::size_t>(i)];
        if (!presetTabAdmits(m_tab, group, mine))
            continue;
        slots.push_back(i);
    }
    m_grid->setSlots(std::move(slots));

    if (m_clearButton != nullptr) {
        if (m_filter.empty())
            m_clearButton->hide();
        else
            m_clearButton->show();
    }
    if (m_scroll != nullptr)
        m_scroll->scroll_to(0, 0); // a new result set starts at the top
    layoutChildren();
    redraw();
}

void BrushPresetPanel::resize(int X, int Y, int W, int H) {
    Fl_Widget::resize(X, Y, W, H); // NOT Fl_Group::resize -- we place the children ourselves
    layoutChildren();
}

void BrushPresetPanel::layoutChildren() {
    if (m_search == nullptr || m_scroll == nullptr || m_grid == nullptr)
        return;
    const int X = x();
    const int Y = y();
    const int W = w();
    const int H = h();

    const int searchY = Y + kHeaderH;
    const int clearW = m_filter.empty() ? 0 : kClearW + 4;
    const int editW = m_editButton != nullptr ? kEditW + 4 : 0;
    const int searchW = std::max(20, W - kLeftInset - kRightPad - clearW - editW);
    m_search->resize(X + kLeftInset, searchY, searchW, kSearchH);
    m_clearButton->resize(X + kLeftInset + searchW + 4, searchY, kClearW, kSearchH);
    if (m_editButton != nullptr)
        m_editButton->resize(X + W - kRightPad - kEditW, searchY, kEditW, kSearchH);

    // The tab strip, between the search row and the grid. It disappears entirely when there is
    // nothing to file (the Eraser's three presets, or an empty library) -- one tab reading "All" is
    // not a filing system, it is a label pretending to be one.
    int stripBottom = searchY + kSearchH;
    const bool showTabs = m_tabs != nullptr && m_tabs->tabs().size() > 1;
    if (m_tabs != nullptr) {
        if (showTabs) {
            m_tabs->resize(X + kLeftInset, stripBottom + kTabStripGap,
                           std::max(20, W - kLeftInset - kRightPad), kTabH);
            m_tabs->show();
            m_tabs->scrollActiveIntoView(); // a narrower dock can push the active tab off the end
            stripBottom = m_tabs->y() + kTabH;
        } else {
            m_tabs->hide();
        }
    }

    const int scrollTop = stripBottom + kSearchGap;
    const int scrollX = X + kGrabBand; // never under the dock's resize band
    m_scroll->resize(scrollX, scrollTop, std::max(2, X + W - 1 - scrollX),
                     std::max(1, Y + H - scrollTop));

    // The scrollbar gutter has to come from a LAYOUT pass, never from `scrollbar.visible()` at draw
    // time -- and it feeds back into the metrics, so the pass runs twice: once to find out whether
    // the bar appears at all, once to lay the columns out in what is left.
    const int count = static_cast<int>(m_grid->slots().size());
    PresetGridMetrics metrics = presetGridMetrics(m_scroll->w(), m_displayMode);
    int contentH = presetGridContentHeight(count, metrics);
    const int gutter = m_scroll->scrollbarGutter(contentH);
    if (gutter > 0) {
        metrics = presetGridMetrics(m_scroll->w() - gutter, m_displayMode);
        contentH = presetGridContentHeight(count, metrics);
    }
    // A different cell size means every cached thumbnail is the wrong size: re-scale from the SOURCE
    // on the next paint rather than resample a resample. The source is m_icons -- in memory, decoded
    // once. ⚠ m_icons is NOT touched here; dropping it would send a 3 px drag back to the archive,
    // which is exactly the bug this split fixed.
    if (metrics.thumb != m_thumbSize) {
        m_thumbs.clear();
        m_thumbSize = metrics.thumb;
    }
    m_grid->setMetrics(metrics);
    // Fl_Scroll places its children in absolute coords, so the content origin is the viewport's top
    // MINUS how far it has been scrolled (the HistoryPanel::layoutRows idiom).
    m_grid->resize(m_scroll->x(), m_scroll->y() - m_scroll->yposition(),
                   std::max(1, m_scroll->w() - gutter), std::max(1, contentH));
    // The content may have got shorter (a filter, a wider dock): clamp the scroll so the grid cannot
    // be left hanging above the viewport's top edge.
    const int maxScroll = std::max(0, contentH - m_scroll->h());
    if (m_scroll->yposition() > maxScroll)
        m_scroll->scroll_to(0, maxScroll);
}

void BrushPresetPanel::scrollCellIntoView(int slot) {
    if (m_scroll == nullptr || m_grid == nullptr || slot < 0)
        return;
    const PresetCellRect r = presetCellRect(slot, m_grid->metrics(), 0, 0);
    const int view = m_scroll->h();
    int target = m_scroll->yposition();
    if (r.y < target)
        target = std::max(0, r.y - kGridPad);
    else if (r.y + r.h > target + view)
        target = r.y + r.h - view + kGridPad;
    m_scroll->scroll_to(0, std::max(0, target));
}

common::Color8 BrushPresetPanel::cellGround() const {
    return activePalette().panelBg;
}

const common::Image* BrushPresetPanel::thumbnailPixels(int index) {
    if (thumbnailFor(index) == nullptr)
        return nullptr;
    const auto it = m_thumbs.find(index);
    return it != m_thumbs.end() && !it->second.pixels.empty() ? &it->second.pixels : nullptr;
}

const common::Image* BrushPresetPanel::iconFor(int index) {
    if (const auto it = m_icons.find(index); it != m_icons.end())
        return it->second.empty() ? nullptr : &it->second; // empty == the remembered miss

    const io::brush::LibraryPreset* p = presetAt(index);
    if (p == nullptr)
        return nullptr; // not a preset at all -- nothing to remember

    // ⚠ THE decode, and the only one a preset ever gets. It runs from the cell's own draw(), so only
    // the presets a panel actually SHOWS cost anything (io/brush/library.hpp: "a 200x200 RGBA
    // thumbnail per preset is ~160 KB that only the presets a panel shows should cost") -- and now
    // it also runs at most ONCE, because nothing below this line invalidates m_icons.
    common::Image icon; // stays empty on a refusal -> the miss is what gets cached
    std::string error;
    // ⚠ A LOOSE preset has no ARCHIVE ENTRY, and that empty field is the flag (ui/brush_presets.hpp):
    // its container IS the file, so it is decoded here rather than handed to the library, whose
    // loadIcon would open a `.mbp` as a zip and report a broken bundle. The two containers put their
    // raster in the same place -- the PNG the file is -- so this is one branch, not a second path.
    std::optional<common::Image> decoded =
        p->entryName.empty() ? loadLoosePresetIcon(p->sourcePath, &error)
                             : m_store->library().loadIcon(*p, &error);
    if (decoded)
        icon = std::move(*decoded);
    else
        common::log::category("brush")->debug("preset icon {}: {}", p->preset.name, error);

    const auto [it, inserted] = m_icons.emplace(index, std::move(icon));
    return it->second.empty() ? nullptr : &it->second;
}

Fl_RGB_Image* BrushPresetPanel::thumbnailFor(int index) {
    if (index < 0 || m_store == nullptr || m_thumbSize <= 0)
        return nullptr;
    if (const auto it = m_thumbs.find(index); it != m_thumbs.end())
        return it->second.img.get(); // null for a preset whose icon would not decode

    // The re-scale, and it is ALL a dock-width drag pays now: a box filter over an image that is
    // already in memory. No file, no zip, no XML.
    Thumb entry;
    if (const common::Image* icon = iconFor(index); icon != nullptr) {
        entry.pixels = presetThumbnail(*icon, m_thumbSize, cellGround());
        entry.img = std::make_unique<Fl_RGB_Image>(entry.pixels.rgba.data(),
                                                   static_cast<int>(entry.pixels.width),
                                                   static_cast<int>(entry.pixels.height), 4);
    }
    Fl_RGB_Image* raw = entry.img.get();
    m_thumbs.emplace(index, std::move(entry));
    return raw;
}

const core::brush::BrushParams* BrushPresetPanel::paramsFor(int index) {
    if (const auto it = m_params.find(index); it != m_params.end())
        return &it->second;

    core::brush::BrushParams p;
    if (index >= 0) {
        const io::brush::LibraryPreset* lp = presetAt(index);
        if (lp == nullptr)
            return nullptr;
        // ⚠ ONCE PER PRESET, NEVER PER RENDER. This mints the tip's raster id, and a fresh id is a
        // permanently cold dab cache -- so a preview that rebuilt its params would pay for a full
        // mask rasterization on every single dab, every single time.
        p = io::brush::presetBrushParams(*lp);
    } else if (m_corpus == PresetCorpus::Eraser) {
        // ⚠ The Default-round cell is whatever the TOOL holding it does. For the Brush it is the
        // engine's analytic circle; for the Eraser it is that same circle CARVING -- which is exactly
        // what the Eraser paints with when it holds no preset. A card that previewed the eraser's
        // plain round as a stroke of paint would be advertising the wrong tool.
        p.strokeMode = core::brush::StrokeMode::Erase;
    }
    // A default-constructed BrushParams IS the analytic circle the engine paints when no preset is
    // chosen. That is the honest picture of it.
    const auto [it, inserted] = m_params.emplace(index, std::move(p));
    return &it->second;
}

Fl_RGB_Image* BrushPresetPanel::strokePreviewFor(int index, int renderWidth, int height) {
    if (m_store == nullptr || renderWidth <= 0 || height <= 0)
        return nullptr;

    // The strip's size is part of the key, not part of the entry: when it changes, every cached
    // stroke is for the wrong strip and the whole cache goes. Bucketing the width (see
    // presetStrokeRenderWidth) is what makes that rare enough to afford.
    if (renderWidth != m_strokeW || height != m_strokeH) {
        m_strokes.clear();
        m_strokeW = renderWidth;
        m_strokeH = height;
    }
    if (const auto it = m_strokes.find(index); it != m_strokes.end())
        return it->second.img.get();

    const core::brush::BrushParams* params = paramsFor(index);
    if (params == nullptr)
        return nullptr;

    const bool eraser = params->strokeMode == core::brush::StrokeMode::Erase;
    const core::brush::StrokePreviewStyle style = presetStrokeStyle(activePalette(), eraser);

    Stroke entry;
    entry.pixels = core::brush::renderStrokePreview(*params, renderWidth, height, style);
    ++m_strokeRenders; // an EVENT, and the only thing a test can honestly assert on
    entry.img = std::make_unique<Fl_RGB_Image>(entry.pixels.rgba.data(),
                                               static_cast<int>(entry.pixels.width),
                                               static_cast<int>(entry.pixels.height), 4);
    Fl_RGB_Image* raw = entry.img.get();
    m_strokes.emplace(index, std::move(entry));
    return raw;
}

void BrushPresetPanel::setDisplayMode(PresetDisplayMode mode) {
    if (mode == m_displayMode)
        return;
    m_displayMode = mode;
    // The cells change shape, so both size-keyed caches are for the wrong cell now. m_icons and
    // m_params survive -- neither knows anything about a cell's size.
    m_thumbs.clear();
    m_thumbSize = 0;
    m_strokes.clear();
    m_strokeW = 0;
    m_strokeH = 0;
    layoutChildren();
    rebuildSlots();
    redraw();
}

void BrushPresetPanel::reapplyTheme() {
    Panel::reapplyTheme();
    if (m_search != nullptr)
        m_search->applyStyle(); // it CACHES its colours; the redraw alone would not move them
    if (m_scroll != nullptr) {
        m_scroll->color(toFl(activePalette().panelBg));
        m_scroll->reapplyTheme();
    }
    m_thumbs.clear(); // they were composited over the OLD panel ground; m_icons was not, and stays
    // ⚠ ... and so were the STROKES, now that the card's paper and ink follow the theme (they did not
    // when this cache was written, and the omission would have left a dark-paper card sitting in a
    // light dock until something else happened to evict it). m_params is theme-free and stays.
    m_strokes.clear();
    m_strokeW = 0;
    m_strokeH = 0;
    redraw();
}

void BrushPresetPanel::draw() {
    Panel::draw(); // themed fill + left edge + children (the search row, the scroll, the grid)

    // Text drawn straight onto the panel ground may ONLY go down when that ground was just cleared:
    // Fl_Group::draw() clears the box only when damage is more than FL_DAMAGE_CHILD, and re-stamping
    // text over itself anti-aliases it heavier every pass (the "labels get bolder" bug).
    if ((damage() & ~FL_DAMAGE_CHILD) == 0)
        return;

    const Palette& pal = activePalette();
    fl_font(FL_HELVETICA_BOLD, 13);
    fl_color(toFl(pal.text));
    // The header names the CORPUS the grid is actually showing. "Brush Presets" over the three
    // erasers is simply false, and the corpus split (§8.2) is what made it so.
    const char* title =
        m_corpus == PresetCorpus::Eraser ? _("Eraser Presets") : _("Brush Presets");
    fl_draw(title, x() + kLeftInset, y(), w() - kLeftInset - kRightPad, kHeaderH,
            FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

    // The count, right-aligned: "114" at rest, "12 of 114" while filtering -- so a filter that hides
    // everything says so instead of just showing an empty box.
    //
    // ⚠ The denominator is the CORPUS, not the library. The Brush is not offered the 3 erasers and
    // the Eraser is offered nothing else, so a Brush grid reading "117" would be counting brushes it
    // will not show you -- and the arithmetic "12 of 117" would never be able to reach its own total.
    // It is also per-TAB: on Draw, the total is the drawing brushes, because that is what "all of
    // them" means while you are standing in that tab.
    const int total = corpusCount();
    const int shown = matchCount();
    char count[48];
    if (m_filter.empty())
        std::snprintf(count, sizeof(count), "%d", total);
    else
        std::snprintf(count, sizeof(count), _("%d of %d"), shown, total);
    fl_font(FL_HELVETICA, 11);
    fl_color(toFl(pal.textMuted));
    fl_draw(count, x() + w() - kRightPad - 80, y(), 80, kHeaderH, FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);

    if (m_grid != nullptr && m_grid->slots().empty() && m_scroll != nullptr) {
        fl_font(FL_HELVETICA, 12);
        fl_color(toFl(pal.textMuted));
        fl_draw(m_store == nullptr || corpusCount() == 0 ? _("No presets installed")
                                                        : _("No preset matches"),
                m_scroll->x(), m_scroll->y(), m_scroll->w(), m_scroll->h(), FL_ALIGN_CENTER);
    }
}

} // namespace mosaic::ui
