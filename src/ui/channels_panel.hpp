#pragma once

#include "common/image.hpp" // common::Image (the histogram source), common::Color8

#include <FL/Fl_Group.H>
#include <FL/Fl_Widget.H>

#include <array>
#include <vector>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

class Fl_RGB_Image;

namespace mosaic::core {
class Document;
}

namespace mosaic::render {
class HistogramGpu;
}

// The right dock's **Channels** tab (a sibling of Layers | History, S16-b): the image's colour
// channels listed with a per-channel visibility toggle, above a professional, colour-coded
// HISTOGRAM. The histogram source is the app's on-screen composite of the visible layers
// (MainWindow::m_lastComposite) — the same pixels the canvas shows — pulled through a provider
// callback so the panel never holds or copies the image, and re-binned only when that composite
// actually changes (keyed on MainWindow::m_compositeRevision, and then on a settle rather than on
// the bump itself — see notifyChanged) or when the tab is (re)shown. It is
// a display + inspection view: the eye toggles pick which channel bands the histogram draws, AND
// (S16-b follow-up) which channels the canvas itself shows. That on-canvas isolation is a
// present-pass parameter, not a pixel edit — canvas_present.comp applies it where it samples the
// composite (S60-a item 10) — so nothing on this side of the app ever sees an isolated pixel.
namespace mosaic::ui {

class ChannelsPanel;

// A 256-bin-per-channel histogram of an RGBA image, plus the per-channel means used for the row
// stats. `luma` is the Rec.601 luminance distribution (the neutral "RGB composite" band). Pure
// data — computed by computeHistogram (no FLTK), so it is unit-testable.
struct ChannelHistogram {
    // ⚠ The colour bins are ALPHA-WEIGHTED and their unit is "alpha", not "pixels": a fully opaque
    // pixel contributes 255, a half-transparent one 128, a fully transparent one nothing. Only
    // ratios between bins are meaningful, which is all the view (and any sane reader) uses. The
    // ALPHA bins are plain pixel counts -- the alpha distribution is precisely what that channel
    // describes, so dropping transparent pixels there would hide how much canvas is empty.
    std::array<std::uint64_t, 256> r{};
    std::array<std::uint64_t, 256> g{};
    std::array<std::uint64_t, 256> b{};
    std::array<std::uint64_t, 256> a{};
    std::array<std::uint64_t, 256> luma{};
    std::uint64_t totalPixels = 0;  // every pixel examined, transparent ones included
    std::uint64_t visibleAlpha = 0; // sum of alpha == the colour bins' total weight
    // meanR/G/B/Luma are alpha-weighted (the mean colour you can actually SEE); meanA is a plain
    // mean over every pixel. All are 0 for a fully transparent image, which has no visible colour.
    double meanR = 0.0, meanG = 0.0, meanB = 0.0, meanA = 0.0, meanLuma = 0.0;

    void clear() { *this = ChannelHistogram{}; }
    [[nodiscard]] bool empty() const noexcept { return totalPixels == 0; }
};

// Bin every pixel of `img` (straight-alpha RGBA8) into a ChannelHistogram.
//
// A histogram describes what you can SEE. The composite carries anything uncovered as transparent
// BLACK, so binning a transparent pixel's stored RGB like any other invents a black pixel that is
// not on screen -- and since an uncovered canvas is the common case, not the exotic one, it planted
// a huge false spike at bin 0 in R, G and B (user-reported 2026-07-23: "major spikes of colour even
// when the colour is evenly distributed"). Soft brush edges and feathered selections smeared the
// same error across the low bins. Excluding the 0/255 endpoints from the view's normalization was
// only ever a band-aid over this: it rescaled the plot but still drew the phantom bar, and did
// nothing for the near-black bins.
//
// So colour is weighted by coverage: a pixel 10% visible contributes 10% of a count. Fully
// transparent pixels contribute nothing at all. Pure.
//
// ⚠ THIS IS THE REFERENCE. render::HistogramGpu (S60-e) reproduces it on the device and
// tests/test_histogram_gpu.cpp holds the two BYTE-IDENTICAL -- bins are integer counts, so there is
// no tolerance to hide a defect behind and none is wanted. A retune here fails parity there.
[[nodiscard]] ChannelHistogram computeHistogram(const common::Image& img);

// The seam the panel bins THROUGH (S60-e; docs/s60-readback-consumers.md consumer A5, whose
// disposition is "can be computed GPU-side instead"): fill `out` from `img` and return true, or
// return false to let computeHistogram serve. A refusal is an ordinary outcome -- no device, CPU-
// only mode, a canvas past the lane's cap -- and it moves the work, never the bins.
using HistogramBinner = std::function<bool(const common::Image& img, ChannelHistogram& out)>;

// The device lane as a binner. `lane` must outlive the returned function; a null lane yields an
// EMPTY binner, i.e. the CPU reference, which is what the default build gets.
//
// The reconstruction of the totals and means from the lane's five bin arrays lives inside this
// function, next to the reference it has to match: everything computeHistogram reports is linear in
// the bins it fills (sum_v v*r[v] IS its alpha-weighted red sum, sum_v r[v] IS its visible-alpha
// weight, sum_v a[v] IS its pixel count), so the whole struct is recoverable exactly -- integer
// addition is associative, and the two float steps are then performed in the reference's own order.
[[nodiscard]] HistogramBinner gpuHistogramBinner(render::HistogramGpu* lane);

// The bin height the histogram plot scales to full height. Bars above it CLIP (and are drawn with
// a bright cap so the plot never silently understates them).
//
// Not the tallest bin, which is what a naive plot uses and what made the histogram unreadable
// (user-reported 2026-07-23, "still doesn't appear to be all that trustable"). A single dominant
// bin is not an exotic case -- it is what any large area of near-constant colour produces, and it
// does not need the whole image to be flat. Diagnosed on the reported document: its hills hold red
// at exactly 34 and blue at exactly 48 while only green varies, so R and B each pile ~20x higher
// than anything else and every real feature -- the sky's blue plateau, the hills' green spread --
// was crushed into a one-pixel sliver along the bottom. The DATA was right; the scale was useless.
//
// So: the `pct`-th percentile of the NON-EMPTY bins across the given bands. It adapts on its own --
// a genuinely flat histogram normalizes to its own level and fills the plot, a spiky one lets the
// outliers clip and shows the bulk. Returns at least 1 (never divides by zero).
//
// The default is 99, chosen by measuring the reported document rather than picked for roundness.
// Swept against it, the percentile trades "how much of the bulk is readable" against "how much
// real structure gets flat-topped at the ceiling", and the shape of that trade is sharp:
//
//     pct   normalizer   bins clipped   the hills' green plateau
//      90         6798             68   flat-topped over 24 bins  -- shape destroyed
//      95        10968             34   flat-topped over 20 bins
//      97        17825             20   flat-topped over 14 bins
//      99        22097              6   intact
//     100(max)  287402              0   bulk crushed to 3.8% of plot height -- the reported bug
//
// 99 is where only the genuine outliers clip (the two towers at 287402 and 268901) while every
// real feature keeps its shape, and it still lifts the bulk by 13x over the naive maximum.
[[nodiscard]] std::uint64_t histogramNormalizer(
    const std::vector<const std::array<std::uint64_t, 256>*>& bands, int pct = 99);

// Which channel a row / a histogram band represents. `Luma` is the neutral "RGB composite" row.
enum class Channel : std::uint8_t { Luma, Red, Green, Blue, Alpha };
inline constexpr int kChannelCount = 5;

// The on-canvas CHANNEL-VIEW derived from the eye toggles — what gets shown on the canvas (a
// display-only remap; the true composite stays untouched). Semantics (Photoshop-flavoured):
//   * all three colour channels on, alpha off  -> NORMAL: the image passes through unchanged;
//   * exactly one colour channel on            -> that channel as GRAYSCALE (rgb = its value);
//   * two colour channels on                   -> those shown, the hidden one ZEROED;
//   * alpha on (exclusive)                      -> the alpha channel as grayscale, rgb=(a,a,a), a=255.
// Colour views keep the pixel's own alpha (transparent regions still read through the checkerboard);
// the alpha view forces opaque so the coverage values themselves are what you see.
struct ChannelViewMask {
    bool red = true;
    bool green = true;
    bool blue = true;
    bool alpha = false; // the exclusive alpha-isolation view (overrides the colour flags)
    bool operator==(const ChannelViewMask&) const = default;
    // NORMAL == the full-colour composite: nothing to remap.
    [[nodiscard]] bool isNormal() const noexcept { return red && green && blue && !alpha; }
};

// Remap `img` (straight-alpha RGBA8) in place to the channel view `mask` describes. A no-op for the
// normal view or an empty image. Pure (no FLTK) → unit-tested.
//
// ⚠ This is the CPU form, and since S60-a item 10 it is no longer how the CANVAS shows an
// isolation: the canvas remap lives in canvas_present.comp, applied where the present pass samples
// the composite (see channelViewShaderCode). This stays the SPECIFICATION the shader reproduces —
// and the thing any future non-display consumer (an export of the isolated view, say) would use.
void applyChannelViewMask(common::Image& img, const ChannelViewMask& mask);

// ---- The on-device form of the same mask (S60-a item 10) --------------------------------------
// How canvas_present.comp's `channelView` encodes a ChannelViewMask. The bits live HERE, not in
// render/, because what they mean is a UI concept: the renderer only carries the code to the shader
// (render::WindowRenderer::setChannelView takes a plain uint for exactly that reason).
//
// NORMAL is 0 and is the shader's identity early-out — which also makes it the safe value for a
// renderer nobody has told about a view. Every other view is kChannelViewActive OR'd with one bit
// per SHOWN channel, and the marker bit is load-bearing: without it, "every colour channel hidden"
// (a legal view — applyChannelViewMask renders it black) would encode as 0 and read as normal.
inline constexpr std::uint32_t kChannelViewNormal = 0x00u;
inline constexpr std::uint32_t kChannelViewRed = 0x01u;
inline constexpr std::uint32_t kChannelViewGreen = 0x02u;
inline constexpr std::uint32_t kChannelViewBlue = 0x04u;
inline constexpr std::uint32_t kChannelViewAlpha = 0x08u;
inline constexpr std::uint32_t kChannelViewActive = 0x10u;

// `mask` as the present shader reads it. Pure → unit-tested against applyChannelViewMask itself.
[[nodiscard]] std::uint32_t channelViewShaderCode(const ChannelViewMask& mask) noexcept;

// The custom histogram widget (the task's "custom widget"): it renders the enabled channel bands
// into an offscreen RGBA raster — anti-aliased filled areas, a subtle grid + frame, additive on a
// dark theme / subtractive on a light one so the colour coding reads either way — and blits it,
// caching the raster until something it depends on (bins, the enabled set, size, theme) changes.
// It reads the bins + the enabled flags from its owning ChannelsPanel (the HistoryRow pattern).
class HistogramView : public Fl_Widget {
public:
    HistogramView(int X, int Y, int W, int H, ChannelsPanel* panel);

    // Rebuild the raster on the next draw (bins / enabled set / theme changed).
    void markDirty();
    void resize(int X, int Y, int W, int H) override;

protected:
    void draw() override;

private:
    void rebuild(); // (re)render the raster from the panel's histogram + enabled channels

    ChannelsPanel* m_panel;
    common::Image m_raster;               // the rendered plot (opaque RGBA), the Fl_RGB_Image's backing
    std::unique_ptr<Fl_RGB_Image> m_img;  // views m_raster's pixels; rebuilt with it (order matters)
    bool m_dirty = true;
};

class ChannelsPanel : public Fl_Group {
public:
    ChannelsPanel(int X, int Y, int W, int H);
    ~ChannelsPanel() override; // drops the settle timer (an FLTK timeout outliving its widget is a UAF)

    // Point the panel at the document (non-owning; null clears). Only used for the empty-state and
    // to gate the rows; the histogram pixels come from the composite provider below.
    void setDocument(core::Document* doc);

    // Hand the panel the way to reach the current on-screen composite (MainWindow::m_lastComposite).
    // The panel never stores the image — it reads through this at (re)compute time — so there is no
    // copy per recomposite and no lifetime coupling to the pixels.
    void setSourceProvider(std::function<const common::Image*()> get) { m_source = std::move(get); }

    // Install the device binning lane (S60-e), or clear it with an empty function. The panel never
    // BUILDS one: a lane constructed lazily on the first re-bin is a Vulkan device creation on the
    // frame path wearing a cache miss as a disguise. The host owns the lane's lifetime and hands it
    // over once; see gpuHistogramBinner above.
    void setBinner(HistogramBinner fn) { m_binner = std::move(fn); }

    // The host calls this after every composite-revision bump, passing MainWindow::m_compositeRevision.
    // The bins are re-derived only when the revision differs from what they were last built from AND
    // this tab is visible — so an edit on another tab, or a live drag the user is not watching, costs
    // nothing here. onTabShown() catches up the deferred case.
    //
    // ⚠ THE RE-BIN IS COALESCED ONTO A SETTLE, and that is a correctness property of the frame path
    // rather than a nicety. Since the region writer started notifying too (S60-a; the F2 fix in
    // docs/s60-readback-consumers.md §10.3) this fires on every revision bump — potentially once per
    // brush dab — and each one used to re-bin the WHOLE canvas synchronously. A5's latency tolerance
    // is "high" precisely so it does not have to: nobody reads a histogram mid-stroke. So a bump
    // schedules, it does not bin, and the device lane — which would otherwise put an upload and a
    // fence on every dab — can never become a new fence on the frame path.
    void notifyChanged(std::uint64_t revision);

    // Called by the dock the moment this tab becomes visible (before the first paint): pull the
    // current composite and re-bin SYNCHRONOUSLY, so the histogram is always current when it is
    // looked at. This is the catch-up for everything notifyChanged deferred or dropped — an edit
    // made while another tab was up, or a settle that was still pending when the tab was hidden.
    void onTabShown();

    void reapplyTheme(); // runtime theme change: re-render the raster in the new palette

    void resize(int X, int Y, int W, int H) override;

    // Read by HistogramView.
    [[nodiscard]] const ChannelHistogram& histogram() const noexcept { return m_hist; }
    [[nodiscard]] bool channelEnabled(Channel c) const noexcept {
        return m_enabled[static_cast<std::size_t>(c)];
    }

    // ---- On-canvas channel isolation (the eyes drive the display, not just the histogram) ------
    // The channel view the eyes currently describe, and whether it is anything but the normal
    // composite. Since S60-a item 10 the host does not remap any pixels for this: it hands
    // viewShaderCode() to the renderer and canvas_present.comp applies the view where it samples
    // the composite. So the canvas texture — like m_lastComposite behind it — keeps holding the
    // TRUE composite, which is what the histogram, the cursor colour readout and Smart Resize read.
    [[nodiscard]] ChannelViewMask viewMask() const noexcept;
    [[nodiscard]] bool isolationActive() const noexcept { return !viewMask().isNormal(); }
    // The active view as canvas_present.comp reads it; kChannelViewNormal while nothing is isolated.
    [[nodiscard]] std::uint32_t viewShaderCode() const noexcept {
        return channelViewShaderCode(viewMask());
    }
    // Remap `img` in place to the active channel view (a no-op while normal). NOT the canvas path
    // any more (see above) — it remains for any consumer that needs the isolated view as PIXELS.
    void applyIsolation(common::Image& img) const { applyChannelViewMask(img, viewMask()); }
    // Fired when toggling an eye actually changes the on-canvas view, so the host can push the new
    // code and draw a frame. No recomposite, no re-upload, no re-mask: the view is a present-pass
    // parameter now.
    void setOnIsolationChanged(std::function<void()> cb) { m_onIsolationChanged = std::move(cb); }

protected:
    void draw() override;           // the channel rows (eye + swatch + name + mean), on the panel ground
    int handle(int event) override; // row clicks toggle a channel's band; hover highlight

private:
    void layoutChildren();
    void recompute();                 // pull the composite via m_source and re-bin, NOW
    void scheduleRecompute();         // ... or on the settle, which is what an edit gets
    static void settleTimer(void* data); // the FLTK timeout thunk for the above
    void toggleChannel(Channel c);
    void syncMaster();                // keep the RGB-composite row's eye = isNormal()
    [[nodiscard]] int rowsTop() const;         // y of the first channel row
    [[nodiscard]] int rowAt(int eventY) const; // channel index under eventY, or -1

    core::Document* m_doc = nullptr;
    std::function<const common::Image*()> m_source;
    HistogramBinner m_binner;        // the device lane, or empty for the CPU reference
    ChannelHistogram m_hist;
    std::uint64_t m_computedRev = 0; // the revision m_hist was built from (0 = none yet)
    std::uint64_t m_latestRev = 0;   // the newest revision the host has announced
    // When the bins were last actually rebuilt. The settle waits for the edits to stop, but never
    // longer than kMaxDeferSec — a drag that runs for seconds should not freeze the plot for them.
    std::chrono::steady_clock::time_point m_lastBin{};
    HistogramView* m_histView = nullptr;
    std::function<void()> m_onIsolationChanged; // fired when the on-canvas channel view changes
    // Per-channel visibility (indexed by Channel), driving BOTH the histogram bands and the on-canvas
    // view. Default = the normal composite: the RGB master on, all three colour channels on, alpha
    // off (the histogram then shows the classic overlaid R/G/B). Luma mirrors isNormal() (see
    // syncMaster) so the RGB row's eye reads open exactly when the canvas shows the full composite.
    std::array<bool, kChannelCount> m_enabled{true, true, true, true, false};
    int m_hoverRow = -1;
};

} // namespace mosaic::ui
