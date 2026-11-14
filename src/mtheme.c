/* mtheme.c: the runtime under mermaid's themes, which src/mtheme_gen.h (tools/gen_mtheme.mjs)
 * holds as C: a theme object (string keys, JavaScript values), the JavaScript operators the
 * themes use, the khroma calls (src/mcolor.c), and the calculate() variants. See mtheme.h. */
#include "mtheme.h"
#include "mcolor.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef enum : unsigned char { V_UNDEF, V_STR, V_NUM, V_BOOL } VKind;
typedef struct Val { VKind k; bool b; double num; Str s; } Val;
#define VUNDEF ((Val){.k = V_UNDEF})

typedef struct Prop { Str key; uint32_t h; Val val; } Prop; /* key.p == nullptr: a free slot */

enum { SLOTS = 1024, MAX_ROOTS = 16 }; /* the themes hold about 300 variables */

struct MTheme {
    Arena *a;
    Prop *v; /* open addressing, linear probing */
    size_t n;
    Str roots[MAX_ROOTS]; /* keys that hold an object: "xyChart", "radar" ... */
    unsigned nroots;
    bool ok; /* false once a khroma call fails (mermaid throws there) or the table is full */
};

/* ---- the object ---- */

static uint32_t hash(Str key) { /* FNV-1a; tools/gen_mtheme.mjs hashes the literal keys the same way */
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < key.n; i++) h = (h ^ (unsigned char)key.p[i]) * 16777619u;
    return h;
}

typedef struct Key { Str s; uint32_t h; } Key;
#define K(lit, hv) ((Key){.s = S(lit), .h = (hv)})
static Key key_of(Str s) { return (Key){.s = s, .h = hash(s)}; }

/* the slot holding key, or the free slot where it would go */
static size_t find(const MTheme t[static 1], Key k) {
    size_t i = k.h & (SLOTS - 1);
    while (t->v[i].key.p && (t->v[i].h != k.h || !str_eq(t->v[i].key, k.s))) i = (i + 1) & (SLOTS - 1);
    return i;
}

static Val get(const MTheme t[static 1], Key k) {
    const Prop *p = &t->v[find(t, k)];
    return p->key.p ? p->val : VUNDEF;
}

static bool is_field(Str k, Str of) { return k.n > of.n && k.p[of.n] == '.' && !memcmp(k.p, of.p, of.n); }

static void unset(MTheme t[static 1], size_t i) { /* backward-shift deletion keeps probe runs whole */
    t->v[i].key.p = nullptr;
    t->n--;
    for (size_t j = (i + 1) & (SLOTS - 1); t->v[j].key.p; j = (j + 1) & (SLOTS - 1)) {
        size_t home = t->v[j].h & (SLOTS - 1);
        if (((j - home) & (SLOTS - 1)) < ((j - i) & (SLOTS - 1))) continue; /* still reachable */
        t->v[i] = t->v[j], t->v[j].key.p = nullptr, i = j;
    }
}

static unsigned root_of(const MTheme t[static 1], Str key) { /* its index in roots, nroots if none */
    unsigned r = 0;
    while (r < t->nroots && !str_eq(t->roots[r], key)) r++;
    return r;
}

/* delete key and, when it held an object, the object's fields ("key.*") */
static void drop(MTheme t[static 1], Key k) {
    size_t i = find(t, k);
    if (t->v[i].key.p) unset(t, i);
    unsigned r = root_of(t, k.s);
    if (r == t->nroots) return;
    t->roots[r] = t->roots[--t->nroots];
    for (size_t j = 0; j < SLOTS;) /* rare: an object replaced */
        if (t->v[j].key.p && is_field(t->v[j].key, k.s)) unset(t, j); else j++;
}

/* this[key] = v: a plain value replaces whatever object key held; undefined is stored as absence */
static void put(MTheme t[static 1], Key k, Val v) {
    if (!t->ok) return;
    size_t i = find(t, k);
    if (root_of(t, k.s) < t->nroots) drop(t, k), i = find(t, k);
    else if (t->v[i].key.p) { /* a plain value: replace it */
        if (v.k != V_UNDEF) t->v[i].val = v; else unset(t, i);
        return;
    }
    if (v.k == V_UNDEF) return;
    const char *dot = memchr(k.s.p, '.', k.s.n);
    if (dot) { /* a field: its object is a root to drop with */
        Str obj = {.p = k.s.p, .n = (size_t)(dot - k.s.p)};
        if (root_of(t, obj) == t->nroots) {
            if (t->nroots == MAX_ROOTS) { t->ok = false; return; }
            t->roots[t->nroots++] = obj;
        }
    }
    if (t->n >= SLOTS / 2) { t->ok = false; return; }
    t->v[i] = (Prop){.key = k.s, .h = k.h, .val = v};
    t->n++;
}

/* ---- JavaScript values ---- */

static Val vstr(Str s) { return (Val){.k = V_STR, .s = s}; }
static Val vnum(double d) { return (Val){.k = V_NUM, .num = d}; }
static Val vbool(bool b) { return (Val){.k = V_BOOL, .b = b}; }

static bool truthy(Val v) {
    switch (v.k) {
    case V_STR: return v.s.n > 0;
    case V_NUM: return v.num != 0 && !isnan(v.num);
    case V_BOOL: return v.b;
    default: return false;
    }
}

static double to_num(Val v) {
    double d = NAN;
    switch (v.k) {
    case V_NUM: return v.num;
    case V_BOOL: return v.b;
    case V_STR: return mc_js_number(v.s, &d) ? d : NAN;
    default: return NAN;
    }
}

/* String(v), in the arena */
static Str js_str(MTheme t[static 1], Val v) {
    char b[32];
    switch (v.k) {
    case V_STR: return v.s;
    case V_BOOL: return v.b ? S("true") : S("false");
    case V_NUM: return arena_strdup(t->a, (Str){.p = b, .n = fmt_js(v.num, b)});
    default: return S("undefined");
    }
}

/* a string where khroma or a property name needs one; khroma throws on anything else */
static Str tstr(MTheme t[static 1], Val v) {
    if (v.k != V_STR) t->ok = false;
    return v.k == V_STR ? v.s : S("");
}

static Val js_add(MTheme t[static 1], Val a, Val b) {
    if (a.k != V_STR && b.k != V_STR) return vnum(to_num(a) + to_num(b));
    Str x = js_str(t, a), y = js_str(t, b);
    char *p = ANEW(t->a, char, x.n + y.n + 1);
    if (!p) { t->ok = false; return VUNDEF; }
    if (x.n) memcpy(p, x.p, x.n); /* x.p is null where js_str ran out of memory */
    if (y.n) memcpy(p + x.n, y.p, y.n);
    p[x.n + y.n] = 0;
    return vstr((Str){.p = p, .n = x.n + y.n});
}

static bool js_seq(Val a, Val b) {
    if (a.k != b.k) return false;
    switch (a.k) {
    case V_STR: return str_eq(a.s, b.s);
    case V_NUM: return a.num == b.num;
    case V_BOOL: return a.b == b.b;
    default: return true;
    }
}

/* a < b (or <=): strings by code unit when both are strings, numbers otherwise, NaN unordered */
static bool js_lt(Val a, Val b, bool eq) {
    if (a.k == V_STR && b.k == V_STR) { /* by UTF-16 code unit, which byte order matches below U+10000 */
        size_t n = a.s.n < b.s.n ? a.s.n : b.s.n;
        int c = n ? memcmp(a.s.p, b.s.p, n) : 0;
        if (!c) c = (a.s.n > b.s.n) - (a.s.n < b.s.n);
        return eq ? c <= 0 : c < 0;
    }
    double x = to_num(a), y = to_num(b);
    return eq ? x <= y : x < y;
}

/* ---- khroma ---- */

static Val colour(MTheme t[static 1], bool ok, Str out) {
    if (!ok) { t->ok = false; return VUNDEF; }
    return vstr(out);
}
static Val cadjust(MTheme t[static 1], Str c, MCAmounts am) { Str o = {}; return colour(t, t->ok && mc_adjust(t->a, c, am, &o), o); }
static Val clighten(MTheme t[static 1], Str c, double amt) { Str o = {}; return colour(t, t->ok && mc_lighten(t->a, c, amt, &o), o); }
static Val cdarken(MTheme t[static 1], Str c, double amt) { Str o = {}; return colour(t, t->ok && mc_darken(t->a, c, amt, &o), o); }
static Val cinvert(MTheme t[static 1], Str c) { Str o = {}; return colour(t, t->ok && mc_invert(t->a, c, 100, &o), o); }
static Val cis_dark(MTheme t[static 1], Str c) {
    bool d = false;
    if (t->ok && !mc_is_dark(c, &d)) t->ok = false;
    return vbool(d);
}
static Val crgba(MTheme t[static 1], double r, double g, double b, double a) {
    Str o = {};
    mc_rgba(t->a, r, g, b, a, &o);
    return colour(t, o.p, o);
}
/* theme-helpers.js (gen_mtheme.mjs checks it has not changed) */
static Val mkborder(MTheme t[static 1], Str c, bool dark) { return cadjust(t, c, (MCAmounts){.mask = 6, .s = -40, .l = dark ? 10 : -10}); }

/* ---- the themes ---- */

typedef enum Calc : unsigned char { CALC_COPY, CALC_RESET_COPY, CALC_COPY_GRADIENT } Calc;
#include "mtheme_gen.h"

typedef struct ThemeDef { void (*ctor)(MTheme *), (*update)(MTheme *); Calc calc; } ThemeDef;
static const ThemeDef DEFS[] = {
    [MT_DEFAULT] = {default_ctor, default_update, default_calc},
    [MT_BASE] = {base_ctor, base_update, base_calc},
    [MT_DARK] = {dark_ctor, dark_update, dark_calc},
    [MT_FOREST] = {forest_ctor, forest_update, forest_calc},
    [MT_NEUTRAL] = {neutral_ctor, neutral_update, neutral_calc},
};

/* YAML's reading of a scalar: true/false, a number, else the string */
static Val scalar(Str s) {
    double d;
    if (str_eq(s, S("true")) || str_eq(s, S("false"))) return vbool(s.p[0] == 't');
    return str_trim(s).n && mc_js_number(s, &d) ? vnum(d) : vstr(s);
}

/* calculate()'s keys.forEach((k) => { this[k] = overrides[k]; }), the last value for each key
 * winning, with one deliberate difference: mermaid assigns each top-level key, so a nested override
 * (xyChart.titleColor) replaces the theme's whole object and a dark xychart loses every other dark
 * colour; here it sets that field, and the object keeps the rest */
static void copy_overrides(MTheme t[static 1], size_t n, const Str *keys, const Str *vals) {
    for (size_t i = 0; i < n; i++) {
        bool later = false;
        for (size_t m = i + 1; m < n && !later; m++) later = str_eq(keys[m], keys[i]);
        if (later) continue;
        const char *dot = memchr(keys[i].p, '.', keys[i].n);
        Key top = key_of((Str){.p = keys[i].p, .n = dot ? (size_t)(dot - keys[i].p) : 0});
        if (dot && root_of(t, top.s) == t->nroots) drop(t, top); /* a field turns a plain value into an object */
        put(t, key_of(keys[i]), scalar(vals[i]));
    }
}

MTheme *mtheme_new(Arena a[static 1], MThemeId id, size_t n, const Str *keys, const Str *vals) {
    MTheme *t = ANEW(a, MTheme, 1);
    Prop *v = ANEW(a, Prop, SLOTS);
    if (!t || !v || id >= countof(DEFS)) return nullptr;
    memset(v, 0, SLOTS * sizeof *v);
    *t = (MTheme){.a = a, .v = v, .ok = true};
    const ThemeDef *d = &DEFS[id];
    d->ctor(t);
    if (d->calc == CALC_RESET_COPY) { /* the default theme's placeholders become undefined */
        Key calc[SLOTS / 2];
        size_t nc = 0;
        for (size_t i = 0; i < SLOTS; i++)
            if (t->v[i].key.p && t->v[i].val.k == V_STR && str_eq(t->v[i].val.s, S("calculated")))
                calc[nc++] = (Key){.s = t->v[i].key, .h = t->v[i].h};
        for (size_t i = 0; i < nc; i++) drop(t, calc[i]);
    }
    copy_overrides(t, n, keys, vals);
    d->update(t);
    copy_overrides(t, n, keys, vals);
    if (d->calc == CALC_COPY_GRADIENT) {
        bool border = false, gradient = false;
        for (size_t i = 0; i < n; i++)
            border |= str_eq(keys[i], S("nodeBorder")) || is_field(keys[i], S("nodeBorder")),
            gradient |= str_eq(keys[i], S("useGradient")) || is_field(keys[i], S("useGradient"));
        if (border && !gradient) put(t, key_of(S("useGradient")), vbool(false));
    }
    return t->ok && !a->oom ? t : nullptr;
}

Str mtheme_get(const MTheme *t, Str key) {
    Val v = get(t, key_of(key));
    switch (v.k) {
    case V_STR: return v.s;
    case V_BOOL: return v.b ? S("true") : S("false");
    case V_NUM: { /* rare (radius, fontSize as a number ...): the arena is not reachable from a const theme */
        static thread_local char b[8][32];
        static thread_local unsigned slot;
        char *o = b[slot++ % countof(b)];
        return (Str){.p = o, .n = fmt_js(v.num, o)};
    }
    default: return (Str){};
    }
}

const MTheme *mtheme_preset(MThemeId id) {
    static _Atomic(MTheme *) cache[countof(DEFS)];
    if (id >= countof(DEFS)) return nullptr;
    MTheme *t = atomic_load_explicit(&cache[id], memory_order_acquire);
    if (t) return t;
    Arena *a = calloc(1, sizeof *a); /* the cache owns it for the life of the process */
    if (!a) return nullptr;
    MTheme *seen = nullptr;
    if ((t = mtheme_new(a, id, 0, nullptr, nullptr)) && atomic_compare_exchange_strong(&cache[id], &seen, t)) return t;
    arena_free(a); /* out of memory, or another thread was first */
    free(a);
    return seen;
}
