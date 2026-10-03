// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "wisp/reader.hpp"

namespace wisp {

/// The guest standard library, embedded in the binary. Loading is explicit;
/// constructing an evaluator installs only primitives.
std::string_view base_library() noexcept;

/// The guest compiler from RFC 0020, which analyzes source forms into
/// semantic records. Load it after the base library.
std::string_view compiler_library() noexcept;

/// Read and evaluate one top-level form at a time. Owns the input and roots
/// the current run, so callers may collect between advance() calls. The
/// heap and evaluator must outlive this loader. Guest GC requests collect
/// between committed transitions; callers must root other live words.
/// No I/O. A loader's source/cursor are host state, not a portable heap
/// image.
class loader
{
public:
    loader(
        heap & storage,
        evaluator & machine,
        std::string_view source,
        std::string_view path = "<string>");

    /// A turn counts reading a form or advancing one evaluator transition.
    /// Zero only polls; exhaustion returns runnable. Parsing/allocation in
    /// a turn is not a wall-clock bound. Read and language errors terminate
    /// loading with a condition in run().err; subsequent forms are not
    /// read.
    evaluation advance(std::size_t budget);

    /// NIL before any form; otherwise the rooted current or last run.
    word run() const noexcept
    {
        return run_.get();
    }

    std::size_t position() const noexcept
    {
        return input_.position();
    }

    /// Enclosing top-level form, not a subexpression or function backtrace.
    std::string location() const
    {
        return input_.location(input_.form_position());
    }

private:
    heap & heap_;
    evaluator & machine_;
    reader input_;
    root run_;
    evaluation state_ = evaluation::runnable;
};

} // namespace wisp
