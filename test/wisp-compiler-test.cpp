// SPDX-License-Identifier: AGPL-3.0-or-later
// Compiler tests that need native APIs. Most compiler tests and the
// semantic corpus are Wisp files in test/wisp/, run by wisp-script-test.cpp.
#include <wisp/printer.hpp>

#include "test.hpp"
#include "wisp-source.hpp"

namespace wisp::test {
namespace {

using namespace boost::ut;

static suite compiler_tests{
    "WISP COMPILER", [] {
        "analyzed graphs keep their binding identity in a tape"_test = [] {
            source_machine m{compiler_image()};
            m.load(R"((defvar x (make-ir-binding 'x nil))
                      (defvar node (analyze '(if x (foo x) 17) (list x))))");
            source_machine copy{tape::decode(tape::encode(m.vm))};
            copy.check(
                R"((list (ir-show node)
                         (eq? x (ir-reference-binding (ir-branch-test node)))
                         (eq? (record-type node) <ir-branch>)))",
                "((:IF (:REFERENCE X 1) (:CALL FOO (:REFERENCE X 1)) "
                "(:CONSTANT 17)) T T)");
        };
    }};

} // namespace
} // namespace wisp::test
