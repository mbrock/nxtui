#pragma once

// Public async facade.

#include "nxtio/async-core.hpp"
#include "nxtio/yardtask.hpp"

#include "nxt/signal.hpp"
#include "nxtio/event-queue.hpp"

#include <optional>
#include <stop_token>
#include <type_traits>
#include <utility>
#include <variant>

namespace nxt {

/// Buffered async stream of values.
template<typename T>
using channel = nxt::detail::event_queue<T>;

template<typename T>
struct source_value;

template<typename T>
struct source_value<signal<T>>
{
    using type = T;
};

template<typename T>
struct source_value<channel<T>>
{
    using type = T;
};

template<typename T, typename Pull>
class pull_source
{
public:
    explicit pull_source(Pull pull)
        : pull_(std::move(pull))
    {}

    [[nodiscard]] task<std::optional<T>>
    next(std::stop_token stop = {})
    {
        co_return co_await pull_(std::move(stop));
    }

private:
    Pull pull_;
};

template<typename T, typename Pull>
struct source_value<pull_source<T, Pull>>
{
    using type = T;
};

template<typename T>
using source_value_t =
    typename source_value<std::remove_cvref_t<T>>::type;

template<typename S>
concept scoped = requires(S & s) {
    s.stop_token();
    s.scheduler();
};

template<typename S>
concept has_context = requires(S & s) {
    s.context();
};

template<scoped Scope>
[[nodiscard]] auto stop_token(Scope & s)
{
    return s.stop_token();
}

template<scoped Scope>
[[nodiscard]] auto & scheduler_of(Scope & s)
{
    return s.scheduler();
}

template<scoped Scope>
[[nodiscard]] auto subscope(Scope & s)
{
    return s.subscope();
}

template<scoped Scope, typename Context>
[[nodiscard]] auto subscope(Scope & s, Context context)
{
    return s.subscope(std::move(context));
}

template<scoped Scope>
void cancel(Scope & s)
{
    s.cancel();
}

template<scoped Scope>
void spawn(Scope & s, task<> t)
{
    s.spawn(std::move(t));
}

template<scoped Scope, typename... Awaitables>
[[nodiscard]] auto all(Scope & s, Awaitables &&... awaitables)
{
    return s.all(std::forward<Awaitables>(awaitables)...);
}

template<scoped Scope, typename... Awaitables>
[[nodiscard]] auto any(Scope & s, Awaitables &&... awaitables)
{
    return s.any(std::forward<Awaitables>(awaitables)...);
}

template<typename T>
[[nodiscard]] auto next(const signal<T> & sig)
{
    return sig.next();
}

template<typename T>
[[nodiscard]] auto next(const signal<T> & sig, std::stop_token stop)
{
    return sig.next(std::move(stop));
}

template<typename T>
[[nodiscard]] task<std::optional<T>>
next(channel<T> & ch, std::stop_token = {})
{
    co_return co_await ch.next();
}

template<typename T, typename Pull>
[[nodiscard]] task<std::optional<T>>
next(pull_source<T, Pull> & src, std::stop_token stop = {})
{
    co_return co_await src.next(std::move(stop));
}

template<typename T>
[[nodiscard]] task<void> publish(const publisher<T> & pub, T value)
{
    co_await pub.push(std::move(value));
}

template<typename T>
[[nodiscard]] task<void> publish(const bound_publisher<T> & pub)
{
    co_await pub.push();
}

template<typename T>
[[nodiscard]] task<bool> publish(channel<T> & ch, T value)
{
    co_return co_await ch.publish(std::move(value));
}

template<typename T>
[[nodiscard]] auto publisher_for(const signal<T> & sig)
{
    return sig.publisher();
}

template<typename T>
[[nodiscard]] auto publisher_for(const signal<T> & sig, T value)
{
    return sig.publisher(std::move(value));
}

template<typename T>
[[nodiscard]] task<void> close(channel<T> & ch)
{
    co_await ch.close();
}

template<typename T>
void close(signal<T> & sig)
{
    sig.close();
}

template<typename T>
[[nodiscard]] task<void> cancel(channel<T> & ch)
{
    co_await ch.cancel();
}

template<typename T, typename Pull>
[[nodiscard]] auto make_source(Pull && pull)
{
    return pull_source<T, std::decay_t<Pull>>{
        std::forward<Pull>(pull)};
}

template<typename Source>
concept source = requires(Source & src, std::stop_token stop) {
    { next(src, stop) };
};

template<typename Sink, typename T>
concept sink = requires(Sink & out, T value) {
    { publish(out, std::move(value)) };
};

template<scoped Scope, typename... Sources>
[[nodiscard]] task<std::variant<std::optional<source_value_t<Sources>>...>>
select(Scope & scope, Sources &... sources)
{
    auto race = subscope(scope);
    co_return co_await any(
        race,
        next(sources, race.stop_token())...);
}

} // namespace nxt
