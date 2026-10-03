#pragma once

#include "nxtrt/wand.hpp"

#include <concepts>
#include <variant>

/// Shared execution lifecycle for wand backends.
///
/// Every wish a wand prepares becomes one execution record whose state is
/// `lifecycle<P, S>::state`: prepared, then parked (holding the waiting
/// task's `need` and a backend parked phase), then settled (result stored,
/// task requeued, backend settled phase), then retired (erased at a sync
/// point). Each step only moves forward.
///
/// The executable model of these rules is `nxtrt/runtime.rkt`, checked with
/// `make spec`. Its vocabulary maps onto this file: `prepared-state`,
/// `parked-state`, `settled-state`, and `retired-state` are the four
/// alternatives here; `queued-phase` is `queued`; `submitted-phase` and
/// `cancelling-phase` are backend parked phases (`submitted`;
/// `cancel_queued`, `cancel_submitted`, `cancel_drained` in io_uring and
/// `delete_queued` in kqueue); `ready-to-retire-phase` is `ready_to_retire`;
/// `draining-phase` is a settled phase that waits on the kernel
/// (`waiting_cancel_cqe` in io_uring, `delete_pending`/`delete_applied` in
/// kqueue). The model checks, among others, that an execution retires only
/// from settled + ready-to-retire, that draining follows only a cancel, and
/// that a settled execution never parks again. Update the model when these
/// rules change.
namespace nxtrt::detail::wand_exec {

/// Allocated by `prep`; not yet parked by the urge.
struct prepared
{};

/// Parked and waiting for backend-specific submission.
struct queued
{};

/// No backend events are needed before the hub slot can be reused.
struct ready_to_retire
{};

/// Tombstone used during hub compaction.
struct retired
{};

/// True when a backend phase set can represent one common phase.
template<typename Phase, typename Alternative>
concept phase_accepts = std::constructible_from<Phase, Alternative>;

/// Common prepared/parked/settled/retired spine for wand executions.
///
/// Backends provide their own parked and settled phase variants, but they must
/// accept `queued` and `ready_to_retire` so the shared lifecycle has a common
/// start and compaction point. A record may be freed only once
/// `is_retirable` holds, which is when the kernel can no longer deliver an
/// event naming it.
template<typename ParkedPhase, typename SettledPhase>
    requires phase_accepts<ParkedPhase, queued>
          && phase_accepts<SettledPhase, ready_to_retire>
struct lifecycle
{
    /// Suspended task plus the backend-specific phase of its wish.
    struct parked
    {
        /// Coroutine to requeue when the wish settles.
        need continuation;
        /// Current phase while the continuation is still parked.
        ParkedPhase phase = ParkedPhase{queued{}};
    };

    /// Fulfilled execution waiting for a compaction sync point.
    struct settled
    {
        /// Backend-specific phase after the task has been fulfilled.
        SettledPhase phase = SettledPhase{ready_to_retire{}};
    };

    /// Full lifecycle of one wish execution.
    using state = std::variant<prepared, parked, settled, retired>;

    /// True when a settled execution can be erased at a sync point.
    [[nodiscard]] static bool is_retirable(state const & s) noexcept
    {
        auto const * settled_state = std::get_if<settled>(&s);
        return settled_state != nullptr
            && std::holds_alternative<ready_to_retire>(
                settled_state->phase);
    }
};

} // namespace nxtrt::detail::wand_exec
