#pragma once

#include "io/options_schema.hpp"

#include <string>
#include <string_view>

// io/export_path -- WHERE an export goes, as a pure function (Export & I/O plan §6, "Path
// behavior"). Small, but it is the answer to two user-visible bugs, so it is deliberately its own
// unit-testable module rather than a few lines inside the modal:
//
//   * the export path used to default to the PROCESS WORKING DIRECTORY -- whatever directory the
//     binary happened to be launched from, which is never a place a user meant to write to. No
//     rule here can ever produce it: the policy only ever answers with a path it was handed, and
//     a relative one is REFUSED rather than resolved (resolving is what silently reintroduces the
//     cwd). tests/test_export_path.cpp asserts that directly.
//   * the picker was handed a BARE FILE NAME as its start location. kdialog runs that argument
//     through QUrl::fromUserInput(), which treats a dotted relative string as a web address and
//     hands the dialog "http://asdf.png" (see platform/file_dialog.cpp). Everything the picker is
//     given is now an absolute path or nothing at all.
//
// The sticky half of §6: after the first export a document REMEMBERS its target (so re-export is
// one click, the "Export to <file>" menu item) but the memory NEVER leaks to the next document.
// It lives in the app's per-document session state -- never a sidecar file beside the user's
// image, which the standing project rule forbids.
namespace mosaic::io {

// One document's export memory. Empty until that document has been exported once.
struct ExportTarget {
    std::string path;      // absolute path of the last successful export ("" = never exported)
    std::string formatId;  // formatIdName() of the format it was written with ("" = none)
    // ... and the options it was written WITH. §6 only asks for path+format, but a one-click
    // "Export to <file>" that silently reverted a JPEG from quality 60 to the default 90 would be
    // a worse lie than no re-export at all. An empty bag simply coerces to the schema's defaults.
    OptionValues values;

    [[nodiscard]] bool empty() const noexcept { return path.empty(); }
    void clear() {
        path.clear();
        formatId.clear();
        values.clear();
    }
    bool operator==(const ExportTarget&) const = default;
};

// Everything the policy may look at, as plain strings: no Document, no environment, no
// filesystem. That is what makes the rules testable without a picker or a home directory --
// the only way either of the two bugs above gets a regression test.
struct ExportPathInputs {
    std::string lastExportPath;  // ExportTarget::path for THIS document ("" = never exported)
    std::string documentPath;    // core::Document::filePath() ("" = never on disk)
    std::string fallbackDir;     // the OS pictures (export) or documents (save) location
    std::string homeDir;         // the last resort before "nothing resolved"
};

// The directory the system picker should open in, in this order:
//   1. the directory of this document's last export   (sticky per document, §6)
//   2. the directory the document itself came from    (you export beside your source)
//   3. the OS pictures/documents location
//   4. the home directory
// A candidate that is not ABSOLUTE is skipped rather than resolved -- a relative document path
// (a command line of `mosaic shot.png`) would otherwise resolve against the process working
// directory, which is exactly the bug. Returns "" when nothing resolved; callers treat that as
// "let the picker choose", never as ".".
[[nodiscard]] std::string exportStartFolder(const ExportPathInputs& in);

// What to open the picker with, and whether a one-click re-export is available.
struct ExportSeed {
    std::string folder;     // exportStartFolder(in); "" when nothing resolved
    std::string name;       // the file name to pre-fill: "<stem>.<ext>", never a path
    std::string fullPath;   // folder/name, or just `name` when no folder resolved
    bool reExport = false;  // this document has an export target: "Export to <file>" is live
};

// Seed a fresh export of `stem` as `extension` ("png", or ".png" -- either spelling). When the
// document has been exported before, the remembered file's STEM wins over `stem` (you exported
// "shot-final.png"; changing the format offers "shot-final.jxl", not the document's own name).
// An empty stem falls back to "untitled".
[[nodiscard]] ExportSeed seedExportTarget(const ExportPathInputs& in, std::string_view stem,
                                          std::string_view extension);

// Resolve what the user typed in the modal's path field. An absolute path is taken verbatim; a
// bare name or a relative path is resolved against `folder` (the seed's directory) -- NEVER
// against the process working directory. Returns "" for empty input, or when `typed` is relative
// and `folder` is empty (there is no honest answer, and the caller must open the picker).
[[nodiscard]] std::string resolveExportPath(std::string_view typed, std::string_view folder);

// Give `path` the extension `ext` ("png" or ".png") unless it already ends in one of the
// format's extensions. Used to guarantee the chosen format's suffix on a hand-typed name.
[[nodiscard]] std::string withExtension(std::string_view path, std::string_view ext);

// The last component of `path` ("" when it ends in a separator or is empty). Deliberately not
// std::filesystem::path::filename so the rule is the same on every platform the picker runs on:
// split on '/' and '\\'.
[[nodiscard]] std::string fileNameOf(std::string_view path);

// True when `path` is absolute in the host's filesystem sense ('/' on POSIX, a drive or UNC
// prefix on Windows). An empty path is never absolute.
[[nodiscard]] bool isAbsolutePath(std::string_view path);

// ---------------------------------------------------------------------------------------------
// Environment companions -- NOT pure. Kept out of the rules above on purpose.
// ---------------------------------------------------------------------------------------------

// $HOME (or %USERPROFILE%), or "" when unset. Never falls back to ".".
[[nodiscard]] std::string userHomeDir();

// The user's pictures folder: $XDG_PICTURES_DIR, else the XDG user-dirs record, else
// $HOME/Pictures when it exists, else $HOME. "" when even $HOME is unset.
[[nodiscard]] std::string userPicturesDir();

// The user's documents folder, resolved the same way ($XDG_DOCUMENTS_DIR, user-dirs,
// $HOME/Documents, $HOME).
[[nodiscard]] std::string userDocumentsDir();

// The two ready-made input bags: an EXPORT falls back to the pictures folder, a document SAVE /
// OPEN to the documents folder.
[[nodiscard]] ExportPathInputs exportPathInputs(std::string lastExportPath,
                                                std::string documentPath);
[[nodiscard]] ExportPathInputs documentPathInputs(std::string documentPath);

} // namespace mosaic::io
