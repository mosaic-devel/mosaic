#pragma once

class Fl_Window;

// The themed "Help -> About Mosaic" dialog (replaces the plain fl_message box). A small modal
// window: the animated "Mosaic" wordmark (each glyph swoops in on a curve, one after another), the
// app logo, the version, a credits "drum" that cycles the contributors, the one-line description
// and a single themed Close button. Modal -- a child of the host so it never strays into its own
// top-level on native Wayland (the recurring sub-window trap), matching showNewDocumentDialog().
namespace mosaic::ui {

// Show the dialog and block until it is closed. Centres over `parent` when given, otherwise on the
// screen. Requires a display.
void showAboutDialog(Fl_Window* parent = nullptr);

} // namespace mosaic::ui
