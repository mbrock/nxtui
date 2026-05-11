#pragma once

// Small C++ wrapper for libvterm. It owns a VTerm screen model, accepts raw
// PTY bytes, exposes screen cells for rendering, and can encode terminal input
// events back into bytes for the child side of the PTY.

#include <vterm.h>

#include <cstdint>
#include <experimental/mdspan>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nxt::vterm {

struct Color
{
    VTermColor c;

    Color() noexcept
    {
        c.type = VTERM_COLOR_DEFAULT_FG | VTERM_COLOR_DEFAULT_BG;
    }

    static Color
    rgb(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept
    {
        Color col;
        vterm_color_rgb(&col.c, r, g, b);
        return col;
    }

    static Color indexed(std::uint8_t idx) noexcept
    {
        Color col;
        vterm_color_indexed(&col.c, idx);
        return col;
    }

    [[nodiscard]] bool is_rgb() const noexcept
    {
        return VTERM_COLOR_IS_RGB(&c);
    }

    [[nodiscard]] bool is_indexed() const noexcept
    {
        return VTERM_COLOR_IS_INDEXED(&c);
    }

    [[nodiscard]] bool is_default_fg() const noexcept
    {
        return VTERM_COLOR_IS_DEFAULT_FG(&c);
    }

    [[nodiscard]] bool is_default_bg() const noexcept
    {
        return VTERM_COLOR_IS_DEFAULT_BG(&c);
    }
};

struct Cell
{
    std::u32string chars;
    Color fg;
    Color bg;
    int width = 1;
    bool bold:1 = false;
    bool italic:1 = false;
    bool underline:1 = false;
    bool blink:1 = false;
    bool reverse:1 = false;
    bool strike:1 = false;

    static Cell from_vterm(const VTermScreenCell & cell) noexcept
    {
        Cell c;

        for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; i++)
            c.chars.push_back(cell.chars[i]);

        c.fg.c = cell.fg;
        c.bg.c = cell.bg;
        c.width = cell.width;
        c.bold = cell.attrs.bold;
        c.italic = cell.attrs.italic;
        c.underline = cell.attrs.underline;
        c.blink = cell.attrs.blink;
        c.reverse = cell.attrs.reverse;
        c.strike = cell.attrs.strike;

        return c;
    }
};

enum class CursorShape {
    block,
    underline,
    bar_left,
};

struct Cursor
{
    int row = 0;
    int col = 0;
    bool visible = true;
    CursorShape shape = CursorShape::block;
};

class Terminal
{
public:
    Terminal(int rows, int cols)
        : vt_(vterm_new(rows, cols), &vterm_free)
    {
        if (!vt_)
            throw std::runtime_error("failed to create VTerm");

        vterm_set_utf8(vt_.get(), true);
        screen_ = vterm_obtain_screen(vt_.get());

        if (!screen_)
            throw std::runtime_error("failed to obtain VTermScreen");

        state_ = vterm_obtain_state(vt_.get());
        if (!state_)
            throw std::runtime_error("failed to obtain VTermState");

        install_screen_callbacks();
        vterm_screen_reset(screen_, 1);
    }

    Terminal(const Terminal &) = delete;
    Terminal & operator=(const Terminal &) = delete;
    Terminal(Terminal && other) noexcept
        : vt_(std::move(other.vt_))
        , screen_(std::exchange(other.screen_, nullptr))
        , state_(std::exchange(other.state_, nullptr))
        , cursor_visible_(other.cursor_visible_)
        , cursor_shape_(other.cursor_shape_)
    {
        install_screen_callbacks();
    }

    Terminal & operator=(Terminal && other) noexcept
    {
        if (this != &other) {
            vt_ = std::move(other.vt_);
            screen_ = std::exchange(other.screen_, nullptr);
            state_ = std::exchange(other.state_, nullptr);
            cursor_visible_ = other.cursor_visible_;
            cursor_shape_ = other.cursor_shape_;
            install_screen_callbacks();
        }
        return *this;
    }

    void write(std::string_view data) const
    {
        vterm_input_write(vt_.get(), data.data(), data.size());
    }

    void keyboard_unichar(
        std::uint32_t codepoint,
        VTermModifier modifiers = VTERM_MOD_NONE) const
    {
        vterm_keyboard_unichar(vt_.get(), codepoint, modifiers);
    }

    void keyboard_key(
        VTermKey key,
        VTermModifier modifiers = VTERM_MOD_NONE) const
    {
        vterm_keyboard_key(vt_.get(), key, modifiers);
    }

    [[nodiscard]] std::string read_pending_output() const
    {
        std::string out;
        auto pending = vterm_output_get_buffer_current(vt_.get());
        out.resize(pending);
        if (pending > 0) {
            auto n = vterm_output_read(vt_.get(), out.data(), out.size());
            out.resize(n);
        }
        return out;
    }

    [[nodiscard]] std::optional<Cell>
    get_cell(int row, int col) const
    {
        const VTermPos pos{row, col};
        VTermScreenCell cell;

        if (!vterm_screen_get_cell(screen_, pos, &cell))
            return std::nullopt;

        return Cell::from_vterm(cell);
    }

    [[nodiscard]] std::vector<Cell> get_row(int row) const
    {
        auto [rows, cols] = get_size();
        (void) rows;

        std::vector<Cell> result;
        result.reserve(cols);

        for (int col = 0; col < cols;) {
            if (auto cell = get_cell(row, col)) {
                result.push_back(*cell);
                col += std::max(1, cell->width);
            } else {
                ++col;
            }
        }

        return result;
    }

    [[nodiscard]] std::string get_text(
        int start_row,
        int start_col,
        int end_row,
        int end_col) const
    {
        const VTermRect rect{
            start_row, end_row + 1, start_col, end_col + 1};

        const std::size_t buf_size =
            (end_row - start_row + 1) * (end_col - start_col + 1) * 4;
        std::string buffer(buf_size, '\0');

        const std::size_t written = vterm_screen_get_text(
            screen_, buffer.data(), buffer.size(), rect);
        buffer.resize(written);

        return buffer;
    }

    [[nodiscard]] std::string get_screen_text() const
    {
        auto [rows, cols] = get_size();
        return get_text(0, 0, rows - 1, cols - 1);
    }

    [[nodiscard]] std::string get_row_text(int row) const
    {
        auto [rows, cols] = get_size();
        (void) rows;
        return get_text(row, 0, row, cols - 1);
    }

    [[nodiscard]] std::pair<int, int> get_size() const
    {
        int rows, cols;
        vterm_get_size(vt_.get(), &rows, &cols);
        return {rows, cols};
    }

    void set_size(int rows, int cols)
    {
        rows = std::max(1, rows);
        cols = std::max(1, cols);
        vterm_set_size(vt_.get(), rows, cols);
    }

    void reset(bool hard = true)
    {
        vterm_screen_reset(screen_, hard ? 1 : 0);
    }

    [[nodiscard]] std::optional<Cursor> cursor() const noexcept
    {
        if (state_ == nullptr)
            return std::nullopt;

        VTermPos pos{};
        vterm_state_get_cursorpos(state_, &pos);
        if (pos.row < 0 || pos.col < 0)
            return std::nullopt;

        return Cursor{
            .row = pos.row,
            .col = pos.col,
            .visible = cursor_visible_,
            .shape = cursor_shape()};
    }

    [[nodiscard]] VTerm * raw() const noexcept
    {
        return vt_.get();
    }

    [[nodiscard]] VTermScreen * screen() const noexcept
    {
        return screen_;
    }

    struct ScreenSnapshot
    {
        std::vector<Cell> cells;
        int rows;
        int cols;

        using mdspan_extents = std::experimental::
            extents<int, std::dynamic_extent, std::dynamic_extent>;
        using cell_view_t = std::experimental::mdspan<Cell, mdspan_extents>;
        using const_cell_view_t =
            std::experimental::mdspan<const Cell, mdspan_extents>;

        ScreenSnapshot(int r, int c)
            : cells(r * c)
            , rows(r)
            , cols(c)
        {
        }

        [[nodiscard]] cell_view_t view() noexcept
        {
            return {cells.data(), mdspan_extents{rows, cols}};
        }

        [[nodiscard]] const_cell_view_t view() const noexcept
        {
            return {cells.data(), mdspan_extents{rows, cols}};
        }

        template<typename Pred>
        [[nodiscard]] bool all_of(Pred && pred) const
        {
            const auto v = view();
            for (int r = 0; r < rows; ++r)
                for (int c = 0; c < cols; ++c)
                    if (!pred(v[r, c]))
                        return false;
            return true;
        }

        template<typename Pred>
        [[nodiscard]] bool any_of(Pred && pred) const
        {
            const auto v = view();
            for (int r = 0; r < rows; ++r)
                for (int c = 0; c < cols; ++c)
                    if (pred(v[r, c]))
                        return true;
            return false;
        }

        template<typename Pred>
        [[nodiscard]] int count_if(Pred && pred) const
        {
            const auto v = view();
            int count = 0;
            for (int r = 0; r < rows; ++r)
                for (int c = 0; c < cols; ++c)
                    if (pred(v[r, c]))
                        ++count;
            return count;
        }
    };

    [[nodiscard]] ScreenSnapshot snapshot() const
    {
        auto [rows, cols] = get_size();
        ScreenSnapshot snap(rows, cols);
        const auto view = snap.view();

        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols;) {
                if (auto cell = get_cell(r, c)) {
                    view[r, c] = *cell;
                    const int width = std::max(1, cell->width);
                    for (int i = 1; i < width && c + i < cols; ++i)
                        view[r, c + i] = *cell;
                    c += width;
                } else {
                    ++c;
                }
            }
        }

        return snap;
    }

private:
    void install_screen_callbacks() noexcept
    {
        if (screen_ == nullptr)
            return;

        vterm_screen_set_callbacks(screen_, screen_callbacks(), this);
    }

    [[nodiscard]] CursorShape cursor_shape() const noexcept
    {
        switch (cursor_shape_) {
        case VTERM_PROP_CURSORSHAPE_UNDERLINE:
            return CursorShape::underline;
        case VTERM_PROP_CURSORSHAPE_BAR_LEFT:
            return CursorShape::bar_left;
        default:
            return CursorShape::block;
        }
    }

    static const VTermScreenCallbacks * screen_callbacks() noexcept
    {
        static const auto callbacks = [] {
            VTermScreenCallbacks cbs{};
            cbs.movecursor = &Terminal::on_move_cursor;
            cbs.settermprop = &Terminal::on_set_term_prop;
            return cbs;
        }();
        return &callbacks;
    }

    static int on_move_cursor(
        VTermPos,
        VTermPos,
        int visible,
        void * user) noexcept
    {
        auto * self = static_cast<Terminal *>(user);
        if (self != nullptr)
            self->cursor_visible_ = visible != 0;
        return 1;
    }

    static int on_set_term_prop(
        VTermProp prop,
        VTermValue * val,
        void * user) noexcept
    {
        auto * self = static_cast<Terminal *>(user);
        if (self == nullptr || val == nullptr)
            return 1;

        if (prop == VTERM_PROP_CURSORVISIBLE)
            self->cursor_visible_ = val->boolean != 0;
        else if (prop == VTERM_PROP_CURSORSHAPE)
            self->cursor_shape_ = val->number;

        return 1;
    }

    std::unique_ptr<VTerm, void (*)(VTerm *)> vt_;
    VTermScreen * screen_ = nullptr;
    VTermState * state_ = nullptr;
    bool cursor_visible_ = true;
    int cursor_shape_ = VTERM_PROP_CURSORSHAPE_BLOCK;
};

} // namespace nxt::vterm
