// mosaic_stats -- where the bytes went. A census of one .mosaic file: per chunk type, per layer
// KIND, and per individual layer, split between the checkpoint and the appended-history region.
//
// It exists because "the file is 289 MB" is not a finding. The format stores lossless raster
// tiles, so a large document is expected to be large -- but "expected to be large" and "this
// particular layer is 55% of the file" are different statements, and only the second one tells
// you whether anything is wrong. A cost model nobody can query is a cost model nobody trusts.
//
// It reads through the REAL reader twice, deliberately:
//
//   scanChunks()     -- every frame's ON-DISK extent (`consumed`: header + link + payload +
//                       checksum), which is the only number that adds up to the file size. Payload
//                       length alone silently omits ~10% of framing overhead across 30k frames.
//   openDocument()   -- the layer tree, so a TILE key's layer id becomes a NAME and a KIND.
//                       Without it every row would read "layer 47", which is a census of nothing.
//
// TILE and VECT keys both carry the owner layer id as LE64 in bytes[0..8) (chunk.cpp), and a
// layer's MASK surface shares that id with bit 63 set (docio.hpp kMaskSurfaceBit) -- so masks are
// attributed to their layer but counted on their own row, because "the mask costs more than the
// pixels" is a thing that happens and should be visible when it does.
//
// Usage: mosaic_stats <file.mosaic> [--top N]

#include "core/document.hpp"
#include "core/layer.hpp"
#include "io/mosaic/chunk.hpp"
#include "io/mosaic/docio.hpp"
#include "io/mosaic/format.hpp"
#include "io/mosaic/save.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace io = mosaic::io::native;
namespace core = mosaic::core;
namespace fs = std::filesystem;

namespace {

// ---- formatting --------------------------------------------------------------------------------

std::string bytesHuman(std::uintmax_t n) {
    char buf[64];
    if (n >= 1024ull * 1024 * 1024)
        std::snprintf(buf, sizeof buf, "%.2f GB", n / (1024.0 * 1024 * 1024));
    else if (n >= 1024ull * 1024)
        std::snprintf(buf, sizeof buf, "%.1f MB", n / (1024.0 * 1024));
    else if (n >= 1024)
        std::snprintf(buf, sizeof buf, "%.1f KB", n / 1024.0);
    else
        std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(n));
    return buf;
}

double pct(std::uintmax_t part, std::uintmax_t whole) {
    return whole == 0 ? 0.0 : 100.0 * static_cast<double>(part) / static_cast<double>(whole);
}

void rule(int n = 78) {
    for (int i = 0; i < n; ++i)
        std::putchar('-');
    std::putchar('\n');
}

const char* kindName(core::LayerKind k) {
    switch (k) {
    case core::LayerKind::Group:
        return "group";
    case core::LayerKind::Raster:
        return "raster";
    case core::LayerKind::Vector:
        return "vector";
    case core::LayerKind::Text:
        return "text";
    case core::LayerKind::Adjustment:
        return "adjustment";
    case core::LayerKind::Magic:
        return "magic";
    case core::LayerKind::Texture:
        return "texture";
    }
    return "?";
}

// ---- accumulators -------------------------------------------------------------------------------

struct Bucket {
    std::uintmax_t frames = 0;
    std::uintmax_t onDisk = 0;       // sum of `consumed`: what the file actually spends
    std::uintmax_t compressed = 0;   // payload bytes
    std::uintmax_t uncompressed = 0; // what those payloads expand to

    void add(const io::ChunkRecord& r) {
        ++frames;
        onDisk += r.consumed;
        compressed += r.payloadLen;
        uncompressed += r.uncompressedLen;
    }
};

struct LayerStat {
    std::string name = "(not in the current tree)";
    core::LayerKind kind = core::LayerKind::Group;
    bool known = false;
    Bucket content; // TILE + VECT for the layer itself
    Bucket mask;    // TILE for its mask surface
    std::uintmax_t tiles = 0;

    [[nodiscard]] std::uintmax_t total() const { return content.onDisk + mask.onDisk; }
};

std::uint64_t loadLe64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i)
        v = (v << 8) | p[static_cast<std::size_t>(i)];
    return v;
}

std::vector<std::uint8_t> readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// Walk the tree so nested layers are reachable: the census addresses layers by id, and every one
// of this fixture's interesting layers lives inside a group.
void indexTree(const core::GroupLayer& g, std::map<std::uint64_t, LayerStat>& out) {
    for (const auto& c : g.children()) {
        LayerStat& s = out[c->id()];
        s.name = c->name();
        s.kind = c->kind();
        s.known = true;
        if (const auto* sub = c->as<core::GroupLayer>())
            indexTree(*sub, out);
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string path;
    std::size_t top = 15;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--top" && i + 1 < argc)
            top = static_cast<std::size_t>(std::atoi(argv[++i]));
        else if (a == "-h" || a == "--help") {
            std::printf("usage: mosaic_stats <file.mosaic> [--top N]\n");
            return 0;
        } else if (path.empty())
            path = a;
    }
    if (path.empty()) {
        std::fprintf(stderr, "usage: mosaic_stats <file.mosaic> [--top N]\n");
        return 2;
    }

    const std::vector<std::uint8_t> bytes = readFile(path);
    if (bytes.empty()) {
        std::fprintf(stderr, "mosaic_stats: %s is empty or unreadable\n", path.c_str());
        return 1;
    }
    const std::uintmax_t fileSize = bytes.size();

    // The document, for names and kinds.
    const io::OpenReport report = io::openDocument(bytes);
    std::string err;
    auto opened = io::documentFromReport(report, &err);
    std::map<std::uint64_t, LayerStat> layers;
    std::map<core::LayerKind, std::size_t> kindCounts;
    if (opened.has_value() && opened->document != nullptr) {
        indexTree(opened->document->root(), layers);
        for (const auto& [id, s] : layers)
            ++kindCounts[s.kind];
    } else {
        std::printf("warning: could not rebuild the document (%s) -- rows will be unnamed\n\n",
                    err.c_str());
    }

    // The frame census. `walStart` splits the checkpoint from the appended-save region: history
    // lives past it and carries no parity, so mixing the two would misreport both.
    const std::size_t walStart = static_cast<std::size_t>(report.base.walStartOffset);
    std::map<std::string, Bucket> byType;
    Bucket checkpoint, history, invalid;
    std::uintmax_t accounted = 0;

    for (const io::ChunkRecord& r : io::scanChunks(bytes)) {
        if (!r.valid) {
            invalid.add(r);
            continue;
        }
        accounted += r.consumed;
        const std::string tag(reinterpret_cast<const char*>(r.type.data()), r.type.size());
        byType[tag].add(r);
        (r.offset < walStart ? checkpoint : history).add(r);

        if (r.type != io::kTypeTile && r.type != io::kTypeVector)
            continue;

        const std::uint64_t owner = loadLe64(r.key.bytes.data());
        const bool isMask = (owner & io::kMaskSurfaceBit) != 0;
        const std::uint64_t layerId = owner & ~io::kMaskSurfaceBit;
        LayerStat& s = layers[layerId];
        if (isMask)
            s.mask.add(r);
        else {
            s.content.add(r);
            if (r.type == io::kTypeTile)
                ++s.tiles;
        }
    }

    // ---- report --------------------------------------------------------------------------------
    std::printf("\n%s\n", fs::absolute(path).string().c_str());
    if (opened.has_value() && opened->document != nullptr) {
        const core::Document& d = *opened->document;
        std::printf("  %ux%u (%.1f MP), %zu layers, %zu saved state(s)\n", d.width(), d.height(),
                    d.width() * double(d.height()) / 1e6, d.layerCount(), report.commits.size());
    }
    std::printf("  %s on disk\n\n", bytesHuman(fileSize).c_str());

    // Region split.
    std::printf("REGION\n");
    rule();
    std::printf("  %-22s %12s %8s  %s\n", "", "on disk", "share", "frames");
    std::printf("  %-22s %12s %7.1f%%  %llu\n", "checkpoint (content)",
                bytesHuman(checkpoint.onDisk).c_str(), pct(checkpoint.onDisk, fileSize),
                static_cast<unsigned long long>(checkpoint.frames));
    std::printf("  %-22s %12s %7.1f%%  %llu\n", "appended history",
                bytesHuman(history.onDisk).c_str(), pct(history.onDisk, fileSize),
                static_cast<unsigned long long>(history.frames));
    if (fileSize > accounted)
        std::printf("  %-22s %12s %7.1f%%  (framing slack / padding)\n", "unaccounted",
                    bytesHuman(fileSize - accounted).c_str(), pct(fileSize - accounted, fileSize));
    if (invalid.frames != 0)
        std::printf("  %-22s %12s           %llu frame(s) failed checksum\n", "INVALID",
                    bytesHuman(invalid.onDisk).c_str(),
                    static_cast<unsigned long long>(invalid.frames));
    std::printf("\n");

    // Per chunk type.
    std::printf("CHUNK TYPE\n");
    rule();
    std::printf("  %-6s %8s %12s %8s %12s %7s\n", "tag", "frames", "on disk", "share",
                "uncompressed", "ratio");
    std::vector<std::pair<std::string, Bucket>> types(byType.begin(), byType.end());
    std::sort(types.begin(), types.end(),
              [](const auto& a, const auto& b) { return a.second.onDisk > b.second.onDisk; });
    for (const auto& [tag, b] : types) {
        const double ratio =
            b.compressed == 0 ? 0.0 : static_cast<double>(b.uncompressed) / b.compressed;
        std::printf("  %-6s %8llu %12s %7.1f%% %12s  %5.2fx\n", tag.c_str(),
                    static_cast<unsigned long long>(b.frames), bytesHuman(b.onDisk).c_str(),
                    pct(b.onDisk, fileSize), bytesHuman(b.uncompressed).c_str(), ratio);
    }
    std::printf("\n");

    // Per layer kind.
    std::printf("LAYER KIND\n");
    rule();
    std::printf("  %-12s %7s %12s %8s %12s %9s\n", "kind", "layers", "on disk", "share",
                "avg/layer", "tiles");
    std::map<core::LayerKind, Bucket> byKind;
    std::map<core::LayerKind, std::uintmax_t> tilesByKind;
    for (const auto& [id, s] : layers) {
        if (!s.known)
            continue;
        Bucket& b = byKind[s.kind];
        b.frames += s.content.frames + s.mask.frames;
        b.onDisk += s.total();
        b.compressed += s.content.compressed + s.mask.compressed;
        b.uncompressed += s.content.uncompressed + s.mask.uncompressed;
        tilesByKind[s.kind] += s.tiles;
    }
    for (const auto& [kind, count] : kindCounts) {
        const Bucket& b = byKind[kind];
        std::printf("  %-12s %7zu %12s %7.1f%% %12s %9llu\n", kindName(kind), count,
                    bytesHuman(b.onDisk).c_str(), pct(b.onDisk, fileSize),
                    bytesHuman(count ? b.onDisk / count : 0).c_str(),
                    static_cast<unsigned long long>(tilesByKind[kind]));
    }
    std::printf(
        "\n  (adjustment and group layers carry no pixel or geometry payload of their own;\n"
        "   their whole cost is a few lines of the shared MFST manifest)\n\n");

    // Per layer, heaviest first.
    std::printf("HEAVIEST LAYERS\n");
    rule();
    std::printf("  %-28s %-11s %12s %8s %7s %10s\n", "layer", "kind", "on disk", "share", "tiles",
                "mask");
    std::vector<const LayerStat*> ranked;
    for (const auto& [id, s] : layers)
        if (s.total() != 0)
            ranked.push_back(&s);
    std::sort(ranked.begin(), ranked.end(),
              [](const LayerStat* a, const LayerStat* b) { return a->total() > b->total(); });
    std::size_t shown = 0;
    std::uintmax_t restBytes = 0;
    for (const LayerStat* s : ranked) {
        if (shown++ < top) {
            std::string nm = s->name;
            if (nm.size() > 28)
                nm = nm.substr(0, 27) + "…";
            std::printf("  %-28s %-11s %12s %7.1f%% %7llu %10s\n", nm.c_str(), kindName(s->kind),
                        bytesHuman(s->total()).c_str(), pct(s->total(), fileSize),
                        static_cast<unsigned long long>(s->tiles),
                        s->mask.onDisk ? bytesHuman(s->mask.onDisk).c_str() : "-");
        } else
            restBytes += s->total();
    }
    if (shown > top)
        std::printf("  %-28s %-11s %12s %7.1f%%\n",
                    ("+ " + std::to_string(shown - top) + " more").c_str(), "",
                    bytesHuman(restBytes).c_str(), pct(restBytes, fileSize));
    std::printf("\n");
    return 0;
}
