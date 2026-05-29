#pragma once

#include "nxtrt/exceptions.hpp"

#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace nxtrt {

class deck;
class wand;

namespace detail {
struct promise_base;
}

/// Stable process-local identity for an ambient environment key type.
template<typename Key>
inline const void * env_key_id() noexcept
{
    static const int key = 0;
    return &key;
}

/// Type-erased owned value in a task runtime environment.
struct env_entry_base
{
    explicit env_entry_base(const void * key) noexcept
        : key(key)
    {}

    virtual ~env_entry_base() = default;

    /// Copy this entry when a coroutine inherits ambient state.
    [[nodiscard]] virtual std::unique_ptr<env_entry_base> clone() const = 0;

    const void * key = nullptr;
};

template<typename Key>
struct env_entry : env_entry_base
{
    using value_type = std::remove_cv_t<typename Key::value_type>;

    explicit env_entry(value_type value)
        : env_entry_base(env_key_id<Key>())
        , value(std::move(value))
    {}

    [[nodiscard]] std::unique_ptr<env_entry_base> clone() const override
    {
        return std::make_unique<env_entry<Key>>(value);
    }

    value_type value;
};

struct missing_env : runtime_error
{
    explicit missing_env(std::string_view name)
        : runtime_error{
            "missing runtime env binding: " + std::string{name}}
    {}
};

/// Promise-owned ambient environment for the currently running task.
///
/// Entries are flat rather than parent-linked: binding a key replaces exactly
/// that key for this task, and restoring the binding reinstates the previous
/// entry if one existed.
struct runtime_env
{
    deck * current_deck = nullptr;
    detail::promise_base * current_promise = nullptr;
    std::vector<std::unique_ptr<env_entry_base>> entries;

    runtime_env() = default;

    runtime_env(const runtime_env & other)
    {
        copy_entries_from(other);
    }

    runtime_env & operator=(const runtime_env & other)
    {
        copy_entries_from(other);
        return *this;
    }

    runtime_env(runtime_env &&) noexcept = default;
    runtime_env & operator=(runtime_env &&) noexcept = default;

    /// Replace this environment's entries with cloned entries from `other`.
    void copy_entries_from(const runtime_env & other)
    {
        if (this == &other)
            return;

        auto next = std::vector<std::unique_ptr<env_entry_base>>{};
        next.reserve(other.entries.size());
        for (auto const & entry : other.entries)
            next.push_back(entry->clone());

        entries = std::move(next);
        current_deck = nullptr;
        current_promise = nullptr;
    }

    template<typename Key>
    typename Key::value_type * get() const noexcept
    {
        for (auto const & entry : entries) {
            if (entry->key == env_key_id<Key>())
                return &static_cast<env_entry<Key> *>(entry.get())->value;
        }
        return nullptr;
    }

    template<typename Key>
    typename Key::value_type & require() const
    {
        if (auto * value = get<Key>())
            return *value;
        throw missing_env{Key::name};
    }

    /// Bind `Key` to `value`, returning the previous entry for RAII restore.
    template<typename Key>
    [[nodiscard]] std::unique_ptr<env_entry_base>
    replace(typename Key::value_type value)
    {
        auto next = std::make_unique<env_entry<Key>>(std::move(value));
        for (auto & entry : entries) {
            if (entry->key != env_key_id<Key>())
                continue;
            auto previous = std::move(entry);
            entry = std::move(next);
            return previous;
        }

        entries.push_back(std::move(next));
        return nullptr;
    }

    /// Restore the entry returned by `replace<Key>()`.
    template<typename Key>
    void restore(std::unique_ptr<env_entry_base> previous) noexcept
    {
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            if ((*it)->key != env_key_id<Key>())
                continue;

            if (previous)
                *it = std::move(previous);
            else
                entries.erase(it);
            return;
        }

        std::terminate();
    }
};

namespace detail {

inline thread_local runtime_env * current_env = nullptr;

class env_guard
{
public:
    /// Make `env` the current running environment for one coroutine resume.
    env_guard(
        runtime_env & env,
        deck * current_deck,
        promise_base * current_promise) noexcept
        : previous_(current_env)
        , env_(env)
        , previous_deck_(env.current_deck)
        , previous_promise_(env.current_promise)
    {
        env_.current_deck = current_deck;
        env_.current_promise = current_promise;
        current_env = &env;
    }

    env_guard(const env_guard &) = delete;
    env_guard & operator=(const env_guard &) = delete;

    ~env_guard() noexcept
    {
        env_.current_deck = previous_deck_;
        env_.current_promise = previous_promise_;
        current_env = previous_;
    }

private:
    runtime_env * previous_ = nullptr;
    runtime_env & env_;
    deck * previous_deck_ = nullptr;
    promise_base * previous_promise_ = nullptr;
};

} // namespace detail

inline runtime_env * current_env() noexcept
{
    return detail::current_env;
}

inline runtime_env & require_current_env()
{
    auto * env = current_env();
    if (env == nullptr)
        throw runtime_error{"nxtrt operation used without runtime env"};
    return *env;
}

template<typename Key>
typename Key::value_type * env_get() noexcept
{
    auto * env = current_env();
    if (env == nullptr)
        return nullptr;
    return env->get<Key>();
}

template<typename Key>
typename Key::value_type & env_require()
{
    return require_current_env().require<Key>();
}

} // namespace nxtrt
