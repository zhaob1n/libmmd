#ifndef CMM_TEXT_H
#define CMM_TEXT_H
#include "base.h"

/* Width in px of a single line of UTF-8 text at font size fs. */
[[nodiscard]] double text_width(Str s, double fs);
[[nodiscard]] uint32_t ent_lookup(size_t n, const char s[static n]);
/* Normalise a label: strip quotes/markdown markers, convert <br> to '\n'.
 * Returns a view into the source when no rewrite is needed. */
[[nodiscard]] Str label_normalize(Arena *a, Str raw, bool *ok);
/* Measure a normalised label: max line width and line count. */
void label_measure(Str s, double fs, double *w, unsigned *lines);
[[nodiscard]] double text_width_os(Str s, double fs);
void label_measure_f(Str s, double fs, double (*meas)(Str, double), double *w, unsigned *lines);
/* iterate lines: returns next line, advances *pos; false at end */
bool label_next_line(Str s, size_t *pos, Str *line);
#endif
