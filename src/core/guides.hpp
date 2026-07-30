#pragma once

#include <cstdint>

// Document guides (View -> Show Guides): draggable horizontal / vertical reference lines stored per
// document, in document coordinates. A pure model type shared by the Document store, the undoable
// guide commands (commands.hpp), the snapping engine (snap.hpp), and the canvas overlay. FLTK-free.
namespace mosaic::core {

struct Guide {
    enum class Orientation : std::uint8_t { Horizontal, Vertical };

    // A Horizontal guide is a line of constant Y (it spans the document left-to-right); a Vertical
    // guide is a line of constant X. `position` is that constant coordinate, in document px.
    Orientation orientation = Orientation::Horizontal;
    double position = 0.0;
    // Stable per-document id (Document::mintGuideId), so move / remove / undo can address a guide
    // without relying on its index (which shifts as guides are added and removed).
    std::uint64_t id = 0;

    [[nodiscard]] bool horizontal() const noexcept { return orientation == Orientation::Horizontal; }

    friend bool operator==(const Guide&, const Guide&) = default;
};

} // namespace mosaic::core
