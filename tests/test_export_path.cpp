#include "io/export_path.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <string>

// The export-path policy (docs/export-system-plan.md §6 "Path behavior") -- the pure half of the
// two user-reported picker bugs, and the only part of either that CAN be tested headlessly: the
// system file picker itself cannot be driven without a display and a portal.
//
// Bug 1: the default export path was the process working directory. Every rule below is written so
//        that answer is unreachable, and the last test case asserts it directly.
// Bug 2: the picker was handed a bare file name as its start LOCATION, which kdialog resolved
//        through QUrl::fromUserInput() into "http://<name>". The policy's job here is to keep the
//        directory and the file name apart and to hand out only absolute directories.
using namespace mosaic;
using namespace mosaic::io;

namespace {

ExportPathInputs inputs(std::string last, std::string doc, std::string fallback,
                        std::string home = "/home/tester") {
    ExportPathInputs in;
    in.lastExportPath = std::move(last);
    in.documentPath = std::move(doc);
    in.fallbackDir = std::move(fallback);
    in.homeDir = std::move(home);
    return in;
}

} // namespace

TEST_CASE("the start folder follows last-export, then document, then the OS folder, then home") {
    // 1. the sticky per-document target wins.
    CHECK(exportStartFolder(inputs("/srv/out/shot.png", "/home/tester/pics/shot.jpg",
                                   "/home/tester/Pictures")) == "/srv/out");
    // 2. ... else the document's own directory: you export beside your source.
    CHECK(exportStartFolder(inputs("", "/home/tester/pics/shot.jpg", "/home/tester/Pictures")) ==
          "/home/tester/pics");
    // 3. ... else the OS pictures folder.
    CHECK(exportStartFolder(inputs("", "", "/home/tester/Pictures")) == "/home/tester/Pictures");
    // 4. ... else home.
    CHECK(exportStartFolder(inputs("", "", "")) == "/home/tester");
    // 5. ... and with nothing at all, NOTHING -- never ".".
    CHECK(exportStartFolder(inputs("", "", "", "")).empty());
}

TEST_CASE("a relative candidate is skipped, never resolved") {
    // A document opened as `mosaic shot.png` has a relative filePath(). Resolving it would mean
    // resolving against the working directory, which is the bug -- so it is dropped and the next
    // rule answers.
    CHECK(exportStartFolder(inputs("", "shot.png", "/home/tester/Pictures")) ==
          "/home/tester/Pictures");
    CHECK(exportStartFolder(inputs("", "sub/dir/shot.png", "/home/tester/Pictures")) ==
          "/home/tester/Pictures");
    // The same for a relative remembered export path (which should never happen, but the rule
    // must not depend on that).
    CHECK(exportStartFolder(inputs("out/shot.png", "/home/tester/pics/a.jpg", "")) ==
          "/home/tester/pics");
    // A relative fallback/home is refused too: the answer is always absolute or empty.
    CHECK(exportStartFolder(inputs("", "", "Pictures", "home")).empty());
}

TEST_CASE("a file at the filesystem root still yields a directory, not an empty string") {
    CHECK(exportStartFolder(inputs("/shot.png", "", "")) == "/");
}

TEST_CASE("the seed keeps the folder and the file name apart") {
    const ExportSeed s = seedExportTarget(inputs("", "/home/tester/pics/beach.jpg", ""), "beach",
                                          "png");
    CHECK(s.folder == "/home/tester/pics");
    CHECK(s.name == "beach.png");            // a NAME: no directory, no scheme, no separator
    CHECK(s.name.find('/') == std::string::npos);
    CHECK(s.fullPath == "/home/tester/pics/beach.png");
    CHECK_FALSE(s.reExport);
}

TEST_CASE("both extension spellings mean the same thing, and an empty stem falls back") {
    const ExportSeed dotted = seedExportTarget(inputs("", "", "/pics"), "shot", ".png");
    const ExportSeed bare = seedExportTarget(inputs("", "", "/pics"), "shot", "png");
    CHECK(dotted.name == "shot.png");
    CHECK(bare.name == "shot.png");
    CHECK(seedExportTarget(inputs("", "", "/pics"), "", "png").name == "untitled.png");
}

TEST_CASE("after a first export the remembered file's own name is offered again") {
    const ExportPathInputs in = inputs("/srv/out/shot-final.png", "/home/tester/pics/DSC_0001.jpg",
                                       "/home/tester/Pictures");
    const ExportSeed same = seedExportTarget(in, "DSC_0001", "png");
    CHECK(same.reExport);
    CHECK(same.folder == "/srv/out");
    CHECK(same.name == "shot-final.png"); // you named that file; we keep the name
    // Changing the format keeps the stem and swaps the extension.
    CHECK(seedExportTarget(in, "DSC_0001", "jxl").name == "shot-final.jxl");
}

TEST_CASE("a hand-typed path resolves against the seed folder, never the working directory") {
    CHECK(resolveExportPath("/tmp/a.png", "/home/tester/Pictures") == "/tmp/a.png");
    CHECK(resolveExportPath("a.png", "/home/tester/Pictures") == "/home/tester/Pictures/a.png");
    CHECK(resolveExportPath("sub/a.png", "/home/tester/Pictures") ==
          "/home/tester/Pictures/sub/a.png");
    CHECK(resolveExportPath("a.png", "/home/tester/Pictures/") == "/home/tester/Pictures/a.png");
    // Empty in, empty out -- the caller must open the picker rather than invent a location.
    CHECK(resolveExportPath("", "/home/tester").empty());
    CHECK(resolveExportPath("a.png", "").empty());
}

TEST_CASE("withExtension only appends when the extension is not already there") {
    CHECK(withExtension("/a/b/shot", "png") == "/a/b/shot.png");
    CHECK(withExtension("/a/b/shot.png", "png") == "/a/b/shot.png");
    CHECK(withExtension("/a/b/shot.PNG", ".png") == "/a/b/shot.PNG"); // case-insensitive match
    CHECK(withExtension("/a/b/shot.jpg", "png") == "/a/b/shot.jpg.png");
    CHECK(withExtension("/a/b/shot", "") == "/a/b/shot");
    // A dot in a DIRECTORY is not an extension.
    CHECK(withExtension("/a.d/shot", "png") == "/a.d/shot.png");
}

TEST_CASE("fileNameOf and isAbsolutePath answer the picker's two questions") {
    CHECK(fileNameOf("/home/tester/shot.png") == "shot.png");
    CHECK(fileNameOf("shot.png") == "shot.png");
    CHECK(fileNameOf("/home/tester/").empty());
    CHECK(fileNameOf("").empty());

    CHECK(isAbsolutePath("/home/tester"));
    CHECK_FALSE(isAbsolutePath("home/tester"));
    CHECK_FALSE(isAbsolutePath("shot.png"));
    CHECK_FALSE(isAbsolutePath("."));
    CHECK_FALSE(isAbsolutePath(""));
}

TEST_CASE("the process working directory is NEVER the answer") {
    const std::string cwd = std::filesystem::current_path().string();

    // Nothing usable at all: the policy answers "nothing", and the caller falls back to $HOME in
    // the picker layer -- not to wherever the binary was launched from.
    const std::string none = exportStartFolder(inputs("", "", "", ""));
    CHECK(none.empty());
    CHECK(none != cwd);
    CHECK(none != ".");

    // Only relative candidates: still not the working directory.
    const std::string rel = exportStartFolder(inputs("out/a.png", "b.png", "pics", "home"));
    CHECK(rel.empty());
    CHECK(rel != cwd);

    // And the seed built from that state carries a bare NAME with no invented directory, so a
    // caller cannot accidentally turn it into a cwd-relative write.
    const ExportSeed s = seedExportTarget(inputs("", "", "", ""), "shot", "png");
    CHECK(s.folder.empty());
    CHECK(s.fullPath == "shot.png");
    CHECK(resolveExportPath(s.fullPath, s.folder).empty()); // ... which resolves to nothing
}

TEST_CASE("the environment companions answer with an absolute path or nothing") {
    for (const std::string& dir : {userHomeDir(), userPicturesDir(), userDocumentsDir()}) {
        CHECK(dir != ".");
        CHECK(dir != "..");
        if (!dir.empty())
            CHECK(isAbsolutePath(dir));
    }
}
