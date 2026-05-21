#pragma once

#include <nxtio/stacktrace.hpp>

#include <chrono>
#include <exception>
#include <format>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace boost::ut {

inline int failures = 0;
inline int tests_run = 0;
inline int tests_failed = 0;
inline bool progress_started = false;

inline constexpr double slow_test_failure_ms = 100.0;
inline constexpr std::string_view test_root_name = "nxt";

struct test_result
{
    std::string_view name;
    double elapsed_ms = 0.0;
    bool failed = false;
    std::vector<test_result> children;
};

inline std::vector<test_result> tests;
inline std::vector<test_result *> active_tests;

inline std::string format_ms(double elapsed_ms)
{
    return std::format("{:.0f}ms", elapsed_ms);
}

inline std::string visible_duration(double elapsed_ms)
{
    auto duration = format_ms(elapsed_ms);
    if (elapsed_ms < 1.0)
        return "";
    if (elapsed_ms >= slow_test_failure_ms)
        return "\x1b[31m" + duration + "\x1b[0m";
    if (elapsed_ms >= 16.0)
        return "\x1b[33m" + duration + "\x1b[0m";
    return "\x1b[2m" + duration + "\x1b[0m";
}

inline std::string
tree_prefix(const std::vector<bool> & last_at_depth, bool has_children)
{
    auto out = std::string{};
    for (auto depth = std::size_t{0}; depth < last_at_depth.size();
         ++depth) {
        if (depth + 1 == last_at_depth.size()) {
            if (has_children)
                out += last_at_depth[depth] ? "└┬" : "├┬";
            else
                out += last_at_depth[depth] ? "└─" : "├─";
        } else {
            out += last_at_depth[depth] ? " " : "│";
        }
    }
    return "\x1b[2m" + out + "\x1b[0m ";
}

inline std::string spacer_prefix(const std::vector<bool> & last_at_depth)
{
    auto out = std::string{};
    for (auto depth = std::size_t{0}; depth + 1 < last_at_depth.size();
         ++depth)
        out += last_at_depth[depth] ? " " : "│";
    return "\x1b[2m" + out + "\x1b[0m";
}

inline void print_test_result(
    const test_result & result, const std::vector<bool> & last_at_depth)
{
    auto status = result.failed ? " FAILED" : "";
    auto duration = visible_duration(result.elapsed_ms);
    auto has_children = !result.children.empty();

    std::cout << tree_prefix(last_at_depth, has_children);
    if (has_children)
        std::cout << "\x1b[1m";
    std::cout << result.name << status;
    if (has_children)
        std::cout << "\x1b[0m";
    if (!duration.empty())
        std::cout << ' ' << duration;
    std::cout << '\n';

    for (auto i = std::size_t{0}; i < result.children.size(); ++i) {
        auto child_path = last_at_depth;
        child_path.push_back(i + 1 == result.children.size());
        print_test_result(result.children[i], child_path);
    }

    if (!has_children && !last_at_depth.empty() && last_at_depth.back())
        std::cout << spacer_prefix(last_at_depth) << '\n';
}

inline void print_report()
{
    std::cout << "\x1b[1m" << test_root_name << "\x1b[0m\n";
    for (auto i = std::size_t{0}; i < tests.size(); ++i)
        print_test_result(tests[i], {i + 1 == tests.size()});
}

inline std::string plural(int count, std::string_view singular)
{
    return std::format("{} {}{}", count, singular, count == 1 ? "" : "s");
}

inline void print_summary()
{
    if (tests_failed == 0) {
        std::cout << "\n\x1b[32m✓\x1b[0m all " << plural(tests_run, "test")
                  << " passed\n";
        return;
    }

    std::cout << "\n\x1b[31m✗\x1b[0m " << plural(tests_failed, "test")
              << " failed";
    if (failures != tests_failed)
        std::cout << " with " << plural(failures, "expectation")
                  << " failed";
    std::cout << " out of " << tests_run << '\n';
}

inline void note_progress(bool failed)
{
    if (!progress_started) {
        progress_started = true;
        std::cout << "\x1b[2mrunning tests…\x1b[0m";
    }

    if (failed)
        std::cout << " \x1b[31m×\x1b[0m";
}

struct scoped_test
{
    explicit scoped_test(test_result & result)
    {
        active_tests.push_back(&result);
    }

    ~scoped_test()
    {
        active_tests.pop_back();
    }
};

struct test_case
{
    std::string_view name;

    template<typename F>
    void operator=(F && f) const
    {
        auto & siblings =
            active_tests.empty() ? tests : active_tests.back()->children;
        auto & result = siblings.emplace_back();
        result.name = name;

        auto failures_before = failures;
        auto failed_tests_before = tests_failed;
        auto scope = scoped_test{result};
        auto start = std::chrono::steady_clock::now();
        try {
            std::forward<F>(f)();
        } catch (const std::exception & e) {
            ++failures;
            std::cerr << result.name
                      << ": unexpected exception: " << e.what() << '\n';
            nxt::io::print_current_exception_trace(std::cerr, "  ");
        } catch (...) {
            ++failures;
            std::cerr << result.name << ": unexpected non-std exception\n";
            nxt::io::print_current_exception_trace(std::cerr, "  ");
        }

        auto elapsed =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start);
        result.elapsed_ms = static_cast<double>(elapsed.count()) / 1000.0;

        ++tests_run;
        if (result.elapsed_ms >= slow_test_failure_ms) {
            ++failures;
            std::cerr << result.name
                      << ": too slow: " << format_ms(result.elapsed_ms)
                      << " >= " << format_ms(slow_test_failure_ms) << '\n';
        }

        result.failed = failures != failures_before
                        || tests_failed != failed_tests_before;
        if (result.failed)
            ++tests_failed;

        note_progress(result.failed);
    }
};

struct suite
{
    template<typename F>
    suite(F && f)
    {
        std::forward<F>(f)();
    }

    template<typename F>
    suite(std::string_view name, F && f)
    {
        test_case{name} = std::forward<F>(f);
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
{};

struct run_options
{
    bool report_errors = false;
};

template<typename>
struct config
{
    int run(run_options = {}) const
    {
        if (progress_started)
            std::cout << "\x1b[2m done\x1b[0m\n\n";
        else
            std::cout << '\n';
        print_report();
        print_summary();
        return tests_failed == 0 ? 0 : 1;
    }
};

template<typename T>
inline constexpr config<T> cfg{};

} // namespace boost::ut
