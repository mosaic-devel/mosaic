#include "io/mosaic/docio.hpp"

#include "io/mosaic/docjson.hpp"
#include "io/mosaic/paeth.hpp"
#include "io/mosaic/preview.hpp"
#include "io/mosaic/wire.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <utility>

namespace mosaic::io::native {
namespace {

using nlohmann::json;

// Reader-side allocation sanity (the manifest is hostile input until proven otherwise): each
// surface dimension is capped like the image codecs' kMaxDim, and the SUM of surface bytes is
// capped so a thousand declared 30000x30000 layers cannot ask for terabytes.
constexpr std::uint32_t kMaxSurfaceDim = 30000;
constexpr std::uint64_t kMaxTotalSurfaceBytes = std::uint64_t{4} << 30; // 4GB

constexpr std::array<std::pair<core::LayerKind, const char*>, 7> kKindTokens{{
    {core::LayerKind::Group, "group"},
    {core::LayerKind::Raster, "raster"},
    {core::LayerKind::Vector, "vector"},
    {core::LayerKind::Text, "text"},
    {core::LayerKind::Adjustment, "adjustment"},
    {core::LayerKind::Magic, "magic"},
    {core::LayerKind::Texture, "texture"},
}};

const char* kindToken(core::LayerKind k) {
    for (const auto& [e, s] : kKindTokens)
        if (e == k)
            return s;
    return "raster";
}

std::optional<core::LayerKind> kindFromToken(const std::string& s) {
    for (const auto& [e, t] : kKindTokens)
        if (s == t)
            return e;
    return std::nullopt;
}

// ---- write side --------------------------------------------------------------------------------

// One pixel surface headed for sparse TILE chunks.
struct SurfaceSrc {
    std::uint64_t id = 0;
    const std::uint8_t* data = nullptr;
    std::uint32_t w = 0, h = 0;
    std::uint32_t bpp = 4; // rgba8 = 4, a8 (masks) = 1
    std::uint8_t skipByte = 0; // a tile uniformly this byte is not stored (0 / 255)
};

struct Writer {
    json surfaces = json::array();
    std::vector<SurfaceSrc> pixelSurfaces;
    std::vector<FileChunk> vectChunks;
    std::string error;

    void addSurface(std::uint64_t id, const std::uint8_t* data, std::uint32_t w, std::uint32_t h,
                    std::uint32_t bpp) {
        surfaces.push_back(json{{"id", id}, {"fmt", bpp == 4 ? "rgba8" : "a8"},
                                {"w", w}, {"h", h}});
        pixelSurfaces.push_back(
            {id, data, w, h, bpp, static_cast<std::uint8_t>(bpp == 4 ? 0 : 255)});
    }

    void addVectPayload(core::LayerId id, const json& payload) {
        FileChunk c;
        c.type = kTypeVector;
        c.key = vectorKey(id);
        c.generation = 0;
        c.profile = Profile::Balanced;
        c.flags = kFlagCritical;
        c.parity = true; // current-content vector chunks are parity-covered (spec 2.7)
        const std::string s = payload.dump();
        c.payload.assign(s.begin(), s.end());
        vectChunks.push_back(std::move(c));
    }

    std::optional<json> layerToJson(const core::Layer& layer) {
        json node{{"id", layer.id()},
                  {"kind", kindToken(layer.kind())},
                  {"name", layer.name()},
                  {"visible", layer.visible()},
                  {"locked", layer.locked()},
                  {"clip", layer.clipToBelow()},
                  {"pasted", layer.pastedMarker()},
                  {"opacity", layer.opacity()},
                  {"blend", detail::blendModeToken(layer.blendMode())},
                  {"transform", detail::affineToJson(layer.transform())}};
        if (const core::RasterMask* mask = layer.mask(); mask != nullptr && !mask->empty()) {
            const std::uint64_t sid = layer.id() | kMaskSurfaceBit;
            addSurface(sid, mask->coverage.data(), mask->width, mask->height, 1);
            // ⚠ `place` (RasterMask::toLocal) is REQUIRED for a mask on a layer with no pixel grid
            // of its own -- vector/group/adjustment sheets are the DOC WINDOW captured at build
            // time, so without it the sheet reloads pinned to layer-local (0,0), which for a shape
            // (centred on its own origin) covers only one quadrant. Written always; read optionally,
            // so pre-S31 documents keep loading at identity.
            node["mask"] = json{{"surface", sid},        {"w", mask->width},
                                {"h", mask->height},     {"enabled", mask->enabled},
                                {"linked", mask->linked}, {"place", detail::affineToJson(mask->toLocal)}};
        }
        if (layer.hasEffects())
            node["effects"] = detail::effectsToJson(layer.effects());
        // The coverage-partition link (core::CoveragePartition), so a document saved right after a
        // feathered cut + paste in place reopens without the seam. `selfAlphaHash` is deliberately
        // NOT written: the file's own pixels ARE the split state, so the reader re-hashes what it
        // just loaded rather than trusting a number in the manifest (see reseedPartitions).
        if (const std::optional<core::CoveragePartition>& p = layer.partition(); p.has_value())
            node["partition"] = json{{"partner", p->partner},
                                     {"token", p->token},
                                     {"rel", detail::affineToJson(p->relToPartner)}};
        // Only carrying-something exif is written: exifFromJson rejects an empty node, so an
        // empty one must never exist on the wire (the docjson section note).
        if (layer.exif().has_value() && layer.exif()->hasAny())
            node["exif"] = detail::exifToJson(*layer.exif());
        // S35-b: the per-layer warp lattice, so re-opening a document restores the warp tool's
        // handles where the user left them. Written only for a VALID grid -- warpFromJson rejects an
        // invalid one, so an invalid node must never exist on the wire (the "exif" rule above).
        if (const core::WarpGrid* warp = layer.warp(); warp != nullptr && warp->valid())
            node["warp"] = detail::warpToJson(*warp);

        switch (layer.kind()) {
        case core::LayerKind::Group: {
            const auto& group = static_cast<const core::GroupLayer&>(layer);
            json children = json::array();
            for (const auto& child : group.children()) {
                auto cj = layerToJson(*child);
                if (!cj.has_value())
                    return std::nullopt;
                children.push_back(std::move(*cj));
            }
            node["group"] = json{{"expanded", group.expanded()},
                                 {"children", std::move(children)}};
            break;
        }
        case core::LayerKind::Raster: {
            const auto& raster = static_cast<const core::RasterLayer&>(layer);
            const common::Image& img = raster.image();
            addSurface(layer.id(), img.rgba.data(), img.width, img.height, 4);
            node["raster"] = json{{"surface", layer.id()}, {"w", img.width}, {"h", img.height}};
            break;
        }
        case core::LayerKind::Magic: {
            const auto& magic = static_cast<const core::MagicLayer&>(layer);
            const common::Image& img = magic.source();
            addSurface(layer.id(), img.rgba.data(), img.width, img.height, 4);
            node["magic"] = json{{"surface", layer.id()}, {"w", img.width}, {"h", img.height}};
            break;
        }
        case core::LayerKind::Vector: {
            const auto& vector = static_cast<const core::VectorLayer&>(layer);
            node["vector"] = json{{"has_object", vector.hasObject()}};
            if (vector.hasObject())
                addVectPayload(layer.id(), detail::vectorObjectToJson(*vector.object()));
            break;
        }
        case core::LayerKind::Text: {
            const auto& text = static_cast<const core::TextLayer&>(layer);
            node["text"] = json{{"auto_named", text.autoNamed()}};
            addVectPayload(layer.id(), detail::textBlockToJson(text.block()));
            break;
        }
        case core::LayerKind::Adjustment: {
            const auto& adj = static_cast<const core::AdjustmentLayer&>(layer);
            node["adjustment"] = json{{"kind", detail::adjustmentKindToken(adj.adjustmentKind())},
                                      {"params", adj.params()}};
            break;
        }
        case core::LayerKind::Texture: {
            // The params ARE the content (small, inline like adjustment); the pixel cache is
            // NOT stored -- it regenerates deterministically from (params, document size) on
            // the first composite after load (docs/texture-generator.md §8.3).
            const auto& tex = static_cast<const core::TextureLayer&>(layer);
            node["texture"] = detail::textureParamsToJson(tex.params());
            break;
        }
        }
        return node;
    }

    // Slice one surface into sparse TILE chunks (spec 2.2 tile keys; 64px, Round 10-measured).
    void sliceSurface(const SurfaceSrc& s, std::vector<FileChunk>& out) {
        const std::uint32_t rowBytes = s.w * s.bpp;
        std::vector<std::uint8_t> tile;
        for (std::uint32_t ty = 0; ty * kTileSize < s.h; ++ty) {
            for (std::uint32_t tx = 0; tx * kTileSize < s.w; ++tx) {
                const std::uint32_t x0 = tx * kTileSize;
                const std::uint32_t y0 = ty * kTileSize;
                const std::uint32_t tw = std::min(kTileSize, s.w - x0);
                const std::uint32_t th = std::min(kTileSize, s.h - y0);
                tile.resize(static_cast<std::size_t>(tw) * th * s.bpp);
                bool uniform = true;
                for (std::uint32_t row = 0; row < th; ++row) {
                    const std::uint8_t* src =
                        s.data + static_cast<std::size_t>(y0 + row) * rowBytes +
                        static_cast<std::size_t>(x0) * s.bpp;
                    std::uint8_t* dst = tile.data() + static_cast<std::size_t>(row) * tw * s.bpp;
                    std::memcpy(dst, src, static_cast<std::size_t>(tw) * s.bpp);
                    for (std::uint32_t i = 0; uniform && i < tw * s.bpp; ++i)
                        uniform = dst[i] == s.skipByte;
                }
                if (uniform)
                    continue; // absent = the surface default (transparent / fully visible)
                FileChunk c;
                c.type = kTypeTile;
                c.key = tileKey(s.id, tx, ty);
                c.generation = 0;
                c.profile = Profile::Balanced;
                c.flags = kFlagCritical;
                c.parity = true;
                if (s.bpp == 4) {
                    c.payload.resize(tile.size());
                    filterPaethRgba(tile, tw, th, c.payload);
                    c.flags |= kFlagFiltered;
                } else {
                    c.payload = tile;
                }
                out.push_back(std::move(c));
            }
        }
    }
};

// ---- read side ---------------------------------------------------------------------------------

struct SurfaceInfo {
    std::uint32_t w = 0, h = 0;
    std::uint32_t bpp = 4;
};

// Where a surface's decoded tiles land: a pointer into the OWNING layer's final storage (the
// image / mask lives inside the layer, so the pointer is taken after tree insertion).
struct FillTarget {
    std::uint8_t* data = nullptr;
    std::uint32_t w = 0, h = 0;
    std::uint32_t bpp = 4;
};

struct Reader {
    const std::map<std::pair<ChunkTag, std::array<std::uint8_t, 16>>, const RecoveredChunk*>&
        content;
    std::map<std::uint64_t, SurfaceInfo> surfaces;
    std::map<std::uint64_t, FillTarget> targets;
    std::set<core::LayerId> seenIds;
    std::size_t rejected = 0;
    std::string error;

    bool fail(const char* what) {
        if (error.empty())
            error = what;
        return false;
    }

    const RecoveredChunk* findVect(core::LayerId id) const {
        const ChunkKey k = vectorKey(id);
        const auto it = content.find({kTypeVector, k.bytes});
        return it == content.end() ? nullptr : it->second;
    }

    bool applyChrome(core::Layer& layer, const json& node) {
        std::string name, blend;
        bool visible = true, locked = false, clip = false, pasted = false;
        float opacity = 1.0f;
        const auto tf = node.contains("transform")
                            ? detail::affineFromJson(node["transform"])
                            : std::optional<common::Affine2D>{};
        if (!tf || !node.contains("name") || !node["name"].is_string() ||
            !node.contains("blend") || !node["blend"].is_string() ||
            !node.contains("visible") || !node["visible"].is_boolean() ||
            !node.contains("locked") || !node["locked"].is_boolean() ||
            !node.contains("clip") || !node["clip"].is_boolean() ||
            !node.contains("pasted") || !node["pasted"].is_boolean() ||
            !node.contains("opacity") || !node["opacity"].is_number())
            return fail("malformed layer chrome");
        name = node["name"].get<std::string>();
        blend = node["blend"].get<std::string>();
        visible = node["visible"].get<bool>();
        locked = node["locked"].get<bool>();
        clip = node["clip"].get<bool>();
        pasted = node["pasted"].get<bool>();
        opacity = node["opacity"].get<float>();
        const auto mode = detail::blendModeFromToken(blend);
        if (!mode)
            return fail("unknown blend mode");
        layer.setName(std::move(name));
        layer.setVisible(visible);
        layer.setLocked(locked);
        layer.setClipToBelow(clip);
        layer.setPastedMarker(pasted);
        layer.setOpacity(opacity);
        layer.setBlendMode(*mode);
        layer.setTransform(*tf);
        if (const auto maskIt = node.find("mask"); maskIt != node.end()) {
            if (!maskIt->is_object())
                return fail("malformed mask");
            const json& m = *maskIt;
            std::uint64_t sid = 0, w = 0, h = 0;
            if (!m.contains("surface") || !m["surface"].is_number_unsigned() ||
                !m.contains("w") || !m["w"].is_number_unsigned() || !m.contains("h") ||
                !m["h"].is_number_unsigned() || !m.contains("enabled") ||
                !m["enabled"].is_boolean() || !m.contains("linked") ||
                !m["linked"].is_boolean())
                return fail("malformed mask");
            sid = m["surface"].get<std::uint64_t>();
            w = m["w"].get<std::uint64_t>();
            h = m["h"].get<std::uint64_t>();
            const auto sit = surfaces.find(sid);
            if (sit == surfaces.end() || sit->second.bpp != 1 || sit->second.w != w ||
                sit->second.h != h)
                return fail("mask references a missing or mismatched surface");
            core::RasterMask mask(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h),
                                  255);
            mask.enabled = m["enabled"].get<bool>();
            mask.linked = m["linked"].get<bool>();
            // OPTIONAL on purpose: a document written before the mask sheet carried a placement has
            // no "place" key, and its sheets were all identity-placed. Absent must not fail().
            if (const auto pit = m.find("place"); pit != m.end()) {
                if (const auto place = detail::affineFromJson(*pit))
                    mask.toLocal = *place;
                else
                    return fail("mask carries an unreadable placement");
            }
            layer.setMask(std::move(mask));
            targets[sid] = {layer.mask()->coverage.data(), static_cast<std::uint32_t>(w),
                            static_cast<std::uint32_t>(h), 1};
        }
        if (const auto fxIt = node.find("effects"); fxIt != node.end()) {
            auto fx = detail::effectsFromJson(*fxIt);
            if (!fx)
                return fail("malformed layer effects");
            layer.setEffects(std::move(*fx));
        }
        if (const auto pIt = node.find("partition"); pIt != node.end()) {
            // A partition is a compositing HINT, never load-critical: partitionPairLive re-derives
            // every condition from the live tree, so a malformed or lying node can only cost the
            // seam it was there to hide. Drop it rather than reject the document.
            const json& p = *pIt;
            if (p.is_object() && p.contains("partner") && p["partner"].is_number_unsigned() &&
                p.contains("token") && p["token"].is_number_unsigned() && p.contains("rel")) {
                if (const auto rel = detail::affineFromJson(p["rel"])) {
                    core::CoveragePartition cp;
                    cp.partner = p["partner"].get<std::uint64_t>();
                    cp.token = p["token"].get<std::uint64_t>();
                    cp.relToPartner = *rel;
                    layer.setPartition(cp);  // selfRevision re-seeded after the pixels land
                }
            }
        }
        if (const auto exifIt = node.find("exif"); exifIt != node.end()) {
            auto exif = detail::exifFromJson(*exifIt);
            if (!exif)
                return fail("malformed exif");
            layer.setExif(std::move(*exif));
        }
        // S35-b, on the "exif" rule: absent is fine (every layer written before this node existed),
        // present-but-malformed refuses the file. A warp grid is not a hint the way a coverage
        // partition is -- it decides where the tool's handles are and what a re-edit's difference is
        // measured against -- so a grid we cannot trust is a corrupt document, not a droppable one.
        if (const auto warpIt = node.find("warp"); warpIt != node.end()) {
            auto warp = detail::warpFromJson(*warpIt);
            if (!warp)
                return fail("malformed warp");
            layer.setWarp(std::move(*warp));
        }
        return true;
    }

    // Validate a node's pixel-surface reference against the manifest surface table.
    bool pixelSurface(const json& node, const char* key, std::uint64_t expectId,
                      SurfaceInfo& out) {
        const auto it = node.find(key);
        if (it == node.end() || !it->is_object())
            return fail("malformed pixel layer");
        const json& p = *it;
        if (!p.contains("surface") || !p["surface"].is_number_unsigned() || !p.contains("w") ||
            !p["w"].is_number_unsigned() || !p.contains("h") || !p["h"].is_number_unsigned())
            return fail("malformed pixel layer");
        const std::uint64_t sid = p["surface"].get<std::uint64_t>();
        const auto sit = surfaces.find(sid);
        if (sid != expectId || sit == surfaces.end() || sit->second.bpp != 4 ||
            sit->second.w != p["w"].get<std::uint64_t>() ||
            sit->second.h != p["h"].get<std::uint64_t>())
            return fail("pixel layer references a missing or mismatched surface");
        out = sit->second;
        return true;
    }

    std::unique_ptr<core::Layer> layerFromJson(const json& node) {
        if (!node.is_object() || !node.contains("id") || !node["id"].is_number_unsigned() ||
            !node.contains("kind") || !node["kind"].is_string()) {
            fail("malformed layer node");
            return nullptr;
        }
        const core::LayerId id = node["id"].get<std::uint64_t>();
        const auto kind = kindFromToken(node["kind"].get<std::string>());
        if (id == core::kInvalidLayerId || (id & kMaskSurfaceBit) != 0 || !kind ||
            !seenIds.insert(id).second) {
            fail("invalid or duplicate layer id");
            return nullptr;
        }

        std::unique_ptr<core::Layer> layer;
        switch (*kind) {
        case core::LayerKind::Group: {
            auto group = std::make_unique<core::GroupLayer>(id, "");
            const json* g = node.contains("group") && node["group"].is_object()
                                ? &node["group"]
                                : nullptr;
            if (g == nullptr || !g->contains("expanded") || !(*g)["expanded"].is_boolean() ||
                !g->contains("children") || !(*g)["children"].is_array()) {
                fail("malformed group node");
                return nullptr;
            }
            group->setExpanded((*g)["expanded"].get<bool>());
            for (const json& cj : (*g)["children"]) {
                auto child = layerFromJson(cj);
                if (child == nullptr)
                    return nullptr;
                group->addOnTop(std::move(child));
            }
            layer = std::move(group);
            break;
        }
        case core::LayerKind::Raster: {
            SurfaceInfo info;
            if (!pixelSurface(node, "raster", id, info))
                return nullptr;
            auto raster = std::make_unique<core::RasterLayer>(id, "", info.w, info.h);
            std::fill(raster->image().rgba.begin(), raster->image().rgba.end(), 0);
            targets[id] = {raster->image().rgba.data(), info.w, info.h, 4};
            layer = std::move(raster);
            break;
        }
        case core::LayerKind::Magic: {
            SurfaceInfo info;
            if (!pixelSurface(node, "magic", id, info))
                return nullptr;
            common::Image source(info.w, info.h);
            std::fill(source.rgba.begin(), source.rgba.end(), 0);
            auto magic = std::make_unique<core::MagicLayer>(id, "", std::move(source));
            targets[id] = {magic->source().rgba.data(), info.w, info.h, 4};
            layer = std::move(magic);
            break;
        }
        case core::LayerKind::Vector: {
            auto vector = std::make_unique<core::VectorLayer>(id, "");
            const json* v = node.contains("vector") && node["vector"].is_object()
                                ? &node["vector"]
                                : nullptr;
            if (v == nullptr || !v->contains("has_object") ||
                !(*v)["has_object"].is_boolean()) {
                fail("malformed vector node");
                return nullptr;
            }
            if ((*v)["has_object"].get<bool>()) {
                const RecoveredChunk* payload = findVect(id);
                if (payload == nullptr) {
                    ++rejected; // the object chunk was lost: an EMPTY vector layer, flagged
                } else {
                    const json oj = json::parse(payload->payload.begin(),
                                                payload->payload.end(), nullptr, false);
                    auto object = oj.is_discarded()
                                      ? std::nullopt
                                      : detail::vectorObjectFromJson(oj);
                    if (object.has_value())
                        vector->setObject(std::move(*object));
                    else
                        ++rejected;
                }
            }
            layer = std::move(vector);
            break;
        }
        case core::LayerKind::Text: {
            auto text = std::make_unique<core::TextLayer>(id, "");
            const json* t = node.contains("text") && node["text"].is_object() ? &node["text"]
                                                                              : nullptr;
            if (t == nullptr || !t->contains("auto_named") ||
                !(*t)["auto_named"].is_boolean()) {
                fail("malformed text node");
                return nullptr;
            }
            text->setAutoNamed((*t)["auto_named"].get<bool>());
            const RecoveredChunk* payload = findVect(id);
            if (payload == nullptr) {
                ++rejected; // block lost: an empty text layer, flagged
            } else {
                const json bj = json::parse(payload->payload.begin(), payload->payload.end(),
                                            nullptr, false);
                auto block = bj.is_discarded() ? std::nullopt : detail::textBlockFromJson(bj);
                if (block.has_value())
                    text->setBlock(std::move(*block));
                else
                    ++rejected;
            }
            layer = std::move(text);
            break;
        }
        case core::LayerKind::Adjustment: {
            const json* a = node.contains("adjustment") && node["adjustment"].is_object()
                                ? &node["adjustment"]
                                : nullptr;
            if (a == nullptr || !a->contains("kind") || !(*a)["kind"].is_string() ||
                !a->contains("params") || !(*a)["params"].is_object()) {
                fail("malformed adjustment node");
                return nullptr;
            }
            const auto adjKind =
                detail::adjustmentKindFromToken((*a)["kind"].get<std::string>());
            if (!adjKind) {
                fail("unknown adjustment kind");
                return nullptr;
            }
            auto adj = std::make_unique<core::AdjustmentLayer>(id, "", *adjKind);
            for (const auto& [key, value] : (*a)["params"].items()) {
                if (!value.is_number()) {
                    fail("malformed adjustment params");
                    return nullptr;
                }
                adj->params()[key] = value.get<double>();
            }
            layer = std::move(adj);
            break;
        }
        case core::LayerKind::Texture: {
            const json* t = node.contains("texture") && node["texture"].is_object()
                                ? &node["texture"]
                                : nullptr;
            auto params = t != nullptr ? detail::textureParamsFromJson(*t) : std::nullopt;
            if (!params) {
                fail("malformed texture node");
                return nullptr;
            }
            // No surface to register: the pixel cache regenerates from the params on the
            // first composite after load.
            layer = std::make_unique<core::TextureLayer>(id, "", std::move(*params));
            break;
        }
        }
        if (!applyChrome(*layer, node))
            return nullptr;
        return layer;
    }

    // Decode every recovered TILE chunk into its registered surface. Damage tolerance lives
    // here: a tile whose shape disagrees with its grid slot is rejected (counted), never
    // guessed at. Tiles for owners we did not register (retained history of deleted layers,
    // once H2 lands) are ignored -- they are not current content.
    void fillSurfaces() {
        for (const auto& [key, chunk] : content) {
            if (key.first != kTypeTile)
                continue;
            const std::uint64_t owner = detail::loadLe64(key.second.data());
            const std::uint32_t tx = detail::loadLe32(key.second.data() + 8);
            const std::uint32_t ty = detail::loadLe32(key.second.data() + 12);
            const auto it = targets.find(owner);
            if (it == targets.end())
                continue;
            FillTarget& t = it->second;
            const std::uint64_t x0 = std::uint64_t{tx} * kTileSize;
            const std::uint64_t y0 = std::uint64_t{ty} * kTileSize;
            if (x0 >= t.w || y0 >= t.h) {
                ++rejected;
                continue;
            }
            const std::uint32_t tw = std::min<std::uint64_t>(kTileSize, t.w - x0);
            const std::uint32_t th = std::min<std::uint64_t>(kTileSize, t.h - y0);
            std::vector<std::uint8_t> pixels = chunk->payload;
            if (pixels.size() != static_cast<std::size_t>(tw) * th * t.bpp) {
                ++rejected;
                continue;
            }
            const bool filtered = (chunk->flags & kFlagFiltered) != 0;
            if (filtered && t.bpp != 4) {
                ++rejected; // the Paeth filter is defined over rgba8 only
                continue;
            }
            if (filtered)
                unfilterPaethRgba(pixels, tw, th);
            for (std::uint32_t row = 0; row < th; ++row) {
                std::uint8_t* dst = t.data +
                                    (static_cast<std::size_t>(y0 + row) * t.w + x0) * t.bpp;
                std::memcpy(dst, pixels.data() + static_cast<std::size_t>(row) * tw * t.bpp,
                            static_cast<std::size_t>(tw) * t.bpp);
            }
        }
    }
};

// Re-seed every restored coverage partition's alpha fingerprint from the layer it sits on, once the
// pixel surfaces have been filled. It is not written to the file: what the file guarantees is that
// the pixels it holds ARE the split state, so hashing what just loaded is the right snapshot.
// Anything that edits a half after this point changes its coverage and retires the link, on exactly
// the same terms as a partition made this session.
void reseedPartitions(core::GroupLayer& group) {
    for (std::size_t i = 0; i < group.childCount(); ++i) {
        core::Layer& child = group.child(i);
        if (const std::optional<core::CoveragePartition>& p = child.partition(); p.has_value()) {
            core::CoveragePartition cp = *p;
            if (const auto* r = child.as<core::RasterLayer>()) {
                cp.selfAlphaHash = r->alphaFingerprint();
                child.setPartition(cp);
            } else {
                child.setPartition(std::nullopt);  // only rasters can be halves
            }
        }
        if (auto* g = child.as<core::GroupLayer>())
            reseedPartitions(*g);
    }
}

} // namespace

std::string mintDocumentUuid() {
    std::random_device rd;
    std::array<std::uint32_t, 4> words{rd(), rd(), rd(), rd()};
    std::array<std::uint8_t, 16> bytes{};
    for (std::size_t i = 0; i < 4; ++i)
        detail::storeLe32(bytes.data() + i * 4, words[i]);
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (std::size_t i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            out.push_back('-');
        out.push_back(kHex[bytes[i] >> 4]);
        out.push_back(kHex[bytes[i] & 0xF]);
    }
    return out;
}

std::optional<CheckpointInput> buildDocumentCheckpoint(const core::Document& doc,
                                                       std::string* error,
                                                       const common::Image* preview) {
    const auto fail = [&](const char* what) {
        if (error != nullptr)
            *error = what;
        return std::nullopt;
    };
    if (doc.uuid().empty())
        return fail("the document has no uuid; mint one before saving");

    Writer writer;
    json layers = json::array();
    for (const auto& child : doc.root().children()) {
        auto node = writer.layerToJson(*child);
        if (!node.has_value())
            return fail(writer.error.empty() ? "could not serialize a layer" :
                                               writer.error.c_str());
        layers.push_back(std::move(*node));
    }

    json color{{"space", detail::colorSpaceToken(doc.colorSpace())},
               {"precision", detail::precisionToken(doc.precision())}};
    if (!doc.iccProfilePath().empty()) {
        // The document-level ICC working profile (File -> New "Custom..."). Additive, grown
        // fields: a reader without them keeps the enum in "space" -- which is exactly the
        // documented fallback when the profile file itself is unavailable.
        color["icc_path"] = doc.iccProfilePath();
        color["icc_name"] = doc.iccProfileName();
    }
    const json manifest{{"schema", 1},
                        {"uuid", doc.uuid()},
                        {"title", doc.title()},
                        {"canvas", json{{"w", doc.width()}, {"h", doc.height()},
                                        {"dpi", doc.dpi()}}},
                        {"color", std::move(color)},
                        {"next_layer_id", doc.nextLayerId()},
                        {"surfaces", std::move(writer.surfaces)},
                        {"layers", std::move(layers)}};

    CheckpointInput in;
    in.documentType = kDocTypeRasterVector;
    in.documentUuid = doc.uuid();
    in.generation = 0; // undo states consume ids ABOVE the checkpoint generation (spec 2.2)
    FileChunk mfst;
    mfst.type = kTypeManifest;
    mfst.key = zeroKey();
    mfst.generation = 0;
    mfst.profile = Profile::Balanced;
    mfst.flags = kFlagCritical;
    // Not parity-covered (spec 2.7 stripes tile/vector content: a stripe pads every shard to its
    // longest member, so a big manifest would inflate its whole stripe, and it would still survive
    // only m losses). buildCheckpoint gives it a REPLICA instead (spec 2.3) -- cheaper, and it
    // survives a burst that takes an entire stripe. An earlier comment here claimed HIST manifest
    // snapshots backed it up; they were never emitted, and the manifest was the file's one true
    // single point of failure until the replica landed.
    mfst.parity = false;
    const std::string manifestStr = manifest.dump();
    mfst.payload.assign(manifestStr.begin(), manifestStr.end());
    in.chunks.push_back(std::move(mfst));
    for (FileChunk& c : writer.vectChunks)
        in.chunks.push_back(std::move(c));
    for (const SurfaceSrc& s : writer.pixelSurfaces)
        writer.sliceSurface(s, in.chunks);
    // The app-supplied composite becomes an ordinary PRVW chunk (S48-b) -- present in the
    // serialization exactly like content, so the differ treats it like content and only a
    // changed downscale ever reaches the file.
    if (preview != nullptr && !preview->empty())
        in.chunks.push_back(makePreviewChunk(*preview));
    return in;
}

std::optional<DocumentReadResult> documentFromReport(const OpenReport& report,
                                                     std::string* error) {
    const auto fail = [&](const std::string& what) {
        if (error != nullptr)
            *error = what;
        return std::nullopt;
    };

    // A container from the future (spec 2.1): the reader already declined to interpret it. Say so
    // in the user's words -- "damaged" would be a lie about a file that is perfectly intact, and it
    // is the lie a reader tells when it degrades into recovery instead of checking the version.
    if (report.base.unsupportedVersion) {
        char buf[160];
        std::snprintf(buf, sizeof buf,
                      "this file needs a newer Mosaic (container version %u; this build reads %u)",
                      static_cast<unsigned>(report.base.formatVersion),
                      static_cast<unsigned>(kFormatVersion));
        return fail(buf);
    }

    // Current content: highest generation wins per (TYPE, KEY) across checkpoint + committed.
    std::map<std::pair<ChunkTag, std::array<std::uint8_t, 16>>, const RecoveredChunk*> content;
    const auto offer = [&](const RecoveredChunk& c) {
        auto& slot = content[{c.type, c.key.bytes}];
        if (slot == nullptr || c.generation >= slot->generation)
            slot = &c;
    };
    for (const RecoveredChunk& c : report.base.chunks)
        offer(c);
    for (const RecoveredChunk& c : report.committed)
        offer(c);

    const auto mfstIt = content.find({kTypeManifest, zeroKey().bytes});
    if (mfstIt == content.end())
        return fail("no manifest survived; the document structure is gone");
    const json m = json::parse(mfstIt->second->payload.begin(), mfstIt->second->payload.end(),
                               nullptr, false);
    if (m.is_discarded() || !m.is_object())
        return fail("the manifest is unreadable");
    if (!m.contains("schema") || !m["schema"].is_number_integer() ||
        m["schema"].get<int>() != 1)
        return fail("this file needs a newer Mosaic (unknown manifest schema)");

    std::string uuid, title;
    if (!m.contains("uuid") || !m["uuid"].is_string() || !m.contains("title") ||
        !m["title"].is_string() || !m.contains("canvas") || !m["canvas"].is_object() ||
        !m.contains("color") || !m["color"].is_object() || !m.contains("next_layer_id") ||
        !m["next_layer_id"].is_number_unsigned() || !m.contains("surfaces") ||
        !m["surfaces"].is_array() || !m.contains("layers") || !m["layers"].is_array())
        return fail("the manifest is malformed");
    uuid = m["uuid"].get<std::string>();
    title = m["title"].get<std::string>();

    const json& canvas = m["canvas"];
    if (!canvas.contains("w") || !canvas["w"].is_number_unsigned() || !canvas.contains("h") ||
        !canvas["h"].is_number_unsigned() || !canvas.contains("dpi") ||
        !canvas["dpi"].is_number())
        return fail("the manifest canvas is malformed");
    const std::uint64_t width = canvas["w"].get<std::uint64_t>();
    const std::uint64_t height = canvas["h"].get<std::uint64_t>();
    if (width == 0 || height == 0 || width > kMaxSurfaceDim || height > kMaxSurfaceDim)
        return fail("the manifest canvas size is out of range");

    const json& color = m["color"];
    if (!color.contains("space") || !color["space"].is_string() || !color.contains("precision") ||
        !color["precision"].is_string())
        return fail("the manifest color state is malformed");
    const auto space = detail::colorSpaceFromToken(color["space"].get<std::string>());
    const auto precision = detail::precisionFromToken(color["precision"].get<std::string>());
    if (!space || !precision)
        return fail("the manifest color state is unknown");

    Reader reader{content};
    std::uint64_t totalBytes = 0;
    for (const json& s : m["surfaces"]) {
        if (!s.is_object() || !s.contains("id") || !s["id"].is_number_unsigned() ||
            !s.contains("fmt") || !s["fmt"].is_string() || !s.contains("w") ||
            !s["w"].is_number_unsigned() || !s.contains("h") || !s["h"].is_number_unsigned())
            return fail("the manifest surface table is malformed");
        const std::string fmt = s["fmt"].get<std::string>();
        SurfaceInfo info;
        info.w = s["w"].get<std::uint32_t>();
        info.h = s["h"].get<std::uint32_t>();
        if (fmt == "rgba8")
            info.bpp = 4;
        else if (fmt == "a8")
            info.bpp = 1;
        else
            return fail("unknown surface format");
        if (info.w == 0 || info.h == 0 || info.w > kMaxSurfaceDim || info.h > kMaxSurfaceDim)
            return fail("a manifest surface size is out of range");
        totalBytes += std::uint64_t{info.w} * info.h * info.bpp;
        if (totalBytes > kMaxTotalSurfaceBytes)
            return fail("the manifest asks for an unreasonable amount of pixel memory");
        if (!reader.surfaces.emplace(s["id"].get<std::uint64_t>(), info).second)
            return fail("duplicate surface id");
    }

    auto doc = std::make_unique<core::Document>(static_cast<std::uint32_t>(width),
                                                static_cast<std::uint32_t>(height), *space,
                                                *precision);
    doc->setDpi(canvas["dpi"].get<double>());
    doc->setTitle(title);
    doc->setUuid(uuid);
    // Grown fields (absent in older files): the document-level ICC working profile. The colour
    // enum parsed above stays the honest fallback when the file at this path has vanished.
    if (color.contains("icc_path") && color["icc_path"].is_string()) {
        const std::string iccName = color.contains("icc_name") && color["icc_name"].is_string()
                                        ? color["icc_name"].get<std::string>()
                                        : std::string();
        doc->setIccProfile(color["icc_path"].get<std::string>(), iccName);
    }

    for (const json& node : m["layers"]) {
        auto layer = reader.layerFromJson(node);
        if (layer == nullptr)
            return fail(reader.error.empty() ? "could not rebuild a layer" : reader.error);
        doc->root().addOnTop(std::move(layer));
    }
    // The allocator must clear every persisted id -- setNextLayerId never goes backwards, so a
    // lying manifest cannot make future mints collide with live layers.
    std::uint64_t maxId = 0;
    for (const core::LayerId id : reader.seenIds)
        maxId = std::max(maxId, id);
    doc->setNextLayerId(std::max(m["next_layer_id"].get<std::uint64_t>(), maxId + 1));

    reader.fillSurfaces();
    reseedPartitions(doc->root());

    DocumentReadResult result;
    result.document = std::move(doc);
    result.uuid = std::move(uuid);
    result.rejectedChunks = reader.rejected;
    return result;
}

namespace {

// A mutable live pixel surface a TILE delta writes into: the layer's own content (raster image /
// magic source, rgba8) or its mask (a8, owner id | kMaskSurfaceBit).
struct LiveSurface {
    std::uint8_t* data = nullptr;
    std::uint32_t w = 0, h = 0, bpp = 0;
    core::Layer* layer = nullptr;
};

std::optional<LiveSurface> resolveSurface(core::Document& doc, std::uint64_t owner) {
    const bool isMask = (owner & kMaskSurfaceBit) != 0;
    core::Layer* layer = doc.find(static_cast<core::LayerId>(owner & ~kMaskSurfaceBit));
    if (layer == nullptr)
        return std::nullopt;
    if (isMask) {
        core::RasterMask* mask = layer->mask();
        if (mask == nullptr || mask->empty())
            return std::nullopt;
        return LiveSurface{mask->coverage.data(), mask->width, mask->height, 1, layer};
    }
    if (auto* r = layer->as<core::RasterLayer>())
        return LiveSurface{r->image().rgba.data(), r->image().width, r->image().height, 4, layer};
    if (auto* mg = layer->as<core::MagicLayer>())
        return LiveSurface{mg->source().rgba.data(), mg->source().width, mg->source().height, 4,
                           layer};
    return std::nullopt; // group/adjustment/vector/text/texture layers have no content surface
}

// Bump the layer's content revision so the compositor re-reads the patched surface (the per-kind
// invalidateContentBounds; a no-op for kinds without a pixel surface, which cannot be a TILE
// owner anyway).
void bumpLayer(core::Layer& layer) {
    if (auto* r = layer.as<core::RasterLayer>())
        r->invalidateContentBounds();
    else if (auto* mg = layer.as<core::MagicLayer>())
        mg->invalidateContentBounds();
    else if (auto* v = layer.as<core::VectorLayer>())
        v->invalidateContentBounds();
    else if (auto* t = layer.as<core::TextLayer>())
        t->invalidateContentBounds();
    else if (auto* x = layer.as<core::TextureLayer>())
        x->invalidateContentBounds();  // a mask TILE delta: re-render + recomposite like text
}

} // namespace

void applyChunksToDocument(core::Document& doc, std::span<const StateChunk> chunks,
                           std::size_t* rejected) {
    std::size_t rej = 0;
    for (const StateChunk& c : chunks) {
        if (c.type == kTypeTile) {
            const std::uint64_t owner = detail::loadLe64(c.key.bytes.data());
            const std::uint32_t tx = detail::loadLe32(c.key.bytes.data() + 8);
            const std::uint32_t ty = detail::loadLe32(c.key.bytes.data() + 12);
            const auto surf = resolveSurface(doc, owner);
            if (!surf.has_value()) {
                ++rej;
                continue;
            }
            const std::uint64_t x0 = std::uint64_t{tx} * kTileSize;
            const std::uint64_t y0 = std::uint64_t{ty} * kTileSize;
            if (x0 >= surf->w || y0 >= surf->h) {
                ++rej;
                continue;
            }
            const std::uint32_t tw = static_cast<std::uint32_t>(std::min<std::uint64_t>(
                kTileSize, surf->w - x0));
            const std::uint32_t th = static_cast<std::uint32_t>(std::min<std::uint64_t>(
                kTileSize, surf->h - y0));
            const std::size_t tileBytes = static_cast<std::size_t>(tw) * th * surf->bpp;
            std::vector<std::uint8_t> pixels;
            if (c.payload.empty()) {
                // Clear-to-default: the key was blank at the target state (rgba8 -> transparent 0,
                // a8 mask -> fully visible 255). loadCommittedHistory emits this for a tile that
                // first appeared at a later save, so stepping below it must erase it.
                pixels.assign(tileBytes, surf->bpp == 4 ? 0x00 : 0xFF);
            } else {
                pixels = c.payload;
                const bool filtered = (c.flags & kFlagFiltered) != 0;
                if (filtered && surf->bpp != 4) {
                    ++rej; // the Paeth filter is defined over rgba8 only
                    continue;
                }
                if (filtered)
                    unfilterPaethRgba(pixels, tw, th);
                if (pixels.size() != tileBytes) {
                    ++rej;
                    continue;
                }
            }
            for (std::uint32_t row = 0; row < th; ++row) {
                std::uint8_t* dst =
                    surf->data + (static_cast<std::size_t>(y0 + row) * surf->w + x0) * surf->bpp;
                std::memcpy(dst, pixels.data() + static_cast<std::size_t>(row) * tw * surf->bpp,
                            static_cast<std::size_t>(tw) * surf->bpp);
            }
            bumpLayer(*surf->layer);
        } else if (c.type == kTypeVector) {
            const std::uint64_t lid = detail::loadLe64(c.key.bytes.data());
            core::Layer* layer = doc.find(static_cast<core::LayerId>(lid));
            if (layer == nullptr || c.payload.empty()) {
                ++rej;
                continue;
            }
            const json j = json::parse(c.payload.begin(), c.payload.end(), nullptr, false);
            if (j.is_discarded()) {
                ++rej;
                continue;
            }
            if (auto* v = layer->as<core::VectorLayer>()) {
                auto obj = detail::vectorObjectFromJson(j);
                if (!obj.has_value()) {
                    ++rej;
                    continue;
                }
                v->setObject(std::move(*obj));
            } else if (auto* t = layer->as<core::TextLayer>()) {
                auto block = detail::textBlockFromJson(j);
                if (!block.has_value()) {
                    ++rej;
                    continue;
                }
                t->setBlock(std::move(*block));
            } else {
                ++rej;
            }
        } else if (c.type == kTypePreview) {
            // S48-b hard rule: a PRVW must never reach this function. A preview is a derived
            // artifact, not document content -- loadedStates skips it at the source, so one
            // arriving here is a pipeline bug. Counted (visible to tests), never applied.
            ++rej;
        }
        // Other chunk types (MFST) belong to a structural step -- the whole-tree command's job.
    }
    if (rejected != nullptr)
        *rejected = rej;
}

} // namespace mosaic::io::native
