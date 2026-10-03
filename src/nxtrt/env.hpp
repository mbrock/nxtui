#pragma once

#include "nxtrt/exceptions.hpp"

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

/// Process-local identity of an env key type: a distinct address per
/// `Key`. Used internally to look entries up.
template<typename Key>
inline const void * env_key_id() noexcept
{
    static const int key = 0;
    return &key;
}

/// A nullable reference, returned by optional env lookups such as
/// `env_get<Key>()`.
///
/// Test it with `if (ref)`. Dereferencing an empty one (`*`, `->`, `get()`)
/// throws `runtime_error` rather than invoking undefined behavior.
template<typename T>
class optional_ref
{
public:
    optional_ref() noexcept = default;

    explicit optional_ref(T & value) noexcept
        : value_(&value)
    {}

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return value_ != nullptr;
    }

    [[nodiscard]] T & operator*() const
    {
        return *require_value();
    }

    [[nodiscard]] T * operator->() const
    {
        return require_value();
    }

    [[nodiscard]] T & get() const
    {
        return *require_value();
    }

private:
    [[nodiscard]] T * require_value() const
    {
        if (value_ == nullptr)
            throw runtime_error{"empty nxtrt optional_ref"};
        return value_;
    }

    T * value_ = nullptr;
};

/// Type-erased owned value in a task runtime environment. Internal to
/// `runtime_env`.
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

/// The entry holding a `Key::value_type`. Internal to `runtime_env`.
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

/// Thrown by `env_require<Key>()` when `Key` is not bound. The message
/// names `Key::name`.
struct missing_env : runtime_error
{
    explicit missing_env(std::string_view name)
        : runtime_error{
            "missing runtime env binding: " + std::string{name}}
    {}
};

/// The ambient key/value environment of a task, plus the deck and promise
/// that are running it.
///
/// Each task's promise owns one. A new task copies the env of the code
/// that creates it; when it is awaited, or started by a group, it takes
/// the awaiting task's (or group's) env instead. A root task created by
/// `deck::sync_wait` or `root_task` starts from an empty env. Bind values
/// for a subtree of tasks with `with_env<Key>`; read them with
/// `env_get<Key>()` or `env_require<Key>()`.
///
/// An env key is an empty tag type naming a value type and a display name:
///
/// @code
/// struct request_id_key
/// {
///     using value_type = int;
///     static constexpr auto name = "request-id";
/// };
/// @endcode
///
/// Entries live in an immutable, shared snapshot. Copying an env shares
/// the snapshot; binding a key builds a new snapshot (copying every entry,
/// so values must be copyable) and leaves other tasks' views unchanged.
/// Lookup is a linear scan, intended for a handful of keys. Copies never
/// carry `current_deck` or `current_promise`, which are set only while a
/// deck is resuming the task. Deck-thread only.
struct runtime_env
{
    using entry_list = std::vector<std::unique_ptr<env_entry_base>>;
    using entry_snapshot = std::shared_ptr<const entry_list>;

    deck * current_deck = nullptr;
    detail::promise_base * current_promise = nullptr;
    entry_snapshot entries = empty_entries();

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

    /// Replace this environment's entries with `other`'s shared snapshot.
    ///
    /// Snapshots are immutable, so child coroutine frames can inherit ambient
    /// values by sharing a reference-counted snapshot. Later bindings install a
    /// new snapshot instead of mutating storage that another task can observe.
    void copy_entries_from(const runtime_env & other)
    {
        if (this == &other)
            return;

        entries = other.entries == nullptr
            ? empty_entries()
            : other.entries;
        current_deck = nullptr;
        current_promise = nullptr;
    }

    /// The value bound to `Key`, or an empty reference.
    template<typename Key>
    optional_ref<const std::remove_cv_t<typename Key::value_type>>
    get() const noexcept
    {
        if (entries == nullptr)
            return {};

        for (auto const & entry : *entries) {
            if (entry->key == env_key_id<Key>()) {
                auto const * typed =
                    static_cast<const env_entry<Key> *>(entry.get());
                return optional_ref<
                    const std::remove_cv_t<typename Key::value_type>>{
                    typed->value};
            }
        }
        return {};
    }

    /// The value bound to `Key`. Throws `missing_env` if unbound.
    template<typename Key>
    const std::remove_cv_t<typename Key::value_type> & require() const
    {
        if (auto value = get<Key>())
            return *value;
        throw missing_env{Key::name};
    }

    /// Bind `Key` to `value` in this env (replacing any earlier binding) and
    /// return the previous snapshot for `restore()`. Prefer `with_env`,
    /// which scopes the binding to a child task.
    template<typename Key>
    [[nodiscard]] entry_snapshot
    replace(typename Key::value_type value)
    {
        auto previous = entries == nullptr
            ? empty_entries()
            : entries;
        auto next = clone_entries(previous);
        auto replacement =
            std::make_unique<env_entry<Key>>(std::move(value));
        for (auto & entry : *next) {
            if (entry->key != env_key_id<Key>())
                continue;
            entry = std::move(replacement);
            entries = std::move(next);
            return previous;
        }

        next->push_back(std::move(replacement));
        entries = std::move(next);
        return previous;
    }

    /// Restore the snapshot returned by `replace<Key>()`.
    void restore(entry_snapshot previous) noexcept
    {
        entries = previous == nullptr
            ? empty_entries()
            : std::move(previous);
    }

private:
    [[nodiscard]] static entry_snapshot empty_entries()
    {
        static const auto empty = std::make_shared<const entry_list>();
        return empty;
    }

    [[nodiscard]] static std::shared_ptr<entry_list>
    clone_entries(const entry_snapshot & source)
    {
        auto next = std::make_shared<entry_list>();
        if (source == nullptr)
            return next;

        next->reserve(source->size());
        for (auto const & entry : *source)
            next->push_back(entry->clone());
        return next;
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

/// The env of the task currently running on this thread, or null outside
/// deck execution (and outside `sync_wait`'s factory call).
inline runtime_env * current_env() noexcept
{
    return detail::current_env;
}

/// `current_env()`, or throw `runtime_error` when there is none.
inline runtime_env & require_current_env()
{
    auto * env = current_env();
    if (env == nullptr)
        throw runtime_error{"nxtrt operation used without runtime env"};
    return *env;
}

/// The value bound to `Key` in the running task's env, or an empty
/// reference if unbound or outside a task. The reference points into the
/// current snapshot; copy the value if it must outlive the binding.
template<typename Key>
optional_ref<const std::remove_cv_t<typename Key::value_type>>
env_get() noexcept
{
    auto * env = current_env();
    if (env == nullptr)
        return {};
    return env->get<Key>();
}

/// The value bound to `Key` in the running task's env. Throws
/// `missing_env` if unbound, or `runtime_error` outside a task.
template<typename Key>
const std::remove_cv_t<typename Key::value_type> & env_require()
{
    return require_current_env().require<Key>();
}

} // namespace nxtrt
