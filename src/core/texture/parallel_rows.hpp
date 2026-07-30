#pragma once

#include <cstddef>
#include <utility>

#include "common/thread_pool.hpp"

// Band-parallel rows for the texture generators (extracted from texture_render.cpp when the sky
// grew its own TU in S55-b; the compositor's file-local pattern). Bands write disjoint rows and
// every sample is a pure function of (seed, x, y), so any thread count is bit-identical to
// serial -- the §8.3 determinism contract and the golden tests survive parallelism.
//
// The split is the shared one (16 rows minimum per band, at most one band per hardware thread,
// never more than 32) and is unchanged since S55-b; S60-b only moved the bands off a per-call
// thread spawn onto the process-wide pool.
namespace mosaic::core::texture {

template <typename Fn>
void parallelRows(std::size_t count, Fn&& fn) {
    common::parallelFor(count, 16, std::forward<Fn>(fn));
}

}  // namespace mosaic::core::texture
