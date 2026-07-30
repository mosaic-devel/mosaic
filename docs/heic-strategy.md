# HEIC / HEVC-in-HEIF Strategy

How Mosaic supports reading **and** writing `.heic` without ever bundling an HEVC codec. Researched
+ verified against primary sources 2026-07-04. This is the design; it is **parked out of the main
Export arc** — HEIC ships only after the delegation layer exists. Cross-ref:
`docs/export-system-plan.md` (§12.1).

---

## 0. Position (one sentence)

**Mosaic ships zero HEVC code.** HEIC read/write work *only* by delegating to a codec the user's
system already provides and licenses; if none is present, Mosaic declines — with personality — and
never tells the user how to obtain one.

This is not a workaround; it is the documented, industry-standard way an application that declines to
bundle a codec supports HEIC. Safari delegates to the OS HEVC decoder; GIMP runtime-gates on
libheif's codec probes; Chrome/Firefox refuse to bundle a decoder at all; Fedora ships libheif
AVIF-only; Debian isolates HEVC into optional plugin packages. Mosaic adopts the same spine,
slightly stricter.

---

## 1. Why HEIC is special

The HEVC bitstream inside a `.heic` is the one thing Mosaic will not bundle, and the rule cuts
**both** directions — no encoder *and* no decoder. That is stricter than most applications, and it
is the whole reason this document exists.

The escape: on every target platform the actual codec is a **separately-licensed system component**
we can call without shipping any HEVC implementation. Whoever ships that component (Apple, Microsoft
+ OEM/user, or the Linux user's distro) is the licensee; a caller that ships no HEVC code carries no
obligation of its own.

---

## 2. Architecture — one `HeicIO` interface, three delegating backends

```
core/io/heic/
  heic_io.hpp            HeicIO interface: probe() → HeicCaps{canDecode,canEncode}; decode(); encode()
  heic_macos.mm          ImageIO backend        (compiled on macOS)
  heic_windows.cpp       WIC + Media Foundation (compiled on Windows)
  heic_linux.cpp         libheif backend        (compiled on Linux)
  heic_unavailable.cpp   null backend           (when built without any path → always declines)
```

- `HeicIO::probe()` is called **once at startup** (and on demand), separately for **decode** and
  **encode** (you can have decode without encode). Result cached.
- The probe drives everything: whether HEIC is *active* in the Open filter / Export combobox, and
  whether the snark fires (§4).
- The interface is delegation glue only. No backend contains or links an HEVC codec.

### 2.1 macOS — Apple ImageIO (`heic_macos.mm`)

- **Read:** `CGImageSourceCreateWithURL/Data` → `CGImageSourceCreateImageAtIndex`. `public.heic` is
  auto-detected; read is available on **10.13+** universally.
- **Write:** `CGImageDestinationCreateWithURL(url, "public.heic" /*kUTTypeHEIC*/, 1, NULL)` →
  `AddImage` → `Finalize`.
- **Capability probe (documented):** read = `public.heic` ∈ `CGImageSourceCopyTypeIdentifiers()`
  (true on 10.13+); **write = `CGImageDestinationCreateWithURL(...) != nil`** — Apple's own
  documented way to detect HEIC-write capability (nil when no HEVC encoder is available; HW encode on
  Skylake+ / all Apple Silicon, 10-bit software encode elsewhere).
- HEVC is baked into the OS and licensed by Apple; ImageIO delegates to VideoToolbox internally — we
  never touch it. 10/12-bit, wide-gamut/ICC, Exif/XMP, gain maps all supported by ImageIO.

### 2.2 Windows — WIC over Media Foundation (`heic_windows.cpp`)

- **Container:** `GUID_ContainerFormatHeif`. Decode: `IWICImagingFactory::CreateDecoderFromFilename`
  → `IWICBitmapDecoder` → frame. Encode: `CreateEncoder(GUID_ContainerFormatHeif,…)` → frame →
  `HeifCompressionMethod` property (`WICHeifCompressionHEVC` for `.heic`,
  `WICHeifCompressionAV1` for `.avif`) → `WriteSource` → `Commit`.
- **Two-layer dependency:** the free **HEIF Image Extension** (WIC container codec) **plus** the
  **HEVC Video Extensions** (the actual HEVC MFT — the paid $0.99 SKU the *user* buys, or the free
  "from Device Manufacturer" SKU the *OEM* pre-installs). Still-HEIC decode/encode runs
  through the **Media Foundation HEVC MFT**.
- **Capability probe (documented):** `MFTEnumEx(MFVideoFormat_HEVC, …)` for decoder / encoder
  presence (Microsoft's guidance: "HEVC … might not be available on all PCs"); or attempt-and-catch
  `WINCODEC_ERR_*` on `CreateDecoder`/`Commit`. Min **Windows 10 1809**.
- The license rides in the HEVC Video Extensions package; the calling app is not the licensee.

### 2.3 Linux — libheif runtime plugins (`heic_linux.cpp`) — the crux

libheif (**≥ v1.14.0**) loads its codec backends as **runtime plugins**. If a compatible codec
library is present on the user's system, libheif will load it — including from any directory named in
the standard `LIBHEIF_PLUGIN_PATH` environment variable. Mosaic relies on this only passively: it
**ships no such plugin and documents no way to build or obtain one**; if the user's environment
already provides one, HEIC lights up on its own.

- **We link libheif** (LGPLv3 — safe to link from GPLv3; libheif contains no HEVC code of its own)
  and **ship no HEVC plugin `.so`**. Best practice: **dynamically link the distro's libheif** rather
  than vendoring, so the distro's own HEVC policy governs.
- **Capability probe:** `heif_have_decoder_for_format(heif_compression_HEVC)` /
  `heif_have_encoder_for_format(heif_compression_HEVC)` (also `heif_get_encoder_descriptors`,
  `heif_context_get_encoder_for_format`). Only expose HEIC read/write when these return true.
- Because Mosaic never bundles the plugins, the plugin licenses (x265 GPL, libde265 LGPL) never enter
  Mosaic's combined work — the *user's system* provides them.
- **Distro reality (read at runtime, never shipped by us):** **Arch** `extra/libheif` hard-deps
  libde265 + x265 → HEIC works out of the box. **Debian/Ubuntu** split `libheif-plugin-libde265`
  (Depends) / `libheif-plugin-x265` (Recommends). **Fedora** base libheif is HEVC-free → HEVC only
  via RPM Fusion's `libheif-freeworld`.

### 2.4 The reusable spine (strategic payoff)

The same "call the OS imaging codec, or dlopen a libheif plugin" abstraction extends cleanly to other
system-codec-gated or heavy formats. **AVIF** in particular falls out for free (WIC `WICHeifCompressionAV1`
/ ImageIO `public.avif` / libheif `heif_compression_AV1`). **Policy: Mosaic delegates to a system
codec only for formats it deliberately refuses to bundle (today: just HEIC).** Formats we *do* bundle
(AVIF via libaom, PNG/JPEG/JXL/WebP/TIFF/GIF, etc.) keep their own cross-platform encoders so output
is byte-consistent regardless of OS — the reason not to route everyday formats through per-OS codecs.
The delegation layer is kept general enough that JP2/JXL-via-OS could be added later if ever useful.

---

## 3. UI placement & availability behavior

- HEIC is a **mainstream consumer format** (every iPhone), so it is **always visible and
  discoverable** — *not* hidden behind "Show all export formats," and *not* silently removed the way
  GIMP hides its menu entries. It must be visible for the snark to ever fire.
- It is marked **"requires system codec"** and availability-probed.
- **Open:** `.heic` stays in the Open/"All images" filter regardless of codec presence. The decoder
  check lives at the **decode layer** (`io::loadImage` gains a HEIC branch that routes to `HeicIO`),
  so the **open snark** (§4) fires on **every** open path that reaches a `.heic` without a decoder —
  File→Open, **drag-and-drop** (S50 drop-on-canvas / drop-on-tab), recent files, and CLI open — not
  just the menu.
- **Export:** HEIC appears in the format combobox. With no encoder → the **export snark** in the loss
  banner + a steer to AVIF/JXL. With an encoder → a neutral "using your system's HEVC codec" line
  plus the normal lossy/subsampling warnings (HEIC is lossy HEVC, like AVIF).

**Deliberate divergence from GIMP:** GIMP *hides* HEIC when unavailable; Mosaic *shows it and
declines with a message*. The snark is a feature (personality), and it stays inside the rule because
it gives **no acquisition instructions** — see §4.

---

## 4. The snark (personality, within the rule)

The decline messages are deliberately **vague about how to get a codec** ("check your system's codec
support") — they never link the Microsoft Store, RPM Fusion, or name x265/libde265. This keeps us
inside "no acquisition instructions" while still being helpful and on-brand.

**Open — no decoder available** (user-authored, locked):

> Somewhere far away, a group of lawyers decided you owed them money to open this format. Mosaic
> declined on your behalf. You might still be able to open this format — check your system's codec
> support.

**Export — no encoder available** (loss-banner; the open message with *open* → *export*, locked):

> Somewhere far away, a group of lawyers decided you owed them money to export this format. Mosaic
> declined on your behalf. You might still be able to export this format — check your system's codec
> support.

**Export — encoder available** (neutral, no snark): *Using your system's HEVC codec.* (+ standard
lossy warnings.)

Copy lives in the i18n catalog like all UI strings.

---

## 5. What Mosaic ships vs never ships

| Ships | Never ships |
|---|---|
| The `HeicIO` interface + per-platform delegation glue | Any HEVC encoder/decoder implementation (x265, libde265, Apple's or Microsoft's codec) |
| The decode/encode capability probes | Any bundled libheif HEVC plugin `.so` |
| The graceful-degradation + snark UI | Instructions to obtain/install/build a codec |
| (Linux) a link against the distro's libheif (LGPLv3, no HEVC code) | (if libheif is ever vendored) any HEVC backend compiled in |

If libheif is ever vendored on Windows/macOS, build it with **zero HEVC backends** and use the OS
path instead (you already have ImageIO/WIC there, so vendoring is unnecessary).

---

## 6. Sequencing

HEIC is **not** in the initial Export milestones. It lands after: (a) the core Export/I-O framework
(loss system, backend registry) exists; and (b) the `HeicIO` delegation layer is built and probed on
at least Linux (the dev platform) via the distro libheif. Windows/macOS backends land with their
respective platform ports (S57/S58). Until then, HEIC is simply absent (the null backend declines
everywhere) — which is honest and safe.
