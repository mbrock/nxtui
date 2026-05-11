#pragma once

/// @file
/// Small C++ wrapper for libvterm.
///
/// The wrapper owns a `VTerm` screen model, accepts raw PTY bytes, exposes
/// decoded cells for rendering, and can encode keyboard input back into bytes
/// for the child side of a PTY.

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

/// Terminal color as reported by libvterm.
struct Color
{
    /// Raw libvterm color value.
    VTermColor c;

    /// Construct a default foreground/background color sentinel.
    Color() noexcept
    {
        c.type = VTERM_COLOR_DEFAULT_FG | VTERM_COLOR_DEFAULT_BG;
    }

    /// Construct a true-color RGB value.
    static Color
    rgb(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept
    {
        Color col;
        vterm_color_rgb(&col.c, r, g, b);
        return col;
    }

    /// Construct an indexed terminal-palette color.
    static Color indexed(std::uint8_t idx) noexcept
    {
        Color col;
        vterm_color_indexed(&col.c, idx);
        return col;
    }

    /// True when the color carries explicit RGB components.
    [[nodiscard]] bool is_rgb() const noexcept
    {
        return VTERM_COLOR_IS_RGB(&c);
    }

    /// True when the color refers to the terminal palette.
    [[nodiscard]] bool is_indexed() const noexcept
    {
        return VTERM_COLOR_IS_INDEXED(&c);
    }

    /// True when the color requests the terminal default foreground.
    [[nodiscard]] bool is_default_fg() const noexcept
    {
        return VTERM_COLOR_IS_DEFAULT_FG(&c);
    }

    /// True when the color requests the terminal default background.
    [[nodiscard]] bool is_default_bg() const noexcept
    {
        return VTERM_COLOR_IS_DEFAULT_BG(&c);
    }
};

/// One terminal screen cell decoded from libvterm.
struct Cell
{
    /// Unicode codepoints stored in the cell.
    std::u32string chars;
    /// Foreground color.
    Color fg;
    /// Background color.
    Color bg;
    /// Display-cell width. Continuation cells are represented by callers.
    int width = 1;
    /// Text attributes carried by the terminal cell.
    bool bold:1 = false;
    bool italic:1 = false;
    bool underline:1 = false;
    bool blink:1 = false;
    bool reverse:1 = false;
    bool strike:1 = false;

    /// Convert a libvterm cell into the wrapper representation.
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

/// Cursor shape requested by the terminal application.
enum class CursorShape {
    block,
    underline,
    bar_left,
};

/// Current cursor position and presentation state.
struct Cursor
{
    /// Zero-based row in terminal cells.
    int row = 0;
    /// Zero-based column in terminal cells.
    int col = 0;
    /// Whether the cursor should be rendered.
    bool visible = true;
    /// Shape requested by the terminal state.
    CursorShape shape = CursorShape::block;
};

/// Owning libvterm terminal screen model.
class Terminal
{
public:
    /// Create a terminal screen with the given row/column size.
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

    /// Feed raw bytes from the child PTY into libvterm.
    void write(std::string_view data) const
    {
        vterm_input_write(vt_.get(), data.data(), data.size());
    }

    /// Encode a Unicode keypress through libvterm's keyboard encoder.
    void keyboard_unichar(
        std::uint32_t codepoint,
        VTermModifier modifiers = VTERM_MOD_NONE) const
    {
        vterm_keyboard_unichar(vt_.get(), codepoint, modifiers);
    }

    /// Encode a special keypress through libvterm's keyboard encoder.
    void keyboard_key(
        VTermKey key,
        VTermModifier modifiers = VTERM_MOD_NONE) const
    {
        vterm_keyboard_key(vt_.get(), key, modifiers);
    }

    /// Read bytes libvterm wants to send back to the child PTY.
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

    /// Return the cell at a zero-based row/column position.
    [[nodiscard]] std::optional<Cell>
    get_cell(int row, int col) const
    {
        const VTermPos pos{row, col};
        VTermScreenCell cell;

        if (!vterm_screen_get_cell(screen_, pos, &cell))
            return std::nullopt;

        return Cell::from_vterm(cell);
    }

    /// Return visible cells from one terminal row.
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

    /// Extract UTF-8 text from an inclusive rectangular cell range.
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

    /// Extract UTF-8 text for the whole screen.
    [[nodiscard]] std::string get_screen_text() const
    {
        auto [rows, cols] = get_size();
        return get_text(0, 0, rows - 1, cols - 1);
    }

    /// Extract UTF-8 text for one row.
    [[nodiscard]] std::string get_row_text(int row) const
    {
        auto [rows, cols] = get_size();
        (void) rows;
        return get_text(row, 0, row, cols - 1);
    }

    /// Current terminal size as `{rows, cols}`.
    [[nodiscard]] std::pair<int, int> get_size() const
    {
        int rows, cols;
        vterm_get_size(vt_.get(), &rows, &cols);
        return {rows, cols};
    }

    /// Resize the terminal, clamping both dimensions to at least one cell.
    void set_size(int rows, int cols)
    {
        rows = std::max(1, rows);
        cols = std::max(1, cols);
        vterm_set_size(vt_.get(), rows, cols);
    }

    /// Reset the screen; a hard reset clears more terminal state.
    void reset(bool hard = true)
    {
        vterm_screen_reset(screen_, hard ? 1 : 0);
    }

    /// Return the cursor state when the underlying terminal state is valid.
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

    /// Raw libvterm handle for low-level integrations.
    [[nodiscard]] VTerm * raw() const noexcept
    {
        return vt_.get();
    }

    /// Raw libvterm screen handle for low-level integrations.
    [[nodiscard]] VTermScreen * screen() const noexcept
    {
        return screen_;
    }

    /// Dense copy of the current screen cells.
    struct ScreenSnapshot
    {
        /// Row-major cell storage.
        std::vector<Cell> cells;
        /// Snapshot row count.
        int rows;
        /// Snapshot column count.
        int cols;

        /// Dynamic extents used by the mdspan views.
        using mdspan_extents = std::experimental::
            extents<int, std::dynamic_extent, std::dynamic_extent>;
        /// Mutable 2D view over the snapshot cells.
        using cell_view_t = std::experimental::mdspan<Cell, mdspan_extents>;
        /// Const 2D view over the snapshot cells.
        using const_cell_view_t =
            std::experimental::mdspan<const Cell, mdspan_extents>;

        /// Allocate a snapshot with `r * c` cells.
        ScreenSnapshot(int r, int c)
            : cells(r * c)
            , rows(r)
            , cols(c)
        {
        }

        /// Mutable 2D view into `cells`.
        [[nodiscard]] cell_view_t view() noexcept
        {
            return {cells.data(), mdspan_extents{rows, cols}};
        }

        /// Const 2D view into `cells`.
        [[nodiscard]] const_cell_view_t view() const noexcept
        {
            return {cells.data(), mdspan_extents{rows, cols}};
        }

        /// True when every cell satisfies `pred`.
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

        /// True when any cell satisfies `pred`.
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

        /// Count cells satisfying `pred`.
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

    /// Capture the current libvterm screen into a dense snapshot.
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
