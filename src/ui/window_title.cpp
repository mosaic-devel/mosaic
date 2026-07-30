#include "ui/window_title.hpp"

#include <string>

namespace mosaic::ui {

std::string formatUnsavedDuration(int seconds, bool includeSeconds, std::string_view minuteUnit,
                                  std::string_view secondUnit) {
    if (seconds < 0)
        seconds = 0;
    const int minutes = seconds / 60;
    const auto unit = [](int n, std::string_view u) {
        return std::to_string(n) + " " + std::string(u);
    };
    if (!includeSeconds)
        return unit(minutes, minuteUnit);
    const int secs = seconds % 60;
    if (minutes > 0)
        return unit(minutes, minuteUnit) + " " + unit(secs, secondUnit);
    return unit(secs, secondUnit);
}

std::string displayDocumentName(std::string_view fileName) {
    static constexpr std::string_view kExt = ".mosaic";
    if (fileName.size() > kExt.size()) {
        const std::string_view tail = fileName.substr(fileName.size() - kExt.size());
        bool match = true;
        for (std::size_t i = 0; i < kExt.size(); ++i)
            if ((tail[i] | 0x20) != kExt[i]) { // ASCII lower; kExt is lowercase
                match = false;
                break;
            }
        if (match)
            return std::string(fileName.substr(0, fileName.size() - kExt.size()));
    }
    return std::string(fileName);
}

std::string formatWindowTitle(std::string_view docName, bool dirty, int unsavedSeconds,
                              const UnsavedTitleFormat& fmt) {
    // The document name leads (taskbars/alt-tab truncate from the right); "Mosaic" trails. Bullet
    // (U+2022) and em dash (U+2014) are the fixed chrome, exactly as the S18-d spec lays it out.
    std::string title(docName.empty() ? fmt.untitled : docName);
    if (dirty) {
        title += " \xE2\x80\xA2 "; // " • "
        title += fmt.unsaved;
        if (fmt.showDuration && unsavedSeconds >= 0 && unsavedSeconds >= fmt.thresholdSeconds) {
            title += " " + std::string(fmt.durationFor) + " " +
                     formatUnsavedDuration(unsavedSeconds, fmt.includeSeconds, fmt.minutes,
                                           fmt.seconds);
        }
    }
    title += " \xE2\x80\x94 Mosaic"; // " — Mosaic"
    return title;
}

} // namespace mosaic::ui
