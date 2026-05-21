#pragma once

#include <glaze/glaze.hpp>

#include <string>
#include <vector>

namespace nxt::ai::openai {

using raw_json = glz::raw_json;

inline constexpr auto json_read_opts =
    glz::opts{.error_on_unknown_keys = false};

struct response_ref
{
    std::string id;
    std::string status;
};

struct response_event
{
    std::string type;
    response_ref response;
};

struct output_item_event
{
    std::string type;
    raw_json item;
};

struct text_delta_event
{
    std::string type;
    std::string delta;
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

struct message_content_part
{
    std::string type;
    std::string text;
};

struct message_item
{
    std::string id;
    std::string type;
    std::string role;
    std::string status;
    std::vector<message_content_part> content;
};

} // namespace nxt::ai::openai
