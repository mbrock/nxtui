#pragma once

#include "nxtrt/fs.hpp"
#include "nxtrt/http.hpp"
#include <nxt/unique-fd.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>

namespace nxtrt::http {

/// A response body streamed from an owned file descriptor.
///
/// The server sends exactly LENGTH bytes starting at OFFSET, framed with
/// `Content-Length: LENGTH`, after the response head, and closes FD when
/// the response is done (or dropped, as for HEAD, 204 and 304). Reads use
/// positioned reads, so the descriptor's file offset is unused. If the
/// file ends before LENGTH bytes, the connection is aborted rather than
/// misframed; the client sees a short body.
struct file_body
{
    nxt::unique_fd fd;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
};

/// What a `request_handler` returns.
///
/// STATUS must be 200..599. Header names must be tokens and values free of
/// control characters other than tab; Content-Length, Connection,
/// Transfer-Encoding, Trailer, Upgrade, Keep-Alive and Proxy-Connection are
/// validated and then dropped, since the server owns framing. Anything
/// else invalid turns the response into a 500.
struct response
{
    int status = 200;
    std::vector<header> headers;
    /// The whole body; at most `server_options::max_response_body_bytes`.
    std::string body;
    /// Replaces BODY when set (BODY must then be empty); not bounded by
    /// max_response_body_bytes.
    std::optional<file_body> file;
};

/// Limits and deadlines for `serve`. `serve` throws
/// `std::invalid_argument` if any timeout is not positive,
/// `max_connections` is outside 1..1024, `max_requests_per_connection` is
/// 0, `max_header_bytes` is below 4 or `max_response_header_bytes` below
/// 128.
struct server_options
{
    /// Request line plus header fields, including the blank line; larger
    /// heads get 431. Chunk-size lines and trailers are bounded by it too.
    std::size_t max_header_bytes = 16 * 1024;
    /// Request body after de-chunking; larger bodies get 413.
    std::size_t max_body_bytes = 1024 * 1024;
    /// Request body bytes as received. Includes chunk framing, extensions
    /// and trailers, not the initial head.
    std::size_t max_body_wire_bytes = 2 * 1024 * 1024;
    /// Connections served at once (1..1024).
    std::size_t max_connections = 64;
    /// Requests served on one connection before it is closed; the last
    /// response carries `Connection: close`.
    std::size_t max_requests_per_connection = 1000;
    /// Response header fields as given by the handler (each counted as
    /// name, value and 4 bytes), plus 128 bytes reserved for the status
    /// line and framing fields.
    std::size_t max_response_header_bytes = 16 * 1024;
    /// Size of `response::body`; does not apply to `response::file`.
    std::size_t max_response_body_bytes = 8 * 1024 * 1024;
    /// Whole time to receive a request head, including the idle wait on a
    /// kept-alive connection. Expiry answers 408 and closes.
    std::chrono::nanoseconds header_timeout = std::chrono::seconds{10};
    /// Whole time to receive a request body. Expiry answers 408.
    std::chrono::nanoseconds body_timeout = std::chrono::seconds{30};
    /// Whole time for the handler. Expiry cancels the handler, waits for
    /// it to finish, and answers 504.
    std::chrono::nanoseconds handler_timeout = std::chrono::seconds{30};
    /// Whole time to write the response head and string body, and,
    /// separately, each 16 KiB chunk of a file body.
    std::chrono::nanoseconds write_timeout = std::chrono::seconds{30};
    /// Reads file bodies off the deck where the wand would block; null
    /// reads them with plain read wishes. Borrowed; must outlive `serve`.
    fs::files * files = nullptr;
};

/// Request handler for `serve`: takes the request by value and returns a
/// task producing the response.
///
/// Handlers run on the serving deck, one at a time per connection and
/// concurrently across connections. An exception from the handler becomes
/// a 500 and closes the connection.
using request_handler = std::function<task<response>(request)>;

/// Serves HTTP/1.1 on LISTENER until cancelled.
///
/// Plain HTTP/1.1 origin server, intended behind a TLS-terminating proxy.
/// LISTENER is a bound, listening socket; it is borrowed and must outlive
/// this task, including cancellation. Accepted sockets close only after
/// their operations have drained. The task is lazy and runs on the deck
/// that awaits it.
///
/// Stopping: cancel the awaiting task (or the group it runs in). Stop
/// cancels acceptance and every open connection, including handlers,
/// waits for all of them to finish, and then `serve` returns normally.
/// Handlers must cooperate with runtime cancellation. A failed connection
/// (peer disconnect, write error) is dropped without stopping the server.
///
/// A bounded pool admits connection recipes from a single async accept
/// feed, rather than running permanent workers; a slot frees when its
/// connection ends.
///
/// Protocol: requests are sequential per connection (pipelined bytes are
/// kept), concurrent up to max_connections. Only HTTP/1.1 is accepted
/// (other versions get 505), with exactly one valid Host header. Only
/// origin-form targets (and OPTIONS *) are accepted. CONNECT (501),
/// upgrades, expectations (417), folded headers and ambiguous framing are
/// rejected and the connection closed. Request bodies may use
/// Content-Length or chunked framing; trailer fields are validated but
/// not exposed to handlers. Forwarded/X-Forwarded-* remain untrusted
/// ordinary headers. There is no decompression or streaming body API.
///
/// Responses: the server owns Content-Length/Connection/Transfer-Encoding
/// policy; application values for these are ignored after validation.
/// Invalid response headers/status or oversized responses produce 500 and
/// close. HEAD reports the representation length without sending a body;
/// 204/304 omit body and length, and 205 sends Content-Length: 0.
/// Informational (1xx) responses are not an application response. A
/// response the server generates for a failure always closes the
/// connection.
///
/// Limits bound server buffering, not handler allocations. Timeouts cover
/// whole phases, not individual reads/writes, except that a file body gets
/// a fresh write_timeout for each chunk, so long downloads only need to
/// keep making progress. Throws `std::invalid_argument` for invalid
/// options or an empty handler.
///
/// @code
/// auto listener = nxtrt::net::listen_tcp_loopback(8080);
/// co_await nxtrt::http::serve(
///     listener.get(),
///     [](nxtrt::http::request req) -> nxtrt::task<nxtrt::http::response> {
///         co_return nxtrt::http::response{.body = "hello\n"};
///     });
/// @endcode
task<void>
serve(int listener, request_handler handler, server_options options = {});

} // namespace nxtrt::http
