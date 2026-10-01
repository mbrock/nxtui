// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "wisp/eval.hpp"
#include "nxtrt/task.hpp"

namespace wisp {

/// Drive one rooted run on the current NXT deck. A quantum bounds evaluator
/// calls, not elapsed time: a transition can parse, scan, or dispatch
/// nested STEP! calls. Guest GC requests collect only after committed
/// transitions.
///
/// The evaluator, heap, and root must outlive this task. Accepting a root
/// reference (not a word) also permits collection before first resumption.
/// Other live host values must be rooted across awaits. Do not concurrently
/// drive or replace this run. Cancellation leaves it suspended and
/// resumable; it neither poisons guest state nor turns host cancellation
/// into guest ERROR. Host I/O/effect policy belongs to the caller, not this
/// scheduler adapter.
inline nxtrt::task<evaluation>
drive(evaluator & machine, root & run, std::size_t quantum = 256)
{
    if (quantum == 0)
        throw std::invalid_argument(
            "Wisp drive requires a positive quantum");
    while (true) {
        nxtrt::throw_if_stop_requested();
        const auto state = machine.advance(run.get(), quantum);
        if (machine.collection_requested())
            machine.collect();
        if (state != evaluation::runnable)
            co_return state;
        co_await nxtrt::yield();
    }
}

} // namespace wisp
