#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "common/geometry.hpp"
#include "common/image.hpp"
#include "core/layer.hpp"    // core::WarpGrid / core::WarpKind -- the model this kernel reads
#include "render/render.hpp" // ResampleFilter

// The warp kernel (S35-b; docs/warp-tools.md): the pixels behind Mesh Warp and Perspective Warp.
// Pure, FLTK-free and GPU-free -- images in, images out -- so the whole deformation is unit-tested
// headlessly and the canvas owns nothing but the handles.
//
// ---- INVERSE MAPPING, ALWAYS ------------------------------------------------------------------
// Every path here walks the DESTINATION and asks "where did this pixel come from", never the source
// asking "where does this pixel go". A forward walk scatters: a magnification leaves the gaps
// between the landed pixels empty and a reduction piles several sources onto one destination, so a
// forward warp has to invent a hole-filling pass and still cannot low-pass. The inverse walk has
// neither problem -- it is O(output), it covers every output pixel exactly once, and it hands each
// one a source COORDINATE, which is precisely what a reconstruction kernel needs. That is the whole
// amateur/professional divide in an image warp, and it is why the two engines below look the way
// they do rather than being "draw the source quad through the mesh".
//
// The two engines, and why they are two:
//
//   * MESH -- the lattice defines a piecewise CATMULL-ROM surface (Catmull & Rom 1974), so the
//     deformation is C1 across patch boundaries instead of creasing at every control point. It is
//     evaluated ONCE for the whole surface into a fine vertex grid (dest position + source
//     position per vertex) and the resulting triangles are rasterised with the source coordinate
//     interpolated barycentrically. Evaluating per patch instead would give adjacent patches their
//     own copies of the shared boundary vertices, and two float paths that agree only to the last
//     bit draw a hairline seam down every patch edge.
//
//   * PERSPECTIVE -- a single 3x3 HOMOGRAPHY solved from the 4 source corners to the 4 destination
//     corners, then inverse-mapped per destination pixel with the proper per-pixel w divide. A
//     homography is deliberately NOT triangulated: a projective map is not affine, so splitting the
//     quad into two triangles replaces it with two different affines that agree only along the
//     shared diagonal -- the classic visible-diagonal-crease implementation. A near-singular or
//     non-convex quad is REFUSED (`ok == false`) rather than rendered as garbage.
//
// Sampling is the compositor's own: render/resample.hpp's kernel bank (kernelRadius / kernelWeight
// / bilinearPremul), accumulated in PREMULTIPLIED alpha and un-premultiplied once, with the tap
// footprint widened by the local minification factor. Nothing new is invented here -- a second
// kernel bank in the tree would be a second thing to keep in step with shaders/composite_tile.comp.
namespace mosaic::render {

// How much of the quality budget one call may spend. The two are the SAME algorithm at different
// subdivision + kernel settings, not two code paths: Draft is what a live handle drag re-warps
// through every frame, Final is what the release (and the undoable command) lands. Mirrors the
// compositor's own liveDrag/commit split in chooseAutoFilter.
enum class WarpQuality : std::uint8_t { Draft, Final };

// Catmull-Rom evaluation steps per patch edge for each quality. A patch is subdivided into
// steps x steps cells, so the cost is quadratic in this -- which is exactly why the drag lane
// takes the smaller number.
[[nodiscard]] int warpPatchSteps(WarpQuality q) noexcept;

// The result of a warp: the deformed pixels plus where they sit.
struct WarpResult {
    common::Image px;   // the warped pixels, tight to the deformed content's own extent
    int offX = 0;       // px(0,0) lies at (offX, offY) in the INPUT's pixel space, so a caller that
    int offY = 0;       // replaces the layer's image post-translates its transform by (offX, offY)
                        // and not one pixel already on the canvas moves
    bool ok = false;    // false => nothing usable was produced (see the refusals per engine)
};

// The Catmull-Rom surface point at lattice parameter (u, v), u in [0, cols-1], v in [0, rows-1]
// (so integer parameters are the control points themselves and the surface passes through them).
// Outside the lattice the parameters are clamped. One ring of phantom control points is
// extrapolated as P[-1] = 2*P[0] - P[1], the natural C1 continuation, so an edge patch bends like
// its neighbours instead of flattening.
//
// A 2-point axis degenerates EXACTLY to linear interpolation (the phantom extrapolation cancels
// the quadratic and cubic terms), which is what makes a 2x2 mesh a plain bilinear quad -- and why
// Perspective needs its own engine rather than "a 2x2 mesh".
[[nodiscard]] common::Vec2 warpSurfacePoint(const core::WarpGrid& g, double u, double v);

// The grid's isolines as polylines in the grid's OWN space (layer-local px): one per lattice row
// then one per lattice column, each sampled at `stepsPerEdge` points per patch edge. This is what
// the on-canvas overlay draws, and it calls the same warpSurfacePoint the pixels do -- straight
// chords between control points would draw a grid that does not bend the way the image does.
//
// For a Perspective grid the useful lines are the 4 straight edges and the 2 diagonals; ask
// warpQuadLines for those instead (a projective interior is not the bilinear one this samples).
[[nodiscard]] std::vector<std::vector<common::Vec2>> warpIsolines(const core::WarpGrid& g,
                                                                  int stepsPerEdge);

// A Perspective grid's read: [0] the closed boundary (5 points, the first repeated), [1] and [2] the
// two diagonals. Empty when the grid is not a valid 2x2.
[[nodiscard]] std::vector<std::vector<common::Vec2>> warpQuadLines(const core::WarpGrid& g);

// A 2x2 grid's four points in the CYCLIC order TL, TR, BR, BL. The lattice is stored row-major, so
// its raw order is TL, TR, BL, BR; every quad consumer here (convexity, the homography solve, the
// diagonals, the point test) reads the cyclic one, so the swap lives in exactly this one place.
[[nodiscard]] std::array<common::Vec2, 4> warpQuadCorners(const core::WarpGrid& g);

// A 3x3 projective transform, row-major, with m[8] normalised to 1 by the solve.
struct Homography {
    std::array<double, 9> m{{1, 0, 0, 0, 1, 0, 0, 0, 1}};

    // p -> (m0 x + m1 y + m2, m3 x + m4 y + m5) / (m6 x + m7 y + m8). `outW` receives the
    // homogeneous divisor, which the caller needs: its SIGN flips across the map's horizon line,
    // and a pixel on the far side of that line has no honest pre-image at all.
    [[nodiscard]] common::Vec2 apply(common::Vec2 p, double* outW = nullptr) const;
    [[nodiscard]] std::optional<Homography> inverse() const;
};

// The homography carrying `from`'s four points onto `to`'s, in the cyclic order TL, TR, BR, BL.
// Solved as the 8x8 linear system in the eight free coefficients by Gaussian elimination with
// partial pivoting -- the textbook 4-point solve (Hartley & Zisserman's modern exposition of a
// 19th-century construction). nullopt when the system is singular to within its pivot tolerance,
// which is what a collapsed or collinear quad produces.
[[nodiscard]] std::optional<Homography> solveHomography(const std::array<common::Vec2, 4>& from,
                                                        const std::array<common::Vec2, 4>& to);

// Is `q` (cyclic TL, TR, BR, BL) a strictly convex, non-degenerate quadrilateral? A projective map
// onto a non-convex or self-crossing quad folds the plane back over itself, so the tool refuses it
// rather than producing a picture nobody asked for.
[[nodiscard]] bool convexQuad(const std::array<common::Vec2, 4>& q);

// Is `p` inside the convex quad `q` (cyclic order), allowing `slack` px of outward tolerance? The
// perspective engine's source-side clip, and the gesture's "did the press land in the quad" test.
[[nodiscard]] bool pointInQuad(const std::array<common::Vec2, 4>& q, common::Vec2 p,
                               double slack = 0.0);

// ---- The entry points -------------------------------------------------------------------------
//
// TWO grids, not one. `from` says where each lattice parameter sits in `src`'s pixel space and `to`
// says where it must end up: the deformation is "the pixel under from(u,v) moves to to(u,v)". The
// single-grid overload below fills in the undeformed lattice, which is the first warp of a layer.
//
// The pair is what makes RE-EDITING correct. A layer that has been warped once carries its grid
// (core::Layer::warp()), and its pixels are already deformed -- so warping those pixels by the
// stored grid AGAIN would apply the displacement twice. Passing the stored grid as `from` and the
// edited grid as `to` applies exactly the difference, which is the only reading under which
// re-entering the tool and nudging one handle does what it looks like it does.
//
// `from` and `to` must agree on kind, cols and rows; they may differ in every point.
[[nodiscard]] WarpResult warpImage(const common::Image& src, const core::WarpGrid& from,
                                   const core::WarpGrid& to, ResampleFilter filter,
                                   WarpQuality quality = WarpQuality::Final);

// The first warp of an undeformed image: `to`'s own lattice positions are the source.
[[nodiscard]] WarpResult warpImage(const common::Image& src, const core::WarpGrid& to,
                                   ResampleFilter filter,
                                   WarpQuality quality = WarpQuality::Final);

} // namespace mosaic::render
