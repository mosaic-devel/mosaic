#include "common/image.hpp"
#include "io/io.hpp"
#include "io/mosaic/preview.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

// mosaic-thumbnailer (S48-b): the freedesktop thumbnailer for .mosaic files, a SEPARATE small
// binary (the ffmpegthumbnailer shape, user call) because a file manager spawns one per file in
// a directory. Links mosaic_io + mosaic_common only -- no FLTK, no Vulkan, no fontconfig, no
// display -- and reads just the newest PRVW chunk (a verified linear scan that decompresses no
// tile content), downscales to the requested size, and writes a PNG.
//
// A file that predates previews (pre-S48-b) has no PRVW to read. Compositing it would mean
// linking the document model AND a compositor into this binary -- exactly the render dependency
// the design forbids -- so the deliberate answer is: no preview -> no thumbnail, exit nonzero,
// and the file manager shows the mimetype icon. One Save in a current Mosaic embeds a preview
// and the file thumbnails forever after.
//
// Invocation (share/thumbnailers/mosaic.thumbnailer): mosaic-thumbnailer -s %s %i %o
namespace {

int usage(const char* argv0) {
    std::fprintf(stderr, "usage: %s [-s size] input.mosaic output.png\n", argv0);
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    long size = 256; // freedesktop "large"; requests only ever downscale the 256px PRVW
    const char* input = nullptr;
    const char* output = nullptr;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-s") {
            if (i + 1 >= argc)
                return usage(argv[0]);
            size = std::strtol(argv[++i], nullptr, 10);
        } else if (input == nullptr) {
            input = argv[i];
        } else if (output == nullptr) {
            output = argv[i];
        } else {
            return usage(argv[0]);
        }
    }
    if (input == nullptr || output == nullptr || size <= 0)
        return usage(argv[0]);

    std::string err;
    const auto preview = mosaic::io::native::readNewestPreview(input, &err);
    if (!preview.has_value()) {
        // Honest and quiet: a pre-preview file is not an error worth a stack of log noise, but
        // the exit code must say "no thumbnail" so the caller falls back to the mimetype icon.
        std::fprintf(stderr, "mosaic-thumbnailer: %s: %s\n", input, err.c_str());
        return 1;
    }

    const mosaic::common::Image scaled = mosaic::io::native::downscalePreview(
        *preview, static_cast<std::uint32_t>(size));
    if (!mosaic::io::savePng(scaled, output, {}, &err)) {
        std::fprintf(stderr, "mosaic-thumbnailer: %s: %s\n", output, err.c_str());
        return 1;
    }
    return 0;
}
