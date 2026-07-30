#pragma once

#include "formats/formats.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// ICO -- the Windows icon CONTAINER.
//
// An .ico is not an image format at all: it is a directory of independent images, each stored
// either as a bare DIB or (Vista and later) as a whole PNG file. Two details are the whole game:
//
//   * a DIB entry's biHeight is DOUBLED -- it spans the XOR image and the 1-bit AND mask that
//     follows it. Writing the real height there is the classic ICO bug: the file looks correct in
//     most viewers and comes out half-height in Explorer, which is the only place anyone looks at
//     an icon;
//   * a 256-pixel entry stores 0 in the directory's one-byte size fields, because 256 does not fit
//     in a byte and 0 was declared to mean it.
//
// PNG payloads are handed IN by the caller rather than produced here: this library has no PNG
// encoder (that is libpng's job, on the far side of the dependency fence) and growing one to fill
// a directory slot would defeat the whole point of §2.2.
namespace mosaicfmt {

// One directory slot. Exactly one of the two sources is used: `png`, when set, is embedded verbatim
// (and must be a real PNG file -- its signature is checked, because a directory that describes
// bytes it does not have is a corrupt icon); otherwise a 32-bit BGRA DIB is generated from
// `pixels`.
struct IcoEntry {
    ImageView pixels;
    const std::vector<std::uint8_t>* png = nullptr;
};

// The format's own ceiling on an entry's side, since the directory records it in one byte.
inline constexpr std::uint32_t kMaxIcoSide = 256;

// Assemble an .ico. Entries are written in the order given -- convention is smallest first, and
// nothing enforces it. Rejects an empty list, an entry larger than 256 px a side, and a `png`
// payload that is not a PNG.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encodeIco(
    const std::vector<IcoEntry>& entries, std::string* error = nullptr);

// Where one entry's payload lives, and what kind it is. This exists so a caller that HAS a PNG
// decoder can decode a PNG-payload icon without this library gaining a dependency on one.
struct IcoPayload {
    std::uint32_t width = 0;   // as the DIRECTORY declares it (0 in the file means 256)
    std::uint32_t height = 0;
    std::size_t offset = 0;    // into the .ico buffer
    std::size_t size = 0;
    bool isPng = false;
};

// The entry a viewer would show: the largest by area, then by declared bit depth, then by payload
// size. nullopt when the directory is unusable.
[[nodiscard]] std::optional<IcoPayload> selectIcoEntry(const std::uint8_t* data, std::size_t size,
                                                       std::string* error = nullptr);

// Decode the best entry, provided it is a DIB. A PNG-payload entry fails with a message saying so
// -- use selectIcoEntry() plus your own PNG decoder for those.
[[nodiscard]] std::optional<Bitmap> decodeIco(const std::uint8_t* data, std::size_t size,
                                              std::string* error = nullptr);

} // namespace mosaicfmt
