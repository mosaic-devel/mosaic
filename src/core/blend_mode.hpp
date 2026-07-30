#pragma once

#include <string_view>

// Layer blend modes. The enum is the document-model contract; the Vulkan compositor (S7)
// implements each as a shader, and the names are the stable keys used by the UI (S10) and the
// .mosaic serializer (S48). Ordering groups them as creative tools present them (darken /
// lighten / contrast / inversion / component families), matching Photoshop/Krita conventions.
namespace mosaic::core {

enum class BlendMode {
    Normal,
    // Darken family
    Darken,
    Multiply,
    ColorBurn,
    LinearBurn,
    // Lighten family
    Lighten,
    Screen,
    ColorDodge,
    LinearDodge,  // a.k.a. Add
    // Contrast family
    Overlay,
    SoftLight,
    HardLight,
    VividLight,
    LinearLight,
    PinLight,
    // Inversion / comparative
    Difference,
    Exclusion,
    Subtract,
    Divide,
    // Component (HSL)
    Hue,
    Saturation,
    Color,
    Luminosity,
};

// Number of blend modes; useful for iterating the enum (it is contiguous from 0).
inline constexpr int kBlendModeCount = static_cast<int>(BlendMode::Luminosity) + 1;

// Stable, human-readable name (also the serialization key). Never returns nullptr.
[[nodiscard]] constexpr std::string_view blendModeName(BlendMode mode) {
    switch (mode) {
        case BlendMode::Normal: return "Normal";
        case BlendMode::Darken: return "Darken";
        case BlendMode::Multiply: return "Multiply";
        case BlendMode::ColorBurn: return "Color Burn";
        case BlendMode::LinearBurn: return "Linear Burn";
        case BlendMode::Lighten: return "Lighten";
        case BlendMode::Screen: return "Screen";
        case BlendMode::ColorDodge: return "Color Dodge";
        case BlendMode::LinearDodge: return "Linear Dodge";
        case BlendMode::Overlay: return "Overlay";
        case BlendMode::SoftLight: return "Soft Light";
        case BlendMode::HardLight: return "Hard Light";
        case BlendMode::VividLight: return "Vivid Light";
        case BlendMode::LinearLight: return "Linear Light";
        case BlendMode::PinLight: return "Pin Light";
        case BlendMode::Difference: return "Difference";
        case BlendMode::Exclusion: return "Exclusion";
        case BlendMode::Subtract: return "Subtract";
        case BlendMode::Divide: return "Divide";
        case BlendMode::Hue: return "Hue";
        case BlendMode::Saturation: return "Saturation";
        case BlendMode::Color: return "Color";
        case BlendMode::Luminosity: return "Luminosity";
    }
    return "Normal";
}

}  // namespace mosaic::core
