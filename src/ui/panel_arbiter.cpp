#include "ui/panel_arbiter.hpp"

#include <algorithm>

namespace mosaic::ui {

void PanelArbiter::addExplicit(Id id, std::function<bool()> valid) {
    m_explicit.push_back({id, std::move(valid)});
}

void PanelArbiter::addConditional(Id id, std::function<std::uint64_t()> wants, int precedence) {
    m_conditional.push_back({id, std::move(wants), precedence, 0});
}

void PanelArbiter::toggle(Id id) {
    if (m_request == id) {
        m_request.reset(); // re-triggering the open panel closes it
        return;
    }
    m_request = id; // replaces any other explicit request (the shared-corner exclusivity)
}

void PanelArbiter::hidden(Id id) {
    if (m_request == id) {
        m_request.reset();
        return;
    }
    for (ConditionalEntry& e : m_conditional) {
        if (e.id != id)
            continue;
        // Suppress for the CURRENT context: the panel stays away until wants() answers with a
        // different token (another layer, or the condition lapsing and re-arming).
        e.suppressedToken = e.wants ? e.wants() : 0;
        return;
    }
}

std::optional<PanelArbiter::Id> PanelArbiter::resolve() {
    if (m_request) {
        const auto it = std::find_if(m_explicit.begin(), m_explicit.end(),
                                     [&](const ExplicitEntry& e) { return e.id == *m_request; });
        if (it != m_explicit.end() && (!it->valid || it->valid()))
            return m_request;
        m_request.reset(); // unknown id or its context died (session ended, anchor gone)
    }
    const ConditionalEntry* best = nullptr;
    std::uint64_t bestToken = 0;
    for (ConditionalEntry& e : m_conditional) {
        const std::uint64_t token = e.wants ? e.wants() : 0;
        if (e.suppressedToken != 0 && token != e.suppressedToken)
            e.suppressedToken = 0; // the context moved on: re-arm
        if (token == 0 || token == e.suppressedToken)
            continue;
        if (best == nullptr || e.precedence > best->precedence) {
            best = &e;
            bestToken = token;
        }
    }
    (void)bestToken;
    if (best != nullptr)
        return best->id;
    return std::nullopt;
}

} // namespace mosaic::ui
