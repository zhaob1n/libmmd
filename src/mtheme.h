/* mermaid's theme engine: the five classic themes' constructors, updateColors() and calculate(),
 * generated from mermaid's own sources into src/mtheme_gen.h (tools/gen_mtheme.mjs), over the
 * khroma colour maths they call (src/mcolor.c). mtheme_get() answers with the value mermaid's
 * themes[id].getThemeVariables(overrides) holds; tests/mtheme_test.c checks every variable of
 * ~1000 override sets against mermaid itself. */
#pragma once
#include "base.h"

typedef enum MThemeId : unsigned char { MT_DEFAULT, MT_BASE, MT_DARK, MT_FOREST, MT_NEUTRAL } MThemeId;
typedef struct MTheme MTheme;

/* overrides: n keys and values (nullable when n is 0), the themeVariables object: keys flat ("primaryColor") or dotted for a nested object's
 * field ("xyChart.titleColor"), values the strings YAML/JSON gives. As mermaid's config reads YAML,
 * "true"/"false" are booleans and JavaScript numerals ("16", "0.5", "0x10") numbers; anything else
 * is a string. As calculate() assigns each top-level key, a nested override replaces the theme's
 * whole object (xyChart.titleColor alone leaves xyChart with that one field).
 * nullptr when the arena runs out (a->oom says so), where getThemeVariables throws (an override
 * that a derivation reaches but khroma cannot read, e.g. primaryColor: bogus, or a non-string
 * where a colour belongs, e.g. primaryColor: true), and past 512 variables. */
[[nodiscard]] MTheme *mtheme_new(Arena a[static 1], MThemeId id, size_t n, const Str *keys, const Str *vals);
/* the variable as getThemeVariables leaves it: strings as is, numbers and booleans as String(v);
 * empty when undefined or when key names an object ("xyChart") */
Str mtheme_get(const MTheme *t, Str key);
/* mtheme_new(id) without overrides, computed once per process (thread-safe); nullptr only when
 * memory ran out, and then the next call tries again */
const MTheme *mtheme_preset(MThemeId id);
