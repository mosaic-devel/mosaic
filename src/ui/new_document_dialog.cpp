#include "common/charconv_compat.hpp"
#include "ui/new_document_dialog.hpp"

#include "common/i18n.hpp"
#include "common/image.hpp"
#include "core/color_management.hpp"
#include "core/layer.hpp"
#include "ui/ask_or_tell_dialog.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <FL/Enumerations.H>
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace mosaic::ui {

// ---- pure logic (no FLTK; unit-tested in tests/test_new_document.cpp) ----------------------

std::string_view sizeUnitName(SizeUnit u) {
    switch (u) {
    case SizeUnit::Pixels: return "Pixels";
    case SizeUnit::Millimeters: return "Millimeters";
    case SizeUnit::Centimeters: return "Centimeters";
    case SizeUnit::Inches: return "Inches";
    case SizeUnit::Points: return "Points";
    }
    return "Pixels";
}

std::string_view sizeUnitAbbrev(SizeUnit u) {
    switch (u) {
    case SizeUnit::Pixels: return "px";
    case SizeUnit::Millimeters: return "mm";
    case SizeUnit::Centimeters: return "cm";
    case SizeUnit::Inches: return "in";
    case SizeUnit::Points: return "pt";
    }
    return "px";
}

std::string_view newDocBackgroundName(NewDocBackground b) {
    switch (b) {
    case NewDocBackground::White: return "White";
    case NewDocBackground::Black: return "Black";
    case NewDocBackground::Transparent: return "Transparent";
    }
    return "White";
}

double unitToPixels(double value, SizeUnit unit, double dpi) {
    switch (unit) {
    case SizeUnit::Pixels: return value;
    case SizeUnit::Inches: return value * dpi;
    case SizeUnit::Millimeters: return value / 25.4 * dpi;
    case SizeUnit::Centimeters: return value / 2.54 * dpi;
    case SizeUnit::Points: return value / 72.0 * dpi;
    }
    return value;
}

double pixelsToUnit(double pixels, SizeUnit unit, double dpi) {
    if (unit == SizeUnit::Pixels || dpi <= 0.0)
        return pixels;
    switch (unit) {
    case SizeUnit::Pixels: return pixels;
    case SizeUnit::Inches: return pixels / dpi;
    case SizeUnit::Millimeters: return pixels / dpi * 25.4;
    case SizeUnit::Centimeters: return pixels / dpi * 2.54;
    case SizeUnit::Points: return pixels / dpi * 72.0;
    }
    return pixels;
}

namespace {
std::uint32_t resolvePixels(double value, SizeUnit unit, double dpi) {
    const double px = unitToPixels(value, unit, dpi);
    if (!std::isfinite(px) || px < 1.0)
        return 1;
    if (px > static_cast<double>(kMaxCanvasDimension))
        return kMaxCanvasDimension;
    return static_cast<std::uint32_t>(std::llround(px));
}
} // namespace

std::uint32_t NewDocumentSpec::pixelWidth() const { return resolvePixels(width, unit, dpi); }
std::uint32_t NewDocumentSpec::pixelHeight() const { return resolvePixels(height, unit, dpi); }

NewDocumentSpec defaultNewDocumentSpec() {
    return NewDocumentSpec{}; // the in-class defaults: Full HD, sRGB, 8-bit, white
}

const std::vector<DocumentPreset>& documentPresets() {
    // ISO 216 A-series default to 300 ppi (print); US paper likewise; pixel presets to 72 ppi.
    // Texture sizes are the power-of-two squares (128..8192; user 2026-07-22).
    using PC = PresetCategory;
    static const std::vector<DocumentPreset> presets = {
        {"A0  (841 × 1189 mm)", 841.0, 1189.0, SizeUnit::Millimeters, 300.0, PC::Print},
        {"A1  (594 × 841 mm)", 594.0, 841.0, SizeUnit::Millimeters, 300.0, PC::Print},
        {"A2  (420 × 594 mm)", 420.0, 594.0, SizeUnit::Millimeters, 300.0, PC::Print},
        {"A3  (297 × 420 mm)", 297.0, 420.0, SizeUnit::Millimeters, 300.0, PC::Print},
        {"A4  (210 × 297 mm)", 210.0, 297.0, SizeUnit::Millimeters, 300.0, PC::Print},
        {"A5  (148 × 210 mm)", 148.0, 210.0, SizeUnit::Millimeters, 300.0, PC::Print},
        {"US Letter  (8.5 × 11 in)", 8.5, 11.0, SizeUnit::Inches, 300.0, PC::Print},
        {"US Legal  (8.5 × 14 in)", 8.5, 14.0, SizeUnit::Inches, 300.0, PC::Print},
        {"US Tabloid  (11 × 17 in)", 11.0, 17.0, SizeUnit::Inches, 300.0, PC::Print},
        {"4K UHD  (3840 × 2160 px)", 3840.0, 2160.0, SizeUnit::Pixels, 72.0, PC::Screen},
        {"Full HD  (1920 × 1080 px)", 1920.0, 1080.0, SizeUnit::Pixels, 72.0, PC::Screen},
        {"HD  (1280 × 720 px)", 1280.0, 720.0, SizeUnit::Pixels, 72.0, PC::Screen},
        {"Square  (1080 × 1080 px)", 1080.0, 1080.0, SizeUnit::Pixels, 72.0, PC::Screen},
        // SVGA/VGA carry real short names so their size reads as a SUBTITLE like every other
        // card -- the two bare "W × H px" names left their cards without the size line, which
        // made the Screen shelf (and Square beside them) read as mismatched (user round 5).
        {"SVGA  (800 × 600 px)", 800.0, 600.0, SizeUnit::Pixels, 72.0, PC::Screen},
        {"VGA  (640 × 480 px)", 640.0, 480.0, SizeUnit::Pixels, 72.0, PC::Screen},
        {"128  (128 × 128 px)", 128.0, 128.0, SizeUnit::Pixels, 72.0, PC::Texture},
        {"256  (256 × 256 px)", 256.0, 256.0, SizeUnit::Pixels, 72.0, PC::Texture},
        {"512  (512 × 512 px)", 512.0, 512.0, SizeUnit::Pixels, 72.0, PC::Texture},
        {"1024  (1024 × 1024 px)", 1024.0, 1024.0, SizeUnit::Pixels, 72.0, PC::Texture},
        {"2048  (2048 × 2048 px)", 2048.0, 2048.0, SizeUnit::Pixels, 72.0, PC::Texture},
        {"4096  (4096 × 4096 px)", 4096.0, 4096.0, SizeUnit::Pixels, 72.0, PC::Texture},
        {"8192  (8192 × 8192 px)", 8192.0, 8192.0, SizeUnit::Pixels, 72.0, PC::Texture},
    };
    return presets;
}

PresetCategory presetCategory(const DocumentPreset& p) {
    return p.category;
}

std::string_view presetShortName(const DocumentPreset& p) {
    const std::size_t split = p.name.find("  ");
    return split == std::string_view::npos ? p.name : p.name.substr(0, split);
}

std::string presetDetail(const DocumentPreset& p) {
    const std::size_t open = p.name.find('(');
    const std::size_t close = p.name.rfind(')');
    if (open == std::string_view::npos || close == std::string_view::npos || close <= open)
        return {};
    return std::string(p.name.substr(open + 1, close - open - 1));
}

std::size_t bytesPerPixel(core::Precision p) {
    switch (p) {
    case core::Precision::U8: return 4;   // 8-bit RGBA
    case core::Precision::U16: return 8;  // 16-bit RGBA
    case core::Precision::F16: return 8;  // half-float RGBA
    case core::Precision::F32: return 16; // float RGBA
    }
    return 4;
}

std::uint64_t layerMemoryBytes(const NewDocumentSpec& spec) {
    return static_cast<std::uint64_t>(spec.pixelWidth()) * spec.pixelHeight() *
           bytesPerPixel(spec.precision);
}

std::string formatByteSize(std::uint64_t bytes) {
    if (bytes < 1000)
        return std::to_string(bytes) + " B";
    static constexpr const char* kUnits[] = {"KB", "MB", "GB", "TB"};
    double v = static_cast<double>(bytes);
    int u = -1;
    while (v >= 1000.0 && u < 3) {
        v /= 1000.0;
        ++u;
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, v < 100.0 ? "%.1f %s" : "%.0f %s", v, kUnits[u]);
    return buf;
}

std::vector<TemplateFile> scanDocumentTemplates(const std::filesystem::path& dir) {
    std::vector<TemplateFile> out;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        std::error_code fileEc;
        if (!entry.is_regular_file(fileEc) || entry.path().extension() != ".mosaic")
            continue;
        const std::string stem = entry.path().stem().string();
        std::size_t digits = 0;
        while (digits < stem.size() &&
               std::isdigit(static_cast<unsigned char>(stem[digits])) != 0)
            ++digits;
        int order = INT_MAX; // un-numbered files sort after every numbered one
        std::string name = stem;
        if (digits > 0 && digits < stem.size() && stem[digits] == '-') {
            const long parsed = std::strtol(stem.c_str(), nullptr, 10);
            order = static_cast<int>(std::min<long>(parsed, INT_MAX));
            name = stem.substr(digits + 1);
        }
        std::replace(name.begin(), name.end(), '_', ' '); // "Birthday_Card" reads "Birthday Card"
        if (name.empty())
            name = stem;
        out.push_back({entry.path(), name, order});
    }
    std::sort(out.begin(), out.end(), [](const TemplateFile& a, const TemplateFile& b) {
        if (a.order != b.order)
            return a.order < b.order;
        const auto ci = [](const std::string& s) {
            std::string t = s;
            std::transform(t.begin(), t.end(), t.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return t;
        };
        const std::string an = ci(a.name);
        const std::string bn = ci(b.name);
        if (an != bn)
            return an < bn;
        return a.path < b.path; // total order even for duplicate names
    });
    return out;
}

int matchDocumentPreset(const NewDocumentSpec& spec) {
    const auto& presets = documentPresets();
    for (std::size_t i = 0; i < presets.size(); ++i) {
        const DocumentPreset& p = presets[i];
        if (p.unit != spec.unit || std::abs(p.dpi - spec.dpi) >= 1e-3)
            continue;
        // Orientation-blind: a landscape A4 (297 x 210) is still the A4 preset, so the seed
        // re-selects its card on the next open instead of landing on "custom".
        const bool straight = std::abs(p.width - spec.width) < 1e-3 &&
                              std::abs(p.height - spec.height) < 1e-3;
        const bool turned = std::abs(p.width - spec.height) < 1e-3 &&
                            std::abs(p.height - spec.width) < 1e-3;
        if (straight || turned)
            return static_cast<int>(i);
    }
    return -1;
}

namespace {
// "%.10g" genuinely without locale surprises: enough digits that a pixel size never rounds,
// trailing zeros trimmed. The custom-size tokens and card titles share it.
//
// This used to be snprintf("%.10g"), which is NOT locale-independent -- i18n::init() moves
// LC_NUMERIC to the user's locale, so it wrote "1234,5" across most of Europe while
// parseCustomSizeToken() reads through common::fromChars(), which only accepts '.'. Every custom
// size a comma-locale user saved was therefore unreadable on the way back in, and the recent
// silently vanished. gToString() is the matching write half of fromChars(). (S54)
std::string numToken(double v) {
    return mosaic::common::gToString(v, 10);
}
} // namespace

std::string customSizeToken(const NewDocumentSpec& spec) {
    return numToken(spec.width) + ";" + numToken(spec.height) + ";" +
           std::string(sizeUnitAbbrev(spec.unit)) + ";" + numToken(spec.dpi);
}

std::optional<NewDocumentSpec> parseCustomSizeToken(const std::string& token) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t sep = token.find(';', start);
        parts.push_back(token.substr(start, sep == std::string::npos ? std::string::npos
                                                                     : sep - start));
        if (sep == std::string::npos)
            break;
        start = sep + 1;
    }
    if (parts.size() != 4) // exactly W;H;unit;dpi -- a trailing field is garbage, not slack
        return std::nullopt;
    const auto num = [](const std::string& s, double& out) {
        const char* end = s.data() + s.size();
        return !s.empty() &&
               mosaic::common::fromChars(s.data(), end, out).ptr == end; // whole string, '.' only
    };
    NewDocumentSpec spec;
    if (!num(parts[0], spec.width) || !num(parts[1], spec.height) || !num(parts[3], spec.dpi))
        return std::nullopt;
    if (!(spec.width > 0.0) || !(spec.height > 0.0) || !(spec.dpi > 0.0))
        return std::nullopt;
    bool unitOk = false;
    for (int u = 0; u <= static_cast<int>(SizeUnit::Points); ++u) {
        if (sizeUnitAbbrev(static_cast<SizeUnit>(u)) == parts[2]) {
            spec.unit = static_cast<SizeUnit>(u);
            unitOk = true;
            break;
        }
    }
    if (!unitOk)
        return std::nullopt;
    return spec;
}

std::string customSizeFace(const NewDocumentSpec& spec) {
    return numToken(spec.width) + " × " + numToken(spec.height);
}

std::string customSizeTitle(const NewDocumentSpec& spec) {
    return customSizeFace(spec) + " " + std::string(sizeUnitAbbrev(spec.unit));
}

// A document title is DATA -- it is written into the file and shown wherever that document is --
// so core keeps the plain English default and never translates. What the UI translates is the name
// it GENERATES for a brand-new document, the way a localized desktop names a new file. That makes
// the recogniser below bilingual by necessity: a file created in one language must still read as
// "this name was auto-generated" when opened in another, or renaming it would start prompting.
bool matchesUntitledBase(std::string_view title, std::string_view base) {
    if (title == base)
        return true;
    if (title.size() <= base.size() + 1 || title.substr(0, base.size()) != base ||
        title[base.size()] != ' ') {
        return false;
    }
    for (std::size_t i = base.size() + 1; i < title.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(title[i])) == 0)
            return false;
    }
    return true;
}

bool isAutoUntitledTitle(std::string_view title) {
    return matchesUntitledBase(title, _("Untitled")) || matchesUntitledBase(title, "Untitled");
}

std::string nextUntitledTitle(const std::vector<std::string>& openTitles) {
    const auto used = [&openTitles](const std::string& t) {
        return std::find(openTitles.begin(), openTitles.end(), t) != openTitles.end();
    };
    const std::string base = _("Untitled");
    if (!used(base))
        return base;
    for (int n = 2;; ++n) {
        std::string t = base + " " + std::to_string(n);
        if (!used(t))
            return t;
    }
}

std::string abbreviatedLocation(const std::string& absolutePath, const std::string& homeDir) {
    std::string dir = std::filesystem::path(absolutePath).parent_path().string();
    if (!homeDir.empty() && dir.rfind(homeDir, 0) == 0 &&
        (dir.size() == homeDir.size() || dir[homeDir.size()] == '/')) {
        dir.replace(0, homeDir.size(), "~");
    }
    return dir;
}

FitSize fitPreservingAspect(std::uint32_t srcW, std::uint32_t srcH, int maxW, int maxH) {
    if (srcW == 0 || srcH == 0 || maxW <= 0 || maxH <= 0)
        return {};
    const double scale = std::min({1.0, static_cast<double>(maxW) / srcW,
                                   static_cast<double>(maxH) / srcH});
    return {std::max(1, static_cast<int>(std::lround(srcW * scale))),
            std::max(1, static_cast<int>(std::lround(srcH * scale)))};
}

common::Image boxDownscale(const common::Image& src, int dstW, int dstH) {
    if (src.empty() || dstW <= 0 || dstH <= 0)
        return {};
    common::Image dst(static_cast<std::uint32_t>(dstW), static_cast<std::uint32_t>(dstH));
    const auto sw = static_cast<double>(src.width);
    const auto sh = static_cast<double>(src.height);
    for (int dy = 0; dy < dstH; ++dy) {
        // The source band this destination row averages: [y0, y1) with at least one row.
        const auto y0 = static_cast<std::uint32_t>(dy * sh / dstH);
        auto y1 = static_cast<std::uint32_t>((dy + 1) * sh / dstH);
        y1 = std::max(y1, y0 + 1);
        for (int dx = 0; dx < dstW; ++dx) {
            const auto x0 = static_cast<std::uint32_t>(dx * sw / dstW);
            auto x1 = static_cast<std::uint32_t>((dx + 1) * sw / dstW);
            x1 = std::max(x1, x0 + 1);
            std::uint64_t sum[4] = {0, 0, 0, 0};
            for (std::uint32_t sy = y0; sy < y1 && sy < src.height; ++sy) {
                const std::uint8_t* row =
                    src.rgba.data() + (static_cast<std::size_t>(sy) * src.width + x0) * 4;
                for (std::uint32_t sx = x0; sx < x1 && sx < src.width; ++sx, row += 4) {
                    sum[0] += row[0];
                    sum[1] += row[1];
                    sum[2] += row[2];
                    sum[3] += row[3];
                }
            }
            const std::uint64_t n =
                static_cast<std::uint64_t>(std::min(x1, src.width) - x0) *
                (std::min(y1, src.height) - y0);
            std::uint8_t* out =
                dst.rgba.data() + (static_cast<std::size_t>(dy) * dst.width + dx) * 4;
            for (int c = 0; c < 4; ++c)
                out[c] = static_cast<std::uint8_t>((sum[c] + n / 2) / n);
        }
    }
    return dst;
}

common::Image checkerCompose(const common::Image& src) {
    if (src.empty())
        return {};
    // The compositor's transparency greys at an 8px cell -- matches what the retired cache
    // baked, so cards look unchanged.
    constexpr int kCell = 8;
    constexpr std::uint8_t kLight = 205;
    constexpr std::uint8_t kDark = 150;
    common::Image out(src.width, src.height);
    for (std::uint32_t y = 0; y < src.height; ++y) {
        const std::uint8_t* in = src.rgba.data() + static_cast<std::size_t>(y) * src.width * 4;
        std::uint8_t* dst = out.rgba.data() + static_cast<std::size_t>(y) * src.width * 4;
        for (std::uint32_t x = 0; x < src.width; ++x, in += 4, dst += 4) {
            const std::uint8_t g =
                (((x / kCell) + (y / kCell)) & 1) != 0 ? kDark : kLight;
            const unsigned a = in[3];
            dst[0] = static_cast<std::uint8_t>((in[0] * a + g * (255 - a) + 127) / 255);
            dst[1] = static_cast<std::uint8_t>((in[1] * a + g * (255 - a) + 127) / 255);
            dst[2] = static_cast<std::uint8_t>((in[2] * a + g * (255 - a) + 127) / 255);
            dst[3] = 255;
        }
    }
    return out;
}

std::unique_ptr<core::Document> buildDocument(const NewDocumentSpec& spec) {
    auto doc = std::make_unique<core::Document>(spec.pixelWidth(), spec.pixelHeight(),
                                                spec.colorSpace, spec.precision);
    doc->setDpi(spec.dpi > 0.0 ? spec.dpi : 72.0);
    doc->setTitle(spec.title.empty() ? std::string("Untitled") : spec.title);

    if (!spec.iccProfilePath.empty()) // the Color dropdown's "Custom..." pick (round 5)
        doc->setIccProfile(spec.iccProfilePath, core::iccProfileName(spec.iccProfilePath));

    if (spec.background == NewDocBackground::Transparent) {
        // A single empty (fully transparent) layer, so the compositor's checkerboard shows.
        doc->root().addOnTop(doc->makeRaster("Layer 1"));
    } else {
        auto bg = doc->makeRaster("Background");
        bg->image().fill(spec.background == NewDocBackground::Black
                             ? common::Color8{0, 0, 0, 255}
                             : common::Color8{255, 255, 255, 255});
        // The opaque Background starts UNLOCKED so a fresh canvas is paintable immediately (user
        // 2026-06-19; matches Krita/GIMP/Affinity). A lock UI + the locked-Background-by-default
        // question live in the PLAN §12 layer-locking backlog.
        doc->root().addOnTop(std::move(bg));
    }
    return doc;
}

// ---- the modal FLTK dialog -----------------------------------------------------------------

namespace {

Fl_Color toFl(common::Color8 c) {
    return fl_rgb_color(c.r, c.g, c.b);
}

double parseDouble(const char* s) {
    if (s == nullptr)
        return 0.0;
    // Simple arithmetic evaluates live ("1024*2" reads as 2048 -- user request); a malformed or
    // mid-typing entry ("1024*") falls back to its leading number so the summary never jumps to 0.
    if (const std::optional<double> v = evaluateFieldExpression(s))
        return *v;
    char* end = nullptr;
    const double v = std::strtod(s, &end);
    return end == s ? 0.0 : v;
}

double parsePositive(const char* s, double fallback) {
    const double v = parseDouble(s);
    return v > 0.0 ? v : fallback;
}

void setNumber(Fl_Input* in, double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.6g", v); // trims trailing zeros; integers show no '.'
    in->value(buf);
}

void styleInput(Fl_Input* in, const Palette& p) {
    in->box(MOSAIC_INPUT_BOX);
    in->color(toFl(p.controlBg));
    in->textcolor(toFl(p.text));
    in->cursor_color(toFl(p.text));
    in->labelcolor(toFl(p.text));
    in->textsize(13);
}

// The unit tag's ink: the accent pulled just over halfway toward the field ground -- present at
// a glance, clearly not part of the value.
common::Color8 faintAccent(const Palette& p) {
    const auto ch = [](std::uint8_t a, std::uint8_t b) {
        return static_cast<std::uint8_t>(a + (b - a) * 55 / 100);
    };
    return {ch(p.controlBg.r, p.accent.r), ch(p.controlBg.g, p.accent.g),
            ch(p.controlBg.b, p.accent.b), 255};
}

// A NumberField with a faint accent-coloured unit tag ("px", "ppi") inside its right edge, so
// the unit reads without an external label (user request, S9-follow-ups feedback round).
// Optionally carries a QUICK-PICK list (the Resolution field's common ppi values, round 5):
// a small chevron joins the tag and clicking that end zone opens the themed ContextMenu.
class UnitField : public NumberField {
public:
    UnitField(int X, int Y, int W, int H) : NumberField(X, Y, W, H) {}

    void setUnitText(std::string u) {
        if (u != m_unit) {
            m_unit = std::move(u);
            redraw();
        }
    }

    void setQuickPicks(std::vector<double> values) { m_quickPicks = std::move(values); }

protected:
    static constexpr int kChevronW = 11; // the down chevron's cell beside the tag

    // Where the quick-pick zone begins: the chevron AND the unit tag are the target -- the tag
    // names what the list offers, so it is part of the affordance (user round 6: the chevron
    // alone was too small a click area). Matches draw()'s erase rect exactly.
    [[nodiscard]] int pickZoneX() const {
        fl_font(FL_HELVETICA, 11); // the tag's face, set for measuring outside draw()
        return x() + w() - 7 - kChevronW - static_cast<int>(fl_width(m_unit.c_str())) - 3;
    }

    void draw() override {
        NumberField::draw();
        if (m_unit.empty())
            return;
        const Palette& p = activePalette();
        fl_font(FL_HELVETICA, 11);
        // ERASE the tag's cell first: Fl_Input's partial redraws never clear this corner, so an
        // unerased tag composites over its previous frame and boldens on every repaint (the
        // standing draw()-must-erase trap). color() is the field ground styleInput set.
        const bool picks = !m_quickPicks.empty();
        const int chevW = picks ? kChevronW : 0; // tag shifts left of the chevron
        const int tw = static_cast<int>(fl_width(m_unit.c_str()));
        fl_color(color()); // the field ground styleInput set
        fl_rectf(x() + w() - 7 - chevW - tw - 3, y() + 2, chevW + tw + 8, h() - 4);
        fl_color(toFl(active_r() ? faintAccent(p) : p.textMuted));
        fl_draw(m_unit.c_str(), x(), y(), w() - 7 - chevW, h(),
                FL_ALIGN_RIGHT | FL_ALIGN_INSIDE, nullptr, 0);
        if (picks) { // a small down chevron marks the quick-pick zone
            const int cx = x() + w() - 11;
            const int cy = y() + h() / 2 - 1;
            fl_polygon(cx - 3, cy - 1, cx + 3, cy - 1, cx, cy + 3);
        }
    }

    int handle(int event) override {
        const bool hasZone = !m_quickPicks.empty() && active_r();
        if (hasZone && (event == FL_PUSH || event == FL_RELEASE) &&
            Fl::event_x() >= pickZoneX()) {
            if (event == FL_PUSH)
                openQuickPicks();
            return 1; // claim the pair; the menu acts on its own events
        }
        if (hasZone && (event == FL_ENTER || event == FL_MOVE)) {
            // The link hand says "this end opens a list", the I-beam "this part edits" (user
            // round 6). An in-zone move must NEVER reach the base: Fl_Input re-asserts the
            // I-beam on every move, so setting the hand after it meant two cursor changes per
            // motion event -- a rapid I-beam/hand flicker (user round 8). Out of the zone the
            // base runs and keeps its own I-beam current; FL_LEAVE stays with the base too
            // (its default-cursor reset).
            if (Fl::event_x() >= pickZoneX()) {
                if (window() != nullptr)
                    window()->cursor(FL_CURSOR_HAND);
                return 1;
            }
            return NumberField::handle(event);
        }
        return NumberField::handle(event);
    }

private:
    void openQuickPicks() {
        ContextMenu* menu = contextMenuFor(top_window());
        if (menu == nullptr)
            return;
        std::vector<ContextAction> actions;
        for (const double v : m_quickPicks) {
            actions.push_back({formatFieldNumber(v, 1.0) + " " + m_unit,
                               [this, v] {
                                   char buf[32];
                                   std::snprintf(buf, sizeof buf, "%.10g", v);
                                   value(buf);
                                   do_callback(); // exactly like a typed edit
                               },
                               true, false});
        }
        menu->openWith(x(), y() + h() + 2, std::move(actions));
    }

    std::string m_unit;
    std::vector<double> m_quickPicks;
};

// The Width<->Height ratio link: a chain toggle drawn between the two fields -- two closed rings
// when engaged (accent), pulled apart when not. Editing one linked field re-derives the other
// from the ratio captured when the link engaged (or the last preset/programmatic set).
class LinkButton : public Fl_Widget {
public:
    LinkButton(int X, int Y, int W, int H, std::function<void(bool)> onToggle)
        : Fl_Widget(X, Y, W, H), m_onToggle(std::move(onToggle)) {
        tooltip(_("Link width and height (keep the ratio)"));
    }

    void setLinked(bool l) {
        if (l != m_linked) {
            m_linked = l;
            redraw();
        }
    }
    [[nodiscard]] bool linked() const noexcept { return m_linked; }

protected:
    int handle(int event) override {
        if (!active_r())
            return 0; // a disabled link neither hovers nor toggles
        switch (event) {
        case FL_ENTER: m_hover = true; redraw(); return 1;
        case FL_LEAVE: m_hover = false; redraw(); return 1;
        case FL_PUSH: return 1; // claim the pair
        case FL_RELEASE:
            if (Fl::event_inside(this)) {
                m_linked = !m_linked;
                redraw();
                if (m_onToggle)
                    m_onToggle(m_linked);
            }
            return 1;
        default: return Fl_Widget::handle(event);
        }
    }

    void draw() override {
        const Palette& p = activePalette();
        fl_color(toFl(p.panelBg)); // erase: sits on the form panel
        fl_rectf(x(), y(), w(), h());
        const bool on = active_r(); // disabled: muted rings, no accent/hover (Dropdown greying)
        const common::Color8 ink =
            !on ? p.textMuted : m_linked ? p.accent : m_hover ? p.text : p.textMuted;
        const int cy = y() + h() / 2;
        const int cx = x() + w() / 2;
        const int gap = m_linked ? 3 : 6; // engaged rings overlap; broken ones pull apart
        // Both rings in ONE patch -- engaged, they interlock, and a second opaque patch would cut
        // the first. Stroke 2 is the width the fl_line_style(FL_SOLID, 2) this replaces set, and a
        // 2 px ring is exactly where an un-anti-aliased arc reads worst.
        drawAAArcs(p.panelBg, {aaArcFromBox(cx - gap - 4, cy - 4, 9, 9, 0, 360, 2.0, ink),
                               aaArcFromBox(cx + gap - 4, cy - 4, 9, 9, 0, 360, 2.0, ink)});
    }

private:
    std::function<void(bool)> m_onToggle;
    bool m_linked = false;
    bool m_hover = false;
};

// Portrait | Landscape segmented switch (user request: "no way to change the orientation").
// Two halves, each a drawn page glyph + word; the ACTIVE half reflects the current W/H order
// (neither for a square) and clicking the other swaps the fields. The swap deliberately keeps a
// selected preset card selected -- "A4, landscape" is still A4.
class OrientationSwitch : public Fl_Widget {
public:
    OrientationSwitch(int X, int Y, int W, int H, std::function<void(bool)> onPickLandscape)
        : Fl_Widget(X, Y, W, H), m_onPick(std::move(onPickLandscape)) {}

    // 0 = portrait, 1 = landscape, -1 = square (neither half reads active).
    void setState(int s) {
        if (s != m_state) {
            m_state = s;
            redraw();
        }
    }

protected:
    int handle(int event) override {
        if (!active_r())
            return 0; // disabled: no hover, no picks
        switch (event) {
        case FL_ENTER:
        case FL_MOVE: {
            const int h = Fl::event_x() < x() + w() / 2 ? 0 : 1;
            if (h != m_hoverHalf) {
                m_hoverHalf = h;
                redraw();
            }
            return 1;
        }
        case FL_LEAVE:
            m_hoverHalf = -1;
            redraw();
            return 1;
        case FL_PUSH:
            return 1; // claim the pair
        case FL_RELEASE:
            if (Fl::event_inside(this) && m_onPick) {
                const bool landscape = Fl::event_x() >= x() + w() / 2;
                if (m_state != (landscape ? 1 : 0))
                    m_onPick(landscape);
            }
            return 1;
        default:
            return Fl_Widget::handle(event);
        }
    }

    void draw() override {
        const Palette& p = activePalette();
        const bool enabled = active_r(); // disabled: muted inks, no accent fill (Dropdown greying)
        const int halfW = w() / 2;
        for (int i = 0; i < 2; ++i) {
            const int hx = x() + i * halfW;
            const int hw = i == 0 ? halfW : w() - halfW;
            const bool active = m_state == i;
            const common::Color8 fill = !enabled
                                            ? (active ? p.controlHover : p.controlBg)
                                            : active ? p.accent
                                            : m_hoverHalf == i ? p.controlHover
                                                               : p.controlBg;
            fl_color(toFl(fill));
            fl_rectf(hx, y(), hw, h());
            const common::Color8 ink =
                !enabled ? p.textMuted : active ? p.onAccent : p.text;
            // The page glyph: an outlined rect, tall for portrait / wide for landscape.
            const int gw = i == 0 ? 9 : 13;
            const int gh = i == 0 ? 13 : 9;
            const int gx = hx + 12;
            const int gy = y() + (h() - gh) / 2;
            fl_color(toFl(ink));
            fl_rect(gx, gy, gw, gh);
            fl_font(FL_HELVETICA, 12);
            fl_draw(i == 0 ? _("Portrait") : _("Landscape"), gx + 13 + 6, y(),
                    hw - (13 + 6) - 14, h(), FL_ALIGN_LEFT | FL_ALIGN_INSIDE, nullptr, 0);
        }
        fl_color(toFl(p.border));
        fl_rect(x(), y(), w(), h());
        fl_line(x() + halfW, y(), x() + halfW, y() + h() - 1);
    }

private:
    std::function<void(bool)> m_onPick;
    int m_state = -1;
    int m_hoverHalf = -1;
};

// A muted field caption above its control (the form reads as labelled groups, not a grid).
Fl_Box* fieldLabel(int x, int y, int w, const char* text, const Palette& p) {
    auto* b = new Fl_Box(x, y, w, 16, text);
    b->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    b->labelcolor(toFl(p.textMuted));
    b->labelsize(11);
    b->box(FL_NO_BOX);
    return b;
}

// ---- layout ---------------------------------------------------------------------------------

constexpr int kWinW = 900;
constexpr int kWinH = 560;
constexpr int kRailW = 170;   // left category rail
constexpr int kFormW = 280;   // right settings panel
constexpr int kBarH = 56;     // bottom action bar
constexpr int kMargin = 16;

constexpr int kGalleryX = kRailW;
constexpr int kGalleryW = kWinW - kRailW - kFormW; // 450
constexpr int kBodyH = kWinH - kBarH;

constexpr int kCardW = 126;
constexpr int kCardH = 96 + GalleryCard::kTitleH + GalleryCard::kSubtitleH; // 96px preview cell
constexpr int kCardGap = 12;
constexpr int kGalleryPad = 16;

// The dialog's rail categories, in rail order: the two "personal" shelves (Recent, Templates)
// first, then the built-in preset shelves (user round 6: Templates up beside Recent).
enum class Category { Recent, Templates, Print, Screen, Texture };
constexpr int kCategoryCount = 5;

const char* categoryName(Category c) {
    switch (c) {
    case Category::Recent: return _("Recent");
    case Category::Print: return _("Print");
    case Category::Screen: return _("Screen");
    case Category::Texture: return _("Texture");
    case Category::Templates: return _("Templates");
    }
    return "";
}

// The rail category a preset category lives under (the landing logic + gallery filter).
Category categoryOf(PresetCategory pc) {
    switch (pc) {
    case PresetCategory::Print: return Category::Print;
    case PresetCategory::Screen: return Category::Screen;
    case PresetCategory::Texture: return Category::Texture;
    }
    return Category::Print;
}

// The rail's title: "New <something>" cycling through grander ambitions with the About box's
// credit-drum motion -- hold, slide up, wrap; the leaving/entering word fades toward the rail
// ground near the clip edges (user round 5: "with the drum label we use in the about box").
// "New" itself HOLDS STILL: it is the one word every title shares, so only the ambition after
// it rides the drum (user round 6, which also asked for more encouraging words).
class TitleDrum : public Fl_Widget {
public:
    TitleDrum(int X, int Y, int W, int H) : Fl_Widget(X, Y, W, H) {
        m_words = {_("Document"), _("Creation"),   _("Artwork"), _("Adventure"),
                   _("Story"),    _("Vision"),     _("Wonder"),  _("Masterpiece")};
        m_t0 = now();
        Fl::add_timeout(kHoldS, tick, this);
    }
    ~TitleDrum() override { Fl::remove_timeout(tick, this); }

protected:
    void draw() override {
        const Palette& p = activePalette();
        fl_color(toFl(p.panelBg)); // erase: sits on the rail panel
        fl_rectf(x(), y(), w(), h());
        fl_font(FL_HELVETICA_BOLD, 15);

        const std::string prefix = std::string(_("New")) + " ";
        fl_color(toFl(p.text));
        fl_draw(prefix.c_str(), x() + 14, y(), w() - 14, h(), FL_ALIGN_LEFT | FL_ALIGN_INSIDE,
                nullptr, 0);
        const int wordX = x() + 14 + static_cast<int>(fl_width(prefix.c_str()));

        const int n = static_cast<int>(m_words.size());
        const double cycle = kHoldS + kSlideS;
        const double e = now() - m_t0;
        const int cyclesDone = static_cast<int>(std::floor(e / cycle));
        const double phase = e - cyclesDone * cycle;
        const int cur = ((cyclesDone % n) + n) % n;
        double offset = 0.0;
        if (phase > kHoldS) {
            const double t = std::clamp((phase - kHoldS) / kSlideS, 0.0, 1.0);
            // easeInOutCubic, the About drum's slide curve.
            offset = (t < 0.5 ? 4.0 * t * t * t : 1.0 - std::pow(-2.0 * t + 2.0, 3.0) / 2.0) *
                     h();
        }
        const auto drawWord = [&](const std::string& s, double off) {
            const double a = std::clamp(1.0 - std::abs(off) / (h() * 0.95), 0.0, 1.0);
            if (a <= 0.02)
                return;
            const auto ch = [a](std::uint8_t g, std::uint8_t t2) {
                return static_cast<std::uint8_t>(std::lround(g + (t2 - g) * a));
            };
            fl_color(toFl({ch(p.panelBg.r, p.text.r), ch(p.panelBg.g, p.text.g),
                           ch(p.panelBg.b, p.text.b), 255}));
            fl_draw(s.c_str(), wordX, y() + static_cast<int>(std::lround(off)),
                    x() + w() - wordX, h(), FL_ALIGN_LEFT | FL_ALIGN_INSIDE, nullptr, 0);
        };
        fl_push_clip(wordX, y(), x() + w() - wordX, h());
        drawWord(m_words[static_cast<std::size_t>(cur)], -offset);
        if (offset > 0.0)
            drawWord(m_words[static_cast<std::size_t>((cur + 1) % n)], h() - offset);
        fl_pop_clip();
    }

private:
    static double now() {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }
    // Redraws only while sliding; a hold schedules one wake exactly at its end (cheap drum).
    static void tick(void* data) {
        auto* self = static_cast<TitleDrum*>(data);
        const double cycle = kHoldS + kSlideS;
        const double phase = std::fmod(now() - self->m_t0, cycle);
        self->redraw();
        Fl::repeat_timeout(phase < kHoldS ? kHoldS - phase : 1.0 / 60.0, tick, data);
    }

    static constexpr double kHoldS = 2.4;   // dwell on a title
    static constexpr double kSlideS = 0.55; // the About drum's slide time
    std::vector<std::string> m_words; // what follows the fixed "New"
    double m_t0 = 0.0;
};

// A rail row (the Settings dialog's NavItem / Texture Generator's RailItem look): accent-filled
// when active, controlHover on hover, otherwise the panel ground.
class RailItem : public Fl_Widget {
public:
    // A small line-art badge at the row's right edge: the two personal shelves carry one so
    // they read apart from the preset shelves (user round 6) -- a clock for Recent, a birthday
    // cake for Templates (in theme with the birthday-card template to come).
    enum class Badge { None, Clock, Cake };

    RailItem(int X, int Y, int W, int H, const char* label, std::function<void()> onClick)
        : Fl_Widget(X, Y, W, H), m_onClick(std::move(onClick)) {
        copy_label(label);
    }

    void setActive(bool a) {
        if (a != m_active) {
            m_active = a;
            redraw();
        }
    }

    void setBadge(Badge b) {
        if (b != m_badge) {
            m_badge = b;
            redraw();
        }
    }

protected:
    int handle(int event) override {
        switch (event) {
        case FL_ENTER: m_hover = true; redraw(); return 1;
        case FL_LEAVE: m_hover = false; redraw(); return 1;
        case FL_PUSH: return 1; // claim the pair; act on the release
        case FL_RELEASE:
            if (Fl::event_inside(this) && m_onClick)
                m_onClick();
            return 1;
        default: return Fl_Widget::handle(event);
        }
    }

    void draw() override {
        const Palette& p = activePalette();
        const common::Color8 rowBg = m_active ? p.accent : m_hover ? p.controlHover : p.panelBg;
        fl_color(toFl(rowBg));
        fl_rectf(x(), y(), w(), h());
        fl_font(FL_HELVETICA, 13);
        fl_color(toFl(m_active ? p.onAccent : p.text));
        fl_draw(label(), x() + 14, y(), w() - 20, h(), FL_ALIGN_LEFT | FL_ALIGN_INSIDE, nullptr, 0);
        if (m_badge != Badge::None) {
            const int cx = x() + w() - 18; // right-edge badge, clear of the short labels
            const int cy = y() + h() / 2;
            const common::Color8 badgeInk = m_active ? p.onAccent : p.textMuted;
            fl_color(toFl(badgeInk));
            if (m_badge == Badge::Clock) { // a wall clock at three o'clock
                // The rim is anti-aliased; the hands stay fl_line and still go down after it, so the
                // patch's ground is just the row fill (the label is left-aligned and short).
                drawAAArcs(rowBg, {aaArcFromBox(cx - 5, cy - 5, 11, 11, 0, 360, 1.0, badgeInk)});
                fl_line(cx, cy, cx, cy - 3); // minute hand to twelve
                fl_line(cx, cy, cx + 3, cy); // hour hand to three
            } else { // the cake: body, three candles, their flames
                fl_rect(cx - 5, cy, 11, 6);
                fl_line(cx - 3, cy - 1, cx - 3, cy - 3);
                fl_line(cx, cy - 1, cx, cy - 4); // the middle candle stands taller
                fl_line(cx + 3, cy - 1, cx + 3, cy - 3);
                fl_point(cx - 3, cy - 4);
                fl_point(cx, cy - 5);
                fl_point(cx + 3, cy - 4);
            }
        }
    }

private:
    std::function<void()> m_onClick;
    bool m_active = false;
    bool m_hover = false;
    Badge m_badge = Badge::None;
};

// Which card is selected: a built-in preset, the clipboard image, a remembered custom size, or
// an index into ctx.templates / ctx.recents.
enum class SelKind { None, Preset, Clipboard, CustomSize, Template, Recent };

// All widget pointers plus the bookkeeping the live callbacks need. Lives on showNewDocument's
// stack for the dialog's (modal) lifetime.
struct DialogState {
    NewDocumentContext* ctx = nullptr; // the dialog's OWN copy; card removal edits it live

    // Form.
    Fl_Input* name = nullptr;
    UnitField* width = nullptr;
    UnitField* height = nullptr;
    LinkButton* link = nullptr;
    OrientationSwitch* orientation = nullptr;
    Dropdown* unit = nullptr;
    UnitField* dpi = nullptr;
    Dropdown* colorSpace = nullptr;
    Dropdown* depth = nullptr;
    Dropdown* background = nullptr;
    // ScrollingLabels: a recent's summary shows its file name and LOCATION, both of arbitrary
    // length -- they pan inside the panel instead of spilling past it (user round 6).
    ScrollingLabel* sizeReadout = nullptr;
    ScrollingLabel* memReadout = nullptr;
    std::vector<Fl_Widget*> sizeForm; // everything a file-backed selection deactivates

    // Gallery.
    ScrollView* gallery = nullptr;
    Fl_Group* galleryContent = nullptr;
    std::vector<RailItem*> rail;
    std::vector<GalleryCard*> cards;
    std::vector<SelKind> cardKinds;
    std::vector<int> cardIndices; // preset index / ctx list index, parallel to `cards`

    // Actions.
    FilledButton* create = nullptr;
    FlatButton* cancel = nullptr;

    Category category = Category::Print;
    SelKind selKind = SelKind::None;
    int selIndex = -1;

    SizeUnit prevUnit = SizeUnit::Pixels; // to convert width/height when the unit changes
    double ratio = 16.0 / 9.0; // width/height (unit-agnostic) enforced while `link` is engaged
    // The Color dropdown's "Custom..." state: the validated .icc + its display name, and the
    // last built-in choice to fall back to when a pick is cancelled or fails to load.
    std::string iccPath;
    std::string iccName;
    int lastColorSpace = static_cast<int>(core::ColorSpace::SRGB);
    // The Name field's last user-editable text: a Recent selection shows the FILE's own name in
    // the greyed box (user round 6), and leaving it brings the user's text back.
    std::string editableName;
    // The user's explicit orientation pick: -1 = follow the preset/custom values as they are,
    // 0/1 = portrait/landscape. It SURVIVES preset clicks (pick Landscape then A4 = A4
    // landscape) and turns the preset sheets accordingly (user round 4).
    int orientationPref = -1;
    bool syncing = false;                 // suppress field callbacks during programmatic updates
    bool accepted = false;                // Create/Open pressed (vs Cancel/Escape)
    NewDocumentChoice choice;
};

// Re-capture the linked ratio from the fields (after a preset fill, a unit change, or the link
// engaging) so the NEXT hand edit preserves what is currently shown.
void captureRatio(DialogState& s) {
    const double wv = parseDouble(s.width->value());
    const double hv = parseDouble(s.height->value());
    if (wv > 0.0 && hv > 0.0)
        s.ratio = wv / hv;
}

// The Color dropdown's item count is the enum plus one trailing "Custom..." row.
constexpr int kCustomColorIdx = static_cast<int>(core::ColorSpace::Rec2020) + 1;

NewDocumentSpec readSpec(const DialogState& s) {
    NewDocumentSpec spec;
    if (const char* n = s.name->value())
        spec.title = n;
    spec.width = parseDouble(s.width->value());
    spec.height = parseDouble(s.height->value());
    spec.unit = static_cast<SizeUnit>(s.unit->value());
    spec.dpi = parsePositive(s.dpi->value(), 72.0);
    if (s.colorSpace->value() == kCustomColorIdx && !s.iccPath.empty()) {
        spec.colorSpace = core::ColorSpace::SRGB; // the fallback under a custom profile
        spec.iccProfilePath = s.iccPath;
    } else { // clamp: the "Custom..." row with no profile (transient) must not cast to enum 5
        spec.colorSpace = static_cast<core::ColorSpace>(
            std::clamp(s.colorSpace->value(), 0, kCustomColorIdx - 1));
    }
    spec.precision = static_cast<core::Precision>(s.depth->value());
    spec.background = static_cast<NewDocBackground>(s.background->value());
    return spec;
}

void updateSummary(DialogState& s) {
    // The orientation switch mirrors the fields whatever the selection -- a file-backed card's
    // seeded values show that document's orientation (read-only, the switch is disabled then).
    {
        const double wv = parseDouble(s.width->value());
        const double hv = parseDouble(s.height->value());
        // A SQUARE canvas can't take its state from the fields, so while the switch is live it
        // shows the user's explicit pick instead -- clicking Landscape on 1024 x 1024 turned
        // every preset sheet yet left the switch dead, which read as the switch "not working"
        // (user round 6). Disabled (file-backed) squares keep the neutral face: that document
        // genuinely has no orientation.
        const int squareState = s.orientation->active_r() ? s.orientationPref : -1;
        s.orientation->setState(wv > hv ? 1 : wv < hv ? 0 : squareState);
    }
    if (s.selKind == SelKind::Clipboard) {
        s.sizeReadout->setText(s.ctx->clipboardSubtitle);
        s.memReadout->setText(_("from the clipboard"));
        return;
    }
    if (s.selKind == SelKind::Template || s.selKind == SelKind::Recent) {
        // The document comes from a file: the summary shows what is known about it -- its pixel
        // size when the cache knows it, and (for a recent) WHERE it lives.
        const auto& card = (s.selKind == SelKind::Template
                                ? s.ctx->templates
                                : s.ctx->recents)[static_cast<std::size_t>(s.selIndex)];
        s.sizeReadout->setText(!card.detail.empty() ? card.detail : card.title);
        s.memReadout->setText(card.subtitle);
        return;
    }
    const NewDocumentSpec spec = readSpec(s);
    s.sizeReadout->setText(std::to_string(spec.pixelWidth()) + " × " +
                           std::to_string(spec.pixelHeight()) + " px");
    s.memReadout->setText(std::string("≈ ") + formatByteSize(layerMemoryBytes(spec)) + " " +
                          _("per layer"));
}

// The orientation switch's action: exchange Width and Height. Runs through the syncing guard so
// the linked-ratio machinery doesn't fight it; the ratio then re-captures inverted. A selected
// preset card deliberately STAYS selected -- "A4, landscape" is still A4.
void swapOrientation(DialogState& s) {
    const double wv = parseDouble(s.width->value());
    const double hv = parseDouble(s.height->value());
    if (wv != hv) { // a square swaps nothing -- but the summary still runs so the switch
        s.syncing = true; // reflects the pick (see updateSummary's square rule)
        setNumber(s.width, hv);
        setNumber(s.height, wv);
        s.syncing = false;
        captureRatio(s);
    }
    updateSummary(s);
}

void setSizeFormActive(DialogState& s, bool active) {
    for (Fl_Widget* w : s.sizeForm) {
        if (active)
            w->activate();
        else
            w->deactivate();
    }
}

void refreshCardRings(DialogState& s) {
    for (std::size_t i = 0; i < s.cards.size(); ++i)
        s.cards[i]->setSelected(s.cardKinds[i] == s.selKind &&
                                s.cardIndices[i] == s.selIndex);
}

void updateUnitTags(DialogState& s) {
    const std::string tag(sizeUnitAbbrev(static_cast<SizeUnit>(s.unit->value())));
    s.width->setUnitText(tag);
    s.height->setUnitText(tag);
}

// Fill the size half of the form (used by preset AND custom-size cards; both stay editable).
void applySizeToForm(DialogState& s, double w, double h, SizeUnit unit, double dpi) {
    // The explicit orientation pick survives the card: A4 under Landscape fills 297 x 210.
    if ((s.orientationPref == 1 && w < h) || (s.orientationPref == 0 && w > h))
        std::swap(w, h);
    s.syncing = true;
    setNumber(s.width, w);
    setNumber(s.height, h);
    s.unit->value(static_cast<int>(unit));
    s.prevUnit = unit;
    setNumber(s.dpi, dpi);
    s.syncing = false;
    updateUnitTags(s);
    captureRatio(s); // the linked ratio follows what the card just showed
}

void applyPresetToForm(DialogState& s, int presetIdx) {
    const DocumentPreset& p = documentPresets()[static_cast<std::size_t>(presetIdx)];
    applySizeToForm(s, p.width, p.height, p.unit, p.dpi);
}

// Seed the (about to be disabled) size form with a file-backed card's real values, so the greyed
// form reads as the document you would get -- not leftover blank-form values (user round 4).
void seedFormFromValues(DialogState& s, const NewDocumentSpec& v) {
    s.syncing = true;
    setNumber(s.width, v.width);
    setNumber(s.height, v.height);
    s.unit->value(static_cast<int>(v.unit));
    s.prevUnit = v.unit;
    setNumber(s.dpi, v.dpi);
    s.colorSpace->value(static_cast<int>(v.colorSpace));
    s.colorSpace->setOverrideText(""); // a file-backed seed replaces any custom-profile pick
    s.iccPath.clear();
    s.iccName.clear();
    s.depth->value(static_cast<int>(v.precision));
    s.syncing = false;
    updateUnitTags(s);
    captureRatio(s);
}

void selectCard(DialogState& s, SelKind kind, int index) {
    const bool wasRecent = s.selKind == SelKind::Recent;
    s.selKind = kind;
    s.selIndex = index;
    refreshCardRings(s);
    if (wasRecent && kind != SelKind::Recent) // leaving a Recent card: the file's name goes
        s.name->value(s.editableName.c_str()); // with it; the user's own text comes back
    if (kind == SelKind::Preset || kind == SelKind::CustomSize) {
        if (kind == SelKind::Preset) {
            applyPresetToForm(s, index);
        } else {
            const NewDocumentSpec& v = s.ctx->customSizes[static_cast<std::size_t>(index)];
            applySizeToForm(s, v.width, v.height, v.unit, v.dpi);
        }
        setSizeFormActive(s, true);
        s.name->activate();
        s.create->copy_label(_("Create"));
    } else {
        // The document's geometry/colour comes from the file (or the clipboard image); only the
        // Name still applies, so the size half of the form reads disabled. Opening a RECENT
        // keeps the file's own name, so there the Name field greys out too (user 2026-07-22:
        // an active field that changes nothing reads as a bug). The greyed form is SEEDED with
        // the source's real values where known (px size, ppi, colour, depth).
        if (kind == SelKind::Clipboard) {
            if (s.ctx->clipboardValues.has_value())
                seedFormFromValues(s, *s.ctx->clipboardValues);
        } else {
            const auto& list = kind == SelKind::Template ? s.ctx->templates : s.ctx->recents;
            if (list[static_cast<std::size_t>(index)].values.has_value())
                seedFormFromValues(s, *list[static_cast<std::size_t>(index)].values);
        }
        setSizeFormActive(s, false);
        if (kind == SelKind::Template) {
            s.name->value(s.ctx->templates[static_cast<std::size_t>(index)].title.c_str());
            s.name->activate();
            s.create->copy_label(_("Create"));
        } else if (kind == SelKind::Recent) {
            // The greyed Name shows the document you would OPEN (user round 6): the manifest's
            // own title where the .mosaic carries one, else the card title (its file name).
            if (!wasRecent)
                s.editableName = s.name->value() != nullptr ? s.name->value() : "";
            const NewDocumentCard& card = s.ctx->recents[static_cast<std::size_t>(index)];
            const bool hasDocName = card.values.has_value() && !card.values->title.empty();
            s.name->value((hasDocName ? card.values->title : card.title).c_str());
            s.name->deactivate();
            s.create->copy_label(_("Open"));
        } else {
            s.name->activate();
            s.create->copy_label(_("Create"));
        }
    }
    updateSummary(s);
}

void acceptDialog(DialogState& s, Fl_Window* win) {
    switch (s.selKind) {
    case SelKind::Template:
        s.choice.kind = NewDocumentChoice::Kind::Template;
        s.choice.path = s.ctx->templates[static_cast<std::size_t>(s.selIndex)].path;
        s.choice.spec = readSpec(s); // carries the Name for the instantiated document
        break;
    case SelKind::Recent:
        s.choice.kind = NewDocumentChoice::Kind::RecentFile;
        s.choice.path = s.ctx->recents[static_cast<std::size_t>(s.selIndex)].path;
        break;
    case SelKind::Clipboard:
        s.choice.kind = NewDocumentChoice::Kind::Clipboard;
        s.choice.spec = readSpec(s); // carries the Name for the pasted document
        break;
    default:
        s.choice.kind = NewDocumentChoice::Kind::Blank;
        s.choice.spec = readSpec(s);
        break;
    }
    s.accepted = true;
    win->hide();
}

// ---- gallery construction -------------------------------------------------------------------

// The sheet a placeholder card draws. Sizing settled after three rounds of feedback: sheets are
// UNIFORM PER ORIENTATION CLASS -- every landscape sheet is one fixed box, every portrait sheet
// its rotation, every square sheet one square -- because any ratio-proportional scheme (fit,
// equal-area, shared-height) left some card reading smaller than its neighbours (the Square
// each time). The exact ratio is the subtitle's job; the sheet communicates orientation.
void drawSheet(int px, int py, int pw, int ph, double aspect, bool checker,
               common::Color8 fill, common::Color8 labelInk, const std::string& label) {
    int rw = 92;
    int rh = 58;
    if (aspect < 0.98) { // portrait: the landscape box turned upright
        rw = 58;
        rh = 92;
    } else if (aspect <= 1.02) { // square: between the two
        rw = 68;
        rh = 68;
    }
    if (rh > ph) { // a shallow preview cell scales the box down proportionally
        rw = static_cast<int>(std::lround(rw * static_cast<double>(ph) / rh));
        rh = ph;
    }
    if (rw > pw) {
        rh = static_cast<int>(std::lround(rh * static_cast<double>(pw) / rw));
        rw = pw;
    }
    rw = std::max(8, rw);
    rh = std::max(8, rh);
    const int rx = px + (pw - rw) / 2;
    const int ry = py + (ph - rh) / 2;

    if (checker) { // the transparent background: the compositor's checkerboard language
        for (int ty = 0; ty < rh; ty += 6)
            for (int tx = 0; tx < rw; tx += 6) {
                const bool dark = (((tx / 6) + (ty / 6)) & 1) != 0;
                fl_color(dark ? fl_rgb_color(150, 150, 150) : fl_rgb_color(205, 205, 205));
                fl_rectf(rx + tx, ry + ty, std::min(6, rw - tx), std::min(6, rh - ty));
            }
    } else {
        fl_color(toFl(fill));
        fl_rectf(rx, ry, rw, rh);
    }
    fl_color(toFl(activePalette().border));
    fl_rect(rx, ry, rw, rh);

    int size = 12;
    fl_font(FL_HELVETICA, size);
    while (size > 7 && fl_width(label.c_str()) > rw - 8) {
        fl_font(FL_HELVETICA, --size);
    }
    fl_color(toFl(labelInk));
    fl_draw(label.c_str(), rx, ry, rw, rh, FL_ALIGN_CENTER, nullptr, 0);
}

// Size-preset placeholder art: the sheet at the preset's aspect, FILLED with the currently
// chosen Background (white / black / transparency checker) so the card previews the canvas the
// preset would actually create; the short name is centred on it.
GalleryCard::PreviewFn paperPreview(const DialogState* st, double aspect, std::string label) {
    return [st, aspect, label = std::move(label)](int px, int py, int pw, int ph, common::Color8) {
        // The explicit orientation pick turns the sheet (the preset's DIMS stay canonical --
        // the swap happens when the preset applies to the form).
        double a = aspect;
        if (st->orientationPref == 1)
            a = std::max(a, 1.0 / a);
        else if (st->orientationPref == 0)
            a = std::min(a, 1.0 / a);
        const auto bg = static_cast<NewDocBackground>(st->background->value());
        common::Color8 fill{250, 250, 252, 255};   // White: paper-white in both themes
        common::Color8 ink{120, 124, 134, 255};
        if (bg == NewDocBackground::Black) {
            fill = {24, 26, 30, 255};
            ink = {150, 155, 166, 255};
        } else if (bg == NewDocBackground::Transparent) {
            ink = {86, 90, 100, 255};
        }
        drawSheet(px, py, pw, ph, a, bg == NewDocBackground::Transparent, fill, ink, label);
    };
}

// Placeholder art for a file card with no cached thumbnail: a portrait white sheet with the
// file kind (a file's own content, not the Background choice -- deliberately bg-independent).
GalleryCard::PreviewFn filePreview(std::string kind) {
    return [kind = std::move(kind)](int px, int py, int pw, int ph, common::Color8) {
        drawSheet(px, py, pw, ph, 3.0 / 4.0, false, {250, 250, 252, 255}, {120, 124, 134, 255},
                  kind);
    };
}

std::string upperExtOf(const std::string& path) {
    const std::filesystem::path p(path);
    std::string ext = p.extension().string();
    if (!ext.empty() && ext.front() == '.')
        ext.erase(ext.begin());
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return ext.empty() ? std::string("FILE") : ext;
}

void addEmptyGalleryNote(DialogState& s, const char* text) {
    auto* note = new Fl_Box(s.gallery->x() + kGalleryPad, s.gallery->y() + kGalleryPad,
                            s.gallery->w() - 2 * kGalleryPad - Fl::scrollbar_size(), 60);
    note->copy_label(text);
    note->align(FL_ALIGN_CENTER | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    note->labelcolor(toFl(activePalette().textMuted));
    note->labelsize(12);
    note->box(FL_NO_BOX);
}

// A gallery section caption: a muted label with a hairline running out to the right edge --
// separates Clipboard / Sizes / Files inside the Recent gallery (user round 5: hand-entered
// sizes must not read as files).
class SectionLabel : public Fl_Widget {
public:
    SectionLabel(int X, int Y, int W, int H, const char* text) : Fl_Widget(X, Y, W, H) {
        copy_label(text);
    }

protected:
    void draw() override {
        const Palette& p = activePalette();
        fl_color(toFl(p.windowBg)); // erase: sits on the gallery ground
        fl_rectf(x(), y(), w(), h());
        fl_font(FL_HELVETICA, 11);
        fl_color(toFl(p.textMuted));
        fl_draw(label(), x(), y(), w(), h(), FL_ALIGN_LEFT | FL_ALIGN_INSIDE, nullptr, 0);
        const int tw = static_cast<int>(fl_width(label()));
        const int ly = y() + h() / 2;
        fl_color(toFl(p.border));
        fl_line(x() + tw + 10, ly, x() + w() - 1, ly);
    }
};

constexpr int kSectionH = 24; // one caption row above a section's cards

void openCardHousekeeping(DialogState& s, SelKind kind, int index); // defined after rebuild

// `cardY` is the card's absolute y (the section layout owns the vertical cursor).
void addCard(DialogState& s, SelKind kind, int index, int col, int cardY,
             const std::string& title, const std::string& subtitle) {
    const int cx = s.gallery->x() + kGalleryPad + col * (kCardW + kCardGap);
    auto* card = new GalleryCard(cx, cardY, kCardW, kCardH, title, subtitle);
    DialogState* st = &s;
    card->setOnSelect([st, kind, index] { selectCard(*st, kind, index); });
    card->setOnActivate([st, kind, index] {
        selectCard(*st, kind, index);
        acceptDialog(*st, st->gallery->window());
    });
    // The remembered lists take right-click housekeeping (round 7): a recent you just don't
    // want listed was otherwise stuck until ten newer opens pushed it out.
    if (kind == SelKind::Recent || kind == SelKind::CustomSize)
        card->setOnContextMenu([st, kind, index] { openCardHousekeeping(*st, kind, index); });
    s.cards.push_back(card);
    s.cardKinds.push_back(kind);
    s.cardIndices.push_back(index);
}

void rebuildGallery(DialogState& s) {
    s.gallery->scroll_to(0, 0);
    s.galleryContent->clear();
    s.cards.clear();
    s.cardKinds.clear();
    s.cardIndices.clear();

    const int cols =
        std::max(1, (s.gallery->w() - 2 * kGalleryPad - Fl::scrollbar_size() + kCardGap) /
                        (kCardW + kCardGap));
    const auto rowsFor = [cols](int count) { return (count + cols - 1) / cols; };
    const bool presetCat = s.category == Category::Print || s.category == Category::Screen ||
                           s.category == Category::Texture;
    const PresetCategory want = s.category == Category::Print    ? PresetCategory::Print
                                : s.category == Category::Screen ? PresetCategory::Screen
                                                                 : PresetCategory::Texture;
    const auto& presets = documentPresets();

    // The Recent gallery's section plan (Clipboard / Sizes / Files, in that order). Captions
    // appear only when at least two sections are present -- a lone section keeps the clean
    // unlabelled look every other category has.
    struct Section {
        const char* label;
        SelKind kind;
        int count;
    };
    std::vector<Section> sections;
    if (s.category == Category::Recent) {
        if (s.ctx->clipboardHasImage)
            sections.push_back({_("Clipboard"), SelKind::Clipboard, 1});
        if (!s.ctx->customSizes.empty())
            sections.push_back({_("Sizes"), SelKind::CustomSize,
                                static_cast<int>(s.ctx->customSizes.size())});
        if (!s.ctx->recents.empty())
            sections.push_back(
                {_("Files"), SelKind::Recent, static_cast<int>(s.ctx->recents.size())});
    }
    const bool labelled = sections.size() >= 2;

    // Measure first: the content group must take its final size BEFORE children are added (an
    // Fl_Group resize with children rescales them -- its default resizable is itself).
    int contentH = kGalleryPad;
    if (presetCat) {
        int count = 0;
        for (const DocumentPreset& p : presets)
            count += presetCategory(p) == want ? 1 : 0;
        contentH += rowsFor(count) * (kCardH + kCardGap);
    } else if (s.category == Category::Templates) {
        const int count = static_cast<int>(s.ctx->templates.size());
        contentH += std::max(1, rowsFor(count)) * (kCardH + kCardGap);
    } else {
        for (const Section& sec : sections)
            contentH += (labelled ? kSectionH : 0) + rowsFor(sec.count) * (kCardH + kCardGap);
        if (sections.empty())
            contentH += kCardH + kCardGap; // room for the empty-state note
    }
    contentH += kGalleryPad - kCardGap;
    s.galleryContent->resizable(nullptr); // Fl_Group::clear() resets it to the group itself
    s.galleryContent->resize(s.gallery->x(), s.gallery->y(),
                             s.gallery->w() - Fl::scrollbar_size(), contentH);

    s.galleryContent->begin();
    const int contentW = s.gallery->w() - 2 * kGalleryPad - Fl::scrollbar_size();
    int yCursor = s.gallery->y() + kGalleryPad; // absolute; sections advance it
    if (presetCat) {
        int slot = 0;
        for (std::size_t i = 0; i < presets.size(); ++i) {
            if (presetCategory(presets[i]) != want)
                continue;
            addCard(s, SelKind::Preset, static_cast<int>(i), slot % cols,
                    yCursor + (slot / cols) * (kCardH + kCardGap),
                    std::string(presetShortName(presets[i])), presetDetail(presets[i]));
            s.cards.back()->setPreviewFn(
                paperPreview(&s, presets[i].width / presets[i].height,
                             std::string(presetShortName(presets[i]))));
            ++slot;
        }
    } else if (s.category == Category::Templates) {
        const auto& list = s.ctx->templates;
        for (std::size_t i = 0; i < list.size(); ++i) {
            addCard(s, SelKind::Template, static_cast<int>(i), static_cast<int>(i) % cols,
                    yCursor + (static_cast<int>(i) / cols) * (kCardH + kCardGap), list[i].title,
                    list[i].subtitle);
            if (!list[i].thumb.empty())
                s.cards.back()->setThumbnail(list[i].thumb); // copies; rebuilt per switch
            else
                s.cards.back()->setPreviewFn(filePreview("MOSAIC"));
        }
        if (list.empty())
            addEmptyGalleryNote(s, _("No templates installed yet.\nTemplate .mosaic files "
                                     "appear here from the app's data/presets folder."));
    } else {
        for (const Section& sec : sections) {
            if (labelled) {
                new SectionLabel(s.gallery->x() + kGalleryPad, yCursor, contentW,
                                 kSectionH - 6, sec.label);
                yCursor += kSectionH;
            }
            for (int i = 0; i < sec.count; ++i) {
                const int cardY = yCursor + (i / cols) * (kCardH + kCardGap);
                if (sec.kind == SelKind::Clipboard) {
                    // The pre-fetched clipboard image leads the Recent gallery.
                    addCard(s, SelKind::Clipboard, 0, i % cols, cardY, _("Clipboard"),
                            s.ctx->clipboardSubtitle);
                    if (!s.ctx->clipboardThumb.empty())
                        s.cards.back()->setThumbnail(s.ctx->clipboardThumb);
                    else
                        s.cards.back()->setPreviewFn(filePreview("IMAGE"));
                } else if (sec.kind == SelKind::CustomSize) {
                    // A remembered hand-entered size: preset-style sheet art, numeric face.
                    const NewDocumentSpec& v =
                        s.ctx->customSizes[static_cast<std::size_t>(i)];
                    addCard(s, SelKind::CustomSize, i, i % cols, cardY, customSizeTitle(v),
                            numToken(v.dpi) + " ppi");
                    s.cards.back()->setPreviewFn(
                        paperPreview(&s, v.width / v.height, customSizeFace(v)));
                } else {
                    const NewDocumentCard& card =
                        s.ctx->recents[static_cast<std::size_t>(i)];
                    addCard(s, SelKind::Recent, i, i % cols, cardY, card.title, card.subtitle);
                    if (!card.thumb.empty())
                        s.cards.back()->setThumbnail(card.thumb);
                    else
                        s.cards.back()->setPreviewFn(filePreview(upperExtOf(card.path)));
                }
            }
            yCursor += rowsFor(sec.count) * (kCardH + kCardGap);
        }
        if (sections.empty())
            addEmptyGalleryNote(s, _("No recent files yet.\nDocuments you open or save "
                                     "appear here."));
    }
    s.galleryContent->end();
    refreshCardRings(s);
    s.gallery->redraw();
}

// A card's right-click removal: take the entry off the dialog's own list copy, tell the caller
// (so the persisted list follows), fix up the selection, and rebuild the gallery in place.
void removeRecentEntry(DialogState& s, SelKind kind, int index) {
    NewDocumentContext& ctx = *s.ctx;
    if (kind == SelKind::Recent) {
        if (ctx.onRemoveRecentFile)
            ctx.onRemoveRecentFile(ctx.recents[static_cast<std::size_t>(index)].path);
        ctx.recents.erase(ctx.recents.begin() + index);
    } else {
        if (ctx.onForgetRecentSize)
            ctx.onForgetRecentSize(ctx.customSizes[static_cast<std::size_t>(index)]);
        ctx.customSizes.erase(ctx.customSizes.begin() + index);
    }
    if (s.selKind == kind) {
        if (s.selIndex == index) { // the selected card is gone; back to a blank form
            s.selKind = SelKind::None;
            s.selIndex = -1;
            if (kind == SelKind::Recent) { // the greyed name/form belonged to the removed card
                s.name->value(s.editableName.c_str());
                setSizeFormActive(s, true);
                s.name->activate();
                s.create->copy_label(_("Create"));
            }
        } else if (s.selIndex > index) {
            --s.selIndex; // the list shifted under the selection
        }
    }
    rebuildGallery(s);
    updateSummary(s);
}

// The right-click menu on a remembered card (round 7): one action -- off the list.
void openCardHousekeeping(DialogState& s, SelKind kind, int index) {
    ContextMenu* menu = contextMenuFor(s.gallery->top_window());
    if (menu == nullptr)
        return;
    DialogState* st = &s;
    std::vector<ContextAction> actions;
    actions.push_back({kind == SelKind::Recent ? _("Remove from Recents") : _("Forget This Size"),
                       [st, kind, index] { removeRecentEntry(*st, kind, index); }, true, false});
    menu->openWith(Fl::event_x(), Fl::event_y(), std::move(actions));
}

void switchCategory(DialogState& s, Category c) {
    if (s.category == c)
        return;
    s.category = c;
    for (int i = 0; i < kCategoryCount; ++i)
        s.rail[static_cast<std::size_t>(i)]->setActive(static_cast<Category>(i) == c);
    // A category switch drops a FILE-BACKED selection -- Create must never read "Open" for a
    // card that is no longer on screen. A preset/custom-size selection PERSISTS: the live form
    // is exactly what Create makes, and the card's ring is simply there again on return (user
    // round 7: switching away and back lost the visual selection).
    if (s.selKind == SelKind::Recent) // the greyed box showed the FILE's name; hand the field
        s.name->value(s.editableName.c_str()); // back with the user's own text
    if (s.selKind != SelKind::Preset && s.selKind != SelKind::CustomSize) {
        s.selKind = SelKind::None;
        s.selIndex = -1;
        setSizeFormActive(s, true);
        s.name->activate();
        s.create->copy_label(_("Create"));
    }
    rebuildGallery(s);
    updateSummary(s);
}

// ---- field callbacks ------------------------------------------------------------------------

void onManualEdit(DialogState& s, Fl_Widget* sender) {
    if (s.syncing)
        return;
    // The engaged link re-derives the OTHER dimension from the captured ratio as you type.
    if (s.link->linked() && (sender == s.width || sender == s.height) && s.ratio > 0.0) {
        s.syncing = true;
        if (sender == s.width)
            setNumber(s.height, parseDouble(s.width->value()) / s.ratio);
        else
            setNumber(s.width, parseDouble(s.height->value()) * s.ratio);
        s.syncing = false;
    }
    if (s.selKind == SelKind::Preset ||
        s.selKind == SelKind::CustomSize) { // a hand edit to size/resolution means "custom" again
        s.selKind = SelKind::None;
        s.selIndex = -1;
        refreshCardRings(s);
    }
    updateSummary(s);
}

void onUnitChange(DialogState& s) {
    if (!s.syncing) {
        // Keep the pixel dimensions fixed; re-express width/height in the newly chosen unit.
        const double dpi = parsePositive(s.dpi->value(), 72.0);
        const double pxW = unitToPixels(parseDouble(s.width->value()), s.prevUnit, dpi);
        const double pxH = unitToPixels(parseDouble(s.height->value()), s.prevUnit, dpi);
        const SizeUnit nu = static_cast<SizeUnit>(s.unit->value());
        s.syncing = true;
        setNumber(s.width, pixelsToUnit(pxW, nu, dpi));
        setNumber(s.height, pixelsToUnit(pxH, nu, dpi));
        s.syncing = false;
        s.prevUnit = nu;
        updateUnitTags(s);
        captureRatio(s); // same ratio, re-captured from the re-expressed values
        if (s.selKind == SelKind::Preset || s.selKind == SelKind::CustomSize) {
            s.selKind = SelKind::None;
            s.selIndex = -1;
            refreshCardRings(s);
        }
    }
    updateSummary(s);
}

void cbSizeEdit(Fl_Widget* w, void* v) { onManualEdit(*static_cast<DialogState*>(v), w); }
void cbUnit(Fl_Widget*, void* v) { onUnitChange(*static_cast<DialogState*>(v)); }
void cbSummaryOnly(Fl_Widget*, void* v) { updateSummary(*static_cast<DialogState*>(v)); }

// Set the Color dropdown's custom-profile state: Custom row selected, the profile's real name
// shown in the closed control (Dropdown::setOverrideText).
void showCustomProfile(DialogState& s, std::string path, std::string name) {
    s.iccPath = std::move(path);
    s.iccName = std::move(name);
    if (s.iccName.empty()) // a profile with no description tag still needs a face
        s.iccName = std::filesystem::path(s.iccPath).filename().string();
    s.colorSpace->value(kCustomColorIdx);
    s.colorSpace->setOverrideText(s.iccName);
}

void onColorSpaceChange(DialogState& s) {
    if (s.colorSpace->value() != kCustomColorIdx) {
        s.lastColorSpace = s.colorSpace->value();
        s.iccPath.clear();
        s.iccName.clear();
        s.colorSpace->setOverrideText("");
        updateSummary(s);
        return;
    }
    // "Custom...": pick an RGB .icc, validated by actually loading it (the Settings-dialog
    // convention -- a wrong pick never sticks). Cancel/failure falls back to the last choice.
    Fl_Native_File_Chooser chooser;
    chooser.title(_("Choose an RGB ICC profile"));
    chooser.type(Fl_Native_File_Chooser::BROWSE_FILE);
    chooser.filter(_("ICC profiles\t*.{icc,icm}"));
    const char* picked = chooser.show() == 0 ? chooser.filename() : nullptr;
    bool ok = false;
    if (picked != nullptr && picked[0] != '\0') {
        core::ColorEngine probe(core::ColorSpace::SRGB);
        ok = probe.loadWorkingProfileFile(picked);
        if (!ok) {
            // The app's own themed "tell" (docs/askortell-dialog.md), never FLTK's stock
            // fl_alert -- system message boxes ignore the theme and the icon language.
            AskOrTellDialog dlg;
            dlg.ask({AskOrTellDialog::Icon::Warning, _("Not an RGB profile"),
                     _("That file is not an RGB ICC profile, so it was not applied."),
                     {_("OK")}},
                    s.colorSpace->window());
        }
    }
    if (ok) {
        showCustomProfile(s, picked, core::iccProfileName(picked));
    } else {
        s.colorSpace->value(s.lastColorSpace);
        s.colorSpace->setOverrideText("");
        s.iccPath.clear();
        s.iccName.clear();
    }
    updateSummary(s);
}
void cbColorSpace(Fl_Widget*, void* v) { onColorSpaceChange(*static_cast<DialogState*>(v)); }
void cbBackgroundChange(Fl_Widget*, void* v) {
    auto& s = *static_cast<DialogState*>(v);
    updateSummary(s);
    s.gallery->redraw(); // the preset sheets preview the chosen background live
}

void cbCreate(Fl_Widget* w, void* v) {
    acceptDialog(*static_cast<DialogState*>(v), w->window());
}

// Used both as the Cancel button callback and the window callback (Escape / WM close).
void cbCancel(Fl_Widget* w, void*) {
    Fl_Window* win = w->as_window() != nullptr ? w->as_window() : w->window();
    if (win != nullptr)
        win->hide();
}

// The dialog window: hosts the themed pop-ups (outside-click dismissal) and the Enter/Escape
// keys (Escape closes an open list first, then cancels -- the FillDialog convention).
class DialogWindow : public Fl_Double_Window {
public:
    DialogWindow(int W, int H, const char* title, DialogState* st)
        : Fl_Double_Window(W, H, title), m_st(st) {}

protected:
    int handle(int event) override {
        if (event == FL_PUSH) {
            dismissActiveDropdownPopupOnOutsideClick(Fl::event_x(), Fl::event_y());
            dismissActiveContextMenuOnOutsideClick(Fl::event_x(), Fl::event_y());
        }
        if (event == FL_KEYBOARD) {
            const int key = Fl::event_key();
            if (key == FL_Escape) {
                if (activeContextMenu() != nullptr) {
                    dismissActiveContextMenu();
                    return 1;
                }
                if (activeDropdownPopup() != nullptr) {
                    dismissActiveDropdownPopup();
                    return 1;
                }
                // Fall through to FLTK's default: the window callback (= cancel).
            } else if ((key == FL_Enter || key == FL_KP_Enter) &&
                       activeDropdownPopup() == nullptr && activeContextMenu() == nullptr) {
                acceptDialog(*m_st, this);
                return 1;
            }
        }
        return Fl_Double_Window::handle(event);
    }

private:
    DialogState* m_st;
};

} // namespace

std::optional<NewDocumentChoice> showNewDocumentDialog(NewDocumentContext ctx,
                                                       const NewDocumentSpec* initial,
                                                       Fl_Window* host) {
    const Palette& pal = activePalette();
    const NewDocumentSpec seed = initial != nullptr ? *initial : defaultNewDocumentSpec();

    DialogState st;
    st.ctx = &ctx;
    st.prevUnit = seed.unit;

    // The window is on the stack; every child is heap-allocated so the group owns and frees them.
    DialogWindow win(kWinW, kWinH, _("New Document"), &st);
    win.color(toFl(pal.windowBg));
    win.begin();

    // ---- left rail ----
    {
        auto* railBg = new Fl_Box(0, 0, kRailW, kBodyH);
        railBg->box(FL_FLAT_BOX);
        railBg->color(toFl(pal.panelBg));
        new TitleDrum(0, kMargin, kRailW, 24); // the cycling dialog title tops the stack
        int ry = kMargin + 24 + 14;
        for (int i = 0; i < kCategoryCount; ++i) {
            const auto cat = static_cast<Category>(i);
            DialogState* stp = &st;
            auto* item = new RailItem(0, ry, kRailW, 34, categoryName(cat),
                                      [stp, cat] { switchCategory(*stp, cat); });
            if (cat == Category::Recent)
                item->setBadge(RailItem::Badge::Clock);
            else if (cat == Category::Templates)
                item->setBadge(RailItem::Badge::Cake);
            st.rail.push_back(item);
            ry += 36;
        }
    }

    // ---- centre gallery ----
    st.gallery = new ScrollView(kGalleryX, 0, kGalleryW, kBodyH);
    st.gallery->type(Fl_Scroll::VERTICAL);
    st.gallery->box(FL_NO_BOX);
    st.gallery->color(toFl(pal.windowBg));
    // One inner content group is rebuilt per category switch (never Fl_Scroll::clear(), which
    // would take the scrollbars with it).
    st.galleryContent = new Fl_Group(kGalleryX, 0, kGalleryW - Fl::scrollbar_size(), kBodyH);
    st.galleryContent->box(FL_NO_BOX);
    st.galleryContent->end();
    st.gallery->end();

    // ---- right form panel ----
    {
        auto* form = new Panel(kWinW - kFormW, 0, kFormW, kBodyH);
        form->borderEdges(Panel::EdgeLeft);
        form->begin();
        const int fx = kWinW - kFormW + kMargin;
        const int fw = kFormW - 2 * kMargin; // 248
        int fy = kMargin;

        fieldLabel(fx, fy, fw, _("Name"), pal);
        st.name = new TextInput(fx, fy + 16, fw, 26);
        styleInput(st.name, pal);
        st.name->value(seed.title.c_str());
        fy += 50;

        // Width [link] Height: two 106px columns bridged by the ratio-link chain; the unit reads
        // as a faint tag inside each field (so no external unit labels compete with the values).
        constexpr int kCol = 106;
        constexpr int kCol2X = kCol + 36; // the 36px bridge hosts the LinkButton
        st.sizeForm.push_back(fieldLabel(fx, fy, kCol, _("Width"), pal));
        st.width = new UnitField(fx, fy + 16, kCol, 26);
        styleInput(st.width, pal);
        st.width->when(FL_WHEN_CHANGED);
        st.width->callback(cbSizeEdit, &st);
        st.sizeForm.push_back(st.width);
        {
            DialogState* stp = &st;
            st.link = new LinkButton(fx + kCol + 4, fy + 16, 28, 26, [stp](bool on) {
                if (on)
                    captureRatio(*stp); // the engaged ratio is whatever is shown right now
            });
            st.sizeForm.push_back(st.link);
        }
        st.sizeForm.push_back(fieldLabel(fx + kCol2X, fy, kCol, _("Height"), pal));
        st.height = new UnitField(fx + kCol2X, fy + 16, kCol, 26);
        styleInput(st.height, pal);
        st.height->when(FL_WHEN_CHANGED);
        st.height->callback(cbSizeEdit, &st);
        st.sizeForm.push_back(st.height);
        fy += 50;

        st.sizeForm.push_back(fieldLabel(fx, fy, fw, _("Orientation"), pal));
        {
            DialogState* stp = &st;
            st.orientation =
                new OrientationSwitch(fx, fy + 16, fw, 26, [stp](bool landscape) {
                    stp->orientationPref = landscape ? 1 : 0;
                    swapOrientation(*stp);
                    stp->gallery->redraw(); // the preset sheets turn with the pick
                });
            st.sizeForm.push_back(st.orientation);
        }
        fy += 50;

        st.sizeForm.push_back(fieldLabel(fx, fy, kCol, _("Units"), pal));
        st.unit = new Dropdown(fx, fy + 16, kCol, 26);
        for (int u = 0; u <= static_cast<int>(SizeUnit::Points); ++u)
            st.unit->add(std::string(sizeUnitName(static_cast<SizeUnit>(u))).c_str());
        st.unit->callback(cbUnit, &st);
        st.sizeForm.push_back(st.unit);
        st.sizeForm.push_back(fieldLabel(fx + kCol2X, fy, kCol, _("Resolution"), pal));
        st.dpi = new UnitField(fx + kCol2X, fy + 16, kCol, 26); // aligned under Height
        styleInput(st.dpi, pal);
        st.dpi->setUnitText("ppi");
        st.dpi->setQuickPicks({72.0, 96.0, 150.0, 300.0, 600.0}); // the common ppi ladder
        st.dpi->when(FL_WHEN_CHANGED);
        st.dpi->callback(cbSizeEdit, &st);
        st.sizeForm.push_back(st.dpi);
        fy += 50;

        st.sizeForm.push_back(fieldLabel(fx, fy, fw, _("Color"), pal));
        st.colorSpace = new Dropdown(fx, fy + 16, fw, 26);
        for (auto cs : {core::ColorSpace::SRGB, core::ColorSpace::LinearSRGB,
                        core::ColorSpace::DisplayP3, core::ColorSpace::AdobeRGB,
                        core::ColorSpace::Rec2020})
            st.colorSpace->add(std::string(core::colorSpaceName(cs)).c_str());
        st.colorSpace->add(_("Custom…")); // an RGB .icc of the user's own (round 5)
        st.colorSpace->callback(cbColorSpace, &st);
        st.sizeForm.push_back(st.colorSpace);
        fy += 50;

        st.sizeForm.push_back(fieldLabel(fx, fy, kCol, _("Depth"), pal));
        st.depth = new Dropdown(fx, fy + 16, kCol, 26); // on the same grid as Width/Units
        for (auto pr : {core::Precision::U8, core::Precision::U16, core::Precision::F16,
                        core::Precision::F32})
            st.depth->add(std::string(core::precisionName(pr)).c_str());
        st.depth->callback(cbSummaryOnly, &st); // the memory estimate follows the bit depth
        st.sizeForm.push_back(st.depth);
        st.sizeForm.push_back(fieldLabel(fx + kCol2X, fy, kCol, _("Background"), pal));
        st.background = new Dropdown(fx + kCol2X, fy + 16, kCol, 26);
        for (int b = 0; b <= static_cast<int>(NewDocBackground::Transparent); ++b)
            st.background->add(
                std::string(newDocBackgroundName(static_cast<NewDocBackground>(b))).c_str());
        st.background->callback(cbBackgroundChange, &st); // the preset sheets follow it live
        st.sizeForm.push_back(st.background);
        fy += 50 + 12;

        // Document summary: the resolved pixel size + what one layer of it costs in memory --
        // or, for a file card, its name and LOCATION, both of arbitrary length: ScrollingLabels
        // pan long lines inside the panel instead of spilling past it (user round 6).
        st.sizeReadout = new ScrollingLabel(fx, fy, fw, 20);
        st.sizeReadout->setAlign(ScrollingLabel::Align::Left);
        st.sizeReadout->labelfont(FL_HELVETICA_BOLD);
        st.sizeReadout->labelsize(13);
        st.sizeReadout->labelcolor(toFl(pal.text));
        st.memReadout = new ScrollingLabel(fx, fy + 20, fw, 18);
        st.memReadout->setAlign(ScrollingLabel::Align::Left);
        st.memReadout->labelsize(12);
        st.memReadout->labelcolor(toFl(pal.textMuted));

        form->end();
    }

    // ---- bottom action bar ----
    {
        auto* rule = new Fl_Box(0, kBodyH, kWinW, 1);
        rule->box(FL_FLAT_BOX);
        rule->color(toFl(pal.border));
        const int by = kBodyH + (kBarH - 30) / 2;
        st.create = new FilledButton(kWinW - kMargin - 110, by, 110, 30, _("Create"));
        st.create->callback(cbCreate, &st);
        st.cancel = new FlatButton(kWinW - kMargin - 110 - 10 - 96, by, 96, 30, _("Cancel"));
        st.cancel->callback(cbCancel, &st);
    }

    // The themed pop-up hosts, created while the window is UNSHOWN (a sub-window added to a
    // realized parent is promoted to a stray top-level) and last (so they stack above the rest).
    (new ContextMenu())->hide();
    (new DropdownPopup())->hide();

    win.end();

    // Seed the controls (programmatic value() calls do not fire callbacks).
    st.unit->value(static_cast<int>(seed.unit));
    st.colorSpace->value(static_cast<int>(seed.colorSpace));
    st.lastColorSpace = static_cast<int>(seed.colorSpace);
    if (!seed.iccProfilePath.empty()) {
        // Last time's custom profile, re-offered only if it still loads (it may have moved).
        core::ColorEngine probe(core::ColorSpace::SRGB);
        if (probe.loadWorkingProfileFile(seed.iccProfilePath.c_str()))
            showCustomProfile(st, seed.iccProfilePath,
                              core::iccProfileName(seed.iccProfilePath));
    }
    st.depth->value(static_cast<int>(seed.precision));
    st.background->value(static_cast<int>(seed.background));
    setNumber(st.width, seed.width);
    setNumber(st.height, seed.height);
    setNumber(st.dpi, seed.dpi);
    updateUnitTags(st);
    captureRatio(st);

    // Land where the action is: a clipboard image leads Recent with its card pre-selected (you
    // copied something, then asked for a new document -- Enter finishes the thought); otherwise
    // the seed's preset category with its card selected, else Recent when anything is recent,
    // else Print.
    const int seedPreset = matchDocumentPreset(seed);
    if (ctx.clipboardHasImage)
        st.category = Category::Recent;
    else if (seedPreset >= 0)
        st.category =
            categoryOf(presetCategory(documentPresets()[static_cast<std::size_t>(seedPreset)]));
    else
        st.category = ctx.recents.empty() ? Category::Print : Category::Recent;
    for (int i = 0; i < kCategoryCount; ++i)
        st.rail[static_cast<std::size_t>(i)]->setActive(static_cast<Category>(i) == st.category);
    rebuildGallery(st);
    if (ctx.clipboardHasImage)
        selectCard(st, SelKind::Clipboard, 0);
    else if (seedPreset >= 0) {
        st.selKind = SelKind::Preset;
        st.selIndex = seedPreset;
        refreshCardRings(st);
    }
    updateSummary(st);

    win.callback(cbCancel, &st); // Escape / window-manager close = cancel
    win.set_modal();
    win.show();
    centerWindowOver(win, host); // over the app window; multi-monitor-correct without it

    while (win.shown())
        Fl::wait();

    if (!st.accepted)
        return std::nullopt;
    return st.choice;
}

} // namespace mosaic::ui
