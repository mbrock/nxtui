#pragma once

#include "nxt/rt/app.hpp"
#include "nxt/rt/buffers.hpp"
#include "nxt/rt/stdout_trace.hpp"
#include "nxt/rt/terminal_app.hpp"

#include <nxt/ansi.hpp>
#include <nxt/any_layout.hpp>
#include <nxt/compositor.hpp>
#include <nxt/glyph-table.hpp>
#include <nxt/input.hpp>
#include <nxt/regional-tty.hpp>
#include <nxt/slot.hpp>
#include <nxt/tui.hpp>
#include <nxt/units.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <functional>
#include <poll.h>
#include <source_location>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#include <unistd.h>

namespace nxt::rt {

using widget_slot = nxt::tui::Slot<nxt::tui::AnyLayout>;

[[nodiscard]] inline task<void> write_stdout_all(
    std::string bytes,
    std::source_location where = std::source_location::current())
{
    trace_stdout_write(bytes, where);
    auto stdout_sink = standard_output();
    co_await write_all(stdout_sink, std::move(bytes));
}

struct ui_runtime_options
{
    bool render = true;
    bool hide_cursor = true;
    nxt::Size fallback_size{96 * nxt::ch, 26 * nxt::ln};
};

/// New-runtime UI owner for terminal-guest applications.
///
/// This is the `nxt::rt` successor to the useful part of the old
/// `nxt::ui::UIRuntime`: a live layout surface, a bottom HUD rendered through
/// `TerminalCompositor`, and durable scrollback blocks written above it.
class ui_runtime
{
public:
    explicit ui_runtime(ui_runtime_options options = {})
        : options_(options)
        , terminal_surface_(options.render && ::isatty(STDOUT_FILENO) != 0)
        , raw_(STDIN_FILENO, terminal_surface_ && ::isatty(STDIN_FILENO) != 0)
        , size_(current_terminal_size(options.fallback_size))
        , compositor_(size_, glyphs_)
        , surface_(nxt::tui::AnyLayout{}, [this] { signal_damage(); })
    {
        nxt::ansi::init();
        nxt::ansi::mode = nxt::ansi::Mode::enabled;
    }

    ui_runtime(const ui_runtime &) = delete;
    ui_runtime & operator=(const ui_runtime &) = delete;
    ui_runtime(ui_runtime &&) = delete;
    ui_runtime & operator=(ui_runtime &&) = delete;

    ~ui_runtime()
    {
        if (!cleaned_up_ && terminal_surface_) {
            static constexpr auto fallback_restore =
                std::string_view{"\x1b[r\x1b[0m\x1b[?25h"};
            (void)::write(
                STDOUT_FILENO,
                fallback_restore.data(),
                fallback_restore.size());
        }
    }

    [[nodiscard]] bool has_terminal_surface() const noexcept
    {
        return terminal_surface_;
    }

    [[nodiscard]] nxt::Size terminal_size() const noexcept
    {
        return size_;
    }

    [[nodiscard]] widget_slot surface() const
    {
        return surface_;
    }

    [[nodiscard]] widget_slot make_surface()
    {
        return widget_slot{nxt::tui::AnyLayout{}, [this] {
            signal_damage();
        }};
    }

    [[nodiscard]] event & damage_event() noexcept
    {
        return damage_event_;
    }

    void signal_damage()
    {
        ++damage_generation_;
        damage_event_.set();
    }

    void request_shutdown()
    {
        stopping_ = true;
        damage_event_.set();
    }

    [[nodiscard]] bool stop_requested() const noexcept
    {
        return stopping_;
    }

    template<nxt::tui::Layout Layout>
    void draw(Layout && layout) const
    {
        surface_.publish(
            nxt::tui::AnyLayout{std::forward<Layout>(layout)});
    }

    void print_block(std::string text)
    {
        auto published = commands_.try_publish(
            terminal_command{
                .kind = terminal_command_kind::print_block,
                .text = std::move(text),
            });
        if (published)
            signal_damage();
    }

    template<nxt::tui::Layout Layout>
    void print(Layout && layout)
    {
        print_block(
            render_scrollback_layout(std::forward<Layout>(layout)));
    }

    task<void> run_terminal_owner(
        std::chrono::milliseconds frame_time = std::chrono::milliseconds{16})
    {
        co_await finally(
            terminal_owner_loop(frame_time),
            [this]() -> task<void> {
                co_await cleanup();
            });
    }

    task<void> run_input_owner(deck & d)
    {
        if (!terminal_surface_ || ::isatty(STDIN_FILENO) == 0)
            co_return;

        auto parser = nxt::input::Parser{};
        auto storage = std::array<std::byte, 256>{};

        while (!stop_requested()) {
            co_await op::timeout::after(std::chrono::milliseconds{16});

            while (true) {
                auto n = ::read(
                    STDIN_FILENO,
                    storage.data(),
                    storage.size());
                if (n == 0)
                    break;
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                        break;
                    if (errno == EINTR)
                        continue;
                    co_return;
                }

                auto bytes = std::string_view{
                    reinterpret_cast<const char *>(storage.data()),
                    static_cast<std::size_t>(n)};
                for (auto & event : parser.feed(bytes)) {
                    if (event.is_ctrl_l()) {
                        print_block(d.runtime_dump_text());
                        continue;
                    }
                    if (event.is_ctrl_c() || event.is_ctrl_z()) {
                        request_shutdown();
                        if (auto * zone = current_zone())
                            zone->stop();
                        co_return;
                    }
                }
            }
        }
    }

    task<void> cleanup()
    {
        if (cleaned_up_)
            co_return;
        cleaned_up_ = true;

        auto out = std::ostringstream{};
        flush_output_queue(out);
        if (has_terminal_surface())
            compositor_.set_hud_height(0 * ln, size_.h, out);
        if (has_terminal_surface()) {
            auto restore = std::string{};
            auto w = nxt::ansi::Writer{restore};
            w.save_cursor();
            w.reset_scroll_region();
            w.restore_cursor();
            w.move_to(nxt::Pos{
                nxt::terminal_origin + 0 * nxt::ch,
                nxt::terminal_origin_v + size_.h - 1 * nxt::ln});
            w.clear_line();
            w.reset();
            w.show_cursor();
            out << restore;
        }
        co_await write_stdout_all(out.str());
    }

private:
    enum class terminal_command_kind {
        print_block,
    };

    struct terminal_command
    {
        terminal_command_kind kind{};
        std::string text;
    };

    struct queued_output
    {
        std::string text;
    };

    [[nodiscard]] static bool is_line_blank(std::string_view line) noexcept
    {
        for (auto ch : line)
            if (ch != ' ' && ch != '\t' && ch != '\r')
                return false;
        return true;
    }

    [[nodiscard]] static std::string
    trim_blank_boundary_lines(std::string_view text)
    {
        while (!text.empty()) {
            auto end = text.find('\n');
            auto line =
                end == std::string_view::npos ? text : text.substr(0, end);
            if (!is_line_blank(line))
                break;
            if (end == std::string_view::npos)
                return {};
            text.remove_prefix(end + 1);
        }

        while (!text.empty()) {
            auto end = text.size();
            if (text.back() == '\n')
                --end;
            auto begin = text.rfind('\n', end == 0 ? 0 : end - 1);
            begin = begin == std::string_view::npos ? 0 : begin + 1;
            auto line = text.substr(begin, end - begin);
            if (!is_line_blank(line))
                break;
            text = text.substr(0, begin == 0 ? 0 : begin - 1);
        }

        return std::string{text};
    }

    [[nodiscard]] bool refresh_terminal_size()
    {
        auto next = current_terminal_size(options_.fallback_size);
        if (next.w == size_.w && next.h == size_.h)
            return false;
        size_ = next;
        compositor_.resize(size_);
        return true;
    }

    void flush_output_queue(std::ostream & out)
    {
        auto pending = std::vector<queued_output>{};
        pending.swap(output_queue_);

        for (const auto & output : pending)
            write_output(out, output);
        out.flush();
    }

    [[nodiscard]] std::optional<row_t> query_insertion_cursor() const
    {
        if (!has_terminal_surface())
            return std::nullopt;
        if (terminal_geometry_initialized_)
            return std::nullopt;
        // Cursor position reporting is an in-band terminal protocol response.
        // Do not synchronously poll stdin from the render path; a future
        // terminal owner can observe CPR asynchronously and feed it into the
        // geometry model.
        return std::nullopt;
    }

    template<nxt::tui::Layout Layout>
    [[nodiscard]] std::string render_scrollback_layout(Layout && layout)
    {
        auto height = layout.height_hint().min;
        if (height.count() == 0)
            height = 1 * ln;

        auto raster = nxt::Raster(size_.w, height, glyphs_);
        auto view = raster.view();
        layout.render(view, raster.extent());
        auto out = std::string{};
        // Scrollback cassettes often intentionally paint full-width bands. If
        // autowrap is left enabled, writing the last terminal column followed by
        // a newline can consume an extra row on real terminals.
        out += "\x1b[?7l";
        out += nxt::ansi::render_raster(raster);
        out += "\x1b[?7h";
        return out;
    }

    void write_output(std::ostream & out, const queued_output & output)
    {
        auto block_text = trim_blank_boundary_lines(output.text);
        if (block_text.empty())
            return;

        if (!has_terminal_surface()) {
            out << block_text;
            if (block_text.back() != '\n')
                out << '\n';
            return;
        }

        const auto & partition = compositor_.partition();
        if (partition.hidden()) {
            auto buf = scrollback_.append_block<
                regional_tty::ansi_string_backend>(partition, block_text);
            out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
            return;
        }

        if (!partition.windowed())
            return;

        auto buf = scrollback_.append_block<
            regional_tty::ansi_string_backend>(partition, block_text);
        out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
    }

    task<void> enter_terminal()
    {
        if (terminal_surface_ && options_.hide_cursor)
            co_await write_stdout_all("\x1b[?25l");
    }

    void drain_commands()
    {
        while (auto command = commands_.try_pop()) {
            switch (command->kind) {
            case terminal_command_kind::print_block:
                output_queue_.push_back(
                    queued_output{.text = std::move(command->text)});
                break;
            }
        }
    }

    std::string render_frame_bytes(const widget_slot & layout)
    {
        auto out = std::ostringstream{};
        auto hint = layout.height_hint();
        auto target_h = hint.flex > 0 * one ? size_.h : hint.min;
        target_h = std::min(target_h, size_.h);

        if (hint.flex == 0 * one && target_h > 0 * ln) {
            auto reserved_log_rows = 7 * ln;
            if (size_.h > reserved_log_rows)
                target_h = std::min(target_h, size_.h - reserved_log_rows);
        }

        auto insertion_cursor = target_h > 0 * ln
                                    ? query_insertion_cursor()
                                    : std::optional<row_t>{};
        compositor_.set_hud_height(
            target_h, size_.h, out, insertion_cursor);
        if (target_h > 0 * ln)
            terminal_geometry_initialized_ = true;

        flush_output_queue(out);

        auto & buffer = compositor_.back_buffer();
        buffer.clear();
        auto view = buffer.view();
        layout.render(view, compositor_.size());

        compositor_.present_frame(out);
        flush_output_queue(out);
        return out.str();
    }

    std::string render_output_only_bytes()
    {
        auto out = std::ostringstream{};
        flush_output_queue(out);
        return out.str();
    }

    task<void> terminal_owner_loop(std::chrono::milliseconds frame_time)
    {
        (void)frame_time;
        co_await enter_terminal();
        auto next_size_refresh = std::chrono::steady_clock::now();

        while (!stop_requested()) {
            auto rendered_generation = damage_generation_;
            drain_commands();
            if (has_terminal_surface()) {
                auto now = std::chrono::steady_clock::now();
                if (now >= next_size_refresh) {
                    (void)refresh_terminal_size();
                    next_size_refresh = now + std::chrono::milliseconds{250};
                }
                co_await write_stdout_all(render_frame_bytes(surface_));
            } else {
                co_await write_stdout_all(render_output_only_bytes());
            }

            if (damage_generation_ != rendered_generation)
                continue;
            damage_event_.reset();
            if (damage_generation_ != rendered_generation)
                continue;
            try {
                co_await damage_event_;
            } catch (const operation_cancelled &) {
                co_return;
            }
        }

        drain_commands();
        if (has_terminal_surface()) {
            (void)refresh_terminal_size();
            co_await write_stdout_all(render_frame_bytes(surface_));
        } else {
            co_await write_stdout_all(render_output_only_bytes());
        }
    }

    ui_runtime_options options_;
    bool terminal_surface_ = false;
    raw_terminal_mode raw_;
    nxt::GlyphTable glyphs_;
    nxt::Size size_;
    nxt::ui::TerminalCompositor compositor_;
    widget_slot surface_;
    event damage_event_;
    std::uint64_t damage_generation_ = 0;
    channel<terminal_command> commands_;
    std::vector<queued_output> output_queue_;
    regional_tty::scrollback_append_state scrollback_;
    bool stopping_ = false;
    bool cleaned_up_ = false;
    bool terminal_geometry_initialized_ = false;
};

struct current_ui_runtime_key
{
    using value_type = ui_runtime *;
    static constexpr auto name = "ui-runtime";
};

struct current_widget_slot_key
{
    using value_type = widget_slot;
    static constexpr auto name = "widget-slot";
};

inline ui_runtime * current_ui_runtime() noexcept
{
    auto * value = env_get<current_ui_runtime_key>();
    if (value == nullptr)
        return nullptr;
    return *value;
}

inline ui_runtime & require_current_ui_runtime()
{
    auto * ui = current_ui_runtime();
    if (ui == nullptr)
        throw runtime_error{"nxt::rt ui operation used without ui runtime"};
    return *ui;
}

inline const widget_slot * current_widget_slot() noexcept
{
    return env_get<current_widget_slot_key>();
}

inline const widget_slot & require_current_widget_slot()
{
    auto * slot = current_widget_slot();
    if (slot == nullptr)
        throw runtime_error{"nxt::rt draw used without widget slot"};
    return *slot;
}

[[nodiscard]] inline widget_slot make_widget_slot()
{
    return require_current_ui_runtime().make_surface();
}

class draw_awaiter
{
public:
    explicit draw_awaiter(nxt::tui::AnyLayout layout)
        : layout_(std::move(layout))
    {}

    [[nodiscard]] bool await_ready() const noexcept
    {
        return true;
    }

    void await_suspend(std::coroutine_handle<>) const noexcept {}

    void await_resume()
    {
        throw_if_stop_requested();
        require_current_widget_slot().publish(std::move(layout_));
    }

private:
    nxt::tui::AnyLayout layout_;
};

template<nxt::tui::Layout Layout>
[[nodiscard]] draw_awaiter draw(Layout && layout)
{
    throw_if_stop_requested();
    return draw_awaiter{
        nxt::tui::AnyLayout{std::forward<Layout>(layout)}};
}

inline void clear_widget()
{
    require_current_widget_slot().publish(nxt::tui::AnyLayout{});
}

inline bool has_terminal_surface()
{
    return require_current_ui_runtime().has_terminal_surface();
}

inline void print_block(std::string text)
{
    require_current_ui_runtime().print_block(std::move(text));
}

template<nxt::tui::Layout Layout>
void print(Layout && layout)
{
    require_current_ui_runtime().print(std::forward<Layout>(layout));
}

inline void request_ui_shutdown()
{
    require_current_ui_runtime().request_shutdown();
}

template<typename Fn>
    requires stored_task_factory<std::decay_t<Fn>>
[[nodiscard]] auto with_widget_slot(widget_slot slot, Fn && fn)
{
    return with_env<current_widget_slot_key>(
        std::move(slot), std::forward<Fn>(fn));
}

template<typename T>
class widget_child
{
public:
    widget_child(widget_slot slot, deed<T> child)
        : slot_(std::move(slot))
        , child_(std::move(child))
    {}

    widget_child(const widget_child &) = delete;
    widget_child & operator=(const widget_child &) = delete;
    widget_child(widget_child &&) noexcept = default;
    widget_child & operator=(widget_child &&) noexcept = default;

    [[nodiscard]] const widget_slot & surface() const noexcept
    {
        return slot_;
    }

    [[nodiscard]] deed<T> release() &&
    {
        return std::move(child_);
    }

    [[nodiscard]] catching_deed<T> cope() &&
    {
        return std::move(child_).cope();
    }

private:
    widget_slot slot_;
    deed<T> child_;
};

namespace detail {

template<typename Body>
    requires std::invocable<Body &>
        && is_task_v<std::invoke_result_t<Body &>>
        && std::is_void_v<task_result_t<std::invoke_result_t<Body &>>>
task<void> run_ui_zone_body_child(
    Body & body,
    ui_runtime & ui)
{
    try {
        co_await with_env<current_ui_runtime_key>(
            &ui,
            [&] {
                return with_widget_slot(
                    ui.surface(),
                    [&] {
                        return std::invoke(body);
                    });
            });
    } catch (...) {
        ui.request_shutdown();
        throw;
    }
    ui.request_shutdown();
}

inline task<void> clear_widget_slot_task(widget_slot slot)
{
    slot.publish(nxt::tui::AnyLayout{});
    co_return;
}

template<typename Body>
    requires std::invocable<Body &>
        && is_task_v<std::invoke_result_t<Body &>>
[[nodiscard]] auto run_widget_child(widget_slot child, Body body)
    -> task<task_result_t<std::invoke_result_t<Body &>>>
{
    using result_t = task_result_t<std::invoke_result_t<Body &>>;
    auto work = with_widget_slot(child, [&body] {
        return std::invoke(body);
    });
    auto cleanup = [child] {
        return clear_widget_slot_task(child);
    };

    if constexpr (std::is_void_v<result_t>) {
        co_await finally(std::move(work), std::move(cleanup));
    } else {
        co_return co_await finally(std::move(work), std::move(cleanup));
    }
}

} // namespace detail

template<typename Body>
    requires std::invocable<Body &>
        && is_task_v<std::invoke_result_t<Body &>>
[[nodiscard]] auto spawn_widget(Body body)
{
    using task_t = std::invoke_result_t<Body &>;
    using result_t = task_result_t<task_t>;

    auto child_slot = make_widget_slot();
    auto child_deed =
        nxt::rt::fork(
            detail::run_widget_child(child_slot, std::move(body)));
    return widget_child<result_t>{
        std::move(child_slot),
        std::move(child_deed)};
}

inline auto child_slots_column(std::span<const widget_slot> slots)
{
    return nxt::tui::each(
        slots,
        [](const widget_slot & slot) {
            return slot;
        });
}

inline auto child_slots_column(const std::vector<widget_slot> & slots)
{
    return child_slots_column(std::span<const widget_slot>{slots});
}

namespace detail {

template<typename Body>
    requires std::invocable<Body &>
        && is_task_v<std::invoke_result_t<Body &>>
        && std::is_void_v<task_result_t<std::invoke_result_t<Body &>>>
task<void> run_ui_zone_children(
    Body & body,
    ui_runtime & ui,
    std::chrono::milliseconds frame_time,
    catching_deed<void> & owner,
    catching_deed<void> & input,
    catching_deed<void> & worker)
{
    owner = fork(ui.run_terminal_owner(frame_time)).cope();
    auto * deck = current_deck();
    if (deck == nullptr)
        throw runtime_error{"nxt::rt ui input used without a deck"};
    input = fork(ui.run_input_owner(*deck)).cope();
    worker = fork(
        detail::stop_zone_on_completion(
            detail::run_ui_zone_body_child(body, ui)))
        .cope();
    co_return;
}

} // namespace detail

template<typename Body>
    requires std::invocable<Body &>
        && is_task_v<std::invoke_result_t<Body &>>
        && std::is_void_v<task_result_t<std::invoke_result_t<Body &>>>
task<void> with_ui_zone(
    Body body,
    ui_runtime_options options = {},
    std::chrono::milliseconds frame_time = std::chrono::milliseconds{16})
{
    auto ui = ui_runtime{options};
    auto owner = catching_deed<void>{};
    auto input = catching_deed<void>{};
    auto worker = catching_deed<void>{};

    co_await with_zone([&] {
        return detail::run_ui_zone_children(
            body,
            ui,
            frame_time,
            owner,
            input,
            worker);
    });

    auto worked = std::move(worker).get();
    if (!worked
        && !(ui.stop_requested() && is_operation_cancelled(worked.error())))
        rethrow(worked.error());
    auto owned = std::move(owner).get();
    if (!owned && !(ui.stop_requested() && is_operation_cancelled(owned.error())))
        rethrow(owned.error());
    auto input_done = std::move(input).get();
    if (!input_done
        && !(ui.stop_requested() && is_operation_cancelled(input_done.error())))
        rethrow(input_done.error());
}

} // namespace nxt::rt
