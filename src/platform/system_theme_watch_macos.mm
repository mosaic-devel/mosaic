#include "platform/system_theme.hpp"

// macOS backend for watchSystemAppearance(): observe the OS light/dark toggle and accent-colour
// change, firing the callback on the main thread so a "System"-mode theme re-resolves live. The
// blocks are enqueued on the main queue, which FLTK's Cocoa run loop drains. (Linux has its own
// sd-bus backend.)

#import <AppKit/AppKit.h>

#include <functional>
#include <utility>

namespace mosaic::platform {

void watchSystemAppearance(std::function<void()> onChange) {
    // Leaked on purpose: the subscription lives for the whole process (there is no unsubscribe).
    auto* cb = new std::function<void()>(std::move(onChange));
    void (^fire)(NSNotification*) = ^(NSNotification*) {
        if (cb != nullptr && *cb)
            (*cb)();
    };
    // Light/dark toggle arrives as a distributed notification; accent-colour changes as a
    // system-colours notification. Deliver both on the main queue (where applyTheme is safe).
    [[NSDistributedNotificationCenter defaultCenter]
        addObserverForName:@"AppleInterfaceThemeChangedNotification"
                    object:nil
                     queue:[NSOperationQueue mainQueue]
                usingBlock:fire];
    [[NSNotificationCenter defaultCenter]
        addObserverForName:NSSystemColorsDidChangeNotification
                    object:nil
                     queue:[NSOperationQueue mainQueue]
                usingBlock:fire];
}

} // namespace mosaic::platform
