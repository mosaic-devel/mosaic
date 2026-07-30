#include "ui/about_dialog.hpp"

#include "common/i18n.hpp"
#include "common/image.hpp"
#include "common/image_svg.hpp"
#include "common/version.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/filename.H> // fl_open_uri
#include <FL/fl_draw.H>
#include <algorithm>
#include <array>
#include <assets/app_icon_svg.hpp> // generated: mosaic::assets::app_icon_svg[]
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace mosaic::ui {
namespace {

// ---- Layout (all in dialog-local pixels) ------------------------------------------------------
constexpr int kWidth = 480;
constexpr int kHeight = 330;
constexpr int kMargin = 24;

constexpr int kLogoSize = 72;
constexpr int kLogoX = kWidth - kMargin - kLogoSize;
constexpr int kLogoY = kMargin;

constexpr int kTitleSize = 46;     // the "Mosaic" wordmark
constexpr int kTitleBaseline = 78; // sits in the same band as the logo
constexpr int kLetterTracking = 1; // extra px between glyphs

constexpr int kVersionSize = 15;
constexpr int kVersionBaseline = 128;
constexpr int kBySize = 14;
constexpr int kByNameSize = 14;
constexpr int kByBaseline = 158;
constexpr int kCreditsRowH = 22; // the credit drum's visible window (one line tall)

constexpr int kDescSize = 13;
constexpr int kDescBaseline1 = 202;
constexpr int kDescBaseline2 = 223;

constexpr int kCloseW = 100;
constexpr int kCloseH = 30;

// The ghosted logo watermark behind everything: a large app icon, tilted and drawn at a low
// opacity. Baked once into the background blit (rebuilt only on a theme change), so it costs
// nothing per frame. It only tints the ground, so the (glyph-only) text and credit drum read over
// it cleanly; the drum's fade-to-windowBg is off by at most a few levels over so faint a wash.
// Anchored at the top-right and bled ~half off the right edge, sitting behind the real logo as a
// subtle accent (centre on the right edge, level with the logo).
constexpr int kWatermarkSize = 300;
constexpr double kWatermarkOpacity = 0.05;
constexpr double kWatermarkAngleDeg = -18.0;
constexpr double kWatermarkCx = kWidth;                  // centre on the right edge -> half off
constexpr double kWatermarkCy = kLogoY + kLogoSize / 2.0; // level with the logo

// The "muted repo/website link" in the bottom-left.
constexpr const char* kRepoUrl = "https://github.com/mosaic-devel/mosaic";
constexpr const char* kRepoLabel = "github.com/mosaic-devel/mosaic";
constexpr const char* kLicenceUrl = "https://www.gnu.org/licenses/gpl-3.0.html";
constexpr int kRepoBaseline = kHeight - kMargin - kCloseH / 2 + 5; // aligned with the Close button

// ---- Animation timing -------------------------------------------------------------------------
constexpr double kFrameS = 1.0 / 60.0;
constexpr double kLetterStaggerS = 0.085; // gap between successive glyphs taking off
constexpr double kLetterDurS = 0.52;      // one glyph's flight time
constexpr double kDx0 = -30.0;            // glyph start offset: to the left of home...
constexpr double kDy0 = -66.0;            // ...and above it, so it swoops down into place
constexpr double kBow = 16.0;             // lateral bow that bends the flight into a curve

constexpr double kCreditStartDelayS = 0.9; // let the wordmark land before the drum starts cycling
constexpr double kCreditHoldS = 1.9;       // dwell on a name
constexpr double kCreditSlideS = 0.55;     // slide to the next

constexpr double kPi = 3.14159265358979323846;

double nowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

Fl_Color toFl(common::Color8 c) { return fl_rgb_color(c.r, c.g, c.b); }

common::Color8 lerp(common::Color8 a, common::Color8 b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    const auto ch = [t](std::uint8_t x, std::uint8_t y) {
        return static_cast<std::uint8_t>(std::lround(x + (static_cast<double>(y) - x) * t));
    };
    return {ch(a.r, b.r), ch(a.g, b.g), ch(a.b, b.b), 255};
}

double easeOutCubic(double t) { return 1.0 - std::pow(1.0 - t, 3.0); }

// Slight overshoot at the end -- the glyph drops a hair past home and settles, reading as a "pop".
double easeOutBack(double t) {
    constexpr double c1 = 1.70158;
    constexpr double c3 = c1 + 1.0;
    const double u = t - 1.0;
    return 1.0 + c3 * u * u * u + c1 * u * u;
}

double easeInOutCubic(double t) {
    return t < 0.5 ? 4.0 * t * t * t : 1.0 - std::pow(-2.0 * t + 2.0, 3.0) / 2.0;
}

double smoothstep(double e0, double e1, double x) {
    const double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// The wordmark. Drawn glyph-by-glyph so each can fly its own arc; the string is the app name and is
// deliberately NOT translated (a brand mark), unlike the dialog's prose.
const std::string& wordmark() {
    static const std::string kName = "Mosaic";
    return kName;
}

// The credit drum's contents. For now a hand-kept list -- the author plus the AI pair-programmer.
//
// Baking in real outside contributors later: don't grow this array by hand. At configure/build time
// generate it from a curated source -- a checked-in CONTRIBUTORS file, or `git shortlog -sne` run
// through .mailmap to fold duplicate identities and drop bot/CI accounts -- and emit it as a
// generated header (the same mechanism the embedded assets use). Then the binary's credits always
// track the repository without a code edit, and the .mailmap keeps it from turning into a mess.
const std::vector<std::string>& contributors() {
    static const std::vector<std::string> kNames = {
        "mosaic-devel",
        "Claude (Opus 4.8 & Fable 5)",
    };
    return kNames;
}

common::Image appIconImage(int px) {
    return common::rasterizeSvg(mosaic::assets::app_icon_svg, mosaic::assets::app_icon_svg_size, px,
                                px, nullptr);
}

// Decorative extras, enabled only during the twelfth month of the local calendar; a no-op the rest
// of the year (and never built or drawn when inactive).
bool eggSeasonActive() {
    const std::time_t t = std::time(nullptr);
    std::tm lt{};
#if defined(_WIN32)
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    return lt.tm_mon == 11;
}

struct EggMote {
    double x = 0.0;
    double y = 0.0;
    double vy = 0.0;      // fall speed (px/s)
    double swayAmp = 0.0; // lateral sway velocity amplitude (px/s)
    double swayRate = 0.0;
    double phase = 0.0;
    int bucket = 0; // which sprite (size/brightness)
};

// ----------------------------------------------------------------------------------------------

// A cached clickable region (text drawn by the window, so hit-tested by rect, not by a child).
struct LinkRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    [[nodiscard]] bool contains(int px, int py) const {
        return w > 0 && px >= x && px < x + w && py >= y && py < y + h;
    }
};
enum class LinkTarget { None, Licence, Repo, Version };

// Draw `s` as a link (faint-accent at rest, full accent on hover) with an underline, recording its
// hit rect in `out`. The caller sets the font first; returns the advance width.
int drawLink(const char* s, int x, int baseline, bool hovered, const Palette& pal, LinkRect& out) {
    fl_color(toFl(hovered ? pal.accent : lerp(pal.textMuted, pal.accent, 0.4)));
    fl_draw(s, x, baseline);
    const int wpx = static_cast<int>(std::ceil(fl_width(s)));
    fl_line(x, baseline + 2, x + wpx - 1, baseline + 2);
    const int asc = fl_height() - fl_descent();
    out = LinkRect{x, baseline - asc, wpx, asc + fl_descent() + 3};
    return wpx;
}

class AboutWindow : public Fl_Double_Window {
public:
    AboutWindow();
    ~AboutWindow() override;

    // Begin the redraw heartbeat (call after show()+position()). Resets the animation clock so the
    // wordmark flies in the moment the dialog appears, not when it was constructed.
    void arm();

protected:
    void draw() override;
    int handle(int event) override; // links (clickable text) + Enter-to-close

private:
    static void onClose(Fl_Widget*, void* self) { static_cast<AboutWindow*>(self)->hide(); }
    static void tick(void* self);

    [[nodiscard]] LinkTarget hitLink(int px, int py) const;
    void activateLink(LinkTarget t);

    void buildBackground(const Palette& pal); // windowBg + the tilted ghost-logo watermark
    void paintLogo();
    void paintTitle(const Palette& pal);
    void paintMeta(const Palette& pal);
    void paintCredits(const Palette& pal, int x, int baseline);
    void paintDescription(const Palette& pal);
    void paintRepoLink(const Palette& pal);

    void buildMoteSprites();
    void seedMotes();
    void stepMotes(double dt);
    void drawEggLights(const Palette& pal);
    void drawMotes();

    FlatButton* m_close = nullptr;
    double m_t0 = 0.0;
    double m_lastT = 0.0;
    bool m_armed = false;

    common::Image m_logoBuf;                // owns the pixels Fl_RGB_Image references
    std::unique_ptr<Fl_RGB_Image> m_logoImg;

    common::Image m_bgBuf;          // pre-composited ground (windowBg + watermark), blitted per frame
    common::Color8 m_bgBg{0, 0, 0, 0}; // the windowBg m_bgBuf was baked with (rebuild on a re-theme)

    LinkTarget m_hoverLink = LinkTarget::None;
    LinkRect m_versionRect;  // click to copy the build info
    LinkRect m_licenceRect;  // "GPLv3" in the description -> opens the licence
    LinkRect m_repoRect;     // bottom-left repo/website link
    double m_copiedFlashUntil = 0.0; // show a brief "copied" note after a version-line click

    bool m_egg = false;
    std::mt19937 m_rng{0xA11CE};
    std::vector<EggMote> m_motes;
    std::array<common::Image, 3> m_moteBuf;                  // pixel backing for the sprites
    std::array<std::unique_ptr<Fl_RGB_Image>, 3> m_moteImg;  // soft-dot sprites (small..large)
    std::array<int, 3> m_moteR{2, 3, 4};                     // sprite radius per bucket
};

AboutWindow::AboutWindow() : Fl_Double_Window(kWidth, kHeight, _("About Mosaic")) {
    color(toFl(activePalette().windowBg));
    m_egg = eggSeasonActive();

    m_logoBuf = appIconImage(kLogoSize);
    if (!m_logoBuf.empty()) {
        m_logoImg = std::make_unique<Fl_RGB_Image>(m_logoBuf.rgba.data(),
                                                   static_cast<int>(m_logoBuf.width),
                                                   static_cast<int>(m_logoBuf.height), 4);
    }
    if (m_egg) {
        buildMoteSprites();
        seedMotes();
    }

    begin();
    m_close = new FlatButton(kWidth - kMargin - kCloseW, kHeight - kMargin - kCloseH, kCloseW,
                             kCloseH, _("Close"));
    m_close->callback(onClose, this);
    end();

    callback(onClose, this); // Escape / window-manager close
    set_modal();             // one top-level: dodges the native-Wayland stray-window trap
}

AboutWindow::~AboutWindow() { Fl::remove_timeout(tick, this); }

void AboutWindow::arm() {
    m_t0 = nowSeconds();
    m_lastT = m_t0;
    if (!m_armed) {
        Fl::add_timeout(kFrameS, tick, this);
        m_armed = true;
    }
}

void AboutWindow::tick(void* self) {
    auto* w = static_cast<AboutWindow*>(self);
    if (!w->shown()) { // closed: let the timer lapse
        w->m_armed = false;
        return;
    }
    const double now = nowSeconds();
    double dt = now - w->m_lastT;
    w->m_lastT = now;
    dt = std::clamp(dt, 0.0, 0.05); // clamp a scheduling hiccup so motes never teleport
    if (w->m_egg)
        w->stepMotes(dt);
    w->redraw(); // full repaint -> children (Close) re-stack on top of the animated scene
    Fl::repeat_timeout(kFrameS, tick, self);
}

void AboutWindow::draw() {
    const Palette& pal = activePalette();
    if (m_bgBuf.empty() || m_bgBg != pal.windowBg)
        buildBackground(pal); // first frame, or the theme changed under us
    fl_draw_image(m_bgBuf.rgba.data(), 0, 0, kWidth, kHeight, 4, 0);

    paintLogo();
    if (m_egg)
        drawEggLights(pal); // strung across the top, behind the wordmark (letters fly past in front)
    paintTitle(pal);
    paintMeta(pal);
    paintDescription(pal);
    paintRepoLink(pal);

    if (m_egg)
        drawMotes(); // in front of the text -- it drifts over everything

    draw_children(); // the themed Close button, on top
}

int AboutWindow::handle(int event) {
    switch (event) {
    case FL_MOVE: {
        const LinkTarget t = hitLink(Fl::event_x(), Fl::event_y());
        m_hoverLink = t;
        cursor(t == LinkTarget::None ? FL_CURSOR_DEFAULT : FL_CURSOR_HAND);
        break; // fall through so the Close button still gets its hover
    }
    case FL_PUSH: {
        const LinkTarget t = hitLink(Fl::event_x(), Fl::event_y());
        if (t != LinkTarget::None) {
            activateLink(t);
            return 1;
        }
        break;
    }
    case FL_LEAVE:
        m_hoverLink = LinkTarget::None; // un-highlight a link when the pointer leaves the window
        cursor(FL_CURSOR_DEFAULT);
        break;
    case FL_KEYBOARD:
    case FL_SHORTCUT:
        if (Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter) {
            hide(); // Enter closes (Escape already does, via the window callback)
            return 1;
        }
        break;
    default:
        break;
    }
    return Fl_Double_Window::handle(event);
}

LinkTarget AboutWindow::hitLink(int px, int py) const {
    if (m_versionRect.contains(px, py))
        return LinkTarget::Version;
    if (m_licenceRect.contains(px, py))
        return LinkTarget::Licence;
    if (m_repoRect.contains(px, py))
        return LinkTarget::Repo;
    return LinkTarget::None;
}

void AboutWindow::activateLink(LinkTarget t) {
    switch (t) {
    case LinkTarget::Licence:
        fl_open_uri(kLicenceUrl);
        break;
    case LinkTarget::Repo:
        fl_open_uri(kRepoUrl);
        break;
    case LinkTarget::Version: {
        const std::string info = common::buildInfo(); // full name+version+rev+build, for bug reports
        Fl::copy(info.c_str(), static_cast<int>(info.size()), 1);
        m_copiedFlashUntil = nowSeconds() + 1.4;
        break;
    }
    case LinkTarget::None:
        break;
    }
}

void AboutWindow::paintLogo() {
    if (m_logoImg)
        m_logoImg->draw(kLogoX, kLogoY);
}

void AboutWindow::paintTitle(const Palette& pal) {
    fl_font(FL_HELVETICA_BOLD, kTitleSize);
    const double elapsed = nowSeconds() - m_t0;
    const std::string& name = wordmark();
    double penX = kMargin;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char buf[2] = {name[i], '\0'};
        const double glyphW = fl_width(buf);
        const double tau =
            std::clamp((elapsed - static_cast<double>(i) * kLetterStaggerS) / kLetterDurS, 0.0, 1.0);
        const double ex = easeOutCubic(tau);
        const double ey = easeOutBack(tau);
        const double gx = penX + kDx0 * (1.0 - ex) + kBow * std::sin(kPi * tau);
        const double gy = kTitleBaseline + kDy0 * (1.0 - ey);
        const double a = smoothstep(0.0, 0.38, tau); // fade in as it arrives
        const int ix = static_cast<int>(std::lround(gx));
        const int iy = static_cast<int>(std::lround(gy));
        // Soft drop shadow first (two offset passes), faded with the glyph, for a touch of depth.
        fl_color(toFl(lerp(pal.windowBg, {0, 0, 0, 255}, 0.30 * a)));
        fl_draw(buf, ix + 2, iy + 3);
        fl_color(toFl(lerp(pal.windowBg, {0, 0, 0, 255}, 0.22 * a)));
        fl_draw(buf, ix + 1, iy + 2);
        fl_color(toFl(lerp(pal.windowBg, pal.accent, a)));
        fl_draw(buf, ix, iy);
        penX += glyphW + kLetterTracking;
    }
}

void AboutWindow::paintMeta(const Palette& pal) {
    fl_font(FL_HELVETICA, kVersionSize);
    fl_color(toFl(pal.text));
    const std::string ver = std::string(_("Version ")) + std::string(common::appVersion());
    fl_draw(ver.c_str(), kMargin, kVersionBaseline);
    const int verW = static_cast<int>(std::ceil(fl_width(ver.c_str())));
    // The whole version run is clickable: it copies the full build info to the clipboard (handy for
    // bug reports). Capture its hit rect while the version font is still active.
    m_versionRect = LinkRect{kMargin, kVersionBaseline - (fl_height() - fl_descent()) - 2, verW,
                             fl_height() + 4};
    int trailX = kMargin + verW + 6; // where the rev (then the copied note) trails the version

    // The build's git revision (e.g. "+ge8025da-dirty"), muted and a hair smaller, trailing the
    // version on the same baseline -- the build-metadata the old dialog showed via buildInfo().
    const std::string rev(common::appGitRev());
    if (!rev.empty()) {
        fl_font(FL_HELVETICA, kVersionSize - 2);
        fl_color(toFl(pal.textMuted));
        const std::string revStr = "+" + rev;
        fl_draw(revStr.c_str(), trailX, kVersionBaseline);
        trailX += static_cast<int>(std::ceil(fl_width(revStr.c_str()))) + 8;
    }

    // A brief confirmation after a version-line click.
    if (nowSeconds() < m_copiedFlashUntil) {
        fl_font(FL_HELVETICA, kVersionSize - 2);
        fl_color(toFl(pal.accent));
        fl_draw(_("copied ✓"), trailX, kVersionBaseline);
    }

    fl_font(FL_HELVETICA, kBySize);
    fl_color(toFl(pal.textMuted));
    const char* by = _("by ");
    fl_draw(by, kMargin, kByBaseline);
    const int byW = static_cast<int>(std::ceil(fl_width(by)));
    paintCredits(pal, kMargin + byW, kByBaseline);
}

// The iOS-style credit drum: one visible line that holds, then slides up to the next name and
// wraps, with the leaving/entering lines fading toward the background near the clip edges.
void AboutWindow::paintCredits(const Palette& pal, int x, int baseline) {
    const std::vector<std::string>& names = contributors();
    const int n = static_cast<int>(names.size());
    if (n == 0)
        return;

    fl_font(FL_HELVETICA_BOLD, kByNameSize);
    const int rowH = kCreditsRowH;
    const int wheelW = kWidth - kMargin - x;
    const int asc = fl_height() - fl_descent();
    const int top = baseline - asc / 2 - rowH / 2; // centre the window on the text's visual middle

    const auto drawLine = [&](const std::string& s, double off) {
        const double a = std::clamp(1.0 - std::abs(off) / (rowH * 0.95), 0.0, 1.0);
        if (a <= 0.02)
            return;
        fl_color(toFl(lerp(pal.windowBg, pal.text, a)));
        fl_draw(s.c_str(), x, baseline + static_cast<int>(std::lround(off)));
    };

    fl_push_clip(x, top, wheelW, rowH);
    if (n == 1) {
        drawLine(names[0], 0.0);
    } else {
        const double cycle = kCreditHoldS + kCreditSlideS;
        const double e = std::max(0.0, (nowSeconds() - m_t0) - kCreditStartDelayS);
        const int cyclesDone = static_cast<int>(std::floor(e / cycle));
        const double phase = e - cyclesDone * cycle;
        const int cur = ((cyclesDone % n) + n) % n;
        double offset = 0.0;
        if (phase > kCreditHoldS)
            offset = easeInOutCubic(std::clamp((phase - kCreditHoldS) / kCreditSlideS, 0.0, 1.0)) *
                     rowH;
        drawLine(names[cur], -offset);
        if (offset > 0.0)
            drawLine(names[(cur + 1) % n], rowH - offset);
    }
    fl_pop_clip();
}

void AboutWindow::paintDescription(const Palette& pal) {
    fl_font(FL_HELVETICA, kDescSize);
    fl_color(toFl(pal.textMuted));
    fl_draw(_("A professional, GPU-accelerated image editor."), kMargin, kDescBaseline1);

    // "Licensed under GPLv3." with the licence name a link to the full text. Split for the clickable
    // run; "GPLv3" is a proper noun and stays untranslated.
    const char* prefix = _("Licensed under ");
    fl_draw(prefix, kMargin, kDescBaseline2);
    const int gx = kMargin + static_cast<int>(std::ceil(fl_width(prefix)));
    const int gw =
        drawLink("GPLv3", gx, kDescBaseline2, m_hoverLink == LinkTarget::Licence, pal, m_licenceRect);
    fl_color(toFl(pal.textMuted));
    fl_draw(".", gx + gw, kDescBaseline2);
}

void AboutWindow::paintRepoLink(const Palette& pal) {
    fl_font(FL_HELVETICA, kDescSize);
    drawLink(kRepoLabel, kMargin, kRepoBaseline, m_hoverLink == LinkTarget::Repo, pal, m_repoRect);
}

// Bake the ground once: windowBg with a big app-icon ghost rotated into it at a low opacity. The
// icon is rasterized large, then inverse-mapped (rotate dest->src + bilinear) so the tilt stays
// smooth, and composited over windowBg via its own alpha scaled by kWatermarkOpacity.
void AboutWindow::buildBackground(const Palette& pal) {
    const common::Color8 bg = pal.windowBg;
    common::Image out(static_cast<std::uint32_t>(kWidth), static_cast<std::uint32_t>(kHeight));
    out.fill(bg);

    const common::Image ico = appIconImage(kWatermarkSize);
    if (!ico.empty()) {
        const int S = static_cast<int>(ico.width);
        const double theta = kWatermarkAngleDeg * kPi / 180.0;
        const double ct = std::cos(theta);
        const double st = std::sin(theta);
        const double cx = kWatermarkCx;
        const double cy = kWatermarkCy;
        const double s0 = S * 0.5;
        const auto sample = [&](double su, double sv, double& r, double& g, double& b, double& a) {
            const int x0 = static_cast<int>(std::floor(su));
            const int y0 = static_cast<int>(std::floor(sv));
            if (x0 < 0 || y0 < 0 || x0 + 1 >= S || y0 + 1 >= S) {
                a = 0.0;
                return;
            }
            const double fx = su - x0;
            const double fy = sv - y0;
            const auto px = [&](int xx, int yy, int ch) {
                return static_cast<double>(ico.rgba[(static_cast<std::size_t>(yy) * S + xx) * 4 + ch]);
            };
            const auto bilerp = [&](int ch) {
                const double top = px(x0, y0, ch) * (1 - fx) + px(x0 + 1, y0, ch) * fx;
                const double bot = px(x0, y0 + 1, ch) * (1 - fx) + px(x0 + 1, y0 + 1, ch) * fx;
                return top * (1 - fy) + bot * fy;
            };
            r = bilerp(0);
            g = bilerp(1);
            b = bilerp(2);
            a = bilerp(3);
        };
        std::size_t k = 0;
        for (int dy = 0; dy < kHeight; ++dy) {
            for (int dx = 0; dx < kWidth; ++dx, k += 4) {
                const double rx = dx - cx;
                const double ry = dy - cy;
                double r = 0;
                double g = 0;
                double b = 0;
                double a = 0;
                sample(ct * rx + st * ry + s0, -st * rx + ct * ry + s0, r, g, b, a);
                if (a <= 0.0)
                    continue;
                const double f = (a / 255.0) * kWatermarkOpacity;
                out.rgba[k + 0] = static_cast<std::uint8_t>(std::lround(out.rgba[k + 0] + (r - out.rgba[k + 0]) * f));
                out.rgba[k + 1] = static_cast<std::uint8_t>(std::lround(out.rgba[k + 1] + (g - out.rgba[k + 1]) * f));
                out.rgba[k + 2] = static_cast<std::uint8_t>(std::lround(out.rgba[k + 2] + (b - out.rgba[k + 2]) * f));
            }
        }
    }
    m_bgBuf = std::move(out);
    m_bgBg = bg;
}

// ---- decorative extras (inactive most of the year: never built, never drawn) ------------------

void AboutWindow::buildMoteSprites() {
    for (std::size_t b = 0; b < m_moteBuf.size(); ++b) {
        const int r = m_moteR[b];
        const int side = 2 * r + 1;
        const double baseA = 150.0 + 42.0 * static_cast<double>(b); // larger == brighter/closer
        common::Image img(static_cast<std::uint32_t>(side), static_cast<std::uint32_t>(side));
        std::size_t k = 0;
        for (int yy = 0; yy < side; ++yy) {
            for (int xx = 0; xx < side; ++xx) {
                const double d = std::hypot(xx - r, yy - r);
                const double t = std::clamp(d / r, 0.0, 1.0);
                const double a = baseA * std::pow(1.0 - t, 1.5); // soft radial falloff
                img.rgba[k++] = 255;
                img.rgba[k++] = 255;
                img.rgba[k++] = 255;
                img.rgba[k++] = static_cast<std::uint8_t>(std::lround(std::clamp(a, 0.0, 255.0)));
            }
        }
        m_moteBuf[b] = std::move(img);
        m_moteImg[b] = std::make_unique<Fl_RGB_Image>(m_moteBuf[b].rgba.data(), side, side, 4);
    }
}

void AboutWindow::seedMotes() {
    std::uniform_real_distribution<double> ux(0.0, kWidth);
    std::uniform_real_distribution<double> uy(0.0, kHeight);
    std::uniform_real_distribution<double> uvy(16.0, 44.0);
    std::uniform_real_distribution<double> usa(6.0, 16.0);
    std::uniform_real_distribution<double> usr(0.6, 1.4);
    std::uniform_real_distribution<double> uph(0.0, 2.0 * kPi);
    std::discrete_distribution<int> ubucket({5, 3, 2}); // mostly small flecks

    m_motes.resize(46);
    for (EggMote& m : m_motes) {
        m.x = ux(m_rng);
        m.y = uy(m_rng);
        m.vy = uvy(m_rng);
        m.swayAmp = usa(m_rng);
        m.swayRate = usr(m_rng);
        m.phase = uph(m_rng);
        m.bucket = ubucket(m_rng);
    }
}

void AboutWindow::stepMotes(double dt) {
    std::uniform_real_distribution<double> ux(0.0, kWidth);
    std::uniform_real_distribution<double> utop(0.0, 30.0);
    for (EggMote& m : m_motes) {
        m.phase += m.swayRate * dt;
        m.y += m.vy * dt;
        m.x += std::sin(m.phase) * m.swayAmp * dt;
        const int r = m_moteR[m.bucket];
        if (m.y - r > kHeight) { // fell off the bottom: respawn just above the top
            m.y = -r - utop(m_rng);
            m.x = ux(m_rng);
        }
        if (m.x < -r)
            m.x += kWidth + 2 * r;
        else if (m.x > kWidth + r)
            m.x -= kWidth + 2 * r;
    }
}

void AboutWindow::drawEggLights(const Palette& pal) {
    constexpr int kStripH = 22;
    constexpr int kBulbSpacing = 38;
    constexpr double kGlowR = 5.0;
    const int stripY = 0; // strung across the top edge, hanging down into the dialog

    static const std::array<common::Color8, 5> kBulbs = {{
        {224, 64, 64, 255},   // red
        {244, 176, 48, 255},  // amber
        {72, 200, 96, 255},   // green
        {72, 132, 236, 255},  // blue
        {206, 88, 206, 255},  // magenta
    }};
    const common::Color8 wire = lerp(pal.windowBg, {8, 10, 16, 255}, 0.65);

    common::Image strip(static_cast<std::uint32_t>(kWidth), static_cast<std::uint32_t>(kStripH));
    // Seed from the baked ground so the watermark (and windowBg) show through the garland's gaps.
    if (!m_bgBuf.empty() && m_bgBuf.width == static_cast<std::uint32_t>(kWidth) &&
        m_bgBuf.height >= static_cast<std::uint32_t>(stripY + kStripH)) {
        std::copy_n(m_bgBuf.rgba.begin() + static_cast<std::size_t>(stripY) * kWidth * 4,
                    static_cast<std::size_t>(kStripH) * kWidth * 4, strip.rgba.begin());
    } else {
        strip.fill(pal.windowBg);
    }
    const auto put = [&](int sx, int sy, common::Color8 c, double f) {
        if (sx < 0 || sx >= kWidth || sy < 0 || sy >= kStripH)
            return;
        f = std::clamp(f, 0.0, 1.0);
        const std::size_t idx = (static_cast<std::size_t>(sy) * kWidth + sx) * 4;
        strip.rgba[idx + 0] =
            static_cast<std::uint8_t>(std::lround(strip.rgba[idx + 0] + (c.r - strip.rgba[idx + 0]) * f));
        strip.rgba[idx + 1] =
            static_cast<std::uint8_t>(std::lround(strip.rgba[idx + 1] + (c.g - strip.rgba[idx + 1]) * f));
        strip.rgba[idx + 2] =
            static_cast<std::uint8_t>(std::lround(strip.rgba[idx + 2] + (c.b - strip.rgba[idx + 2]) * f));
    };

    // The sagging wire (a shallow catenary: lowest in the middle).
    constexpr double kWireTop = 5.0;
    constexpr double kSag = 4.0;
    const auto wireY = [&](int x) { return kWireTop + kSag * std::sin(kPi * x / kWidth); };
    for (int x = 0; x < kWidth; ++x) {
        const int wy = static_cast<int>(std::lround(wireY(x)));
        put(x, wy, wire, 0.9);
        put(x, wy + 1, wire, 0.45);
    }

    // The bulbs, each twinkling on its own phase.
    const double t = nowSeconds() - m_t0;
    int bi = 0;
    for (int bx = kBulbSpacing / 2; bx < kWidth; bx += kBulbSpacing, ++bi) {
        const common::Color8 col = kBulbs[bi % kBulbs.size()];
        const double bright = 0.5 + 0.5 * std::sin(t * 2.2 + bi * 1.7);
        const double by = wireY(bx) + 4.0; // hangs just below the wire
        const int x0 = bx - static_cast<int>(kGlowR) - 1;
        const int x1 = bx + static_cast<int>(kGlowR) + 1;
        const int y0 = static_cast<int>(by - kGlowR) - 1;
        const int y1 = static_cast<int>(by + kGlowR) + 1;
        for (int py = y0; py <= y1; ++py) {
            for (int px = x0; px <= x1; ++px) {
                const double d = std::hypot(px - bx, py - by);
                if (d > kGlowR)
                    continue;
                const double inten = bright * std::pow(1.0 - d / kGlowR, 1.7);
                put(px, py, col, inten * 0.9);
                if (d <= 1.6) // a hot near-white core
                    put(px, py, lerp(col, {255, 255, 255, 255}, 0.6), bright * (1.0 - d / 1.6));
            }
        }
    }

    fl_draw_image(strip.rgba.data(), 0, stripY, kWidth, kStripH, 4, 0); // pre-composited, opaque
}

void AboutWindow::drawMotes() {
    for (const EggMote& m : m_motes) {
        const int r = m_moteR[m.bucket];
        m_moteImg[m.bucket]->draw(static_cast<int>(std::lround(m.x)) - r,
                                  static_cast<int>(std::lround(m.y)) - r);
    }
}

} // namespace

void showAboutDialog(Fl_Window* parent) {
    AboutWindow win;
    win.show();
    centerWindowOver(win, parent); // over the app; multi-monitor-correct without it
    win.arm();

    while (win.shown())
        Fl::wait();
}

} // namespace mosaic::ui
