#include "io/options_schema.hpp"

#include "common/charconv_compat.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace mosaic::io {

namespace {

// Round `v` to the nearest multiple of `step` measured from `min` -- the grid an IntStepper /
// slider actually offers. A non-positive or non-finite step means "no grid".
[[nodiscard]] int snapToStep(double v, double min, double step) {
    if (!(step > 0.0) || !std::isfinite(step))
        return static_cast<int>(std::lround(v));
    const double n = std::round((v - min) / step);
    return static_cast<int>(std::lround(min + n * step));
}

} // namespace

// ---------------------------------------------------------------------------------------------
// OptionValues
// ---------------------------------------------------------------------------------------------

bool OptionValues::has(std::string_view key) const noexcept {
    return m_values.find(key) != m_values.end();
}

void OptionValues::set(std::string_view key, OptionValue value) {
    if (auto it = m_values.find(key); it != m_values.end()) {
        it->second = std::move(value);
        return;
    }
    m_values.emplace(std::string(key), std::move(value));
}

void OptionValues::erase(std::string_view key) {
    if (auto it = m_values.find(key); it != m_values.end())
        m_values.erase(it);
}

const OptionValue* OptionValues::find(std::string_view key) const noexcept {
    const auto it = m_values.find(key);
    return it == m_values.end() ? nullptr : &it->second;
}

bool OptionValues::boolean(std::string_view key, bool fallback) const noexcept {
    const OptionValue* v = find(key);
    if (v == nullptr)
        return fallback;
    if (const bool* b = std::get_if<bool>(v))
        return *b;
    // A bool that survived a JSON round-trip as 0/1 still reads as the bool it was.
    if (const int* i = std::get_if<int>(v))
        return *i != 0;
    return fallback;
}

int OptionValues::integer(std::string_view key, int fallback) const noexcept {
    const OptionValue* v = find(key);
    if (v == nullptr)
        return fallback;
    if (const int* i = std::get_if<int>(v))
        return *i;
    if (const double* d = std::get_if<double>(v)) {
        if (!std::isfinite(*d))
            return fallback;
        return static_cast<int>(std::lround(*d));
    }
    return fallback;
}

double OptionValues::number(std::string_view key, double fallback) const noexcept {
    const OptionValue* v = find(key);
    if (v == nullptr)
        return fallback;
    if (const double* d = std::get_if<double>(v))
        return *d;
    if (const int* i = std::get_if<int>(v))
        return static_cast<double>(*i);
    return fallback;
}

std::string OptionValues::text(std::string_view key, std::string fallback) const {
    const OptionValue* v = find(key);
    if (v == nullptr)
        return fallback;
    if (const std::string* s = std::get_if<std::string>(v))
        return *s;
    return fallback;
}

std::string OptionValues::asString(std::string_view key) const {
    const OptionValue* v = find(key);
    return v == nullptr ? std::string() : optionValueToString(*v);
}

std::string optionValueToString(const OptionValue& value) {
    // std::visit would do, but the arms want different formatting rules and gToString is the
    // locale-safe one (i18n moves LC_NUMERIC; see common/charconv_compat.hpp).
    if (const bool* b = std::get_if<bool>(&value))
        return *b ? "true" : "false";
    if (const int* i = std::get_if<int>(&value))
        return std::to_string(*i);
    if (const double* d = std::get_if<double>(&value))
        return common::gToString(*d);
    if (const std::string* s = std::get_if<std::string>(&value))
        return *s;
    return {};
}

// ---------------------------------------------------------------------------------------------
// OptionsSchema
// ---------------------------------------------------------------------------------------------

const OptionDesc* OptionsSchema::find(std::string_view key) const noexcept {
    for (const OptionDesc& d : options)
        if (d.key == key)
            return &d;
    return nullptr;
}

OptionValues OptionsSchema::defaults() const {
    OptionValues v;
    for (const OptionDesc& d : options)
        v.set(d.key, d.defaultValue);
    coerce(v); // the declared defaults are clamped by the same rules as user input
    return v;
}

void OptionsSchema::coerce(OptionValues& values) const {
    OptionValues out;
    for (const OptionDesc& d : options) {
        const OptionValue* given = values.find(d.key);
        switch (d.type) {
        case OptionType::Bool: {
            const bool fallback = std::holds_alternative<bool>(d.defaultValue) &&
                                  std::get<bool>(d.defaultValue);
            const bool b = given == nullptr ? fallback : values.boolean(d.key, fallback);
            out.set(d.key, boolValue(b));
            break;
        }
        case OptionType::Int: {
            const int fallback = std::holds_alternative<int>(d.defaultValue)
                                     ? std::get<int>(d.defaultValue)
                                     : static_cast<int>(std::lround(d.min));
            int n = given == nullptr ? fallback : values.integer(d.key, fallback);
            if (d.max >= d.min) {
                n = snapToStep(static_cast<double>(n), d.min, d.step);
                n = std::clamp(n, static_cast<int>(std::ceil(d.min)),
                               static_cast<int>(std::floor(d.max)));
            }
            out.set(d.key, intValue(n));
            break;
        }
        case OptionType::Real: {
            const double fallback =
                std::holds_alternative<double>(d.defaultValue) ? std::get<double>(d.defaultValue)
                : std::holds_alternative<int>(d.defaultValue)
                    ? static_cast<double>(std::get<int>(d.defaultValue))
                    : d.min;
            double x = given == nullptr ? fallback : values.number(d.key, fallback);
            if (!std::isfinite(x))
                x = fallback;
            if (d.max >= d.min)
                x = std::clamp(x, d.min, d.max);
            out.set(d.key, realValue(x));
            break;
        }
        case OptionType::Enum: {
            std::string fallback = std::holds_alternative<std::string>(d.defaultValue)
                                       ? std::get<std::string>(d.defaultValue)
                                       : std::string();
            if (fallback.empty() && !d.choices.empty())
                fallback = d.choices.front().id;
            std::string id = given == nullptr ? fallback : values.text(d.key, fallback);
            const bool known = std::any_of(d.choices.begin(), d.choices.end(),
                                           [&](const EnumChoice& c) { return c.id == id; });
            out.set(d.key, textValue(known ? std::move(id) : std::move(fallback)));
            break;
        }
        case OptionType::Text: {
            std::string fallback = std::holds_alternative<std::string>(d.defaultValue)
                                       ? std::get<std::string>(d.defaultValue)
                                       : std::string();
            std::string s = given == nullptr ? std::move(fallback)
                                             : values.text(d.key, std::move(fallback));
            out.set(d.key, textValue(std::move(s)));
            break;
        }
        }
    }
    values = std::move(out); // keys the schema does not declare are dropped, by construction
}

bool OptionsSchema::visible(const OptionDesc& desc, const OptionValues& values) const {
    for (const OptionCondition& c : desc.visibleWhen) {
        const std::string actual = values.asString(c.key);
        const bool hit = std::find(c.values.begin(), c.values.end(), actual) != c.values.end();
        if (hit == c.negate)
            return false;
    }
    return true;
}

bool OptionsSchema::visible(std::string_view key, const OptionValues& values) const {
    const OptionDesc* d = find(key);
    return d != nullptr && visible(*d, values);
}

// ---------------------------------------------------------------------------------------------
// validateSchema
// ---------------------------------------------------------------------------------------------

std::vector<std::string> validateSchema(const OptionsSchema& schema) {
    std::vector<std::string> problems;
    const auto report = [&](const std::string& key, const char* what) {
        problems.push_back("option '" + key + "': " + what);
    };

    std::set<std::string> groupIds;
    for (const OptionGroup& g : schema.groups) {
        if (g.id.empty())
            problems.emplace_back("group: the empty id is reserved for the common section");
        else if (!groupIds.insert(g.id).second)
            problems.push_back("group '" + g.id + "': duplicate id");
        if (g.label.empty())
            problems.push_back("group '" + g.id + "': empty label");
    }

    std::set<std::string> keys;
    for (const OptionDesc& d : schema.options) {
        if (d.key.empty()) {
            problems.emplace_back("option: empty key");
            continue;
        }
        if (!keys.insert(d.key).second)
            report(d.key, "duplicate key");
        if (d.label.empty())
            report(d.key, "empty label");
        if (!d.group.empty() && groupIds.count(d.group) == 0)
            report(d.key, "belongs to a group the schema does not declare");

        switch (d.type) {
        case OptionType::Bool:
            if (!std::holds_alternative<bool>(d.defaultValue))
                report(d.key, "Bool option with a non-bool default");
            break;
        case OptionType::Int:
            if (!std::holds_alternative<int>(d.defaultValue)) {
                report(d.key, "Int option with a non-int default");
            } else if (d.max >= d.min) {
                const double n = static_cast<double>(std::get<int>(d.defaultValue));
                if (n < d.min || n > d.max)
                    report(d.key, "default is outside [min, max]");
            }
            if (d.max < d.min)
                report(d.key, "max is below min");
            if (!(d.step > 0.0))
                report(d.key, "step must be positive");
            break;
        case OptionType::Real:
            if (!std::holds_alternative<double>(d.defaultValue)) {
                report(d.key, "Real option with a non-double default");
            } else if (d.max >= d.min) {
                const double x = std::get<double>(d.defaultValue);
                if (x < d.min || x > d.max)
                    report(d.key, "default is outside [min, max]");
            }
            if (d.max < d.min)
                report(d.key, "max is below min");
            break;
        case OptionType::Enum: {
            if (d.choices.empty()) {
                report(d.key, "Enum option with no choices");
                break;
            }
            std::set<std::string> ids;
            for (const EnumChoice& c : d.choices) {
                if (c.id.empty())
                    report(d.key, "enum choice with an empty id");
                else if (!ids.insert(c.id).second)
                    report(d.key, "duplicate enum choice id");
                if (c.label.empty())
                    report(d.key, "enum choice with an empty label");
            }
            if (!std::holds_alternative<std::string>(d.defaultValue))
                report(d.key, "Enum option with a non-string default");
            else if (ids.count(std::get<std::string>(d.defaultValue)) == 0)
                report(d.key, "default is not one of the choice ids");
            break;
        }
        case OptionType::Text:
            if (!std::holds_alternative<std::string>(d.defaultValue))
                report(d.key, "Text option with a non-string default");
            break;
        }
    }

    // Conditions are checked in a second pass so a forward reference is legal (panel order and
    // dependency order need not agree).
    for (const OptionDesc& d : schema.options) {
        for (const OptionCondition& c : d.visibleWhen) {
            if (c.key.empty()) {
                report(d.key, "visibility condition with an empty key");
                continue;
            }
            if (c.key == d.key) {
                report(d.key, "visibility condition on itself");
                continue;
            }
            const OptionDesc* src = schema.find(c.key);
            if (src == nullptr) {
                report(d.key, "visibility condition names an option the schema does not declare");
                continue;
            }
            if (src->type == OptionType::Real)
                report(d.key, "visibility condition on a Real option (formatting is not exact)");
            if (c.values.empty())
                report(d.key, "visibility condition with no values");
            if (src->type == OptionType::Enum) {
                for (const std::string& want : c.values) {
                    const bool known =
                        std::any_of(src->choices.begin(), src->choices.end(),
                                    [&](const EnumChoice& ch) { return ch.id == want; });
                    if (!known)
                        report(d.key, "visibility condition wants an enum id that does not exist");
                }
            }
            if (src->type == OptionType::Bool) {
                for (const std::string& want : c.values)
                    if (want != "true" && want != "false")
                        report(d.key, "visibility condition on a Bool wants something other than "
                                      "\"true\"/\"false\"");
            }
        }
    }
    return problems;
}

} // namespace mosaic::io
