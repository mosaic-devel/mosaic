// The macOS half of the .mosaic thumbnailer (S58-e): a Quick Look THUMBNAIL extension, so Finder,
// the Open panel, Spotlight and the Get Info window all show a document's own picture instead of a
// generic icon. The Linux half is two separate mechanisms (a freedesktop thumbnailer binary and a
// KIO plugin, src/thumbnailer/); this is the third, and like both of those it reads ONLY the
// newest PRVW chunk -- a verified linear scan that decompresses no tile content -- so it links
// mosaic_io + mosaic_common and nothing else. No FLTK, no Vulkan, no display.
//
// WHY AN EXTENSION AND NOT A BINARY. macOS has no "spawn a helper per file" thumbnailer protocol;
// since 10.15 the supported path is a QLThumbnailProvider app extension, an .appex bundle inside
// the host app's Contents/PlugIns. The system registers it through LaunchServices when the app is
// installed, then runs it out-of-process, sandboxed, handing it one file URL at a time. That is why
// this file has a main() at all: an extension's executable is expected to hand control straight to
// Foundation's NSExtensionMain, which reads NSExtensionPrincipalClass out of the bundle's Info.plist
// and instantiates it.
//
// A file that predates previews (pre-S48-b) has no PRVW, and compositing one here would mean
// linking the document model plus a compositor into an extension -- exactly the dependency the
// design forbids. The deliberate answer matches the Linux binary: no preview, no thumbnail, and
// the system falls back to the document icon. One Save in a current Mosaic fixes a file forever.
//
// ⚠ Compile/link-verified only; there is no Mac in the loop. The runtime unknowns are collected at
// the end of packaging/macos/README.md -- chiefly whether an ad-hoc-signed extension in a
// non-notarized app is loaded, which is a LaunchServices/pluginkit question, not a code one.

#include "common/image.hpp"
#include "io/mosaic/fileinfo.hpp"
#include "io/mosaic/preview.hpp"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <QuickLookThumbnailing/QuickLookThumbnailing.h>
#import <QuickLookUI/QuickLookUI.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace {

// The straight-alpha RGBA the preview reader hands back, wrapped as a CGImage. Core Graphics has no
// straight-alpha bitmap format for drawing, so the pixels are PREMULTIPLIED here -- doing it any
// later would mean asking CG to composite something it would read as too bright wherever the
// document is transparent.
CGImageRef createCGImage(const mosaic::common::Image& img) {
    const std::size_t w = img.width;
    const std::size_t h = img.height;
    if (w == 0 || h == 0 || img.rgba.size() < w * h * 4)
        return nullptr;

    NSMutableData* data = [NSMutableData dataWithLength:w * h * 4];
    auto* dst = static_cast<std::uint8_t*>([data mutableBytes]);
    const std::uint8_t* src = img.rgba.data();
    for (std::size_t i = 0; i < w * h; ++i) {
        const std::uint32_t a = src[4 * i + 3];
        dst[4 * i + 0] = static_cast<std::uint8_t>((src[4 * i + 0] * a + 127) / 255);
        dst[4 * i + 1] = static_cast<std::uint8_t>((src[4 * i + 1] * a + 127) / 255);
        dst[4 * i + 2] = static_cast<std::uint8_t>((src[4 * i + 2] * a + 127) / 255);
        dst[4 * i + 3] = static_cast<std::uint8_t>(a);
    }

    CGDataProviderRef provider =
        CGDataProviderCreateWithCFData(static_cast<CFDataRef>(data));
    if (provider == nullptr)
        return nullptr;
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    // The two halves of the bitmap-info word live in different (anonymous) enums, so ORing them
    // directly is a deprecated enum-enum conversion under -Werror. Build the value as the
    // CGBitmapInfo it is.
    const CGBitmapInfo bitmapInfo = static_cast<CGBitmapInfo>(kCGBitmapByteOrderDefault) |
                                    static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast);
    CGImageRef image = CGImageCreate(w, h, 8, 32, w * 4, space, bitmapInfo, provider, nullptr,
                                     /*shouldInterpolate=*/true, kCGRenderingIntentDefault);
    CGColorSpaceRelease(space);
    CGDataProviderRelease(provider);
    return image;
}

// The preview's caption: what the file is, from the manifest alone. readDocumentInfo decompresses
// only that chunk, so this costs no more than the PRVW read beside it. Empty when the manifest does
// not parse -- the picture alone is still a useful preview.
NSString* captionForFile(const std::string& path) {
    const auto info = mosaic::io::native::readDocumentInfo(path);
    if (!info.has_value() || info->width == 0 || info->height == 0)
        return nil;
    NSMutableString* s = [NSMutableString stringWithFormat:@"%u × %u px", info->width, info->height];
    if (info->dpi > 0.0)
        [s appendFormat:@" · %g dpi", info->dpi];
    return s;
}

} // namespace

// The principal class named by NSExtensionPrincipalClass in the extension's Info.plist. The name is
// part of that contract: rename it here and the extension loads as an empty stub.
API_AVAILABLE(macos(10.15))
@interface MosaicThumbnailProvider : QLThumbnailProvider
@end

@implementation MosaicThumbnailProvider

- (void)provideThumbnailForFileRequest:(QLFileThumbnailRequest*)request
                     completionHandler:
                         (void (^)(QLThumbnailReply* _Nullable, NSError* _Nullable))handler {
    NSURL* url = [request fileURL];
    if (url == nil || ![url isFileURL]) {
        handler(nil, nil); // not a local file: no thumbnail, no error worth surfacing
        return;
    }

    std::string err;
    const auto preview =
        mosaic::io::native::readNewestPreview(std::string([[url path] UTF8String]), &err);
    if (!preview.has_value()) {
        handler(nil, nil); // a pre-preview file is not an error: fall back to the document icon
        return;
    }

    // Fit the preview inside the requested box, never upscaling past its own size -- the PRVW is
    // 256 px on its longest edge, and a blown-up 256 px thumbnail in a 512 px slot looks worse than
    // a small sharp one. Core Graphics scales the CGImage into whatever context size we ask for, so
    // the aspect ratio has to be honoured here rather than left to the drawing block.
    const CGSize maxSize = [request maximumSize];
    const double srcW = static_cast<double>(preview->width);
    const double srcH = static_cast<double>(preview->height);
    if (srcW <= 0.0 || srcH <= 0.0) {
        handler(nil, nil);
        return;
    }
    const double fit = std::min({maxSize.width / srcW, maxSize.height / srcH, 1.0});
    const CGSize contextSize =
        CGSizeMake(std::max(1.0, std::floor(srcW * fit)), std::max(1.0, std::floor(srcH * fit)));

    CGImageRef image = createCGImage(*preview);
    if (image == nullptr) {
        handler(nil, nil);
        return;
    }

    // The block outlives this method (Quick Look calls it on its own schedule), so the CGImage is
    // handed over to it and released inside -- the block runs exactly once per reply.
    handler([QLThumbnailReply replyWithContextSize:contextSize
                                      drawingBlock:^BOOL(CGContextRef context) {
                                        CGContextSetInterpolationQuality(context, kCGInterpolationHigh);
                                        CGContextDrawImage(
                                            context,
                                            CGRectMake(0, 0, contextSize.width, contextSize.height),
                                            image);
                                        CGImageRelease(image);
                                        return YES;
                                      }],
            nil);
}

@end

// ---- the space-bar preview -------------------------------------------------------------------
//
// A SECOND extension point (com.apple.quicklook.preview), and therefore a second .appex bundle:
// NSExtensionPointIdentifier is single-valued, so one bundle cannot serve both. Both bundles embed
// this same binary and differ only in which principal class their plist names -- the two providers
// share the PRVW reader, and a second copy of a 1 MB executable is cheaper than a second build.
//
// It shows the SAME embedded preview the thumbnail does, captioned with the canvas size read from
// the manifest. It does not composite the document: that would mean linking the layer stack and a
// compositor into an extension, which is the dependency this whole family exists to avoid. So the
// preview is as sharp as the stored PRVW (256 px on its longest edge) and no sharper -- Quick Look
// scales the reply to its panel. Sizing the context to the picture's own pixels is what tells it
// the aspect ratio and keeps the scaling honest rather than inventing detail.
API_AVAILABLE(macos(12.0))
@interface MosaicPreviewProvider : QLPreviewProvider <QLPreviewingController>
@end

@implementation MosaicPreviewProvider

- (void)providePreviewForFileRequest:(QLFilePreviewRequest*)request
                   completionHandler:(void (^)(QLPreviewReply* _Nullable, NSError* _Nullable))handler {
    NSURL* url = [request fileURL];
    if (url == nil || ![url isFileURL]) {
        handler(nil, nil);
        return;
    }
    const std::string path([[url path] UTF8String]);

    std::string err;
    const auto preview = mosaic::io::native::readNewestPreview(path, &err);
    if (!preview.has_value()) {
        // No PRVW (a file older than S48-b): decline, and Quick Look shows its own "no preview"
        // face rather than an error.
        handler(nil, nil);
        return;
    }
    CGImageRef image = createCGImage(*preview);
    if (image == nullptr) {
        handler(nil, nil);
        return;
    }

    NSString* caption = captionForFile(path);
    const CGFloat imgW = static_cast<CGFloat>(preview->width);
    const CGFloat imgH = static_cast<CGFloat>(preview->height);
    const CGFloat captionH = caption != nil ? 28.0 : 0.0;
    const CGSize contextSize = CGSizeMake(std::max(imgW, 220.0), imgH + captionH);

    QLPreviewReply* reply = [[QLPreviewReply alloc]
        initWithContextSize:contextSize
                   isBitmap:YES
               drawingBlock:^BOOL(CGContextRef context, QLPreviewReply* r, NSError** e) {
                 (void)r;
                 (void)e;
                 // The context is bottom-left origin, so the picture sits ABOVE the caption strip.
                 CGContextSetGrayFillColor(context, 0.11, 1.0);
                 CGContextFillRect(context, CGRectMake(0, 0, contextSize.width, contextSize.height));
                 CGContextSetInterpolationQuality(context, kCGInterpolationHigh);
                 CGContextDrawImage(
                     context,
                     CGRectMake((contextSize.width - imgW) / 2, captionH, imgW, imgH), image);
                 CGImageRelease(image);
                 if (caption != nil) {
                     NSGraphicsContext* prev = [NSGraphicsContext currentContext];
                     [NSGraphicsContext setCurrentContext:
                         [NSGraphicsContext graphicsContextWithCGContext:context flipped:NO]];
                     NSDictionary* attrs = @{
                         NSFontAttributeName : [NSFont systemFontOfSize:11],
                         NSForegroundColorAttributeName : [NSColor colorWithWhite:0.72 alpha:1.0]
                     };
                     const NSSize sz = [caption sizeWithAttributes:attrs];
                     [caption drawAtPoint:NSMakePoint((contextSize.width - sz.width) / 2,
                                                      (captionH - sz.height) / 2)
                           withAttributes:attrs];
                     [NSGraphicsContext setCurrentContext:prev];
                 }
                 return YES;
               }];
    handler(reply, nil);
}

@end

// An app extension's executable hands control to Foundation, which reads the principal class out of
// the bundle's Info.plist and runs the XPC service loop. (Xcode's templates achieve the same thing
// by linking with `-e _NSExtensionMain`; spelling it as a main() keeps the link line ordinary.)
extern "C" int NSExtensionMain(void);

int main(void) {
    return NSExtensionMain();
}
