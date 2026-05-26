#pragma once

#include "nxtrt/exceptions.hpp"

#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace nxtrt {

class deck;
class wand;

namespace detail {
struct promise_base;
}

template<typename Key>
inline const void * env_key_id() noexcept
{
    static const int key = 0;
    return &key;
}

struct env_binding_base
{
    env_binding_base * parent = nullptr;
    const void * key = nullptr;
};

template<typename Key>
struct env_binding : env_binding_base
{
    using value_type = std::remove_cv_t<typename Key::value_type>;

    env_binding(env_binding_base * parent, value_type value)
        : env_binding_base{
            .parent = parent,
            .key = env_key_id<Key>(),
        }
        , value(std::move(value))
    {}

    value_type value;
};

struct missing_env : runtime_error
{
    explicit missing_env(std::string_view name)
        : runtime_error{
            "missing runtime env binding: " + std::string{name}}
    {}
};

struct runtime_env
{
    deck * current_deck = nullptr;
    detail::promise_base * current_promise = nullptr;
    env_binding_base * bindings = nullptr;

    template<typename Key>
    typename Key::value_type * get() const noexcept
    {
        for (auto * binding = bindings; binding != nullptr;
             binding = binding->parent) {
            if (binding->key == env_key_id<Key>())
                return &static_cast<env_binding<Key> *>(binding)->value;
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
};

namespace detail {

inline thread_local runtime_env * current_env = nullptr;

class env_guard
{
public:
    explicit env_guard(runtime_env & env) noexcept
        : previous_(current_env)
    {
        current_env = &env;
    }

    env_guard(const env_guard &) = delete;
    env_guard & operator=(const env_guard &) = delete;

    ~env_guard()
    {
        current_env = previous_;
    }

private:
    runtime_env * previous_ = nullptr;
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
