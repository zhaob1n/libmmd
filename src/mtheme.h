/* mermaid's theme engine: the five classic themes' constructors, updateColors() and calculate(),
 * generated from mermaid's own sources into src/mtheme_gen.h (tools/gen_mtheme.mjs), over the
 * khroma colour maths they call (src/mcolor.c). mtheme_get() answers with the value mermaid's
 * themes[id].getThemeVariables(overrides) holds, but for nested overrides (below);
 * tests/internal/mtheme.c checks every variable of ~1000 override sets against mermaid itself. */
#pragma once
#include "base.h"

typedef enum MThemeId : unsigned char { MT_DEFAULT, MT_BASE, MT_DARK, MT_FOREST, MT_NEUTRAL } MThemeId;
typedef struct MTheme MTheme;

/* overrides: n keys and values (nullable when n is 0), the themeVariables object: keys flat ("primaryColor") or dotted for a nested object's
 * field ("xyChart.titleColor"), values the strings YAML/JSON gives. As mermaid's config reads YAML,
 * "true"/"false" are booleans and JavaScript numerals ("16", "0.5", "0x10") numbers; anything else
 * is a string. A nested override sets its field and leaves the object's others as the theme derives
 * them; mermaid's calculate() replaces the whole object (xyChart.titleColor alone leaves xyChart
 * with that one field), which libmmd does not copy.
 * nullptr when the arena runs out (a->oom says so), where getThemeVariables throws (an override
 * that a derivation reaches but khroma cannot read, e.g. primaryColor: bogus, or a non-string
 * where a colour belongs, e.g. primaryColor: true), past 512 variables, and for a value of 4 GiB
 * or more. The theme lives in a and reads its overrides' strings in place. */
[[nodiscard]] MTheme *mtheme_new(Arena a[static 1], MThemeId id, size_t n, const Str *keys, const Str *vals);
/* the variable as getThemeVariables leaves it: strings as is, numbers and booleans as String(v);
 * empty when undefined or when key names an object ("xyChart"). A colour mtheme_new() derived is
 * printed into the theme when first asked for, so one theme is not for two threads at once. */
[[gnu::nonnull(1)]] Str mtheme_get(const MTheme *t, Str key);
/* mtheme_new(id) without overrides, a constant table (any thread may read it); nullptr only for
 * an id out of range */
const MTheme *mtheme_preset(MThemeId id);
