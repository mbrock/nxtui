#pragma once

#include "nxtrt/http.hpp"

#include <chrono>
#include <functional>

namespace nxtrt::http {

struct response
{
    int status = 200;
    std::vector<header> headers;
    std::string body;
};

struct server_options
{
    std::size_t max_header_bytes = 16 * 1024;
    std::size_t max_body_bytes = 1024 * 1024;
    // Includes chunk framing, extensions and trailers, not the initial
    // head.
    std::size_t max_body_wire_bytes = 2 * 1024 * 1024;
    std::size_t max_connections = 64; // 1..1024 concurrent connections
    std::size_t max_requests_per_connection = 1000;
    std::size_t max_response_header_bytes = 16 * 1024;
    std::size_t max_response_body_bytes = 8 * 1024 * 1024;
    std::chrono::nanoseconds header_timeout = std::chrono::seconds{10};
    std::chrono::nanoseconds body_timeout = std::chrono::seconds{30};
    std::chrono::nanoseconds handler_timeout = std::chrono::seconds{30};
    std::chrono::nanoseconds write_timeout = std::chrono::seconds{30};
};

using request_handler = std::function<task<response>(request)>;

/// Plain HTTP/1.1 origin server, intended behind a TLS-terminating proxy.
/// The listener is borrowed and must outlive this task, including
/// cancellation. Accepted sockets close only after their operations have
/// drained. Cancel and await serve to stop; handlers must cooperate with
/// runtime cancellation.
///
/// A bounded pool admits connection recipes from a single async accept
/// feed, rather than running permanent workers. Stop drains both acceptance
/// and connections before returning normally and releasing the pool's
/// storage.
///
/// Requests are sequential per connection, concurrent up to
/// max_connections. Only origin-form targets (and OPTIONS *) are accepted.
/// CONNECT, upgrades, expectations, folded headers and ambiguous framing
/// are rejected and closed. Trailer fields are not exposed to handlers.
/// Forwarded/X-Forwarded-* remain untrusted ordinary headers. There is no
/// decompression or streaming body API.
///
/// The server owns response Content-Length/Connection/Transfer-Encoding
/// policy; application values for these are ignored after validation.
/// Invalid response headers/status or oversized responses produce 500 and
/// close. HEAD reports the representation length without sending a body;
/// 204/304 omit body and length, and 205 sends Content-Length: 0.
/// Informational responses are not an application response. Limits bound
/// server buffering, not handler allocations. Timeouts cover whole phases,
/// not individual reads/writes; all must be > 0.
task<void>
serve(int listener, request_handler handler, server_options options = {});

} // namespace nxtrt::http
