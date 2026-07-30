// Smart Resize crediting record — see credits.hpp for why this lives in core.

#include "core/retarget/credits.hpp"

namespace mosaic::core::retarget {

const RetargetInfo& retargetInfo() {
    static const RetargetInfo info = [] {
        RetargetInfo i;
        i.method = "Content-aware crop & rigid recompose";
        i.lineage = "Suh, Ling, Bederson & Jacobs 2003 · Setlur, Takagi, Raskar, Gleicher & "
                    "Gooch 2005 · Rubinstein, Gutierrez, Sorkine & Shamir 2010";
        i.cost = "Analysis runs in milliseconds on a downsampled map; Recompose heals through "
                 "the Inpainting engine";
        i.summary =
            "Smart Resize scores the picture with one fused importance map (gradient energy, "
            "edge density, a centre prior), suggests the single best crop for the target ratio, "
            "and shows what it is keeping as clickable chips. When everything marked cannot fit "
            "any crop, Recompose moves the subjects closer instead: each is cut out with a loose "
            "feathered margin, the holes are healed, the background is cropped to the new frame, and the subjects "
            "are placed back rigidly — nothing that matters is ever deformed.";
        i.sections = {
            {"Built on (published work)",
             {
                 "Importance-driven automatic cropping — Suh, Ling, Bederson & Jacobs, "
                 "“Automatic Thumbnail Cropping and its Effectiveness” (UIST 2003); "
                 "Chen et al. 2003; Liu & Gleicher 2006.",
                 "Cropping as the operator people actually prefer — Rubinstein, Gutierrez, "
                 "Sorkine & Shamir, “A Comparative Study of Image Retargeting” "
                 "(SIGGRAPH Asia 2010): the RetargetMe benchmark ranked simple, artifact-free "
                 "operators above deforming ones. That finding is why Smart Resize exists in "
                 "this form.",
                 "Rigid recomposition — Setlur, Takagi, Raskar, Gleicher & Gooch, “Automatic "
                 "Image Retargeting” (MUM 2005): cut the subjects, retarget the background, "
                 "re-place them preserving their relative arrangement. Mosaic rebuilds the 2005 "
                 "architecture from its own parts.",
                 "Hole healing and seam blending — the Inpainting engine (Settings → Inpainting "
                 "carries its own credits).",
             }},
            {"Deliberately not done",
             {
                 "No seam carving — the benchmark ranked it last of the compared methods.",
                 "No warping or non-uniform scaling — visible deformation was the benchmark's "
                 "top complaint.",
                 "No learned aesthetics — the suggestion comes from classic composition rules, "
                 "not a model trained on “pleasing” crops.",
                 "No automatic operator switching — Recompose is offered when a crop cannot keep "
                 "everything, but only you invoke it.",
             }},
        };
        i.footnotes = {
            "Design notes: docs/smart-resize-research.md and "
            "docs/smart-recompose-plan.md in the Mosaic source tree.",
        };
        return i;
    }();
    return info;
}

} // namespace mosaic::core::retarget
