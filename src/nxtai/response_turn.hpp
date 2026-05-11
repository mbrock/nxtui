#pragma once

#include "nxt/tui.hpp"
#include <nxtai/agent.hpp>
#include <nxtai/responses.hpp>
#include <nxtai/tools.hpp>
#include <nxtio/async.hpp>
#include <nxtio/net.hpp>
#include <nxtio/process.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <exception>
#include <format>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nxt::ai::response_turn {

using function_call = tools::function_call;
using function_tool = tools::function_tool;
using llm_request = responses::openai_responses_request;
using output_item_result = agent::output_item_result;
using response_stream_result = agent::response_stream_result;
using stream_event = responses::stream_event;

inline nxt::io::net::endpoint openai_responses_endpoint()
{
    return nxt::io::net::endpoint{
        .host = "api.openai.com",
        .port = 443,
    };
}

inline std::size_t stream_wrap_width(nxt::ui::yard & self)
{
    auto columns = self.runtime().terminal_width().count();
    if (columns > 32)
        return columns - 8;
    return std::max<std::size_t>(1, columns);
}

inline void for_complete_words(std::string & text, bool finish, auto fn)
{
    auto n =
        finish ? text.size() : nxt::utf8::complete_words_prefix_size(text);
    auto complete = std::string_view{text}.substr(0, n);
    for (auto segment : nxt::utf8::segments(complete))
        fn(segment);
    text.erase(0, n);
}

template<typename Stream>
auto response_event_source(Stream & stream)
{
    return nxt::make_source<stream_event>(
        [&stream](
            std::stop_token) -> nxt::task<std::optional<stream_event>> {
            co_return co_await stream.next();
        });
}

template<typename Stream>
nxt::task<std::optional<nlohmann::json>> read_text_delta_item(
    Stream & stream,
    nxt::ui::yard & self,
    std::string_view delta_event_type,
    tui::Style style,
    auto on_delta)
{
    auto text = std::string{};
    auto cursor = std::size_t{0};
    auto wrap_width = stream_wrap_width(self);
    auto wrote = false;

    auto print_segment = [&](nxt::utf8::text_segment segment) {
        if (segment.kind == nxt::utf8::text_segment::kind_t::line_break) {
            self.print("\n");
            cursor = 0;
            wrote = true;
            return;
        }

        auto word_width = segment.width.count();
        if (cursor > 0 && cursor + word_width > wrap_width) {
            self.print("\n");
            cursor = 0;
        }
        self.print(tui::text(std::string(segment.text), style));
        self.print(" ");
        cursor += word_width + 1;
        wrote = true;
    };

    while (auto event = co_await nxt::next(stream)) {
        if (agent::is_event(*event, delta_event_type)) {
            auto delta = event->payload.value("delta", std::string{});
            if (!delta.empty()) {
                text += delta;
                for_complete_words(text, false, print_segment);
                on_delta(std::string_view{delta});
            }
            continue;
        }

        if (agent::is_event(*event, "response.output_item.done")) {
            auto item = agent::output_item_from_event(*event);
            for_complete_words(text, true, print_segment);
            self.print("\n");
            co_return item;
        }
    }

    for_complete_words(text, true, print_segment);
    if (wrote) {
        self.print("\n");
    }

    co_return std::nullopt;
}

template<typename Stream>
nxt::task<std::optional<nlohmann::json>>
read_reasoning_item(Stream & stream, nxt::ui::yard & self)
{
    co_return co_await read_text_delta_item(
        stream,
        self,
        "response.reasoning_summary_text.delta",
        nxt::tui::fg(nxt::Rgba8::cyan()),
        [&](std::string_view delta) {
            self.draw(nxt::tui::styled_text(
                nxt::tui::span(
                    "[" + std::string{delta} + "]",
                    nxt::tui::fg(nxt::Rgba8::cyan()))));
        });
}

template<typename Stream>
nxt::task<std::optional<nlohmann::json>>
read_message_item(Stream & stream, nxt::ui::yard & self)
{
    co_return co_await read_text_delta_item(
        stream,
        self,
        "response.output_text.delta",
        nxt::tui::fg(nxt::Rgba8::yellow()),
        [&](std::string_view delta) {
            self.draw(nxt::tui::styled_text(
                nxt::tui::span(
                    "(" + std::string{delta} + ")",
                    nxt::tui::fg(nxt::Rgba8::yellow()))));
        });
}

template<typename Stream>
nxt::task<output_item_result> read_output_item(
    Stream & stream,
    nxt::ui::yard & self,
    const stream_event & first)
{
    auto type = agent::output_item_type(first);

    if (type == "reasoning") {
        co_return output_item_result{
            .call = std::nullopt,
            .item = co_await read_reasoning_item(stream, self),
        };
    }

    if (type == "message") {
        co_return output_item_result{
            .call = std::nullopt,
            .item = co_await read_message_item(stream, self),
        };
    }

    if (type == "function_call") {
        auto item = agent::output_item_from_event(first);
        while (auto event = co_await nxt::next(stream)) {
            if (agent::is_event(*event, "response.output_item.done")) {
                if (auto done_item = agent::output_item_from_event(*event))
                    item = std::move(*done_item);
                break;
            }
        }

        auto call = item ? tools::function_call_from_item(*item)
                         : std::optional<function_call>{};
        co_return output_item_result{
            .call = std::move(call),
            .item = std::move(item),
        };
    }

    self.draw(nxt::tui::text("Reading item..."));

    auto item = agent::output_item_from_event(first);
    while (auto event = co_await nxt::next(stream)) {
        if (agent::is_event(*event, "response.output_item.done")) {
            self.println(" [✓]");
            self.println("");
            if (auto done_item = agent::output_item_from_event(*event))
                item = std::move(*done_item);
            break;
        }
    }

    co_return output_item_result{
        .call = std::nullopt,
        .item = std::move(item),
    };
}

template<typename ReadStream>
nxt::task<response_stream_result> with_openai_response_stream(
    nxt::ui::yard & self,
    const llm_request & request,
    ReadStream read_stream)
{
    auto & runtime = self.runtime();
    auto transport = co_await nxt::io::net::connect_tls(
        runtime.scheduler_handle(), openai_responses_endpoint());

    using transport_t = decltype(transport);
    auto stream = responses::openai_response_stream<transport_t>{
        transport, runtime.get_stop_token()};

    auto result = response_stream_result{};
    auto failure = std::exception_ptr{};
    try {
        co_await stream.connect(request);
        result = co_await read_stream(stream);
    } catch (...) {
        if (!runtime.shutdown_requested())
            failure = std::current_exception();
    }

    co_await transport.shutdown();
    if (failure)
        std::rethrow_exception(failure);
    co_return result;
}

template<typename Stream>
nxt::task<response_stream_result>
read_openai_response_stream(nxt::ui::yard & self, Stream & stream)
{
    self.draw(nxt::tui::text("Streaming..."));

    auto events = response_event_source(stream);
    auto result = response_stream_result{};

    while (auto event = co_await nxt::next(events)) {
        if (auto response_id = responses::response_id_from_event(*event))
            result.response_id = std::move(*response_id);

        if (agent::is_event(*event, "response.output_item.added")) {
            auto output_item =
                co_await read_output_item(events, self, *event);

            self.print("\n\n");

            if (output_item.item)
                result.output_items.push_back(std::move(*output_item.item));

            if (output_item.call)
                result.function_calls.push_back(std::move(*output_item.call));

            continue;
        }

        if (agent::is_event(*event, "response.completed")) {
            result.completed = true;
            co_return result;
        }

        if (agent::is_event(*event, "response.failed")
            || agent::is_event(*event, "response.incomplete")) {
            co_return result;
        }
    }

    result.completed = true;
    co_return result;
  }

inline nxt::task<response_stream_result> request_response_turn(
    nxt::ui::yard & self, const llm_request & request)
{
    self.draw(nxt::tui::text("Dialing OpenAI.com..."));
    co_return co_await with_openai_response_stream(
        self,
        request,
        [&](auto & stream) -> nxt::task<response_stream_result> {
            co_return co_await read_openai_response_stream(self, stream);
        });
}

inline nxt::task<std::vector<nlohmann::json>> run_requested_tools(
    nxt::ui::yard & self,
    const std::vector<function_tool> & tool_list,
    const std::vector<function_call> & calls)
{
    auto outputs = std::vector<nlohmann::json>{};
    outputs.reserve(calls.size());

    for (const auto & call : calls) {
        self.println(
            std::format("tool: {}({})", call.name, call.arguments));

        auto output =
            co_await tools::run_function_tool(tool_list, call);
        self.println(std::format("tool result: {}", output));
        outputs.push_back(
            tools::function_call_output(call.call_id, std::move(output)));
    }

    co_return outputs;
}

} // namespace nxt::ai::response_turn
