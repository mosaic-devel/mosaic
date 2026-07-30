#pragma once

#include "io/format_backend.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

// io/format_registry -- the one place that knows which formats exist (Export & I/O plan §2.1).
//
// Construction is EXPLICIT (see format_registry.cpp), not a set of self-registering static
// objects: mosaic_io is a static library, and a translation unit whose only contribution is a
// namespace-scope registrar object is exactly what the linker is entitled to drop unless the
// whole archive is force-linked. An explicit list also gives the combobox a deterministic order
// and lets a test enumerate the backends without touching global state.
namespace mosaic::io {

class FormatRegistry {
public:
    // The process-wide registry. Built once, immutable afterwards, so it is safe to hand out
    // raw backend pointers -- they outlive every caller.
    [[nodiscard]] static const FormatRegistry& instance();

    // A registry of your own, for tests that want to exercise the ordering rules over a known
    // set. `add` returns false (and ignores the backend) on a duplicate FormatId.
    FormatRegistry() = default;
    bool add(std::unique_ptr<FormatBackend> backend);

    [[nodiscard]] std::size_t size() const noexcept { return m_backends.size(); }

    [[nodiscard]] const FormatBackend* find(FormatId id) const noexcept;
    // Extension lookup, case-insensitive, with or without a leading dot ("png", ".PNG").
    [[nodiscard]] const FormatBackend* findByExtension(std::string_view ext) const noexcept;
    // The backend that claims `path`'s extension, or nullptr.
    [[nodiscard]] const FormatBackend* findByPath(std::string_view path) const noexcept;

    // Every registered backend, in registration order, available or not.
    [[nodiscard]] std::vector<const FormatBackend*> all() const;

    // The export combobox order (§3): the Common tier first in its declared popularity order,
    // then Curated pro and -- only when `includeExotic` -- the exotic tier, both alphabetical by
    // display name. Backends whose available() is false are omitted entirely.
    [[nodiscard]] std::vector<const FormatBackend*> exportOrder(bool includeExotic = false) const;

private:
    std::vector<std::unique_ptr<FormatBackend>> m_backends;
};

// The lower-cased extension of `path` without its dot ("" when there is none). Shared by the
// registry and the Export modal's "does the typed path match the chosen format" check.
[[nodiscard]] std::string extensionOf(std::string_view path);

} // namespace mosaic::io
