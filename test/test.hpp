#pragma once

#include <nxt/stacktrace.hpp>
#include <nxt/function-ref.hpp>

#include <chrono>
#include <csignal>
#include <cstring>
#include <exception>
#include <format>
#include <functional>
#include <type_traits>
#include <iostream>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <sys/time.h>
#include <unistd.h>
#include <utility>
#include <vector>

// Nested tests in one pass.
//
// Tests are declared as a tree: `"name"_group = [] { ... }` holds child
// groups and tests, and `"name"_test = [] { ... }` is a leaf. Every node gets
// a dotted number from its position, so `nxt-tests 2.7` selects a subtree.
// The runner walks the tree once: a group body runs only to declare its
// children, and a test body runs only when the test is selected, so nothing
// runs twice and unselected work never runs at all. Results print as each
// test finishes.
//
// Slow tests (`"name"_test.slow()`, or a whole `.slow()` group) are
// integration and stress cases that CI runs but everyday runs skip:
// `--slow` adds them, `--only-slow` runs just them, and a selector naming a
// slow test or something inside it runs it too.
//
// A test body may also be a coroutine, `[]() -> nxtrt::task<void> { ... }`,
// when the file includes task-test.hpp: the runner awaits it on a fresh deck.

namespace boost::ut {

/// std::vformat, compiled once in test-main.cpp: libc++'s std::format is
/// header-only and would otherwise be compiled into every test file.
std::string test_vformat(std::string_view fmt, std::format_args args);

template<typename... Args>
std::string test_format(std::format_string<Args...> fmt, Args &&... args)
{
    return test_vformat(fmt.get(), std::make_format_args(args...));
}

inline int failures = 0;
inline int tests_run = 0;
inline int tests_failed = 0;
inline int slow_skipped = 0;
inline bool include_slow = false;
inline bool only_slow = false;

inline constexpr double slow_test_failure_ms = 1000.0;
inline constexpr std::chrono::seconds test_timeout{1};
inline constexpr std::string_view test_root_name = "nxt";

// Suite registration happens during static initialization; execution happens
// after main configures selectors. Own suite closures because registration
// temporaries have already been destroyed when the runner invokes them.
struct test_definition
{
    std::string_view name;
    std::function<void()> body;
};

inline std::vector<test_definition> & test_definitions()
{
    static auto definitions = std::vector<test_definition>{};
    return definitions;
}

/// One open group on the way down to the running node.
struct open_group
{
    std::string_view name;
    std::vector<int> path;
    bool slow = false;
    bool printed = false;
    int children = 0;
};

inline std::vector<open_group> open_groups;
inline std::vector<std::vector<int>> filters;
inline bool inside_test = false;
inline char active_timeout_label[512] = {};
inline struct sigaction previous_alarm_action {};
inline bool alarm_handler_installed = false;

inline std::string format_ms(double elapsed_ms)
{
    return test_format("{:.0f}ms", elapsed_ms);
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
        out += test_format("{}", path[i]);
    }
    return out;
}

inline void write_signal_text(std::string_view text)
{
    const auto written = ::write(STDERR_FILENO, text.data(), text.size());
    (void)written;
}

inline void write_signal_cstr(const char * text)
{
    if (text != nullptr) {
        const auto written = ::write(STDERR_FILENO, text, std::strlen(text));
        (void)written;
    }
}

[[noreturn]] inline void test_timeout_handler(int)
{
    write_signal_text("\n\nTEST TIMEOUT ");
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

inline void
arm_test_timeout(std::string_view label, std::chrono::seconds timeout)
{
    install_timeout_handler();
    set_timeout_label(label);

    auto timer = itimerval{};
    timer.it_value.tv_sec = timeout.count();
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

/// Inside a selected subtree (everything is, without selectors).
inline bool selected_path(const std::vector<int> & path)
{
    if (filters.empty())
        return true;
    for (const auto & filter : filters)
        if (path_starts_with(path, filter))
            return true;
    return false;
}

/// On the way down to a selected subtree.
inline bool ancestor_path(const std::vector<int> & path)
{
    for (const auto & filter : filters)
        if (path_starts_with(filter, path))
            return true;
    return false;
}

/// A selector names this node or something inside it.
inline bool named_by_selector(const std::vector<int> & path)
{
    return ancestor_path(path);
}

inline void print_line(
    const std::vector<int> & path,
    std::string_view name,
    bool group,
    bool failed,
    double elapsed_ms)
{
    std::cout << "\x1b[2m" << format_path(path) << "\x1b[0m  ";
    if (group) {
        std::cout << "\x1b[1m";
        for (auto c : name)
            std::cout << char(::toupper(static_cast<unsigned char>(c)));
        std::cout << "\x1b[0m";
    } else {
        std::cout << name;
        if (failed)
            std::cout << " \x1b[31mFAILED\x1b[0m";
        if (auto duration = visible_duration(elapsed_ms); !duration.empty())
            std::cout << ' ' << duration;
    }
    std::cout << '\n';
}

/// Group headers print just before their first reported test, so a filtered
/// run never shows empty groups.
inline void print_open_groups()
{
    for (auto & group : open_groups) {
        if (group.printed)
            continue;
        print_line(group.path, group.name, true, false, 0.0);
        group.printed = true;
    }
}

inline std::string plural(int count, std::string_view singular)
{
    return test_format("{} {}{}", count, singular, count == 1 ? "" : "s");
}

inline void print_summary(double elapsed_ms)
{
    auto const slow_note = slow_skipped == 0
                               ? std::string{}
                               : test_format(
                                     " \x1b[2m({} skipped; run with "
                                     "--slow)\x1b[0m",
                                     plural(slow_skipped, "slow test"));
    auto const timing =
        test_format(" \x1b[2min {:.1f}s\x1b[0m", elapsed_ms / 1000.0);

    if ((!filters.empty() || only_slow) && tests_run == 0) {
        std::cout << "\n\x1b[31m✗\x1b[0m no tests matched" << slow_note
                  << '\n';
        return;
    }

    if (tests_failed == 0 && failures == 0) {
        std::cout << "\n\x1b[32m✓\x1b[0m all " << plural(tests_run, "test")
                  << " passed" << timing << slow_note << '\n';
        return;
    }

    std::cout << "\n\x1b[31m✗\x1b[0m " << plural(tests_failed, "test")
              << " failed";
    if (failures != tests_failed)
        std::cout << " with " << plural(failures, "expectation")
                  << " failed";
    std::cout << " out of " << tests_run << timing << slow_note << '\n';
}

/// Runs a test body that returns something to run rather than finishing
/// when called, such as a coroutine task. A header that defines such a type
/// specializes this; see task-test.hpp for `nxtrt::task<void>`.
template<typename Result>
struct test_body_runner;

template<typename F>
void run_body(F & body)
{
    using result_type = std::invoke_result_t<F &>;
    if constexpr (std::is_void_v<result_type>)
        body();
    else
        test_body_runner<result_type>::run(
            nxt::function_ref<result_type()>{body});
}

/// A borrowed test or group body. Erasing it keeps the runner below to one
/// copy per file instead of one per test: each test instantiates only a
/// small thunk that calls its own body.
struct body_ref
{
    nxt::function_ref<void()> call;
    bool returns_void = true;

    void operator()() const
    {
        call();
    }
};

template<typename F>
body_ref make_body_ref(F & body)
{
    return {
        .call = {nxt::nontype<run_body<F>>, body},
        .returns_void = std::is_void_v<std::invoke_result_t<F &>>,
    };
}

inline void run_group(
    std::string_view name, std::vector<int> path, bool slow, body_ref body)
{
    open_groups.push_back(
        {.name = name, .path = std::move(path), .slow = slow});
    try {
        if (body.returns_void) {
            body();
        } else {
            ++failures;
            std::cerr << name << ": a group body only declares tests and "
                                 "cannot be a coroutine\n";
        }
    } catch (const std::exception & e) {
        ++failures;
        std::cerr << name << ": group failed while declaring tests: "
                  << e.what() << '\n';
        nxt::debug::print_current_exception_trace(std::cerr, "  ");
    } catch (...) {
        ++failures;
        std::cerr << name << ": group failed while declaring tests\n";
    }
    open_groups.pop_back();
}

inline void run_test(
    std::string_view name,
    const std::vector<int> & path,
    std::chrono::seconds timeout,
    body_ref body)
{
    auto failures_before = failures;
    arm_test_timeout(
        test_format("after {}s: {} {}", timeout.count(), format_path(path), name),
        timeout);
    inside_test = true;
    auto start = std::chrono::steady_clock::now();
    try {
        body();
    } catch (const std::exception & e) {
        ++failures;
        std::cerr << name << ": unexpected exception: " << e.what() << '\n';
        nxt::debug::print_current_exception_trace(std::cerr, "  ");
    } catch (...) {
        ++failures;
        std::cerr << name << ": unexpected non-std exception\n";
        nxt::debug::print_current_exception_trace(std::cerr, "  ");
    }
    auto elapsed_ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - start)
                          .count();
    inside_test = false;
    disarm_test_timeout();

    auto timeout_ms =
        std::chrono::duration<double, std::milli>(timeout).count();
    if (elapsed_ms >= timeout_ms) {
        ++failures;
        std::cerr << name << ": too slow: " << format_ms(elapsed_ms)
                  << " >= " << format_ms(timeout_ms) << '\n';
    }

    auto failed = failures != failures_before;
    ++tests_run;
    if (failed)
        ++tests_failed;
    print_open_groups();
    print_line(path, name, false, failed, elapsed_ms);
    std::cout.flush();
}

struct test_case
{
    std::string_view name;
    std::chrono::seconds timeout = test_timeout;
    bool is_slow = false;
    bool is_group = false;

    // Full-system integration tests can opt into a longer deadline without
    // weakening the one-second limit on ordinary unit tests.
    test_case with_timeout(std::chrono::seconds limit) const
    {
        auto result = *this;
        result.timeout = limit;
        return result;
    }

    /// Mark a test, or a whole group, as a slow integration case.
    test_case slow() const
    {
        auto result = *this;
        result.is_slow = true;
        return result;
    }

    template<typename F>
    void operator=(F && f) const
    {
        declare(make_body_ref(f));
    }

    void declare(body_ref body) const
    {
        if (inside_test) {
            ++failures;
            std::cerr << "\"" << name
                      << "\" is declared inside a test; declare its parent "
                         "with _group\n";
            return;
        }

        auto & parent = open_groups.back();
        auto path = parent.path;
        path.push_back(++parent.children);
        if (!selected_path(path) && !ancestor_path(path))
            return;

        auto const slow = is_slow || parent.slow;
        if (slow && !include_slow && !only_slow && !named_by_selector(path)) {
            ++slow_skipped;
            return;
        }

        if (is_group) {
            run_group(name, std::move(path), slow, body);
            return;
        }
        // A test cannot hold a deeper selection, so only selected tests run.
        if (!selected_path(path) || (only_slow && !slow))
            return;
        run_test(name, path, timeout, body);
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

inline test_case operator""_group(const char * name, std::size_t len)
{
    return {.name = std::string_view{name, len}, .is_group = true};
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

inline bool configure(run_options options)
{
    filters.clear();
    include_slow = false;
    only_slow = false;
    for (auto i = 1; i < options.argc; ++i) {
        auto const arg = std::string_view{options.argv[i]};
        if (arg == "--slow") {
            include_slow = true;
            continue;
        }
        if (arg == "--only-slow") {
            only_slow = true;
            continue;
        }
        auto filter = parse_filter(arg);
        if (!filter) {
            std::cerr << "invalid test selector: " << arg
                      << " (expected a dotted number like 2.7, --slow, or "
                         "--only-slow)\n";
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
    slow_skipped = 0;
    inside_test = false;
    open_groups.clear();
}

template<typename>
struct config
{
    int run(run_options options = {}) const
    {
        reset_run_state();
        if (!configure(options))
            return 1;

        std::cout << "\x1b[1m" << test_root_name << "\x1b[0m\n";
        std::cout.flush();
        auto start = std::chrono::steady_clock::now();

        // The registered suites are the root's children.
        open_groups.push_back(
            {.name = test_root_name, .path = {}, .printed = true});
        for (const auto & definition : test_definitions())
            test_case{.name = definition.name, .is_group = true} =
                definition.body;
        open_groups.clear();

        print_summary(std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - start)
                          .count());
        auto const matched =
            (filters.empty() && !only_slow) || tests_run != 0;
        return tests_failed == 0 && failures == 0 && matched ? 0 : 1;
    }
};

template<typename T>
inline constexpr config<T> cfg{};

} // namespace boost::ut
