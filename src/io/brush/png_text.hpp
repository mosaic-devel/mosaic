#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The PNG text-chunk walker under the brush preset formats (docs/brushes.md §3.1, §7).
//
// A `.kpp` preset is a PNG whose payload rides in a text chunk keyed `preset`; the future native
// `.mbp` is a PNG with an `iTXt` keyed `mosaic-preset`. libpng's SIMPLIFIED API -- what io/png.cpp
// decodes thumbnails with -- cannot see chunks at all, so this walks the container itself:
// length/type/data/CRC per chunk, collecting every text chunk (`tEXt`, `zTXt`, `iTXt`) and
// inflating the compressed ones.
//
// ⚠ **Match on the KEYWORD, never on the chunk type.** In the shipped CC-0 bundles the `preset`
// chunk is `zTXt` in 213 presets and a plain `tEXt` in 35 -- 19 % of the default set -- and the
// `version` text predicts nothing about which. A reader that expects `zTXt` silently drops those
// 35, and because they are not a random sample, it also skews every statistic computed over what
// remains. (§3.1, verified against the shipped files 2026-07-09.)
//
// The input is a third-party file, so the walker is total: it either returns a scan or an error,
// never crashes, and never allocates more than its documented caps however hostile the header. A
// text chunk that is individually undecodable (bad CRC, unknown compression method, inflate
// failure, over the size cap) is SKIPPED and counted rather than failing the file -- one corrupt
// comment must not make an otherwise loadable preset vanish -- and the count is an honesty
// counter in the docio style: a caller can tell "the keyword is absent" from "the keyword may
// have been in a chunk we could not read".
namespace mosaic::io::brush {

// Which container chunk carried a text. NEVER dispatch on this to find a payload -- it exists so
// the corpus verification can reproduce §3.1's 213-zTXt/35-tEXt split, and for error messages.
enum class PngTextKind : std::uint8_t { Text, ZText, IText };

// One decoded text chunk. `keyword` is 1..79 bytes per the PNG spec (Latin-1 in tEXt/zTXt, UTF-8
// in iTXt -- for the ASCII keywords the formats use, the distinction never matters). `text` is the
// chunk's payload, inflated when it was compressed; for `iTXt` the language tag and translated
// keyword are dropped (no consumer reads them).
struct PngText {
    std::string keyword;
    std::string text;
    PngTextKind kind = PngTextKind::Text;
};

struct PngTextScan {
    // Every decodable text chunk, in file order.
    std::vector<PngText> chunks;
    // Text chunks present in the file that were skipped: CRC mismatch, an unknown compression
    // method, inflate failure, a malformed keyword, or a payload over the caps. Zero on every
    // well-formed file.
    int undecodable = 0;

    // The first chunk whose keyword is exactly `keyword`, or nullptr. THE lookup -- see the
    // header comment for why it must be by keyword, not chunk type.
    [[nodiscard]] const PngText* find(std::string_view keyword) const noexcept;
};

// A single text payload may not inflate past this (the shipped preset XMLs are ~60 KB; the cap
// leaves two orders of magnitude of headroom while stopping a zip bomb).
inline constexpr std::size_t kMaxTextBytes = 16u << 20;
// ...and one file's text payloads may not amount to more than this ACROSS chunks. Without it the
// two caps below still admit 64 chunks x 16 MB = a gigabyte of amplification out of a ~1 MB file.
inline constexpr std::size_t kMaxTotalTextBytes = 64u << 20;
// Text chunks collected per file before further ones count as undecodable (shipped presets carry
// 2). Bounds the walk against a file that is nothing but text chunks.
inline constexpr int kMaxTextChunks = 64;

// Walk the PNG at `data`. Returns std::nullopt (and a reason in `*error`) only when the container
// itself is unusable: not a PNG signature, or the chunk framing breaks (truncated chunk, missing
// IEND) before the walk completes. Per-chunk problems are counted, not fatal -- see above.
[[nodiscard]] std::optional<PngTextScan> scanPngText(const std::uint8_t* data, std::size_t size,
                                                     std::string* error = nullptr);

} // namespace mosaic::io::brush
