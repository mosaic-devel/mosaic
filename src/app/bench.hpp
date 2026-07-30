#pragma once

#include <cstdint>
#include <string_view>

// The `--bench` measurement harness (S60, docs/s60-performance-plan.md §8.2). Headless,
// deterministic, prints a table -- "an optimisation arc without a measurement harness is a rewrite
// with extra steps". Every scenario builds its own synthetic document IN CODE from a fixed seed
// (nothing on disk, no UI, no display, no GPU), drives the exact code path the interactive app
// drives, and reports min/median/mean/max over N samples plus the shape of what it measured.
//
// Rules this harness lives by, so that a number is still readable a month later:
//   * the numeric output is pinned to LC_NUMERIC="C" (a decimal comma silently breaks a parser);
//   * every case prints its canvas size and layer count beside its timings;
//   * the CPU backend is forced everywhere -- these are the paths S60 is optimising, and the GPU
//     lanes need a device the bench machine may not have.
namespace mosaic::app {

// List the available scenarios (token + one line each) on stdout.
void printBenchScenarios();

// Run `scenario` and print its table. `iterations` = 0 uses the scenario's own default.
// Returns a process exit code: 0 on success, 2 for an unknown/empty scenario.
[[nodiscard]] int runBench(std::string_view scenario, std::uint32_t iterations);

}  // namespace mosaic::app
