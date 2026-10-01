// SPDX-License-Identifier: AGPL-3.0-or-later
#include "wisp/load.hpp"
#include "wisp/nxt.hpp"
#include "wisp/printer.hpp"
#include "wisp/tape.hpp"
#include "nxtrt/app.hpp"
#include "nxtrt/buffers.hpp"

#include <charconv>
#include <csignal>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <set>
#include <system_error>
#include <unistd.h>

namespace wisp {
namespace {

#include "wisp-host.hpp"

using namespace std::chrono;

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
    require(
        error == std::errc{} && end == str.data() + str.size()
            && deadline >= 0,
        "invalid timer deadline");
    return deadline;
}

std::string read_file(const std::string & path)
{
    std::ifstream file(path, std::ios::binary);
    require(file.is_open(), "cannot open source file");
    std::string result;
    std::array<char, 8192> buffer;
    while (file.read(buffer.data(), buffer.size()) || file.gcount()) {
        require(
            result.size() + file.gcount() <= tape::default_limit,
            "source exceeds 64 MiB");
        result.append(buffer.data(), file.gcount());
    }
    require(file.eof() && !file.bad(), "cannot read source file");
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

struct host
{
    heap & h;
    evaluator & vm;
    root & entry;
    nxtrt::fd_source input{STDIN_FILENO, 1};
    nxtrt::fd_sink output{STDOUT_FILENO};
    nxtrt::fd_sink errors{STDERR_FILENO};

    word get(std::size_t field) const
    {
        const auto xs = vector(h, entry.get(), 6);
        require(
            xs[0] == vm.keyword("NXT-WISP-1"), "unsupported host image");
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
            require(
                value.size() < tape::default_limit,
                "input line exceeds 64 MiB");
            value += char(std::to_integer<unsigned char>(*byte));
        }
        if (value.empty())
            co_return std::nullopt;
        co_return value;
    }

    nxtrt::task<void> boot()
    {
        for (auto source : {base_library(), host_source}) {
            loader load{h, vm, source};
            while (true) {
                const auto state = load.advance(256);
                if (state == evaluation::failed)
                    throw std::runtime_error(
                        print(h, h.get<tag::run, field::err>(load.run())));
                if (state == evaluation::done)
                    break;
                co_await nxtrt::yield();
            }
            vm.collect();
        }
    }

    void source(std::string_view value)
    {
        require(
            value.size() <= tape::default_limit, "source exceeds 64 MiB");
        entry.set(h.newv32(
            std::array{
                vm.keyword("NXT-WISP-1"),
                h.newv08(value),
                word{0},
                nil,
                nil,
                nil}));
    }

    // Native registrations are temporary. The guest record remains rooted
    // throughout the await and is removed only when a resume/raise run has
    // been installed. Cancellation of this task does not consume it.
    nxtrt::task<word> perform(root & pending)
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
            co_return nil;
        }
        if (operation == vm.keyword("STDOUT")
            || operation == vm.keyword("STDERR")) {
            std::string bytes;
            std::set<word> seen;
            for (auto cur = argument; cur != nil;) {
                require(
                    tag_of(cur) == tag::duo && seen.insert(cur).second,
                    "invalid write arguments");
                const auto [string, next] = h.read<tag::duo>(cur);
                bytes += text(h, string);
                cur = next;
            }
            co_await write(
                operation == vm.keyword("STDOUT") ? output : errors,
                std::move(bytes));
            co_return nil;
        }
        if (operation == vm.keyword("READ-LINE")) {
            const auto value = co_await line();
            co_return value ? h.newv08(*value) : nil;
        }
        if (operation == vm.keyword("READ-BYTES")) {
            require(
                tag_of(argument) == tag::integer && integer(argument) >= 0
                    && integer(argument)
                           <= std::int64_t(tape::default_limit),
                "invalid byte count");
            const auto count = integer(argument);
            std::string bytes;
            for (int i = 0; i < count; ++i) {
                const auto byte = co_await input.take();
                if (!byte)
                    break;
                bytes += char(std::to_integer<unsigned char>(*byte));
            }
            co_return h.newv08(bytes);
        }
        throw std::runtime_error("unsupported host operation");
    }

    // Return false only after a successful save-and-stop at a timer.
    nxtrt::task<bool>
    run(bool effects,
        const std::string & save,
        bool cancel = false,
        bool echo = false)
    {
        while (true) {
            root run{h, get(3)};
            if (run.get() != nil) {
                require(tag_of(run.get()) == tag::run, "invalid host run");
                if (co_await drive(vm, run) == evaluation::failed)
                    throw std::runtime_error(
                        print(h, h.get<tag::run, field::err>(run.get())));
                set(5, h.get<tag::run, field::val>(run.get()));
                set(3, nil);
            }
            root pending{h, get(4)};
            if (pending.get() != nil) {
                auto record = vector(h, pending.get(), 5);
                auto request = vector(h, record[1], 2);
                require(
                    tag_of(record[0]) == tag::sym
                        && tag_of(record[3]) == tag::fun
                        && tag_of(record[4]) == tag::fun,
                    "invalid pending request");
                require(
                    effects,
                    "effects disabled; inspect the tape or restore with --effects");
                root result{h};
                bool failed = false;
                try {
                    if (cancel) {
                        cancel = false;
                        throw std::runtime_error("CANCELLED");
                    }
                    if (request[0] == vm.keyword("TIMER")
                        && record[2] == nil) {
                        require(
                            tag_of(request[1]) == tag::integer
                                && integer(request[1]) >= 0,
                            "invalid timer delay");
                        const auto deadline = h.newv08(
                            std::to_string(now_ms() + integer(request[1])));
                        h.v32set(pending.get(), 2, deadline);
                    }
                } catch (const std::runtime_error & error) {
                    failed = true;
                    result.set(h.newv08(error.what()));
                }
                if (!failed && !save.empty()
                    && vector(h, vector(h, pending.get(), 5)[1], 2)[0]
                           == vm.keyword("TIMER")) {
                    vm.collect();
                    checkpoint(save, vm, entry.get());
                    co_return false;
                }
                if (!failed) {
                    try {
                        result.set(co_await perform(pending));
                    } catch (const nxtrt::operation_cancelled &) {
                        throw;
                    } catch (const std::runtime_error & error) {
                        failed = true;
                        result.set(h.newv08(error.what()));
                    }
                }
                const auto callback =
                    vector(h, pending.get(), 5)[failed ? 4 : 3];
                set(3, vm.start(call(callback, result.get())));
                set(4, nil);
                vm.collect();
                continue;
            }
            if (echo && run.get() != nil)
                co_await write(
                    output, print(h, get(5), vm.current_package()) + "\n");
            const auto offset = get(2);
            const auto program = text(h, get(1));
            require(
                tag_of(offset) == tag::integer && integer(offset) >= 0
                    && std::size_t(integer(offset)) <= program.size(),
                "invalid source position");
            reader input{
                h, vm, std::string_view{program}.substr(integer(offset))};
            const auto form = input.next();
            set(2, fixnum(integer(offset) + input.position()));
            if (!form)
                co_return true;
            const auto thunk = h.make<tag::fun>({nil, nil, *form, nil, 0});
            const auto start = h.cons(
                vm.intern("%NXT-START"),
                h.cons(quote(thunk), h.cons(quote(entry.get()), nil)));
            set(3, vm.start(start));
            vm.collect();
        }
    }

    nxtrt::task<void> repl()
    {
        co_await boot();
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
            } catch (const std::runtime_error & exception) {
                error = exception.what();
            }
            if (!error.empty())
                co_await write(errors, error + "\n");
        }
    }
};

nxtrt::task<bool> execute(host & app, std::string source, std::string save)
{
    co_await app.boot();
    app.source(source);
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
                << "wisp run SOURCE [--checkpoint TAPE]\n"
                   "wisp restore TAPE [--effects] [--cancel] [--checkpoint TAPE]\n"
                   "wisp inspect TAPE\nwisp repl\n"
                   "Checkpoints stop at the next timer. Restores default to effects disabled.\n";
            return 0;
        }
        if (command == "repl") {
            require(argc <= 2, "repl takes no arguments");
            heap h;
            evaluator vm{h};
            root entry{h};
            host app{h, vm, entry};
            nxtrt::runtime runtime;
            runtime.run([&] { return app.repl(); });
            return 0;
        }
        require(
            argc >= 3,
            "expected run SOURCE, restore TAPE, inspect TAPE, or repl");
        require(
            command == "run" || command == "restore"
                || command == "inspect",
            "unknown command");
        bool effects = false, cancel = false;
        std::string save;
        for (int i = 3; i < argc; ++i) {
            const std::string_view option{argv[i]};
            if (option == "--effects" && command == "restore")
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
        if (command == "run") {
            heap h;
            evaluator vm{h};
            root entry{h};
            host app{h, vm, entry};
            nxtrt::runtime runtime;
            const auto source = read_file(argv[2]);
            const bool done =
                runtime.run([&] { return execute(app, source, save); });
            require(
                !done || save.empty(),
                "program completed without a timer; no checkpoint written");
        } else {
            std::ifstream file(argv[2], std::ios::binary);
            require(file.is_open(), "cannot open tape");
            auto image = tape::read(file);
            host app{image->storage, image->machine, image->entry};
            if (command == "inspect") {
                const auto pending = app.get(4);
                std::cout << "source-byte-offset: "
                          << print(image->storage, app.get(2)) << "\n";
                if (pending == nil)
                    std::cout << "pending: NIL\n";
                else {
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
