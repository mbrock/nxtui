#pragma once

#include <algorithm>
#include <cstddef>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef NXT_HAVE_CPPTRACE
#  include <cpptrace/exceptions.hpp>
#  include <cpptrace/from_current.hpp>
#endif

namespace nxtrt {

/// @name Base exception types
/// The runtime's exception bases. With cpptrace (`NXT_HAVE_CPPTRACE`) these
/// are cpptrace's types, which record a stack trace where they are thrown;
/// otherwise they are the `std` types of the same names. Catch them by
/// these aliases, not by the cpptrace names.
/// @{
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
/// @}

[[noreturn]] inline void rethrow(std::exception_ptr failure);

/// A wish's system call was interrupted (`EINTR`). Stream code catches it
/// and retries.
class interrupted_system_call : public runtime_error
{
public:
    interrupted_system_call()
        : runtime_error{"interrupted system call"}
    {}
};

/// An operation that failed with an OS error. Wands raise this for failed
/// wishes so callers can distinguish errno values, not just messages.
class errno_error : public runtime_error
{
public:
    errno_error(int code, std::string const & message)
        : runtime_error{message}
        , code_(code)
    {}

    [[nodiscard]] int code() const noexcept
    {
        return code_;
    }

private:
    int code_;
};

/// Several exceptions reported together, for example every failure of a
/// `wait_any`, or a `finally` whose body and cleanup both failed.
///
/// `exceptions()` holds the members in order; `summary()` is the short
/// message given at construction, and `what()` renders the summary plus
/// each member, nested groups indented. Usually thrown through
/// `throw_exceptions`, which avoids wrapping a single exception.
class exception_group : public runtime_error
{
public:
    explicit exception_group(
        std::string summary,
        std::vector<std::exception_ptr> exceptions);

    [[nodiscard]] const std::vector<std::exception_ptr> &
    exceptions() const noexcept
    {
        return exceptions_;
    }

    [[nodiscard]] const std::string & summary() const noexcept
    {
        return summary_;
    }

private:
    std::string summary_;
    std::vector<std::exception_ptr> exceptions_;
};

/// Limits for `format_exception_tree`: how many members of each group to
/// print before summarizing the rest as "... N <overflow_label>".
struct exception_tree_options
{
    std::size_t max_group_children = 5;
    std::string_view overflow_label = "more failures";
};

namespace detail {

inline void append_exception_tree(
    std::string & out,
    const std::exception_ptr & failure,
    std::string_view indent,
    exception_tree_options options)
{
    if (!failure)
        return;

    try {
        rethrow(failure);
    } catch (const exception_group & group) {
        out += indent;
        out += group.summary();
        out += '\n';

        auto child_indent = std::string{indent};
        child_indent += "  ";
        auto const & children = group.exceptions();
        auto const shown =
            std::min(children.size(), options.max_group_children);
        for (auto i = std::size_t{0}; i < shown; ++i)
            append_exception_tree(out, children[i], child_indent, options);
        if (children.size() > shown) {
            out += child_indent;
            out += "... ";
            out += std::to_string(children.size() - shown);
            out += ' ';
            out += options.overflow_label;
            out += '\n';
        }
    } catch (const std::exception & e) {
        out += indent;
        out += e.what();
        out += '\n';
    } catch (...) {
        out += indent;
        out += "<non-std exception>\n";
    }
}

} // namespace detail

/// Render `failure` as indented text, one line per exception, expanding
/// `exception_group`s into their members (up to
/// `options.max_group_children` each). Non-`std::exception` values print as
/// `<non-std exception>`. Returns an empty string for a null pointer.
[[nodiscard]] inline std::string format_exception_tree(
    const std::exception_ptr & failure,
    std::string_view indent = "  ",
    exception_tree_options options = {})
{
    auto out = std::string{};
    detail::append_exception_tree(out, failure, indent, options);
    if (!out.empty() && out.back() == '\n')
        out.pop_back();
    return out;
}

namespace detail {

[[nodiscard]] inline std::string format_exception_group_what(
    std::string_view summary,
    const std::vector<std::exception_ptr> & exceptions)
{
    auto out = std::string{summary};
    out += " (";
    out += std::to_string(exceptions.size());
    out += exceptions.size() == 1 ? " exception)" : " exceptions)";

    for (auto i = std::size_t{0}; i < exceptions.size(); ++i) {
        out += "\n  [";
        out += std::to_string(i);
        out += "] ";
        append_exception_tree(out, exceptions[i], "    ", {});
    }

    if (!out.empty() && out.back() == '\n')
        out.pop_back();
    return out;
}

} // namespace detail

inline exception_group::exception_group(
    std::string summary,
    std::vector<std::exception_ptr> exceptions)
    : runtime_error{
        detail::format_exception_group_what(summary, exceptions)}
    , summary_(std::move(summary))
    , exceptions_(std::move(exceptions))
{}

/// The exception a stopped operation ends with.
///
/// Thrown when a cancelled wish resumes, by `throw_if_stop_requested()`,
/// and by groups (`settle`, `when_all`, ...) whose awaiting task was
/// stopped. Groups also report it in the outcome of a job stopped before
/// it started. Test an `exception_ptr` with `is_operation_cancelled`.
class operation_cancelled : public runtime_error
{
public:
    operation_cancelled()
        : runtime_error{"nxtrt operation cancelled"}
    {}
};

/// The exception `timeout_after` throws, and therefore what `with_timeout`
/// throws when its deadline passes first. Distinct from
/// `operation_cancelled`, which means an outside stop request.
class timeout_error : public runtime_error
{
public:
    timeout_error()
        : runtime_error{"nxtrt operation timed out"}
    {}
};

/// Rethrow `failure`, preserving its stack trace when built with
/// cpptrace. Use instead of `std::rethrow_exception`. `failure` must not be
/// null.
[[noreturn]] inline void rethrow(std::exception_ptr failure)
{
#ifdef NXT_HAVE_CPPTRACE
    cpptrace::rethrow(std::move(failure));
#else
    std::rethrow_exception(std::move(failure));
#endif
}

/// Rethrow the exception being handled, like `throw;`, preserving its
/// stack trace when built with cpptrace. Only valid inside a `catch`.
[[noreturn]] inline void rethrow_current_exception()
{
#ifdef NXT_HAVE_CPPTRACE
    cpptrace::rethrow();
#else
    throw;
#endif
}

/// True if `failure` holds an `operation_cancelled` (or a type derived from
/// it); false for null. Does not look inside `exception_group`s.
[[nodiscard]] inline bool is_operation_cancelled(std::exception_ptr failure)
{
    if (!failure)
        return false;
    try {
        rethrow(std::move(failure));
    } catch (const operation_cancelled &) {
        return true;
    } catch (...) {
        return false;
    }
}

/// Throw a list of failures as one: a single exception is rethrown as is,
/// two or more are thrown as an `exception_group` with summary `message`.
/// Throws `logic_error` for an empty list.
[[noreturn]] inline void throw_exceptions(
    std::string message,
    std::vector<std::exception_ptr> exceptions)
{
    if (exceptions.empty())
        throw logic_error{"nxtrt throw_exceptions called without exceptions"};
    if (exceptions.size() == 1)
        rethrow(std::move(exceptions.front()));
    throw exception_group{std::move(message), std::move(exceptions)};
}

} // namespace nxtrt
