#pragma once

#include <chrono>
#include <exception>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <utility>

namespace boost::ut {

inline int failures = 0;
inline int tests_run = 0;

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
        auto failures_before = failures;
        ++tests_run;
        std::cout << "[ RUN      ] " << name << '\n';
        auto start = std::chrono::steady_clock::now();
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

        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start);
        auto elapsed_ms =
            static_cast<double>(elapsed.count()) / 1000.0;
        if (failures == failures_before)
            std::cout << "[       OK ] " << name << " (" << std::fixed
                      << std::setprecision(3) << elapsed_ms
                      << " ms)\n";
        else
            std::cout << "[  FAILED  ] " << name << " (" << std::fixed
                      << std::setprecision(3) << elapsed_ms
                      << " ms)\n";
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
        std::cout << "[==========] " << tests_run << " tests ran\n";
        if (failures == 0)
            std::cout << "[  PASSED  ] " << tests_run << " tests\n";
        else
            std::cout << "[  FAILED  ] " << failures << " expectations\n";
        return failures == 0 ? 0 : 1;
    }
};

template<typename T>
inline constexpr config<T> cfg{};

} // namespace boost::ut
