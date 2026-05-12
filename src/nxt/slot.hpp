#pragma once

#include "nxt/raster.hpp"
#include "nxt/tui.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <utility>

namespace nxt::tui {

/// A live hole in the layout tree.
///
/// `Slot<L>` satisfies the `Layout` concept by forwarding to whichever
/// `L` value was most recently published into it. The slot is a shared
/// handle: copies share the same cell, so the layout tree can copy a
/// slot by value each frame while a coroutine elsewhere keeps publishing
/// fresh layouts into the same cell.
///
/// Publishing uses the standard atomic shared_ptr operations and invokes
/// an optional on-publish callback — typically `UIRuntime::signal_damage`.
template<Layout L>
class Slot
{
public:
    /// Construct a slot showing `initial`. `on_publish` is invoked after
    /// every successful `publish()`; it is intended to wake the render
    /// loop (e.g. `[&rt]{ rt.signal_damage(); }`). It can also be set
    /// later via `set_on_publish` for cases where the runtime is not
    /// available at construction time.
    explicit Slot(L initial, std::function<void()> on_publish = {})
        : cell_(std::make_shared<Cell>())
    {
        cell_->on_publish = std::move(on_publish);
        std::atomic_store_explicit(
            &cell_->current,
            std::make_shared<const L>(std::move(initial)),
            std::memory_order_release);
    }

    /// Replace the current layout. Thread-safe.
    void publish(L layout) const
    {
        std::atomic_store_explicit(
            &cell_->current,
            std::make_shared<const L>(std::move(layout)),
            std::memory_order_release);
        if (cell_->on_publish)
            cell_->on_publish();
    }

    /// Install or replace the on-publish callback. Not thread-safe; call
    /// before any coroutine starts publishing.
    void set_on_publish(std::function<void()> fn) const
    {
        cell_->on_publish = std::move(fn);
    }

    WidthHint width_hint() const
    {
        return load()->width_hint();
    }

    HeightHint height_hint() const
    {
        return load()->height_hint();
    }

    void render(RasterView & raster, Size size) const
    {
        load()->render(raster, size);
    }

private:
    struct Cell
    {
        std::shared_ptr<const L> current;
        std::function<void()> on_publish;
    };

    std::shared_ptr<const L> load() const
    {
        return std::atomic_load_explicit(
            &cell_->current, std::memory_order_acquire);
    }

    std::shared_ptr<Cell> cell_;
};

/// Deduction helper: `slot(text("hi"), [&rt]{ rt.signal_damage(); })`.
template<Layout L>
auto slot(L initial, std::function<void()> on_publish = {})
{
    return Slot<L>(std::move(initial), std::move(on_publish));
}

} // namespace nxt::tui
