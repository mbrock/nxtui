#pragma once

#include "nxt/ansi.hpp"
#include "nxt/units.hpp"

#include <algorithm>
#include <concepts>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nxt::regional_tty {

/// Half-open vertical terminal region: [top, bottom_exclusive).
///
/// This is the core coordinate object. Counts are `height_t`; positions are
/// `row_t`. A value that names a row should stay a row until the last possible
/// moment.
struct vertical_region
{
    row_t top{terminal_origin_v + 0 * ln};
    row_t bottom_exclusive{terminal_origin_v + 0 * ln};

    friend constexpr bool
    operator==(vertical_region, vertical_region) noexcept = default;

    [[nodiscard]] static constexpr vertical_region
    from_top_and_height(row_t top, height_t height) noexcept
    {
        return {top, top + height};
    }

    [[nodiscard]] static constexpr vertical_region
    from_terminal_height(height_t height) noexcept
    {
        return from_top_and_height(terminal_origin_v + 0 * ln, height);
    }

    [[nodiscard]] constexpr bool empty() const noexcept
    {
        return top == bottom_exclusive;
    }

    [[nodiscard]] constexpr height_t height() const noexcept
    {
        return bottom_exclusive - top;
    }

    [[nodiscard]] constexpr bool contains(row_t row) const noexcept
    {
        return top <= row && row < bottom_exclusive;
    }

    [[nodiscard]] constexpr row_t bottom_inclusive() const noexcept
    {
        return bottom_exclusive - 1 * ln;
    }
};

/// DECSTBM scroll region. Unlike `vertical_region`, both terminal margins are
/// inclusive because that is what the terminal configures.
struct scroll_region
{
    vertical_region rows{};

    friend constexpr bool
    operator==(scroll_region, scroll_region) noexcept = default;

    [[nodiscard]] constexpr row_t top_margin() const noexcept
    {
        return rows.top;
    }

    [[nodiscard]] constexpr row_t bottom_margin() const noexcept
    {
        return rows.bottom_inclusive();
    }
};

enum class partition_kind {
    hidden_fixed_region,
    windowed_fixed_region,
    fullscreen_fixed_region,
};

/// The terminal divided into the scrollable region and the bottom fixed region
/// our HUD owns. In windowed mode the two regions form a precise partition of
/// the terminal rows.
struct screen_partition
{
    vertical_region terminal{};
    partition_kind kind{partition_kind::hidden_fixed_region};
    std::optional<scroll_region> scroll{};
    vertical_region bottom_fixed{};

    friend constexpr bool
    operator==(screen_partition, screen_partition) noexcept = default;

    [[nodiscard]] static constexpr screen_partition
    for_bottom_fixed_height(height_t terminal_height, height_t fixed_height)
        noexcept
    {
        auto terminal_rows =
            vertical_region::from_terminal_height(terminal_height);
        fixed_height = std::min(fixed_height, terminal_height);

        if (fixed_height == 0 * ln) {
            return screen_partition{
                .terminal = terminal_rows,
                .kind = partition_kind::hidden_fixed_region,
                .scroll = std::nullopt,
                .bottom_fixed = vertical_region{
                    terminal_rows.bottom_exclusive,
                    terminal_rows.bottom_exclusive}};
        }

        if (fixed_height == terminal_height) {
            return screen_partition{
                .terminal = terminal_rows,
                .kind = partition_kind::fullscreen_fixed_region,
                .scroll = std::nullopt,
                .bottom_fixed = terminal_rows};
        }

        auto fixed_top = terminal_rows.bottom_exclusive - fixed_height;
        auto scroll_rows = vertical_region{terminal_rows.top, fixed_top};
        return screen_partition{
            .terminal = terminal_rows,
            .kind = partition_kind::windowed_fixed_region,
            .scroll = scroll_region{scroll_rows},
            .bottom_fixed = vertical_region{
                fixed_top, terminal_rows.bottom_exclusive}};
    }

    [[nodiscard]] constexpr bool windowed() const noexcept
    {
        return kind == partition_kind::windowed_fixed_region;
    }

    [[nodiscard]] constexpr bool hidden() const noexcept
    {
        return kind == partition_kind::hidden_fixed_region;
    }

    [[nodiscard]] constexpr bool fullscreen() const noexcept
    {
        return kind == partition_kind::fullscreen_fixed_region;
    }

    [[nodiscard]] constexpr row_t chrome_start() const noexcept
    {
        if (windowed())
            return bottom_fixed.top;
        if (fullscreen())
            return terminal.top;
        return terminal.bottom_exclusive;
    }
};

/// The explicit scroll needed before a partition change can be applied without
/// losing durable rows beneath the incoming fixed region.
struct scroll_transfer
{
    height_t rows{0 * ln};

    friend constexpr bool
    operator==(scroll_transfer, scroll_transfer) noexcept = default;

    [[nodiscard]] constexpr bool active() const noexcept
    {
        return rows > 0 * ln;
    }
};

/// A named transition from one terminal partition to another.
struct repartition
{
    std::optional<screen_partition> old{};
    screen_partition next{};
    std::optional<row_t> insertion_cursor{};
    bool initial_attachment = false;

    [[nodiscard]] static constexpr repartition
    initial(
        screen_partition next,
        std::optional<row_t> insertion_cursor = std::nullopt) noexcept
    {
        return repartition{
            .old = std::nullopt,
            .next = next,
            .insertion_cursor = insertion_cursor,
            .initial_attachment = true};
    }

    [[nodiscard]] static constexpr repartition
    from(screen_partition old, screen_partition next) noexcept
    {
        return repartition{
            .old = old,
            .next = next,
            .initial_attachment = false};
    }

    [[nodiscard]] constexpr scroll_transfer reservation() const noexcept
    {
        if (!next.windowed())
            return {};

        if (initial_attachment) {
            if (insertion_cursor) {
                auto bottom = next.scroll->bottom_margin();
                if (*insertion_cursor > bottom)
                    return {.rows = *insertion_cursor - bottom};
                return {};
            }

            // If the starting cursor is unknown, do not invent a bottom-row
            // insertion target. The scrollback writer should follow the real
            // cursor; callers that can observe it pass insertion_cursor.
            return {};
        }

        if (!old || !old->windowed()) {
            // Re-entering windowed mode after the HUD was hidden means durable
            // scrollback may occupy the terminal bottom. Reserve the whole
            // incoming fixed region.
            return {.rows = next.bottom_fixed.height()};
        }

        auto old_bottom = old->scroll->bottom_margin();
        auto new_bottom = next.scroll->bottom_margin();
        if (new_bottom < old_bottom)
            return {.rows = old_bottom - new_bottom};

        return {};
    }

    [[nodiscard]] constexpr scroll_transfer release() const noexcept
    {
        if (initial_attachment || !old || !old->windowed())
            return {};

        if (next.hidden())
            return {.rows = old->bottom_fixed.height()};

        if (!next.windowed())
            return {};

        auto old_bottom = old->scroll->bottom_margin();
        auto new_bottom = next.scroll->bottom_margin();
        if (new_bottom > old_bottom)
            return {.rows = new_bottom - old_bottom};

        return {};
    }

    [[nodiscard]] constexpr vertical_region chrome_to_clear() const noexcept
    {
        if (initial_attachment)
            return {next.terminal.bottom_exclusive,
                    next.terminal.bottom_exclusive};

        if (next.hidden() && old)
            return old->bottom_fixed;

        auto start = next.chrome_start();
        if (old)
            start = std::min(start, old->chrome_start());
        start = std::clamp(
            start, next.terminal.top, next.terminal.bottom_exclusive);
        return {start, next.terminal.bottom_exclusive};
    }
};

enum class command_kind {
    save_cursor,
    restore_cursor,
    reset_graphics,
    set_scroll_region,
    reset_scroll_region,
    scroll_up,
    scroll_down,
    move_up,
    move_down,
    move_to_left,
    clear_line,
    line_feed,
    text,
};

/// Semantic command list backend. Useful for tests and for reading the model as
/// a specification before choosing a concrete byte encoding.
struct command
{
    command_kind kind{};
    row_t row{terminal_origin_v + 0 * ln};
    height_t amount{0 * ln};
    scroll_region scroll{};
    std::string text{};
};

struct command_list_backend
{
    using program_type = std::vector<command>;

    [[nodiscard]] static program_type empty()
    {
        return {};
    }

    static void append(program_type & out, program_type step)
    {
        out.insert(
            out.end(),
            std::make_move_iterator(step.begin()),
            std::make_move_iterator(step.end()));
    }

    [[nodiscard]] static program_type save_cursor()
    {
        return {{.kind = command_kind::save_cursor}};
    }

    [[nodiscard]] static program_type restore_cursor()
    {
        return {{.kind = command_kind::restore_cursor}};
    }

    [[nodiscard]] static program_type reset_graphics()
    {
        return {{.kind = command_kind::reset_graphics}};
    }

    [[nodiscard]] static program_type set_scroll_region(scroll_region region)
    {
        return {{.kind = command_kind::set_scroll_region, .scroll = region}};
    }

    [[nodiscard]] static program_type reset_scroll_region()
    {
        return {{.kind = command_kind::reset_scroll_region}};
    }

    [[nodiscard]] static program_type scroll_up(height_t rows)
    {
        return {{.kind = command_kind::scroll_up, .amount = rows}};
    }

    [[nodiscard]] static program_type scroll_down(height_t rows)
    {
        return {{.kind = command_kind::scroll_down, .amount = rows}};
    }

    [[nodiscard]] static program_type move_up(height_t rows)
    {
        return {{.kind = command_kind::move_up, .amount = rows}};
    }

    [[nodiscard]] static program_type move_down(height_t rows)
    {
        return {{.kind = command_kind::move_down, .amount = rows}};
    }

    [[nodiscard]] static program_type move_to_left(row_t row)
    {
        return {{.kind = command_kind::move_to_left, .row = row}};
    }

    [[nodiscard]] static program_type clear_line()
    {
        return {{.kind = command_kind::clear_line}};
    }

    [[nodiscard]] static program_type line_feed()
    {
        return {{.kind = command_kind::line_feed}};
    }

    [[nodiscard]] static program_type text(std::string_view text)
    {
        return {{.kind = command_kind::text, .text = std::string{text}}};
    }
};

/// Concrete ANSI string backend built on the existing writer.
struct ansi_string_backend
{
    using program_type = std::string;

    [[nodiscard]] static program_type empty()
    {
        return {};
    }

    static void append(program_type & out, program_type step)
    {
        out += step;
    }

    [[nodiscard]] static program_type with_writer(auto fn)
    {
        auto out = std::string{};
        ansi::Writer writer(out);
        fn(writer);
        return out;
    }

    [[nodiscard]] static program_type save_cursor()
    {
        return with_writer([](ansi::Writer & w) { w.save_cursor(); });
    }

    [[nodiscard]] static program_type restore_cursor()
    {
        return with_writer([](ansi::Writer & w) { w.restore_cursor(); });
    }

    [[nodiscard]] static program_type reset_graphics()
    {
        return with_writer([](ansi::Writer & w) { w.reset(); });
    }

    [[nodiscard]] static program_type set_scroll_region(scroll_region region)
    {
        return with_writer([&](ansi::Writer & w) {
            w.set_scroll_region(
                region.top_margin(), region.bottom_margin());
        });
    }

    [[nodiscard]] static program_type reset_scroll_region()
    {
        return with_writer([](ansi::Writer & w) { w.reset_scroll_region(); });
    }

    [[nodiscard]] static program_type scroll_up(height_t rows)
    {
        return with_writer([&](ansi::Writer & w) { w.scroll_up(rows); });
    }

    [[nodiscard]] static program_type scroll_down(height_t rows)
    {
        return with_writer([&](ansi::Writer & w) { w.scroll_down(rows); });
    }

    [[nodiscard]] static program_type move_up(height_t rows)
    {
        return with_writer([&](ansi::Writer & w) { w.move_up(rows); });
    }

    [[nodiscard]] static program_type move_down(height_t rows)
    {
        return with_writer([&](ansi::Writer & w) { w.move_down(rows); });
    }

    [[nodiscard]] static program_type move_to_left(row_t row)
    {
        return with_writer([&](ansi::Writer & w) {
            w.move_to(Pos{terminal_origin + 0 * ch, row});
        });
    }

    [[nodiscard]] static program_type clear_line()
    {
        return with_writer([](ansi::Writer & w) { w.clear_line(); });
    }

    [[nodiscard]] static program_type line_feed()
    {
        return "\n";
    }

    [[nodiscard]] static program_type text(std::string_view text)
    {
        return std::string{text};
    }
};

template<typename Backend>
concept backend = requires(
    typename Backend::program_type program,
    row_t row,
    height_t rows,
    scroll_region region,
    std::string_view text) {
    { Backend::empty() } -> std::same_as<typename Backend::program_type>;
    { Backend::append(program, Backend::empty()) } -> std::same_as<void>;
    { Backend::save_cursor() } -> std::same_as<typename Backend::program_type>;
    { Backend::restore_cursor() }
        -> std::same_as<typename Backend::program_type>;
    { Backend::reset_graphics() }
        -> std::same_as<typename Backend::program_type>;
    { Backend::set_scroll_region(region) }
        -> std::same_as<typename Backend::program_type>;
    { Backend::reset_scroll_region() }
        -> std::same_as<typename Backend::program_type>;
    { Backend::scroll_up(rows) } -> std::same_as<typename Backend::program_type>;
    { Backend::scroll_down(rows) }
        -> std::same_as<typename Backend::program_type>;
    { Backend::move_up(rows) } -> std::same_as<typename Backend::program_type>;
    { Backend::move_down(rows) } -> std::same_as<typename Backend::program_type>;
    { Backend::move_to_left(row) }
        -> std::same_as<typename Backend::program_type>;
    { Backend::clear_line() } -> std::same_as<typename Backend::program_type>;
    { Backend::line_feed() } -> std::same_as<typename Backend::program_type>;
    { Backend::text(text) } -> std::same_as<typename Backend::program_type>;
};

template<backend Backend>
class program_builder
{
public:
    using program_type = typename Backend::program_type;

    void emit(program_type step)
    {
        Backend::append(program_, std::move(step));
    }

    [[nodiscard]] program_type finish() &&
    {
        return std::move(program_);
    }

private:
    program_type program_{Backend::empty()};
};

template<backend Backend>
[[nodiscard]] typename Backend::program_type emit_repartition(
    const repartition & change)
{
    auto out = program_builder<Backend>{};
    out.emit(Backend::reset_graphics());

    auto reservation = change.reservation();
    if (reservation.active()) {
        out.emit(Backend::scroll_up(reservation.rows));
        out.emit(Backend::move_up(reservation.rows));
    }

    // DECSTBM moves the cursor to home. The cursor is also our insertion
    // target, so repartition brackets margin changes and then applies only
    // relative cursor corrections matching the scroll transfer.
    out.emit(Backend::save_cursor());

    if (change.next.scroll)
        out.emit(Backend::set_scroll_region(*change.next.scroll));
    else
        out.emit(Backend::reset_scroll_region());

    out.emit(Backend::restore_cursor());

    auto clear = change.chrome_to_clear();
    if (!clear.empty()) {
        out.emit(Backend::save_cursor());
        for (auto row = clear.top; row < clear.bottom_exclusive;
             row += 1 * ln) {
            out.emit(Backend::move_to_left(row));
            out.emit(Backend::clear_line());
        }
        out.emit(Backend::restore_cursor());
    }

    return std::move(out).finish();
}

template<backend Backend>
[[nodiscard]] typename Backend::program_type append_block(
    const screen_partition &,
    std::string_view block)
{
    auto out = program_builder<Backend>{};
    if (block.empty())
        return std::move(out).finish();

    out.emit(Backend::reset_graphics());
    auto remaining = block;
    while (true) {
        auto end = remaining.find('\n');
        auto line =
            end == std::string_view::npos ? remaining
                                           : remaining.substr(0, end);
        if (!line.empty())
            out.emit(Backend::text(line));
        out.emit(Backend::line_feed());
        if (end == std::string_view::npos)
            break;
        remaining.remove_prefix(end + 1);
    }
    return std::move(out).finish();
}

/// Stateful durable-output appender.
///
/// A complete scrollback block should stop on its final visible row. The next
/// block, not the previous one, spends the line feed needed to start a new row.
/// That keeps HUD repartitions from promoting a speculative blank row into the
/// terminal scrollback history.
struct scrollback_append_state
{
    bool after_block = false;

    template<backend Backend>
    [[nodiscard]] typename Backend::program_type append_block(
        const screen_partition &,
        std::string_view block)
    {
        auto out = program_builder<Backend>{};
        if (block.empty())
            return std::move(out).finish();

        out.emit(Backend::reset_graphics());
        if (after_block)
            out.emit(Backend::line_feed());

        auto remaining = block;
        while (true) {
            auto end = remaining.find('\n');
            auto line =
                end == std::string_view::npos ? remaining
                                               : remaining.substr(0, end);
            if (!line.empty())
                out.emit(Backend::text(line));
            if (end == std::string_view::npos)
                break;
            out.emit(Backend::line_feed());
            remaining.remove_prefix(end + 1);
        }

        after_block = true;
        return std::move(out).finish();
    }
};

} // namespace nxt::regional_tty
