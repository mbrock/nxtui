#pragma once

#include <nxt/stacktrace.hpp>

#include <chrono>
#include <csignal>
#include <cstring>
#include <exception>
#include <format>
#include <functional>
#include <iostream>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <sys/time.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace boost::ut {

inline int failures = 0;
inline int tests_run = 0;
inline int tests_failed = 0;
inline bool progress_started = false;

inline constexpr double slow_test_failure_ms = 1000.0;
inline constexpr std::chrono::seconds test_timeout{1};
inline constexpr std::string_view test_root_name = "nxt";

struct test_result
{
    std::string_view name;
    std::vector<int> path;
    double elapsed_ms = 0.0;
    bool failed = false;
    bool counted = false;
    std::vector<test_result> children;
};

struct test_definition
{
    std::string_view name;
    std::function<void()> body;
};

inline std::vector<test_result> tests;
inline std::vector<test_result *> active_tests;
inline std::vector<int> active_path;
inline std::vector<int> sibling_counts;
inline std::vector<std::vector<int>> filters;
inline std::optional<std::vector<int>> active_leaf_path;
inline char active_timeout_label[512] = {};
inline struct sigaction previous_alarm_action {};
inline bool alarm_handler_installed = false;

enum class run_phase
{
    discovery,
    execution,
};

inline run_phase phase = run_phase::execution;

inline std::vector<test_definition> & test_definitions()
{
    static auto definitions = std::vector<test_definition>{};
    return definitions;
}

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

inline std::string format_path(const std::vector<int> & path)
{
    auto out = std::string{};
    for (auto i = std::size_t{0}; i < path.size(); ++i) {
        if (i != 0)
            out += '.';
        out += std::format("{}", path[i]);
    }
    return out;
}

inline void write_signal_text(std::string_view text)
{
    (void)::write(STDERR_FILENO, text.data(), text.size());
}

inline void write_signal_cstr(const char * text)
{
    if (text != nullptr)
        (void)::write(STDERR_FILENO, text, std::strlen(text));
}

[[noreturn]] inline void test_timeout_handler(int)
{
    write_signal_text("\n\nTEST TIMEOUT after 1s: ");
    write_signal_cstr(active_timeout_label);
    write_signal_text("\n");
    nxt::debug::print_current_stacktrace(
        std::cerr,
        "  ",
        "Timeout site",
        2);
    std::cerr.flush();
    ::_exit(124);
}

inline void install_timeout_handler()
{
    if (alarm_handler_installed)
        return;

    struct sigaction action {};
    action.sa_handler = test_timeout_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESETHAND;
    sigaction(SIGALRM, &action, &previous_alarm_action);
    alarm_handler_installed = true;
}

inline void set_timeout_label(std::string_view label)
{
    auto n = std::min(label.size(), sizeof(active_timeout_label) - 1);
    std::memcpy(active_timeout_label, label.data(), n);
    active_timeout_label[n] = '\0';
}

inline void arm_test_timeout(std::string_view label)
{
    install_timeout_handler();
    set_timeout_label(label);

    auto timer = itimerval{};
    timer.it_value.tv_sec = test_timeout.count();
    setitimer(ITIMER_REAL, &timer, nullptr);
}

inline void disarm_test_timeout()
{
    auto timer = itimerval{};
    setitimer(ITIMER_REAL, &timer, nullptr);
    active_timeout_label[0] = '\0';
}

inline bool path_starts_with(
    const std::vector<int> & path, const std::vector<int> & prefix)
{
    if (prefix.size() > path.size())
        return false;
    for (auto i = std::size_t{0}; i < prefix.size(); ++i)
        if (path[i] != prefix[i])
            return false;
    return true;
}

inline bool selected_path(const std::vector<int> & path)
{
    if (filters.empty())
        return true;
    for (const auto & filter : filters)
        if (path_starts_with(path, filter))
            return true;
    return false;
}

inline bool ancestor_path(const std::vector<int> & path)
{
    for (const auto & filter : filters)
        if (path_starts_with(filter, path))
            return true;
    return false;
}

inline bool related_path(const std::vector<int> & path)
{
    return selected_path(path) || ancestor_path(path);
}

inline bool active_execution_path(const std::vector<int> & path)
{
    return active_leaf_path && path_starts_with(*active_leaf_path, path);
}

inline test_result & find_or_add_result(
    std::vector<test_result> & siblings,
    std::string_view name,
    const std::vector<int> & path)
{
    for (auto & result : siblings)
        if (result.path == path)
            return result;

    auto & result = siblings.emplace_back();
    result.name = name;
    result.path = path;
    return result;
}

inline test_result *
find_result(std::vector<test_result> & results, const std::vector<int> & path)
{
    for (auto & result : results) {
        if (result.path == path)
            return &result;
        if (auto * child = find_result(result.children, path))
            return child;
    }
    return nullptr;
}

inline void collect_selected_leaves(
    const std::vector<test_result> & results,
    std::vector<std::vector<int>> & leaves)
{
    for (const auto & result : results) {
        if (!related_path(result.path))
            continue;
        if (result.children.empty()) {
            if (selected_path(result.path))
                leaves.push_back(result.path);
            continue;
        }
        collect_selected_leaves(result.children, leaves);
    }
}

inline void print_test_result(const test_result & result)
{
    auto status = result.failed ? " FAILED" : "";
    auto duration = visible_duration(result.elapsed_ms);
    auto has_children = !result.children.empty();

    std::cout << "\x1b[2m" << format_path(result.path) << "\x1b[0m  ";
    if (has_children) {
        std::cout << "\x1b[1m";
        for (auto c : result.name)
            std::cout << char(::toupper(static_cast<unsigned char>(c)));
        std::cout << status;
    } else {
        std::cout << result.name << status;
    }
    if (has_children)
        std::cout << "\x1b[0m";
    if (!duration.empty())
        std::cout << ' ' << duration;
    std::cout << '\n';

    for (const auto & child : result.children)
        print_test_result(child);
}

inline void print_report()
{
    std::cout << "\x1b[1m" << test_root_name << "\x1b[0m\n";
    for (const auto & test : tests)
        print_test_result(test);
}

inline std::string plural(int count, std::string_view singular)
{
    return std::format("{} {}{}", count, singular, count == 1 ? "" : "s");
}

inline void print_summary()
{
    if (!filters.empty() && tests_run == 0) {
        std::cout << "\n\x1b[31m✗\x1b[0m no tests matched\n";
        return;
    }

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
    scoped_test(test_result & result, const std::vector<int> & path)
    {
        active_tests.push_back(&result);
        active_path = path;
        sibling_counts.push_back(0);
    }

    ~scoped_test()
    {
        sibling_counts.pop_back();
        active_tests.pop_back();
        if (active_tests.empty())
            active_path.clear();
        else
            active_path = active_tests.back()->path;
    }
};

struct test_case
{
    std::string_view name;

    template<typename F>
    void operator=(F && f) const
    {
        auto path = active_path;
        auto & sibling_count = sibling_counts.back();
        path.push_back(++sibling_count);

        if (phase == run_phase::execution && !active_execution_path(path))
            return;

        auto & siblings =
            active_tests.empty() ? tests : active_tests.back()->children;
        auto & result = phase == run_phase::discovery
                           ? siblings.emplace_back()
                           : find_or_add_result(siblings, name, path);
        if (phase == run_phase::discovery) {
            result.name = name;
            result.path = path;
        }

        auto failures_before = failures;
        auto failed_tests_before = tests_failed;
        auto count_this_test =
            phase == run_phase::execution && active_leaf_path == path;
        auto scope = scoped_test{result, path};
        auto timeout_label =
            std::format("{} {}", format_path(path), result.name);
        arm_test_timeout(timeout_label);
        auto start = std::chrono::steady_clock::now();
        try {
            std::forward<F>(f)();
        } catch (const std::exception & e) {
            if (phase == run_phase::execution) {
                ++failures;
                std::cerr << result.name
                          << ": unexpected exception: " << e.what() << '\n';
                nxt::debug::print_current_exception_trace(std::cerr, "  ");
            }
        } catch (...) {
            if (phase == run_phase::execution) {
                ++failures;
                std::cerr << result.name << ": unexpected non-std exception\n";
                nxt::debug::print_current_exception_trace(std::cerr, "  ");
            }
        }
        disarm_test_timeout();

        auto elapsed =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start);
        if (phase == run_phase::execution)
            result.elapsed_ms += static_cast<double>(elapsed.count()) / 1000.0;

        if (count_this_test) {
            result.counted = true;
            ++tests_run;
        }
        if (count_this_test && result.elapsed_ms >= slow_test_failure_ms) {
            ++failures;
            std::cerr << result.name
                      << ": too slow: " << format_ms(result.elapsed_ms)
                      << " >= " << format_ms(slow_test_failure_ms) << '\n';
        }

        result.failed = result.failed || failures != failures_before
                        || tests_failed != failed_tests_before;
        if (count_this_test && result.failed)
            ++tests_failed;

        if (count_this_test)
            note_progress(result.failed);
    }
};

struct suite
{
    template<typename F>
    suite(F && f)
    {
        test_definitions().push_back(
            {.name = "<anonymous suite>", .body = std::forward<F>(f)});
    }

    template<typename F>
    suite(std::string_view name, F && f)
    {
        test_definitions().push_back(
            {.name = name, .body = std::forward<F>(f)});
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
    if (phase == run_phase::discovery)
        return {true};
    if (!ok)
        ++failures;
    return {ok};
}

struct override
{};

struct run_options
{
    bool report_errors = false;
    int argc = 0;
    char ** argv = nullptr;
};

inline std::optional<std::vector<int>> parse_filter(std::string_view text)
{
    auto path = std::vector<int>{};
    auto i = std::size_t{0};
    while (i < text.size()) {
        if (text[i] < '0' || text[i] > '9')
            return std::nullopt;

        auto value = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            value = value * 10 + (text[i] - '0');
            ++i;
        }
        if (value == 0)
            return std::nullopt;
        path.push_back(value);

        if (i == text.size())
            break;
        if (text[i] != '.')
            return std::nullopt;
        ++i;
        if (i == text.size())
            return std::nullopt;
    }
    return path;
}

inline bool configure_filters(run_options options)
{
    filters.clear();
    for (auto i = 1; i < options.argc; ++i) {
        auto filter = parse_filter(options.argv[i]);
        if (!filter) {
            std::cerr << "invalid test selector: " << options.argv[i]
                      << '\n';
            return false;
        }
        filters.push_back(std::move(*filter));
    }
    return true;
}

inline void reset_run_state()
{
    failures = 0;
    tests_run = 0;
    tests_failed = 0;
    progress_started = false;
    tests.clear();
    active_tests.clear();
    active_path.clear();
    sibling_counts.clear();
    active_leaf_path.reset();
    phase = run_phase::execution;
}

inline void run_registered_tests()
{
    sibling_counts.push_back(0);
    for (const auto & definition : test_definitions())
        test_case{definition.name} = definition.body;
    sibling_counts.pop_back();
}

inline std::vector<std::vector<int>> discover_test_leaves()
{
    tests.clear();
    active_tests.clear();
    active_path.clear();
    sibling_counts.clear();
    active_leaf_path.reset();
    phase = run_phase::discovery;

    run_registered_tests();

    auto leaves = std::vector<std::vector<int>>{};
    collect_selected_leaves(tests, leaves);
    return leaves;
}

inline void run_test_leaf(const std::vector<int> & path)
{
    active_tests.clear();
    active_path.clear();
    sibling_counts.clear();
    active_leaf_path = path;
    phase = run_phase::execution;

    auto failures_before = failures;
    auto tests_run_before = tests_run;
    auto tests_failed_before = tests_failed;

    run_registered_tests();

    if (failures == failures_before && tests_failed == tests_failed_before)
        return;

    if (auto * leaf = find_result(tests, path)) {
        leaf->failed = true;
        leaf->counted = true;
    }
    if (tests_run == tests_run_before)
        ++tests_run;
    if (tests_failed == tests_failed_before)
        ++tests_failed;
}

inline void run_selected_tests()
{
    auto leaves = discover_test_leaves();

    failures = 0;
    tests_run = 0;
    tests_failed = 0;
    progress_started = false;
    tests.clear();

    for (const auto & leaf : leaves)
        run_test_leaf(leaf);

    active_tests.clear();
    active_path.clear();
    sibling_counts.clear();
    active_leaf_path.reset();
    phase = run_phase::execution;
}

template<typename>
struct config
{
    int run(run_options options = {}) const
    {
        reset_run_state();
        if (!configure_filters(options))
            return 1;
        run_selected_tests();

        if (progress_started)
            std::cout << "\x1b[2m done\x1b[0m\n\n";
        else
            std::cout << '\n';
        print_report();
        print_summary();
        return tests_failed == 0
                       && (!filters.empty() ? tests_run != 0 : true)
                   ? 0
                   : 1;
    }
};

template<typename T>
inline constexpr config<T> cfg{};

} // namespace boost::ut
