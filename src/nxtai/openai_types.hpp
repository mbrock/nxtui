#pragma once

#include <glaze/glaze.hpp>

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace nxt::ai::openai {

using raw_json = glz::raw_json;

inline constexpr auto json_read_opts =
    glz::opts{.error_on_unknown_keys = false};
inline constexpr auto lazy_json_opts =
    glz::opts{.error_on_unknown_keys = false, .null_terminated = false};

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

[[nodiscard]] inline auto lazy_event(std::string_view json)
{
    auto doc = glz::lazy_json<lazy_json_opts>(json);
    if (!doc)
        throw std::runtime_error{glz::format_error(doc.error(), json)};
    return doc;
}

[[nodiscard]] inline auto item_view(auto & doc)
{
    return doc["item"];
}

template<typename T, typename View>
[[nodiscard]] inline T read_view(View view)
{
    auto out = T{};
    if (auto ec = glz::read_json(out, view))
        throw std::runtime_error{glz::format_error(ec, view.raw_json())};
    return out;
}

[[nodiscard]] inline raw_json raw_json_from(auto view)
{
    return raw_json{std::string{view.raw_json()}};
}

[[nodiscard]] inline std::string delta_from_event_data(std::string_view json)
{
    auto doc = lazy_event(json);
    auto delta = (*doc)["delta"];
    if (!delta)
        return {};
    if (auto value = delta.template get<std::string>())
        return std::move(*value);
    return {};
}

[[nodiscard]] inline std::optional<raw_json>
item_from_event_data(std::string_view json)
{
    auto doc = lazy_event(json);
    auto item = item_view(*doc);
    if (!item)
        return std::nullopt;
    return raw_json_from(item);
}

[[nodiscard]] inline std::optional<std::string>
response_id_from_event_data(std::string_view json)
{
    auto doc = lazy_event(json);
    auto response = (*doc)["response"];
    if (response) {
        auto response_id = response["id"];
        if (response_id) {
            if (auto value = response_id.template get<std::string>();
                value && !value->empty())
                return std::move(*value);
        }
    }

    auto id = (*doc)["response_id"];
    if (id) {
        if (auto value = id.template get<std::string>();
            value && !value->empty())
            return std::move(*value);
    }
    return std::nullopt;
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
    if (!has_json(raw))
        return {};
    auto item = response_output_item_header{};
    if (auto ec = read_json(item, raw.str))
        throw std::runtime_error{glz::format_error(ec, raw.str)};
    return item.type;
}

} // namespace nxt::ai::openai
