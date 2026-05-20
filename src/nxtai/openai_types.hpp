#pragma once

#include <glaze/glaze.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace nxt::ai::openai {

using raw_json = glz::raw_json;

inline constexpr auto json_read_opts =
    glz::opts{.error_on_unknown_keys = false};

struct response_ref
{
    std::string id;
    std::string status;
};

struct reasoning_item
{
    std::string id;
    std::string type;
    raw_json summary;
};

struct function_call_item
{
    std::string id;
    std::string type;
    std::string call_id;
    std::string name;
    std::string arguments;
    std::string status;
};

struct message_item
{
    std::string id;
    std::string type;
    std::string role;
    std::string status;
    raw_json content;
};

using response_output_item =
    std::variant<function_call_item, message_item, reasoning_item>;

struct response_output_item_header
{
    std::string type;
};

struct response_event_payload
{
    std::string type;
    std::int64_t sequence_number = -1;
    std::int64_t output_index = -1;
    std::int64_t content_index = -1;
    std::int64_t summary_index = -1;
    std::string item_id;
    std::string response_id;
    std::string delta;
    std::string text;
    std::string arguments;
    raw_json item;
    raw_json part;
    raw_json response;
};

[[nodiscard]] inline bool has_json(const raw_json & raw) noexcept
{
    return !raw.str.empty();
}

[[nodiscard]] inline glz::error_ctx
read_json(auto & out, std::string_view json)
{
    return glz::read<json_read_opts>(out, json);
}

[[nodiscard]] inline glz::expected<std::string, glz::error_ctx>
write_json(auto && value)
{
    return glz::write_json(std::forward<decltype(value)>(value));
}

[[nodiscard]] inline response_event_payload
parse_response_event_payload(std::string_view json)
{
    auto payload = response_event_payload{};
    if (auto ec = read_json(payload, json))
        throw std::runtime_error{glz::format_error(ec, json)};
    return payload;
}

[[nodiscard]] inline response_output_item
parse_response_output_item(const raw_json & raw)
{
    auto item = response_output_item{};
    if (auto ec = read_json(item, raw.str))
        throw std::runtime_error{glz::format_error(ec, raw.str)};
    return item;
}

[[nodiscard]] inline std::string output_item_type(const raw_json & raw)
{
    auto item = response_output_item_header{};
    if (!has_json(raw))
        return {};
    if (auto ec = read_json(item, raw.str))
        throw std::runtime_error{glz::format_error(ec, raw.str)};
    return item.type;
}

[[nodiscard]] inline response_ref
parse_response_ref(const raw_json & raw)
{
    auto response = response_ref{};
    if (!has_json(raw))
        return response;
    if (auto ec = read_json(response, raw.str))
        throw std::runtime_error{glz::format_error(ec, raw.str)};
    return response;
}

} // namespace nxt::ai::openai
