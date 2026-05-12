#pragma once

#include <cstdint>
#include <string>

namespace nxt::io {

/// Generate a typeable 8-character Crockford base32 identifier from
/// system randomness. Crockford excludes the visually-confusable letters
/// `I`, `L`, `O`, `U`, so the produced ids can be read aloud or typed
/// without ambiguity. 40 bits of entropy is not safe against an
/// adversary, but is more than enough for spans and run ids within a
/// single process run.
[[nodiscard]] std::string make_short_id();

/// Encode the low 40 bits of `value` as a Crockford base32 string. Used
/// for tests and for callers that already have a unique counter and
/// just want a stable string form.
[[nodiscard]] std::string encode_short_id(std::uint64_t value);

} // namespace nxt::io
