#pragma once

#include <nxtai/responses_request.hpp>
#include <nxtai/responses_stream.hpp>
#include <nxtrt/websocket.hpp>

namespace nxtai {

struct responses_websocket_options
{
    std::string url = "wss://api.openai.com/v1/responses";
    std::string ca_file = {};
    std::size_t max_message_size = 16 * 1024 * 1024;
};

/// One persistent Responses connection, opened lazily with request.api_key.
/// Uses the default lane, one request at a time. Supply
/// previous_response_id and only new input to continue, even with
/// store=false; run_agent does this automatically. Omitting the id starts a
/// new chain, not an implicit append.
///
/// Deck-confined and immovable. This transport and the observer must
/// outlive their tasks. Credentials must stay the same for the connection;
/// only a digest is retained after Upgrade. TLS verification is always
/// enabled. Any turn failure (including observer failure or cancellation)
/// drains I/O and discards the connection. Create a new transport to
/// recover: replay full context without an id for store=false, or use a
/// persisted id with store=true. No automatic retry, reconnect, credential
/// loading or logging.
class responses_websocket_transport
{
public:
    static constexpr bool supports_unstored_continuation = true;

    explicit responses_websocket_transport(
        responses_websocket_options options = {})
        : options_(std::move(options))
    {
        if (!options_.url.starts_with("wss://"))
            throw nxtrt::invalid_argument{
                "OpenAI Responses requires wss://"};
    }

    responses_websocket_transport(const responses_websocket_transport &) =
        delete;
    responses_websocket_transport &
    operator=(const responses_websocket_transport &) = delete;

    template<typename Observer>
    nxtrt::task<responses::response_result> operator()(
        responses::openai_responses_request request, Observer & observer)
    {
        nxtrt::throw_if_stop_requested();
        if (busy_)
            throw nxtrt::logic_error{
                "overlapping Responses WebSocket turns"};
        if (failed_)
            throw nxtrt::logic_error{
                "Responses WebSocket session failed; create a new transport"};
        if (request.api_key.empty()
            || request.api_key.find_first_of("\r\n") != std::string::npos
            || request.api_key.find('\0') != std::string::npos)
            throw nxtrt::invalid_argument{"invalid OpenAI API key"};
        auto credential =
            nxt::crypto::sha256(std::as_bytes(std::span{request.api_key}));
        if (connection_ && credential != credential_)
            throw nxtrt::invalid_argument{
                "Responses WebSocket credentials changed"};
        auto turn = guard{*this};
        if (!connection_) {
            auto config = nxtrt::websocket::options{
                .ca_file = options_.ca_file,
                .max_message_size = options_.max_message_size,
                .headers = {
                    {"Authorization", "Bearer " + request.api_key},
                    {"User-Agent", "nxtllm/0"}}};
            connection_ = co_await nxtrt::websocket::connect(
                options_.url, std::move(config));
            credential_ = credential;
        }
        co_await connection_->send(
            nxtrt::websocket::message_type::text,
            responses::openai_responses_body(request, true));
        auto decoder = responses::stream_decoder{};
        while (auto event = co_await connection_->receive()) {
            if (event->type == nxtrt::websocket::message_type::pong)
                continue;
            if (event->type == nxtrt::websocket::message_type::close)
                break;
            if (event->type != nxtrt::websocket::message_type::text)
                throw nxtrt::runtime_error{
                    "OpenAI Responses expected WebSocket text event"};
            auto type = tools::json_string_member(event->data, "type");
            if (!type)
                throw nxtrt::runtime_error{
                    "Responses WebSocket event missing type"};
            if (auto delta = co_await decoder.accept(*type, event->data))
                co_await observer.text(std::move(*delta));
            if (decoder.completed) {
                turn.complete = true;
                co_return decoder.finish();
            }
        }
        co_return decoder.finish(); // Premature Close is never completion.
    }

private:
    struct guard
    {
        responses_websocket_transport & owner;
        bool complete = false;

        explicit guard(responses_websocket_transport & owner)
            : owner(owner)
        {
            owner.busy_ = true;
        }

        ~guard()
        {
            owner.busy_ = false;
            if (!complete) {
                owner.connection_.reset();
                owner.failed_ = true;
            }
        }
    };

    responses_websocket_options options_;
    std::unique_ptr<nxtrt::websocket::client> connection_;
    std::array<std::byte, nxt::crypto::sha256_len> credential_{};
    bool busy_ = false, failed_ = false;
};

} // namespace nxtai
