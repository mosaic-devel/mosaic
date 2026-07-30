#include "core/command.hpp"
#include "core/commands.hpp"
#include "core/document.hpp"
#include "core/layer.hpp"

#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

// LoadedStateCommand + CommandStack::adoptHistory (S48): a .mosaic's committed save history is
// loaded into the undo stack pre-applied, so the History panel shows it and can jump between saved
// states. Each step owns the whole layer tree of the state below it and swaps it with the live
// tree; this pins that the walk lands on the right state, both directions, and the saved marker.
namespace {

using namespace mosaic::core;

std::vector<std::unique_ptr<Layer>> layersNamed(Document& src,
                                                const std::vector<std::string>& names) {
    std::vector<std::unique_ptr<Layer>> v;
    for (const auto& n : names)
        v.push_back(src.makeRaster(n, 8, 8));
    return v;
}

std::vector<std::string> treeNames(const Document& doc) {
    std::vector<std::string> out;
    for (const auto& c : doc.root().children())
        out.push_back(c->name());
    return out;
}

} // namespace

TEST_CASE("loaded history walks the document between saved states, both directions") {
    Document doc(8, 8);
    doc.root().addOnTop(doc.makeRaster("tip0", 8, 8));
    doc.root().addOnTop(doc.makeRaster("tip1", 8, 8));
    doc.root().addOnTop(doc.makeRaster("tip2", 8, 8));

    // Two earlier saved states, oldest first: a bare "s0" and a two-layer "s1".
    Document scratch(8, 8);
    std::vector<std::unique_ptr<Command>> history;
    history.push_back(std::make_unique<LoadedStateCommand>("Edited s0", layersNamed(scratch, {"s0"})));
    history.push_back(
        std::make_unique<LoadedStateCommand>("Edited s1", layersNamed(scratch, {"s1a", "s1b"})));
    doc.commands().adoptHistory(std::move(history));

    // Adopted pre-applied: the document still shows its newest state and is clean there.
    CHECK(doc.commands().size() == 2);
    CHECK(doc.commands().position() == 2);
    CHECK(doc.commands().isSaved());
    CHECK(doc.commands().nameAt(0) == "Edited s0");
    CHECK(doc.commands().nameAt(1) == "Edited s1");
    CHECK(treeNames(doc) == std::vector<std::string>{"tip0", "tip1", "tip2"});

    SUBCASE("jump to the baseline shows the oldest state") {
        doc.commands().jumpTo(0);
        CHECK(treeNames(doc) == std::vector<std::string>{"s0"});
        CHECK_FALSE(doc.commands().isSaved()); // moved off the saved position
    }
    SUBCASE("jump to a middle state shows exactly that state") {
        doc.commands().jumpTo(1);
        CHECK(treeNames(doc) == std::vector<std::string>{"s1a", "s1b"});
    }
    SUBCASE("stepping down then back up restores the tip exactly (pure inverse)") {
        doc.commands().jumpTo(0);
        doc.commands().jumpTo(2);
        CHECK(treeNames(doc) == std::vector<std::string>{"tip0", "tip1", "tip2"});
        CHECK(doc.commands().isSaved()); // back at the saved tip
    }
    SUBCASE("single undo/redo is symmetric") {
        doc.commands().undo();
        CHECK(treeNames(doc) == std::vector<std::string>{"s1a", "s1b"});
        doc.commands().redo();
        CHECK(treeNames(doc) == std::vector<std::string>{"tip0", "tip1", "tip2"});
    }
}
