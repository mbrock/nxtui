#include "nxt/stacktrace.hpp"

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <ostream>
#include <string>
#include <vector>

#ifdef NXT_HAVE_CPPTRACE
#  include <cpptrace/cpptrace.hpp>
#  include <cpptrace/from_current.hpp>
#  include <cpptrace/utils.hpp>
#elif __has_include(<stacktrace>)
#  include <stacktrace>
#endif

#if defined(__cpp_lib_stacktrace) && __cpp_lib_stacktrace >= 202011L
#  define NXT_HAVE_STD_STACKTRACE 1
#else
#  define NXT_HAVE_STD_STACKTRACE 0
#endif

namespace nxt::debug {

#ifdef NXT_HAVE_CPPTRACE
namespace {

struct CrashColors
{
    bool enabled = false;

    [[nodiscard]] std::string_view reset() const noexcept
    {
        return enabled ? "\033[0m" : "";
    }

    [[nodiscard]] std::string_view dim() const noexcept
    {
        return enabled ? "\033[2m" : "";
    }

    [[nodiscard]] std::string_view symbol() const noexcept
    {
        return enabled ? "\033[1m" : "";
    }

    [[nodiscard]] std::string_view file() const noexcept
    {
        return enabled ? "\033[2m" : "";
    }
};

bool should_color_crash_report()
{
    return cpptrace::isatty(cpptrace::stderr_fileno);
}

std::string relative_filename(
    std::string filename,
    const std::filesystem::path & cwd)
{
    if (filename.empty() || cwd.empty())
        return filename;

    auto path = std::filesystem::path{filename};
    if (!path.is_absolute())
        return filename;

    auto relative = path.lexically_normal().lexically_relative(cwd);
    if (relative.empty())
        return filename;

    auto first = relative.begin();
    if (first != relative.end() && *first == "..")
        return filename;

    return relative.generic_string();
}

bool keep_frame(const cpptrace::stacktrace_frame & f)
{
    if (f.filename.contains("from_current.hpp"))
        return false;
    if (f.symbol.empty())
        return false;
    if (f.symbol.contains("::__") && !f.line.has_value())
        return false;
    if (f.symbol.starts_with("__pthread_")
        || f.symbol.starts_with("_thread_")
        || f.symbol.starts_with("start + "))
        return false;
    if (f.filename.ends_with("/dyld"))
        return false;
    if (f.filename.ends_with("libsystem_pthread.dylib"))
        return false;
    return true;
}

cpptrace::stacktrace_frame normalize_frame(
    cpptrace::stacktrace_frame frame,
    const std::filesystem::path & cwd)
{
    frame.filename = relative_filename(std::move(frame.filename), cwd);
    if (!frame.line.has_value())
        frame.filename.clear();
    return frame;
}

std::vector<cpptrace::stacktrace_frame>
display_frames(const cpptrace::stacktrace & trace)
{
    std::error_code ec;
    auto pwd = std::filesystem::current_path(ec).lexically_normal();
    if (ec)
        pwd.clear();

    auto frames = std::vector<cpptrace::stacktrace_frame>{};
    for (auto frame : trace.frames) {
        frame = normalize_frame(std::move(frame), pwd);
        if (keep_frame(frame))
            frames.push_back(std::move(frame));
    }
    return frames;
}

std::string frame_symbol(const cpptrace::stacktrace_frame & frame)
{
    if (frame.symbol.empty())
        return {};
    return cpptrace::prune_symbol(frame.symbol);
}

} // namespace

void print_stacktrace(
    std::ostream & out,
    const cpptrace::stacktrace & trace,
    std::string_view indent)
{
    auto frames = display_frames(trace);
    if (frames.empty()) {
        out << indent << "<empty trace>\n";
        return;
    }

    auto colors = CrashColors{.enabled = should_color_crash_report()};
    auto width = static_cast<int>(
        std::max<std::size_t>(1, std::to_string(frames.size() - 1).size()));

    for (std::size_t i = 0; i < frames.size(); ++i) {
        const auto & frame = frames[i];
        out << indent << colors.dim()
            << std::setw(width) << std::setfill(' ') << i
            << colors.reset() << ' ';
        auto symbol = frame_symbol(frame);
        if (!symbol.empty())
            out << colors.symbol() << symbol << colors.reset();
        else
            out << colors.dim() << "<unknown>" << colors.reset();
        out << '\n';

        if (!frame.filename.empty()) {
            out << indent << "  " << colors.dim()
                << colors.file() << frame.filename << colors.reset();
            if (frame.line.has_value())
                out << colors.dim() << ':' << frame.line.value()
                    << colors.reset();
            out << '\n';
        }
    }
}
#endif

bool print_current_exception_trace(
    std::ostream & out,
    std::string_view indent,
    std::string_view label)
{
    (void) label;
#ifdef NXT_HAVE_CPPTRACE
    const auto & exception_trace = cpptrace::from_current_exception();
    if (!exception_trace.empty()) {
        out << "\n" << indent << label << ":\n";
        auto trace_indent = std::string{indent};
        print_stacktrace(out, exception_trace, trace_indent);
        if (cpptrace::current_exception_was_rethrown()) {
            const auto & rethrow_trace =
                cpptrace::from_current_exception_rethrow();
            if (!rethrow_trace.empty()) {
                out << indent << "rethrow trace:\n";
                print_stacktrace(out, rethrow_trace, trace_indent);
            }
        }
        return true;
    }

    out << indent << "current trace:\n";
    print_stacktrace(
        out, cpptrace::generate_trace(1), std::string{indent});
    return true;
#elif NXT_HAVE_STD_STACKTRACE
    out << indent << label << ":\n" << std::stacktrace::current();
    return true;
#else
    out << indent << "stack trace unavailable "
           "(cpptrace and standard <stacktrace> are not available)\n";
    return false;
#endif
}

[[noreturn]] void rethrow(std::exception_ptr failure)
{
#ifdef NXT_HAVE_CPPTRACE
    cpptrace::rethrow(std::move(failure));
#else
    std::rethrow_exception(std::move(failure));
#endif
}

[[noreturn]] void rethrow_current_exception()
{
#ifdef NXT_HAVE_CPPTRACE
    cpptrace::rethrow();
#else
    throw;
#endif
}

} // namespace nxt::debug
