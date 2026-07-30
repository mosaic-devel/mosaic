#include "ui/channels_panel.hpp"

#include "common/i18n.hpp"
#include "render/histogram_gpu.hpp" // the S60-e device binning lane (gpuHistogramBinner)
#include "ui/icons.hpp" // drawIcon / Icon (the shared panel-chrome eye glyphs)
#include "ui/theme.hpp"

#include <FL/Enumerations.H>
#include <FL/Fl.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace mosaic::ui {
namespace {

// ---- Re-bin cadence (S60-e; docs/s60-readback-consumers.md A5) ---------------------------------
// A revision bump is not a reason to bin -- the edits STOPPING is. kSettleSec is short enough that
// a single edit still reads as instant and long enough that a stroke's dabs collapse into one
// re-bin; kMaxDeferSec keeps a long continuous drag from freezing the plot for its whole duration,
// at ~2 re-bins a second, which is well under what a full-canvas pass costs per dab. Neither is a
// setting: there is one right answer here and it is not the user's to tune.
constexpr double kSettleSec = 0.12;
constexpr double kMaxDeferSec = 0.5;

// ---- Layout -----------------------------------------------------------------------------------
constexpr int kPad = 10;       // inset from the panel's left/right edges (clears the splitter band)
constexpr int kHistTop = 12;   // gap from the panel top to the histogram
constexpr int kHistH = 140;    // the histogram widget's total height (plot + axis strip)
constexpr int kAxisH = 16;     // the tick-label strip inside the histogram, below the plot
constexpr int kRowsGap = 14;   // gap from the histogram to the first channel row
constexpr int kChanRowH = 30;  // one channel row
constexpr int kEyeW = 26;      // the visibility-toggle cell at a row's left (mirrors the layer dock)
constexpr int kSwatch = 12;    // the channel colour swatch square
constexpr int kStatW = 46;     // the right-aligned mean-value column

// ---- Band + swatch colours (toned primaries: additive on dark reaches near-white where they
// overlap without harsh clipping; subtractive on light leaves each channel's hue). -------------
constexpr common::Color8 kRed{235, 72, 72, 255};
constexpr common::Color8 kGreen{84, 200, 96, 255};
constexpr common::Color8 kBlue{96, 132, 240, 255};
constexpr common::Color8 kLumaGray{170, 170, 174, 255};
constexpr common::Color8 kAlphaGray{150, 150, 156, 255};

Fl_Color toFl(common::Color8 c) {
    return fl_rgb_color(c.r, c.g, c.b);
}

std::uint8_t clamp8(double v) {
    if (v <= 0.0)
        return 0;
    if (v >= 255.0)
        return 255;
    return static_cast<std::uint8_t>(v + 0.5);
}

common::Color8 mix(common::Color8 a, common::Color8 b, double t) {
    return {clamp8(a.r * (1.0 - t) + b.r * t), clamp8(a.g * (1.0 - t) + b.g * t),
            clamp8(a.b * (1.0 - t) + b.b * t), 255};
}

} // namespace

// ---- computeHistogram -------------------------------------------------------------------------

ChannelHistogram computeHistogram(const common::Image& img) {
    ChannelHistogram h;
    if (img.empty())
        return h;
    const std::vector<std::uint8_t>& px = img.rgba;
    const std::size_t n = px.size();
    // Colour sums are alpha-weighted, so they need the extra headroom: 255 * 255 per pixel.
    std::uint64_t sumR = 0, sumG = 0, sumB = 0, sumL = 0; // weighted
    std::uint64_t sumA = 0, count = 0, weight = 0;
    for (std::size_t p = 0; p + 3 < n; p += 4) {
        const std::uint32_t r = px[p], g = px[p + 1], b = px[p + 2], a = px[p + 3];
        ++count;
        sumA += a;
        ++h.a[a]; // the alpha channel counts EVERY pixel -- that distribution is the point of it
        if (a == 0)
            continue; // invisible: contributes no colour, because there is no colour to see
        // Rec.601 luminance; the weights (77,150,29) sum to 256, so >>8 lands in [0,255].
        std::uint32_t l = (r * 77u + g * 150u + b * 29u) >> 8;
        if (l > 255u)
            l = 255u;
        // Weight by coverage: a 10%-visible pixel is 10% of a pixel's worth of colour on screen.
        h.r[r] += a;
        h.g[g] += a;
        h.b[b] += a;
        h.luma[l] += a;
        sumR += static_cast<std::uint64_t>(r) * a;
        sumG += static_cast<std::uint64_t>(g) * a;
        sumB += static_cast<std::uint64_t>(b) * a;
        sumL += static_cast<std::uint64_t>(l) * a;
        weight += a;
    }
    h.totalPixels = count;
    h.visibleAlpha = weight;
    if (count != 0)
        h.meanA = static_cast<double>(sumA) / static_cast<double>(count);
    if (weight != 0) {
        // Divide by the WEIGHT, not the pixel count: these are the means of what is visible, so a
        // fully transparent canvas has no mean colour rather than a mean of black.
        const double inv = 1.0 / static_cast<double>(weight);
        h.meanR = static_cast<double>(sumR) * inv;
        h.meanG = static_cast<double>(sumG) * inv;
        h.meanB = static_cast<double>(sumB) * inv;
        h.meanLuma = static_cast<double>(sumL) * inv;
    }
    return h;
}

// ---- gpuHistogramBinner (S60-e) ---------------------------------------------------------------

namespace {

// Rebuild the whole ChannelHistogram from the device lane's five bin arrays.
//
// This is EXACT, not approximate, and the reason is worth stating: every total computeHistogram
// accumulates is linear in the bins it fills, so the bins already contain it. `h.r[r] += a` makes
// sum_v v * r[v] the alpha-weighted red sum and sum_v r[v] the visible-alpha weight; `++h.a[a]`
// makes sum_v a[v] the pixel count and sum_v v * a[v] the plain alpha sum. Integer addition is
// associative and none of these can overflow a uint64 (255 * 255 * pixels), so summing in a
// different ORDER than the reference cannot change the answer.
//
// ⚠ The two float steps are then performed with the reference's own operations in the reference's
// own order -- ONE reciprocal shared by the alpha-weighted trio, a plain division for meanA -- for
// the opposite reason: float addition is NOT associative, and "the same value computed differently"
// is exactly what stops being byte-identical.
ChannelHistogram histogramFromBins(const render::HistogramBins& bins) {
    ChannelHistogram h;
    h.r = bins.r;
    h.g = bins.g;
    h.b = bins.b;
    h.a = bins.a;
    h.luma = bins.luma;

    std::uint64_t sumR = 0, sumG = 0, sumB = 0, sumL = 0;
    std::uint64_t sumA = 0, count = 0, weight = 0;
    for (std::size_t v = 0; v < 256; ++v) {
        const std::uint64_t bv = static_cast<std::uint64_t>(v);
        count += bins.a[v];
        sumA += bv * bins.a[v];
        weight += bins.r[v]; // the colour bins' total weight IS the visible alpha
        sumR += bv * bins.r[v];
        sumG += bv * bins.g[v];
        sumB += bv * bins.b[v];
        sumL += bv * bins.luma[v];
    }
    h.totalPixels = count;
    h.visibleAlpha = weight;
    if (count != 0)
        h.meanA = static_cast<double>(sumA) / static_cast<double>(count);
    if (weight != 0) {
        const double inv = 1.0 / static_cast<double>(weight);
        h.meanR = static_cast<double>(sumR) * inv;
        h.meanG = static_cast<double>(sumG) * inv;
        h.meanB = static_cast<double>(sumB) * inv;
        h.meanLuma = static_cast<double>(sumL) * inv;
    }
    return h;
}

} // namespace

HistogramBinner gpuHistogramBinner(render::HistogramGpu* lane) {
    if (lane == nullptr)
        return {}; // no lane: computeHistogram serves, which is the whole of the default build
    return [lane](const common::Image& img, ChannelHistogram& out) {
        render::HistogramBins bins;
        if (!lane->bin(img, bins))
            return false; // an ordinary refusal -- the caller runs the reference
        out = histogramFromBins(bins);
        return true;
    };
}

// ---- histogramNormalizer ----------------------------------------------------------------------

std::uint64_t histogramNormalizer(const std::vector<const std::array<std::uint64_t, 256>*>& bands,
                                  int pct) {
    // Only NON-EMPTY bins take part. Empty bins are the overwhelming majority in a typical image
    // (a photo touches maybe half the range per channel), and including them would drag any
    // percentile down to zero and blow every real bar off the top of the plot.
    std::vector<std::uint64_t> vals;
    vals.reserve(bands.size() * 64);
    for (const auto* bins : bands) {
        if (bins == nullptr)
            continue;
        for (const std::uint64_t v : *bins)
            if (v != 0)
                vals.push_back(v);
    }
    if (vals.empty())
        return 1;
    const int p = std::clamp(pct, 0, 100);
    // Nearest-rank on the sorted values: index = ceil(p/100 * n) - 1, clamped into range.
    std::size_t idx = (static_cast<std::size_t>(p) * vals.size() + 99) / 100;
    idx = idx == 0 ? 0 : idx - 1;
    idx = std::min(idx, vals.size() - 1);
    std::nth_element(vals.begin(), vals.begin() + static_cast<std::ptrdiff_t>(idx), vals.end());
    return std::max<std::uint64_t>(vals[idx], 1);
}

// ---- applyChannelViewMask ---------------------------------------------------------------------

void applyChannelViewMask(common::Image& img, const ChannelViewMask& mask) {
    if (mask.isNormal() || img.empty())
        return; // the full composite passes through untouched
    std::vector<std::uint8_t>& px = img.rgba;
    const std::size_t n = px.size();

    if (mask.alpha) {
        // The alpha view: the coverage values themselves, as opaque grayscale.
        for (std::size_t p = 0; p + 3 < n; p += 4) {
            const std::uint8_t a = px[p + 3];
            px[p] = a;
            px[p + 1] = a;
            px[p + 2] = a;
            px[p + 3] = 255;
        }
        return;
    }

    const int nOn = (mask.red ? 1 : 0) + (mask.green ? 1 : 0) + (mask.blue ? 1 : 0);
    if (nOn == 1) {
        // A single colour channel isolated: show it as grayscale (its value in all three), keeping
        // the pixel's own alpha so transparent regions still read.
        for (std::size_t p = 0; p + 3 < n; p += 4) {
            const std::uint8_t v = mask.red ? px[p] : (mask.green ? px[p + 1] : px[p + 2]);
            px[p] = v;
            px[p + 1] = v;
            px[p + 2] = v;
        }
    } else {
        // Two channels shown (or the degenerate none): keep the shown ones, zero the hidden ones.
        for (std::size_t p = 0; p + 3 < n; p += 4) {
            if (!mask.red)
                px[p] = 0;
            if (!mask.green)
                px[p + 1] = 0;
            if (!mask.blue)
                px[p + 2] = 0;
        }
    }
}

// ---- channelViewShaderCode --------------------------------------------------------------------

std::uint32_t channelViewShaderCode(const ChannelViewMask& mask) noexcept {
    // NORMAL first, and NORMAL is 0: the shader early-outs on it, so a canvas with nothing isolated
    // runs exactly the instructions it ran before any of this existed. Everything else carries the
    // marker bit, which is what keeps the all-colours-hidden view (legal, and black) out of 0.
    if (mask.isNormal())
        return kChannelViewNormal;
    std::uint32_t code = kChannelViewActive;
    if (mask.red)
        code |= kChannelViewRed;
    if (mask.green)
        code |= kChannelViewGreen;
    if (mask.blue)
        code |= kChannelViewBlue;
    if (mask.alpha)
        code |= kChannelViewAlpha; // exclusive: the shader tests this bit before the colour ones,
                                   // exactly as applyChannelViewMask tests mask.alpha first
    return code;
}

// ---- HistogramView ----------------------------------------------------------------------------

HistogramView::HistogramView(int X, int Y, int W, int H, ChannelsPanel* panel)
    : Fl_Widget(X, Y, W, H), m_panel(panel) {
    box(FL_NO_BOX); // the panel behind us owns the ground; we blit an opaque raster over it
}

void HistogramView::markDirty() {
    m_dirty = true;
    redraw();
}

void HistogramView::resize(int X, int Y, int W, int H) {
    Fl_Widget::resize(X, Y, W, H);
    m_dirty = true;
}

void HistogramView::rebuild() {
    m_dirty = false;
    const int plotW = std::max(1, w());
    const int plotH = std::max(1, h() - kAxisH);
    m_raster = common::Image(static_cast<std::uint32_t>(plotW), static_cast<std::uint32_t>(plotH));
    const Palette& pal = activePalette();

    // The plot ground reads as a shallow "well": a touch darker than the panel on a dark theme, a
    // touch brighter (toward white) on a light one -- so the coloured bands pop either way.
    const common::Color8 plotBg =
        pal.dark ? mix(pal.panelBg, {0, 0, 0, 255}, 0.22) : mix(pal.panelBg, {255, 255, 255, 255}, 0.55);
    const common::Color8 gridCol = mix(plotBg, pal.border, 0.7);
    const common::Color8 frameCol = pal.border;
    m_raster.fill(plotBg);

    auto put = [&](int px, int py, common::Color8 c) {
        if (px < 0 || px >= plotW || py < 0 || py >= plotH)
            return;
        const std::size_t o = (static_cast<std::size_t>(py) * plotW + px) * 4;
        m_raster.rgba[o] = c.r;
        m_raster.rgba[o + 1] = c.g;
        m_raster.rgba[o + 2] = c.b;
        m_raster.rgba[o + 3] = 255;
    };

    // A subtle grid: verticals at the quarter tones (64/128/192), horizontals at the quarters.
    for (const int v : {64, 128, 192}) {
        const int gx = static_cast<int>(std::lround(static_cast<double>(v) / 255.0 * (plotW - 1)));
        for (int py = 0; py < plotH; ++py)
            put(gx, py, gridCol);
    }
    for (const double f : {0.25, 0.5, 0.75}) {
        const int gy = static_cast<int>(std::lround(f * (plotH - 1)));
        for (int px = 0; px < plotW; ++px)
            put(px, gy, gridCol);
    }

    // Which bands to draw, in composite order (read live from the panel's toggles).
    struct Band {
        const std::array<std::uint64_t, 256>* bins;
        common::Color8 color;
    };
    const ChannelHistogram& hist = m_panel->histogram();
    std::vector<Band> bands;
    // Bands follow the same eye state that drives the canvas view: the alpha isolation shows the
    // alpha distribution alone; otherwise the enabled colour channels overlay (no separate luma
    // band -- the neutral RGB row is the "normal view" master, not a distribution of its own).
    if (m_panel->channelEnabled(Channel::Alpha)) {
        bands.push_back({&hist.a, kAlphaGray});
    } else {
        if (m_panel->channelEnabled(Channel::Red))
            bands.push_back({&hist.r, kRed});
        if (m_panel->channelEnabled(Channel::Green))
            bands.push_back({&hist.g, kGreen});
        if (m_panel->channelEnabled(Channel::Blue))
            bands.push_back({&hist.b, kBlue});
    }

    if (!bands.empty() && !hist.empty()) {
        // Display normalization: a robust percentile of the shown bands, NOT their tallest bin --
        // see histogramNormalizer for why the naive maximum made this plot unreadable. Bars above
        // it clip, and get a bright cap so the plot never silently understates them.
        //
        // This also subsumes the old "exclude the 0/255 endpoints" special case: a clipped photo's
        // pile-up at pure black/white, and an opaque document's every-pixel-in-alpha-bin-255, are
        // just outliers, and the percentile already handles outliers wherever they land. One rule
        // instead of a rule plus two exceptions.
        std::vector<const std::array<std::uint64_t, 256>*> binPtrs;
        binPtrs.reserve(bands.size());
        for (const Band& bd : bands)
            binPtrs.push_back(bd.bins);
        const std::uint64_t maxBin = histogramNormalizer(binPtrs);

        // Per-column bar height for each band. A column spans a slice of the 256 bins; take the MAX
        // in that slice so a narrow-canvas downscale never drops a spike (and a wide dock steps the
        // bars, one bin across several columns).
        std::vector<std::vector<double>> heights(bands.size(),
                                                 std::vector<double>(static_cast<std::size_t>(plotW), 0.0));
        // Per (band, column): did this bar run past the top? A clipped bar drawn plain would claim
        // to be exactly full height, which is a lie about data the user cannot otherwise see.
        std::vector<std::vector<bool>> clipped(bands.size(),
                                               std::vector<bool>(static_cast<std::size_t>(plotW), false));
        for (std::size_t bi = 0; bi < bands.size(); ++bi) {
            const std::array<std::uint64_t, 256>& bins = *bands[bi].bins;
            for (int x = 0; x < plotW; ++x) {
                long lo = static_cast<long>(x) * 256 / plotW;
                long hi = (static_cast<long>(x) + 1) * 256 / plotW;
                if (hi <= lo)
                    hi = lo + 1;
                lo = std::clamp<long>(lo, 0, 256);
                hi = std::clamp<long>(hi, 0, 256);
                std::uint64_t m = 0;
                for (long i = lo; i < hi; ++i)
                    m = std::max(m, bins[static_cast<std::size_t>(i)]);
                const double raw = static_cast<double>(m) / static_cast<double>(maxBin);
                clipped[bi][static_cast<std::size_t>(x)] = raw > 1.0;
                heights[bi][static_cast<std::size_t>(x)] = std::min(1.0, raw) * static_cast<double>(plotH);
            }
        }

        // Composite the bands with per-row coverage (anti-aliased tops). Dark theme: additive light
        // (overlaps brighten toward white). Light theme: subtract each channel's complement
        // (overlaps darken toward neutral). Both keep the colour coding legible on their ground.
        const bool dark = pal.dark;
        const double strength = dark ? 0.92 : 0.85;
        for (int py = 0; py < plotH; ++py) {
            const int fromBottom = plotH - 1 - py;
            for (int x = 0; x < plotW; ++x) {
                const std::size_t o = (static_cast<std::size_t>(py) * plotW + x) * 4;
                double cr = m_raster.rgba[o];
                double cg = m_raster.rgba[o + 1];
                double cb = m_raster.rgba[o + 2];
                for (std::size_t bi = 0; bi < bands.size(); ++bi) {
                    double cov = heights[bi][static_cast<std::size_t>(x)] - static_cast<double>(fromBottom);
                    if (cov <= 0.0)
                        continue;
                    if (cov > 1.0)
                        cov = 1.0;
                    const common::Color8 col = bands[bi].color;
                    // A clipped bar's topmost row is drawn at full strength: a brighter cap that
                    // reads as "this continues past the top", so a clipped bar and a bar that
                    // genuinely reaches the ceiling are never confusable.
                    const bool cap = clipped[bi][static_cast<std::size_t>(x)] && py == 0;
                    const double s = cap ? 1.0 : cov * strength;
                    if (dark) {
                        cr += s * col.r;
                        cg += s * col.g;
                        cb += s * col.b;
                    } else {
                        cr -= s * (255.0 - col.r);
                        cg -= s * (255.0 - col.g);
                        cb -= s * (255.0 - col.b);
                    }
                }
                m_raster.rgba[o] = clamp8(cr);
                m_raster.rgba[o + 1] = clamp8(cg);
                m_raster.rgba[o + 2] = clamp8(cb);
            }
        }
    }

    // A crisp 1px frame around the plot.
    for (int x = 0; x < plotW; ++x) {
        put(x, 0, frameCol);
        put(x, plotH - 1, frameCol);
    }
    for (int py = 0; py < plotH; ++py) {
        put(0, py, frameCol);
        put(plotW - 1, py, frameCol);
    }

    m_img = std::make_unique<Fl_RGB_Image>(m_raster.rgba.data(), plotW, plotH, 4);
}

void HistogramView::draw() {
    if (m_dirty || !m_img)
        rebuild();
    const Palette& pal = activePalette();
    const int plotH = std::max(1, h() - kAxisH);

    // Self-clear (the raster covers only the plot; the axis strip shows the panel ground). Robust on
    // a child-only redraw, where the parent group does not refill our cell.
    fl_color(toFl(pal.panelBg));
    fl_rectf(x(), y(), w(), h());

    if (m_img)
        m_img->draw(x(), y());

    if (m_panel->histogram().empty()) {
        fl_font(FL_HELVETICA, 12);
        fl_color(toFl(pal.textMuted));
        fl_draw(_("No image data"), x(), y(), w(), plotH, FL_ALIGN_CENTER);
    }

    // Tick labels along the value axis.
    fl_font(FL_HELVETICA, 9);
    fl_color(toFl(pal.textMuted));
    const int ty = y() + plotH + 1;
    fl_draw("0", x() + 1, ty, 20, kAxisH - 2, FL_ALIGN_LEFT | FL_ALIGN_TOP);
    fl_draw("128", x(), ty, w(), kAxisH - 2, FL_ALIGN_CENTER | FL_ALIGN_TOP);
    fl_draw("255", x() + w() - 25, ty, 24, kAxisH - 2, FL_ALIGN_RIGHT | FL_ALIGN_TOP);
}

// ---- ChannelsPanel ----------------------------------------------------------------------------

ChannelsPanel::ChannelsPanel(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
    const Palette& pal = activePalette();
    box(FL_FLAT_BOX);          // a plain panelBg fill (rows are drawn directly, so we self-clear)
    color(toFl(pal.panelBg));
    begin();
    m_histView = new HistogramView(X + kPad, Y + kHistTop, std::max(1, W - 2 * kPad), kHistH, this);
    end();
    resizable(nullptr); // layoutChildren places the histogram explicitly
    layoutChildren();
    syncMaster(); // the RGB row's eye tracks isNormal() from the first paint
    m_lastBin = std::chrono::steady_clock::now(); // the settle's clock starts now, not at the epoch
}

ChannelsPanel::~ChannelsPanel() {
    // A pending timeout holding `this` after the widget dies is a use-after-free, and the dock
    // destroys this panel on shutdown like any other child.
    Fl::remove_timeout(settleTimer, this);
}

void ChannelsPanel::layoutChildren() {
    if (m_histView != nullptr)
        m_histView->resize(x() + kPad, y() + kHistTop, std::max(1, w() - 2 * kPad), kHistH);
}

void ChannelsPanel::resize(int X, int Y, int W, int H) {
    Fl_Widget::resize(X, Y, W, H); // NOT Fl_Group::resize -- we place the one child ourselves
    layoutChildren();
}

void ChannelsPanel::setDocument(core::Document* doc) {
    Fl::remove_timeout(settleTimer, this); // a pending re-bin is about the document being replaced
    m_doc = doc;
    m_hist.clear();
    m_computedRev = 0;
    m_latestRev = 0;
    if (m_histView != nullptr)
        m_histView->markDirty();
    redraw();
}

void ChannelsPanel::notifyChanged(std::uint64_t revision) {
    m_latestRev = revision;
    if (revision != m_computedRev && visible_r())
        scheduleRecompute();
}

void ChannelsPanel::scheduleRecompute() {
    // ⚠ THIS FUNCTION NEVER BINS. A bump arrives from the composite writer, i.e. on the frame path,
    // and binning here would put a full-canvas pass -- and, with the device lane installed, an
    // upload and a fence -- on the frame that is drawing a brush dab. That is the shape S60-a
    // already regressed on once (docs/s60-readback-consumers.md §7): the guard has to live in the
    // mechanism, not in a caller's good intentions.
    const double sinceLast =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - m_lastBin).count();
    // Past the staleness cap with a timer already armed: let it FIRE rather than pushing it back
    // again, so a drag that keeps bumping still refreshes ~twice a second instead of freezing the
    // plot for its whole duration. Below the cap, each bump restarts the settle, which is what
    // collapses a stroke's dabs into one re-bin.
    if (sinceLast >= kMaxDeferSec && Fl::has_timeout(settleTimer, this) != 0)
        return;
    // Never stack two timers on us (the motivation-ticker idiom): FLTK's add_timeout does not
    // replace an existing one with the same callback and data, it queues a second.
    Fl::remove_timeout(settleTimer, this);
    Fl::add_timeout(kSettleSec, settleTimer, this);
}

void ChannelsPanel::settleTimer(void* data) {
    auto* self = static_cast<ChannelsPanel*>(data);
    // Hidden in the meantime (the user switched to Layers mid-stroke): drop it. onTabShown catches
    // up the moment the tab is looked at again, which is the only moment the bins matter.
    if (self->visible_r())
        self->recompute();
}

void ChannelsPanel::onTabShown() {
    // Synchronous, deliberately: this is a discrete user act, not the frame path, and the bins must
    // be current BEFORE the first paint. It also catches every re-bin the settle dropped.
    recompute();
}

void ChannelsPanel::recompute() {
    Fl::remove_timeout(settleTimer, this); // whatever was pending, this satisfies it
    m_lastBin = std::chrono::steady_clock::now();
    // No document -> nothing to histogram (the source composite would be empty anyway).
    const common::Image* img = (m_doc != nullptr && m_source) ? m_source() : nullptr;
    if (img != nullptr && !img->empty()) {
        // The device lane (S60-e) bins the same 5x256 bins from the same pixels; a refusal -- no
        // device, CPU-only mode, an over-cap canvas -- falls through to computeHistogram, which
        // stays the definition of what the bins ARE rather than a degraded substitute.
        if (!m_binner || !m_binner(*img, m_hist))
            m_hist = computeHistogram(*img);
    } else {
        m_hist.clear();
    }
    m_computedRev = m_latestRev;
    if (m_histView != nullptr)
        m_histView->markDirty();
    redraw();
}

void ChannelsPanel::reapplyTheme() {
    color(toFl(activePalette().panelBg));
    if (m_histView != nullptr)
        m_histView->markDirty();
    redraw();
}

int ChannelsPanel::rowsTop() const {
    return y() + kHistTop + kHistH + kRowsGap;
}

int ChannelsPanel::rowAt(int eventY) const {
    const int top = rowsTop();
    if (eventY < top)
        return -1;
    const int idx = (eventY - top) / kChanRowH;
    if (idx < 0 || idx >= kChannelCount)
        return -1;
    return idx;
}

ChannelViewMask ChannelsPanel::viewMask() const noexcept {
    ChannelViewMask m;
    m.red = m_enabled[static_cast<std::size_t>(Channel::Red)];
    m.green = m_enabled[static_cast<std::size_t>(Channel::Green)];
    m.blue = m_enabled[static_cast<std::size_t>(Channel::Blue)];
    m.alpha = m_enabled[static_cast<std::size_t>(Channel::Alpha)];
    return m;
}

void ChannelsPanel::syncMaster() {
    // The RGB-composite row's eye reads open exactly when the canvas shows the full colour composite.
    m_enabled[static_cast<std::size_t>(Channel::Luma)] = viewMask().isNormal();
}

// The channel-eye state machine (Photoshop-flavoured): the RGB row is a master that resets to the
// normal composite; a colour eye toggles that channel (isolating to grayscale when it is the only
// one left); alpha is an exclusive view. Never lands on a blank canvas (the last colour won't hide).
void ChannelsPanel::toggleChannel(Channel c) {
    const std::size_t R = static_cast<std::size_t>(Channel::Red);
    const std::size_t G = static_cast<std::size_t>(Channel::Green);
    const std::size_t B = static_cast<std::size_t>(Channel::Blue);
    const std::size_t A = static_cast<std::size_t>(Channel::Alpha);
    const ChannelViewMask before = viewMask();

    switch (c) {
    case Channel::Luma:
        // The RGB composite master: always return to the normal full-colour view.
        m_enabled[R] = m_enabled[G] = m_enabled[B] = true;
        m_enabled[A] = false;
        break;
    case Channel::Red:
    case Channel::Green:
    case Channel::Blue: {
        const std::size_t i = static_cast<std::size_t>(c);
        if (m_enabled[A]) {
            // Leaving the alpha view: isolate the clicked colour channel.
            m_enabled[R] = m_enabled[G] = m_enabled[B] = false;
            m_enabled[A] = false;
            m_enabled[i] = true;
        } else if (m_enabled[i]) {
            // Turning a colour off: keep at least one visible (never a blank canvas).
            const int on = (m_enabled[R] ? 1 : 0) + (m_enabled[G] ? 1 : 0) + (m_enabled[B] ? 1 : 0);
            if (on > 1)
                m_enabled[i] = false;
        } else {
            m_enabled[i] = true; // turning a colour back on
        }
        break;
    }
    case Channel::Alpha:
        if (!m_enabled[A]) {
            // Enter the alpha view (exclusive): show only the alpha channel.
            m_enabled[R] = m_enabled[G] = m_enabled[B] = false;
            m_enabled[A] = true;
        } else {
            // Leave it: back to the normal full-colour view.
            m_enabled[R] = m_enabled[G] = m_enabled[B] = true;
            m_enabled[A] = false;
        }
        break;
    }

    syncMaster();
    if (m_histView != nullptr)
        m_histView->markDirty();
    redraw();
    if (viewMask() != before && m_onIsolationChanged)
        m_onIsolationChanged(); // the on-canvas view moved: let the host re-push the masked copy
}

void ChannelsPanel::draw() {
    Fl_Group::draw(); // panelBg fill (full redraw) + the histogram child
    const Palette& pal = activePalette();

    // Self-clear the row region (rows are direct-drawn, not children): keeps a child-only redraw
    // from re-stamping the labels over themselves (the layer dock's "labels get bolder" class).
    const int listTop = rowsTop() - 8;
    fl_color(toFl(pal.panelBg));
    fl_rectf(x(), listTop, w(), std::max(1, y() + h() - listTop));

    // A hairline dividing the histogram from the channel list.
    fl_color(toFl(pal.border));
    fl_line(x() + kPad, listTop, x() + w() - kPad, listTop);

    const char* names[kChannelCount] = {_("RGB"), _("Red"), _("Green"), _("Blue"), _("Alpha")};
    const double means[kChannelCount] = {m_hist.meanLuma, m_hist.meanR, m_hist.meanG, m_hist.meanB,
                                         m_hist.meanA};
    const common::Color8 swatches[kChannelCount] = {kLumaGray, kRed, kGreen, kBlue, kAlphaGray};
    const bool haveData = !m_hist.empty();

    for (int i = 0; i < kChannelCount; ++i) {
        const int ry = rowsTop() + i * kChanRowH;
        const bool en = m_enabled[static_cast<std::size_t>(i)];
        const int cy = ry + kChanRowH / 2;

        if (i == m_hoverRow) {
            fl_color(toFl(pal.controlHover));
            fl_rectf(x() + kPad - 4, ry, w() - 2 * kPad + 8, kChanRowH);
        }

        // Visibility eye (toggles this channel's histogram band).
        const common::Color8 eyeInk =
            en ? (i == m_hoverRow ? pal.accent : pal.text) : pal.textMuted;
        drawIcon(en ? Icon::EyeOpen : Icon::EyeClosed, x() + kPad + kEyeW / 2, cy, eyeInk);

        // Colour swatch (dimmed while the channel is hidden).
        const int swX = x() + kPad + kEyeW + 2;
        const int swY = cy - kSwatch / 2;
        const common::Color8 sw = en ? swatches[i] : mix(swatches[i], pal.panelBg, 0.55);
        fl_color(toFl(sw));
        fl_rectf(swX, swY, kSwatch, kSwatch);
        fl_color(toFl(pal.border));
        fl_rect(swX, swY, kSwatch, kSwatch);

        // Name.
        const int nameX = swX + kSwatch + 8;
        const int statX = x() + w() - kPad - kStatW;
        const int nameW = std::max(10, statX - 6 - nameX);
        fl_font(FL_HELVETICA, 12);
        fl_color(toFl(en ? pal.text : pal.textMuted));
        fl_draw(names[i], nameX, ry, nameW, kChanRowH, FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

        // Mean value (0..255), right-aligned.
        if (haveData) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.0f", means[i]);
            fl_color(toFl(pal.textMuted));
            fl_draw(buf, statX, ry, kStatW, kChanRowH, FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
        }
    }
}

int ChannelsPanel::handle(int event) {
    switch (event) {
    case FL_PUSH:
        if (Fl::event_button() == FL_LEFT_MOUSE) {
            const int r = rowAt(Fl::event_y());
            if (r >= 0) {
                toggleChannel(static_cast<Channel>(r));
                return 1;
            }
        }
        break;
    case FL_MOVE:
    case FL_ENTER: {
        const int r = rowAt(Fl::event_y());
        if (r != m_hoverRow) {
            m_hoverRow = r;
            redraw();
        }
        return 1;
    }
    case FL_LEAVE:
        if (m_hoverRow != -1) {
            m_hoverRow = -1;
            redraw();
        }
        return 1;
    default:
        break;
    }
    return Fl_Group::handle(event);
}

} // namespace mosaic::ui
