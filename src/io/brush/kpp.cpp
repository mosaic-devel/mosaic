#include "io/brush/kpp.hpp"

#include "io/brush/mapper.hpp"
#include "io/brush/png_text.hpp"
#include "io/brush/preset_xml.hpp"
#include "io/detail.hpp"

#include <vector>

namespace mosaic::io::brush {

std::optional<BrushPreset> readKpp(const std::uint8_t* data, std::size_t size,
                                   std::string* error) {
    const std::optional<PngTextScan> scan = scanPngText(data, size, error);
    if (!scan)
        return std::nullopt;

    // Version gate first, and strictly: the producer rejects anything but these two strings,
    // absent included. A future 6.x format bumping the version must fail loudly here rather than
    // half-parse.
    const PngText* version = scan->find("version");
    if (version == nullptr || (version->text != "2.2" && version->text != "5.0")) {
        if (error != nullptr)
            *error = version == nullptr
                         ? "not a preset: no version chunk"
                         : "unsupported preset version '" + version->text + "'";
        return std::nullopt;
    }

    const PngText* preset = scan->find("preset");
    if (preset == nullptr) {
        if (error != nullptr)
            *error = scan->undecodable > 0
                         ? "no readable preset chunk (a damaged text chunk was skipped)"
                         : "not a preset: no preset chunk";
        return std::nullopt;
    }

    const std::optional<PresetXml> xml = parsePresetXml(preset->text, error);
    if (!xml)
        return std::nullopt;

    return mapPreset(*xml, "kpp");
}

std::optional<common::Image> readKppIcon(const std::uint8_t* data, std::size_t size,
                                         std::string* error) {
    // The raster is an ordinary PNG; the simplified decoder that reads every other PNG in the
    // application reads this one.
    std::vector<std::uint8_t> buf(data, data + size);
    return detail::decodePng(buf, error);
}

} // namespace mosaic::io::brush
