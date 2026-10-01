// SPDX-License-Identifier: AGPL-3.0-or-later
// C++ port of mbrock/wisp's core/step.zig and core/jets-{ctl,fun}.zig.
#pragma once

#include "wisp/heap.hpp"

namespace wisp {

enum class evaluation { runnable, done, failed };

/// The language machine, independent of any host event loop. All suspended
/// execution is in run/ktx rows, not in this object's native stack. The
/// heap must outlive the evaluator. One evaluator installs fresh WISP,
/// KEYWORD, and KEY packages, with WISP initially current.
class evaluator
{
public:
    explicit evaluator(heap & storage);

    /// Exact, case-sensitive names in WISP; independent of current package.
    /// Case folding and current-package selection belong to the reader.
    /// Interned symbols are retained by their rooted package. WISP's NIL
    /// and T are immediates. Keywords self-evaluate.
    word intern(std::string_view name);
    /// Search own symbols, then direct used packages in list order (not
    /// transitively), then create locally. package must belong to this
    /// evaluator. Malformed uses lists throw std::invalid_argument.
    word intern(std::string_view name, word package);
    word keyword(std::string_view name);
    /// Exact name lookup; NIL means absent, not a usable package.
    word find_package(std::string_view name) const noexcept;
    /// Create an empty package. Duplicate names throw
    /// std::invalid_argument.
    word define_package(std::string_view name);

    word current_package() const noexcept
    {
        return current_.get();
    }

    /// Inputs must be live words in this heap; an environment is NIL or a
    /// list of even-length name/value vectors. As with heap's low-level
    /// API, fabricated pointers and malformed machine rows are not checked.
    word start(word expression, word environment = nil);
    evaluation status(word run) const noexcept;
    evaluation step(word run);

    /// At most budget calls to step; zero only polls. Exhaustion
    /// leaves a runnable run, not an error. A transition can scan a list or
    /// dispatch nested STEP!, so this is not a wall-clock bound. Neither
    /// step nor advance collects or invokes host callbacks. Collect between
    /// calls, rooting the run and every other host-held value. Return
    /// values are unrooted words. Language errors go to ERROR prompts;
    /// failed delivery populates run.err. Host allocation exceptions escape
    /// and do not promise rollback or safe retry of the interrupted step.
    evaluation advance(word run, std::size_t budget);

    /// GC only requests a safepoint. advance stops early while requested;
    /// step still performs one (possibly nested STEP!) transition. Neither
    /// collects. The host must root all live words before servicing it.
    bool collection_requested() const noexcept
    {
        return collect_;
    }

    /// Explicit collection, clearing the request only after success.
    void collect();

private:
    friend struct eval_step;
    friend struct tape_codec;
    friend struct image;

    // Restore registers roots without installing packages or overwriting
    // saved definitions. Only a validated tape may fill these slots.
    evaluator(heap & storage, std::nullptr_t);

    struct jet_info
    {
        std::string_view name;
        bool control;
    };

    static std::vector<jet_info> jet_manifest();

    heap & heap_;
    root base_;
    root keywords_;
    root keys_;
    root packages_;
    root current_;
    root nil_name_;
    root true_name_;
    // Like Zig's cached keyword identities: never search the package list
    // on the transition hot path. Roots keep these current across GC.
    root do_, if_, eval_, let_, prompt_, binding_, optional_, rest_, body_;
    bool collect_ = false;
    // Host-independent fresh keys: unique within this evaluator, not
    // Zig's date/random names or a portable identity across images.
    std::uint64_t next_key_ = 0;
};

} // namespace wisp
