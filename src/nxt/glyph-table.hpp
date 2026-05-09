#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace nxt {

/// Unicode string interning table for terminal glyphs.
/// Maps UTF-8 sequences to 32-bit glyph IDs.
/// IDs 0-255 are reserved for single-byte ASCII (self-mapped).
class GlyphTable
{
public:
    using GlyphId = std::uint32_t;

    /// ASCII values 0-255 are self-mapped
    static constexpr GlyphId ASCII_MAX = 255;

    /// Initialize with pre-populated ASCII 0-255
    GlyphTable();

    /// Rule of 5: GlyphTable is non-copyable, non-movable (owns mutex)
    GlyphTable(const GlyphTable &) = delete;
    GlyphTable & operator=(const GlyphTable &) = delete;
    GlyphTable(GlyphTable &&) = delete;
    GlyphTable & operator=(GlyphTable &&) = delete;
    ~GlyphTable() = default;

    /// Intern a UTF-8 string, returning its GlyphId.
    /// Single-byte strings are fast-pathed to return the byte value.
    /// @throws std::length_error if bytes.size() > 255
    [[nodiscard]] GlyphId intern(std::string_view bytes);

    /// Get the UTF-8 bytes for a glyph ID as a span.
    /// Returns nullopt if ID is invalid.
    [[nodiscard]] std::optional<std::span<const char>>
    get_span(GlyphId id) const noexcept;

    /// Get the UTF-8 bytes for a glyph ID as a string_view.
    /// Returns nullopt if ID is invalid.
    [[nodiscard]] std::optional<std::string_view>
    get(GlyphId id) const noexcept;

    /// Get the UTF-8 bytes for a glyph ID.
    /// @throws std::out_of_range if ID is invalid
    [[nodiscard]] std::string_view operator[](GlyphId id) const;

    /// Number of interned glyphs
    [[nodiscard]] std::size_t size() const noexcept;

    /// Clear interned glyphs and restore the reserved ASCII entries.
    void clear();

private:
    struct Span
    {
        std::uint32_t offset;
        std::uint8_t length;
    };

    void init_ascii();

    /// Arena holds all UTF-8 bytes contiguously
    std::vector<char> arena_;

    /// Parallel array: span info per glyph ID (index == ID)
    std::vector<Span> spans_;

    /// Hash table for fast lookup. Keys own their bytes; reverse lookup
    /// still uses spans into arena_.
    std::unordered_map<std::string, GlyphId> table_;

    /// Mutex for thread-safe interning (multiple widgets may render
    /// concurrently)
    mutable std::mutex mutex_;
};

} // namespace nxt
