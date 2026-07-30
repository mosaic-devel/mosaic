#include "platform/file_dialog.hpp"

#include <doctest/doctest.h>

#include <string>
#include <vector>

// platform::FileDialog -- the halves that CAN be pinned headlessly.
//
// WHAT IS NOT HERE, and why: the picker itself. The X11+KDE path spawns kdialog and waits for it
// from a nested FLTK loop; the fallback path does a full XDG-portal round trip over sd-bus. Both
// need a display server, a session bus and a human, so neither is faked here -- a fake would only
// pin the fake. The user's interactive pass is what covers them, and the freeze they used to cause
// is designed out structurally (see the long comments in platform/file_dialog.cpp: nothing blocks
// the main loop, no descriptor outlives its registration, no event dispatched inside a nested pump
// reaches the app, and every "the answer can never arrive" case ends the wait instead of spinning).
//
// What IS testable is every pure input the pickers are fed -- and each of those has already shipped
// a user-visible bug: a path pasted into a file-NAME field, a bare name used as a start LOCATION
// (which kdialog resolved through QUrl::fromUserInput() into "http://asdf.png"), and the process
// working directory as the default export folder.

// Guarded to match the `detail` namespace's own guard in platform/file_dialog.hpp: those helpers
// are POSIX-shaped (a leading '/' means absolute, file:// URIs, FLTK brace + kdialog pipe filter
// syntax) and neither the macOS nor the Windows backend implements any of them, so there is nothing
// to pin there. Windows also inverts fileDialogGrabsInput() by design -- its picker is an
// in-process modal -- so the CHECK_FALSE below is a Linux-and-macOS fact, not a portable one (S57).
#if !defined(__APPLE__) && !defined(_WIN32)

using namespace mosaic::platform;

namespace {

FileDialogRequest request(std::string startFolder, std::string suggestedName) {
    FileDialogRequest req;
    req.startFolder = std::move(startFolder);
    req.suggestedName = std::move(suggestedName);
    return req;
}

const std::string kHome = "/home/tester";

} // namespace

TEST_CASE("a name field gets a base name, never a path and never a URI") {
    CHECK(detail::baseNameOf("shot.png") == "shot.png");
    CHECK(detail::baseNameOf("/home/u/pics/shot.png") == "shot.png");
    CHECK(detail::baseNameOf("relative/dir/shot.png") == "shot.png");
    CHECK(detail::baseNameOf("file:///home/u/shot.png") == "shot.png");
    CHECK(detail::baseNameOf("/home/u/pics/") == ""); // a directory has no name part
    CHECK(detail::baseNameOf("") == "");
}

TEST_CASE("the start folder is absolute or it is the fallback -- never the working directory") {
    CHECK(detail::absoluteStartFolder(request("/srv/out", ""), kHome) == "/srv/out");
    // A RELATIVE candidate is skipped, not resolved: resolving is what reintroduces the CWD.
    CHECK(detail::absoluteStartFolder(request("out", ""), kHome) == kHome);
    CHECK(detail::absoluteStartFolder(request(".", ""), kHome) == kHome);
    CHECK(detail::absoluteStartFolder(request("", ""), kHome) == kHome);
    // A bare file name is the exact value that produced "http://asdf.png" once it reached kdialog.
    CHECK(detail::absoluteStartFolder(request("asdf.png", ""), kHome) == kHome);
    // No fallback either: better nothing than something relative.
    CHECK(detail::absoluteStartFolder(request("", ""), "") == "");
}

TEST_CASE("kdialog's start location always comes out absolute") {
    // Folder + name, kept apart by the caller and joined only here.
    CHECK(detail::kdialogStartArgument(request("/srv/out", "shot.png"), /*save=*/true, kHome) ==
          "/srv/out/shot.png");
    // A trailing slash on the folder is not doubled.
    CHECK(detail::kdialogStartArgument(request("/srv/out/", "shot.png"), /*save=*/true, kHome) ==
          "/srv/out/shot.png");
    // An Open gets the directory alone, marked unambiguously as one.
    CHECK(detail::kdialogStartArgument(request("/srv/out", "shot.png"), /*save=*/false, kHome) ==
          "/srv/out/");
    // A caller that hands a whole path as the "name" still cannot smuggle a second path in.
    CHECK(detail::kdialogStartArgument(request("/srv/out", "/tmp/elsewhere/shot.png"),
                                       /*save=*/true, kHome) == "/srv/out/shot.png");
    // No folder: the fallback, never "." and never the bare name.
    CHECK(detail::kdialogStartArgument(request("", "asdf.png"), /*save=*/true, kHome) ==
          kHome + "/asdf.png");
    // No folder AND no fallback: root, which is still absolute.
    CHECK(detail::kdialogStartArgument(request("", "asdf.png"), /*save=*/true, "") == "/asdf.png");
    CHECK(detail::kdialogStartArgument(request("", ""), /*save=*/false, "") == "/");
}

TEST_CASE("no combination of inputs can produce a relative kdialog start location") {
    // The regression bar for the http:// bug, stated as an invariant rather than a case list:
    // whatever goes in, what comes out starts with '/' -- the branch QUrl::fromUserInput() takes
    // to QUrl::fromLocalFile() instead of to its "the user typed a web address" heuristic.
    const std::vector<std::string> folders{"",  ".",        "..",        "out",
                                           "~", "/srv/out", "/srv/out/", "asdf.png"};
    const std::vector<std::string> names{"", "shot.png", "a b.png", "/tmp/x/shot.png", "."};
    const std::vector<std::string> homes{"", kHome};
    const std::vector<bool> saves{false, true};
    for (const std::string& folder : folders) {
        for (const std::string& name : names) {
            for (const bool save : saves) {
                for (const std::string& home : homes) {
                    CAPTURE(folder);
                    CAPTURE(name);
                    CAPTURE(save);
                    CAPTURE(home);
                    const std::string arg =
                        detail::kdialogStartArgument(request(folder, name), save, home);
                    REQUIRE(!arg.empty());
                    CHECK(arg.front() == '/');
                }
            }
        }
    }
}

TEST_CASE("portal file:// URIs decode to local paths") {
    CHECK(detail::uriToPath("file:///home/u/shot.png") == "/home/u/shot.png");
    // The optional host component is stripped.
    CHECK(detail::uriToPath("file://localhost/home/u/shot.png") == "/home/u/shot.png");
    // Percent-encoding, including a byte that is itself a percent sign.
    CHECK(detail::uriToPath("file:///home/u/a%20b.png") == "/home/u/a b.png");
    CHECK(detail::uriToPath("file:///home/u/100%25.png") == "/home/u/100%.png");
    CHECK(detail::uriToPath("file:///home/u/%C3%A9.png") == "/home/u/\xC3\xA9.png");
    // The document-portal FUSE path is a real path and must survive untouched.
    CHECK(detail::uriToPath("file:///run/user/1000/doc/abcd/shot.png") ==
          "/run/user/1000/doc/abcd/shot.png");
    // A bare path (no scheme) is still decoded rather than rejected.
    CHECK(detail::uriToPath("/home/u/shot.png") == "/home/u/shot.png");
    // Malformed escapes are left alone rather than eating the following bytes.
    CHECK(detail::uriToPath("file:///a%zz") == "/a%zz");
    CHECK(detail::uriToPath("file:///a%2") == "/a%2");
    // Nothing usable -> empty, which the caller turns into a cancel rather than a bad path.
    CHECK(detail::uriToPath("") == "");
    CHECK(detail::uriToPath("file://") == "");
}

TEST_CASE("filter lists translate to each backend's syntax") {
    const std::vector<FileFilter> filters{{"PNG image", {"*.png"}, {"image/png"}},
                                          {"Images", {"*.png", "*.jpg", "*.jpeg"}, {}},
                                          {"All files", {"*"}, {}}};
    // FLTK: "Label\tpattern", newline separated; a multi-extension set becomes one brace group.
    CHECK(detail::fltkFilterString(filters) ==
          "PNG image\t*.{png}\nImages\t*.{png,jpg,jpeg}\nAll files\t*");
    // kdialog: "<space-separated globs>|<label>", newline separated. MIME types are portal-only and
    // must not leak in here.
    CHECK(detail::kdialogFilter(filters) ==
          "*.png|PNG image\n*.png *.jpg *.jpeg|Images\n*|All files");
    // A filter with no globs describes no file set and is dropped by both.
    const std::vector<FileFilter> noGlobs{{"Broken", {}, {"image/png"}}, {"PNG", {"*.png"}, {}}};
    CHECK(detail::fltkFilterString(noGlobs) == "PNG\t*.{png}");
    CHECK(detail::kdialogFilter(noGlobs) == "*.png|PNG");
    CHECK(detail::fltkFilterString({}) == "");
    CHECK(detail::kdialogFilter({}) == "");
}

TEST_CASE("a second picker asked for while one is up is REFUSED, not stacked") {
    // Both waits run a nested Fl::wait() loop. A second dialog opened from inside one would stack a
    // second nested loop and a second modal guard whose destructor re-activates the parent while
    // the first picker is still on screen. run() refuses before it touches FLTK or D-Bus, which is
    // why this can be asserted with no display and no session bus.
    CHECK_FALSE(fileDialogInFlight());
    CHECK_FALSE(fileDialogGrabsInput());
    {
        const detail::InFlightScope inFlight;
        CHECK(fileDialogInFlight());
        CHECK(fileDialogGrabsInput()); // an out-of-process picker: input must be blocked

        FileDialogRequest req;
        req.title = "Export";
        req.startFolder = "/srv/out";
        req.suggestedName = "shot.png";
        CHECK_FALSE(showSaveDialog(req).has_value());
        CHECK_FALSE(showOpenDialog(req).has_value());
    }
    // ... and the flag balances, so the refusal is not sticky.
    CHECK_FALSE(fileDialogInFlight());
    CHECK_FALSE(fileDialogGrabsInput());
}

#endif // !__APPLE__ && !_WIN32
