#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// A bounded big-endian cursor for the binary tip formats (io/brush/tip_io.cpp, abr.cpp). The
// input is a third-party file: every read is bounds-checked, failure is sticky (`ok` stays false
// and every later read returns 0), and there is deliberately NO way to read outside [data, size).
// Callers check ok() at decision points instead of wrapping every single read.
namespace mosaic::io::brush::detail {

class ByteReader {
public:
    ByteReader(const std::uint8_t* data, std::size_t size) noexcept : m_data(data), m_size(size) {}

    [[nodiscard]] bool ok() const noexcept { return m_ok; }
    [[nodiscard]] std::size_t pos() const noexcept { return m_pos; }
    [[nodiscard]] std::size_t remaining() const noexcept { return m_size - m_pos; }

    [[nodiscard]] std::uint8_t u8() noexcept {
        if (!ensure(1))
            return 0;
        return m_data[m_pos++];
    }

    [[nodiscard]] std::uint16_t u16be() noexcept {
        if (!ensure(2))
            return 0;
        const std::uint16_t v = static_cast<std::uint16_t>((m_data[m_pos] << 8) | m_data[m_pos + 1]);
        m_pos += 2;
        return v;
    }

    [[nodiscard]] std::uint32_t u32be() noexcept {
        if (!ensure(4))
            return 0;
        const std::uint32_t v = (static_cast<std::uint32_t>(m_data[m_pos]) << 24) |
                                (static_cast<std::uint32_t>(m_data[m_pos + 1]) << 16) |
                                (static_cast<std::uint32_t>(m_data[m_pos + 2]) << 8) |
                                static_cast<std::uint32_t>(m_data[m_pos + 3]);
        m_pos += 4;
        return v;
    }

    [[nodiscard]] std::int32_t i32be() noexcept { return static_cast<std::int32_t>(u32be()); }
    [[nodiscard]] std::int16_t i16be() noexcept { return static_cast<std::int16_t>(u16be()); }

    // The little-endian pair (ZIP local/central headers, io/brush/zip.cpp).
    [[nodiscard]] std::uint16_t u16le() noexcept {
        if (!ensure(2))
            return 0;
        const std::uint16_t v =
            static_cast<std::uint16_t>(m_data[m_pos] | (m_data[m_pos + 1] << 8));
        m_pos += 2;
        return v;
    }

    [[nodiscard]] std::uint32_t u32le() noexcept {
        if (!ensure(4))
            return 0;
        const std::uint32_t v = static_cast<std::uint32_t>(m_data[m_pos]) |
                                (static_cast<std::uint32_t>(m_data[m_pos + 1]) << 8) |
                                (static_cast<std::uint32_t>(m_data[m_pos + 2]) << 16) |
                                (static_cast<std::uint32_t>(m_data[m_pos + 3]) << 24);
        m_pos += 4;
        return v;
    }

    // A view into the buffer, or nullptr (and sticky failure) when `n` bytes are not there.
    [[nodiscard]] const std::uint8_t* bytes(std::size_t n) noexcept {
        if (!ensure(n))
            return nullptr;
        const std::uint8_t* p = m_data + m_pos;
        m_pos += n;
        return p;
    }

    void skip(std::size_t n) noexcept { (void)bytes(n); }

    // Absolute reposition; only within the buffer.
    void seek(std::size_t to) noexcept {
        if (to > m_size)
            m_ok = false;
        else
            m_pos = to;
    }

private:
    [[nodiscard]] bool ensure(std::size_t n) noexcept {
        if (!m_ok || n > m_size - m_pos) {
            m_ok = false;
            return false;
        }
        return true;
    }

    const std::uint8_t* m_data = nullptr;
    std::size_t m_size = 0;
    std::size_t m_pos = 0;
    bool m_ok = true;
};

} // namespace mosaic::io::brush::detail
