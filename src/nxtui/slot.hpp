#pragma once

#include "nxtui/layout.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <utility>

namespace nxtui::tui {

/// A live hole in the layout tree.
///
/// `Slot<L>` satisfies the `Layout` concept by forwarding to whichever
/// `L` value was most recently published into it. The slot is a shared
/// handle: copies share the same cell, so the layout tree can copy a
/// slot by value each frame while a coroutine elsewhere keeps publishing
/// fresh layouts into the same cell.
///
/// Publishing stores through `std::atomic<std::shared_ptr>` and then runs
/// an optional on-publish callback on the publishing thread, typically one
/// that asks the render loop for a new frame (for example
/// `nxtrt::runtime::signal_damage`). A render holds its own reference to
/// the layout it loaded, so a concurrent publish never frees it mid-render.
template<Layout L>
class Slot
{
public:
    /// Construct a slot showing `initial`. `on_publish` runs after every
    /// `publish()`, e.g. `[&rt] { rt.signal_damage(); }`; it can also be
    /// installed later with `set_on_publish`.
    explicit Slot(L initial, std::function<void()> on_publish = {})
        : cell_(std::make_shared<Cell>())
    {
        cell_->on_publish = std::move(on_publish);
        cell_->current.store(
            std::make_shared<const L>(std::move(initial)),
            std::memory_order_release);
    }

    /// Replace the current layout, then run the on-publish callback.
    /// The store is atomic; the callback must be safe to call from the
    /// publishing thread.
    void publish(L layout) const
    {
        cell_->current.store(
            std::make_shared<const L>(std::move(layout)),
            std::memory_order_release);
        if (cell_->on_publish)
            cell_->on_publish();
    }

    /// Install or replace the on-publish callback. Not synchronized with
    /// `publish`; call it before anything starts publishing.
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
        std::atomic<std::shared_ptr<const L>> current;
        std::function<void()> on_publish;
    };

    std::shared_ptr<const L> load() const
    {
        return cell_->current.load(std::memory_order_acquire);
    }

    std::shared_ptr<Cell> cell_;
};

/// Make a `Slot<L>`: `slot(text("hi"), [&rt] { rt.signal_damage(); })`.
template<Layout L>
auto slot(L initial, std::function<void()> on_publish = {})
{
    return Slot<L>(std::move(initial), std::move(on_publish));
}

} // namespace nxtui::tui
