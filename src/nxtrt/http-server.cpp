#include "nxtrt/http-server.hpp"

#include "nxtrt/farm.hpp"
#include "nxtrt/net.hpp"
#include "nxtrt/pool.hpp"

#include <array>
#include <memory>

namespace nxtrt::http {
namespace {

struct rejection
{
    int status;
};

bool token_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
           || (c >= '0' && c <= '9')
           || std::string_view{"!#$%&'*+-.^_`|~"}.find(c)
                  != std::string_view::npos;
}

bool field_char(unsigned char c)
{
    return c == '\t' || (c >= 32 && c != 127);
}

// Strict field syntax: no whitespace before ':', folding or controls.
header parse_field(std::string_view line)
{
    auto colon = line.find(':');
    auto name = line.substr(0, colon);
    if (colon == line.npos || name.empty()
        || !std::ranges::all_of(name, token_char))
        throw rejection{400};
    auto value = line.substr(colon + 1);
    if (!std::ranges::all_of(value, field_char))
        throw rejection{400};
    return {std::string{name}, std::string{trim_ascii(value)}};
}

struct request_head
{
    request req;
    std::uint64_t length = 0;
    bool chunked = false;
    bool keep_alive = true;
    std::size_t bytes = 0;
};

request_head parse_head(std::string_view text)
{
    auto parsed = request_head{};
    parsed.bytes = text.size();
    auto eol = text.find("\r\n");
    auto line = text.substr(0, eol);
    auto space = line.find(' ');
    auto second =
        space == line.npos ? line.npos : line.find(' ', space + 1);
    if (space == line.npos || second == line.npos || space == 0
        || !std::ranges::all_of(line.substr(0, space), token_char))
        throw rejection{400};
    auto target = line.substr(space + 1, second - space - 1);
    if (target.empty() || !std::ranges::all_of(target, [](unsigned char c) {
            return c > 32 && c < 127;
        }))
        throw rejection{400};
    auto version = line.substr(second + 1);
    if (version.size() != 8 || !version.starts_with("HTTP/")
        || version[5] < '0' || version[5] > '9' || version[6] != '.'
        || version[7] < '0' || version[7] > '9')
        throw rejection{400};
    if (version != "HTTP/1.1")
        throw rejection{505};
    parsed.req.method = line.substr(0, space);
    if (parsed.req.method == "CONNECT")
        throw rejection{501};
    if ((target.front() != '/'
         && !(parsed.req.method == "OPTIONS" && target == "*"))
        || target.find('#') != target.npos)
        throw rejection{400};
    parsed.req.target = target;
    text.remove_prefix(eol + 2);
    auto hosts = 0;
    auto lengths = 0;
    auto transfers = 0;
    auto expects = false;
    while (text != "\r\n") {
        eol = text.find("\r\n");
        if (eol == text.npos)
            throw rejection{400};
        auto field = parse_field(text.substr(0, eol));
        text.remove_prefix(eol + 2);
        if (iequals(field.name, "Host")) {
            ++hosts;
            parsed.req.host = field.value;
            if (field.value.empty())
                throw rejection{400};
            for (unsigned char c : field.value) {
                if (c <= 32 || c == 127 || c == ',' || c == '/' || c == '@'
                    || c == '\\' || c == '#' || c == '?')
                    throw rejection{400};
            }
        } else if (iequals(field.name, "Content-Length")) {
            ++lengths;
            auto value = std::string_view{field.value};
            if (value.empty() || !std::ranges::all_of(value, [](char c) {
                    return c >= '0' && c <= '9';
                }))
                throw rejection{400};
            auto [end, ec] = std::from_chars(
                value.data(), value.data() + value.size(), parsed.length);
            if (ec != std::errc{} || end != value.data() + value.size())
                throw rejection{400};
        } else if (iequals(field.name, "Transfer-Encoding")) {
            ++transfers;
            if (!iequals(field.value, "chunked"))
                throw rejection{400};
            parsed.chunked = true;
        } else if (iequals(field.name, "Expect")) {
            expects = true;
        } else if (iequals(field.name, "Upgrade")) {
            throw rejection{400};
        } else if (iequals(field.name, "Connection")) {
            auto tokens = std::string_view{field.value};
            while (true) {
                auto comma = tokens.find(',');
                auto token = trim_ascii(tokens.substr(0, comma));
                if (!token.empty()
                    && !std::ranges::all_of(token, token_char))
                    throw rejection{400};
                if (iequals(token, "upgrade"))
                    throw rejection{400};
                if (iequals(token, "close"))
                    parsed.keep_alive = false;
                if (comma == tokens.npos)
                    break;
                tokens.remove_prefix(comma + 1);
            }
        }
        parsed.req.headers.push_back(std::move(field));
    }
    if (hosts != 1 || lengths > 1 || transfers > 1
        || (lengths && transfers))
        throw rejection{400};
    if (expects)
        throw rejection{417};
    return parsed;
}

// Read only within the current bound, retaining any pipelined bytes.
task<bool> append_input(
    bytefeed & input, std::string & pending, std::size_t limit, int status)
{
    if (pending.size() >= limit)
        throw rejection{status};
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
    request_head & parsed,
    const server_options & options)
{
    auto end = pending.find("\r\n\r\n");
    while (end == std::string::npos) {
        if (!co_await append_input(
                input, pending, options.max_header_bytes, 431)) {
            if (pending.empty())
                co_return false;
            throw rejection{400};
        }
        end = pending.find("\r\n\r\n");
    }
    auto size = end + 4;
    if (size > options.max_header_bytes)
        throw rejection{431};
    parsed = parse_head(std::string_view{pending}.substr(0, size));
    pending.erase(0, size);
    co_return true;
}

// Each consumed body byte, including framing and trailers, spends wire
// budget.
task<std::string> read_line(
    bytefeed & input,
    std::string & pending,
    std::size_t & wire,
    std::size_t limit)
{
    auto end = pending.find("\r\n");
    while (end == pending.npos) {
        auto bound = std::min(wire, limit);
        if (!co_await append_input(
                input, pending, bound, wire < limit ? 413 : 431))
            throw rejection{400};
        end = pending.find("\r\n");
    }
    auto size = end + 2;
    if (size > wire)
        throw rejection{413};
    if (size > limit)
        throw rejection{431};
    auto line = pending.substr(0, end);
    pending.erase(0, size);
    wire -= size;
    co_return line;
}

std::string_view trim_left(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    return text;
}

// Validate ignored extensions as token/quoted-string pairs (RFC
// 9112 7.1.1).
std::uint64_t chunk_length(std::string_view line)
{
    auto end = line.find_first_not_of("0123456789abcdefABCDEF");
    auto digits = line.substr(0, end);
    auto size = std::uint64_t{};
    auto [ptr, ec] = std::from_chars(
        digits.data(), digits.data() + digits.size(), size, 16);
    if (digits.empty() || ec != std::errc{}
        || ptr != digits.data() + digits.size())
        throw rejection{400};
    line.remove_prefix(digits.size());
    while (!line.empty()) {
        line = trim_left(line);
        if (line.empty() || line.front() != ';')
            throw rejection{400};
        line.remove_prefix(1);
        line = trim_left(line);
        auto name = std::size_t{};
        while (name < line.size() && token_char(line[name]))
            ++name;
        if (name == 0)
            throw rejection{400};
        line.remove_prefix(name);
        // BWS is only valid before an extension separator or '='.
        if (line.empty())
            break;
        line = trim_left(line);
        if (line.empty())
            throw rejection{400};
        if (line.front() != '=')
            continue;
        line.remove_prefix(1);
        line = trim_left(line);
        if (line.empty())
            throw rejection{400};
        if (line.front() == '"') {
            line.remove_prefix(1);
            auto closed = false;
            while (!line.empty()) {
                auto c = static_cast<unsigned char>(line.front());
                line.remove_prefix(1);
                if (c == '"') {
                    closed = true;
                    break;
                }
                if (c == '\\') {
                    if (line.empty())
                        throw rejection{400};
                    c = static_cast<unsigned char>(line.front());
                    line.remove_prefix(1);
                }
                if (!field_char(c))
                    throw rejection{400};
            }
            if (!closed)
                throw rejection{400};
        } else {
            auto value = std::size_t{};
            while (value < line.size() && token_char(line[value]))
                ++value;
            if (value == 0)
                throw rejection{400};
            line.remove_prefix(value);
        }
    }
    return size;
}

task<void> read_payload(
    bytefeed & input,
    std::string & pending,
    std::string & body,
    std::uint64_t length,
    std::size_t & wire,
    std::size_t body_limit)
{
    if (length > body_limit - body.size() || length > wire)
        throw rejection{413};
    while (length) {
        if (pending.empty()
            && !co_await append_input(
                input, pending, std::min<std::uint64_t>(length, 4096), 413))
            throw rejection{400};
        auto count = static_cast<std::size_t>(
            std::min<std::uint64_t>(length, pending.size()));
        body.append(pending.data(), count);
        pending.erase(0, count);
        length -= count;
        wire -= count;
    }
}

task<void> read_body(
    bytefeed & input,
    std::string & pending,
    request_head & parsed,
    const server_options & options)
{
    auto wire = options.max_body_wire_bytes;
    auto & body = parsed.req.body;
    if (!parsed.chunked) {
        co_await read_payload(
            input,
            pending,
            body,
            parsed.length,
            wire,
            options.max_body_bytes);
        co_return;
    }
    while (true) {
        auto line = co_await read_line(
            input, pending, wire, options.max_header_bytes);
        auto length = chunk_length(line);
        if (length == 0)
            break;
        co_await read_payload(
            input, pending, body, length, wire, options.max_body_bytes);
        // Chunk data must be followed by exactly CRLF.
        if (wire < 2)
            throw rejection{413};
        while (pending.size() < 2) {
            if (!co_await append_input(input, pending, 2, 400))
                throw rejection{400};
        }
        if (!pending.starts_with("\r\n"))
            throw rejection{400};
        pending.erase(0, 2);
        wire -= 2;
    }
    // Trailer fields share the initial header budget; the final empty line
    // is framing and gets its own two bytes.
    auto trailer_budget = options.max_header_bytes - parsed.bytes + 2;
    while (true) {
        auto line =
            co_await read_line(input, pending, wire, trailer_budget);
        trailer_budget -= line.size() + 2;
        if (line.empty())
            break;
        auto field = parse_field(line);
        if (iequals(field.name, "Content-Length")
            || iequals(field.name, "Transfer-Encoding"))
            throw rejection{400};
        // Trailers are validated and discarded, never merged with the head.
    }
}

bool managed_header(std::string_view name)
{
    return iequals(name, "Content-Length") || iequals(name, "Connection")
           || iequals(name, "Transfer-Encoding") || iequals(name, "Trailer")
           || iequals(name, "Upgrade") || iequals(name, "Keep-Alive")
           || iequals(name, "Proxy-Connection");
}

std::string_view reason_phrase(int status)
{
    switch (status) {
    case 200:
        return "OK";
    case 201:
        return "Created";
    case 202:
        return "Accepted";
    case 203:
        return "Non-Authoritative Information";
    case 204:
        return "No Content";
    case 205:
        return "Reset Content";
    case 206:
        return "Partial Content";
    case 207:
        return "Multi-Status";
    case 208:
        return "Already Reported";
    case 226:
        return "IM Used";
    case 300:
        return "Multiple Choices";
    case 301:
        return "Moved Permanently";
    case 302:
        return "Found";
    case 303:
        return "See Other";
    case 304:
        return "Not Modified";
    case 305:
        return "Use Proxy";
    case 307:
        return "Temporary Redirect";
    case 308:
        return "Permanent Redirect";
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 402:
        return "Payment Required";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 406:
        return "Not Acceptable";
    case 407:
        return "Proxy Authentication Required";
    case 408:
        return "Request Timeout";
    case 409:
        return "Conflict";
    case 410:
        return "Gone";
    case 411:
        return "Length Required";
    case 412:
        return "Precondition Failed";
    case 413:
        return "Payload Too Large";
    case 414:
        return "URI Too Long";
    case 415:
        return "Unsupported Media Type";
    case 416:
        return "Range Not Satisfiable";
    case 417:
        return "Expectation Failed";
    case 418:
        return "I'm a teapot";
    case 421:
        return "Misdirected Request";
    case 422:
        return "Unprocessable Entity";
    case 423:
        return "Locked";
    case 424:
        return "Failed Dependency";
    case 425:
        return "Too Early";
    case 426:
        return "Upgrade Required";
    case 428:
        return "Precondition Required";
    case 429:
        return "Too Many Requests";
    case 431:
        return "Request Header Fields Too Large";
    case 451:
        return "Unavailable For Legal Reasons";
    case 500:
        return "Internal Server Error";
    case 501:
        return "Not Implemented";
    case 502:
        return "Bad Gateway";
    case 503:
        return "Service Unavailable";
    case 504:
        return "Gateway Timeout";
    case 505:
        return "HTTP Version Not Supported";
    case 506:
        return "Variant Also Negotiates";
    case 507:
        return "Insufficient Storage";
    case 508:
        return "Loop Detected";
    case 510:
        return "Not Extended";
    case 511:
        return "Network Authentication Required";
    default:
        return "Unknown";
    }
}

struct message
{
    std::string head;
    std::string body;
};

message make_response(
    response res,
    bool head,
    bool keep_alive,
    const server_options & options,
    std::optional<file_body> & file)
{
    if (res.status < 200 || res.status > 599
        || res.body.size() > options.max_response_body_bytes
        || (res.file && !res.body.empty()))
        throw rejection{500};
    auto out = message{};
    out.head = "HTTP/1.1 " + std::to_string(res.status) + " ";
    out.head += reason_phrase(res.status);
    out.head += "\r\n";
    // Reserve room for generated status line and framing fields.
    auto remaining = options.max_response_header_bytes - 128;
    for (auto const & field : res.headers) {
        if (field.name.empty()
            || !std::ranges::all_of(field.name, token_char)
            || !std::ranges::all_of(field.value, field_char))
            throw rejection{500};
        if (field.name.size() > remaining)
            throw rejection{500};
        remaining -= field.name.size();
        if (remaining < 4 || field.value.size() > remaining - 4)
            throw rejection{500};
        remaining -= field.value.size() + 4;
        if (!managed_header(field.name))
            out.head += field.name + ": " + field.value + "\r\n";
    }
    if (!keep_alive)
        out.head += "Connection: close\r\n";
    if (res.status != 204 && res.status != 304) {
        auto size = res.status == 205 ? std::uint64_t{0}
                    : res.file        ? res.file->length
                                      : res.body.size();
        out.head += "Content-Length: " + std::to_string(size) + "\r\n";
        if (!head && res.status != 205) {
            if (res.file)
                file = std::move(res.file);
            else
                out.body = std::move(res.body);
        }
    }
    out.head += "\r\n";
    return out;
}

task<void> write_response(bytesink & output, const message & msg)
{
    co_await write_all(output, msg.head);
    if (!msg.body.empty())
        co_await write_all(output, msg.body);
}

task<void> write_file(
    bytesink & output,
    file_body & file,
    const server_options & options)
{
    auto buffer = std::array<std::byte, 16 * 1024>{};
    auto offset = file.offset;
    auto remaining = file.length;
    while (remaining) {
        auto chunk = std::span{buffer}.first(
            static_cast<std::size_t>(
                std::min<std::uint64_t>(remaining, buffer.size())));
        auto count = options.files
            ? co_await options.files->read_at(file.fd.get(), chunk, offset)
            : co_await op::read_some{
                  file.fd.get(), chunk, static_cast<off_t>(offset)};
        if (count == 0)
            throw protocol_error{"file body ended early"};
        co_await with_timeout(
            options.write_timeout,
            write_all(output, as_string_view(chunk.first(count))));
        offset += count;
        remaining -= count;
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
        auto file = std::optional<file_body>{};
        auto keep_alive = false;
        auto head = false;
        auto status = 0;
        auto handling = false;
        try {
            auto parsed = request_head{};
            if (!co_await with_timeout(
                    options.header_timeout,
                    read_head(socket.input(), pending, parsed, options)))
                co_return;
            head = parsed.req.method == "HEAD";
            keep_alive = parsed.keep_alive
                         && count + 1 < options.max_requests_per_connection;
            if (parsed.length > options.max_body_bytes
                || parsed.length > options.max_body_wire_bytes)
                throw rejection{413};
            if (parsed.chunked || parsed.length)
                co_await with_timeout(
                    options.body_timeout,
                    read_body(socket.input(), pending, parsed, options));
            auto req = std::move(parsed.req);
            handling = true;
            auto res = co_await with_timeout(
                options.handler_timeout,
                invoke_handler(handler, std::move(req)));
            out = make_response(
                std::move(res), head, keep_alive, options, file);
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
            file.reset();
            keep_alive = false;
            out = make_response(
                response{.status = status, .headers = {}, .body = {}, .file = {}},
                false, false, options, file);
        }
        // Never attempt a second response after a partial write or timeout.
        co_await with_timeout(
            options.write_timeout, write_response(socket.output(), out));
        if (file)
            co_await write_file(socket.output(), *file, options);
        if (!keep_alive)
            co_return;
    }
}

task<void> run_connection(
    nxt::unique_fd fd,
    request_handler * handler,
    const server_options * options)
{
    try {
        co_await connection(std::move(fd), *handler, *options);
    } catch (const operation_cancelled &) {
        if (current_task_stop_token().stop_requested())
            throw;
    } catch (...) {
        if (current_task_stop_token().stop_requested())
            throw;
        // A peer disconnect or write failure must not stop acceptance.
    }
}

struct connection_recipe
{
    nxt::unique_fd fd;
    request_handler * handler;
    const server_options * options;

    task<void> operator()() &
    {
        return run_connection(std::move(fd), handler, options);
    }
};

class accepted_connections final : public feed<connection_recipe>
{
public:
    accepted_connections(
        int listener,
        request_handler & handler,
        const server_options & options)
        : feed(1)
        , listener_(listener)
        , handler_(handler)
        , options_(options)
    {
    }

private:
    task<std::optional<connection_recipe>> next_value() override
    {
        auto fd = co_await net::accept(listener_);
        co_return connection_recipe{std::move(fd), &handler_, &options_};
    }

    int listener_;
    request_handler & handler_;
    const server_options & options_;
};

task<void> consume_connections(pool<connection_recipe> & connections)
{
    while (co_await connections.take())
        ;
}

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
    auto slots = std::make_unique<pool_slot<connection_recipe>[]>(
        options.max_connections);
    auto hot_size =
        nxtrt::farm<pool_slot<connection_recipe>>::hot_capacity_for(
            options.max_connections);
    auto cold_size = mask<>::words_for(options.max_connections);
    auto farm_indices = std::make_unique<std::size_t[]>(hot_size);
    auto cold_indices = std::make_unique<std::uint64_t[]>(cold_size);
    auto output =
        std::make_unique<std::monostate[]>(options.max_connections);
    auto farm = nxtrt::farm<pool_slot<connection_recipe>>{
        std::span{slots.get(), options.max_connections},
        farm_index_storage_ref{
            std::span{farm_indices.get(), hot_size},
            std::span{cold_indices.get(), cold_size}}};
    auto accepted = accepted_connections{listener, handler, options};
    auto connections = pool<connection_recipe>{
        accepted,
        farm,
        value_storage_ref<std::monostate>{
            output.get(), options.max_connections}};
    // Borrowed state and every piece of pool land remain alive through
    // drain.
    try {
        co_await finally(consume_connections(connections), [&connections] {
            return connections.close();
        });
    } catch (const operation_cancelled &) {
        // As with the former server scope, stopping the serving task is a
        // normal shutdown, but only after acceptance and connections drain.
        if (!current_task_stop_token().stop_requested())
            throw;
    }
}

} // namespace nxtrt::http
