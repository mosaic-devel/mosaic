# Script mode: cmake -DBYTES_IN=<file> -DHPP=<out.hpp> -DSYM=<symbol> -DNS=<namespace> \
#                    -P EmbedBytes.cmake
#
# Reads an arbitrary file and writes a header embedding it as an `unsigned char` array in
# the given namespace (e.g. mosaic::assets). Used to bake assets such as the app-icon SVG
# into the binary so there is no runtime file dependency (mirrors EmbedSpirv.cmake, which
# does the same for SPIR-V words).
#
# The header is written through copy_if_different: this script runs on EVERY build (see
# EmbedAssets.cmake), so an unchanged asset must not dirty the header's mtime and cascade
# into recompiles.

if(NOT EXISTS "${BYTES_IN}")
    message(FATAL_ERROR "EmbedBytes: input '${BYTES_IN}' not found")
endif()

file(READ "${BYTES_IN}" hexdata HEX) # lowercase hex, byte order, no separators
# Bulk conversion -- a per-byte foreach costs ~2s on a 32KB asset, and this runs per build.
# (CMake regex has no bounded repetition, so the 12-bytes-per-line pattern is spelled out
# via string(REPEAT); a single unbroken line would exceed MSVC's line-length limits.)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1, " body "${hexdata}")
string(REPEAT "0x[0-9a-f][0-9a-f], " 12 _twelve)
string(REGEX REPLACE "(${_twelve})" "\\1\n    " body "${body}")

file(WRITE "${HPP}.tmp"
"// Generated from ${BYTES_IN} -- do not edit.\n"
"#pragma once\n"
"#include <cstddef>\n"
"namespace ${NS} {\n"
"inline constexpr unsigned char ${SYM}[] = {\n    ${body}\n};\n"
"inline constexpr std::size_t ${SYM}_size = sizeof(${SYM});\n"
"}  // namespace ${NS}\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${HPP}.tmp" "${HPP}")
file(REMOVE "${HPP}.tmp")
