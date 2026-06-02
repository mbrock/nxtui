#include <nxtrt/app.hpp>
#include <nxtrt/mtproto.hpp>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace {

std::string hex(std::span<const std::byte> bytes)
{
    constexpr auto digits = std::string_view{"0123456789abcdef"};
    auto out = std::string{};
    out.reserve(bytes.size() * 2);
    for (auto byte : bytes) {
        auto value = std::to_integer<unsigned>(byte);
        out.push_back(digits[value >> 4]);
        out.push_back(digits[value & 0x0f]);
    }
    return out;
}

std::string hex32(std::uint32_t value)
{
    auto out = std::ostringstream{};
    out << std::hex << std::setfill('0') << std::setw(8) << value;
    return out.str();
}

struct options
{
    std::string command = "auth";
    std::optional<std::string> host;
    std::string service = "443";
    std::optional<std::filesystem::path> env_path;
    std::int32_t dc = 2;
    bool dc_explicit = false;
    std::int32_t dialogs_limit = 20;
    std::int32_t listen_frames = 1;
    std::optional<nxt::mt::telegram::update_state> update_cursor;
    bool followed_migration = false;
};

void set_cursor_field(
    std::optional<nxt::mt::telegram::update_state> & cursor,
    std::string_view name,
    std::int32_t value)
{
    if (!cursor)
        cursor = nxt::mt::telegram::update_state{};
    if (name == "pts")
        cursor->pts = value;
    else if (name == "date")
        cursor->date = value;
    else if (name == "qts")
        cursor->qts = value;
}

std::string host_for_dc(std::int32_t dc)
{
    switch (dc) {
    case 1: return "149.154.175.51";
    case 2: return "149.154.167.50";
    case 3: return "149.154.175.100";
    case 4: return "149.154.167.92";
    case 5: return "91.108.56.149";
    default: throw nxtrt::runtime_error{"unknown Telegram DC"};
    }
}

std::string trim(std::string_view text)
{
    while (!text.empty()
           && (text.front() == ' ' || text.front() == '\t'
               || text.front() == '\r' || text.front() == '\n'))
        text.remove_prefix(1);
    while (!text.empty()
           && (text.back() == ' ' || text.back() == '\t'
               || text.back() == '\r' || text.back() == '\n'))
        text.remove_suffix(1);
    return std::string{text};
}

std::map<std::string, std::string> read_env_file(const std::filesystem::path & path)
{
    auto input = std::ifstream{path};
    if (!input)
        return {};

    auto out = std::map<std::string, std::string>{};
    auto line = std::string{};
    while (std::getline(input, line)) {
        line = trim(line);
        auto view = std::string_view{line};
        if (view.empty() || view.front() == '#')
            continue;
        auto equals = view.find('=');
        if (equals == std::string_view::npos)
            continue;
        auto key = trim(view.substr(0, equals));
        auto value = trim(view.substr(equals + 1));
        if (value.size() >= 2
            && ((value.front() == '"' && value.back() == '"')
                || (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }
        out.emplace(std::move(key), std::move(value));
    }
    return out;
}

std::map<std::string, std::string> load_env(options opts)
{
    if (opts.env_path)
        return read_env_file(*opts.env_path);
    return read_env_file(".env");
}

std::optional<std::string> env_value(
    const std::map<std::string, std::string> & file_env,
    std::string_view name)
{
    if (auto * value = std::getenv(std::string{name}.c_str());
        value != nullptr && std::string_view{value} != "") {
        return value;
    }
    auto found = file_env.find(std::string{name});
    if (found == file_env.end() || found->second.empty())
        return std::nullopt;
    return found->second;
}

std::string require_env(
    const std::map<std::string, std::string> & file_env,
    std::string_view primary,
    std::string_view fallback = {})
{
    if (auto value = env_value(file_env, primary))
        return *value;
    if (!fallback.empty()) {
        if (auto value = env_value(file_env, fallback))
            return *value;
    }
    throw nxtrt::runtime_error{
        "set " + std::string{primary}
        + (fallback.empty() ? std::string{} : " or " + std::string{fallback})};
}

std::int32_t require_i32_env(
    const std::map<std::string, std::string> & file_env,
    std::string_view primary,
    std::string_view fallback = {})
{
    auto text = require_env(file_env, primary, fallback);
    auto value = std::int32_t{};
    auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        throw nxtrt::runtime_error{"invalid integer in " + std::string{primary}};
    return value;
}

std::optional<std::int32_t> i32_env(
    const std::map<std::string, std::string> & file_env,
    std::string_view primary,
    std::string_view fallback = {})
{
    auto text = env_value(file_env, primary);
    if (!text && !fallback.empty())
        text = env_value(file_env, fallback);
    if (!text)
        return std::nullopt;

    auto value = std::int32_t{};
    auto result = std::from_chars(
        text->data(),
        text->data() + text->size(),
        value);
    if (result.ec != std::errc{} || result.ptr != text->data() + text->size())
        throw nxtrt::runtime_error{"invalid integer in " + std::string{primary}};
    return value;
}

options parse_args(int argc, char ** argv)
{
    auto out = options{};
    for (auto i = 1; i < argc; i++) {
        auto arg = std::string_view{argv[i]};
        if ((arg == "--host" || arg == "-h") && i + 1 < argc) {
            out.host = argv[++i];
        } else if ((arg == "--port" || arg == "-p") && i + 1 < argc) {
            out.service = argv[++i];
        } else if (arg == "--env" && i + 1 < argc) {
            out.env_path = argv[++i];
        } else if (arg == "--dc" && i + 1 < argc) {
            out.dc = std::stoi(argv[++i]);
            out.dc_explicit = true;
        } else if (arg == "--limit" && i + 1 < argc) {
            out.dialogs_limit = std::stoi(argv[++i]);
        } else if (arg == "--frames" && i + 1 < argc) {
            out.listen_frames = std::stoi(argv[++i]);
        } else if (arg == "--pts" && i + 1 < argc) {
            set_cursor_field(out.update_cursor, "pts", std::stoi(argv[++i]));
        } else if (arg == "--date" && i + 1 < argc) {
            set_cursor_field(out.update_cursor, "date", std::stoi(argv[++i]));
        } else if (arg == "--qts" && i + 1 < argc) {
            set_cursor_field(out.update_cursor, "qts", std::stoi(argv[++i]));
        } else if (arg == "auth" || arg == "config" || arg == "bot-updates"
                   || arg == "bot-dialogs" || arg == "bot-listen") {
            out.command = arg;
        } else {
            throw nxtrt::runtime_error{
                "usage: nxtmt [auth|config|bot-updates|bot-dialogs] "
                "[--dc N] [--host HOST] [--port PORT] [--env PATH] "
                "[--limit N] [--frames N] [--pts N] [--date N] [--qts N]"};
        }
    }
    return out;
}

nxt::mt::telegram::app_info app_from_env(
    const std::map<std::string, std::string> & file_env)
{
    return nxt::mt::telegram::app_info{
        .api_id = require_i32_env(file_env, "TELEGRAM_API_ID", "TDLIB_API_ID"),
        .device_model = "nxtmt",
        .system_version = "nxt",
        .app_version = "0.1.0",
        .system_lang_code = "en",
        .lang_pack = "",
        .lang_code = "en",
        .layer = nxt::mt::telegram::current_layer,
    };
}

void print_result(std::string_view label, std::span<const std::byte> result)
{
    auto constructor = nxt::mt::telegram::result_constructor(result);
    std::cout << label << ": "
              << nxt::mt::telegram::constructor_name(constructor)
              << " constructor=0x" << hex32(constructor)
              << " bytes=" << result.size() << '\n';
    if (constructor == nxt::mt::telegram::rpc_error_constructor) {
        auto error = nxt::mt::telegram::read_rpc_error(result);
        std::cout << label << " rpc_error: code=" << error.code
                  << " message=" << error.message << '\n';
    }
}

void print_difference_summary(std::span<const std::byte> result)
{
    auto summary = nxt::mt::telegram::summarize_updates_difference(result);
    if (summary.constructor
        == nxt::mt::telegram::updates_difference_empty_constructor) {
        std::cout << "difference empty: date=" << summary.date
                  << " seq=" << summary.seq << '\n';
    } else if (summary.constructor
               == nxt::mt::telegram::updates_difference_too_long_constructor) {
        std::cout << "difference too long: pts=" << summary.pts << '\n';
    } else if (summary.first_vector_count) {
        std::cout << "difference first vector count="
                  << *summary.first_vector_count << '\n';
    }
}

void print_session_event(const nxt::mt::session_event & event)
{
    if (auto * received =
            std::get_if<nxt::mt::session_message_received>(&event)) {
        auto constructor = std::uint32_t{};
        if (received->body.size() >= 4)
            constructor = nxt::mt::telegram::result_constructor(received->body);
        std::cout << "session message received: msg_id="
                  << received->message_id
                  << " seq_no=" << received->seq_no
                  << " body_bytes=" << received->body.size()
                  << " constructor=0x" << hex32(constructor)
                  << " name="
                  << nxt::mt::telegram::constructor_name(constructor)
                  << '\n';
    } else if (auto * unhandled =
                   std::get_if<nxt::mt::unhandled_session_message>(&event)) {
        auto constructor = std::uint32_t{};
        if (unhandled->body.size() >= 4)
            constructor = nxt::mt::telegram::result_constructor(unhandled->body);
        std::cout << "unhandled session message: msg_id="
                  << unhandled->message_id
                  << " seq_no=" << unhandled->seq_no
                  << " body_bytes=" << unhandled->body.size()
                  << " constructor=0x" << hex32(constructor)
                  << " name="
                  << nxt::mt::telegram::constructor_name(constructor)
                  << '\n';
    } else if (auto * ack = std::get_if<nxt::mt::msgs_ack_sent>(&event)) {
        std::cout << "msgs_ack sent: msg_id=" << ack->message_id
                  << " count=" << ack->message_ids.size() << '\n';
    } else if (auto * created =
                   std::get_if<nxt::mt::new_session_created>(&event)) {
        std::cout << "new session: first_msg_id=" << created->first_msg_id
                  << " server_salt=" << created->server_salt << '\n';
    }
}

void print_session_effects(const std::vector<nxt::mt::session_effect> & effects)
{
    for (const auto & effect : effects) {
        auto note = std::get_if<nxt::mt::notify_session>(&effect);
        if (note != nullptr)
            print_session_event(note->event);
    }
}

nxtrt::task<nxt::mt::bytes> invoke_wrapped(
    nxtrt::bytesink & writer,
    nxtrt::bytefeed & reader,
    nxt::mt::session & session,
    std::span<const std::byte> body,
    const nxt::mt::telegram::app_info & app,
    std::string request_name)
{
    auto query = nxt::mt::telegram::wrap_request(body, app);
    co_return co_await nxtrt::mtproto::invoke_raw(
        writer,
        reader,
        session,
        std::move(query),
        std::move(request_name));
}

nxtrt::task<nxt::mt::bytes> import_bot(
    nxtrt::bytesink & writer,
    nxtrt::bytefeed & reader,
    nxt::mt::session & session,
    const nxt::mt::telegram::app_info & app,
    const std::map<std::string, std::string> & file_env)
{
    auto query = nxt::mt::telegram::auth_import_bot_authorization(
        app.api_id,
        require_env(file_env, "TELEGRAM_API_HASH", "TDLIB_API_HASH"),
        require_env(file_env, "TELEGRAM_BOT_TOKEN"));
    auto result = co_await invoke_wrapped(
        writer,
        reader,
        session,
        query,
        app,
        "auth.importBotAuthorization");
    print_result("bot auth", result);
    co_return result;
}

nxtrt::task<int> run_nxtmt(options opts)
{
    auto file_env = load_env(opts);
    if (!opts.dc_explicit) {
        if (auto dc = i32_env(file_env, "NXTMT_DC", "TELEGRAM_DC"))
            opts.dc = *dc;
    }

    while (true) {
        auto tx_buffer = std::array<std::byte, 4096>{};
        auto rx_buffer = std::array<std::byte, 8192>{};
        auto host = opts.host.value_or(host_for_dc(opts.dc));
        auto socket = nxtrt::net::socket{
            co_await nxtrt::net::connect_tcp(
                std::move(host),
                std::move(opts.service)),
            std::span{tx_buffer},
            std::span{rx_buffer},
        };

        co_await nxtrt::mtproto::write_abridged_client_prefix(socket.output());
        auto auth =
            co_await nxtrt::mtproto::perform_auth(socket.output(), socket.input());
        auto session = nxtrt::mtproto::make_session(std::move(auth));

        std::cout << "MTProto auth ok\n";
        std::cout << "auth_key_id=" << hex(session.key.id) << '\n';
        std::cout << "server_salt=" << session.server_salt << '\n';
        std::cout << "session_id=" << session.session_id << '\n';
        std::cout << "time_offset=" << session.time_offset << '\n';

        if (opts.command == "auth")
            co_return EXIT_SUCCESS;

        auto app = app_from_env(file_env);
        if (opts.command == "config") {
            auto query = nxt::mt::telegram::help_get_config();
            auto result = co_await invoke_wrapped(
                socket.output(),
                socket.input(),
                session,
                query,
                app,
                "help.getConfig");
            print_result("help.getConfig", result);
            co_return EXIT_SUCCESS;
        }

        auto bot_auth = co_await import_bot(
            socket.output(),
            socket.input(),
            session,
            app,
            file_env);
        auto bot_auth_constructor =
            nxt::mt::telegram::result_constructor(bot_auth);
        if (bot_auth_constructor == nxt::mt::telegram::rpc_error_constructor) {
            auto error = nxt::mt::telegram::read_rpc_error(bot_auth);
            auto migrate_dc =
                nxt::mt::telegram::migrate_dc_from_error(error.message);
            if (migrate_dc && !opts.host && !opts.followed_migration
                && *migrate_dc != opts.dc) {
                std::cout << "following Telegram migration to DC" << *migrate_dc
                          << '\n';
                opts.dc = *migrate_dc;
                opts.followed_migration = true;
                continue;
            }
            throw nxtrt::runtime_error{
                "bot authorization failed: " + std::to_string(error.code) + " "
                + error.message};
        }
        if (bot_auth_constructor
            != nxt::mt::telegram::auth_authorization_constructor) {
            throw nxtrt::runtime_error{
                "bot authorization returned unexpected TL object"};
        }

        if (opts.command == "bot-dialogs") {
            auto query =
                nxt::mt::telegram::messages_get_dialogs(opts.dialogs_limit);
            auto result = co_await invoke_wrapped(
                socket.output(),
                socket.input(),
                session,
                query,
                app,
                "messages.getDialogs");
            print_result("messages.getDialogs", result);
            co_return EXIT_SUCCESS;
        }

        if (opts.command == "bot-listen") {
            for (auto i = std::int32_t{0}; i < opts.listen_frames; i++) {
                auto effects = co_await nxtrt::mtproto::receive_next_session_effects(
                    socket.output(),
                    socket.input(),
                    session);
                print_session_effects(effects);
            }
            co_return EXIT_SUCCESS;
        }

        auto state = opts.update_cursor;
        if (!state) {
            auto state_query = nxt::mt::telegram::updates_get_state();
            auto state_result = co_await invoke_wrapped(
                socket.output(),
                socket.input(),
                session,
                state_query,
                app,
                "updates.getState");
            print_result("updates.getState", state_result);
            state = nxt::mt::telegram::read_updates_state(state_result);
            std::cout << "updates state: pts=" << state->pts
                      << " qts=" << state->qts
                      << " date=" << state->date
                      << " seq=" << state->seq
                      << " unread=" << state->unread_count << '\n';
        } else {
            std::cout << "using update cursor: pts=" << state->pts
                      << " qts=" << state->qts
                      << " date=" << state->date << '\n';
        }

        auto diff_query = nxt::mt::telegram::updates_get_difference(*state);
        auto diff_result = co_await invoke_wrapped(
            socket.output(),
            socket.input(),
            session,
            diff_query,
            app,
            "updates.getDifference");
        print_result("updates.getDifference", diff_result);
        if (nxt::mt::telegram::result_constructor(diff_result)
            != nxt::mt::telegram::rpc_error_constructor) {
            print_difference_summary(diff_result);
        }
        break;
    }
    co_return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char ** argv)
{
    try {
        auto rt = nxtrt::runtime{};
        return rt.run(run_nxtmt(parse_args(argc, argv)));
    } catch (const std::exception & error) {
        std::cerr << "nxtmt: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
