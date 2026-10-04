// SPDX-License-Identifier: AGPL-3.0-or-later
// C++ port of mbrock/wisp's core/step.zig and core/jets-{ctl,fun}.zig.
#pragma once

#include "wisp/heap.hpp"

#include <array>
#include <string_view>
#include <utility>

namespace wisp {

/// Symbols in WISP that native code names, like Zig's `Kwd`. A machine
/// interns all of them when it starts or is restored and keeps them as
/// roots, so no transition ever searches a package for one. Native code
/// names them as `vm.known("ERROR")`; a name missing here does not compile.
inline constexpr auto known_names = std::to_array<std::string_view>({
    // Reader and special forms.
    "QUOTE", "BACKQUOTE", "UNQUOTE", "UNQUOTE-SPLICING", "FUNCTION",
    "&REST", "&BODY", "&OPTIONAL",
    "BINDING", "COND", "DEFUN", "IF", "FN", "LET", "DO", "EVAL",
    "PROMPT", "RESUME",
    // Evaluator states and conditions.
    "EXP", "VAL", "ERROR",
    // Types, as TYPE-OF names them.
    "BOOLEAN", "CHARACTER", "CONS", "CONTINUATION", "EVALUATOR",
    "EXTERNAL", "INTEGER", "MACRO", "NULL", "PACKAGE", "PIN", "RECORD",
    "STRING", "SYMBOL", "VECTOR",
    // Condition names.
    "ACTIVE-EVALUATOR", "BAD-FIXNUM-DIVISION", "BAD-MODULO",
    "BOUNDS-ERROR", "BUG", "BUILTIN-FAILURE", "CONTINUATION-CALL-ERROR",
    "CYCLIC-LIST", "END-OF-FILE", "EXHAUSTED", "FIXNUM-OVERFLOW",
    "INVALID-ARGUMENT-COUNT", "INVALID-BINDING", "INVALID-CALLEE",
    "INVALID-CONTINUATION", "INVALID-ENVIRONMENT", "INVALID-EXPRESSION",
    "INVALID-FUNCTION", "INVALID-PACKAGE-USES", "INVALID-PARAMETERS",
    "INVALID-STRING-INPUT-STREAM", "INVALID-VALUE", "KEY-SPACE-EXHAUSTED",
    "LOW-LEVEL-ERROR", "PACKAGE-ERROR", "PACKAGE-EXISTS", "PROGRAM-ERROR",
    "PROMPT-TAG-MISSING", "READ-ERROR", "TYPE-MISMATCH",
    "UNBOUND-VARIABLE", "UNDEFINED-FUNCTION", "UNDEFINED-PACKAGE",
    "UNHANDLED-ERROR",
    // Data markers.
    "STRING-INPUT-STREAM",
});

/// A position in `known_names`, found at compile time.
class known_name
{
public:
    consteval known_name(const char * name)
        : index_(find(name))
    {
    }

    constexpr std::size_t index() const noexcept
    {
        return index_;
    }

    static constexpr std::size_t find(std::string_view name)
    {
        for (std::size_t i = 0; i < known_names.size(); ++i)
            if (known_names[i] == name)
                return i;
        throw "not a known Wisp symbol; add it to wisp::known_names";
    }

private:
    std::size_t index_;
};

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

    /// A symbol from `known_names`, without searching any package.
    word known(known_name name) const noexcept
    {
        return known_[name.index()].get();
    }

    /// Inputs must be live words in this heap; an environment is NIL or a
    /// list of even-length key/value vectors (only symbol keys can match).
    /// Mutable syntax, environments, and continuation payloads are checked
    /// on use and signal conditions. As with heap's low-level API,
    /// fabricated pointers and malformed private machine links are not
    /// supported.
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
    // Interns every known name in WISP. Runs when a machine starts and
    // after a tape restores one, like Zig's tape loader.
    void install_known();

    // Roots keep these current across collection.
    std::array<root, known_names.size()> known_;
    bool collect_ = false;

    // Host-independent fresh keys: unique within this evaluator, not
    // Zig's date/random names or a portable identity across images.
    std::uint64_t next_key_ = 0;
};

} // namespace wisp
