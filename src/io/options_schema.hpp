#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// io/options_schema -- the typed, data-only description of one format's encoder options, and the
// value bag that answers it (Export & I/O plan §2.1, §6.5; the per-format option lists in
// Appendix A are the source material).
//
// The point is that the Export modal's "backend settings" pane is RENDERED FROM THIS, not coded
// per format: adding a format is one backend file plus a registry line, and the UI never changes.
// That only works if the vocabulary covers what the real encoders actually expose, so it was
// designed against Appendix A rather than in the abstract:
//
//   * Bool          libpng interlace, libjpeg progressive, libjxl lossless, libwebp exact
//   * Int  + range  libpng compression 0-9, libjpeg quality 0-100, libwebp method 0-6,
//                   libavif speed 0-10, giflib palette size 2-256
//   * Real + range  libjxl butteraugli distance 0-25 (the one genuinely fractional knob)
//   * Enum          libjpeg subsampling, libtiff compression, OpenEXR compression, AVIF codec
//   * Text          PNG tEXt/iTXt chunks, Radiance HDR header variables
//   * groups        "Advanced" collapsed by default (§6.5) -- every backend's long tail
//   * conditions    the dependent knobs that would otherwise need bespoke panel code:
//                   JXL distance is meaningless when lossless is on; TIFF's JPEG-quality knob
//                   only applies when compression == "jpeg"; OpenEXR's DWA level only for
//                   dwaa/dwab; WebP's near-lossless only when lossless is on
//
// Everything here is pure data (no FLTK, no libpng, no allocation policy) so a schema can be
// validated and exercised headlessly -- see tests/test_export_options.cpp.
namespace mosaic::io {

// ---------------------------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------------------------

// One option's value. Enum options store their choice's STABLE ID (a std::string), never an
// index: indices renumber the moment a backend inserts a choice, and these values are persisted
// (export presets, S56) and read by the loss diff. NOTE the C++20 variant converting constructor
// (P0608R3) is what keeps `OptionValue{"lzw"}` from selecting the `bool` arm; the textValue()
// helper below makes the intent explicit anyway.
using OptionValue = std::variant<bool, int, double, std::string>;

[[nodiscard]] inline OptionValue boolValue(bool v) { return OptionValue{v}; }
[[nodiscard]] inline OptionValue intValue(int v) { return OptionValue{v}; }
[[nodiscard]] inline OptionValue realValue(double v) { return OptionValue{v}; }
[[nodiscard]] inline OptionValue textValue(std::string v) { return OptionValue{std::move(v)}; }

// A key -> value bag: what the user chose, what a preset stores, what encode() reads.
// Deliberately ordered (std::map, not unordered_map) so iteration -- and therefore any golden
// test or serialized preset -- is deterministic.
class OptionValues {
public:
    OptionValues() = default;

    [[nodiscard]] bool empty() const noexcept { return m_values.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return m_values.size(); }
    [[nodiscard]] bool has(std::string_view key) const noexcept;
    void set(std::string_view key, OptionValue value);
    void erase(std::string_view key);
    void clear() noexcept { m_values.clear(); }

    // The raw value, or nullptr when the key is absent.
    [[nodiscard]] const OptionValue* find(std::string_view key) const noexcept;

    // Typed reads. Each coerces across the numeric arms (an int read as a double, a double read
    // as an int) because a preset round-trip through JSON cannot preserve the distinction; a
    // genuinely mismatched arm (a string read as an int) yields `fallback`, never a guess.
    [[nodiscard]] bool boolean(std::string_view key, bool fallback = false) const noexcept;
    [[nodiscard]] int integer(std::string_view key, int fallback = 0) const noexcept;
    [[nodiscard]] double number(std::string_view key, double fallback = 0.0) const noexcept;
    [[nodiscard]] std::string text(std::string_view key, std::string fallback = {}) const;

    // The value's canonical, LOCALE-INDEPENDENT string form -- "true"/"false" for bools, decimal
    // for ints, common::gToString for reals, the string itself otherwise. This is what an
    // OptionCondition compares against and what a preset writer should store. Empty when absent.
    [[nodiscard]] std::string asString(std::string_view key) const;

    [[nodiscard]] const std::map<std::string, OptionValue, std::less<>>& all() const noexcept {
        return m_values;
    }

    bool operator==(const OptionValues&) const = default;

private:
    std::map<std::string, OptionValue, std::less<>> m_values;
};

// The canonical string form of a standalone value (the same rule OptionValues::asString uses).
[[nodiscard]] std::string optionValueToString(const OptionValue& value);

// ---------------------------------------------------------------------------------------------
// Descriptors
// ---------------------------------------------------------------------------------------------

enum class OptionType : std::uint8_t {
    Bool,  // a checkbox
    Int,   // an integer in [min, max] stepping by `step`
    Real,  // a double in [min, max]; `decimals` is display precision only
    Enum,  // one of `choices`, stored by choice id
    Text,  // free-form string (PNG text chunks, HDR header variables)
};

// A HINT for the panel; never load-bearing. `Auto` lets the renderer pick from the type and the
// range (a 0-9 int is a stepper, a 0-100 int is a slider), which is what M3 will do by default.
enum class OptionWidget : std::uint8_t {
    Auto,
    Checkbox,
    Slider,
    Stepper,
    Dropdown,
    RadioRow,
    TextField,
};

// One choice of an Enum option. `id` is stable and untranslated (it is the stored value);
// `label` is the English source string the UI shows and M3 hands to gettext.
struct EnumChoice {
    std::string id;
    std::string label;
    std::string help;  // optional one-liner for a tooltip
    bool operator==(const EnumChoice&) const = default;
};

// "Show this option only while <key> is (or is not) one of <values>." Values are compared in the
// canonical string form (OptionValues::asString), so a bool source is matched against "true" /
// "false" and an enum source against a choice id. Reals are a poor condition source (formatting
// round-trips) -- validateSchema() rejects them.
struct OptionCondition {
    std::string key;
    std::vector<std::string> values;
    bool negate = false;  // visible when the value is NOT among `values`
    bool operator==(const OptionCondition&) const = default;
};

// A collapsible section of the panel. The empty group id is the always-visible common section
// (§6.5: "the common knob is always visible; Advanced is collapsed by default").
struct OptionGroup {
    std::string id;
    std::string label;                 // English source string
    bool collapsedByDefault = true;
    bool operator==(const OptionGroup&) const = default;
};

struct OptionDesc {
    std::string key;    // stable, untranslated: the OptionValues key and the preset key
    std::string label;  // English source string (M3 translates)
    std::string help;   // optional one-line explanation for a tooltip
    OptionType type = OptionType::Bool;
    OptionWidget widget = OptionWidget::Auto;
    std::string group;  // "" = the always-visible common section
    OptionValue defaultValue = false;

    // Int/Real only. `step` is the UI increment (and the value grid Int coercion snaps to);
    // `decimals` is Real display precision; `unit` is a suffix the panel appends ("%", "px").
    double min = 0.0;
    double max = 0.0;
    double step = 1.0;
    int decimals = 0;
    std::string unit;

    std::vector<EnumChoice> choices;            // Enum only
    std::vector<OptionCondition> visibleWhen;   // ALL must hold (AND); empty = always visible

    bool operator==(const OptionDesc&) const = default;
};

// The whole option surface of one backend.
struct OptionsSchema {
    std::vector<OptionGroup> groups;   // panel order; the common section is implicit
    std::vector<OptionDesc> options;   // panel order within each group

    [[nodiscard]] const OptionDesc* find(std::string_view key) const noexcept;

    // Every option's default, as a complete value bag. This is what a backend encodes with when
    // the caller supplies nothing.
    [[nodiscard]] OptionValues defaults() const;

    // Make `values` a valid, COMPLETE answer to this schema: fill missing keys from the defaults,
    // drop keys the schema does not declare (so a preset aimed at another format cannot leak
    // through), replace wrong-typed arms with the default, clamp Int/Real into [min, max] and
    // snap Int to the `step` grid, and snap an Enum whose id is not among `choices` back to its
    // default. After this, every typed read in encode() is safe without further checking.
    void coerce(OptionValues& values) const;

    // Whether `desc` should be shown/honoured given the current `values` (all of its
    // visibleWhen conditions hold). An option hidden by its condition still HAS a value --
    // hiding is a UI affordance, not a deletion -- so encode() must consult this too before
    // acting on a dependent knob.
    [[nodiscard]] bool visible(const OptionDesc& desc, const OptionValues& values) const;
    [[nodiscard]] bool visible(std::string_view key, const OptionValues& values) const;

    bool operator==(const OptionsSchema&) const = default;
};

// Structural problems in a schema, as human-readable lines; empty means the schema is sound.
// Catches the mistakes that would otherwise surface as a silently broken panel: duplicate keys,
// an empty key or label, min > max, a default outside the range or of the wrong type, an Enum
// with no choices / duplicate choice ids / a default that is not a choice id, a condition
// naming an unknown or self key, a condition on a Real source, and an option in an undeclared
// group. tests/test_export_registry.cpp runs this over every registered backend.
[[nodiscard]] std::vector<std::string> validateSchema(const OptionsSchema& schema);

// ---------------------------------------------------------------------------------------------
// Well-known option keys
// ---------------------------------------------------------------------------------------------
// The loss diff (io/caps.hpp) has to answer "is this encode lossy?" without knowing any format.
// It does that through a tiny shared vocabulary: a backend that has one of these CONCEPTS must
// spell it with this key and these units. Everything else a backend invents freely.
//
//   kOptQuality      Int 0..100, higher is better; 100 means "the encoder's best" (libjpeg,
//                    libwebp, libavif qcolor)
//   kOptDistance     Real >= 0, butteraugli distance; 0 means mathematically lossless (libjxl)
//   kOptLossless     Bool; true means no generation loss (libjxl, libwebp, libavif, TIFF)
//   kOptSubsampling  Enum of chroma-subsampling ratios spelled EXACTLY "4:4:4", "4:2:2",
//                    "4:2:0", "4:4:0", "4:1:1"; "4:4:4" is the no-loss choice
//   kOptNearLossless Int 0..100, 100 = off (libwebp's near_lossless). It is the one knob that
//                    makes a LOSSLESS mode lossy -- a "lossless" WebP at near-lossless 60 is
//                    not bit-exact -- so the loss diff has to know about it by name, or the
//                    banner would promise an exactness the file does not have.
inline constexpr std::string_view kOptQuality = "quality";
inline constexpr std::string_view kOptDistance = "distance";
inline constexpr std::string_view kOptLossless = "lossless";
inline constexpr std::string_view kOptSubsampling = "subsampling";
inline constexpr std::string_view kOptNearLossless = "near-lossless";
inline constexpr std::string_view kSubsampling444 = "4:4:4";

} // namespace mosaic::io
