#include <nxtio/short_id.hpp>

#if defined(__linux__)
#include <sys/random.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) ||    \
    defined(__NetBSD__)
#include <stdlib.h>
#else
#include <random>
#endif

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace nxt::io {
namespace {

// Crockford base32: digits then A-Z with I, L, O, U removed. Reading
// these aloud is unambiguous because every glyph is visually distinct.
constexpr char crockford[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
static_assert(sizeof(crockford) - 1 == 32);

void fill_random(std::uint8_t * out, std::size_t n)
{
#if defined(__linux__)
    std::size_t filled = 0;
    while (filled < n) {
        auto got = ::getrandom(out + filled, n - filled, 0);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            throw std::runtime_error{
                std::string{"getrandom failed: "} + std::strerror(errno)};
        }
        filled += static_cast<std::size_t>(got);
    }
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) ||    \
    defined(__NetBSD__)
    ::arc4random_buf(out, n);
#else
    std::random_device rng;
    for (std::size_t i = 0; i < n; ++i)
        out[i] = static_cast<std::uint8_t>(rng());
#endif
}

} // namespace

std::string encode_short_id(std::uint64_t value)
{
    // 8 characters times 5 bits per character equals 40 bits. We mask
    // to that width so callers passing larger counters get a stable
    // suffix; the high bits would otherwise silently disappear.
    constexpr std::uint64_t mask = (std::uint64_t{1} << 40) - 1;
    value &= mask;
    std::string out(8, '0');
    for (int i = 7; i >= 0; --i) {
        out[i] = crockford[value & 0x1f];
        value >>= 5;
    }
    return out;
}

std::string make_short_id()
{
    std::array<std::uint8_t, 5> bytes{};
    fill_random(bytes.data(), bytes.size());
    std::uint64_t v = 0;
    for (auto b : bytes)
        v = (v << 8) | b;
    return encode_short_id(v);
}

} // namespace nxt::io
