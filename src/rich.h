#ifndef MMD_RICH_H
#define MMD_RICH_H
/* Rich flowchart labels: markdown emphasis, auto-wrap, fa: icons.
 * Encoded label format (all in-band, never reaches XML raw):
 *   RICH_MD   leading byte: label came from a markdown string ("`...`"), auto-wraps
 *   RICH_BOLD toggles bold, RICH_ITAL toggles italic
 *   "fa:fa-name" / "fab:fa-name" ... tokens render as Font Awesome glyphs */
#include "base.h"

constexpr char RICH_MD = '\x01';
constexpr char RICH_BOLD = '\x02';
constexpr char RICH_ITAL = '\x03';
constexpr double RICH_WRAP_PX = 200.0; /* mermaid flowchart wrappingWidth at 16px */

/* like label_normalize, but keeps markdown emphasis as toggles */
[[nodiscard]] Str rich_normalize(Arena a[static 1], Str raw, bool ok[static 1]);
/* true if the label needs the rich measuring/emitting path */
bool rich_needed(Str s);
/* greedy word wrap of a RICH_MD label at maxw px; returns s unchanged if not md */
[[nodiscard]] Str rich_wrap(Arena a[static 1], Str s, double fs, double maxw, bool ok[static 1]);
void rich_measure(Str s, double fs, double w[static 1], unsigned lines[static 1]);
/* emit one line's content as XML text/tspans; bold/italic state carries across lines */
void rich_emit_line(Buf b[static 1], Str line, bool bold[static 1], bool ital[static 1]);
/* fa token at s[i..]: returns token length (0 if none) and glyph codepoint */
size_t rich_fa_token(Str s, size_t i, uint32_t cp[static 1]);
#endif
