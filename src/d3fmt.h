/* d3-format 3.1 in d3's default (en-US) locale: the number formats mermaid's treemap takes as
 * treemap.valueFormat, "," by default. Formats under render()'s C locale. */
#pragma once
#include "base.h"

typedef struct D3Fmt {
    char fill[4];  /* one BMP character, UTF-8 */
    uint8_t fill_n;
    char align, sign, symbol, type;
    bool zero, comma, trim;
    int width; /* -1: none */
    int precision;
} D3Fmt;

/* [[fill]align][sign][symbol][0][width][,][.precision][~][type], set up as d3's format(spec);
 * false where d3 throws "invalid format" */
[[nodiscard]] bool d3fmt_parse(Str spec, D3Fmt f[static 1]);
/* append format(spec)(v) */
void d3fmt(Buf b[static 1], const D3Fmt f[static 1], double v);
