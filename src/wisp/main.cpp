// SPDX-License-Identifier: AGPL-3.0-or-later
#include "wisp/reader.hpp"
#include "wisp/printer.hpp"
#include "wisp/tape.hpp"
#include "nxtrt/app.hpp"
#include "nxtrt/bell.hpp"
#include "nxtrt/buffers.hpp"
#include "nxtrt/http-server.hpp"
#include "nxtrt/net.hpp"
#include "nxtrt/net_dns.hpp"
#include "nxtrt/tls.hpp"

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
        // Collect at committed host boundaries, proportional to allocation,
        // not on every effect. Guest GC requests collect during evaluation.
        if (heap_bytes() >= gc_threshold) {
            vm.collect();
            gc_threshold = heap_bytes() * 2 + 1024 * 1024;
        }
    }

    word get(std::size_t field) const
    {
        const auto xs = vector(h, entry.get(), 7);
        require(
            xs[0] == vm.keyword("NXT-WISP-3"), "unsupported host image");
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
                vm.keyword("NXT-WISP-3"),
                h.newv08(value),
                word{0},
                nil,
                nil,
                nil,
                h.newv08("0")}));
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
        root state{h, h.filledv32(7, nil)};
        h.v32set(state.get(), 3, start(thunk, state.get()));
        // This callback directly owns its activation and native awaits. The
        // server's deadline/stop follows this task, with no guest job
        // registry.
        if (!co_await execute(state))
            co_return nxtrt::http::response{
                500, {}, "Internal Server Error\n"};
        const auto result = vector(h, vector(h, state.get(), 7)[5], 3);
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
        result.set(h.newv32(
            std::array{fixnum(head.status), headers, h.newv08(body)}));
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
        return h.newv32(
            std::array{
                vm.intern("HOST-ERROR"),
                operation,
                vm.keyword(code),
                h.newv08(message)});
    }

    nxtrt::task<bool> execute(root & state, bool decline = false)
    {
        while (true) {
            nxtrt::throw_if_stop_requested();
            root run{h, vector(h, state.get(), 7)[3]};
            if (run.get() != nil) {
                require(tag_of(run.get()) == tag::run, "invalid host run");
                // Run to return/effect, not to an evaluator scheduling
                // quantum. Collection commits guest transitions but does
                // not yield.
                while (true) {
                    const auto outcome = vm.advance(
                        run.get(), std::numeric_limits<std::size_t>::max());
                    if (vm.collection_requested())
                        vm.collect();
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
                    print(h, h.get<tag::run, field::err>(get(3))));
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
            const auto program = text(h, get(1));
            require(
                tag_of(offset) == tag::integer && integer(offset) >= 0
                    && std::size_t(integer(offset)) <= program.size(),
                "invalid source position");
            reader source{
                h, vm, std::string_view{program}.substr(integer(offset))};
            const auto form = source.next();
            set(2, fixnum(integer(offset) + source.position()));
            if (!form)
                co_return true;
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
