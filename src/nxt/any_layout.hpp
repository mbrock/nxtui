#pragma once

#include "nxt/layout.hpp"

#include <memory>
#include <type_traits>
#include <utility>

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

} // namespace nxt::tui
