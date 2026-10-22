/* Embedded icon paths: logo subset (architecture `logos:x`) and Font Awesome solid subset (mindmap
 * `::icon(fa fa-x)`). See src/icons_data.h for sources and licences. */
#pragma once
#include "base.h"

/* Draw `name` fitted into the s×s tile at (x, y), filled by CSS class `cls`. Accepts `logos:x`,
 * `simple-icons:x`, `fa:x`, `fa fa-x`, `fas fa-x`, `fa-x` or a bare name. Returns false if unknown
 * (nothing written). */
[[nodiscard]] bool icon_draw(Buf *b, Str name, double x, double y, double s, const char *cls);
