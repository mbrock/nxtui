// SPDX-License-Identifier: AGPL-3.0-or-later
// C++ port of mbrock/wisp's core/word.zig.
#pragma once

#include <cassert>
#include <cstdint>

namespace wisp {

using word = std::uint32_t;

// These are format identities, not positions in the vat tuple.
enum class tag : word {
    integer = 0x00,
    sys = 0x11,
    chr = 0x12,
    jet = 0x13,
    rec = 0x14, // record pointer; an NXT extension, absent from Zig Wisp
    duo = 0x15,
    sym = 0x16,
    fun = 0x17,
    mac = 0x18,
    v32 = 0x19,
    v08 = 0x1a,
    pkg = 0x1b,
    run = 0x1c,
    ktx = 0x1d,
    ext = 0x1e,
    pin = 0x1f,
};

inline constexpr word max_index = (word{1} << 26) - 1;
inline constexpr word max_immediate = (word{1} << 27) - 1;
inline constexpr std::int32_t min_fixnum = -(std::int32_t{1} << 30);
inline constexpr std::int32_t max_fixnum = (std::int32_t{1} << 30) - 1;

constexpr tag tag_of(word x) noexcept
{
    return (x & 0x80000000u) == 0 ? tag::integer : tag(x >> 27);
}

constexpr bool is_pointer(tag t) noexcept
{
    return t == tag::rec || (t >= tag::duo && t <= tag::ext);
}

constexpr word fixnum(std::int32_t x) noexcept
{
    assert(x >= min_fixnum && x <= max_fixnum);
    return static_cast<word>(x) & 0x7fffffffu;
}

constexpr std::int32_t integer(word x) noexcept
{
    assert(tag_of(x) == tag::integer);
    // Sign extension of bit 30, without implementation-defined casts.
    return static_cast<std::int32_t>(x & 0x3fffffffu)
           - static_cast<std::int32_t>(x & 0x40000000u);
}

constexpr word immediate(tag t, word payload) noexcept
{
    assert(
        t == tag::sys || t == tag::chr || t == tag::jet || t == tag::pin);
    assert(payload <= max_immediate);
    return (word(t) << 27) | payload;
}

constexpr word pointer(tag t, word index, bool era) noexcept
{
    assert(is_pointer(t) && index <= max_index);
    return (word(t) << 27) | (index << 1) | word(era);
}

constexpr word index_of(word x) noexcept
{
    assert(is_pointer(tag_of(x)));
    return (x >> 1) & max_index;
}

constexpr bool era_of(word x) noexcept
{
    assert(is_pointer(tag_of(x)));
    return (x & 1) != 0;
}

constexpr word payload_of(word x) noexcept
{
    assert(!is_pointer(tag_of(x)) && tag_of(x) != tag::integer);
    return x & max_immediate;
}

inline constexpr word nil = immediate(tag::sys, 0);
inline constexpr word t = immediate(tag::sys, 1);
inline constexpr word nah = immediate(tag::sys, 2);
inline constexpr word zap = immediate(tag::sys, 3);
inline constexpr word top = immediate(tag::sys, 4);

} // namespace wisp
