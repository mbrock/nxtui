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

/// A movable recipe invoked as a mutable stored lvalue to produce a task
/// or hope by value. This is a constraint, not an owning erased wrapper.
///
/// Consumers invoke each admitted recipe once and keep its storage alive
/// through settlement of the returned work, including cancellation/failure.
/// The concept alone cannot enforce that lifetime or invocation count.
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

template<typename Fn, typename T>
concept idea_of = idea<Fn> && std::same_as<idea_result_t<Fn>, T>;

} // namespace nxtrt
