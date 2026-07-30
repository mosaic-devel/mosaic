#pragma once

#include "common/geometry.hpp" // Rect
#include "common/image.hpp"    // Color8

#include <FL/Fl_Group.H>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

// The status bar (PLAN S13-b): a slim strip along the main window's bottom edge showing, left
// to right -- the document size (px + physical size at the document ppi) and bit depth; the live
// cursor position in document coordinates with the colour under it (a chip + hex + RGBA) while
// the pointer is over the canvas, the colour dropping out entirely over a fully transparent
// texel (nothing there to name); the active selection's bounds; and on the right, a status/
// progress area (wired up as workers arrive, §4), the colour-space indicator (moved here from
// its interim S12-b home in the picker; the HDR indicator joins at S43-c), and zoom % +
// rotation°. Readouts update on *events* (mouse move, selection/zoom change), never from the
// frame loop. The formatting helpers below are pure and unit-tested; the future Info panel (§11)
// reads the same CursorReadout so the two surfaces can never disagree.
namespace mosaic::ui {

class ScrollingLabel;

inline constexpr int kStatusBarHeight = 24;

// What the pointer is over, in document terms: the shared pixel-readout model. `color` is the
// real composited pixel value with true alpha (since S19-c the checkerboard is a screen-space
// present effect, not baked into the composite), valid only when `insideDocument`. Because the
// alpha is true, `color.a == 0` means the document really is empty there -- and the strip then
// draws no colour section at all, chip and text both.
struct CursorReadout {
    double docX = 0.0;
    double docY = 0.0;
    bool insideDocument = false;
    common::Color8 color{};
    friend bool operator==(const CursorReadout&, const CursorReadout&) = default;
};

class StatusBar : public Fl_Group {
public:
    StatusBar(int X, int Y, int W, int H);

    // The shown document changed (File->New / open): size, resolution, bit depth.
    void setDocumentInfo(std::uint32_t w, std::uint32_t h, double ppi, std::string_view precision);
    // No document is open (the S50-prerequisite empty startup state): blank the readout.
    void clearDocumentInfo();
    // The measurement system changed (Settings -> General -> Units): re-express the physical size
    // in cm (metric) or in (imperial). Re-formats the stored document info; no-op if unchanged.
    void setMetric(bool metric);
    // The working colour space / ICC profile name (can be arbitrarily long -> ScrollingLabel).
    void setColorSpaceName(const std::string& name);
    // The pointer moved over the canvas (nullopt = it left the canvas; the readout clears).
    void setCursor(const std::optional<CursorReadout>& readout);
    // The view transform changed (zoom/rotation; pushed by the canvas on changes, not per frame).
    void setViewState(double zoom, double rotationDegrees);
    // The selection changed (nullopt = no selection / nothing selected; the readout clears).
    void setSelectionBounds(const std::optional<common::Rect>& bounds);
    // Free-form status/progress text (long worker operations land here, §4; "" hides).
    void setStatus(const std::string& text);
    // Seconds the current status message needs to scroll through once (0 if it fits / none); the
    // transient-status auto-clear uses this so a long message completes at least one pass.
    [[nodiscard]] double statusScrollSeconds() const;

    // ---- Worker progress (a determinate bar + stage label + cancel X) in the transient area ----
    // Show/update the progress strip: `fraction` 0..1, `stage` a short label ("Analyzing"…). While
    // shown it takes over the left flow (the doc-info/cursor/selection readouts are suppressed) so
    // a long operation reads clearly; the X fires the callback set with onProgressCancel.
    void setProgress(float fraction, const std::string& stage);
    void hideProgress();
    [[nodiscard]] bool progressVisible() const;
    void onProgressCancel(std::function<void()> cancel);

    // Runtime theme change: re-apply the cached colours on the colour-space label (the strip itself
    // draws live). The group draws live, so the rest re-themes on the global redraw.
    void reapplyTheme();

protected:
    void draw() override;
    void resize(int X, int Y, int W, int H) override; // re-pin the right-aligned children

private:
    void placeChildren();

    std::string m_docInfo;
    // Raw document metrics kept so setMetric() can re-format the physical size without the caller
    // re-supplying them.
    std::uint32_t m_docW = 0;
    std::uint32_t m_docH = 0;
    double m_docPpi = 0.0;
    std::string m_docPrecision;
    bool m_metric =
        true; // physical-size unit: true = cm, false = in (MainWindow sets the resolved)
    std::optional<CursorReadout> m_cursor;
    std::string m_viewState;
    std::string m_selection; // "" = none
    std::string m_status;
    ScrollingLabel* m_spaceLabel = nullptr;    // the colour-space indicator (right side)
    // The transient status message (lockedAttempt hints, merge-down errors, …): a left-aligned
    // ScrollingLabel sharing the progress strip's area, so a long message scrolls instead of
    // overrunning into the colour-space slot. Shown only while m_status is non-empty.
    ScrollingLabel* m_statusLabel = nullptr;
    class ProgressStrip* m_progress = nullptr; // determinate worker bar + cancel X (hidden at rest)
};

// ---- pure formatting helpers (unit-tested; FLTK-free) ----------------------------------------

// "1920 × 1080 px · 67.7 × 38.1 cm @ 72 ppi · 8-bit integer" (metric), or "... in ..." (imperial).
[[nodiscard]] std::string formatDocumentInfo(std::uint32_t w, std::uint32_t h, double ppi,
                                             std::string_view precision, bool metric);
// "X 123  Y 456" (document px, floored -- the texel the cursor is in).
[[nodiscard]] std::string formatCursorPosition(double docX, double docY);
// "#5E7EFF · 94, 126, 255, 255"
[[nodiscard]] std::string formatColorReadout(common::Color8 c);
// "100% · 0.0°" (zoom is a factor: 1.0 -> "100%"; trims to 4 significant digits).
[[nodiscard]] std::string formatViewState(double zoom, double rotationDegrees);
// "200 × 100 @ (10, 20)" (the selection's tight bounds, document px).
[[nodiscard]] std::string formatSelectionBounds(const common::Rect& r);

} // namespace mosaic::ui
