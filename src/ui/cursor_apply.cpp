#include "ui/cursor_apply.hpp"

#include "platform/native_window.hpp" // activeBackend / windowBufferScale
#include "ui/theme.hpp"               // activePalette

#include <FL/Fl_RGB_Image.H>
#include <FL/Fl_Window.H>
#include <FL/Enumerations.H>

namespace mosaic::ui {

std::unique_ptr<Fl_RGB_Image> makeCursorImage(const CursorImage& c) {
    if (c.image.rgba.empty() || c.logicalW <= 0 || c.logicalH <= 0)
        return nullptr;
    auto img = std::make_unique<Fl_RGB_Image>(c.image.rgba.data(), static_cast<int>(c.image.width),
                                              static_cast<int>(c.image.height), 4);
    img->scale(c.logicalW, c.logicalH, /*proportional=*/0, /*can_expand=*/1);
    return img;
}

double chromeCursorScale(Fl_Window* win) {
    const int scale = platform::windowBufferScale(win);
    return scale > 0 ? static_cast<double>(scale) : 1.0;
}

MoveCursor::MoveCursor() = default;
MoveCursor::~MoveCursor() = default;

void MoveCursor::reset() noexcept {
    m_image.reset();
    m_scale = 0.0;
}

void MoveCursor::apply(Fl_Window* win, bool darkMode, double buildScale) {
    if (win == nullptr)
        return;
    if (platform::activeBackend() != platform::WindowSystem::Wayland) {
        win->cursor(FL_CURSOR_MOVE); // X11 already resolves this to the four-way XC_fleur
        return;
    }
    if (!m_image || darkMode != m_dark || buildScale != m_scale) {
        m_pixels = moveCursor(darkMode, buildScale);
        m_dark = darkMode;
        m_scale = buildScale;
        m_image = makeCursorImage(m_pixels);
    }
    if (!m_image) {
        m_scale = 0.0;               // build failed: never cache the miss
        win->cursor(FL_CURSOR_MOVE); // ... and fall back to whatever the theme does have
        return;
    }
    win->cursor(m_image.get(), m_pixels.logicalHotX, m_pixels.logicalHotY);
}

void MoveCursor::apply(Fl_Window* win) {
    if (win == nullptr)
        return;
    apply(win, activePalette().dark, chromeCursorScale(win));
}

} // namespace mosaic::ui
