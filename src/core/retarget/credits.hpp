#pragma once

// Smart Resize — the in-app crediting record ("About Smart Resize", docs/smart-resize-research.md
// §7; docs/smart-recompose-plan.md §5 commit 6).
//
// Attribution is a first-class feature here, mirroring the Inpainting engine's BackendInfo spec
// sheet: the published lineage the feature is built from, and — honestly — what it deliberately
// does not do and why. The Settings → Tools → Crop pane renders this verbatim; the strings live
// in core next to the code they credit.
//
// Face detection note: an F1 Viola-Jones face tier was scoped (§4.2 of the research doc) and
// DROPPED 2026-07-02 — shipping cascade data wasn't worth the marginal chip quality — so no
// detector is credited. The importance map alone decides.

#include <string>
#include <vector>

namespace mosaic::core::retarget {

// One titled bullet list of the "About Smart Resize" sheet.
struct RetargetInfoSection {
    std::string title;
    std::vector<std::string> items;
};

// The whole sheet, shaped like inpaint::BackendInfo so the same Settings widget can draw both.
struct RetargetInfo {
    std::string method;    // bold header line
    std::string lineage;   // authors line (muted, under the header)
    std::string cost;      // perf note line (muted)
    std::string summary;   // one friendly paragraph
    std::vector<RetargetInfoSection> sections;
    std::vector<std::string> footnotes; // small muted lines at the bottom
};

// The static record. English on purpose (paper titles + author names don't translate); the
// section titles are the only UI-ish strings and the caller may localize them if ever needed.
[[nodiscard]] const RetargetInfo& retargetInfo();

} // namespace mosaic::core::retarget
