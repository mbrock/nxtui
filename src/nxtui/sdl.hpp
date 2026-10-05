#pragma once

#include "nxtui/ui.hpp"

#include <memory>
#include <string>

struct SDL_Renderer;

namespace nxtui::ui {

/// SDL_Renderer + SDL3_ttf (HarfBuzz), not SDL_GPU. Initialize SDL/TTF
/// first. The renderer is borrowed and must outlive this object; frames
/// and shaped runs must be destroyed before this object. All UI/font work
/// stays on the SDL thread. The renderer uses its native output pixels.
class SdlPainter final : public TextMetrics
{
public:
    SdlPainter(
        SDL_Renderer * renderer,
        std::string font_file,
        float font_size = 20);
    ~SdlPainter() override;
    SdlPainter(const SdlPainter &) = delete;
    SdlPainter & operator=(const SdlPainter &) = delete;

    std::shared_ptr<const ShapedText> shape(
        std::string_view text,
        Width wrap_width,
        Emphasis emphasis) override;
    Size viewport() const;
    /// Convert SDL window event coordinates, including high-DPI scaling,
    /// to the same rhythm units used by layout and hit rectangles.
    Pos position(float window_x, float window_y) const;
    void draw(const Frame & frame);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nxtui::ui
