#pragma once

#include <exception>
#include <iostream>
#include <string_view>
#include <utility>

namespace boost::ut {

inline int failures = 0;

struct suite
{
    template<typename F>
    suite(F && f)
    {
        std::forward<F>(f)();
    }
};

struct test_case
{
    std::string_view name;

    template<typename F>
    void operator=(F && f) const
    {
        try {
            std::forward<F>(f)();
        } catch (const std::exception & e) {
            ++failures;
            std::cerr << name << ": unexpected exception: " << e.what()
                      << '\n';
        } catch (...) {
            ++failures;
            std::cerr << name << ": unexpected non-std exception\n";
        }
    }
};

inline test_case operator""_test(const char * name, std::size_t len)
{
    return {std::string_view{name, len}};
}

constexpr int operator""_i(unsigned long long value)
{
    return static_cast<int>(value);
}

constexpr unsigned long operator""_ul(unsigned long long value)
{
    return static_cast<unsigned long>(value);
}

struct expectation
{
    bool ok = false;
    bool explained = false;

    ~expectation()
    {
        if (!ok && !explained)
            std::cerr << "expectation failed\n";
    }

    template<typename T>
    expectation & operator<<(T && msg)
    {
        if (!ok) {
            explained = true;
            std::cerr << std::forward<T>(msg) << '\n';
        }
        return *this;
    }
};

inline expectation expect(bool ok)
{
    if (!ok)
        ++failures;
    return {ok};
}

struct override
{
};

struct run_options
{
    bool report_errors = false;
};

template<typename>
struct config
{
    int run(run_options = {}) const
    {
        return failures == 0 ? 0 : 1;
    }
};

template<typename T>
inline constexpr config<T> cfg{};

} // namespace boost::ut
