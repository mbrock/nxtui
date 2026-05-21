#pragma once

#include <exception>
#include <iosfwd>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

#ifdef NXT_HAVE_CPPTRACE
#  include <cpptrace/exceptions.hpp>
#  include <cpptrace/from_current.hpp>
#endif

namespace nxt::io {

#ifdef NXT_HAVE_CPPTRACE
using exception = cpptrace::exception;
using runtime_error = cpptrace::runtime_error;
using logic_error = cpptrace::logic_error;
using invalid_argument = cpptrace::invalid_argument;
using out_of_range = cpptrace::out_of_range;
#else
using exception = std::exception;
using runtime_error = std::runtime_error;
using logic_error = std::logic_error;
using invalid_argument = std::invalid_argument;
using out_of_range = std::out_of_range;
#endif

namespace detail {

template<typename>
struct callable_argument;

template<typename R, typename Arg>
struct callable_argument<R (*)(Arg)>
{
    using type = Arg;
};

template<typename R>
struct callable_argument<R (*)()>
{
    using type = void;
};

template<typename R, typename C, typename Arg>
struct callable_argument<R (C::*)(Arg) const>
{
    using type = Arg;
};

template<typename R, typename C>
struct callable_argument<R (C::*)() const>
{
    using type = void;
};

template<typename R, typename C, typename Arg>
struct callable_argument<R (C::*)(Arg)>
{
    using type = Arg;
};

template<typename R, typename C>
struct callable_argument<R (C::*)()>
{
    using type = void;
};

template<typename F>
using callable_argument_t = typename callable_argument<decltype(&F::operator())>::type;

template<typename Catch>
void fallback_handle_exception(
    const std::exception_ptr & failure, Catch && catcher)
{
    using Arg = callable_argument_t<std::remove_reference_t<Catch>>;
    if constexpr (std::is_void_v<Arg>) {
        std::forward<Catch>(catcher)();
    } else {
        try {
            std::rethrow_exception(failure);
        } catch (Arg e) {
            std::forward<Catch>(catcher)(e);
        }
    }
}

template<typename Catch, typename NextCatch, typename... Catches>
void fallback_handle_exception(
    const std::exception_ptr & failure,
    Catch && catcher,
    NextCatch && next_catcher,
    Catches &&... catchers)
{
    using Arg = callable_argument_t<std::remove_reference_t<Catch>>;
    if constexpr (std::is_void_v<Arg>) {
        std::forward<Catch>(catcher)();
    } else {
        try {
            std::rethrow_exception(failure);
        } catch (Arg e) {
            std::forward<Catch>(catcher)(e);
        } catch (...) {
            fallback_handle_exception(
                failure,
                std::forward<NextCatch>(next_catcher),
                std::forward<Catches>(catchers)...);
        }
    }
}

} // namespace detail

#ifdef NXT_HAVE_CPPTRACE
/// Run a try/catch block with cpptrace exception interception.
using cpptrace::try_catch;
#else
/// Run a try/catch block. Catch handlers are lambdas with either one typed
/// exception argument or no arguments for a catch-all handler.
template<typename F, typename... Catches>
void try_catch(F && f, Catches &&... catchers)
{
    try {
        std::forward<F>(f)();
    } catch (...) {
        detail::fallback_handle_exception(
            std::current_exception(), std::forward<Catches>(catchers)...);
    }
}
#endif

/// Print the best stack trace available for the active exception catch site.
/// With cpptrace this uses its exception-interception machinery; otherwise it
/// falls back to the platform's standard stacktrace support when available.
bool print_current_exception_trace(
    std::ostream & out,
    std::string_view indent = "",
    std::string_view label = "Catch site");

#ifdef NXT_HAVE_CPPTRACE
/// Print one cpptrace stacktrace using nxt's crash-report formatting.
void print_stacktrace(
    std::ostream & out,
    const cpptrace::stacktrace & trace,
    std::string_view indent = "");
#endif

/// Rethrow an exception while preserving cpptrace metadata when available.
[[noreturn]] void rethrow(std::exception_ptr failure);

/// Rethrow the active exception while preserving cpptrace metadata when
/// available.
[[noreturn]] void rethrow_current_exception();

} // namespace nxt::io
