// macOS implementation of platform/native_window.hpp (PLAN.md S58).
//
// This Objective-C++ translation unit REPLACES native_window.cpp on Apple platforms (see
// platform/CMakeLists.txt): FLTK on macOS is a pure-Cocoa build with no X11/Wayland accessors,
// and the Vulkan WSI surface is VK_EXT_metal_surface, which is fed a CAMetalLayer.
//
// We attach the layer to a dedicated autoresizing subview of the FLTK window's content view --
// the same pattern FLTK's own Cocoa GL driver uses (Fl_Cocoa_Gl_Window_Driver.mm) -- so FLTK's
// FLView is never disturbed. The layer is created once and reused across resizes.

#include "platform/native_window.hpp"

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <FL/Fl_Window.H>
#include <FL/platform.H> // fl_mac_xid() on macOS

namespace mosaic::platform {

void preferWaylandBackendIfUnset() {
    // No windowing-backend choice on macOS -- FLTK is Cocoa-only here.
}

WindowSystem activeBackend() {
    return WindowSystem::Cocoa;
}

int windowBufferScale(Fl_Window* /*win*/) {
    // ⚠ HARD-CODED, the same deliberate pin X11 and Windows use (the reasoning is on
    // NativeSurfaceHandle::scale in native_window.cpp). Cocoa does have a per-window HiDPI factor
    // -- NSWindow.backingScaleFactor, which the canvas reads for its own content scale -- but this
    // value feeds custom-cursor rasterization, and FLTK shows an Fl_RGB_Image cursor here at
    // pixel==point: no HiDPI, so a 2 would draw every chrome cursor at twice its intended size.
    // VulkanCanvas::cursorBuildScale() carries the identical pin for the canvas's own cursors.
    return 1;
}

namespace {

// Our Metal-hosting subview is the one (and only) subview whose backing layer is a CAMetalLayer.
CAMetalLayer* findMetalLayer(NSView* content) {
    for (NSView* sv in [content subviews]) {
        if ([sv.layer isKindOfClass:[CAMetalLayer class]])
            return (CAMetalLayer*)sv.layer;
    }
    return nil;
}

CAMetalLayer* ensureMetalLayer(NSView* content) {
    if (CAMetalLayer* existing = findMetalLayer(content))
        return existing;
    NSView* mv = [[NSView alloc] initWithFrame:[content bounds]];
    [mv setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];
    CAMetalLayer* layer = [CAMetalLayer layer];
    layer.device = MTLCreateSystemDefaultDevice(); // MoltenVK resets this to match the VkPhysicalDevice
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = YES;
    layer.opaque = YES;
    [mv setWantsLayer:YES];
    [mv setLayer:layer];
    [content addSubview:mv];
    return layer;
}

} // namespace

void raiseNativeWindowToTop(Fl_Window* /*win*/) {
    // Nothing to do on macOS: the menus are AppKit's own (Fl_Sys_Menu_Bar draws into the system
    // menu bar, S58-b), and the canvas is a dedicated NSView subview rather than a sibling window.
}

bool nativeSurfaceHandle(Fl_Window* win, NativeSurfaceHandle& out, std::string& error) {
    if (win == nullptr || win->shown() == 0) {
        error = "native handle requested for a window that is not shown";
        return false;
    }
    NSWindow* nsWin = (NSWindow*)fl_mac_xid(win);
    if (nsWin == nil) {
        error = "no Cocoa window for this FLTK window";
        return false;
    }
    NSView* content = [nsWin contentView];
    if (content == nil) {
        error = "Cocoa window has no content view";
        return false;
    }

    CAMetalLayer* layer = ensureMetalLayer(content);
    const CGFloat scale = [nsWin backingScaleFactor] > 0.0 ? [nsWin backingScaleFactor] : 1.0;
    layer.contentsScale = scale;

    out.system = WindowSystem::Cocoa;
    out.display = nullptr;
    out.window = (__bridge void*)layer; // VkMetalSurfaceCreateInfoEXT.pLayer
    out.scale = static_cast<int>(scale + 0.5);
    if (out.scale < 1)
        out.scale = 1;

    // The pixel size comes from FLTK's own w()/h(), NOT from [content bounds].
    //
    // Callers ask for this handle immediately after Fl_Window::resize(), and use the answer to size
    // the swapchain while sizing the canvas VIEWPORT from the same w()/h(). Those two numbers must
    // agree exactly: the present pass lays the document out inside `outSize`, so a swapchain even
    // slightly larger than the viewport it is drawn into puts the picture in a rectangle bigger
    // than the one the user sees, anchored at its top-left corner. Cocoa reconciles a CHILD
    // window's frame on its own schedule (FLTK re-checks subwindow frames from notifications --
    // `checkSubwindowFrame`), so bounds can still describe the previous layout when we ask, and
    // nothing arrives later to correct the mismatch. FLTK's w()/h() is the size that was just SET,
    // which makes it the one both sides can be derived from. [content bounds] stays the fallback
    // for the case FLTK cannot answer.
    out.pixelWidth = win->w() * out.scale;
    out.pixelHeight = win->h() * out.scale;
    if (out.pixelWidth <= 0 || out.pixelHeight <= 0) {
        const NSSize px = [content convertSizeToBacking:[content bounds].size];
        out.pixelWidth = static_cast<int>(px.width);
        out.pixelHeight = static_cast<int>(px.height);
    }
    layer.drawableSize = NSMakeSize(out.pixelWidth, out.pixelHeight);
    return true;
}

} // namespace mosaic::platform
