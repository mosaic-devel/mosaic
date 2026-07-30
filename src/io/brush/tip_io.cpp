#include "io/brush/tip_io.hpp"

#include "core/brush/parse_util.hpp"
#include "io/brush/bytes.hpp"
#include "io/brush/png_text.hpp"
#include "io/detail.hpp"

#include <algorithm>
#include <cstring>
#include <string_view>
#include <vector>

namespace mosaic::io::brush {
namespace {

namespace cb = mosaic::core::brush;

constexpr std::size_t kGbrV1HeaderSize = 20; // header_size, version, w, h, bytes
constexpr std::size_t kGbrHeaderSize = 28;   // + magic, spacing (v2+)
constexpr std::uint32_t kMaxTipDim = mosaic::io::detail::kMaxDim;

void fail(std::string* error, const char* why) {
    if (error != nullptr)
        *error = why;
}

// ------------------------------------------------------------------------------------------------
// GBR -- one stamp, or one cell of a hose.

// The outcome of parsing one cell. `Skip` means the framing is intact but the content is not
// usable (unsupported depth, zero dimension, an out-of-range spacing): `next` is valid and the
// walk may continue. `Stop` means the framing itself broke: nothing past this point is a cell.
enum class CellStatus { Ok, Skip, Stop };

struct GbrCell {
    CellStatus status = CellStatus::Stop;
    std::size_t next = 0;
    cb::TipFrame frame;
    std::string name;
    double spacing = 0.25;
    bool colored = false;  // bytes == 4
    bool hasColor = false; // any non-grey pixel (the bytes==4 verdict; alpha plays no part)
};

[[nodiscard]] GbrCell readGbrCell(const std::uint8_t* data, std::size_t size, std::size_t pos) {
    GbrCell cell;
    // The producer requires the FULL 28-byte header even for v1 (it memcpy's the big struct
    // before looking at the version), so a shorter tail is not a cell.
    if (pos > size || size - pos < kGbrHeaderSize)
        return cell;

    detail::ByteReader r(data + pos, size - pos);
    const std::uint32_t headerSize = r.u32be();
    const std::uint32_t version = r.u32be();
    const std::uint32_t width = r.u32be();
    const std::uint32_t height = r.u32be();
    const std::uint32_t bytes = r.u32be();
    (void)r.u32be(); // the GIMP magic: read and NOT verified, as the producer does
    const std::uint32_t rawSpacing = r.u32be();

    const std::size_t nameOffset = version == 1 ? kGbrV1HeaderSize : kGbrHeaderSize;
    if (headerSize < nameOffset + 1 || headerSize > size - pos)
        return cell; // Stop: the name would overlap the header or run off the file

    // Dimension fences before any size arithmetic: a hostile header must not overflow the
    // framing math, let alone the allocation.
    if (width == 0 || height == 0 || width > kMaxTipDim || height > kMaxTipDim)
        return cell;
    const std::uint64_t pixels = std::uint64_t(width) * height;
    if (pixels > cb::kMaxTipPixels)
        return cell;

    const std::uint64_t dataBytes = pixels * bytes;
    const std::uint64_t total = headerSize + dataBytes;
    if (total > size - pos)
        return cell; // Stop: the cell's data does not fit -- `ncells` was a claim (§3.6.2)
    cell.next = pos + static_cast<std::size_t>(total);

    if (version == 1) {
        // No spacing field; the 0.25 default stands. Name encoding is historically undefined;
        // the bytes are taken as-is.
        cell.name.assign(reinterpret_cast<const char*>(data + pos + kGbrV1HeaderSize),
                         headerSize - kGbrV1HeaderSize - 1);
    } else {
        if (rawSpacing > 1000) {
            cell.status = CellStatus::Skip; // the producer refuses these outright
            return cell;
        }
        cell.spacing = rawSpacing / 100.0; // stored as a percentage
        cell.name.assign(reinterpret_cast<const char*>(data + pos + kGbrHeaderSize),
                         headerSize - kGbrHeaderSize - 1);
    }
    // Strip a stray NUL the length arithmetic may have included.
    if (const std::size_t nul = cell.name.find('\0'); nul != std::string::npos)
        cell.name.resize(nul);

    const std::uint8_t* px = data + pos + headerSize;
    cell.frame.width = width;
    cell.frame.height = height;
    cell.frame.rgba.resize(static_cast<std::size_t>(pixels) * 4);

    if (bytes == 1) {
        // §3.6.1: the raw byte IS coverage (255 = full paint); the tip image wants the opposite.
        for (std::size_t i = 0; i < pixels; ++i) {
            const std::uint8_t grey = static_cast<std::uint8_t>(255 - px[i]);
            cell.frame.rgba[i * 4 + 0] = grey;
            cell.frame.rgba[i * 4 + 1] = grey;
            cell.frame.rgba[i * 4 + 2] = grey;
            cell.frame.rgba[i * 4 + 3] = 255;
        }
    } else if (bytes == 4) {
        std::memcpy(cell.frame.rgba.data(), px, static_cast<std::size_t>(pixels) * 4);
        cell.colored = true;
        for (std::size_t i = 0; i < pixels; ++i) {
            const std::uint8_t* p = px + i * 4;
            if (p[0] != p[1] || p[1] != p[2]) {
                cell.hasColor = true;
                break;
            }
        }
    } else {
        cell.status = CellStatus::Skip; // an unsupported depth; the framing is still good
        cell.frame = {};
        return cell;
    }

    cell.status = CellStatus::Ok;
    return cell;
}

void applyCellVerdicts(TipFile& out, bool anyColored, bool firstColored, bool anyHasColor) {
    out.sourceKind = anyColored ? cb::TipSourceKind::Image : cb::TipSourceKind::Mask;
    out.defaultApplication =
        firstColored ? cb::TipApplication::LightnessMap : cb::TipApplication::AlphaMask;
    out.hasColorAndTransparency = anyHasColor;
}

// ------------------------------------------------------------------------------------------------
// The GIH parasite (§3.6.2). Space-separated key:value tokens; four keys read, the rest ignored.

void parseParasite(std::string_view parasite, cb::HoseParams& hose) {
    using cb::detail::parseLongLong;
    hose.dim = 0; // absent stays 0, exactly as the producer's field does
    int ncells = 0;

    std::size_t pos = 0;
    while (pos < parasite.size()) {
        const std::size_t end = parasite.find(' ', pos);
        const std::string_view token =
            parasite.substr(pos, end == std::string_view::npos ? std::string_view::npos
                                                               : end - pos);
        pos = end == std::string_view::npos ? parasite.size() : end + 1;
        const std::size_t colon = token.find(':');
        if (colon == std::string_view::npos || colon == 0 || colon + 1 >= token.size())
            continue; // not a key:value pair (an empty side is skipped upstream too)
        const std::string_view key = token.substr(0, colon);
        const std::string_view value = token.substr(colon + 1);

        long long n = 0;
        if (key == "dim") {
            // Garbage or out-of-range means 1; only a genuinely absent key leaves 0.
            hose.dim =
                (parseLongLong(value, n) && n >= 1 && n <= cb::kMaxHoseDim) ? static_cast<int>(n)
                                                                            : 1;
        } else if (key == "ncells") {
            ncells = parseLongLong(value, n) && n >= 1
                         ? static_cast<int>(std::min<long long>(n, 1 << 30))
                         : 1;
        } else if (key.substr(0, 4) == "rank") {
            long long idx = 0;
            // The producer accepts index == dim (its own array is one too small for that at
            // dim = 4, which is undefined behaviour there; the bound below keeps the faithful
            // cases and drops only the broken one).
            if (parseLongLong(key.substr(4), idx) && idx >= 0 && idx <= hose.dim &&
                idx < cb::kMaxHoseDim && parseLongLong(value, n)) {
                hose.rank[static_cast<std::size_t>(idx)] =
                    static_cast<int>(std::clamp<long long>(n, 0, 1 << 30));
            }
        } else if (key.substr(0, 3) == "sel") {
            long long idx = 0;
            if (parseLongLong(key.substr(3), idx) && idx >= 0 && idx < hose.dim &&
                idx < cb::kMaxHoseDim) {
                hose.selection[static_cast<std::size_t>(idx)] =
                    cb::frameSelectionFromName(std::string(value));
            }
        }
        // cellwidth/cellheight/step/cols/rows/placement: written by GIMP, read by nobody.
    }

    hose.declaredCells = ncells;

    // The producer's single sanitize rule: a zero rank under incremental/angular selection would
    // divide by zero, so it degrades to constant. Nothing else is sanitized -- A_bamboo-leaves'
    // rank0:5 over ncells:3 stays, and stays painting only cell 0 (§3.6.2).
    for (int i = 0; i < hose.dim; ++i) {
        if (hose.rank[static_cast<std::size_t>(i)] == 0 &&
            (hose.selection[static_cast<std::size_t>(i)] == cb::FrameSelection::Incremental ||
             hose.selection[static_cast<std::size_t>(i)] == cb::FrameSelection::Angular)) {
            hose.selection[static_cast<std::size_t>(i)] = cb::FrameSelection::Constant;
        }
    }
}

// One text line ending at '\n', or nullopt when none is in range. The cap bounds a hostile
// no-newline file; real name/parameter lines are well under it.
[[nodiscard]] std::optional<std::string_view> readLine(const std::uint8_t* data, std::size_t size,
                                                       std::size_t& pos) {
    constexpr std::size_t kMaxLine = 4096;
    const std::size_t limit = std::min(size, pos + kMaxLine);
    for (std::size_t i = pos; i < limit; ++i) {
        if (data[i] == '\n') {
            const std::string_view line(reinterpret_cast<const char*>(data + pos), i - pos);
            pos = i + 1;
            return line;
        }
    }
    return std::nullopt;
}

} // namespace

// ------------------------------------------------------------------------------------------------

std::optional<TipFile> readGbr(const std::uint8_t* data, std::size_t size, std::string* error) {
    GbrCell cell = readGbrCell(data, size, 0);
    if (cell.status != CellStatus::Ok) {
        fail(error, "not a readable GBR (bad header, unsupported depth, or truncated data)");
        return std::nullopt;
    }
    TipFile out;
    out.name = cell.name;
    out.spacing = cell.spacing;
    out.frames.push_back(std::move(cell.frame));
    applyCellVerdicts(out, cell.colored, cell.colored, cell.hasColor);
    return out;
}

std::optional<TipFile> readGih(const std::uint8_t* data, std::size_t size, std::string* error) {
    if (data == nullptr || size == 0) {
        fail(error, "empty file");
        return std::nullopt;
    }
    std::size_t pos = 0;
    const std::optional<std::string_view> nameLine = readLine(data, size, pos);
    const std::optional<std::string_view> paramLine = readLine(data, size, pos);
    if (!nameLine || !paramLine) {
        fail(error, "not a GIH: missing header lines");
        return std::nullopt;
    }

    // "<count> <parasite>": the leading integer drives the read loop; the parasite's `ncells`
    // drives the selection arithmetic. They are two different claims (§3.6.2).
    const std::size_t space = paramLine->find(' ');
    long long claimed = 0;
    if (!cb::detail::parseLongLong(paramLine->substr(0, space), claimed) || claimed < 1) {
        fail(error, "not a GIH: no brush count");
        return std::nullopt;
    }
    claimed = std::min<long long>(claimed, cb::kMaxTipFrames);

    TipFile out;
    out.name = std::string(*nameLine);
    parseParasite(space == std::string_view::npos ? std::string_view{}
                                                  : paramLine->substr(space + 1),
                  out.hose);
    out.declaredCells = out.hose.declaredCells;

    bool anyColored = false, firstColored = false, anyHasColor = false;
    for (long long i = 0; i < claimed; ++i) {
        GbrCell cell = readGbrCell(data, size, pos);
        if (cell.status == CellStatus::Stop)
            break; // the next cell does not fit: the count was a claim
        pos = cell.next;
        if (cell.status == CellStatus::Skip)
            continue;
        if (out.frames.empty())
            firstColored = cell.colored;
        anyColored = anyColored || cell.colored;
        anyHasColor = anyHasColor || cell.hasColor;
        out.spacing = cell.spacing; // the LAST loaded cell's spacing wins
        out.frames.push_back(std::move(cell.frame));
    }
    out.droppedFrames = static_cast<int>(claimed) - static_cast<int>(out.frames.size());

    if (out.frames.empty()) {
        fail(error, "GIH contains no readable cells");
        return std::nullopt;
    }
    applyCellVerdicts(out, anyColored, firstColored, anyHasColor);
    return out;
}

std::optional<TipFile> readPngTip(const std::uint8_t* data, std::size_t size, std::string* error) {
    const std::vector<std::uint8_t> buf(data, data + size);
    const std::optional<common::Image> image = mosaic::io::detail::decodePng(buf, error);
    if (!image)
        return std::nullopt;
    const std::uint64_t pixels = std::uint64_t(image->width) * image->height;
    if (pixels == 0 || pixels > cb::kMaxTipPixels) {
        fail(error, "PNG tip dimensions out of range");
        return std::nullopt;
    }

    TipFile out;
    // The producer honours two text chunks; a fraction, unlike GBR's percent.
    if (const std::optional<PngTextScan> scan = scanPngText(data, size)) {
        if (const PngText* s = scan->find("brush_spacing")) {
            double v = 0.0;
            if (cb::detail::parseDouble(s->text, v))
                out.spacing = v;
        }
        if (const PngText* n = scan->find("brush_name"))
            out.name = n->text;
    }

    // Content tests, not header tests (§3.5): 12 of the default set's 17 png tips are stored as
    // RGB/RGBA/indexed and are grey in content.
    bool hasAlpha = false, allGray = true;
    for (std::size_t i = 0; i < pixels; ++i) {
        const std::uint8_t* p = image->rgba.data() + i * 4;
        hasAlpha = hasAlpha || p[3] != 255;
        allGray = allGray && p[0] == p[1] && p[1] == p[2];
        if (hasAlpha && !allGray)
            break;
    }

    cb::TipFrame frame;
    frame.width = image->width;
    frame.height = image->height;
    frame.rgba = image->rgba;

    if (allGray && !hasAlpha) {
        // A plain mask. (The producer composites it over white here; with every alpha at 255
        // that is the identity, so the pixels are already the tip image.)
        out.sourceKind = cb::TipSourceKind::Mask;
        out.defaultApplication = cb::TipApplication::AlphaMask;
        out.hasColorAndTransparency = false;
    } else {
        out.sourceKind = cb::TipSourceKind::Image;
        out.defaultApplication =
            allGray ? cb::TipApplication::AlphaMask : cb::TipApplication::LightnessMap;
        out.hasColorAndTransparency = !allGray; // the verdict is has-COLOUR, the name aside
    }
    out.frames.push_back(std::move(frame));
    return out;
}

} // namespace mosaic::io::brush
