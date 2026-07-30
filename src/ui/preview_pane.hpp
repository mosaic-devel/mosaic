#pragma once

#include "common/geometry.hpp" // common::Rect
#include "common/image.hpp"

#include <FL/Fl_Widget.H>

#include <string>

// The shared live-preview surface for the modal dialogs (Edit->Fill…, Layer Effects…). It reads as a
// small viewport onto the document: the OFF-CANVAS surround is painted the app's canvas backdrop
// colour and the in-canvas transparent areas the checkerboard, so it is apparent where the canvas
// ends; the (alpha-carrying) preview is composited over both, aspect-fit. The host renders a
// document-space region matched to this pane's aspect (so "fit" fills the pane) and INCLUDING any
// off-canvas area, then reports where the canvas sits within that image so the pane can split
// checker vs backdrop. The preview is drawn inset by the 1px frame, so the frame never overpaints
// content (which used to drop -- and flicker -- a 1px edge line as the zoom changed).
//
// setImage() takes a STRAIGHT-alpha image plus the document rect in image-pixel coords; the pane
// premultiplies once, scales, and composites over the surround at native pane resolution (crisp
// backdrop, only the content scales). The empty state (no image) shows a plain muted note.
namespace mosaic::ui {

// What a preview host returns: the composited region + where the canvas sits within it (image-pixel
// coords; may be negative / extend past the image when the region reaches off-canvas).
struct PreviewContent {
    common::Image image;
    common::Rect canvasInImage;
};

class PreviewPane : public Fl_Widget {
public:
    PreviewPane(int X, int Y, int W, int H) : Fl_Widget(X, Y, W, H) {}

    void setImage(const common::Image& rgba, common::Rect canvasInImage);
    void setContent(const PreviewContent& c) { setImage(c.image, c.canvasInImage); }
    void clearImage();
    void setNote(std::string note); // the empty-state caption (shown when there is no image)

    void reapplyTheme() { redraw(); } // colours are read live in draw(); just repaint

protected:
    void draw() override;

private:
    common::Image m_premul;         // the preview, PREMULTIPLIED (band-free scaling; a = coverage)
    common::Rect m_canvasInImage{}; // document rect within m_premul, in image-pixel coords
    bool m_hasImage = false;
    std::string m_note;
};

} // namespace mosaic::ui
