#include "core/brush/bitmap_tip.hpp"

#include <algorithm>
#include <cmath>

namespace mosaic::core::brush {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

// Euclidean remainder: the result is in [0, m) for any sign of `v`. `%` is not, and a hose whose
// dimension index went negative would index backwards out of its own frame vector.
[[nodiscard]] int positiveMod(long long v, int m) noexcept {
    if (m <= 0)
        return 0;
    long long r = v % m;
    if (r < 0)
        r += m;
    return static_cast<int>(r);
}

// The cell strides of docs/brushes.md §3.6.2. Derived by INTEGER DIVISION of the file's declared
// `ncells`, never by multiplying ranks -- `A_bamboo-leaves.gih` declares 3 cells with rank 5, so its
// stride is 3/5 = 0 and it paints its first cell forever. That is the file's behaviour, reproduced.
[[nodiscard]] std::array<int, kMaxHoseDim> cellStrides(const HoseParams& p, int dim) noexcept {
    std::array<int, kMaxHoseDim> stride{};
    int prev = std::max(p.declaredCells, 0);
    for (int i = 0; i < dim; ++i) {
        prev = (p.rank[i] != 0) ? prev / p.rank[i] : prev;
        stride[i] = prev;
    }
    return stride;
}

// The stateful half of the selection: run once per dab, before the index is read.
[[nodiscard]] int advanceIndex(FrameSelection mode, int index, int rank, int seqNo,
                               StrokeState& state) noexcept {
    if (rank < 1)
        return 0; // a zero rank cannot be cycled; the format degrades such a dimension to Constant
    switch (mode) {
    case FrameSelection::Incremental:
        // Keyed off the dab counter, not off `index + 1`. The first dab of a stroke is cell 0.
        return positiveMod(seqNo, rank);
    case FrameSelection::Random: {
        const double u = state.nextRandom(); // [0,1)
        return std::clamp(static_cast<int>(u * rank), 0, rank - 1);
    }
    case FrameSelection::Constant:
    case FrameSelection::Angular:
    case FrameSelection::Velocity:
    case FrameSelection::Pressure:
    case FrameSelection::TiltX:
    case FrameSelection::TiltY:
        return index; // recomputed from the sample below, or never changes
    }
    return index;
}

// The stateless half: modes that read the current sample rather than a counter.
[[nodiscard]] int sampleIndex(FrameSelection mode, int index, int rank, StrokeState& state,
                              const StrokeInput& in) noexcept {
    if (rank < 1)
        return 0;
    // Three of these modes -- xtilt, ytilt and velocity -- index ONE PAST the last cell at full
    // scale, and the caller's final modulo is what folds them home. That is not a latent overflow to
    // be clamped away: clamping changes which cell paints at the top of the range (`rank - 1` instead
    // of a wrap to 0), and the format's arithmetic is what a hose was authored against. `pressure`,
    // by contrast, scales by `rank - 1` and stays in range -- the asymmetry is real (docs §3.6.2).
    const auto tiltIndex = [rank](double degrees) noexcept {
        const double t = std::clamp(degrees / kMaxTiltDegrees, -1.0, 1.0);
        return static_cast<int>(std::lround(t / 2.0 * rank)) + rank / 2;
    };
    switch (mode) {
    case FrameSelection::Constant:
    case FrameSelection::Incremental:
    case FrameSelection::Random:
        return index;
    case FrameSelection::Pressure:
        return static_cast<int>(detail::clamp01(in.pressure) * (rank - 1) + 0.5);
    case FrameSelection::Angular: {
        // The three-eighths-turn offset is the format's, for compatibility with the tool that wrote
        // these files. It is not a half-bin centring.
        const double a =
            detail::wrapValue(state.drawingAngle() + kPi / 2.0 + kPi / 4.0, 0.0, kTwoPi);
        return static_cast<int>(a / kTwoPi * rank);
    }
    case FrameSelection::Velocity:
        // ⚠ Approximated, and the only mode here that is. The reference maps a raw speed through
        // `log(v+1)` capped at 3; our `speed()` is the documented time-constant EMA of
        // docs/tablet.md, normalized against `SpeedParams::maxSpeed`. Same monotone shape, different
        // calibration. No shipped tip selects on velocity, so nothing in the default set observes it.
        //
        // The scale is `(rank - 1) + 0.5`, i.e. `rank - 0.5` -- NOT `rank - 1` with a rounding term,
        // which is what the neighbouring `pressure` line does. Full speed therefore rounds to `rank`
        // and wraps to cell 0. That reads like a slip, but it is the arithmetic these hoses were
        // authored against, and the wrap is observable; see the note above `tiltIndex`.
        return static_cast<int>(std::lround(detail::clamp01(state.speed()) * ((rank - 1) + 0.5)));
    case FrameSelection::TiltX:
        return tiltIndex(in.xTilt);
    case FrameSelection::TiltY:
        return tiltIndex(in.yTilt);
    }
    return index;
}

} // namespace

bool TipAdjustments::neutral() const noexcept {
    // The reference's own skip test. A midpoint within a tenth of 127 counts as centred.
    return !autoMidPoint && std::abs(midPoint - 127.0) <= 0.1 && brightness == 0.0 &&
           contrast == 0.0;
}

std::string_view frameSelectionName(FrameSelection s) noexcept {
    switch (s) {
    case FrameSelection::Constant:
        return "constant";
    case FrameSelection::Incremental:
        return "incremental";
    case FrameSelection::Angular:
        return "angular";
    case FrameSelection::Velocity:
        return "velocity";
    case FrameSelection::Random:
        return "random";
    case FrameSelection::Pressure:
        return "pressure";
    case FrameSelection::TiltX:
        return "xtilt";
    case FrameSelection::TiltY:
        return "ytilt";
    }
    return "constant";
}

FrameSelection frameSelectionFromName(std::string_view name) noexcept {
    if (name == "incremental")
        return FrameSelection::Incremental;
    if (name == "angular")
        return FrameSelection::Angular;
    if (name == "velocity")
        return FrameSelection::Velocity;
    if (name == "random")
        return FrameSelection::Random;
    if (name == "pressure")
        return FrameSelection::Pressure;
    if (name == "xtilt")
        return FrameSelection::TiltX;
    if (name == "ytilt")
        return FrameSelection::TiltY;
    // Including the literal "constant", which the format never matches explicitly.
    return FrameSelection::Constant;
}

int HoseState::selectFrame(const HoseParams& params, int frameCount, StrokeState& state,
                           const StrokeInput& sample) noexcept {
    if (frameCount <= 0)
        return 0;
    const int dim = std::clamp(params.dim, 0, kMaxHoseDim);
    const std::array<int, kMaxHoseDim> stride = cellStrides(params, dim);
    const int seqNo = std::max(state.dabIndex(), 0);

    long long flat = 0;
    for (int i = 0; i < dim; ++i) {
        m_index[i] = advanceIndex(params.selection[i], m_index[i], params.rank[i], seqNo, state);
        const int idx = sampleIndex(params.selection[i], m_index[i], params.rank[i], state, sample);
        flat += static_cast<long long>(stride[i]) * idx;
    }
    // `dim == 0` (an absent or unparseable parasite) leaves `flat` at 0: a static, single-cell tip.
    return positiveMod(flat, frameCount);
}

namespace detail {

std::array<std::uint8_t, 256> adjustmentTable(const TipAdjustments& adj) noexcept {
    std::array<std::uint8_t, 256> lut{};
    // The hinge. `midX` is pulled off the ends: the reference divides by `midX` and by `255 - midX`,
    // and a preset may legally ask for either to be zero.
    const double midX = std::clamp(adj.midPoint, 1.0, 254.0);
    const double brightness = std::clamp(adj.brightness, -1.0, 1.0);
    const double contrast = std::clamp(adj.contrast, -1.0, 1.0);
    const double midY = brightness > 0.0 ? 127.0 + 128.0 * brightness : 127.0 + 127.0 * brightness;

    double loA = 0.0;
    double hiA = 0.0;
    double loB = 0.0;
    double hiB = 255.0;
    if (contrast != 1.0) {
        if (contrast > 0.0) {
            loA = midY / (1.0 - contrast) / midX;
            hiA = (255.0 - midY) / (1.0 - contrast) / (255.0 - midX);
        } else {
            loA = midY * (1.0 + contrast) / midX;
            hiA = (255.0 - midY) * (1.0 + contrast) / (255.0 - midX);
        }
        loB = midY - midX * loA;
        hiB = midY - midX * hiA;
    }

    for (int v = 0; v < 256; ++v) {
        const double x = static_cast<double>(v);
        const double y = (x >= midX) ? hiA * x + hiB : loA * x + loB;
        // `contrast` near 1 makes the slopes enormous but finite; a NaN can only arrive from a NaN
        // parameter, and clamp01-style saturation is the house rule for that.
        lut[static_cast<std::size_t>(v)] =
            std::isfinite(y) ? static_cast<std::uint8_t>(std::clamp(std::lround(y), 0L, 255L)) : 0;
    }
    return lut;
}

} // namespace detail

namespace {

// Coverage from a tip-image pixel, per application (docs/brushes.md §3.5, §3.6.1). Only `AlphaMask`
// inverts the grey; the other three take the tip's alpha and leave the grey to the colour path.
[[nodiscard]] std::uint8_t coverageOf(TipApplication app, std::uint8_t grey,
                                      std::uint8_t alpha) noexcept {
    if (app == TipApplication::AlphaMask)
        return static_cast<std::uint8_t>(((255 - grey) * alpha + 127) / 255);
    return alpha;
}

[[nodiscard]] BitmapTip::MipLevel downsample(const BitmapTip::MipLevel& src) noexcept {
    BitmapTip::MipLevel dst;
    dst.width = std::max(1u, src.width / 2);
    dst.height = std::max(1u, src.height / 2);
    dst.coverage.resize(static_cast<std::size_t>(dst.width) * dst.height);
    for (std::uint32_t y = 0; y < dst.height; ++y) {
        for (std::uint32_t x = 0; x < dst.width; ++x) {
            // A 2x2 box, clamped -- an odd source dimension makes the last row/column sample itself.
            const std::uint32_t x0 = std::min(2 * x, src.width - 1);
            const std::uint32_t x1 = std::min(2 * x + 1, src.width - 1);
            const std::uint32_t y0 = std::min(2 * y, src.height - 1);
            const std::uint32_t y1 = std::min(2 * y + 1, src.height - 1);
            const unsigned sum = src.coverage[static_cast<std::size_t>(y0) * src.width + x0] +
                                 src.coverage[static_cast<std::size_t>(y0) * src.width + x1] +
                                 src.coverage[static_cast<std::size_t>(y1) * src.width + x0] +
                                 src.coverage[static_cast<std::size_t>(y1) * src.width + x1];
            dst.coverage[static_cast<std::size_t>(y) * dst.width + x] =
                static_cast<std::uint8_t>((sum + 2) / 4);
        }
    }
    return dst;
}

} // namespace

BitmapTip::BitmapTip(std::vector<TipFrame> frames, TipApplication application,
                     TipSourceKind sourceKind, TipAdjustments adjustments, HoseParams hose)
    : m_application(application), m_hose(hose) {
    // The adjustments run on an Image-kind source in every application but ImageStamp -- and even
    // when neutral they desaturate, which is why the table is built for a colour source regardless.
    const bool adjust =
        sourceKind == TipSourceKind::Image && application != TipApplication::ImageStamp;
    const bool useLut = adjust && !adjustments.neutral();
    // With `autoMidPoint` the hinge is the frame's OWN average grey, so the table cannot be shared
    // across the cells of a hose -- each is a different image. Without it, one table serves them all.
    const bool perFrameLut = useLut && adjustments.autoMidPoint;
    std::array<std::uint8_t, 256> sharedLut{};
    if (useLut && !perFrameLut)
        sharedLut = detail::adjustmentTable(adjustments);

    if (frames.size() > static_cast<std::size_t>(kMaxTipFrames)) {
        m_dropped += static_cast<int>(frames.size()) - kMaxTipFrames;
        frames.resize(kMaxTipFrames);
    }

    for (const TipFrame& f : frames) {
        const std::uint64_t px = static_cast<std::uint64_t>(f.width) * f.height;
        if (px == 0 || px > kMaxTipPixels ||
            f.rgba.size() != static_cast<std::size_t>(px) * 4) {
            ++m_dropped;
            continue;
        }
        const auto n = static_cast<std::size_t>(px);

        // Desaturate first: the transfer curve, and the average that may define its hinge, both act
        // on grey. This is also the step that flattens a colour raster for every application but
        // ImageStamp, adjustments or no adjustments.
        std::vector<std::uint8_t> grey(n);
        for (std::size_t i = 0; i < n; ++i)
            grey[i] = detail::luma(f.rgba[i * 4], f.rgba[i * 4 + 1], f.rgba[i * 4 + 2]);

        std::array<std::uint8_t, 256> lut = sharedLut;
        if (perFrameLut) {
            std::uint64_t sum = 0;
            for (std::uint8_t g : grey)
                sum += g;
            TipAdjustments perFrame = adjustments;
            // The reference SAMPLES the image to estimate this; we average all of it, which is the
            // same quantity computed exactly. Cheap -- it is one pass, once, at tip construction.
            perFrame.midPoint = static_cast<double>(sum) / static_cast<double>(n);
            lut = detail::adjustmentTable(perFrame);
        }

        MipLevel base;
        base.width = f.width;
        base.height = f.height;
        base.coverage.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint8_t g = useLut ? lut[grey[i]] : grey[i];
            base.coverage[i] = coverageOf(application, g, f.rgba[i * 4 + 3]);
        }

        std::vector<MipLevel> chain;
        chain.push_back(std::move(base));
        while (chain.back().width > 1 || chain.back().height > 1)
            chain.push_back(downsample(chain.back()));
        m_frames.push_back(std::move(chain));
    }

    // A hose that lost cells still has to index the ones it kept. `declaredCells` is deliberately
    // left as the file wrote it -- the stride depends on the claim, the wrap on the truth.
    m_hose.dim = std::clamp(m_hose.dim, 0, kMaxHoseDim);
}

std::uint32_t BitmapTip::frameWidth(int frame) const noexcept {
    if (frame < 0 || frame >= frameCount())
        return 0;
    return m_frames[static_cast<std::size_t>(frame)][0].width;
}

std::uint32_t BitmapTip::frameHeight(int frame) const noexcept {
    if (frame < 0 || frame >= frameCount())
        return 0;
    return m_frames[static_cast<std::size_t>(frame)][0].height;
}

double BitmapTip::baseSize(int frame) const noexcept {
    return static_cast<double>(std::max(frameWidth(frame), frameHeight(frame)));
}

int BitmapTip::levelCount(int frame) const noexcept {
    if (frame < 0 || frame >= frameCount())
        return 0;
    return static_cast<int>(m_frames[static_cast<std::size_t>(frame)].size());
}

const BitmapTip::MipLevel& BitmapTip::level(int frame, int lvl) const noexcept {
    // `frame` is bounds-checked like every sibling accessor, not merely `lvl`. An out-of-range frame
    // answers with an empty level: a caller that walked `declaredCells` rather than `frameCount()` --
    // which is exactly what a hose whose `ncells` lies invites -- would otherwise read out of bounds.
    static const MipLevel kEmpty{};
    if (frame < 0 || frame >= frameCount())
        return kEmpty;
    const auto& chain = m_frames[static_cast<std::size_t>(frame)];
    return chain[static_cast<std::size_t>(std::clamp(lvl, 0, static_cast<int>(chain.size()) - 1))];
}

int BitmapTip::pickLevel(int frame, double targetW, double targetH) const noexcept {
    const int levels = levelCount(frame);
    if (levels <= 1)
        return 0;
    int best = 0;
    for (int l = 1; l < levels; ++l) {
        const MipLevel& m = level(frame, l);
        if (static_cast<double>(m.width) >= targetW && static_cast<double>(m.height) >= targetH)
            best = l;
        else
            break; // levels shrink monotonically, so the first that is too small ends the search
    }
    return best;
}

DabShape bitmapDabShape(const BitmapTip& tip, int frame, double diameter, double ratio,
                        double angleRad, bool mirrorH, bool mirrorV) noexcept {
    DabShape s;
    s.angleRad = angleRad;
    s.mirrorH = mirrorH;
    s.mirrorV = mirrorV;
    const double base = tip.baseSize(frame);
    if (!(base > 0.0) || !std::isfinite(diameter) || !std::isfinite(ratio)) {
        s.width = 0.0;
        s.height = 0.0;
        return s;
    }
    const double scale = diameter / base;
    s.width = tip.frameWidth(frame) * scale;
    s.height = tip.frameHeight(frame) * scale * ratio;
    return s;
}

DabMask renderDabMask(const BitmapTip& tip, int frame, const DabShape& shape, double subX,
                      double subY) {
    DabMask mask;
    if (frame < 0 || frame >= tip.frameCount())
        return mask;

    subX = detail::normalizePhase(subX);
    subY = detail::normalizePhase(subY);

    const DabExtent ext = dabExtent(shape);
    if (ext.empty())
        return mask;

    mask.width = detail::maskSpan(ext.width, subX);
    mask.height = detail::maskSpan(ext.height, subY);
    if (mask.empty()) {
        mask = DabMask{};
        return mask;
    }
    mask.coverage.resize(static_cast<std::size_t>(mask.width) * mask.height, 0);

    const BitmapTip::MipLevel& src =
        tip.level(frame, tip.pickLevel(frame, shape.width, shape.height));
    const auto lw = static_cast<int>(src.width);
    const auto lh = static_cast<int>(src.height);

    // Everything outside the source raster is zero coverage, so a bilinear tap that straddles the
    // edge fades out rather than smearing the border pixel outward.
    const auto texel = [&src, lw, lh](int x, int y) noexcept -> double {
        if (x < 0 || y < 0 || x >= lw || y >= lh)
            return 0.0;
        return src.coverage[static_cast<std::size_t>(y) * src.width + static_cast<std::size_t>(x)];
    };

    const detail::TipFrameMap toTip = detail::tipFrameMap(shape, ext, subX, subY);
    const double invW = 1.0 / shape.width;
    const double invH = 1.0 / shape.height;
    std::size_t i = 0;
    for (std::uint32_t py = 0; py < mask.height; ++py) {
        for (std::uint32_t px = 0; px < mask.width; ++px, ++i) {
            double tx = 0.0;
            double ty = 0.0;
            toTip(px, py, tx, ty);
            // Tip frame -> normalized [0,1] -> source pixel centres.
            const double sx = (tx * invW + 0.5) * lw - 0.5;
            const double sy = (ty * invH + 0.5) * lh - 0.5;
            const double fx = std::floor(sx);
            const double fy = std::floor(sy);
            if (fx < -1.0 || fy < -1.0 || fx >= lw || fy >= lh)
                continue; // wholly outside: already 0
            const int x0 = static_cast<int>(fx);
            const int y0 = static_cast<int>(fy);
            const double ax = sx - fx;
            const double ay = sy - fy;
            const double top = texel(x0, y0) * (1.0 - ax) + texel(x0 + 1, y0) * ax;
            const double bot = texel(x0, y0 + 1) * (1.0 - ax) + texel(x0 + 1, y0 + 1) * ax;
            const double v = top * (1.0 - ay) + bot * ay;
            mask.coverage[i] = static_cast<std::uint8_t>(std::clamp(std::lround(v), 0L, 255L));
        }
    }
    return mask;
}

} // namespace mosaic::core::brush
