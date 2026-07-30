# `src/core/inpaint/` — inpainting engine + backends

Namespace `mosaic::core::inpaint`. One engine dispatches to pluggable backends; the default
backend lives entirely inside its own directory, and the engine, the interface and the other
backends are independent of it. See `docs/inpainting-research.md` for the algorithm, the
standing implementation constraints (§5) and the §10 performance-optimization notes.

## Layout

```
inpaint/
  inpaint_backend.hpp        SHARED — IInpaintBackend interface, InpaintRequest/Result,
                             Params (per-backend sections), StageTiming, ScopedStage.
  inpaint_engine.hpp/.cpp    SHARED — backend registry + dispatch + makeDefaultEngine().
  backends/
    he_sun/                  DEFAULT backend: He & Sun offset-statistics graph completion.
      offset_stats_backend   IInpaintBackend impl ("offset-stats"); orchestrates the stages.
      offset_statistics      stage 1: dominant patch offsets (decimated KD-tree NNF).
      working_region         crop+downsample the hole neighbourhood for stage 1.
      graph_cut              generic α-expansion + from-scratch Dinic max-flow.
      graph_completion       stage 2: MRF label by α-expansion → composite → Poisson seam blend.
    pde/                     PdeBackend ("pde"): cleared diffusion fallback (Telea / Bertalmío NS).
    script/                  ScriptBackend ("lua:*"): S40 shim to a Lua-registered provider.
```

Shared files (engine + interface) stay at the top level; everything specific to one
method lives under `backends/<method>/`.

## Adding a backend

1. Create `backends/<name>/<name>_backend.{hpp,cpp}` implementing `IInpaintBackend`
   (`id()`, `name()`, `run()`). Keep it pure compute — no UI, no document mutation.
2. Add any method-specific files beside it in the same directory; only put something
   in the top level if a second backend actually shares it.
3. Add the `.cpp`s to `src/core/CMakeLists.txt`, register the backend in
   `makeDefaultEngine()` (`inpaint_engine.cpp`), and surface backend-specific knobs on
   `Params` (the Inpainting settings tab reads these).
4. Read the standing implementation constraints (`docs/inpainting-research.md` §5) *before*
   implementing an algorithm feature — several are load-bearing and are pinned by tests — and
   cite the technique's published lineage in each source file's header.
