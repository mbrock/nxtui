// Baltic color palettes — light (birch) and dark (church) variants.
//
// Ported from the Emacs themes baltic-birch-theme.el and
// baltic-church-theme.el.  The shared `Palette` shape names every slot
// used by either theme; the dark variant has no separate chocolate /
// navy hues, so those slots reuse amber / cyan (matching what the Emacs
// theme actually does for the corresponding font-lock faces).

#pragma once

#include "nxt/raster.hpp"

namespace nxt::theme {

struct Palette
{
    // Surface colors used for terminal-default substitutions and UI chrome.
    Rgb8 bg;              // default background
    Rgb8 bg_soft;         // surface (header-line, blocks)
    Rgb8 bg_elev;         // elevated surface (tooltips, hunk heading)
    Rgb8 bg_active;       // active line
    Rgb8 bg_hl;           // hover / generic highlight
    Rgb8 border;
    Rgb8 border_soft;

    // Mode-line slots mirror the source theme names.
    Rgb8 ml_bg;
    Rgb8 ml_box;
    Rgb8 ml_inactive_bg;
    Rgb8 ml_inactive_box;

    // Foreground ramp, from primary text to low-emphasis comments.
    Rgb8 fg;
    Rgb8 fg_muted;
    Rgb8 fg_subtle;
    Rgb8 fg_comment;

    // Semantic token colors reused by demos and screenshots.
    Rgb8 amber;           // strings
    Rgb8 amber_bg;        // const / number tint
    Rgb8 amber_deep;      // const / number fg
    Rgb8 coral;           // invalid / error
    Rgb8 coral_bg;
    Rgb8 pink;            // escapes / special punctuation
    Rgb8 mint;            // emphasis / attribute
    Rgb8 cyan;            // function / link
    Rgb8 cyan_soft;       // type
    Rgb8 cyan_bg;         // type tint
    Rgb8 regex;           // regex teal
    Rgb8 green;           // keyword
    Rgb8 green_bg;        // keyword tint
    Rgb8 green_bright;    // tag / selector / diff+
    Rgb8 doc_orange;      // doc comment
    Rgb8 chocolate;       // variable
    Rgb8 chocolate_soft;  // property / parameter
    Rgb8 navy;            // accent (minibuffer prompt etc.)

    // Interaction and diff backgrounds.
    Rgb8 sel_bg;
    Rgb8 match_bg;
    Rgb8 symbol_hl;
    Rgb8 added_bg;
    Rgb8 added_hl_bg;
    Rgb8 removed_bg;
    Rgb8 removed_hl_bg;
};

/// Light Baltic theme: warm birch-cream ground with category-tinted tokens.
inline constexpr Palette baltic_birch{
    .bg              = {0xff, 0xfd, 0xf6},
    .bg_soft         = {0xeb, 0xe8, 0xdd},
    .bg_elev         = {0xe2, 0xde, 0xd2},
    .bg_active       = {0xf4, 0xf1, 0xe9},
    .bg_hl           = {0xde, 0xd9, 0xcc},
    .border          = {0xbf, 0xb6, 0xa3},
    .border_soft     = {0xd4, 0xcc, 0xbd},

    .ml_bg           = {0xe2, 0xde, 0xd2},
    .ml_box          = {0xa9, 0x9c, 0x84},
    .ml_inactive_bg  = {0xeb, 0xe8, 0xdd},
    .ml_inactive_box = {0xce, 0xc6, 0xb6},

    .fg              = {0x24, 0x18, 0x0a},
    .fg_muted        = {0x76, 0x6b, 0x58},
    .fg_subtle       = {0x92, 0x86, 0x71},
    .fg_comment      = {0x76, 0x67, 0x44},

    .amber           = {0x9a, 0x5a, 0x00},
    .amber_bg        = {0xff, 0xe8, 0xd0},
    .amber_deep      = {0xc2, 0x4b, 0x2c},
    .coral           = {0xc7, 0x35, 0x2b},
    .coral_bg        = {0xff, 0xd9, 0xd2},
    .pink            = {0xa8, 0x3a, 0xba},
    .mint            = {0x00, 0x8a, 0x7a},
    .cyan            = {0x16, 0x48, 0xc7},
    .cyan_soft       = {0x00, 0x6f, 0xb0},
    .cyan_bg         = {0xdc, 0xf6, 0xf5},
    .regex           = {0x00, 0x74, 0x8c},
    .green           = {0x3e, 0x5f, 0x00},
    .green_bg        = {0xe8, 0xf9, 0xcf},
    .green_bright    = {0x2b, 0x86, 0x3a},
    .doc_orange      = {0xc5, 0x5f, 0x00},
    .chocolate       = {0x70, 0x42, 0x00},
    .chocolate_soft  = {0x7d, 0x4b, 0x00},
    .navy            = {0x00, 0x00, 0x6b},

    .sel_bg          = {0xdc, 0xd9, 0xe8},
    .match_bg        = {0xf4, 0xe0, 0xc8},
    .symbol_hl       = {0xff, 0xf0, 0xa8},
    .added_bg        = {0xdf, 0xf5, 0xcf},
    .added_hl_bg     = {0xc8, 0xe8, 0xb3},
    .removed_bg      = {0xf8, 0xd0, 0xc6},
    .removed_hl_bg   = {0xf0, 0xb6, 0xa7},
};

/// Dark Baltic theme: linen text on near-black with category-tinted tokens.
///
/// The dark theme has no distinct chocolate / navy hues — those slots
/// are filled with amber / cyan to mirror the Emacs theme, where
/// font-lock-variable-name-face uses amber and the cursor / prompt use
/// cyan.
inline constexpr Palette baltic_church{
    .bg              = {0x00, 0x00, 0x00},
    .bg_soft         = {0x01, 0x01, 0x01},
    .bg_elev         = {0x09, 0x09, 0x09},
    .bg_active       = {0x0a, 0x0a, 0x0a},
    .bg_hl           = {0x15, 0x15, 0x15},
    .border          = {0x19, 0x19, 0x19},
    .border_soft     = {0x11, 0x11, 0x11},

    .ml_bg           = {0x1c, 0x1a, 0x16},
    .ml_box          = {0x2c, 0x29, 0x24},
    .ml_inactive_bg  = {0x0d, 0x0c, 0x0a},
    .ml_inactive_box = {0x1a, 0x18, 0x14},

    .fg              = {0xda, 0xcc, 0xb1},
    .fg_muted        = {0xaa, 0x9d, 0x83},
    .fg_subtle       = {0x60, 0x54, 0x3c},
    .fg_comment      = {0xca, 0xbc, 0xa1},

    .amber           = {0xf8, 0xb0, 0x00},
    .amber_bg        = {0x2b, 0x1e, 0x00},
    .amber_deep      = {0xf8, 0xb0, 0x00},  // no separate deep variant
    .coral           = {0xff, 0x81, 0x7a},
    .coral_bg        = {0x3a, 0x18, 0x15},
    .pink            = {0xf5, 0x8e, 0xff},
    .mint            = {0x00, 0xe5, 0xb6},
    .cyan            = {0x3f, 0xc3, 0xff},
    .cyan_soft       = {0x00, 0xce, 0xff},
    .cyan_bg         = {0x00, 0x26, 0x33},
    .regex           = {0x00, 0xe2, 0xe5},
    .green           = {0x3c, 0xe0, 0x74},
    .green_bg        = {0x0e, 0x32, 0x20},
    .green_bright    = {0x3c, 0xe0, 0x74},  // no separate bright variant
    .doc_orange      = {0xff, 0x93, 0x00},
    .chocolate       = {0xf8, 0xb0, 0x00},  // dark theme uses amber here
    .chocolate_soft  = {0xf8, 0xb0, 0x00},
    .navy            = {0x3f, 0xc3, 0xff},  // dark theme uses cyan here

    .sel_bg          = {0x0f, 0x2c, 0x3c},
    .match_bg        = {0x6f, 0x4f, 0x00},
    .symbol_hl       = {0x3a, 0x2a, 0x00},
    .added_bg        = {0x0e, 0x24, 0x18},
    .added_hl_bg     = {0x14, 0x33, 0x23},
    .removed_bg      = {0x3a, 0x18, 0x15},
    .removed_hl_bg   = {0x4a, 0x1d, 0x1a},
};

}  // namespace nxt::theme
