#include <nxtio/llm.hpp>

#include <boost/ut.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nxt::test {

using namespace boost::ut;

suite llm_tests = [] {
    using namespace std::literals;

    "openai responses request serializes response body"_test = [] {
        auto request = nxt::io::llm::openai_responses_request{
            .api_key = "test-key",
            .model = "gpt-5-mini",
            .input = "Say ok.",
            .input_items = nlohmann::json::array(),
            .tools = nlohmann::json::array(),
            .include = nlohmann::json::array(),
            .previous_response_id = {},
            .max_output_tokens = 64,
            .reasoning_effort = "minimal",
            .reasoning_summary = "auto",
            .store = false,
        };

        auto wire =
            nxt::http::serialize(
                nxt::io::llm::openai_responses_http_request(request));
        auto body_start = wire.find("\r\n\r\n");
        expect(body_start != std::string::npos);

        auto body = nlohmann::json::parse(wire.substr(body_start + 4));
        expect(body["model"] == "gpt-5-mini");
        expect(body["input"] == "Say ok.");
        expect(body["stream"] == true);
        expect(body["store"] == false);
        expect(body["reasoning"]["effort"] == "minimal");
        expect(body["reasoning"]["summary"] == "auto");
        expect(wire.find("Authorization: Bearer test-key\r\n") != std::string::npos);
    };

    "openai responses request serializes tools and function outputs"_test = [] {
        auto request = nxt::io::llm::openai_responses_request{
            .api_key = "test-key",
            .model = "gpt-5-mini",
            .input = "ignored for tool output turn",
            .input_items =
                nlohmann::json::array({
                    nxt::io::llm::function_call_output(
                        "call_123",
                        "{\"ok\":true}")}),
            .tools =
                nlohmann::json::array({
                    {
                        {"type", "function"},
                        {"name", "nxt_echo"},
                        {"description", "Echo text."},
                        {"parameters",
                         {
                             {"type", "object"},
                             {"properties",
                              {
                                  {"text", {{"type", "string"}}},
                              }},
                             {"required", {"text"}},
                             {"additionalProperties", false},
                         }},
                        {"strict", true},
                    },
                }),
            .include = nlohmann::json::array({"reasoning.encrypted_content"}),
            .previous_response_id = "resp_123",
            .max_output_tokens = 64,
            .reasoning_summary = "",
            .store = true,
        };

        auto body = nxt::io::llm::openai_responses_body(request);
        expect(body["input"].is_array());
        expect(body["input"][0]["type"] == "function_call_output");
        expect(body["input"][0]["call_id"] == "call_123");
        expect(body["tools"][0]["name"] == "nxt_echo");
        expect(body["include"][0] == "reasoning.encrypted_content");
        expect(body["previous_response_id"] == "resp_123");
        expect(body["store"] == true);
    };

    "openai responses request builds stateless tool input history"_test = [] {
        auto request = nxt::io::llm::openai_responses_request{
            .api_key = "test-key",
            .model = "gpt-5-mini",
            .input = "Use a tool.",
            .input_items = nlohmann::json::array(),
            .tools = nlohmann::json::array(),
            .include = nlohmann::json::array({"reasoning.encrypted_content"}),
            .previous_response_id = {},
            .max_output_tokens = 64,
            .reasoning_summary = "",
            .store = false,
        };

        auto input = nxt::io::llm::input_items_from_request(request);
        input.push_back({
            {"id", "fc_123"},
            {"type", "function_call"},
            {"call_id", "call_123"},
            {"name", "nxt_echo"},
            {"arguments", "{\"text\":\"hello\"}"},
        });
        input.push_back(nxt::io::llm::function_call_output(
            "call_123",
            "{\"text\":\"hello\"}"));

        request.input.clear();
        request.input_items = std::move(input);
        auto body = nxt::io::llm::openai_responses_body(request);
        expect(body["store"] == false);
        expect(body["include"][0] == "reasoning.encrypted_content");
        expect(!body.contains("previous_response_id"));
        expect(body["input"].is_array());
        expect(body["input"][0]["role"] == "user");
        expect(body["input"][1]["type"] == "function_call");
        expect(body["input"][2]["type"] == "function_call_output");
    };

    "function call item parses and runs matching tool"_test = [] {
        auto item = nlohmann::json{
            {"id", "fc_123"},
            {"type", "function_call"},
            {"call_id", "call_123"},
            {"name", "nxt_echo"},
            {"arguments", "{\"text\":\"hello\"}"},
        };
        auto call = nxt::io::llm::function_call_from_item(item);
        expect(call.has_value());
        expect(call->call_id == "call_123");
        expect(call->name == "nxt_echo");

        auto tools = std::vector<nxt::io::llm::function_tool>{};
        tools.push_back({
            .name = "nxt_echo",
            .description = "Echo text.",
            .parameters = nlohmann::json::object(),
            .strict = true,
            .run = [](const nlohmann::json & args) -> nxt::task<std::string> {
                co_return args.value("text", std::string{});
            },
        });

        auto output =
            nxt::sync_wait(nxt::io::llm::run_function_tool(tools, *call));
        expect(output == "hello");

        auto missing = *call;
        missing.name = "missing";
        auto error =
            nxt::sync_wait(nxt::io::llm::run_function_tool(tools, missing));
        expect(nlohmann::json::parse(error)["error"] == "unknown tool");
    };

    "openai responses stream emits parsed sse json events"_test = [] {
        auto sse =
            "event: response.output_text.delta\n"
            "data: {\"type\":\"response.output_text.delta\",\"delta\":\"Ok\"}\n"
            "\n"
            "event: response.completed\n"
            "data: {\"type\":\"response.completed\",\"response\":{\"status\":\"completed\"}}\n"
            "\n"s;

        auto response =
            "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
            "Content-Length: "
            + std::to_string(sse.size()) + "\r\n\r\n" + sse;
        auto chunks = std::array{std::string_view{response}};
        nxt::io::string_transport transport{std::span{chunks}};

        std::vector<nxt::io::llm::stream_event> events;
        auto request = nxt::io::llm::openai_responses_request{
            .api_key = "test-key",
            .model = "gpt-5-mini",
            .input = "Say ok.",
            .input_items = nlohmann::json::array(),
            .tools = nlohmann::json::array(),
            .include = nlohmann::json::array(),
            .previous_response_id = {},
            .max_output_tokens = 64,
            .reasoning_summary = "",
        };

        nxt::sync_wait([&]() -> nxt::task<> {
            auto stream = nxt::io::llm::openai_response_stream<
                nxt::io::string_transport>{transport};
            co_await stream.connect(request);
            while (auto event = co_await stream.next())
                events.push_back(std::move(*event));
        }());

        expect(events.size() == 2_ul);
        expect(events[0].type == "response.output_text.delta");
        expect(events[0].payload["delta"] == "Ok");
        expect(events[1].type == "response.completed");
        expect(transport.written().starts_with("POST /v1/responses HTTP/1.1\r\n"));
    };
};

} // namespace nxt::test

int main()
{
    using namespace boost::ut;
    return cfg<override>.run({.report_errors = true});
}
