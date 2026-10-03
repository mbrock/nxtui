#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace nxtui {

/// Interning table mapping the UTF-8 bytes of one terminal glyph (a
/// grapheme cluster) to a 32-bit id, so raster cells can store an integer.
///
/// Ids 0-255 are reserved: every single-byte string maps to its own byte
/// value without touching the table. Longer strings get the next free id on
/// first `intern` and keep it until `clear()`. Wide glyphs are followed in a
/// raster by continuation cells holding the id of the empty string.
///
/// Rasters and views borrow the table, so it must outlive them, and every
/// raster that is diffed against another must share the same table. The
/// table is neither copyable nor movable.
///
/// Threading: interning, lookups, size, and clear take an internal mutex.
/// Returned views remain valid across interning, but clear invalidates
/// them; callers must synchronize clear with use of those views.
class GlyphTable
{
public:
    /// Glyph identifier stored in raster cells.
    using GlyphId = std::uint32_t;

    /// Largest reserved single-byte id.
    static constexpr GlyphId ASCII_MAX = 255;

    /// Create a table holding only the 256 reserved single-byte entries.
    GlyphTable();

    GlyphTable(const GlyphTable &) = delete;
    GlyphTable & operator=(const GlyphTable &) = delete;
    GlyphTable(GlyphTable &&) = delete;
    GlyphTable & operator=(GlyphTable &&) = delete;
    ~GlyphTable() = default;

    /// Return the id for `bytes`, adding it if new.
    /// Single-byte strings return the byte value directly.
    /// @throws std::length_error if a new glyph is longer than 255 bytes.
    [[nodiscard]] GlyphId intern(std::string_view bytes);

    /// UTF-8 bytes for `id`, or `std::nullopt` for an unknown id.
    /// The returned view remains valid across intern() calls, but is
    /// invalidated by clear(); callers must not race clear() with using it.
    [[nodiscard]] std::optional<std::span<const char>>
    get_span(GlyphId id) const noexcept;

    /// UTF-8 bytes for `id`, or `std::nullopt` for an unknown id.
    [[nodiscard]] std::optional<std::string_view>
    get(GlyphId id) const noexcept;

    /// UTF-8 bytes for `id`.
    /// @throws std::out_of_range if `id` is unknown.
    /// The returned view remains valid across intern() calls, but is
    /// invalidated by clear(); callers must not race clear() with using it.
    [[nodiscard]] std::string_view operator[](GlyphId id) const;

    /// Number of ids in use, including the 256 reserved ones.
    [[nodiscard]] std::size_t size() const noexcept;

    /// Forget every interned glyph and keep only the reserved entries.
    /// Ids handed out earlier become invalid; rasters holding them must be
    /// cleared too.
    void clear();

private:
    void init_ascii();

    /// Individually owned strings keep returned views stable as glyphs
    /// grow.
    std::deque<std::string> glyphs_;

    /// Hash table for fast lookup. Keys own their bytes; reverse lookup
    /// uses the individually owned strings in glyphs_.
    std::unordered_map<std::string, GlyphId> table_;

    /// Serializes interning, lookups, size, and clear.
    mutable std::mutex mutex_;
};

} // namespace nxtui
