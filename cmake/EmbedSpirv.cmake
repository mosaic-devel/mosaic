# Script mode: cmake -DSPV=<in.spv> -DHPP=<out.hpp> -DSYM=<symbol> -P EmbedSpirv.cmake
#
# Reads a SPIR-V binary and writes a header embedding it as a 32-bit-word array in
# namespace mosaic::shaders (uint32_t guarantees the 4-byte alignment Vulkan expects).

if(NOT EXISTS "${SPV}")
    message(FATAL_ERROR "EmbedSpirv: input '${SPV}' not found")
endif()

file(READ "${SPV}" hexdata HEX)  # lowercase hex, byte (little-endian) order, no separators
string(LENGTH "${hexdata}" hexlen)
math(EXPR bytelen "${hexlen} / 2")
math(EXPR rem "${bytelen} % 4")
if(NOT rem EQUAL 0)
    message(FATAL_ERROR "EmbedSpirv: '${SPV}' is ${bytelen} bytes (not a multiple of 4)")
endif()
math(EXPR words "${bytelen} / 4")

set(body "")
set(col 0)
if(words GREATER 0)
    math(EXPR last "${words} - 1")
    foreach(i RANGE ${last})
        math(EXPR off "${i} * 8")
        string(SUBSTRING "${hexdata}" ${off} 8 w)  # 8 hex chars = 4 bytes (b0 b1 b2 b3)
        string(SUBSTRING "${w}" 0 2 b0)
        string(SUBSTRING "${w}" 2 2 b1)
        string(SUBSTRING "${w}" 4 2 b2)
        string(SUBSTRING "${w}" 6 2 b3)
        string(APPEND body "0x${b3}${b2}${b1}${b0}u,")
        math(EXPR col "${col} + 1")
        if(col EQUAL 8)
            string(APPEND body "\n    ")
            set(col 0)
        else()
            string(APPEND body " ")
        endif()
    endforeach()
endif()

file(WRITE "${HPP}"
"// Generated from ${SPV} -- do not edit.\n"
"#pragma once\n"
"#include <cstddef>\n"
"#include <cstdint>\n"
"namespace mosaic::shaders {\n"
"inline constexpr std::uint32_t ${SYM}[] = {\n    ${body}\n};\n"
"inline constexpr std::size_t ${SYM}_size = sizeof(${SYM});\n"
"}  // namespace mosaic::shaders\n")
