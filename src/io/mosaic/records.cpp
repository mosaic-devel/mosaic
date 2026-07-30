#include "io/mosaic/records.hpp"

#include "io/mosaic/naming.hpp"

#include <nlohmann/json.hpp>

namespace mosaic::io::native {
namespace {

using nlohmann::json;

[[nodiscard]] json parsePayload(std::span<const std::uint8_t> payload) {
    return json::parse(payload.begin(), payload.end(), nullptr, /*allow_exceptions=*/false);
}

} // namespace

std::string histRecordJson(const HistRecord& rec, const std::string& extraJson) {
    // Extras first, canonical fields on top: a document-layer field can never shadow the
    // container's state/parent/dirty semantics.
    json j = json::object();
    if (!extraJson.empty()) {
        json extra = json::parse(extraJson, nullptr, /*allow_exceptions=*/false);
        if (extra.is_object())
            j = std::move(extra);
    }
    j["state"] = rec.state;
    j["parent"] = rec.parent;
    json dirty = json::array();
    for (std::size_t i = 0; i < rec.dirty.size(); ++i) {
        const DirtyKey& d = rec.dirty[i];
        json e{{"t", detail::tagToString(d.type)}, {"k", detail::keyToHex(d.key)}};
        // The cas-mode reference (spec 3.9) rides inside its own entry -- never a parallel
        // array, so a reader can never mispair a hash with a key.
        if (i < rec.refs.size() && rec.refs[i].present) {
            e["b"] = detail::bytesToHex(rec.refs[i].hash);
            e["f"] = rec.refs[i].flags;
        }
        dirty.push_back(std::move(e));
    }
    j["dirty"] = std::move(dirty);
    return j.dump();
}

std::optional<HistRecord> parseHistRecord(std::span<const std::uint8_t> payload) {
    const json j = parsePayload(payload);
    if (j.is_discarded() || !j.is_object() || !j.contains("state") || !j["state"].is_number() ||
        !j.contains("dirty") || !j["dirty"].is_array())
        return std::nullopt;
    HistRecord rec;
    rec.state = j["state"].get<std::uint64_t>();
    rec.parent = j.value("parent", std::uint64_t{0});
    bool anyRef = false;
    for (const json& e : j["dirty"]) {
        if (!e.is_object())
            return std::nullopt;
        const auto tag = detail::tagFromString(e.value("t", std::string{}));
        const auto key = detail::keyFromHex(e.value("k", std::string{}));
        if (!tag.has_value() || !key.has_value())
            return std::nullopt; // a dirty list we cannot read exactly is not a dirty list
        rec.dirty.push_back({*tag, *key});
        BlobRef ref;
        if (e.contains("b")) {
            const std::string hex = e.value("b", std::string{});
            if (hex.size() != ref.hash.size() * 2)
                return std::nullopt; // a reference we cannot read exactly is not a reference
            for (std::size_t i = 0; i < ref.hash.size(); ++i) {
                const int hi = detail::hexNibble(hex[i * 2]);
                const int lo = detail::hexNibble(hex[i * 2 + 1]);
                if (hi < 0 || lo < 0)
                    return std::nullopt;
                ref.hash[i] = static_cast<std::uint8_t>((hi << 4) | lo);
            }
            ref.flags = static_cast<std::uint8_t>(e.value("f", 0u));
            ref.present = true;
            anyRef = true;
        }
        rec.refs.push_back(ref);
    }
    if (!anyRef)
        rec.refs.clear(); // the H2 spelling: no refs at all, not a list of absences
    return rec;
}

std::string histExtrasJson(std::span<const std::uint8_t> payload) {
    json j = parsePayload(payload);
    if (j.is_discarded() || !j.is_object())
        return {};
    j.erase("state");
    j.erase("parent");
    j.erase("dirty");
    return j.empty() ? std::string{} : j.dump();
}

std::string cmitRecordJson(const CmitRecord& rec) {
    return json{{"saved_state", rec.savedState}, {"batch_states", rec.batchStates}}.dump();
}

std::optional<CmitRecord> parseCmitRecord(std::span<const std::uint8_t> payload) {
    const json j = parsePayload(payload);
    if (j.is_discarded() || !j.is_object() || !j.contains("saved_state") ||
        !j["saved_state"].is_number() || !j.contains("batch_states") ||
        !j["batch_states"].is_array())
        return std::nullopt;
    CmitRecord rec;
    rec.savedState = j["saved_state"].get<std::uint64_t>();
    for (const json& s : j["batch_states"]) {
        if (!s.is_number())
            return std::nullopt;
        rec.batchStates.push_back(s.get<std::uint64_t>());
    }
    return rec;
}

} // namespace mosaic::io::native
