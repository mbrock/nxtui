// SPDX-License-Identifier: AGPL-3.0-or-later
#include "wisp/reader.hpp"
#include "wisp/nxt.hpp"
#include "wisp/printer.hpp"
#include "wisp/tape.hpp"
#include "nxtrt/app.hpp"
#include "nxtrt/bell.hpp"
#include "nxtrt/buffers.hpp"
#include "nxtrt/http-server.hpp"
#include "nxtrt/net.hpp"

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

constexpr unsigned char boot_tape[] = {
#embed "wisp-boot.tape"
};

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
    // Each slot has one long-lived NXT task and one bell waiter. Finished
    // deeds are not recycled by a firm, so never fork per guest job into
    // this session's lifetime scope.
    static constexpr std::size_t capacity = 64;
    enum class phase { idle, running, timer, frozen, joining, io };

    struct worker
    {
        root job;
        nxtrt::bell wake;
        phase state = phase::idle;
        bool cancel = false;
        bool decline = false;
        nxtrt::firm * scope = nullptr;

        explicit worker(heap & h)
            : job(h)
        {
        }
    };

    heap & h;
    evaluator & vm;
    root & entry;
    nxtrt::fd_source input{STDIN_FILENO, 1};
    nxtrt::fd_sink output{STDOUT_FILENO};
    nxtrt::fd_sink errors{STDERR_FILENO};
    std::array<std::unique_ptr<worker>, capacity> workers;
    std::vector<nxtrt::bell *> observers;
    nxtrt::bell changed;
    std::array<worker *, 3> console{};
    std::exception_ptr fatal;
    bool effects = false, echo = false, freezing = false;
    std::string save;
    std::size_t gc_threshold = 1024 * 1024;
    steady_clock::time_point freeze_deadline;

    host(heap & h, evaluator & vm, root & entry)
        : h(h)
        , vm(vm)
        , entry(entry)
    {
        for (auto & slot : workers)
            slot = std::make_unique<worker>(h);
    }

    void notify()
    {
        changed.ring();
        for (auto & slot : workers)
            slot->wake.ring();
        for (auto * observer : observers)
            observer->ring();
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
        // Collect at committed host boundaries, proportional to allocation,
        // not on every effect. Guest GC requests still collect in drive().
        if (heap_bytes() >= gc_threshold) {
            vm.collect();
            gc_threshold = heap_bytes() * 2 + 1024 * 1024;
        }
    }

    word get(const worker & slot, std::size_t field) const
    {
        return vector(
            h, slot.job.get(), &slot == workers[0].get() ? 8 : 7)[field];
    }

    void set(worker & slot, std::size_t field, word value)
    {
        h.v32set(slot.job.get(), field, value);
    }

    word get(std::size_t field) const
    {
        const auto xs = vector(h, entry.get(), 8);
        require(
            xs[0] == vm.keyword("NXT-WISP-2"), "unsupported host image");
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
            effect_require(
                value.size() < tape::default_limit,
                "input line exceeds 64 MiB");
            value += char(std::to_integer<unsigned char>(*byte));
        }
        if (value.empty())
            co_return std::nullopt;
        co_return value;
    }

    void source(std::string_view value)
    {
        require(
            value.size() <= tape::default_limit, "source exceeds 64 MiB");
        entry.set(h.newv32(
            std::array{
                vm.keyword("NXT-WISP-2"),
                h.newv08(value),
                word{0},
                nil,
                nil,
                nil,
                h.filledv32(capacity, nil),
                h.newv08("0")}));
    }

    void identify(root & pending)
    {
        if (vector(h, pending.get(), 5)[0] != nil)
            return;
        const auto counter = text(h, get(7));
        std::uint64_t serial = 0;
        const auto [end, error] = std::from_chars(
            counter.data(), counter.data() + counter.size(), serial);
        require(
            error == std::errc{} && end == counter.data() + counter.size()
                && serial != std::numeric_limits<std::uint64_t>::max(),
            "invalid or exhausted host request counter");
        const auto id = h.newv08(std::to_string(serial + 1));
        set(7, id);
        h.v32set(pending.get(), 0, id);
    }

    word start(word thunk, word job)
    {
        return vm.start(h.cons(
            vm.intern("%NXT-START"),
            h.cons(quote(thunk), h.cons(quote(job), nil))));
    }

    word spawn(word thunk)
    {
        effect_require(
            tag_of(thunk) == tag::fun, "spawn expected a function");
        for (std::size_t i = 1; i < capacity; ++i) {
            auto & slot = *workers[i];
            if (slot.job.get() != nil
                || vector(h, get(6), capacity)[i] != nil)
                continue;
            const auto job = h.newv32(
                std::array{
                    vm.keyword("NXT-JOB"),
                    nil,
                    fixnum(i),
                    nil,
                    nil,
                    nil,
                    fixnum(0)});
            slot.job.set(job);
            slot.cancel = false;
            slot.state = phase::running;
            h.v32set(get(6), i, job);
            set(slot, 3, start(thunk, job));
            notify();
            return job;
        }
        throw effect_error{"CAPACITY", "all Wisp job slots are occupied"};
    }

    int job_status(word job)
    {
        effect_require(
            tag_of(job) == tag::v32 && h.v32slice(job).size() == 7,
            "join expected a job");
        const auto xs = h.v32slice(job);
        effect_require(
            xs[0] == vm.keyword("NXT-JOB") && tag_of(xs[6]) == tag::integer
                && integer(xs[6]) >= 0 && integer(xs[6]) <= 3,
            "invalid job");
        return integer(xs[6]);
    }

    void observe(word job)
    {
        if (job_status(job) < 2)
            return;
        h.v32set(job, 6, fixnum(3));
        for (std::size_t i = 1; i < capacity; ++i)
            if (vector(h, get(6), capacity)[i] == job)
                h.v32set(get(6), i, nil);
        notify();
    }

    void check_join(worker & slot, word target)
    {
        std::set<word> visited{slot.job.get()};
        while (job_status(target) == 0) {
            if (!visited.insert(target).second)
                throw effect_error{"JOIN-CYCLE", "cyclic job join"};
            effect_require(
                std::ranges::any_of(
                    workers,
                    [&](auto & w) { return w->job.get() == target; }),
                "job is not live in this session");
            const auto pending = vector(h, target, 7)[4];
            if (pending == nil)
                return;
            const auto request = vector(h, vector(h, pending, 5)[1], 2);
            if (request[0] != vm.keyword("JOIN"))
                return;
            target = request[1];
        }
    }

    nxtrt::task<void> acquire(worker & slot, int stream)
    {
        while (true) {
            slot.wake.reset();
            if (!console[stream]) {
                console[stream] = &slot;
                co_return;
            }
            co_await slot.wake;
        }
    }

    struct console_guard
    {
        host & app;
        worker & slot;
        int stream;

        ~console_guard()
        {
            if (app.console[stream] == &slot) {
                app.console[stream] = nullptr;
                app.notify();
            }
        }
    };

    // An HTTP callback has its own single-waiter bell. Destruction cancels
    // only its guest job, including any outstanding I/O, and leaves the
    // reusable worker alive. No guest word is kept in a native registry.
    struct request_waiter
    {
        host & app;
        root & job;
        nxtrt::bell wake;

        request_waiter(host & app, root & job)
            : app(app)
            , job(job)
        {
            app.observers.push_back(&wake);
        }

        ~request_waiter()
        {
            std::erase(app.observers, &wake);
            if (job.get() == nil)
                return;
            app.observe(job.get());
            if (app.job_status(job.get()) != 0)
                return;
            for (auto & slot : app.workers) {
                if (slot->job.get() == job.get()) {
                    slot->cancel = true;
                    if (slot->scope)
                        slot->scope->stop();
                }
            }
        }
    };

    nxtrt::task<nxtrt::http::response>
    http_request(root & pending, nxtrt::http::request request)
    {
        // Copy the native request into heap data before scheduling it.
        // No views or unrooted words survive the first suspension.
        std::vector<word> headers;
        for (const auto & header : request.headers)
            headers.push_back(h.newv32(
                std::array{h.newv08(header.name), h.newv08(header.value)}));
        const auto query = request.target.find('?');
        const auto value = h.newv32(
            std::array{
                h.newv08(request.method),
                h.newv08(request.target.substr(0, query)),
                h.newv08(
                    query == std::string::npos
                        ? ""
                        : request.target.substr(query + 1)),
                h.newv32(headers),
                h.newv08(request.body)});
        const auto handler = vector(
            h, vector(h, vector(h, pending.get(), 5)[1], 2)[1], 2)[1];
        const auto expression = h.cons(
            vm.intern("%NXT-HTTP-HANDLE"),
            h.cons(quote(handler), h.cons(quote(value), nil)));
        const auto thunk = h.make<tag::fun>({nil, nil, expression, nil, 0});
        root job{h};
        // Establish notification/cancellation ownership before admission;
        // descriptor or allocation failure must not leave an orphan job.
        request_waiter waiter{*this, job};
        try {
            job.set(spawn(thunk));
        } catch (const effect_error & error) {
            if (error.code != "CAPACITY")
                throw;
            co_return nxtrt::http::response{
                503, {}, "Service Unavailable\n"};
        }
        while (true) {
            // A bell reset and the condition check form one deck turn.
            // The registry wakes all interested callbacks at completion.
            waiter.wake.reset();
            if (job_status(job.get()) != 0)
                break;
            co_await waiter.wake;
        }
        if (job_status(job.get()) >= 2)
            co_return nxtrt::http::response{
                500, {}, "Internal Server Error\n"};
        const auto result = vector(h, vector(h, job.get(), 7)[5], 3);
        effect_require(
            tag_of(result[0]) == tag::integer, "invalid HTTP status");
        nxtrt::http::response response;
        response.status = integer(result[0]);
        if (result[2] != nil) {
            effect_require(
                tag_of(result[2]) == tag::v08,
                "HTTP body must be a string");
            effect_require(
                h.v08slice(result[2]).size()
                    <= nxtrt::http::server_options{}
                           .max_response_body_bytes,
                "HTTP body too large");
            response.body = text(h, result[2]);
        }
        std::set<word> seen;
        std::size_t header_bytes = 0;
        for (auto list = result[1]; list != nil;) {
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
        co_await nxtrt::http::serve(
            listener.get(), [&](nxtrt::http::request request) {
                return http_request(pending, std::move(request));
            });
    }

    // Native registrations are temporary. The guest record remains rooted
    // throughout the await and is removed only when a resume/raise run has
    // been installed. Results also need roots: another worker can collect
    // between this task returning and its caller resuming.
    nxtrt::task<void> perform(worker & slot, root & pending, root & result)
    {
        const auto record = vector(h, pending.get(), 5);
        const auto request = vector(h, record[1], 2);
        const auto operation = request[0], argument = request[1];
        if (operation == vm.keyword("TIMER")) {
            const auto deadline = deadline_ms(h, record[2]);
            // Recheck wall time in bounded monotonic waits. Already elapsed
            // timers fire immediately after restore, never restart a delay.
            while (true) {
                slot.wake.reset();
                if (slot.cancel)
                    co_return;
                if (freezing) {
                    slot.state = phase::frozen;
                    changed.ring();
                    co_await slot.wake;
                    continue;
                }
                slot.state = phase::timer;
                const auto remaining = deadline - now_ms();
                if (remaining <= 0)
                    break;
                co_await nxtrt::op::timeout::after(
                    milliseconds{std::min<std::int64_t>(remaining, 1000)});
            }
            co_return;
        }
        if (operation == vm.keyword("SPAWN")) {
            result.set(spawn(argument));
            co_return;
        }
        if (operation == vm.keyword("JOIN")) {
            root target{h, argument};
            while (true) {
                slot.wake.reset();
                check_join(slot, target.get());
                const auto status = job_status(target.get());
                if (status) {
                    observe(target.get());
                    result.set(vector(h, target.get(), 7)[5]);
                    if (status >= 2)
                        throw effect_error{
                            "JOB-FAILED", print(h, result.get())};
                    co_return;
                }
                if (slot.cancel)
                    co_return;
                slot.state = phase::joining;
                changed.ring();
                co_await slot.wake;
            }
        }
        slot.state = phase::io;
        if (operation == vm.keyword("HTTP-SERVE")) {
            co_await http_serve(pending);
            co_return;
        }
        if (operation == vm.keyword("REQUEST-HEADER")) {
            const auto args = vector(h, argument, 2);
            const auto request = vector(h, args[0], 5);
            const auto name = text(h, args[1]);
            effect_require(
                tag_of(request[3]) == tag::v32, "invalid HTTP headers");
            for (auto pair : h.v32slice(request[3])) {
                const auto header = vector(h, pair, 2);
                if (nxtrt::http::iequals(name, text(h, header[0]))) {
                    result.set(header[1]);
                    break;
                }
            }
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
            const console_guard guard{*this, slot, stream};
            co_await acquire(slot, stream);
            co_await write(stream == 1 ? output : errors, std::move(bytes));
            co_return;
        }
        if (operation == vm.keyword("READ-LINE")) {
            const console_guard guard{*this, slot, 0};
            co_await acquire(slot, 0);
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
            const console_guard guard{*this, slot, 0};
            co_await acquire(slot, 0);
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
        return h.newv32(
            std::array{
                vm.intern("HOST-ERROR"),
                operation,
                vm.keyword(code),
                h.newv08(message)});
    }

    void finish(worker & slot, word result, int status)
    {
        set(slot, 3, nil);
        set(slot, 4, nil);
        set(slot, 5, result);
        if (&slot != workers[0].get()) {
            set(slot, 6, fixnum(status));
            if (status != 2) {
                for (std::size_t i = 1; i < capacity; ++i)
                    if (vector(h, get(6), capacity)[i] == slot.job.get())
                        h.v32set(get(6), i, nil);
            }
        }
        notify();
    }

    nxtrt::task<void> execute_job(worker & slot)
    {
        while (true) {
            slot.state = phase::running;
            root run{h, get(slot, 3)};
            if (run.get() != nil) {
                require(tag_of(run.get()) == tag::run, "invalid host run");
                if (co_await drive(vm, run) == evaluation::failed) {
                    const auto error =
                        h.get<tag::run, field::err>(run.get());
                    if (&slot == workers[0].get())
                        throw std::runtime_error(print(h, error));
                    finish(slot, error, 2);
                    co_return;
                }
                set(slot, 5, h.get<tag::run, field::val>(run.get()));
                set(slot, 3, nil);
            }
            root pending{h, get(slot, 4)};
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
                    if (slot.decline) {
                        slot.decline = false;
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
                    if (!freezing) {
                        freezing = true;
                        freeze_deadline = steady_clock::now() + seconds{5};
                        notify();
                    }
                }
                if (!failed) {
                    try {
                        co_await perform(slot, pending, result);
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
                const auto callback =
                    vector(h, pending.get(), 5)[failed ? 4 : 3];
                set(slot, 3, vm.start(call(callback, result.get())));
                set(slot, 4, nil);
                collect_if_needed();
                continue;
            }
            if (&slot != workers[0].get()) {
                finish(slot, get(slot, 5), 1);
                co_return;
            }
            if (echo && run.get() != nil) {
                auto bytes = print(h, get(5), vm.current_package()) + "\n";
                slot.state = phase::io;
                const console_guard guard{*this, slot, 1};
                co_await acquire(slot, 1);
                co_await write(output, std::move(bytes));
            }
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
                co_return;
            const auto thunk = h.make<tag::fun>({nil, nil, *form, nil, 0});
            set(3, start(thunk, entry.get()));
            collect_if_needed();
        }
    }

    nxtrt::task<void> work_item(worker & slot)
    {
        if (slot.cancel || nxtrt::current_firm()->stop_requested())
            co_return;
        slot.scope = nxtrt::current_firm();

        struct clear_scope
        {
            worker & slot;

            ~clear_scope()
            {
                slot.scope = nullptr;
            }
        } guard{slot};

        nxtrt::fork(execute_job(slot));
        co_await nxtrt::join();
    }

    nxtrt::task<void> work(worker & slot)
    {
        try {
            while (true) {
                slot.wake.reset();
                if (slot.job.get() == nil) {
                    co_await slot.wake;
                    continue;
                }
                co_await nxtrt::with_firm([&] { return work_item(slot); });
                if (slot.cancel)
                    finish(slot, h.newv08("request cancelled"), 3);
                slot.job.set(nil);
                slot.state = phase::idle;
                collect_if_needed();
                notify();
            }
        } catch (const nxtrt::operation_cancelled &) {
            throw;
        } catch (...) {
            if (!fatal)
                fatal = std::current_exception();
            changed.ring();
        }
    }

    nxtrt::task<void> wait_changed()
    {
        co_await changed;
    }

    nxtrt::task<bool> session()
    {
        auto & scope = *nxtrt::current_firm();
        if (scope.stop_requested())
            throw nxtrt::operation_cancelled{};
        for (auto & slot : workers)
            scope.fork(work(*slot));
        while (true) {
            changed.reset();
            if (fatal)
                std::rethrow_exception(fatal);
            const bool idle = std::ranges::all_of(workers, [](auto & slot) {
                return slot->state == phase::idle;
            });
            if (idle) {
                for (auto job : vector(h, get(6), capacity))
                    if (job != nil && job_status(job) == 2)
                        throw std::runtime_error(
                            "unjoined Wisp job failed: "
                            + print(h, vector(h, job, 7)[5]));
                scope.stop();
                co_await scope.join();
                co_return true;
            }
            if (freezing) {
                const bool quiescent =
                    std::ranges::all_of(workers, [](auto & slot) {
                        return slot->state == phase::idle
                               || slot->state == phase::frozen
                               || slot->state == phase::joining;
                    });
                if (quiescent) {
                    vm.collect();
                    checkpoint(save, vm, entry.get());
                    scope.stop();
                    co_await scope.join();
                    co_return false;
                }
                const auto remaining =
                    freeze_deadline - steady_clock::now();
                require(
                    remaining > steady_clock::duration::zero(),
                    "checkpoint cannot quiesce live I/O or computation");
                try {
                    co_await nxtrt::with_timeout(remaining, wait_changed());
                } catch (const nxtrt::timeout_error &) {
                    throw std::runtime_error(
                        "checkpoint cannot quiesce live I/O or computation");
                }
            } else {
                co_await changed;
            }
        }
    }

    // Return false only after a successful quiescent save-and-stop.
    nxtrt::task<bool>
    run(bool enable_effects,
        const std::string & destination,
        bool cancel = false,
        bool print_results = false)
    {
        effects = enable_effects;
        save = destination;
        echo = print_results;
        freezing = false;
        fatal = nullptr;
        (void) vector(h, get(6), capacity);
        for (std::size_t i = 0; i < capacity; ++i) {
            auto & slot = *workers[i];
            const auto job =
                i == 0 ? entry.get() : vector(h, get(6), capacity)[i];
            const bool live =
                job != nil && (i == 0 || job_status(job) == 0);
            slot.job.set(live ? job : nil);
            slot.state = live ? phase::running : phase::idle;
            slot.cancel = false;
            slot.decline = cancel && live && get(slot, 4) != nil;
        }
        co_return co_await nxtrt::with_firm([&] { return session(); });
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

nxtrt::task<bool> execute(host & app, std::string source, std::string save)
{
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
            auto image = tape::decode(std::as_bytes(std::span{boot_tape}));
            host app{image->storage, image->machine, image->entry};
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
            auto image = tape::decode(std::as_bytes(std::span{boot_tape}));
            host app{image->storage, image->machine, image->entry};
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
                std::cout << "source-byte-offset: "
                          << print(image->storage, app.get(2)) << "\n";
                std::vector<word> jobs{image->entry.get()};
                for (auto job :
                     vector(image->storage, app.get(6), host::capacity))
                    if (job != nil)
                        jobs.push_back(job);
                for (std::size_t i = 0; i < jobs.size(); ++i) {
                    const auto pending =
                        vector(image->storage, jobs[i], i == 0 ? 8 : 7)[4];
                    std::cout << "job: " << i << "\n";
                    if (pending == nil) {
                        std::cout << "pending: NIL\n";
                        continue;
                    }
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
