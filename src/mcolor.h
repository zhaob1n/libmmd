#pragma once
#include "base.h"

/* khroma 2.1.0's color engine, ported to the subset mermaid 12's themes call:
 * parse/stringify plus adjust, lighten, darken, invert, isDark and rgba.
 * Every entry point returns false where khroma throws "Unsupported color format"
 * (or where a channel setter would throw on a non-string), so the theme engine can
 * turn that into mtheme_new() == nullptr.  Strings come back in the arena.
 *
 * The parser follows khroma's regexes, including their quirks: RGB exponents carry
 * no sign ("1e3" yes, "1e-3" no), HSL hue takes deg/grad/rad/turn, a keyword parses
 * to its canonical hex (so lighten("red", 0) is "#ff0000", not "red"). */

/* channel deltas for mc_adjust(); mask bits 1=h 2=s 4=l 8=r 16=g 32=b */
typedef struct MCAmounts {
    uint8_t mask;
    double h, s, l, r, g, b;
} MCAmounts;

static inline MCAmounts mc_h(double h) { return (MCAmounts){.mask = 1, .h = h}; }
static inline MCAmounts mc_s(double s) { return (MCAmounts){.mask = 2, .s = s}; }
static inline MCAmounts mc_l(double l) { return (MCAmounts){.mask = 4, .l = l}; }
static inline MCAmounts mc_sl(double s, double l) { return (MCAmounts){.mask = 6, .s = s, .l = l}; }
static inline MCAmounts mc_hl(double h, double l) { return (MCAmounts){.mask = 5, .h = h, .l = l}; }
static inline MCAmounts mc_hsl(double h, double s, double l) { return (MCAmounts){.mask = 7, .h = h, .s = s, .l = l}; }
static inline MCAmounts mc_rgb(double r, double g, double b) { return (MCAmounts){.mask = 56, .r = r, .g = g, .b = b}; }

[[nodiscard]] bool mc_adjust(Arena a[static 1], Str col, MCAmounts am, Str out[static 1]);
[[nodiscard]] bool mc_lighten(Arena a[static 1], Str col, double amount, Str out[static 1]);
[[nodiscard]] bool mc_darken(Arena a[static 1], Str col, double amount, Str out[static 1]);
[[nodiscard]] bool mc_invert(Arena a[static 1], Str col, double weight, Str out[static 1]);
[[nodiscard]] bool mc_is_dark(Str col, bool out[static 1]);
void mc_rgba(Arena a[static 1], double r, double g, double b, double alpha, Str out[static 1]);
[[nodiscard]] bool mc_js_number(Str s, double out[static 1]);
