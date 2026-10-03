#include "nxtui/glyph-table.hpp"

#include <stdexcept>
#include <utility>

namespace nxtui {

GlyphTable::GlyphTable()
{
    init_ascii();
}

void GlyphTable::init_ascii()
{
    table_.reserve(256);

    for (std::uint32_t i = 0; i <= ASCII_MAX; ++i) {
        auto bytes = std::string(1, static_cast<char>(i));
        glyphs_.push_back(bytes);
        table_.emplace(std::move(bytes), static_cast<GlyphId>(i));
    }
}

GlyphTable::GlyphId GlyphTable::intern(const std::string_view bytes)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // ASCII IDs are self-mapped, and clear() may run concurrently.
    if (bytes.size() == 1)
        return static_cast<unsigned char>(bytes[0]);

    // Check if already interned
    if (const auto it = table_.find(std::string(bytes)); it != table_.end())
        return it->second;

    // Intern new glyph
    if (bytes.size() > 255)
        throw std::length_error("Glyph too long (max 255 bytes)");

    glyphs_.emplace_back(bytes);
    auto id = static_cast<GlyphId>(glyphs_.size() - 1);

    table_.emplace(std::string(bytes), id);

    return id;
}

std::optional<std::span<const char>>
GlyphTable::get_span(const GlyphId id) const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (id >= glyphs_.size())
        return std::nullopt;

    const auto & glyph = glyphs_[id];
    return std::span{glyph.data(), glyph.size()};
}

std::optional<std::string_view>
GlyphTable::get(const GlyphId id) const noexcept
{
    if (const auto span = get_span(id))
        return std::string_view{span->data(), span->size()};
    return std::nullopt;
}

std::string_view GlyphTable::operator[](const GlyphId id) const
{
    if (const auto sv = get(id))
        return *sv;
    throw std::out_of_range("Invalid GlyphId");
}

std::size_t GlyphTable::size() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return glyphs_.size();
}

void GlyphTable::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    table_.clear();
    glyphs_.clear();
    init_ascii();
}

} // namespace nxtui
