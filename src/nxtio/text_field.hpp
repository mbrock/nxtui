// KeyEvent → TextField bridge.  Lives separately from text_field.hpp
// so the rendering side has no dependency on the input subsystem.

#pragma once

#include "nxt/text_field.hpp"
#include "nxtio/input.hpp"

namespace nxt::tui {

/// Apply a single key event to the field.  Handles text insertion
/// and the standard editing keys (backspace, delete, arrows, home,
/// end).  Returns true if the field changed (caller should redraw).
///
/// Release events and unknown keys return false; the caller is free
/// to handle Enter / Escape / etc. itself before or after this call.
[[nodiscard]] inline bool
apply_key(TextField & field, const nxt::input::KeyEvent & event)
{
    using nxt::input::EventType;
    using nxt::input::Key;

    if (event.type == EventType::release)
        return false;

    if (event.is_text()) {
        if (event.text.empty())
            return false;
        field.insert(event.text);
        return true;
    }

    switch (event.key) {
    case Key::backspace:
        return field.erase_left();
    case Key::delete_:
        return field.erase_right();
    case Key::left:
        return field.move_left();
    case Key::right:
        return field.move_right();
    case Key::home:
        return field.move_home();
    case Key::end:
        return field.move_end();
    default:
        return false;
    }
}

}  // namespace nxt::tui
