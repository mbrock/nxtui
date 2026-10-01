#include "nxtrt/http-server.hpp"

#include "nxtrt/bell.hpp"
#include "nxtrt/net.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/serializer.hpp>
#include <boost/beast/http/string_body.hpp>

#include <array>
#include <memory>

namespace nxtrt::http {
namespace {

namespace bh = boost::beast::http;
using parser = bh::request_parser<bh::string_body>;
using message = bh::response<bh::string_body>;

struct rejection
{
    int status;
};

void check_parse_error(boost::system::error_code ec)
{
    if (!ec || ec == bh::error::need_more)
        return;
    if (ec == bh::error::header_limit)
        throw rejection{431};
    if (ec == bh::error::body_limit)
        throw rejection{413};
    throw rejection{400};
}

// Keep unconsumed bytes: put() can consume a prefix even on need_more.
// One bounded contiguous buffer also avoids Beast's multi-buffer
// flattening.
task<bool>
append_input(bytefeed & input, std::string & pending, std::size_t limit)
{
    if (pending.size() >= limit)
        throw rejection{431};
    auto bytes = co_await input.take_some(
        std::min(std::size_t{4096}, limit - pending.size()));
    if (!bytes)
        co_return false;
    pending.append(as_string_view(*bytes));
    co_return true;
}

task<bool> read_head(
    bytefeed & input,
    std::string & pending,
    parser & parsed,
    const server_options & options)
{
    auto end = pending.find("\r\n\r\n");
    while (end == std::string::npos) {
        if (!co_await append_input(
                input, pending, options.max_header_bytes)) {
            if (pending.empty())
                co_return false;
            throw rejection{400};
        }
        end = pending.find("\r\n\r\n");
    }
    auto size = end + 4;
    if (size > options.max_header_bytes)
        throw rejection{431};
    auto head = std::string_view{pending}.substr(0, size);
    // Beast accepts obsolete line folding for compatibility; an origin
    // behind a proxy must not reinterpret such fields differently from the
    // proxy.
    if (head.find("\r\n ") != head.npos || head.find("\r\n\t") != head.npos)
        throw rejection{400};
    auto ec = boost::system::error_code{};
    auto consumed = parsed.put(boost::asio::buffer(head), ec);
    pending.erase(0, consumed);
    check_parse_error(ec);
    if (!parsed.is_header_done())
        throw rejection{400};
    co_return true;
}

request checked_request(parser & parsed)
{
    auto & msg = parsed.get();
    if (msg.version() != 11)
        throw rejection{505};
    if (msg.method() == bh::verb::connect)
        throw rejection{501};
    if (parsed.upgrade() || msg.count(bh::field::upgrade))
        throw rejection{400};
    auto target =
        std::string_view{msg.target().data(), msg.target().size()};
    if (target.empty()
        || (target.front() != '/'
            && !(msg.method() == bh::verb::options && target == "*"))
        || target.find('#') != target.npos)
        throw rejection{400};
    if (msg.count(bh::field::host) != 1 || msg[bh::field::host].empty())
        throw rejection{400};
    for (auto c : msg[bh::field::host]) {
        if (static_cast<unsigned char>(c) <= 32 || c == 127 || c == ','
            || c == '/' || c == '@' || c == '\\' || c == '#' || c == '?')
            throw rejection{400};
    }
    if (msg.count(bh::field::content_length) > 1
        || msg.count(bh::field::transfer_encoding) > 1)
        throw rejection{400};
    if (msg.count(bh::field::content_length)) {
        for (auto c : msg[bh::field::content_length]) {
            if (c < '0' || c > '9')
                throw rejection{400};
        }
    }
    if (msg.count(bh::field::transfer_encoding)) {
        auto value = msg[bh::field::transfer_encoding];
        if (msg.count(bh::field::content_length)
            || !iequals({value.data(), value.size()}, "chunked"))
            throw rejection{400};
    }
    if (msg.count(bh::field::expect))
        throw rejection{417};

    auto req = request{};
    req.method = msg.method_string();
    req.target = target;
    req.host = msg[bh::field::host];
    // Snapshot before body parsing: trailer fields can never replace or
    // append application headers, even on Beast versions that merge
    // trailers.
    for (auto const & field : msg)
        req.headers.push_back(
            {std::string{field.name_string()}, std::string{field.value()}});
    return req;
}

task<void> read_body(
    bytefeed & input,
    std::string & pending,
    parser & parsed,
    const server_options & options)
{
    auto remaining = options.max_body_wire_bytes;
    parsed.eager(true);
    while (!parsed.is_done()) {
        if (remaining == 0)
            throw rejection{413};
        if (!pending.empty()) {
            auto ec = boost::system::error_code{};
            auto consumed = parsed.put(
                boost::asio::buffer(
                    pending.data(), std::min(remaining, pending.size())),
                ec);
            remaining -= consumed;
            pending.erase(0, consumed);
            check_parse_error(ec);
            if (parsed.is_done())
                break;
            if (!ec && consumed)
                continue;
        }
        if (pending.size() >= remaining)
            throw rejection{413};
        if (!co_await append_input(
                input,
                pending,
                std::min(remaining, options.max_header_bytes)))
            throw rejection{400};
    }
}

bool token_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
           || (c >= '0' && c <= '9')
           || std::string_view{"!#$%&'*+-.^_`|~"}.find(c)
                  != std::string_view::npos;
}

bool managed_header(std::string_view name)
{
    return iequals(name, "Content-Length") || iequals(name, "Connection")
           || iequals(name, "Transfer-Encoding") || iequals(name, "Trailer")
           || iequals(name, "Upgrade") || iequals(name, "Keep-Alive")
           || iequals(name, "Proxy-Connection");
}

message make_response(
    response res,
    bool head,
    bool keep_alive,
    const server_options & options)
{
    if (res.status < 200 || res.status > 599
        || res.body.size() > options.max_response_body_bytes)
        throw rejection{500};
    auto out = message{static_cast<bh::status>(res.status), 11};
    // Reserve room for generated status line and framing fields.
    auto remaining = options.max_response_header_bytes - 128;
    for (auto const & field : res.headers) {
        if (field.name.empty()
            || !std::ranges::all_of(field.name, token_char)
            || !std::ranges::all_of(field.value, [](unsigned char c) {
                   return c == '\t' || (c >= 32 && c != 127);
               }))
            throw rejection{500};
        if (field.name.size() > remaining)
            throw rejection{500};
        remaining -= field.name.size();
        if (remaining < 4 || field.value.size() > remaining - 4)
            throw rejection{500};
        remaining -= field.value.size() + 4;
        if (!managed_header(field.name))
            out.insert(field.name, field.value);
    }
    out.keep_alive(keep_alive);
    if (res.status == 204 || res.status == 304)
        return out;
    auto size = res.status == 205 ? 0 : res.body.size();
    out.content_length(size);
    if (!head && res.status != 205)
        out.body() = std::move(res.body);
    return out;
}

task<void> write_response(bytesink & output, message & msg)
{
    auto serializer = bh::response_serializer<bh::string_body>{msg};
    while (!serializer.is_done()) {
        auto ec = boost::system::error_code{};
        auto buffer = boost::asio::const_buffer{};
        // Non-coroutine visitor. References stay in this suspended frame;
        // the serializer/message stay alive until write_all has consumed
        // the span.
        serializer.next(ec, [&buffer](auto &, auto const & buffers) {
            for (auto it = boost::asio::buffer_sequence_begin(buffers);
                 it != boost::asio::buffer_sequence_end(buffers);
                 ++it) {
                if ((*it).size()) {
                    buffer = *it;
                    break;
                }
            }
        });
        if (ec)
            throw protocol_error{ec.message()};
        if (buffer.size()) {
            co_await write_all(
                output,
                std::string_view{
                    static_cast<const char *>(buffer.data()),
                    buffer.size()});
            serializer.consume(buffer.size());
        }
    }
}

task<response> invoke_handler(request_handler & handler, request req)
{
    co_return co_await handler(std::move(req));
}

task<void> connection(
    nxt::unique_fd fd,
    request_handler & handler,
    const server_options & options)
{
    auto rx = std::array<std::byte, 4096>{};
    auto tx = std::array<std::byte, 4096>{};
    auto flags = 0;
#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
#endif
#ifdef SO_NOSIGPIPE
    auto yes = 1;
    if (::setsockopt(fd.get(), SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes)))
        net::throw_errno("setsockopt(SO_NOSIGPIPE)");
#endif
    auto socket = net::socket{std::move(fd), tx, rx, flags};
    auto pending = std::string{};
    for (std::size_t count = 0; count < options.max_requests_per_connection;
         ++count) {
        auto out = message{};
        auto keep_alive = false;
        auto head = false;
        auto status = 0;
        auto handling = false;
        try {
            auto parsed = parser{};
            parsed.header_limit(
                static_cast<std::uint32_t>(options.max_header_bytes));
            parsed.body_limit(options.max_body_bytes);
            if (!co_await with_timeout(
                    options.header_timeout,
                    read_head(socket.input(), pending, parsed, options)))
                co_return;
            head = parsed.get().method() == bh::verb::head;
            auto req = checked_request(parsed);
            // Capture before trailers can influence Beast's connection
            // flags.
            keep_alive = parsed.keep_alive()
                         && count + 1 < options.max_requests_per_connection;
            if (!parsed.is_done())
                co_await with_timeout(
                    options.body_timeout,
                    read_body(socket.input(), pending, parsed, options));
            req.body = std::move(parsed.get().body());
            handling = true;
            auto res = co_await with_timeout(
                options.handler_timeout,
                invoke_handler(handler, std::move(req)));
            out = make_response(std::move(res), head, keep_alive, options);
        } catch (const operation_cancelled &) {
            if (current_task_stop_token().stop_requested())
                throw;
            status = 500; // a handler cancelled its own nested scope
        } catch (const timeout_error &) {
            status = handling ? 504 : 408;
        } catch (const rejection & error) {
            status = error.status;
        } catch (...) {
            if (current_task_stop_token().stop_requested())
                throw;
            if (!handling)
                co_return; // disconnect / receive failure
            status = 500;
        }
        if (current_task_stop_token().stop_requested())
            throw operation_cancelled{};
        if (status) {
            keep_alive = false;
            out = message{static_cast<bh::status>(status), 11};
            out.keep_alive(false);
            out.content_length(0);
        }
        // Never attempt a second response after a partial write or timeout.
        co_await with_timeout(
            options.write_timeout, write_response(socket.output(), out));
        if (!keep_alive)
            co_return;
    }
}

struct worker_slot
{
    bell assigned;
    nxt::unique_fd fd;
    bool idle = true;
};

struct server_state
{
    int listener;
    request_handler handler;
    server_options options;
    std::unique_ptr<worker_slot[]> slots;
    bell available;
};

task<void> worker(server_state & state, worker_slot & slot)
{
    while (true) {
        co_await slot.assigned;
        slot.assigned.reset();
        try {
            co_await connection(
                std::move(slot.fd), state.handler, state.options);
        } catch (const operation_cancelled &) {
            throw;
        } catch (const std::exception &) {
            // A peer disconnect/write timeout must not kill the accept
            // loop.
            if (current_task_stop_token().stop_requested())
                throw;
        }
        slot.idle = true;
        state.available.ring();
    }
}

task<void> dispatch(server_state & state)
{
    while (true) {
        auto * idle = static_cast<worker_slot *>(nullptr);
        for (std::size_t i = 0; i < state.options.max_connections; ++i) {
            if (state.slots[i].idle) {
                idle = &state.slots[i];
                break;
            }
        }
        if (!idle) {
            state.available.reset();
            co_await state.available;
            continue;
        }
        // Only this task accepts. Each bell also has exactly one waiter,
        // which matters for kqueue's single (fd, filter) readiness
        // registration.
        idle->fd = co_await net::accept(state.listener);
        idle->idle = false;
        idle->assigned.ring();
    }
}

struct server_scope : stop_on_failure
{
    server_state * state;

    explicit server_scope(server_state & state)
        : state(&state)
    {
    }

    task<void> operator()()
    {
        if (stop_requested())
            co_return;
        for (std::size_t i = 0; i < state->options.max_connections; ++i)
            fork(worker(*state, state->slots[i]));
        fork(dispatch(*state));
        co_await join();
    }
};

} // namespace

task<void>
serve(int listener, request_handler handler, server_options options)
{
    if (!handler || options.max_connections == 0
        || options.max_connections > 1024
        || options.max_requests_per_connection == 0
        || options.max_header_bytes < 4
        || options.max_header_bytes
               > std::numeric_limits<std::uint32_t>::max()
        || options.max_response_header_bytes < 128
        || options.header_timeout <= std::chrono::nanoseconds::zero()
        || options.body_timeout <= std::chrono::nanoseconds::zero()
        || options.handler_timeout <= std::chrono::nanoseconds::zero()
        || options.write_timeout <= std::chrono::nanoseconds::zero())
        throw std::invalid_argument{"invalid HTTP server options"};
    auto state = server_state{
        listener,
        std::move(handler),
        options,
        std::make_unique<worker_slot[]>(options.max_connections),
        {}};
    // State, bells and any queued descriptors outlive cancellation and
    // join.
    co_await server_scope{state};
}

} // namespace nxtrt::http
