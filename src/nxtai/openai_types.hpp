#pragma once

#include <string>
#include <vector>

namespace nxtai::openai {

struct raw_json
{
    std::string str;
};

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

struct reasoning_summary_text_delta_event
{
    std::string type;
    std::string delta;
};

struct reasoning_summary_text_done_event
{
    std::string type;
    std::string text;
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

struct function_tool_definition
{
    std::string type = "function";
    std::string name;
    std::string description;
    raw_json parameters;
    bool strict = true;
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

} // namespace nxtai::openai
