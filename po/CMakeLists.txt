# Translation-catalog template (.pot) extraction.
#
# This is NOT part of the default build. Regenerate the template after adding/changing UI
# strings with:
#     cmake --build <build-dir> --target pot
# It scans every src/ source for _()/N_()-wrapped strings and writes po/mosaic.pot, which is
# committed as the English template. Translators copy it to <lang>.po, translate, and the .po
# is compiled to <localedir>/<lang>/LC_MESSAGES/mosaic.mo. See docs/i18n.md.

include(GNUInstallDirs)  # CMAKE_INSTALL_LOCALEDIR, where the compiled catalogs are installed

find_program(XGETTEXT_EXECUTABLE xgettext)
if(NOT XGETTEXT_EXECUTABLE)
    message(STATUS "xgettext not found: the 'pot' target (i18n extraction) is disabled.")
    return()
endif()

# Sources to scan, as paths relative to the source root so xgettext records clean, reproducible
# "#: src/...:line" location comments. CONFIGURE_DEPENDS re-globs when files are added/removed.
file(GLOB_RECURSE _abs_sources CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/*.hpp")
set(_rel_sources "")
foreach(_f IN LISTS _abs_sources)
    file(RELATIVE_PATH _r "${CMAKE_SOURCE_DIR}" "${_f}")
    list(APPEND _rel_sources "${_r}")
endforeach()
list(SORT _rel_sources)
# The Annoyances one-liners (src/ui/motivational_lines.*) live in their OWN "motivate" catalog (see
# pot-motivate below), so keep them out of the main template even though they sit under src/.
list(REMOVE_ITEM _rel_sources "src/ui/motivational_lines.cpp" "src/ui/motivational_lines.hpp")

add_custom_target(pot
    COMMAND "${XGETTEXT_EXECUTABLE}"
        --from-code=UTF-8
        --c++ --keyword=_ --keyword=N_
        --add-comments=TRANSLATORS:
        --sort-by-file
        --package-name=Mosaic
        --package-version=${PROJECT_VERSION}
        --msgid-bugs-address=https://github.com/mosaic-devel/mosaic/issues
        --copyright-holder=The\ Mosaic\ contributors
        -o po/mosaic.pot
        ${_rel_sources}
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Extracting i18n template -> po/mosaic.pot"
    VERBATIM)

# The "motivate" satellite catalog (Settings → Annoyances → "Cheesy motivational one-liners"): the
# 100 one-liners are their OWN gettext domain so they never burden po/mosaic.pot. They are wrapped
# in MOTIVATE_(); extract with that keyword ALONE -- the bare `--keyword` first clears xgettext's
# defaults, so no app strings leak in -- into po/motivate.pot. The runtime binds the
# "motivate" domain (common::i18n::initDomain) and reads via dtr("motivate", line). Regenerate with:
#     cmake --build <build-dir> --target pot-motivate
add_custom_target(pot-motivate
    COMMAND "${XGETTEXT_EXECUTABLE}"
        --from-code=UTF-8
        --c++ --keyword --keyword=MOTIVATE_
        --sort-by-file
        --package-name=Mosaic
        --package-version=${PROJECT_VERSION}
        --msgid-bugs-address=https://github.com/mosaic-devel/mosaic/issues
        --copyright-holder=The\ Mosaic\ contributors
        -o po/motivate.pot
        src/ui/motivational_lines.cpp
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Extracting the Annoyances one-liners -> po/motivate.pot"
    VERBATIM)

# ---- Catalog compilation: po/<lang>/<domain>.po -> <lang>/LC_MESSAGES/<domain>.mo -------------
#
# Layout invariant for this directory: **files are templates, directories are languages.** So
# po/mosaic.pot and po/motivate.pot are the English templates, and po/de/mosaic.po is German. That
# is the KDE/Krita layout, which is what a translator arriving from those projects expects.
#
# The .mo files are built into ${CMAKE_BINARY_DIR}/locale, which is a complete locale tree -- so a
# build-tree binary needs no install step to speak another language:
#     MOSAIC_LOCALEDIR=<build>/locale MOSAIC_LANG=de <build>/bin/mosaic
# They are part of ALL because a translation nobody can see is a translation nobody fixes.

find_program(MSGFMT_EXECUTABLE msgfmt)
if(NOT MSGFMT_EXECUTABLE)
    message(STATUS "msgfmt not found: translation catalogs will not be compiled (UI stays English).")
    return()
endif()

# LINGUAS is the shipped language list (generated from tools/i18n/languages.py). Re-run CMake when
# it changes so a newly added language is picked up without a manual reconfigure.
set(_linguas_file "${CMAKE_CURRENT_SOURCE_DIR}/LINGUAS")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_linguas_file}")
file(STRINGS "${_linguas_file}" _linguas REGEX "^[^#]")

set(_mo_files "")
foreach(_lang IN LISTS _linguas)
    string(STRIP "${_lang}" _lang)
    if(NOT _lang)
        continue()
    endif()
    # Both catalogs are optional per language: "motivate" is a joke catalog nobody has to translate,
    # and a language may be listed before anyone has actually filled in its main catalog.
    foreach(_domain mosaic motivate)
        set(_po "${CMAKE_CURRENT_SOURCE_DIR}/${_lang}/${_domain}.po")
        if(NOT EXISTS "${_po}")
            continue()
        endif()
        set(_mo "${CMAKE_BINARY_DIR}/locale/${_lang}/LC_MESSAGES/${_domain}.mo")
        # --check is msgfmt's own opinion on format-specifier and header sanity, independent of
        # tools/i18n/assemble_po.py's checks. A catalog that fails it should break the build, not
        # ship: a %s that survives into a .mo is a crash in someone else's language.
        add_custom_command(
            OUTPUT "${_mo}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/locale/${_lang}/LC_MESSAGES"
            COMMAND "${MSGFMT_EXECUTABLE}" --check --output-file "${_mo}" "${_po}"
            DEPENDS "${_po}"
            COMMENT "Compiling ${_lang}/${_domain} catalog"
            VERBATIM)
        list(APPEND _mo_files "${_mo}")
        install(FILES "${_mo}"
                DESTINATION "${CMAKE_INSTALL_LOCALEDIR}/${_lang}/LC_MESSAGES"
                OPTIONAL)
    endforeach()
endforeach()

list(LENGTH _mo_files _mo_count)
if(_mo_count GREATER 0)
    add_custom_target(catalogs ALL DEPENDS ${_mo_files})
    message(STATUS "i18n: ${_mo_count} translation catalog(s) from ${CMAKE_CURRENT_SOURCE_DIR}")
else()
    message(STATUS "i18n: no .po catalogs present yet (UI stays English).")
endif()

# Coverage report for translators: what fraction of each catalog is actually filled in.
#     cmake --build <build-dir> --target i18n-stats
add_custom_target(i18n-stats
    COMMAND "${CMAKE_COMMAND}" -E env
        "${CMAKE_COMMAND}" -DPO_DIR=${CMAKE_CURRENT_SOURCE_DIR} -DMSGFMT=${MSGFMT_EXECUTABLE}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/stats.cmake"
    COMMENT "Translation coverage per language"
    VERBATIM)
