#pragma once

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <source_location>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>

#ifdef NXT_HAVE_CPPTRACE
#  include <cpptrace/cpptrace.hpp>
#  include <cpptrace/formatting.hpp>
#endif

namespace nxt::rt {

inline std::string stdout_trace_path()
{
    auto const * raw = std::getenv("NXT_STDOUT_TRACE");
    if (raw == nullptr || std::string_view{raw}.empty())
        return {};

    auto value = std::string_view{raw};
    if (value == "1" || value == "true" || value == "yes"
        || value == "on") {
        return "/tmp/nxt-stdout-trace."
            + std::to_string(static_cast<long long>(::getpid()))
            + ".log";
    }

    return std::string{value};
}

inline std::string escaped_stdout_bytes(std::string_view bytes)
{
    auto out = std::ostringstream{};
    for (auto ch : bytes) {
        auto c = static_cast<unsigned char>(ch);
        switch (c) {
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        case '\x1b':
            out << "\\e";
            break;
        default:
            if (std::isprint(c)) {
                out << static_cast<char>(c);
            } else {
                out << "\\x" << std::hex << std::setw(2)
                    << std::setfill('0') << static_cast<int>(c)
                    << std::dec << std::setfill(' ');
            }
            break;
        }
    }
    return out.str();
}

inline void trace_stdout_write(
    std::string_view bytes,
    std::source_location where = std::source_location::current())
{
    static auto path = stdout_trace_path();
    if (path.empty())
        return;

    static auto mutex = std::mutex{};
    auto guard = std::scoped_lock{mutex};
    if (auto parent = std::filesystem::path{path}.parent_path();
        !parent.empty()) {
        std::error_code ignored;
        std::filesystem::create_directories(parent, ignored);
    }

    auto out = std::ofstream{path, std::ios::app};
    if (!out)
        return;

    static auto sequence = std::size_t{0};
    out << "stdout write #" << ++sequence << " bytes " << bytes.size()
        << "\n  at " << where.file_name() << ':' << where.line()
        << " in " << where.function_name() << "\n";

    auto preview = bytes;
    static constexpr auto preview_limit = std::size_t{4096};
    if (preview.size() > preview_limit)
        preview = preview.substr(0, preview_limit);
    out << "  preview \"" << escaped_stdout_bytes(preview) << '"';
    if (bytes.size() > preview.size())
        out << " ... +" << (bytes.size() - preview.size()) << " bytes";
    out << "\n";

#ifdef NXT_HAVE_CPPTRACE
    cpptrace::formatter{}.print(out, cpptrace::generate_trace(1), false);
#endif
    out << "\n";
}

} // namespace nxt::rt
