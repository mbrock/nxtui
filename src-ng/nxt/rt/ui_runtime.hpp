#pragma once

#include "nxt/rt/app.hpp"
#include "nxt/rt/buffers.hpp"
#include "nxt/rt/terminal_app.hpp"

#include <nxt/ansi.hpp>
#include <nxt/any_layout.hpp>
#include <nxt/compositor.hpp>
#include <nxt/glyph-table.hpp>
#include <nxt/regional-tty.hpp>
#include <nxt/slot.hpp>
#include <nxt/tui.hpp>
#include <nxt/units.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#include <unistd.h>

namespace nxt::rt {

[[nodiscard]] inline task<void> write_stdout_all(std::string bytes)
{
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

    [[nodiscard]] nxt::tui::Slot<nxt::tui::AnyLayout> surface() const
    {
        return surface_;
    }

    [[nodiscard]] nxt::tui::Slot<nxt::tui::AnyLayout> make_surface()
    {
        return nxt::tui::Slot<nxt::tui::AnyLayout>{
            nxt::tui::AnyLayout{}, [this] { signal_damage(); }};
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

    task<void> print_block(std::string text)
    {
        auto published = co_await commands_.publish(
            terminal_command{
                .kind = terminal_command_kind::print_block,
                .text = std::move(text),
            });
        if (published)
            signal_damage();
    }

    template<nxt::tui::Layout Layout>
    task<void> print(Layout && layout)
    {
        co_await print_block(
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

    template<nxt::tui::Layout Layout>
    [[nodiscard]] std::string render_scrollback_layout(Layout && layout)
    {
        auto height = layout.height_hint().min;
        if (height.count() == 0)
            height = 1 * ln;

        auto raster = nxt::Raster(size_.w, height, glyphs_);
        auto view = raster.view();
        layout.render(view, raster.extent());
        return nxt::ansi::render_raster(raster);
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
            auto buf = regional_tty::append_block<
                regional_tty::ansi_string_backend>(partition, block_text);
            out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
            return;
        }

        if (!partition.windowed())
            return;

        auto buf =
            regional_tty::append_block<regional_tty::ansi_string_backend>(
                partition, block_text);
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

    std::string render_frame_bytes(const nxt::tui::Slot<nxt::tui::AnyLayout> & layout)
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

        flush_output_queue(out);

        compositor_.set_hud_height(target_h, size_.h, out);

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
    nxt::GlyphTable glyphs_;
    nxt::Size size_;
    nxt::ui::TerminalCompositor compositor_;
    nxt::tui::Slot<nxt::tui::AnyLayout> surface_;
    event damage_event_;
    std::uint64_t damage_generation_ = 0;
    channel<terminal_command> commands_;
    std::vector<queued_output> output_queue_;
    bool stopping_ = false;
    bool cleaned_up_ = false;
};

class ui_scope
{
public:
    explicit ui_scope(ui_runtime & runtime)
        : runtime_(&runtime)
        , surface_(runtime.surface())
    {}

    ui_scope(ui_runtime & runtime, nxt::tui::Slot<nxt::tui::AnyLayout> surface)
        : runtime_(&runtime)
        , surface_(std::move(surface))
    {}

    [[nodiscard]] bool has_terminal_surface() const noexcept
    {
        return runtime_->has_terminal_surface();
    }

    [[nodiscard]] const nxt::tui::Slot<nxt::tui::AnyLayout> & surface()
        const noexcept
    {
        return surface_;
    }

    [[nodiscard]] ui_scope child() const
    {
        return ui_scope{*runtime_, runtime_->make_surface()};
    }

    template<nxt::tui::Layout Layout>
    void draw(Layout && layout) const
    {
        surface_.publish(
            nxt::tui::AnyLayout{std::forward<Layout>(layout)});
    }

    void clear() const
    {
        surface_.publish(nxt::tui::AnyLayout{});
    }

    task<void> print_block(std::string text) const
    {
        co_await runtime_->print_block(std::move(text));
    }

    template<nxt::tui::Layout Layout>
    task<void> print(Layout && layout) const
    {
        co_await runtime_->print(std::forward<Layout>(layout));
    }

    void request_shutdown() const noexcept
    {
        runtime_->request_shutdown();
    }

    template<typename T>
    [[nodiscard]] deed<T> fork(task<T> child) const
    {
        return nxt::rt::fork(std::move(child));
    }

    template<typename Body>
    [[nodiscard]] auto spawn(Body body) const;

private:
    ui_runtime * runtime_ = nullptr;
    nxt::tui::Slot<nxt::tui::AnyLayout> surface_;
};

template<typename T>
class ui_child
{
public:
    ui_child(ui_scope scope, deed<T> child)
        : scope_(std::move(scope))
        , child_(std::move(child))
    {}

    ui_child(const ui_child &) = delete;
    ui_child & operator=(const ui_child &) = delete;
    ui_child(ui_child &&) noexcept = default;
    ui_child & operator=(ui_child &&) noexcept = default;

    [[nodiscard]] const nxt::tui::Slot<nxt::tui::AnyLayout> & surface()
        const noexcept
    {
        return scope_.surface();
    }

    [[nodiscard]] const ui_scope & scope() const noexcept
    {
        return scope_;
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
    ui_scope scope_;
    deed<T> child_;
};

namespace detail {

template<typename Body>
    requires std::invocable<Body &, ui_scope>
        && is_task_v<std::invoke_result_t<Body &, ui_scope>>
[[nodiscard]] auto run_ui_child(ui_scope child, Body body)
    -> task<task_result_t<std::invoke_result_t<Body &, ui_scope>>>
{
    using result_t = task_result_t<std::invoke_result_t<Body &, ui_scope>>;
    auto work = std::invoke(body, child);
    auto cleanup = [child]() -> task<void> {
        child.clear();
        co_return;
    };

    if constexpr (std::is_void_v<result_t>) {
        co_await finally(std::move(work), std::move(cleanup));
    } else {
        co_return co_await finally(std::move(work), std::move(cleanup));
    }
}

} // namespace detail

template<typename Body>
[[nodiscard]] auto ui_scope::spawn(Body body) const
{
    using task_t = std::invoke_result_t<Body &, ui_scope>;
    using result_t = task_result_t<task_t>;

    auto child_scope = child();
    auto child_deed =
        nxt::rt::fork(detail::run_ui_child(child_scope, std::move(body)));
    return ui_child<result_t>{std::move(child_scope), std::move(child_deed)};
}

template<typename Body>
    requires std::invocable<Body &, ui_scope>
        && is_task_v<std::invoke_result_t<Body &, ui_scope>>
        && std::is_void_v<
            task_result_t<std::invoke_result_t<Body &, ui_scope>>>
task<void> with_ui_zone(
    Body body,
    ui_runtime_options options = {},
    std::chrono::milliseconds frame_time = std::chrono::milliseconds{16})
{
    auto ui = ui_runtime{options};
    auto scope = ui_scope{ui};
    auto owner = catching_deed<void>{};

    co_await with_zone([&]() -> task<void> {
        owner = fork(ui.run_terminal_owner(frame_time)).cope();
        try {
            co_await std::invoke(body, scope);
        } catch (...) {
            ui.request_shutdown();
            throw;
        }
        ui.request_shutdown();
    });

    auto owned = std::move(owner).get();
    if (!owned)
        rethrow(owned.error());
}

} // namespace nxt::rt
