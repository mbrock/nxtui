#pragma once

#include "nxtrt/http.hpp"
#include "nxtrt/net_dns.hpp"
#include "nxtrt/tls.hpp"

#include <array>
#include <memory>

namespace nxtrt::websocket {

struct protocol_error : runtime_error
{
    using runtime_error::runtime_error;
};

enum class message_type { text = 1, binary = 2, close = 8, pong = 10 };

/// Owned message, never a view into the connection. Binary data may contain
/// NULs. For close, data is the UTF-8 reason and code is absent for an
/// empty close payload. Ping is consumed internally and answered with a
/// masked pong.
struct message
{
    message_type type;
    std::string data;
    std::optional<std::uint16_t> code = {};
};

struct options
{
    std::string ca_file; ///< Explicit PEM roots required on UWP for wss.
    std::size_t max_message_size = 1024 * 1024; ///< Incoming and outgoing.
};

namespace detail {

inline std::string base64(std::span<const std::byte> bytes)
{
    constexpr auto alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        auto remaining = bytes.size() - i;
        auto n = std::to_integer<unsigned>(bytes[i]) << 16;
        if (remaining > 1)
            n |= std::to_integer<unsigned>(bytes[i + 1]) << 8;
        if (remaining > 2)
            n |= std::to_integer<unsigned>(bytes[i + 2]);
        out += alphabet[(n >> 18) & 63];
        out += alphabet[(n >> 12) & 63];
        out += remaining > 1 ? alphabet[(n >> 6) & 63] : '=';
        out += remaining > 2 ? alphabet[n & 63] : '=';
    }
    return out;
}

inline std::string accept_key(std::string key)
{
    key += "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    return base64(nxt::crypto::sha1(std::as_bytes(std::span{key})));
}

inline bool utf8(std::string_view text)
{
    std::uint32_t value = 0, minimum = 0;
    unsigned left = 0;
    for (unsigned char c : text) {
        if (left) {
            if ((c & 0xc0) != 0x80)
                return false;
            value = (value << 6) | (c & 63);
            if (!--left
                && (value < minimum || value > 0x10ffff
                    || (value >= 0xd800 && value <= 0xdfff)))
                return false;
        } else if (c >= 0x80) {
            if (c >= 0xc2 && c <= 0xdf) {
                left = 1;
                minimum = 0x80;
                value = c & 31;
            } else if (c >= 0xe0 && c <= 0xef) {
                left = 2;
                minimum = 0x800;
                value = c & 15;
            } else if (c >= 0xf0 && c <= 0xf4) {
                left = 3;
                minimum = 0x10000;
                value = c & 7;
            } else {
                return false;
            }
        }
    }
    return left == 0;
}

inline bool close_code(std::uint16_t code)
{
    return (code >= 1000 && code <= 1014 && code != 1004 && code != 1005
            && code != 1006)
           || (code >= 3000 && code <= 4999);
}

inline bool token(std::string_view value)
{
    if (value.empty())
        return false;
    for (unsigned char c : value)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
              || (c >= '0' && c <= '9')
              || std::string_view{"!#$%&'*+-.^_`|~"}.find(c)
                     != std::string_view::npos))
            return false;
    return true;
}

inline void validate_head(std::span<const std::byte> bytes)
{
    auto text = as_string_view(bytes);
    auto eol = text.find("\r\n");
    if (eol == std::string_view::npos
        || !text.substr(0, eol).starts_with("HTTP/1.1 101 "))
        throw protocol_error{"malformed WebSocket Upgrade"};
    for (unsigned char c : text.substr(0, eol))
        if ((c < 32 && c != '\t') || c == 127)
            throw protocol_error{"malformed WebSocket status line"};
    text.remove_prefix(eol + 2);
    while (!text.empty()) {
        eol = text.find("\r\n");
        auto line = text.substr(0, eol);
        if (line.empty())
            break;
        auto colon = line.find(':');
        if (colon == std::string_view::npos
            || !token(line.substr(0, colon)))
            throw protocol_error{"malformed WebSocket header"};
        for (unsigned char c : line.substr(colon + 1))
            if ((c < 32 && c != '\t') || c == 127)
                throw protocol_error{"malformed WebSocket header value"};
        if (eol == std::string_view::npos)
            break;
        text.remove_prefix(eol + 2);
    }
}

inline http::url parse_url(std::string_view url)
{
    auto https = url.starts_with("wss://");
    if (!https && !url.starts_with("ws://"))
        throw invalid_argument{"only ws:// and wss:// are supported"};
    url.remove_prefix(https ? 6 : 5);
    for (unsigned char c : url)
        if (c <= 32 || c == 127 || c == '#' || c == '\\')
            throw invalid_argument{"unsafe WebSocket URL"};
    auto parsed = http::parse_url(
        std::string{https ? "https://" : "http://"} + std::string{url});
    if (parsed.host.find_first_of("@[]?:") != std::string::npos)
        throw invalid_argument{"unsupported WebSocket authority"};
    unsigned port = 0;
    auto [end, error] = std::from_chars(
        parsed.port.data(), parsed.port.data() + parsed.port.size(), port);
    if (error != std::errc{}
        || end != parsed.port.data() + parsed.port.size() || port == 0
        || port > 65535)
        throw invalid_argument{"invalid WebSocket port"};
    return parsed;
}

} // namespace detail

class client;
inline task<std::unique_ptr<client>>
connect(std::string url, options config = {});

/// Owning RFC6455 client over NXT sockets and authenticated TLS 1.3. No
/// extensions/subprotocols, compression, redirects or automatic reconnect.
/// The object is immovable and deck-confined; connect returns unique_ptr.
/// Calls copy their arguments into coroutine frames. The client must
/// outlive its tasks. Operations must not overlap (including send vs
/// receive): this follows the underlying bytefeed/TLS contract; overlap
/// throws logic_error. Apply with_timeout/settle for deadlines.
/// Cancellation or any I/O/protocol failure drains the outstanding runtime
/// wish, then closes the connection; reconnect, never resume a partially
/// read or written frame.
class client
{
public:
    client(const client &) = delete;
    client & operator=(const client &) = delete;

    /// Sends one FIN frame. Payload is owned until completion. Text must be
    /// valid UTF-8; binary strings may contain arbitrary bytes. Every frame
    /// uses a fresh cryptographically random mask, including control
    /// replies.
    task<> send(message_type type, std::string data)
    {
        if (type != message_type::text && type != message_type::binary)
            throw invalid_argument{"send requires text or binary"};
        if (data.size() > config_.max_message_size
            || (type == message_type::text && !detail::utf8(data)))
            throw invalid_argument{"invalid WebSocket message"};
        auto operation = guard{*this};
        if (close_sent_)
            throw logic_error{"WebSocket is closing"};
        co_await frame(static_cast<unsigned>(type), data);
        operation.complete = true;
    }

    task<> ping(std::string data = {})
    {
        if (data.size() > 125)
            throw invalid_argument{"WebSocket ping exceeds 125 bytes"};
        auto operation = guard{*this};
        if (close_sent_)
            throw logic_error{"WebSocket is closing"};
        co_await frame(9, data);
        operation.complete = true;
    }

    /// Sends Close, but does not wait for its reply. Continue receive()
    /// until the close event; use a deadline, since a peer need not
    /// respond. No more data sends are allowed. Destruction aborts without
    /// a closing handshake.
    task<> close(std::uint16_t code = 1000, std::string reason = {})
    {
        if (!detail::close_code(code) || reason.size() > 123
            || !detail::utf8(reason))
            throw invalid_argument{"invalid WebSocket close"};
        auto operation = guard{*this};
        if (!close_sent_) {
            auto data =
                std::string{
                    static_cast<char>(code >> 8),
                    static_cast<char>(code & 255)}
                + reason;
            co_await frame(8, data);
            close_sent_ = true;
        }
        operation.complete = true;
    }

    /// Reassembles a bounded text/binary message, allowing interleaved
    /// controls. Pong events preserve their payload. Close is echoed once,
    /// returned once, then subsequent receives return nullopt. EOF before
    /// Close is an error, never a successful closing handshake.
    task<std::optional<message>> receive()
    {
        if (close_received_)
            co_return std::nullopt;
        auto operation = guard{*this};
        for (;;) {
            auto first = co_await octet();
            auto second = co_await octet();
            auto fin = (first & 128) != 0;
            auto opcode = first & 15;
            auto control = opcode >= 8;
            if ((first & 112) || (second & 128)
                || (opcode != 0 && opcode != 1 && opcode != 2 && opcode != 8
                    && opcode != 9 && opcode != 10))
                throw protocol_error{
                    "invalid WebSocket frame flags/opcode"};
            std::uint64_t size = second & 127;
            if (control && (!fin || size > 125))
                throw protocol_error{"invalid WebSocket control frame"};
            if (size == 126 || size == 127) {
                auto width = size == 126 ? 2 : 8;
                size = 0;
                for (int i = 0; i < width; ++i)
                    size = (size << 8) | co_await octet();
                if ((width == 2 && size < 126)
                    || (width == 8 && (size < 65536 || (size >> 63))))
                    throw protocol_error{"noncanonical WebSocket length"};
            }
            if (!control) {
                if ((opcode == 0 && !fragment_type_)
                    || (opcode != 0 && fragment_type_))
                    throw protocol_error{"invalid WebSocket fragmentation"};
                if (size > config_.max_message_size - fragments_.size())
                    throw protocol_error{
                        "WebSocket message limit exceeded"};
            }
            auto payload = std::string{};
            payload.reserve(static_cast<std::size_t>(size));
            while (payload.size() < size) {
                auto part = co_await input().take_some(
                    static_cast<std::size_t>(size) - payload.size());
                if (!part)
                    throw protocol_error{"truncated WebSocket payload"};
                payload.append(as_string_view(*part));
            }
            if (opcode == 9) {
                co_await frame(10, payload);
                continue;
            }
            if (opcode == 10) {
                operation.complete = true;
                co_return message{message_type::pong, std::move(payload)};
            }
            if (opcode == 8) {
                auto event = message{message_type::close, {}};
                if (!payload.empty()) {
                    if (payload.size() == 1)
                        throw protocol_error{
                            "invalid WebSocket close payload"};
                    auto code = static_cast<std::uint16_t>(
                        (static_cast<unsigned char>(payload[0]) << 8)
                        | static_cast<unsigned char>(payload[1]));
                    event.code = code;
                    event.data = payload.substr(2);
                    if (!detail::close_code(code)
                        || !detail::utf8(event.data))
                        throw protocol_error{
                            "invalid WebSocket close code/reason"};
                }
                if (!close_sent_)
                    co_await frame(8, payload);
                close_sent_ = close_received_ = true;
                fragments_.clear();
                disconnect();
                operation.complete = true;
                co_return event;
            }
            if (opcode != 0)
                fragment_type_ = static_cast<message_type>(opcode);
            fragments_ += payload;
            if (!fin)
                continue;
            auto event = message{*fragment_type_, std::move(fragments_)};
            fragments_.clear();
            fragment_type_.reset();
            if (event.type == message_type::text
                && !detail::utf8(event.data))
                throw protocol_error{"invalid WebSocket UTF-8"};
            if (close_sent_)
                continue; // Discard data after initiating Close.
            operation.complete = true;
            co_return event;
        }
    }

private:
    friend task<std::unique_ptr<client>> connect(std::string, options);

    explicit client(options config)
        : config_(std::move(config))
    {
    }

    struct guard
    {
        client & owner;
        bool complete = false;

        explicit guard(client & owner)
            : owner(owner)
        {
            if (owner.busy_)
                throw logic_error{"overlapping WebSocket operations"};
            if (!owner.socket_)
                throw logic_error{"WebSocket connection is closed"};
            owner.busy_ = true;
        }

        ~guard()
        {
            owner.busy_ = false;
            if (!complete)
                owner.disconnect();
        }
    };

    void disconnect()
    {
        tls_.reset();
        socket_.reset();
    }

    bytefeed & input()
    {
        return tls_ ? static_cast<bytefeed &>(*tls_) : socket_->input();
    }

    task<unsigned> octet()
    {
        throw_if_stop_requested();
        auto byte = co_await input().take();
        if (!byte)
            throw protocol_error{"WebSocket EOF without Close"};
        co_return std::to_integer<unsigned>(*byte);
    }

    task<> write_bytes(std::span<const std::byte> bytes)
    {
        if (tls_)
            co_await tls_->write_all(bytes);
        else {
            co_await nxtrt::write(socket_->output(), bytes);
            co_await socket_->output().flush();
        }
    }

    task<> frame(unsigned opcode, std::string_view data)
    {
        throw_if_stop_requested();
        std::array<std::byte, 4> mask;
        nxt::crypto::random(mask);
        auto bytes = std::vector<std::byte>{std::byte(128 | opcode)};
        auto size = data.size();
        if (size < 126)
            bytes.push_back(std::byte(128 | size));
        else {
            auto width = size <= 65535 ? 2 : 8;
            bytes.push_back(std::byte(width == 2 ? 254 : 255));
            auto wide = static_cast<std::uint64_t>(size);
            for (int i = width - 1; i >= 0; --i)
                bytes.push_back(std::byte((wide >> (8 * i)) & 255));
        }
        bytes.insert(bytes.end(), mask.begin(), mask.end());
        for (std::size_t i = 0; i < size; ++i)
            bytes.push_back(
                std::byte(static_cast<unsigned char>(data[i]))
                ^ mask[i % 4]);
        co_await write_bytes(bytes);
    }

    task<> upgrade(http::url url)
    {
        socket_.reset(new net::socket{
            co_await net::connect_tcp(url.host, url.port), tx_, rx_});
        if (url.tls) {
            tls_ = std::make_unique<tls::tls13_client_session>(
                *socket_, 16384);
            co_await tls_->handshake(url.host, config_.ca_file);
        }
        std::array<std::byte, 16> nonce;
        nxt::crypto::random(nonce);
        auto key = detail::base64(nonce);
        auto request = http::request{
            .target = url.target,
            .host = http::host_header(url),
            .headers =
                {{"Upgrade", "websocket"},
                 {"Connection", "Upgrade"},
                 {"Sec-WebSocket-Key", key},
                 {"Sec-WebSocket-Version", "13"}},
            .body = {}};
        auto wire = http::serialize(request);
        co_await write_bytes(std::as_bytes(std::span{wire}));
        auto bytes = co_await input().take_until("\r\n\r\n");
        detail::validate_head(bytes);
        auto head = http::parse_response_head(bytes);
        unsigned accepts = 0;
        bool upgrade = false, connection = false;
        for (auto const & header : head.headers) {
            auto single = http::response_head{};
            single.headers.push_back(header);
            upgrade |=
                http::has_header_token(single, "upgrade", "websocket");
            connection |=
                http::has_header_token(single, "connection", "upgrade");
            if (http::iequals(header.name, "sec-websocket-accept")) {
                ++accepts;
                if (header.value != detail::accept_key(key))
                    throw protocol_error{"incorrect WebSocket accept"};
            }
            if (http::iequals(header.name, "sec-websocket-extensions")
                || http::iequals(header.name, "sec-websocket-protocol"))
                throw protocol_error{"unsolicited WebSocket negotiation"};
        }
        if (head.version != "HTTP/1.1" || head.status != 101 || !upgrade
            || !connection || accepts != 1)
            throw protocol_error{"invalid WebSocket Upgrade response"};
    }

    options config_;
    std::array<std::byte, 16384> tx_{}, rx_{};
    std::unique_ptr<net::socket> socket_;
    std::unique_ptr<tls::tls13_client_session> tls_;
    std::string fragments_;
    std::optional<message_type> fragment_type_;
    bool busy_ = false, close_sent_ = false, close_received_ = false;
};

/// URL restrictions match NXT HTTP: DNS/IPv4 authorities only, no userinfo,
/// fragments or bracketed IPv6; queries must follow '/'. Upgrade head is
/// bounded by a 16 KiB input buffer. DNS and all I/O use the current deck.
inline task<std::unique_ptr<client>>
connect(std::string url, options config)
{
    throw_if_stop_requested();
    auto parsed = detail::parse_url(url);
    auto result = std::unique_ptr<client>{new client(std::move(config))};
    co_await result->upgrade(std::move(parsed));
    co_return result;
}

} // namespace nxtrt::websocket
