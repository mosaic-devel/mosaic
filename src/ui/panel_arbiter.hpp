#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

// The unified corner-panel model (S32 round 5; docs/adjustment-layers.md §5). Mosaic's pinned
// corner popovers -- the Type "Style…"/"3D…" panels, the adjustment editor, and whatever joins
// them -- all share ONE canvas corner, and every panel that hand-rolled its own show/hide/
// exclusion/reopen logic bred a fresh crop of fights (buried windows, stolen focus, latched
// buttons; user 2026-07-17: "every panel we add in the same manner just brings in more and more
// issues"). This class is the single decision core: it owns WHICH panel should be visible.
//
// Two trigger kinds, per the user's model:
//   * EXPLICIT -- opened by a user action (a bar button): `toggle(id)` requests/dismisses it,
//     and a `valid()` predicate lets it evaporate when its context dies (the text session ends,
//     its anchor button is rebuilt away). At most one explicit request exists; toggling another
//     explicit panel replaces it (the Style/3D mutual exclusion, for free).
//   * CONDITIONAL -- open because the app is in a state ("the active layer is an adjustment"):
//     `wants()` returns a nonzero CONTEXT TOKEN (e.g. the layer id) while the panel should be
//     up, 0 otherwise. `dismiss(id)` (the user Esc-closed it) suppresses it FOR THAT TOKEN --
//     it stays away until the token changes (another layer, or the condition lapsing), which
//     re-arms it. That is exactly the "Esc keeps it closed until I re-select" behavior.
//
// Resolution: a live, valid explicit request always wins; otherwise the highest-precedence
// unsuppressed conditional whose token is nonzero. The QUEUE the user asked for falls out:
// close "Style…" and resolve() lands back on the adjustment panel because its condition still
// holds. Pure logic, no FLTK -- the widget glue (showing, hiding, focus policy, reconciling
// external hides) lives in the host's sync step, and this class stays headless-testable.
namespace mosaic::ui {

class PanelArbiter {
public:
    using Id = int;

    // Register an EXPLICIT (button-toggled) panel. `valid` may be empty (= always valid while
    // requested); when it returns false the request evaporates on the next resolve().
    void addExplicit(Id id, std::function<bool()> valid = {});
    // Register a CONDITIONAL panel. `wants` returns the context token (nonzero = wants to be
    // open); higher `precedence` wins among conditional panels that want open simultaneously.
    void addConditional(Id id, std::function<std::uint64_t()> wants, int precedence = 0);

    // A user action on an explicit panel's trigger: open it (replacing any other explicit
    // request), or close it if it is the current request.
    void toggle(Id id);
    // The panel was hidden outside this class's control (Esc, a theme close, a bar rebuild).
    // Explicit: the request is cleared. Conditional: suppressed for its CURRENT token.
    void hidden(Id id);

    // The panel that should be visible now, or nullopt. Also expires invalid explicit requests
    // and re-arms suppressed conditionals whose token changed. Call once per frame (and after
    // any toggle()/hidden(), for same-event responsiveness).
    [[nodiscard]] std::optional<Id> resolve();

private:
    struct ExplicitEntry {
        Id id;
        std::function<bool()> valid;
    };
    struct ConditionalEntry {
        Id id;
        std::function<std::uint64_t()> wants;
        int precedence;
        std::uint64_t suppressedToken = 0; // nonzero: stay hidden while wants() == this token
    };
    std::vector<ExplicitEntry> m_explicit;
    std::vector<ConditionalEntry> m_conditional;
    std::optional<Id> m_request; // the live explicit request, if any
};

} // namespace mosaic::ui
