// SPDX-License-Identifier: AGPL-3.0-or-later
// C++ port of mbrock/wisp's core/sexp-dump.zig.
#pragma once

#include "wisp/heap.hpp"

#include <string>

namespace wisp {

/// Compact, diagnostic printing, not image serialization or pretty layout.
/// Input must be a live value with well-formed rows in this heap, as for
/// heap::read. Neither guest allocation nor collection nor mutation occurs;
/// only the returned string and host traversal storage are allocated.
/// Traversal is iterative; recursive edges print #<CYCLE>. Acyclic sharing
/// is expanded each time, so output size can exceed the size of the heap.
/// Host allocation/length exceptions propagate without changing the heap.
///
/// Matches Wisp's compact notation, including #<...> vectors, <package>,
/// #'NAME functions/macros, <%ktx ...>, and <run ...>. These diagnostics
/// are not generally readable, nor do they preserve identity or sharing.
/// Builtins, pins, and characters print #<jet N>, #<pin N>, and #<chr N>;
/// IDs are local, not Zig builtin indexes or a portable tape format.
///
/// Unlike the reference, fixnums print signed and strings escape quotes,
/// backslashes, and newlines. Other string bytes (including NUL) and symbol
/// names are preserved verbatim. Symbol spelling is therefore not always
/// reader-safe. Without evaluator context, packages named WISP and KEY
/// print bare symbols, KEYWORD uses ':', other packages use 'NAME:', and
/// uninterned symbols use '#:'. Supplying a live current package replaces
/// the WISP default with that identity; KEY and KEYWORD keep their syntax.
std::string print(const heap &, word, word current_package = nil);

} // namespace wisp
