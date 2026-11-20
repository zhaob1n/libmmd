#pragma once
#include "base.h"

/* khroma 2.1.0's color engine, ported to the subset mermaid 12's themes call:
 * parse/stringify plus adjust, lighten (darken is lighten by -amount), invert and isDark.
 *
 * The parser follows khroma's regexes, including their quirks: RGB exponents carry
 * no sign ("1e3" yes, "1e-3" no), HSL hue takes deg/grad/rad/turn, a keyword parses
 * to its canonical hex (so lighten("red", 0) is "#ff0000", not "red").
 *
 * khroma hands a colour on as text, which the next call parses again; an MColor is what that
 * parse yields, so a chain of calls needs neither the text nor the parse. A result's text is
 * printed when mc_str() first asks for it, into the MColor. */

/* channel deltas for mc_adjust(); mask bits 1=h 2=s 4=l 8=r 16=g 32=b */
typedef struct MCAmounts {
    uint8_t mask;
    double h, s, l, r, g, b;
} MCAmounts;

enum { MC_TEXT = 96 }; /* room for any text mc_str() prints */
typedef struct MColor {
    double c[3], a; /* r, g, b, or h, s, l where hsl */
    Str str;        /* the text it was parsed from, if any */
    bool hsl;
    bool kw;        /* str is a keyword, which khroma hands on as its hex */
    bool bad;       /* its text does not parse back: khroma throws where the colour is used next */
    unsigned char fmt, n; /* how mc_str() prints it; the length printed into buf, 0 for not yet */
    char buf[MC_TEXT];
} MColor;

/* false where khroma throws "Unsupported color format" */
[[nodiscard]] bool mc_parse(Str s, MColor out[static 1]);
/* the colour's text: str, or printed into buf on first use */
Str mc_str(MColor c[static 1]);
/* the operations, on a colour that is not bad */
void mc_adjust(const MColor col[static 1], MCAmounts am, MColor out[static 1]);
void mc_lighten(const MColor col[static 1], double amount, MColor out[static 1]); /* darken: -amount */
void mc_invert(const MColor col[static 1], double weight, MColor out[static 1]);
bool mc_is_dark(const MColor col[static 1]);
[[nodiscard]] bool mc_js_number(Str s, double out[static 1]);
