// SPDX-License-Identifier: AGPL-3.0-or-later
// C++ port of mbrock/wisp's core/step.zig and core/jets-{ctl,fun}.zig.
#pragma once

#include "wisp/heap.hpp"

namespace wisp {

enum class evaluation { runnable, done, failed };

/// The language machine, independent of any host event loop. All suspended
/// execution is in run/ktx rows, not in this object's native stack. The
/// heap must outlive the evaluator. One evaluator installs a fresh WISP and
/// KEYWORD package; package inheritance and the Lisp bootstrap come later.
class evaluator
{
public:
    explicit evaluator(heap & storage);

    /// Exact, case-sensitive names; case folding belongs to the reader.
    /// Interned symbols are retained by their rooted package. WISP's NIL
    /// and T are immediates. Keywords self-evaluate.
    word intern(std::string_view name);
    word keyword(std::string_view name);

    /// Inputs must be live words in this heap; an environment is NIL or a
    /// list of even-length name/value vectors. As with heap's low-level
    /// API, fabricated pointers and malformed machine rows are not checked.
    word start(word expression, word environment = nil);
    evaluation status(word run) const noexcept;
    evaluation step(word run);

    /// At most budget evaluator transitions; zero only polls. Exhaustion
    /// leaves a runnable run, not an error. A transition can scan a list or
    /// allocate, so this is not a wall-clock bound. No method collects or
    /// invokes host callbacks. Collect between calls, rooting the run and
    /// every other host-held value. Return values are unrooted words.
    /// Language errors go to ERROR prompts; failed delivery populates
    /// run.err. Host allocation exceptions escape and do not promise
    /// rollback or safe retry of the interrupted step.
    evaluation advance(word run, std::size_t budget);

private:
    friend struct eval_step;
    word intern(std::string_view name, word package);

    heap & heap_;
    root base_;
    root keywords_;
};

} // namespace wisp
