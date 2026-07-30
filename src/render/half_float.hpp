#pragma once

#include <cstdint>
#include <cstring>

// IEEE-754 binary16 <-> binary32, in one place (S60-a).
//
// The resident compositor's working buffer is `VK_FORMAT_R16G16B16A16_SFLOAT` -- the only float
// format Vulkan 1.0 guarantees for BOTH storage and linear filtering (gpu_caps.hpp, the §2.4
// decision) -- so any host that seeds or reads back an accumulator has to speak half. C++ has no
// standard half, and the Vulkan API takes the bytes without interpreting them, so the conversion
// is ours.
//
// Round-to-nearest-EVEN with subnormals, deliberately: a truncating converter loses up to a full
// ulp, and at fp16's ~11-bit mantissa that is within sight of the 1/255 parity budget the GPU
// lane is held to. The conversion must never be the thing that costs the tolerance.
//
// Header-only and free of Vulkan, so tests can use it without a device.

namespace mosaic::render {

[[nodiscard]] inline std::uint16_t floatToHalf(float f) noexcept {
    std::uint32_t x = 0;
    std::memcpy(&x, &f, sizeof(x));
    const std::uint32_t sign = (x >> 16) & 0x8000u;
    const std::uint32_t rawExp = (x >> 23) & 0xFFu;
    std::uint32_t mant = x & 0x7FFFFFu;
    if (rawExp == 0xFFu)  // inf / NaN -- a NaN must stay a NaN, so keep one mantissa bit set
        return static_cast<std::uint16_t>(sign | 0x7C00u | (mant != 0 ? 0x200u : 0u));
    const std::int32_t exp = static_cast<std::int32_t>(rawExp) - 127 + 15;
    if (exp >= 31) return static_cast<std::uint16_t>(sign | 0x7C00u);  // overflow -> inf
    if (exp <= 0) {                                                    // subnormal or zero
        if (exp < -10) return static_cast<std::uint16_t>(sign);
        mant |= 0x800000u;  // restore the implicit leading 1 before denormalising
        const int shift = 14 - exp;
        std::uint32_t half = mant >> shift;
        const std::uint32_t rem = mant & ((1u << shift) - 1u);
        const std::uint32_t mid = 1u << (shift - 1);
        if (rem > mid || (rem == mid && (half & 1u) != 0)) ++half;
        return static_cast<std::uint16_t>(sign | half);
    }
    std::uint32_t half = (static_cast<std::uint32_t>(exp) << 10) | (mant >> 13);
    const std::uint32_t rem = mant & 0x1FFFu;
    // A carry out of the mantissa lands in the exponent field, which is exactly right: adding one
    // to 0x_3FF rolls the significand to zero and bumps the exponent.
    if (rem > 0x1000u || (rem == 0x1000u && (half & 1u) != 0)) ++half;
    return static_cast<std::uint16_t>(sign | half);
}

[[nodiscard]] inline float halfToFloat(std::uint16_t h) noexcept {
    const std::uint32_t sign = static_cast<std::uint32_t>(h & 0x8000u) << 16;
    const std::uint32_t exp = (h >> 10) & 0x1Fu;
    std::uint32_t mant = h & 0x3FFu;
    std::uint32_t x = 0;
    if (exp == 0) {
        if (mant != 0) {
            // Subnormal: normalise it by hand. Each left shift costs one from the exponent, and
            // 127 - 15 - 1 == 111, so the bias works out to (114 - e) once the loop has run.
            std::uint32_t e = 1;
            while ((mant & 0x400u) == 0) {
                mant <<= 1;
                ++e;
            }
            mant &= 0x3FFu;
            x = sign | ((114u - e) << 23) | (mant << 13);
        } else {
            x = sign;  // +/- 0
        }
    } else if (exp == 31) {
        x = sign | 0x7F800000u | (mant << 13);  // inf / NaN
    } else {
        x = sign | ((exp + 112u) << 23) | (mant << 13);  // 127 - 15 == 112
    }
    float f = 0.0f;
    std::memcpy(&f, &x, sizeof(f));
    return f;
}

}  // namespace mosaic::render
