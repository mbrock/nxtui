#pragma once

#include "nxtrt/task/root.hpp"

#include <concepts>
#include <functional>
#include <type_traits>

namespace nxtrt {

namespace detail {

// Only exact by-value runtime results qualify; references and merely
// convertible awaitables must not inherit an idea's ownership contract.
template<typename>
struct idea_result
{};

template<typename T>
struct idea_result<task<T>>
{
    using type = T;
};

template<typename T>
struct idea_result<hope<T>>
{
    using type = T;
};

} // namespace detail

/// A movable recipe for asynchronous work: a callable that, invoked as a
/// stored mutable lvalue with no arguments, returns a `task<T>` or
/// `hope<T>` by value.
///
/// Ideas are what pools and `drain` consume from a `feed`: the work is not
/// created until a consumer has capacity for it. This is a constraint, not
/// a type-erased wrapper. Consumers invoke each admitted idea once and keep
/// the idea object alive until the work it returned has settled, including
/// by failure or cancellation, so the returned task may refer to the
/// idea's members. The concept cannot check that contract.
///
/// Only exact by-value `task<T>` and `hope<T>` results qualify; references
/// and other awaitables do not.
///
/// @code
/// struct fetch_idea
/// {
///     std::string url;
///     nxtrt::task<int> operator()() & { return fetch(url); }
/// };
/// static_assert(nxtrt::idea_of<fetch_idea, int>);
/// @endcode
template<typename Fn>
concept idea =
    std::move_constructible<Fn>
    && std::invocable<Fn &>
    && requires {
        typename detail::idea_result<std::invoke_result_t<Fn &>>::type;
    };

/// The eventual value type T of an idea returning task<T> or hope<T>.
template<idea Fn>
using idea_result_t =
    typename detail::idea_result<std::invoke_result_t<Fn &>>::type;

/// An `idea` whose work produces exactly `T`.
template<typename Fn, typename T>
concept idea_of = idea<Fn> && std::same_as<idea_result_t<Fn>, T>;

} // namespace nxtrt
