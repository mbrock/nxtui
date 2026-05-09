#pragma once
// Minimal C++ wrapper for libvterm - render ANSI to virtual terminal and
// read back screen state. Usage: Terminal term(24, 80);
// term.write("\x1b[1mBold\x1b[0m"); term.get_cell(0,0)->bold;

#include <vterm.h>

#include <experimental/mdspan>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nxt::vterm {

/// Wrapper for VTermColor with convenient constructors
struct Color
{
    VTermColor c;

    Color() noexcept
    {
        c.type = VTERM_COLOR_DEFAULT_FG | VTERM_COLOR_DEFAULT_BG;
    }

    static Color
    rgb(const std::uint8_t r,
        const std::uint8_t g,
        const std::uint8_t b) noexcept
    {
        Color col;
        vterm_color_rgb(&col.c, r, g, b);
        return col;
    }

    static Color indexed(const std::uint8_t idx) noexcept
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

/// Represents a single cell in the terminal screen
struct Cell
{
    std::u32string chars; ///< UTF-32 characters (base + combining)
    Color fg;             ///< Foreground color
    Color bg;             ///< Background color
    int width;            ///< Cell width (1 or 2 for wide chars)
    bool bold:1;
    bool italic:1;
    bool underline:1;
    bool blink:1;
    bool reverse:1;
    bool strike:1;

    Cell() noexcept
        : width(1)
        , bold(false)
        , italic(false)
        , underline(false)
        , blink(false)
        , reverse(false)
        , strike(false)
    {
    }

    /// Convert from VTermScreenCell
    static Cell from_vterm(const VTermScreenCell & cell) noexcept
    {
        Cell c;

        // Copy characters
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

/// RAII wrapper for VTerm with convenient screen access
class Terminal
{
public:
    /// Create a new terminal with given size
    Terminal(const int rows, const int cols)
        : vt_(vterm_new(rows, cols), &vterm_free)
    {
        if (!vt_)
            throw std::runtime_error("Failed to create VTerm");

        vterm_set_utf8(vt_.get(), true);
        screen_ = vterm_obtain_screen(vt_.get());

        if (!screen_)
            throw std::runtime_error("Failed to obtain VTermScreen");

        vterm_screen_reset(screen_, 1);
    }

    /// Write ANSI sequences to the terminal
    void write(const std::string_view data) const
    {
        vterm_input_write(vt_.get(), data.data(), data.size());
    }

    /// Get a cell at the given position (0-based)
    [[nodiscard]] std::optional<Cell>
    get_cell(const int row, const int col) const
    {
        const VTermPos pos{row, col};
        VTermScreenCell cell;

        if (!vterm_screen_get_cell(screen_, pos, &cell))
            return std::nullopt;

        return Cell::from_vterm(cell);
    }

    /// Get entire row as cells (0-based row)
    [[nodiscard]] std::vector<Cell> get_row(const int row) const
    {
        int rows, cols;
        vterm_get_size(vt_.get(), &rows, &cols);

        std::vector<Cell> result;
        result.reserve(cols);

        for (int col = 0; col < cols;) {
            if (auto cell = get_cell(row, col)) {
                result.push_back(*cell);
                col += cell->width;
            } else {
                ++col;
            }
        }

        return result;
    }

    /// Get text from the screen (0-based coordinates)
    [[nodiscard]] std::string get_text(
        const int start_row,
        const int start_col,
        const int end_row,
        const int end_col) const
    {
        const VTermRect rect{
            start_row, end_row + 1, start_col, end_col + 1};

        // Allocate buffer (estimate: 4 bytes per cell for UTF-8)
        const std::size_t buf_size =
            (end_row - start_row + 1) * (end_col - start_col + 1) * 4;
        std::string buffer(buf_size, '\0');

        const std::size_t written = vterm_screen_get_text(
            screen_, buffer.data(), buffer.size(), rect);
        buffer.resize(written);

        return buffer;
    }

    /// Get entire screen as text
    [[nodiscard]] std::string get_screen_text() const
    {
        int rows, cols;
        vterm_get_size(vt_.get(), &rows, &cols);
        return get_text(0, 0, rows - 1, cols - 1);
    }

    /// Get a single row as text (0-based)
    [[nodiscard]] std::string get_row_text(const int row) const
    {
        int rows, cols;
        vterm_get_size(vt_.get(), &rows, &cols);
        return get_text(row, 0, row, cols - 1);
    }

    /// Get terminal dimensions
    [[nodiscard]] std::pair<int, int> get_size() const
    {
        int rows, cols;
        vterm_get_size(vt_.get(), &rows, &cols);
        return {rows, cols};
    }

    /// Resize terminal
    void set_size(const int rows, const int cols)
    {
        vterm_set_size(vt_.get(), rows, cols);
    }

    /// Reset terminal to initial state
    void reset(const bool hard = true)
    {
        vterm_screen_reset(screen_, hard ? 1 : 0);
    }

    /// Access raw VTerm pointer (for advanced usage)
    [[nodiscard]] VTerm * raw() const noexcept
    {
        return vt_.get();
    }

    /// Access raw VTermScreen pointer (for advanced usage)
    [[nodiscard]] VTermScreen * screen() const noexcept
    {
        return screen_;
    }

    /// Snapshot of terminal screen as a 2D grid
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

        ScreenSnapshot(const int r, const int c)
            : cells(r * c)
            , rows(r)
            , cols(c)
        {
        }

        /// Get 2D view of cells
        [[nodiscard]] cell_view_t view() noexcept
        {
            return {cells.data(), mdspan_extents{rows, cols}};
        }

        [[nodiscard]] const_cell_view_t view() const noexcept
        {
            return {cells.data(), mdspan_extents{rows, cols}};
        }

        /// Helper: check if all cells satisfy a predicate
        template<typename Pred>
        [[nodiscard]] bool all_of(Pred && pred) const
        {
            const auto v = view();
            for (int r = 0; r < rows; ++r) {
                for (int c = 0; c < cols; ++c) {
                    if (!pred(v[r, c]))
                        return false;
                }
            }
            return true;
        }

        /// Helper: check if any cell satisfies a predicate
        template<typename Pred>
        [[nodiscard]] bool any_of(Pred && pred) const
        {
            const auto v = view();
            for (int r = 0; r < rows; ++r) {
                for (int c = 0; c < cols; ++c) {
                    if (pred(v[r, c]))
                        return true;
                }
            }
            return false;
        }

        /// Helper: count cells matching a predicate
        template<typename Pred>
        [[nodiscard]] int count_if(Pred && pred) const
        {
            const auto v = view();
            int count = 0;
            for (int r = 0; r < rows; ++r) {
                for (int c = 0; c < cols; ++c) {
                    if (pred(v[r, c]))
                        ++count;
                }
            }
            return count;
        }
    };

    /// Capture current terminal state into a 2D grid
    [[nodiscard]] ScreenSnapshot snapshot() const
    {
        auto [rows, cols] = get_size();
        ScreenSnapshot snap(rows, cols);
        const auto view = snap.view();

        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols;) {
                if (auto cell = get_cell(r, c)) {
                    view[r, c] = *cell;
                    // Handle wide characters by filling continuation cells
                    const int width = cell->width;
                    for (int i = 1; i < width && c + i < cols; ++i) {
                        view[r, c + i] = *cell; // Duplicate for wide char
                    }
                    c += width;
                } else {
                    ++c;
                }
            }
        }

        return snap;
    }

private:
    std::unique_ptr<VTerm, void (*)(VTerm *)> vt_;
    VTermScreen * screen_ = nullptr;
};

} // namespace nxt::vterm
