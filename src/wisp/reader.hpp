// SPDX-License-Identifier: AGPL-3.0-or-later
// C++ port of mbrock/wisp's core/sexp-read.zig and core/sexp.zig,
// revision 223535633179cdf2a49391820bdab16a5db5bf4e.
#pragma once

#include "wisp/eval.hpp"

#include <optional>
#include <string>

namespace wisp {

struct read_error : std::runtime_error
{
    read_error(std::size_t offset, std::string_view message);
    const std::size_t offset;
};

/// An owning UTF-8 source reader. Copying input before any guest allocation
/// permits construction from a borrowed heap string. The heap and evaluator
/// must belong together and outlive the reader. Names fold ASCII case only.
class reader
{
public:
    reader(heap & storage, evaluator & language, std::string_view source);

    /// EOF is nullopt, distinct from NIL. Never collects; returned words
    /// are unrooted. No guest values survive between calls, so callers may
    /// collect between forms. After read_error, cursor recovery is not
    /// specified; partial allocations/interning are not rolled back.
    std::optional<word> next();

    /// Byte offset just after the last form; trailing space is consumed by
    /// the next call, not this accessor.
    std::size_t position() const noexcept
    {
        return position_;
    }

private:
    static constexpr char32_t eof = 0x110000;

    char32_t peek() const;
    char32_t take();
    void space();
    std::string name();
    word symbol();
    word number();
    word string();

    heap & heap_;
    evaluator & evaluator_;
    std::string source_;
    std::size_t position_ = 0;
};

} // namespace wisp
