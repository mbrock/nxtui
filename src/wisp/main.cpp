// SPDX-License-Identifier: AGPL-3.0-or-later
#include "wisp/reader.hpp"
#include "wisp/printer.hpp"
#include "wisp/tape.hpp"
#include "nxtrt/app.hpp"
#include "nxtrt/bell.hpp"
#include "nxtrt/buffers.hpp"
#include "nxtrt/fs.hpp"
#include "nxtrt/http-server.hpp"
#include "nxtrt/net.hpp"
#include "nxtrt/net_dns.hpp"
#include "nxtrt/subprocess.hpp"
#include "nxtrt/tls.hpp"

#include <cctype>
#include <charconv>
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <system_error>
#include <unistd.h>

namespace wisp {
namespace {

// #embed is intentionally used as a C++23 extension.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wc++26-extensions"
#endif
constexpr unsigned char boot_tape[] = {
#embed "wisp-boot.tape"
};
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

using namespace std::chrono;

struct effect_error : std::runtime_error
{
    std::string_view code;

    effect_error(std::string_view code, std::string_view message)
        : std::runtime_error(std::string{message})
        , code(code)
    {
    }
};

void effect_require(bool yes, std::string_view message)
{
    if (!yes)
        throw effect_error{"INVALID-ARGUMENT", message};
}

void require(bool yes, std::string_view message)
{
    if (!yes)
        throw std::runtime_error(std::string{message});
}

std::span<const word> vector(const heap & h, word value, std::size_t size)
{
    require(tag_of(value) == tag::v32, "host record is not a vector");
    const auto xs = h.v32slice(value);
    require(xs.size() == size, "invalid host record length");
    return xs;
}

std::string text(const heap & h, word value)
{
    require(tag_of(value) == tag::v08, "host expected a string");
    return std::string{h.v08slice(value)};
}

std::int64_t now_ms()
{
    return duration_cast<milliseconds>(
               system_clock::now().time_since_epoch())
        .count();
}

std::int64_t deadline_ms(const heap & h, word value)
{
    const auto str = text(h, value);
    std::int64_t deadline = 0;
    const auto [end, error] =
        std::from_chars(str.data(), str.data() + str.size(), deadline);
    effect_require(
        error == std::errc{} && end == str.data() + str.size()
            && deadline >= 0,
        "invalid timer deadline");
    return deadline;
}

std::string read_file(const std::string & path)
{
    std::ifstream file(path, std::ios::binary);
    require(file.is_open(), path + ":1:1: cannot open source file");
    std::string result;
    std::array<char, 8192> buffer;
    while (file.read(buffer.data(), buffer.size()) || file.gcount()) {
        require(
            result.size() + file.gcount() <= tape::default_limit,
            path + ":1:1: source exceeds 64 MiB");
        result.append(buffer.data(), file.gcount());
    }
    require(
        file.eof() && !file.bad(), path + ":1:1: cannot read source file");
    return result;
}

// Checkpoint files are host-selected, private, and atomically replaced.
// Failure before rename leaves the previous image untouched. A directory
// fsync error after rename is reported, but cannot roll the rename back.
void checkpoint(const std::string & path, const evaluator & vm, word entry)
{
    const auto data = tape::encode(vm, entry);
    std::string temporary = path + ".XXXXXX";
    int fd = ::mkstemp(temporary.data());
    if (fd < 0)
        throw std::system_error(
            errno, std::generic_category(), "checkpoint create");
    bool renamed = false;
    try {
        std::size_t offset = 0;
        while (offset < data.size()) {
            const auto count =
                ::write(fd, data.data() + offset, data.size() - offset);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                throw std::system_error(
                    count < 0 ? errno : EIO,
                    std::generic_category(),
                    "checkpoint write");
            offset += count;
        }
        if (::fsync(fd) != 0)
            throw std::system_error(
                errno, std::generic_category(), "checkpoint fsync");
        const int closed = std::exchange(fd, -1);
        if (::close(closed) != 0)
            throw std::system_error(
                errno, std::generic_category(), "checkpoint close");
        if (::rename(temporary.c_str(), path.c_str()) != 0)
            throw std::system_error(
                errno, std::generic_category(), "checkpoint rename");
        renamed = true;
        const auto slash = path.find_last_of('/');
        const auto directory =
            slash == std::string::npos ? "." : path.substr(0, slash + 1);
        fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0 || ::fsync(fd) != 0)
            throw std::system_error(
                errno,
                std::generic_category(),
                "checkpoint directory fsync");
        const int dir = std::exchange(fd, -1);
        if (::close(dir) != 0)
            throw std::system_error(
                errno,
                std::generic_category(),
                "checkpoint directory close");
    } catch (...) {
        if (fd >= 0)
            ::close(fd);
        if (!renamed)
            ::unlink(temporary.c_str());
        throw;
    }
}

// The path split at "/" without its leading slash, each segment
// percent-decoded; nullopt when an escape is malformed. OPTIONS * gives
// ("*").
std::optional<std::vector<std::string>> path_segments(std::string_view path)
{
    if (path.starts_with('/'))
        path.remove_prefix(1);
    std::vector<std::string> segments;
    for (auto part : std::views::split(path, '/')) {
        const auto raw = std::string_view{part.begin(), part.end()};
        std::string segment;
        for (std::size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] != '%') {
                segment += raw[i];
                continue;
            }
            unsigned value = 0;
            if (raw.size() - i < 3
                || !std::isxdigit(static_cast<unsigned char>(raw[i + 1]))
                || !std::isxdigit(static_cast<unsigned char>(raw[i + 2])))
                return std::nullopt;
            std::from_chars(raw.data() + i + 1, raw.data() + i + 3, value, 16);
            segment += static_cast<char>(value);
            i += 2;
        }
        segments.push_back(std::move(segment));
    }
    return segments;
}

// Directories granted on the command line, like WASI preopens. The
// guest names them by NAME; there is no ambient filesystem authority.
using directories = std::map<std::string, nxt::unique_fd, std::less<>>;

// --dir NAME=PATH, or --dir PATH when PATH is itself a plain name.
void grant(directories & granted, std::string_view spec)
{
    const auto equals = spec.find('=');
    const auto name = spec.substr(0, equals);
    const auto path = equals == spec.npos ? spec : spec.substr(equals + 1);
    require(
        !name.empty() && name.find('/') == name.npos && name != "."
            && name != "..",
        "--dir NAME must be one plain path segment (use NAME=PATH)");
    require(!granted.contains(name), "duplicate --dir name");
    const auto fd = ::open(
        std::string{path}.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        throw std::system_error(
            errno,
            std::generic_category(),
            "cannot open --dir " + std::string{path});
    granted.emplace(std::string{name}, nxt::unique_fd{fd});
}

// Programs granted with --run, by the name the guest uses. Like a --dir
// grant this is explicit authority, but a coarse one: the program itself
// runs with the host's full access.
using programs = std::map<std::string, std::string, std::less<>>;

// --run NAME finds NAME on PATH now; --run NAME=PATH names an executable.
// Either way the guest gets this absolute path, never a PATH lookup.
void grant_program(programs & granted, std::string_view spec)
{
    const auto equals = spec.find('=');
    const auto name = std::string{spec.substr(0, equals)};
    require(
        !name.empty() && name.find('/') == name.npos,
        "--run NAME must not contain '/' (use NAME=PATH)");
    require(!granted.contains(name), "duplicate --run name");
    const auto executable = [](const std::filesystem::path & path) {
        std::error_code error;
        return std::filesystem::is_regular_file(path, error)
               && ::access(path.c_str(), X_OK) == 0;
    };
    auto path = std::filesystem::path{};
    if (equals != spec.npos) {
        path = spec.substr(equals + 1);
    } else {
        const auto * search = std::getenv("PATH");
        for (auto dir : std::views::split(
                 std::string_view{search ? search : ""}, ':')) {
            auto candidate = std::filesystem::path{
                std::string_view{dir.begin(), dir.end()}} / name;
            if (candidate.is_absolute() && executable(candidate)) {
                path = candidate;
                break;
            }
        }
    }
    require(
        !path.empty() && executable(path),
        "--run " + std::string{spec} + ": no such executable");
    granted.emplace(name, std::filesystem::absolute(path).string());
}

struct host
{
    heap & h;
    evaluator & vm;
    root & entry;
    nxtrt::fd_source input{STDIN_FILENO, 1};
    nxtrt::fd_sink output{STDOUT_FILENO};
    nxtrt::fd_sink errors{STDERR_FILENO};
    struct console_guard;
    std::array<std::vector<console_guard *>, 3> console;
    bool effects = false, echo = false, saved = false;
    std::string save;
    std::size_t gc_threshold = 1024 * 1024;
    directories granted;
    programs runnable;

    host(heap & h, evaluator & vm, root & entry)
        : h(h)
        , vm(vm)
        , entry(entry)
    {
    }

    std::size_t heap_bytes() const
    {
        const auto rows = [&]<std::size_t... I>(std::index_sequence<I...>) {
            return (
                (h.table<std::tuple_element_t<I, vat>::type>().size()
                 * sizeof(row<std::tuple_element_t<I, vat>::type>))
                + ...);
        }(std::make_index_sequence<std::tuple_size_v<vat>>{});
        return rows + h.byte_count() + h.word_count() * sizeof(word);
    }

    void collect_if_needed()
    {
        // Only committed safepoints, with every host-held word rooted.
        // Polling does not imply collection: allow growth proportional to
        // the live heap, including after an explicit guest GC request.
        if (vm.collection_requested() || heap_bytes() >= gc_threshold) {
            vm.collect();
            gc_threshold = heap_bytes() * 2 + 1024 * 1024;
        }
    }

    word get(std::size_t field) const
    {
        const auto xs = vector(h, entry.get(), 7);
        require(
            xs[0] == vm.keyword("NXT-WISP-4"), "unsupported host image");
        return xs[field];
    }

    void set(std::size_t field, word value)
    {
        (void) get(field);
        h.v32set(entry.get(), field, value);
    }

    word quote(word value)
    {
        return h.cons(vm.intern("QUOTE"), h.cons(value, nil));
    }

    // Host records are DEFSTRUCTs from host.wisp. Their descriptors are
    // bound to <NAME> and list the slot names, so slot positions are
    // looked up by name here rather than repeated.
    word descriptor(std::string_view name)
    {
        const auto type = h.get<tag::sym, field::val>(
            vm.intern("<" + std::string{name} + ">"));
        require(
            tag_of(type) == tag::rec && h.words<tag::rec>(type).size() == 3,
            "missing host struct descriptor");
        return type;
    }

    std::vector<word> slot_names(word type)
    {
        std::vector<word> names;
        for (auto list = h.words<tag::rec>(type)[2]; list != nil;) {
            const auto [name, rest] = h.read<tag::duo>(list);
            names.push_back(name);
            list = rest;
        }
        return names;
    }

    std::size_t slot_index(word type, std::string_view slot)
    {
        const auto names = slot_names(type);
        for (std::size_t i = 0; i < names.size(); ++i)
            if (h.v08slice(h.get<tag::sym, field::str>(names[i])) == slot)
                return i;
        throw std::logic_error("host struct has no slot " + std::string{slot});
    }

    // Unnamed slots are NIL. Values must be allocated before the call.
    word make_struct(
        std::string_view name,
        std::initializer_list<std::pair<std::string_view, word>> slots)
    {
        const auto type = descriptor(name);
        std::vector<word> words(1 + slot_names(type).size(), nil);
        words[0] = type;
        for (const auto & [slot, value] : slots)
            words[1 + slot_index(type, slot)] = value;
        return h.new_words<tag::rec>(words);
    }

    bool is_struct(word value, std::string_view name)
    {
        return tag_of(value) == tag::rec
               && h.words<tag::rec>(value)[0] == descriptor(name);
    }

    word slot(word value, std::string_view name, std::string_view slot)
    {
        effect_require(
            is_struct(value, name),
            "expected " + std::string{name} + " struct");
        return h.words<tag::rec>(value)[1 + slot_index(descriptor(name), slot)];
    }

    word call(word function, word argument)
    {
        return h.cons(
            vm.intern("CALL"),
            h.cons(quote(function), h.cons(quote(argument), nil)));
    }

    nxtrt::task<void> write(nxtrt::fd_sink & sink, std::string value)
    {
        co_await sink.write(nxtrt::as_bytes(value));
        co_await sink.flush();
    }

    nxtrt::task<std::optional<std::string>> line()
    {
        std::string value;
        while (const auto byte = co_await input.take()) {
            if (*byte == std::byte{'\n'})
                co_return value;
            effect_require(
                value.size() < tape::default_limit,
                "input line exceeds 64 MiB");
            value += char(std::to_integer<unsigned char>(*byte));
        }
        if (value.empty())
            co_return std::nullopt;
        co_return value;
    }

    void source(std::string_view value, std::string_view path = "<repl>")
    {
        require(
            value.size() <= tape::default_limit, "source exceeds 64 MiB");
        // Source name and enclosing form position are saved with the text;
        // diagnostics after restore never consult the original file.
        const auto source =
            h.newv32(std::array{h.newv08(value), h.newv08(path), word{0}});
        entry.set(h.newv32(
            std::array{
                vm.keyword("NXT-WISP-4"),
                source,
                word{0},
                nil,
                nil,
                nil,
                h.newv08("0")}));
    }

    std::string location() const
    {
        const auto source = vector(h, get(1), 3);
        require(
            tag_of(source[2]) == tag::integer && integer(source[2]) >= 0,
            "invalid source form position");
        return source_location(
            text(h, source[0]), text(h, source[1]), integer(source[2]));
    }

    void identify(root & pending)
    {
        if (vector(h, pending.get(), 5)[0] != nil)
            return;
        const auto counter = text(h, get(6));
        std::uint64_t serial = 0;
        const auto [end, error] = std::from_chars(
            counter.data(), counter.data() + counter.size(), serial);
        require(
            error == std::errc{} && end == counter.data() + counter.size()
                && serial != std::numeric_limits<std::uint64_t>::max(),
            "invalid or exhausted host request counter");
        const auto id = h.newv08(std::to_string(serial + 1));
        set(6, id);
        h.v32set(pending.get(), 0, id);
    }

    word start(word thunk, word state)
    {
        return vm.start(h.cons(
            vm.intern("%NXT-START"),
            h.cons(quote(thunk), h.cons(quote(state), nil))));
    }

    // Only console operations need serialization. Each waiter owns its bell
    // and removes itself on cancellation; there are no evaluator workers.
    struct console_guard
    {
        host & app;
        int stream;
        nxtrt::bell wake;

        console_guard(host & app, int stream)
            : app(app)
            , stream(stream)
        {
            app.console[stream].push_back(this);
        }

        nxtrt::task<void> acquire()
        {
            while (app.console[stream].front() != this) {
                wake.reset();
                co_await wake;
            }
            nxtrt::throw_if_stop_requested();
        }

        ~console_guard()
        {
            auto & queue = app.console[stream];
            std::erase(queue, this);
            if (!queue.empty())
                queue.front()->wake.ring();
        }
    };

    // A guest path is NAME or NAME/RELATIVE, where NAME was granted with
    // --dir. The view borrows PATH.
    std::pair<int, std::string_view> beneath(std::string_view path) const
    {
        const auto slash = path.find('/');
        const auto found = granted.find(path.substr(0, slash));
        if (found == granted.end())
            throw effect_error{
                "NOT-CAPABLE", "path is not beneath a granted --dir"};
        return {
            found->second.get(),
            slash == path.npos ? std::string_view{} : path.substr(slash + 1)};
    }

    // Filesystem calls run as io_uring operations or on these workers, so a
    // slow disk never stalls other callbacks. Started on first use.
    std::optional<nxtrt::blocking_pool> workers;
    std::optional<nxtrt::fs::files> file_io;

    nxtrt::fs::files & files()
    {
        if (!file_io) {
            workers.emplace(4, 64);
            file_io.emplace(*workers);
        }
        return *file_io;
    }

    // Symlinks are never followed (ELOOP), so they are not capabilities.
    [[noreturn]] static void filesystem_failure(std::exception_ptr failure)
    {
        try {
            std::rethrow_exception(failure);
        } catch (const std::invalid_argument & error) {
            throw effect_error{"INVALID-ARGUMENT", error.what()};
        } catch (const nxtrt::errno_error & error) {
            const auto code = error.code();
            throw effect_error{
                code == ENOENT || code == ENOTDIR ? "NOT-FOUND"
                : code == ELOOP                   ? "NOT-CAPABLE"
                : code == EACCES || code == EPERM ? "PERMISSION-DENIED"
                                                  : "IO",
                error.what()};
        }
    }

    std::string guest_path(word value, std::string_view operation)
    {
        effect_require(
            tag_of(value) == tag::v08,
            std::string{operation} + " expects a path string");
        return text(h, value);
    }

    // Opens a regular file with its size, without blocking on FIFOs.
    nxtrt::task<std::pair<nxt::unique_fd, std::uint64_t>>
    open_regular(std::string path)
    {
        const auto [dir, relative] = beneath(path);
        auto fd = nxt::unique_fd{};
        try {
            fd = co_await files().open_beneath(
                dir, std::string{relative}, O_RDONLY | O_NONBLOCK);
        } catch (...) {
            filesystem_failure(std::current_exception());
        }
        struct stat info {};
        if (::fstat(fd.get(), &info) != 0)
            throw effect_error{"IO", "fstat failed"};
        effect_require(S_ISREG(info.st_mode), "not a regular file");
        co_return std::pair{std::move(fd), std::uint64_t(info.st_size)};
    }

    nxtrt::task<void> read_file(word argument, root & result)
    {
        auto opened = co_await open_regular(guest_path(argument, "read-file"));
        auto & [fd, size] = opened;
        effect_require(size <= tape::default_limit, "file exceeds 64 MiB");
        std::string bytes(size, '\0');
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            std::size_t count = 0;
            try {
                count = co_await files().read_at(
                    fd.get(),
                    std::as_writable_bytes(std::span{bytes}).subspan(offset),
                    offset);
            } catch (...) {
                filesystem_failure(std::current_exception());
            }
            if (count == 0)
                break; // truncated meanwhile
            offset += count;
        }
        bytes.resize(offset);
        result.set(h.newv08(bytes));
    }

    // A FILE-STATUS, or NIL when nothing is there. Numbers are decimal
    // strings, like timer deadlines: fixnums are only 31 bits.
    nxtrt::task<void> file_status(word argument, root & result)
    {
        const auto path = guest_path(argument, "file-status");
        const auto [dir, relative] = beneath(path);
        auto status = nxtrt::fs::file_status{};
        try {
            status =
                co_await files().stat_beneath(dir, std::string{relative});
        } catch (const nxtrt::errno_error & error) {
            if (error.code() == ENOENT || error.code() == ENOTDIR)
                co_return;
            filesystem_failure(std::current_exception());
        } catch (...) {
            filesystem_failure(std::current_exception());
        }
        using kind = nxtrt::fs::file_kind;
        const auto type = vm.keyword(
            status.kind == kind::regular     ? "FILE"
            : status.kind == kind::directory ? "DIRECTORY"
            : status.kind == kind::symlink   ? "SYMLINK"
                                             : "OTHER");
        const auto size = h.newv08(std::to_string(status.size));
        const auto modified = h.newv08(std::to_string(status.modified_ms));
        result.set(make_struct(
            "FILE-STATUS",
            {{"KIND", type}, {"SIZE", size}, {"MODIFIED", modified}}));
    }

    nxtrt::task<void> list_directory(word argument, root & result)
    {
        const auto path = guest_path(argument, "list-directory");
        const auto [dir, relative] = beneath(path);
        auto entries = std::vector<nxtrt::fs::directory_entry>{};
        try {
            entries =
                co_await files().list_beneath(dir, std::string{relative});
        } catch (...) {
            filesystem_failure(std::current_exception());
        }
        auto list = nil;
        for (const auto & entry : entries | std::views::reverse)
            list = h.cons(h.newv08(entry.name), list);
        result.set(list);
    }

    static constexpr std::size_t max_command_output = 8 * 1024 * 1024;

    // Reads merged output until EOF, or until it exceeds the limit.
    static nxtrt::task<bool>
    read_command_output(int fd, std::string & output)
    {
        auto storage = std::array<std::byte, 16 * 1024>{};
        auto source = nxtrt::fd_source{fd, std::span{storage}};
        while (const auto chunk = co_await source.take_some()) {
            if (output.size() + chunk->size() > max_command_output)
                co_return false;
            output.append(nxtrt::as_string_view(*chunk));
        }
        co_return true;
    }

    static nxtrt::task<nxtrt::child_result> collect_command(
        nxtrt::subprocess::piped_child & child,
        std::string & output,
        bool & waited)
    {
        const auto complete =
            co_await read_command_output(child.output_fd(), output);
        child.output.reset();
        if (!complete) {
            (void) co_await nxtrt::subprocess::terminate_and_wait(child);
            waited = true;
            throw effect_error{"TOO-LARGE", "command output exceeds 8 MiB"};
        }
        auto status = co_await nxtrt::subprocess::wait_child(child);
        waited = true;
        co_return status;
    }

    // On cancellation (an HTTP handler timeout, say) the child is
    // terminated and reaped before the effect settles.
    static nxtrt::task<void>
    settle_command(nxtrt::subprocess::piped_child & child, bool & waited)
    {
        if (!waited)
            (void) co_await nxtrt::subprocess::terminate_and_wait(child);
        waited = true;
    }

    // [name arguments] -> PROCESS-RESULT. Stdin is /dev/null; stdout and
    // stderr arrive merged. A nonzero exit is a result, not an error.
    nxtrt::task<void> run_command(root & pending, root & result)
    {
        if (!save.empty())
            throw effect_error{
                "NOT-REPLAYABLE", "commands cannot be checkpointed"};
        const auto args =
            vector(h, vector(h, vector(h, pending.get(), 5)[1], 2)[1], 2);
        effect_require(
            tag_of(args[0]) == tag::v08,
            "run-command expects a program name and string arguments");
        const auto found = runnable.find(text(h, args[0]));
        if (found == runnable.end())
            throw effect_error{
                "NOT-CAPABLE", "program was not granted with --run"};
        std::vector<std::string> argv{found->second};
        std::set<word> seen;
        for (auto list = args[1]; list != nil;) {
            effect_require(
                tag_of(list) == tag::duo && seen.insert(list).second,
                "run-command arguments must be a proper list");
            const auto [arg, rest] = h.read<tag::duo>(list);
            effect_require(
                tag_of(arg) == tag::v08
                    && h.v08slice(arg).find('\0') == std::string_view::npos,
                "run-command arguments must be strings without NUL");
            argv.push_back(text(h, arg));
            list = rest;
        }
        // Every guest value has been copied; only owned data from here.
        auto child = nxtrt::subprocess::piped_child{};
        try {
            child = co_await nxtrt::subprocess::spawn_piped(std::move(argv));
        } catch (const nxtrt::errno_error & error) {
            throw effect_error{"IO", error.what()};
        }
        std::string output;
        bool waited = false;
        const auto status = co_await nxtrt::finally(
            collect_command(child, output, waited),
            [&child, &waited] { return settle_command(child, waited); });
        const auto text = h.newv08(output);
        result.set(make_struct(
            "PROCESS-RESULT",
            {{"EXIT-CODE", status.exited ? fixnum(status.exit_code) : nil},
             {"SIGNAL", status.signaled ? fixnum(status.signal) : nil},
             {"OUTPUT", text}}));
    }

    nxtrt::task<nxtrt::http::response>
    http_request(root & pending, nxtrt::http::request request)
    {
        // Copy the native request into heap data before scheduling it.
        // No views or unrooted words survive the first suspension.
        const auto query = request.target.find('?');
        const auto path = request.target.substr(0, query);
        const auto decoded = path_segments(path);
        if (!decoded)
            co_return nxtrt::http::response{400, {}, "Bad Request\n", {}};
        auto segments = nil;
        for (const auto & segment : *decoded | std::views::reverse)
            segments = h.cons(h.newv08(segment), segments);
        auto headers = nil;
        for (const auto & header : request.headers | std::views::reverse)
            headers = h.cons(
                h.newv32(
                    std::array{h.newv08(header.name), h.newv08(header.value)}),
                headers);
        const auto method = h.newv08(request.method);
        const auto raw_path = h.newv08(path);
        const auto raw_query = h.newv08(
            query == std::string::npos ? "" : request.target.substr(query + 1));
        const auto request_body = h.newv08(request.body);
        const auto value = make_struct(
            "HTTP-REQUEST",
            {{"METHOD", method},
             {"PATH", raw_path},
             {"QUERY", raw_query},
             {"HEADERS", headers},
             {"BODY", request_body},
             {"SEGMENTS", segments}});
        const auto handler = vector(
            h, vector(h, vector(h, pending.get(), 5)[1], 2)[1], 2)[1];
        const auto expression = h.cons(
            vm.intern("%NXT-HTTP-HANDLE"),
            h.cons(quote(handler), h.cons(quote(value), nil)));
        const auto thunk = h.make<tag::fun>({nil, nil, expression, nil, 0});
        root state{h, h.filledv32(7, nil)};
        h.v32set(state.get(), 3, start(thunk, state.get()));
        // This callback directly owns its activation and native awaits. The
        // server's deadline/stop follows this task, with no guest job
        // registry.
        if (!co_await execute(state))
            co_return nxtrt::http::response{
                500, {}, "Internal Server Error\n", {}};
        const auto result = vector(h, state.get(), 7)[5];
        const auto status = slot(result, "HTTP-RESPONSE", "STATUS");
        effect_require(tag_of(status) == tag::integer, "invalid HTTP status");
        nxtrt::http::response response;
        response.status = integer(status);
        // Opened only after every heap value is copied: awaiting lets other
        // callbacks run and collect, which invalidates RESULT.
        std::optional<std::string> file;
        const auto body = slot(result, "HTTP-RESPONSE", "BODY");
        if (is_struct(body, "FILE-BODY")) {
            const auto path = slot(body, "FILE-BODY", "PATH");
            effect_require(
                tag_of(path) == tag::v08, "FILE-BODY path must be a string");
            file = text(h, path);
        } else if (body != nil) {
            effect_require(
                tag_of(body) == tag::v08,
                "HTTP body must be a string or a FILE-BODY");
            effect_require(
                h.v08slice(body).size()
                    <= nxtrt::http::server_options{}
                           .max_response_body_bytes,
                "HTTP body too large");
            response.body = text(h, body);
        }
        std::set<word> seen;
        std::size_t header_bytes = 0;
        for (auto list = slot(result, "HTTP-RESPONSE", "HEADERS");
             list != nil;) {
            effect_require(
                tag_of(list) == tag::duo && seen.insert(list).second,
                "HTTP headers must be a proper list");
            const auto [pair, rest] = h.read<tag::duo>(list);
            const auto header = vector(h, pair, 2);
            const auto name = text(h, header[0]),
                       value = text(h, header[1]);
            header_bytes += name.size() + value.size() + 4;
            effect_require(
                header_bytes <= nxtrt::http::server_options{}
                                    .max_response_header_bytes,
                "HTTP headers too large");
            response.headers.push_back({name, value});
            list = rest;
        }
        if (file) {
            auto opened = co_await open_regular(std::move(*file));
            auto & [fd, size] = opened;
            response.file = nxtrt::http::file_body{std::move(fd), 0, size};
        }
        co_return response;
    }

    nxtrt::task<void> http_serve(root & pending)
    {
        const auto args =
            vector(h, vector(h, vector(h, pending.get(), 5)[1], 2)[1], 2);
        effect_require(
            tag_of(args[0]) == tag::integer && integer(args[0]) > 0
                && integer(args[0]) <= 65535 && tag_of(args[1]) == tag::fun,
            "serve-http expects a port (1..65535) and a handler");
        if (!save.empty())
            throw effect_error{
                "NOT-REPLAYABLE", "HTTP listeners cannot be checkpointed"};
        const auto listener =
            nxtrt::net::listen_tcp_loopback(integer(args[0]));
        // This lambda returns a named coroutine; its closure is not itself
        // a coroutine frame. The server drains callbacks before returning.
        auto options = nxtrt::http::server_options{};
        options.files = &files();
        co_await nxtrt::http::serve(
            listener.get(),
            [&](nxtrt::http::request request) {
                return http_request(pending, std::move(request));
            },
            options);
    }

    nxtrt::task<void> http_fetch(root & pending, root & result)
    {
        if (!save.empty())
            throw effect_error{
                "NOT-REPLAYABLE", "HTTP requests cannot be checkpointed"};
        const auto args =
            vector(h, vector(h, vector(h, pending.get(), 5)[1], 2)[1], 4);
        effect_require(
            tag_of(args[0]) == tag::v08 && tag_of(args[1]) == tag::v08
                && (args[3] == nil || tag_of(args[3]) == tag::v08),
            "fetch-http expects URL, method, headers and optional string body");
        const auto url_text = text(h, args[0]);
        effect_require(url_text.size() <= 16 * 1024, "HTTP URL too large");
        const auto url = nxtrt::http::parse_url(url_text);
        effect_require(
            std::ranges::all_of(
                url.host,
                [](unsigned char c) {
                    return c > 32 && c != 127
                           && std::string_view{"/@\\?#[]"}.find(c)
                                  == std::string_view::npos;
                })
                && !url.port.empty()
                && std::ranges::all_of(
                    url.port,
                    [](unsigned char c) { return c >= '0' && c <= '9'; })
                && std::ranges::all_of(
                    url.target,
                    [](unsigned char c) {
                        return c > 32 && c != 127 && c != '#';
                    }),
            "invalid HTTP URL (use a DNS/IPv4 host and an escaped /path)");
        const auto token = [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                   || (c >= '0' && c <= '9')
                   || std::string_view{"!#$%&'*+-.^_`|~"}.find(c)
                          != std::string_view::npos;
        };
        auto request = nxtrt::http::request{
            .method = text(h, args[1]),
            .target = url.target,
            .host = nxtrt::http::host_header(url),
            .headers = {},
            .body = args[3] == nil ? "" : text(h, args[3]),
        };
        effect_require(
            !request.method.empty()
                && std::ranges::all_of(request.method, token)
                && request.method != "CONNECT",
            "invalid or unsupported HTTP method");
        effect_require(
            request.body.size() <= 1024 * 1024,
            "HTTP request body too large");
        std::set<word> seen;
        auto header_bytes = request.method.size() + request.target.size()
                            + request.host.size() + 128;
        bool accept_encoding = false;
        for (auto list = args[2]; list != nil;) {
            effect_require(
                tag_of(list) == tag::duo && seen.insert(list).second,
                "HTTP headers must be a proper list");
            const auto [pair, rest] = h.read<tag::duo>(list);
            effect_require(
                tag_of(pair) == tag::v32 && h.v32slice(pair).size() == 2,
                "HTTP header must be a [name value] vector");
            const auto header = vector(h, pair, 2);
            effect_require(
                tag_of(header[0]) == tag::v08
                    && tag_of(header[1]) == tag::v08,
                "HTTP header name and value must be strings");
            const auto name = text(h, header[0]),
                       value = text(h, header[1]);
            effect_require(
                !name.empty() && std::ranges::all_of(name, token)
                    && std::ranges::all_of(
                        value,
                        [](unsigned char c) {
                            return c == '\t' || (c >= 32 && c != 127);
                        }),
                "invalid HTTP header");
            for (auto managed :
                 {"Host",
                  "Content-Length",
                  "Connection",
                  "Transfer-Encoding",
                  "Trailer",
                  "Upgrade",
                  "Expect"})
                effect_require(
                    !nxtrt::http::iequals(name, managed),
                    "HTTP transport headers are managed by fetch-http");
            accept_encoding |=
                nxtrt::http::iequals(name, "Accept-Encoding");
            header_bytes += name.size() + value.size() + 4;
            effect_require(
                header_bytes <= 16 * 1024, "HTTP headers too large");
            request.headers.push_back({name, value});
            list = rest;
        }
        effect_require(header_bytes <= 16 * 1024, "HTTP headers too large");
        if (!accept_encoding)
            request.headers.push_back({"Accept-Encoding", "gzip, deflate"});
        const auto wire = nxtrt::http::serialize(request);

        // All guest arguments have been copied. Only rooted records, never
        // heap views or words, are consulted after this first suspension.
        auto socket = co_await nxtrt::net::connect_tcp(url.host, url.port);
        auto sink =
            nxtrt::socket_sink{socket.get(), 0, std::size_t{16 * 1024}};
        auto source =
            nxtrt::socket_source{socket.get(), 0, std::size_t{16 * 1024}};
        std::optional<nxtrt::tls::tls13_client_session> tls;
        auto * transport = static_cast<nxtrt::bytefeed *>(&source);
        if (url.tls) {
            tls.emplace(source, sink, std::size_t{16 * 1024});
            co_await tls->handshake(url.host);
            // TLS application records are limited to 16 KiB of plaintext.
            for (std::size_t offset = 0; offset < wire.size();
                 offset += 16 * 1024)
                co_await tls->write_all(
                    std::string_view{wire}.substr(offset, 16 * 1024));
            transport = &*tls;
        } else {
            co_await nxtrt::write(sink, std::string_view{wire});
            co_await sink.flush();
        }
        auto head = co_await nxtrt::http::read_response_head(*transport);
        while (head.status >= 100 && head.status < 200) {
            if (head.status == 101)
                throw nxtrt::http::protocol_error{
                    "HTTP upgrades are unsupported"};
            head = co_await nxtrt::http::read_response_head(*transport);
        }
        std::string body;
        if (request.method != "HEAD" && head.status != 204
            && head.status != 205 && head.status != 304) {
            auto reader = nxtrt::http::response_body_decoding_reader(
                *transport, head);
            while (const auto chunk = co_await reader.take_some()) {
                if (body.size() + chunk->size() > 8 * 1024 * 1024)
                    throw nxtrt::http::protocol_error{
                        "HTTP response body too large"};
                body.append(nxtrt::as_string_view(*chunk));
            }
        }
        auto headers = nil;
        for (const auto & header : head.headers | std::views::reverse)
            headers = h.cons(
                h.newv32(
                    std::array{
                        h.newv08(header.name), h.newv08(header.value)}),
                headers);
        const auto text = h.newv08(body);
        result.set(make_struct(
            "HTTP-RESPONSE",
            {{"STATUS", fixnum(head.status)},
             {"HEADERS", headers},
             {"BODY", text}}));
    }

    // Native registrations are temporary. The guest record remains rooted
    // throughout the await and is removed only when a resume/raise run has
    // been installed. Results also need roots: another callback can collect
    // between this task returning and its caller resuming.
    nxtrt::task<void> perform(root & pending, root & result)
    {
        const auto record = vector(h, pending.get(), 5);
        const auto request = vector(h, record[1], 2);
        const auto operation = request[0], argument = request[1];
        if (operation == vm.keyword("TIMER")) {
            const auto deadline = deadline_ms(h, record[2]);
            // Recheck wall time in bounded monotonic waits. Already elapsed
            // timers fire immediately after restore, never restart a delay.
            while (true) {
                const auto remaining = deadline - now_ms();
                if (remaining <= 0)
                    break;
                co_await nxtrt::op::timeout::after(
                    milliseconds{std::min<std::int64_t>(remaining, 1000)});
            }
            co_return;
        }
        if (operation == vm.keyword("HTTP-SERVE")) {
            co_await http_serve(pending);
            co_return;
        }
        if (operation == vm.keyword("HTTP-FETCH")) {
            try {
                co_await nxtrt::with_timeout(
                    seconds{30}, http_fetch(pending, result));
            } catch (const nxtrt::timeout_error &) {
                throw effect_error{"TIMEOUT", "HTTP request timed out"};
            }
            co_return;
        }
        if (operation == vm.keyword("RUN-COMMAND")) {
            co_await run_command(pending, result);
            co_return;
        }
        if (operation == vm.keyword("READ-FILE")) {
            co_await read_file(argument, result);
            co_return;
        }
        if (operation == vm.keyword("FILE-STATUS")) {
            co_await file_status(argument, result);
            co_return;
        }
        if (operation == vm.keyword("LIST-DIRECTORY")) {
            co_await list_directory(argument, result);
            co_return;
        }
        if (operation == vm.keyword("STDOUT")
            || operation == vm.keyword("STDERR")) {
            std::string bytes;
            std::set<word> seen;
            for (auto cur = argument; cur != nil;) {
                effect_require(
                    tag_of(cur) == tag::duo && seen.insert(cur).second,
                    "invalid write arguments");
                const auto [string, next] = h.read<tag::duo>(cur);
                effect_require(
                    tag_of(string) == tag::v08, "write expected a string");
                bytes += text(h, string);
                cur = next;
            }
            const int stream = operation == vm.keyword("STDOUT") ? 1 : 2;
            // Own cleanup before acquisition: cancellation can arrive
            // after acquire returns but before this coroutine resumes.
            console_guard guard{*this, stream};
            co_await guard.acquire();
            co_await write(stream == 1 ? output : errors, std::move(bytes));
            co_return;
        }
        if (operation == vm.keyword("READ-LINE")) {
            console_guard guard{*this, 0};
            co_await guard.acquire();
            const auto value = co_await line();
            result.set(value ? h.newv08(*value) : nil);
            co_return;
        }
        if (operation == vm.keyword("READ-BYTES")) {
            effect_require(
                tag_of(argument) == tag::integer && integer(argument) >= 0
                    && integer(argument)
                           <= std::int64_t(tape::default_limit),
                "invalid byte count");
            const auto count = integer(argument);
            console_guard guard{*this, 0};
            co_await guard.acquire();
            std::string bytes;
            for (int i = 0; i < count; ++i) {
                const auto byte = co_await input.take();
                if (!byte)
                    break;
                bytes += char(std::to_integer<unsigned char>(*byte));
            }
            result.set(h.newv08(bytes));
            co_return;
        }
        throw effect_error{
            "UNSUPPORTED-OPERATION", "unsupported host operation"};
    }

    word
    failure(word pending, std::string_view code, std::string_view message)
    {
        const auto operation = vector(h, vector(h, pending, 5)[1], 2)[0];
        const auto keyword = vm.keyword(code);
        const auto text = h.newv08(message);
        return make_struct(
            "HOST-ERROR",
            {{"OPERATION", operation}, {"CODE", keyword}, {"MESSAGE", text}});
    }

    nxtrt::task<bool> execute(root & state, bool decline = false)
    {
        while (true) {
            nxtrt::throw_if_stop_requested();
            root run{h, vector(h, state.get(), 7)[3]};
            if (run.get() != nil) {
                require(tag_of(run.get()) == tag::run, "invalid host run");
                // Run to return/effect, not to an evaluator scheduling
                // quantum. Poll allocation growth every 4096 step calls,
                // after scratch registers (including nested STEP!) commit.
                // Collection moves run/state roots but never yields: a
                // runnable result immediately continues this activation.
                while (true) {
                    const auto outcome = vm.advance(run.get(), 4096);
                    collect_if_needed();
                    if (outcome == evaluation::failed)
                        co_return false;
                    if (outcome == evaluation::done)
                        break;
                }
                h.v32set(
                    state.get(), 5, h.get<tag::run, field::val>(run.get()));
                h.v32set(state.get(), 3, nil);
            }
            root pending{h, vector(h, state.get(), 7)[4]};
            if (pending.get() != nil) {
                identify(pending);
                auto record = vector(h, pending.get(), 5);
                auto request = vector(h, record[1], 2);
                require(
                    tag_of(record[0]) == tag::v08
                        && tag_of(record[3]) == tag::fun
                        && tag_of(record[4]) == tag::fun,
                    "invalid pending request");
                require(
                    effects,
                    "effects disabled; inspect the tape or restore with --effects");
                root result{h};
                bool failed = false;
                bool armed = false;
                try {
                    if (decline) {
                        decline = false;
                        throw effect_error{
                            "CANCELLED", "operation cancelled"};
                    }
                    if (request[0] == vm.keyword("TIMER")
                        && record[2] == nil) {
                        effect_require(
                            tag_of(request[1]) == tag::integer
                                && integer(request[1]) >= 0,
                            "invalid timer delay");
                        const auto deadline = h.newv08(
                            std::to_string(now_ms() + integer(request[1])));
                        h.v32set(pending.get(), 2, deadline);
                        armed = true;
                    }
                } catch (const effect_error & error) {
                    failed = true;
                    result.set(
                        failure(pending.get(), error.code, error.what()));
                }
                if (!failed && armed && !save.empty()) {
                    // No other guest activations exist in checkpoint mode:
                    // network effects are rejected, and console awaits
                    // drain before reaching this newly issued timer.
                    vm.collect();
                    checkpoint(save, vm, entry.get());
                    saved = true;
                    co_return true;
                }
                if (!failed) {
                    try {
                        co_await perform(pending, result);
                    } catch (const nxtrt::operation_cancelled &) {
                        throw;
                    } catch (const effect_error & error) {
                        failed = true;
                        result.set(failure(
                            pending.get(), error.code, error.what()));
                    } catch (const nxtrt::runtime_error & error) {
                        failed = true;
                        result.set(
                            failure(pending.get(), "IO", error.what()));
#ifdef NXT_HAVE_CPPTRACE
                    } catch (const std::runtime_error & error) {
                        // Host helpers may throw standard exceptions even
                        // when NXT uses cpptrace's exception hierarchy.
                        failed = true;
                        result.set(
                            failure(pending.get(), "IO", error.what()));
#endif
                    }
                }
                nxtrt::throw_if_stop_requested();
                const auto callback =
                    vector(h, pending.get(), 5)[failed ? 4 : 3];
                h.v32set(
                    state.get(), 3, vm.start(call(callback, result.get())));
                h.v32set(state.get(), 4, nil);
                collect_if_needed();
                continue;
            }
            co_return true;
        }
    }

    // Return false only after saving at a newly issued timer.
    nxtrt::task<bool>
    run(bool enable_effects,
        const std::string & destination,
        bool cancel = false,
        bool print_results = false)
    {
        effects = enable_effects;
        save = destination;
        echo = print_results;
        saved = false;
        (void) get(0);
        while (true) {
            const bool active = get(3) != nil || get(4) != nil;
            if (!co_await execute(entry, cancel && get(4) != nil))
                throw std::runtime_error(
                    location() + ": while evaluating top-level form: "
                    + print(h, h.get<tag::run, field::err>(get(3))));
            cancel = false;
            if (saved)
                co_return false;
            if (echo && active) {
                console_guard guard{*this, 1};
                co_await guard.acquire();
                co_await write(
                    output, print(h, get(5), vm.current_package()) + "\n");
            }
            const auto offset = get(2);
            const auto info = vector(h, get(1), 3);
            const auto program = text(h, info[0]);
            const auto path = text(h, info[1]);
            require(
                tag_of(offset) == tag::integer && integer(offset) >= 0
                    && std::size_t(integer(offset)) <= program.size(),
                "invalid source position");
            reader source{
                h, vm, program, path, std::size_t(integer(offset))};
            const auto form = source.next();
            set(2, fixnum(source.position()));
            if (!form)
                co_return true;
            h.v32set(get(1), 2, fixnum(source.form_position()));
            const auto thunk = h.make<tag::fun>({nil, nil, *form, nil, 0});
            set(3, start(thunk, entry.get()));
            collect_if_needed();
        }
    }

    nxtrt::task<void> repl()
    {
        while (true) {
            if (::isatty(STDIN_FILENO))
                co_await write(output, "wisp> ");
            auto value = co_await line();
            if (!value)
                co_return;
            source(*value);
            std::string error;
            try {
                (void) co_await run(true, {}, false, true);
            } catch (const nxtrt::operation_cancelled &) {
                throw;
#ifdef NXT_HAVE_CPPTRACE
            } catch (const nxtrt::runtime_error & exception) {
                error = exception.what();
#endif
            } catch (const std::runtime_error & exception) {
                error = exception.what();
            }
            if (!error.empty())
                co_await write(errors, error + "\n");
        }
    }
};

nxtrt::task<bool>
execute(host & app, std::string source, std::string path, std::string save)
{
    app.source(source, path);
    co_return co_await app.run(true, save);
}

} // namespace
} // namespace wisp

int main(int argc, char ** argv)
{
    using namespace wisp;
    std::signal(SIGPIPE, SIG_IGN);
    try {
        const std::string command = argc > 1 ? argv[1] : "repl";
        if (command == "--help" || command == "help") {
            std::cout
                << "wisp run SOURCE [--checkpoint TAPE] [--dir NAME=PATH]...\n"
                   "wisp restore TAPE [--effects] [--cancel] [--checkpoint TAPE] [--dir NAME=PATH]...\n"
                   "wisp inspect TAPE\nwisp repl [--dir NAME=PATH]... [--run NAME[=PATH]]...\n"
                   "Checkpoints stop at the next timer. Restores default to effects disabled.\n"
                   "--dir grants read access beneath PATH as guest paths NAME/...;\n"
                   "--dir PATH grants a plain directory name as itself.\n"
                   "--run NAME grants run-command NAME (found on PATH now);\n"
                   "--run NAME=PATH grants a specific executable.\n"
                   "run and restore accept --run like --dir.\n";
            return 0;
        }
        require(
            command == "repl" || argc >= 3,
            "expected run SOURCE, restore TAPE, inspect TAPE, or repl");
        require(
            command == "run" || command == "restore"
                || command == "inspect" || command == "repl",
            "unknown command");
        bool effects = false, cancel = false;
        std::string save;
        directories granted;
        programs runnable;
        for (int i = command == "repl" ? 2 : 3; i < argc; ++i) {
            const std::string_view option{argv[i]};
            if (option == "--dir" && command != "inspect" && i + 1 < argc)
                grant(granted, argv[++i]);
            else if (
                option == "--run" && command != "inspect" && i + 1 < argc)
                grant_program(runnable, argv[++i]);
            else if (command == "repl")
                throw std::runtime_error("invalid command option");
            else if (option == "--effects" && command == "restore")
                effects = true;
            else if (option == "--cancel" && command == "restore")
                cancel = true;
            else if (
                option == "--checkpoint" && command != "inspect"
                && i + 1 < argc)
                save = argv[++i];
            else
                throw std::runtime_error("invalid command option");
        }
        if (command == "repl") {
            auto image = tape::decode(std::as_bytes(std::span{boot_tape}));
            host app{image->storage, image->machine, image->entry};
            app.granted = std::move(granted);
            app.runnable = std::move(runnable);
            nxtrt::runtime runtime;
            runtime.run([&] { return app.repl(); });
        } else if (command == "run") {
            auto image = tape::decode(std::as_bytes(std::span{boot_tape}));
            host app{image->storage, image->machine, image->entry};
            app.granted = std::move(granted);
            app.runnable = std::move(runnable);
            nxtrt::runtime runtime;
            const auto source = read_file(argv[2]);
            const bool done = runtime.run(
                [&] { return execute(app, source, argv[2], save); });
            require(
                !done || save.empty(),
                "program completed without a timer; no checkpoint written");
        } else {
            std::ifstream file(argv[2], std::ios::binary);
            require(file.is_open(), "cannot open tape");
            auto image = tape::read(file);
            host app{image->storage, image->machine, image->entry};
            app.granted = std::move(granted);
            app.runnable = std::move(runnable);
            if (command == "inspect") {
                std::cout << "source-location: " << app.location()
                          << "\nsource-byte-offset: "
                          << print(image->storage, app.get(2)) << "\n";
                const auto pending = app.get(4);
                if (pending == nil) {
                    std::cout << "pending: NIL\n";
                } else {
                    const auto xs = vector(image->storage, pending, 5);
                    std::cout
                        << "request-id: " << print(image->storage, xs[0])
                        << "\nrequest: " << print(image->storage, xs[1])
                        << "\ndeadline-unix-ms: "
                        << print(image->storage, xs[2]) << "\n";
                }
            } else {
                nxtrt::runtime runtime;
                const bool done = runtime.run(
                    [&] { return app.run(effects, save, cancel); });
                require(
                    !done || save.empty(),
                    "program completed without a timer; no checkpoint written");
            }
        }
        return 0;
    } catch (const std::exception & error) {
        std::cerr << "wisp: " << error.what() << '\n';
        return 1;
    }
}
