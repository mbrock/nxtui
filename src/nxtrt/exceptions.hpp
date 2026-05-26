#pragma once

#include <exception>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef NXT_HAVE_CPPTRACE
#  include <cpptrace/exceptions.hpp>
#  include <cpptrace/from_current.hpp>
#endif

namespace nxtrt {

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

class interrupted_system_call : public runtime_error
{
public:
    interrupted_system_call()
        : runtime_error{"interrupted system call"}
    {}
};

class exception_group : public std::exception
{
public:
    explicit exception_group(
        std::string message,
        std::vector<std::exception_ptr> exceptions)
        : message_(std::move(message))
        , exceptions_(std::move(exceptions))
    {}

    const char * what() const noexcept override
    {
        return message_.c_str();
    }

    [[nodiscard]] const std::vector<std::exception_ptr> &
    exceptions() const noexcept
    {
        return exceptions_;
    }

private:
    std::string message_;
    std::vector<std::exception_ptr> exceptions_;
};

class operation_cancelled : public runtime_error
{
public:
    operation_cancelled()
        : runtime_error{"nxtrt operation cancelled"}
    {}
};

class timeout_error : public runtime_error
{
public:
    timeout_error()
        : runtime_error{"nxtrt operation timed out"}
    {}
};

[[noreturn]] inline void rethrow(std::exception_ptr failure)
{
#ifdef NXT_HAVE_CPPTRACE
    cpptrace::rethrow(std::move(failure));
#else
    std::rethrow_exception(std::move(failure));
#endif
}

[[noreturn]] inline void rethrow_current_exception()
{
#ifdef NXT_HAVE_CPPTRACE
    cpptrace::rethrow();
#else
    throw;
#endif
}

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
