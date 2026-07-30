#include "ui/motivation_ticker.hpp"

#include "ui/menu_bar.hpp"
#include "ui/motivational_lines.hpp"

#include <FL/Fl.H>

#include <random>

namespace mosaic::ui {
namespace {

// Cadence (docs/motivational-ticker.md; carried over verbatim from the deleted GPU driver). The
// first line appears soon after enabling so the toggle's effect is discoverable, then it settles
// into a rare 2-5 min gap. Each spawned line now has a short ON-SCREEN life of its own -- it
// slides down into the bar, holds (kHoldSeconds), then slides up and out -- so between lines the bar
// is simply empty; the cadence just decides WHEN the next one arrives.
constexpr double kFirstMin = 30.0; // first appearance after enabling
constexpr double kFirstMax = 90.0;
constexpr double kGapMin = 120.0;  // 2 min: shortest wait between lines
constexpr double kGapMax = 300.0;  // 5 min: longest
constexpr double kHoldSeconds = 10.0;  // how long a line sits fully visible before it slides back out

// Uniform random double in [lo, hi) from a lazily-seeded engine (FLTK is single-threaded here).
double randRange(double lo, double hi) {
    static std::mt19937 rng{std::random_device{}()};
    return std::uniform_real_distribution<double>(lo, hi)(rng);
}

} // namespace

MotivationTicker::MotivationTicker(MenuBar* menu) : m_menu(menu) {}

MotivationTicker::~MotivationTicker() {
    if (m_timerArmed)
        Fl::remove_timeout(onTimeout, this);
}

void MotivationTicker::setEnabled(bool on) {
    if (on == m_enabled)
        return;
    m_enabled = on;
    if (on) {
        schedule(randRange(kFirstMin, kFirstMax));
    } else {
        if (m_timerArmed) {
            Fl::remove_timeout(onTimeout, this);
            m_timerArmed = false;
        }
        if (m_menu != nullptr) // drop the current line so a re-enable starts fresh
            m_menu->clearTicker();
    }
}

void MotivationTicker::schedule(double delaySeconds) {
    Fl::remove_timeout(onTimeout, this); // never stack two timers on us
    Fl::add_timeout(delaySeconds, onTimeout, this);
    m_timerArmed = true;
}

void MotivationTicker::onTimeout(void* self) {
    auto* t = static_cast<MotivationTicker*>(self);
    t->m_timerArmed = false;
    t->spawn();
    t->schedule(randRange(kGapMin, kGapMax)); // the next, rare line
}

void MotivationTicker::spawn() {
    if (m_menu != nullptr) // slide the fresh line down into the bar, hold, then slide it out
        m_menu->showTickerLine(randomMotivationalLine(), kHoldSeconds);
}

} // namespace mosaic::ui
