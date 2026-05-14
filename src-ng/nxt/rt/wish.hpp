#pragma once

#include <coroutine>
#include <cstdint>

namespace nxt::rt {

class deck;

namespace detail {
struct promise_base;
}

/// A value describing something outside the deck that a task wants.
///
/// The wish itself is intentionally plain data. Later versions can add timer
/// deadlines, file descriptors, subprocess ids, socket operations, or other
/// platform facts without making the wish own coroutine lifetime machinery.
struct wish
{
    enum class kind
    {
        manual,
    };

    /// Test-shaped wish: a wand may fulfill it whenever it chooses.
    [[nodiscard]] static wish manual(std::uint64_t token = 0) noexcept
    {
        return wish{
            .what = kind::manual,
            .token = token,
        };
    }

    kind what = kind::manual;
    std::uint64_t token = 0;
};

/// A wish paired with the suspended task that made it.
///
/// This is the bridge object a wand sees. The `desired` field is the
/// platform-shaped value; `fulfill()` puts the suspended coroutine back into a
/// deck when the platform event has happened.
struct wish_request
{
    void fulfill(deck & d) const;

    wish desired;
    std::coroutine_handle<> handle;
    detail::promise_base * promise = nullptr;
};

/// Backend interface for platform/event-loop machinery.
///
/// A wand receives wishes discovered by a deck pump. Concrete wands can map
/// them onto epoll/kqueue/io_uring/Emacs/timers/tests, then eventually fulfill
/// the request back into a deck.
class wand
{
public:
    virtual ~wand() = default;

    virtual void post(deck & d, wish_request request) = 0;
};

struct wish_awaiter
{
    [[nodiscard]] bool await_ready() const noexcept
    {
        return false;
    }

    void await_suspend(std::coroutine_handle<> awaiting) const;
    void await_resume() const noexcept {}

    wish desired;
};

/// Await a platform wish through the active deck/wand plumbing.
[[nodiscard]] inline wish_awaiter wait_for(wish desired) noexcept
{
    return wish_awaiter{.desired = desired};
}

} // namespace nxt::rt
