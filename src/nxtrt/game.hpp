#pragma once

#include "nxtrt/debug.hpp"
#include "nxtrt/task.hpp"

#include <algorithm>
#include <concepts>
#include <coroutine>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stop_token>
#include <type_traits>
#include <utility>
#include <vector>

namespace nxtrt {

inline bool game_trace_enabled = false;

inline void trace_games(bool enabled = true) noexcept
{
    game_trace_enabled = enabled;
}

template<typename Event>
struct sync_spec
{
    using predicate = std::function<bool(Event const &)>;

    std::vector<Event> post;
    predicate wait = [](Event const &) { return false; };
    predicate halt = [](Event const &) { return false; };
};

template<typename Event>
class game
{
public:
    class sync_awaiter
    {
    public:
        using stop_callback_type =
            std::stop_callback<std::function<void()>>;

        sync_awaiter(game & owner, sync_spec<Event> spec)
            : owner_(&owner)
            , spec_(std::move(spec))
        {}

        [[nodiscard]] bool await_ready() const noexcept
        {
            return false;
        }

        void await_suspend(std::coroutine_handle<> awaiting)
        {
            auto * current = detail::current_env;
            auto * deck = current == nullptr ? nullptr : current->current_deck;
            auto * promise =
                current == nullptr ? nullptr : current->current_promise;
            if (deck == nullptr || promise == nullptr)
                throw runtime_error{"nxtrt game sync used without a deck"};
            owner_->park(*this, *deck, awaiting, promise);
        }

        Event await_resume()
        {
            stop_callback_.reset();
            if (exception_)
                rethrow(exception_);
            if (!selected_)
                throw runtime_error{"nxtrt game sync resumed without event"};
            return std::move(*selected_);
        }

    private:
        friend class game;

        game * owner_ = nullptr;
        sync_spec<Event> spec_;
        std::optional<Event> selected_;
        std::exception_ptr exception_;
        std::unique_ptr<stop_callback_type> stop_callback_;
    };

    [[nodiscard]] sync_awaiter sync(sync_spec<Event> spec)
    {
        return sync_awaiter{*this, std::move(spec)};
    }

private:
    struct participant
    {
        sync_awaiter * awaiter = nullptr;
        std::coroutine_handle<> handle;
        detail::promise_base * promise = nullptr;
    };

    void park(
        sync_awaiter & awaiter,
        deck & d,
        std::coroutine_handle<> handle,
        detail::promise_base * promise)
    {
        if (game_trace_enabled)
            std::cerr << "game park task " << promise->id.value << "\n";
        debug::park_task(promise->id, 0, "game sync");
        participants_.push_back(participant{
            .awaiter = &awaiter,
            .handle = handle,
            .promise = promise,
        });
        auto stop = promise->stop_token();
        if (stop.stop_possible()) {
            awaiter.stop_callback_ =
                std::make_unique<typename sync_awaiter::stop_callback_type>(
                    stop,
                    [this, &awaiter] {
                        cancel(awaiter);
                    });
        }
        schedule(d);
    }

    void cancel(sync_awaiter & awaiter)
    {
        auto it = std::ranges::find_if(
            participants_,
            [&](participant const & participant) {
                return participant.awaiter == &awaiter;
            });
        if (it == participants_.end())
            return;

        auto participant = *it;
        if (game_trace_enabled && participant.promise != nullptr)
            std::cerr << "game cancel task "
                      << participant.promise->id.value << "\n";
        participants_.erase(it);
        if (participant.promise != nullptr)
            debug::unpark_task(participant.promise->id);
        awaiter.exception_ =
            std::make_exception_ptr(operation_cancelled{});
        if (participant.promise != nullptr)
            participant.promise->enqueue_self(participant.handle);
    }

    void schedule(deck & d)
    {
        if (coordinator_active_)
            return;

        if (game_trace_enabled)
            std::cerr << "game schedule turn\n";
        coordinator_active_ = true;
        coordinator_ = coordinator_task();
        d.start(coordinator_);
    }

    task<void> coordinator_task()
    {
        co_await yield();
        coordinator_active_ = false;
        if (game_trace_enabled)
            std::cerr << "game turn\n";
        drain();
    }

    void drain()
    {
        if (auto selected = select_event())
            publish(std::move(*selected));
    }

    [[nodiscard]] std::optional<Event> select_event() const
    {
        for (auto const & participant : participants_) {
            if (participant.awaiter == nullptr)
                continue;
            for (auto const & event : participant.awaiter->spec_.post) {
                if (!blocked(event)) {
                    if (game_trace_enabled)
                        std::cerr << "game select event\n";
                    return event;
                }
                if (game_trace_enabled)
                    std::cerr << "game blocked candidate\n";
            }
        }
        if (game_trace_enabled)
            std::cerr << "game stalemate\n";
        return {};
    }

    [[nodiscard]] bool blocked(Event const & event) const
    {
        return std::ranges::any_of(
            participants_,
            [&](participant const & participant) {
                return participant.awaiter != nullptr
                    && participant.awaiter->spec_.halt(event);
            });
    }

    [[nodiscard]] static bool requested(
        sync_spec<Event> const & spec,
        Event const & event)
        requires std::equality_comparable<Event>
    {
        return std::ranges::find(spec.post, event) != spec.post.end();
    }

    void publish(Event event)
    {
        auto ready = std::vector<participant>{};
        auto remaining = std::vector<participant>{};

        for (auto & participant : participants_) {
            if (participant.awaiter == nullptr)
                continue;

            auto & spec = participant.awaiter->spec_;
            if (requested(spec, event) || spec.wait(event)) {
                participant.awaiter->selected_ = event;
                ready.push_back(participant);
            } else {
                remaining.push_back(participant);
            }
        }

        participants_ = std::move(remaining);
        for (auto const & participant : ready) {
            if (participant.promise != nullptr) {
                if (game_trace_enabled)
                    std::cerr << "game resume task "
                              << participant.promise->id.value << "\n";
                debug::unpark_task(participant.promise->id);
                participant.promise->enqueue_self(participant.handle);
            }
        }
    }

    std::vector<participant> participants_;
    task<void> coordinator_;
    bool coordinator_active_ = false;
};

template<typename Event>
struct game_key
{
    using value_type = game<Event> *;
    static constexpr auto name = "game";
};

template<typename Event>
inline game<Event> * current_game() noexcept
{
    auto value = env_get<game_key<Event>>();
    if (!value)
        return nullptr;
    return *value;
}

template<typename Event>
inline game<Event> & require_current_game()
{
    auto * current = current_game<Event>();
    if (current == nullptr)
        throw runtime_error{"nxtrt operation used without game"};
    return *current;
}

template<typename Event>
decltype(auto) game_yield(sync_spec<Event> spec)
{
    return require_current_game<Event>().sync(std::move(spec));
}

template<typename Event>
decltype(auto) game_yield(Event event)
{
    return require_current_game<Event>().sync(sync_spec<Event>{
        .post = {std::move(event)},
    });
}

namespace detail {

template<typename Yielded>
decltype(auto) promise_base::yield_value(Yielded && yielded)
{
    return game_yield(std::forward<Yielded>(yielded));
}

} // namespace detail

template<typename Event, typename Fn>
    requires stored_task_factory<std::decay_t<Fn>>
[[nodiscard]] task<stored_task_result_t<std::decay_t<Fn>>>
with_game(Fn && fn)
{
    using factory_type = std::decay_t<Fn>;
    auto game_scope = game<Event>{};
    auto bound = with_env<game_key<Event>>(
        &game_scope,
        [fn = factory_type{std::forward<Fn>(fn)}]() mutable {
            return std::invoke(fn);
        });
    if constexpr (std::is_void_v<stored_task_result_t<factory_type>>) {
        co_await bound;
    } else {
        co_return co_await bound;
    }
}

namespace detail {

template<typename Event, typename T>
[[nodiscard]] task<T> run_game_root(task<T> root)
{
    auto body = [root = std::move(root)]() mutable {
        return std::move(root);
    };

    if constexpr (std::is_void_v<T>) {
        co_await with_firm([body = std::move(body)]() mutable {
            return with_game<Event>(std::move(body));
        });
    } else {
        co_return co_await with_firm([body = std::move(body)]() mutable {
            return with_game<Event>(std::move(body));
        });
    }
}

template<typename Fn>
[[nodiscard]] task<stored_task_result_t<Fn>> run_firm_root(Fn fn)
{
    if constexpr (std::is_void_v<stored_task_result_t<Fn>>) {
        co_await with_firm(std::move(fn));
    } else {
        co_return co_await with_firm(std::move(fn));
    }
}

template<typename Event, typename Fn>
[[nodiscard]] task<stored_task_result_t<Fn>> run_game_root(Fn fn)
{
    auto game_body = [fn = std::move(fn)]() mutable {
        return std::invoke(fn);
    };

    if constexpr (std::is_void_v<stored_task_result_t<Fn>>) {
        co_await with_firm([game_body = std::move(game_body)]() mutable {
            return with_game<Event>(std::move(game_body));
        });
    } else {
        co_return co_await with_firm(
            [game_body = std::move(game_body)]() mutable {
                return with_game<Event>(std::move(game_body));
            });
    }
}

} // namespace detail

template<typename Fn>
    requires stored_task_factory<std::decay_t<Fn>>
[[nodiscard]] stored_task_result_t<std::decay_t<Fn>>
sync_wait_firm(deck & d, Fn && fn)
{
    using factory_type = std::decay_t<Fn>;
    return d.sync_wait(
        [fn = factory_type{std::forward<Fn>(fn)}]() mutable {
            return detail::run_firm_root(std::move(fn));
        });
}

template<typename Event, typename T>
[[nodiscard]] T sync_wait_game(deck & d, task<T> root)
{
    return d.sync_wait(detail::run_game_root<Event>(std::move(root)));
}

template<typename Event, typename Fn>
    requires stored_task_factory<std::decay_t<Fn>>
[[nodiscard]] stored_task_result_t<std::decay_t<Fn>>
sync_wait_game(deck & d, Fn && fn)
{
    using factory_type = std::decay_t<Fn>;
    return d.sync_wait(
        [fn = factory_type{std::forward<Fn>(fn)}]() mutable {
            return detail::run_game_root<Event>(std::move(fn));
        });
}

} // namespace nxtrt
