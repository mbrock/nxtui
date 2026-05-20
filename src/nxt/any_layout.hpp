#pragma once

#include "nxt/raster.hpp"
#include "nxt/tui.hpp"

#include <algorithm>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace nxt::tui {

/// Type-erased Layout. Holds any value satisfying the `Layout` concept
/// behind a small vtable so it can sit inside `Slot<AnyLayout>` and be
/// swapped for differently-shaped layouts over time.
///
/// Cheap to copy (shared_ptr bump). The held layout is immutable; new
/// shapes replace the whole AnyLayout.
class AnyLayout
{
public:
    /// Empty layout renders nothing and reports zero hints.
    AnyLayout() = default;

    template<typename L>
        requires Layout<std::decay_t<L>>
                 && (!std::is_same_v<std::decay_t<L>, AnyLayout>)
    AnyLayout(L && layout)
        : impl_(
              std::make_shared<Model<std::decay_t<L>>>(
                  std::forward<L>(layout)))
    {
    }

    WidthHint width_hint() const
    {
        return impl_ ? impl_->width_hint() : WidthHint{};
    }

    HeightHint height_hint() const
    {
        return impl_ ? impl_->height_hint() : HeightHint{};
    }

    void render(RasterView & r, Size s) const
    {
        if (impl_)
            impl_->render(r, s);
    }

private:
    struct Concept
    {
        virtual ~Concept() = default;
        virtual WidthHint width_hint() const = 0;
        virtual HeightHint height_hint() const = 0;
        virtual void render(RasterView &, Size) const = 0;
    };

    template<Layout L>
    struct Model final : Concept
    {
        L value;

        explicit Model(L v)
            : value(std::move(v))
        {
        }

        WidthHint width_hint() const override
        {
            return value.width_hint();
        }

        HeightHint height_hint() const override
        {
            return value.height_hint();
        }

        void render(RasterView & r, Size s) const override
        {
            value.render(r, s);
        }
    };

    std::shared_ptr<const Concept> impl_;
};

/// Vertical container with a runtime-sized list of children. The
/// variadic `column(...)` covers compile-time-known shapes; this one
/// takes a `std::vector<AnyLayout>` for shapes only known at render time
/// (e.g. one row per item in a list of unknown length).
struct VStack
{
    std::vector<AnyLayout> children;

    WidthHint width_hint() const
    {
        width_t max_min{0 * ch};
        for (const auto & c : children)
            max_min = std::max(max_min, c.width_hint().min);
        return {max_min, 1.0 * one};
    }

    HeightHint height_hint() const
    {
        height_t total_min{0 * ln};
        for (const auto & c : children)
            total_min = total_min + c.height_hint().min;
        return HeightHint::fixed(total_min);
    }

    void render(RasterView & raster, Size size) const
    {
        Pos cursor = Pos::origin();
        for (const auto & c : children) {
            auto h = c.height_hint().min;
            if (h.count() == 0)
                continue;
            if ((cursor.y - Pos::origin().y) + h > size.h)
                break;
            auto child_size = Size{size.w, h};
            auto sub = subraster(raster, cursor, child_size);
            c.render(sub, child_size);
            cursor = cursor + h;
        }
    }
};

/// Horizontal container with a runtime-sized list of children. Mirrors
/// the variadic `row(...)` and applies the same flex distribution rule:
/// fixed minimums first, then remaining width split by `flex` factor.
struct HStack
{
    std::vector<AnyLayout> children;

    WidthHint width_hint() const
    {
        width_t total_min{0 * ch};
        ratio_t total_flex{0.0 * one};
        for (const auto & c : children) {
            total_min += c.width_hint().min;
            total_flex += c.width_hint().flex;
        }
        return {total_min, total_flex};
    }

    HeightHint height_hint() const
    {
        height_t max_min{0 * ln};
        for (const auto & c : children)
            max_min = std::max(max_min, c.height_hint().min);
        return HeightHint::fixed(
            max_min.count() > 0 ? max_min : height_t{1 * ln});
    }

    void render(RasterView & raster, Size size) const
    {
        width_t used{0 * ch};
        ratio_t total_flex{0.0 * one};
        for (const auto & c : children) {
            used += c.width_hint().min;
            total_flex += c.width_hint().flex;
        }

        std::vector<width_t> widths;
        widths.reserve(children.size());
        for (const auto & c : children) {
            auto h = c.width_hint();
            auto w = h.min;
            if (h.flex > 0.0 * one && size.w > used) {
                auto remaining = size.w - used;
                w = w + remaining * (h.flex.value() / total_flex.value());
            }
            widths.push_back(w);
        }

        Pos cursor = Pos::origin();
        for (std::size_t i = 0; i < children.size(); ++i) {
            auto w = widths[i];
            if (w.count() == 0)
                continue;
            auto child_size = Size{w, size.h};
            auto sub = subraster(raster, cursor, child_size);
            children[i].render(sub, child_size);
            cursor = cursor + w;
        }
    }
};

/// Overload of `column(...)` that accepts a runtime-sized vector of
/// type-erased children. Returns a `VStack`.
inline VStack column(std::vector<AnyLayout> children)
{
    return VStack{std::move(children)};
}

/// Overload of `row(...)` that accepts a runtime-sized vector of
/// type-erased children. Returns an `HStack`.
inline HStack row(std::vector<AnyLayout> children)
{
    return HStack{std::move(children)};
}

} // namespace nxt::tui
