# Vulkan rendering & FLTK windowing

How Mosaic gets Vulkan onto an FLTK window, and the platform quirks involved. This grows as
the renderer does; today it covers the bring-up done in **S2** (headless) and **S3** (the
windowed swapchain canvas).

## Two render contexts

| Context | File | Lifetime | Purpose |
|---|---|---|---|
| `render::VulkanContext` | `src/render/vulkan_context.cpp` | one-shot (recreated per call) | Headless offscreen + compute (S2). Backs the `--headless` op-runner and golden tests. No surface/swapchain. |
| `render::GpuCompositor` | `src/render/gpu_compositor.cpp` | persistent (per composite) | The GPU compute blend kernel for the layer compositor (S7-b): runs `composite_blend.comp` over `rgba32f` storage images, allocated via **VMA**. Built on a `VulkanContext`; offscreen (no surface). |
| `render::WindowRenderer` | `src/render/window_renderer.cpp` | persistent (window lifetime) | Surface-backed swapchain that presents to a window (S3). Since S8 it presents the document through a **compute pass** that samples it under the interactive view transform (pan/zoom/rotate); see "Presenting the canvas" below. The document + intermediate images are **VMA**-allocated. |

All target **Vulkan 1.2** and enable `VK_LAYER_KHRONOS_validation` + `VK_EXT_debug_utils`
in debug; validation messages are printed and errors counted. **VMA (Vulkan Memory Allocator)
landed in S7-b** for device memory (used by `GpuCompositor`; `VulkanContext`'s S2 manual
allocations may migrate to it later).

## Getting Vulkan onto an FLTK window

FLTK 1.4 has no `Fl_Vk_Window` (only `Fl_Gl_Window`), so we create the `VkSurfaceKHR`
ourselves from the window's native handle.

**The canvas is a child `Fl_Window` (a subwindow), not a widget.** The main window holds the
FLTK-drawn `Fl_Menu_Bar` plus a `ui::VulkanCanvas : Fl_Window`. Because a child `Fl_Window`
is a *real native subwindow* with its own handle/surface, the Vulkan swapchain owns that
surface outright and never fights the FLTK-drawn menu bar for the parent's surface. The
canvas overrides `draw()` (routes to Vulkan instead of FLTK drawing), `resize()` (flags a
swapchain rebuild) and `hide()` (tears the renderer down before FLTK destroys the native
window). The renderer is created lazily on the first frame, once the handle exists (i.e.
after `show()`).

Frames are driven by a ~60 Hz FLTK timeout (`Fl::repeat_timeout`) plus expose-driven
`draw()`. `--gui-frames N` renders N frames then quits — a display-dependent smoke test.

## Backend selection: native Wayland is the default (S59-a)

Arch's FLTK 1.4 is a **hybrid X11 + Wayland** build that picks a backend at runtime
(`FLTK_BACKEND`, else Wayland if `WAYLAND_DISPLAY` is set). `platform::nativeSurfaceHandle()`
detects the live backend (via `fl_wl_display()` vs `fl_x11_display()`) and returns the raw
handles; `WindowRenderer` builds either a `VK_KHR_xlib_surface` or `VK_KHR_wayland_surface`
from them.

**Default: native Wayland.** `platform::preferWaylandBackendIfUnset()` pins
`FLTK_BACKEND=wayland` on a Wayland session when the user hasn't chosen one; a **pure-Xorg**
session (no `WAYLAND_DISPLAY`) is left alone and still gets X11. A user-set `FLTK_BACKEND` always
wins, so **`FLTK_BACKEND=x11` is the escape hatch** back to XWayland and every X11 path in the
tree stays live behind it.

This flipped in **S59-a** (it used to pin `x11`). The reasons: the native canvas has been
validation-clean since S11-c; the X11 resize "black flash" (§12) does not happen there; the
file-picker portal gets a real `zxdg_exporter_v2` parent instead of an XWayland `x11:<xid>` token
KWin won't honour; and native Wayland is a **hard prerequisite for HDR output** (below). What else
the flip changes — KDE file dialogs, tablet input, the window icon, sub-window popovers, cursors —
is inventoried in **`docs/wayland.md`**, which is the reference for the native path.

## Native Wayland: a dedicated Vulkan subsurface

A plain FLTK Wayland top-level/subwindow renders through FLTK's own libdecor/Cairo path on its
`wl_surface`, and Mesa's Vulkan WSI uses **explicit sync** (`wp_linux_drm_syncobj_surface_v1`).
Attaching a swapchain to FLTK's *same* surface makes Mesa create a second syncobj surface for
it, which the protocol forbids — so `FLTK_BACKEND=wayland` used to initialize the device and
then abort with:

```
Fatal error no 4 in Wayland protocol: wp_linux_drm_syncobj_surface_v1
```

The fix (the same trick `Fl_Gl_Window` uses internally) is to give the GPU its **own**
`wl_surface` that nothing else touches: **`platform::WaylandSubsurface`** creates a
`wl_subsurface` stacked over the FLTK canvas surface, and the swapchain is built on *that*
child surface instead. Mechanics (`src/platform/wayland_subsurface.cpp`):

- FLTK exposes `fl_wl_display()`, `fl_wl_compositor()`, and a window's `fl_wl_surface()`, but
  **not** the `wl_subcompositor` needed for `get_subsurface`. We bind our own from the registry
  on a **private `wl_event_queue`** (so the roundtrip doesn't dispatch/​swallow FLTK's events),
  then hand the bound proxy back to the default queue. Binding a global twice is allowed.
- Create a child `wl_surface` via `fl_wl_compositor()`, make it a `wl_subsurface` of the canvas
  surface at `(0,0)`, **`set_desync`** (so it presents on its own Vulkan commits, not gated on a
  parent commit), and `set_buffer_scale` for HiDPI.
- A new subsurface's placement is parent-cached state, so it only maps once the parent surface
  is committed. FLTK isn't drawing this surface (Vulkan owns it), so we **commit the parent
  once** ourselves (no buffer attached, so FLTK's buffer is preserved).
- `mosaic_platform` therefore links **`libwayland-client`** directly (registry + subcompositor +
  subsurface requests). `render` still only needs the WSI *headers* — the surface entry points
  come from the Vulkan loader.

**Lifetime / teardown order is load-bearing.** The `VkSurfaceKHR` is built on the child
`wl_surface`, so it must be destroyed **before** the child surface, which in turn must go before
FLTK frees the parent. `VulkanCanvas` declares `m_subsurface` before `m_renderer` (so the
renderer dtor runs first) and explicitly resets `m_renderer` then `m_subsurface` in both `hide()`
and its destructor, all before `Fl_Window::hide()`.

**Verified** on AMD RX 6600 XT / RADV: `FLTK_BACKEND=wayland mosaic --gui-frames N` is
validation-clean (no abort), and a `spectacle` screenshot shows the placeholder document
correctly centered with its checkerboard border and blend-mode layers — identical to the X11
path. (`grim` can't capture here — the compositor lacks `wlr-screencopy`; use `spectacle -bnf`.)

**HDR caveat (important).** XWayland and Xorg are **SDR-only**, so HDR *output* (S43 —
presenting PQ/HLG/scRGB via `VK_EXT_swapchain_colorspace` / `VK_EXT_hdr_metadata`) cannot work
over X11. The native-Wayland canvas is therefore a **hard prerequisite for HDR display**, not a
nicety — which is part of why it is now the default (S59-a); a user who takes the
`FLTK_BACKEND=x11` escape hatch gives up HDR output with it. HDR *editing* — the float compositing
pipeline, EXR/JXL/AVIF I/O, and tone-mapping to an SDR window — is backend-independent and
unaffected.

## Swapchain & per-frame work

- **Format/present:** prefer `B8G8R8A8_UNORM` + sRGB-nonlinear, else the first offered;
  present mode `FIFO` (always available, vsync).
- **Present path (S8):** swapchain images use `COLOR_ATTACHMENT | TRANSFER_DST` usage. Before any
  document exists, the frame is a plain `vkCmdClearColorImage` to the canvas bg. Once a document is
  set, it is presented through the **canvas presenter** (below), not a fixed blit.
- **Sync (single frame in flight):** one `imageAvailable` semaphore + one `inFlight` fence,
  and a `renderFinished` semaphore **per swapchain image** (so present never waits on a
  semaphore that may still be in use). Simple and validation-clean; S7 can pipeline deeper.
- **Resize / out-of-date:** `VK_ERROR_OUT_OF_DATE_KHR` / `VK_SUBOPTIMAL_KHR` and `resize()`
  set a recreate flag; the swapchain is rebuilt (`vkDeviceWaitIdle` first) before the next
  frame. A zero-size (minimized) surface is a no-op frame.

## Presenting the canvas (S8 view transform)

The canvas viewport pans, zooms **and rotates**, so the document can't just be `vkCmdBlitImage`-ed
(a blit is axis-aligned scale only). Instead the document is presented by a **compute pass**:

- `ui::CanvasView` (pure, unit-tested) owns the view state and builds a `common::Affine2D` mapping
  document px → the canvas's *logical* screen px. Pan and zoom act in an unrotated frame; the whole
  view then rotates about the viewport centre (so "rotate" spins about the middle of the view).
- `WindowRenderer::setView(docToScreenLogical, contentScale)` is pushed each frame. The renderer
  forms the **physical** transform (`scaling(contentScale) * docToScreenLogical`) and inverts it,
  so `shaders/canvas_present.comp` can map each output pixel back to a document texel.
- The shader writes an intermediate **rgba8 storage image** (`m_viewImage`, sized to the swapchain
  extent), sampling the document (`SAMPLED`, bilinear, clamp-to-edge) where the inverse maps inside
  it, and the canvas bg colour elsewhere. That image is then **blitted 1:1** onto the swapchain
  (the blit maps rgba8 → the swapchain's bgra by component, so colours are correct).
- One persistent descriptor set (binding 0 = the view image, binding 1 = the document sampler,
  binding 2 = the selection mask sampler) is re-pointed only when those images are (re)created
  (resize / new document / selection size change). Single-frame-in-flight, so the set is never
  updated while in use.

This intermediate-image approach avoids needing swapchain `STORAGE` support, per-swapchain-image
descriptors, and a bgra swizzle in the shader.

**Rotation overlay (S8-b).** While the view is being rotated (R held), the same compute pass draws
a **degree-readout dial** over the canvas in screen space — a ring, 15° tick marks, an accent
needle at the current angle, and the numeric degrees via a compact 5×7 bitmap font baked into the
shader. (The canvas is a child `Fl_Window` Vulkan owns, so FLTK can't draw this; it has to be
Vulkan-drawn.) `VulkanCanvas` pushes `setRotationOverlay(active, angle, degrees)` each frame.

**Marching ants (S13).** The active selection's animated marquee is also drawn by the present
pass. The document's 8-bit selection coverage mask is uploaded as an **R8 texture** (binding 2,
same staged-upload lifecycle as the canvas texture — `WindowRenderer::setSelectionMask`, called on
selection *changes*, not per frame; while no selection has ever been pushed a **1×1 zero
placeholder** keeps the binding valid). A screen pixel is on the marquee when its document point is
selected (bilinear sample ≥ 0.5) but one of its **4 screen-space neighbours** — mapped through the
inverse view transform's columns — is not, so the outline stays one screen pixel wide at any
zoom/rotation, and a selection touching the document edge still outlines (outside the document is
never "selected"). Dashes alternate black/white along `(x+y)` with period `ANTS_PERIOD` (= 8 px;
mirrored as `render::kAntsDashPeriodPx`) and crawl with a phase push constant the canvas advances
off the wall clock (`setAntsPhase`, pre-wrapped in double precision). Headless tests assert the
*mask* (`core::selectionFromLayerPixels`, boolean ops), never an animation frame.

## DPI / HiDPI

FLTK 1.4 applies its own UI scaling (`Fl::screen_scale`), so widget coordinates are logical
units. For the swapchain, the **authoritative pixel size is the surface's
`currentExtent`** (reliable under X11); the `NativeSurfaceHandle` pixel size is only a hint
used when the surface reports no fixed extent (`0xFFFFFFFF`, as Wayland does), where the
Wayland integer buffer scale (`fl_wl_buffer_scale`) is folded in.

⚠ `NativeSurfaceHandle::scale` is **hard-coded to 1 on the X11 branch** — X11 has no per-window
buffer scale to read, and every consumer of that number (overlay line widths, the brush reticle,
RGBA cursor rasterization) has only ever run at the Wayland value. Deriving it for X11 would switch
all of them on at once, untested; S59-a deliberately left it alone while flipping the default.
The practical consequence is that HiDPI is exercised on the native-Wayland path only.

## Verifying without a human

```bash
# Headless (no display): offscreen clear/compute + golden checks
./build/linux-debug/bin/mosaic --headless --clear 64,128,192 --export /tmp/out.ppm --gpu-compute

# Windowed swapchain smoke test (needs a display): render 10 frames then exit 0
./build/linux-debug/bin/mosaic --gui-frames 10
# expect: "[ui] Vulkan canvas on <GPU>" and no "[vulkan] ..." validation lines

# On a Wayland session the line above IS the native path (S59-a). To smoke the X11/XWayland
# path -- still supported, and what a pure-Xorg user gets -- pin it explicitly:
FLTK_BACKEND=x11 ./build/linux-debug/bin/mosaic --gui-frames 10
```

`ctest` stays display-independent (render + SVG-rasterization unit tests only); the windowed
path is exercised via `--gui-frames`, not from CI.
