/* mtheme.c: the runtime under mermaid's themes, which src/mtheme_gen.h (tools/gen_mtheme.mjs)
 * holds as C: the theme object, a slot for each variable mermaid's themes know and a list for any
 * other; the JavaScript operators updateColors() uses; the khroma calls (src/mcolor.c), on colours
 * kept parsed; and the calculate() variants. See mtheme.h. */
#include "mtheme.h"
#include "mcolor.h"

#include <math.h>
#include <string.h>

/* a JavaScript value; V_COL is a string khroma made or read, kept as khroma parses it. A string
 * is p and n, n beside k so that a Val is two words. */
typedef enum : unsigned char { V_UNDEF, V_STR, V_NUM, V_BOOL, V_COL } VKind;
typedef struct Val {
    VKind k;
    uint32_t n;
    union { bool b; double num; const char *p; MColor *c; };
} Val;
static_assert(sizeof(Val) == 16);
#define VUNDEF ((Val){})
#define VS(lit) {.k = V_STR, .n = sizeof(lit) - 1, .p = (lit)}
#define VN(d) {.k = V_NUM, .num = (d)}
#define VB(x) {.k = V_BOOL, .b = (x)}

enum { NONE = 0xffff, MAX_VARS = 512 };
typedef struct Key { unsigned slot; Str name; } Key; /* slot NONE: a variable outside the slots */
typedef struct Extra { Str key; Val v; } Extra;

/* the object getThemeVariables() builds: this[key] is v[slot], or an extra */
typedef struct Th {
    Arena *a;
    Val *v;
    bool *obj;      /* the slot's name holds an object: "xyChart", "radar" ... */
    VEC(Extra) x;
    VEC(Str) xobj;  /* the names outside the slots that hold an object */
    bool ok;        /* false once khroma throws (mermaid throws there) or past MAX_VARS */
    MColor dummy;   /* what a khroma call that throws reads on */
} Th;

typedef struct Obj { uint16_t root, lo, hi; } Obj;               /* its fields: slots lo..hi-1 */
typedef struct Fam { Str prefix; uint16_t n; const uint16_t *slot; } Fam; /* this[prefix + i] */
typedef enum Calc : unsigned char { CALC_COPY, CALC_COPY_GRADIENT } Calc;

/* the variables as strings: V_STR, a colour (V_COL) printed on first read, or V_UNDEF */
struct MTheme {
    const Val *v; /* per slot */
    size_t nx;
    const Extra *x;
};

typedef struct ThemeDef {
    const Val *init; /* the variables its constructor leaves */
    void (*update)(Th[static 1]);
    Calc calc;
    MTheme preset;
    unsigned nroots;
    const uint16_t *roots; /* the objects in init */
} ThemeDef;

/* ---- the object ---- */

#define G(s) (t->v[s])
static void drop(Th t[static 1], unsigned s);

/* this[key] = v, key a slot that holds no object's field: a plain value replaces an object */
[[gnu::noinline]] static void put(Th t[static 1], unsigned s, Val v) {
    if (MMD_UNLIKELY(t->obj[s])) drop(t, s);
    t->v[s] = v;
}

/* this[root][field] = v, the field's slot s */
static void putf(Th t[static 1], unsigned s, unsigned root, Val v) {
    if (v.k != V_UNDEF) t->obj[root] = true;
    t->v[s] = v;
}

static bool is_field(Str k, Str of) { return k.n > of.n && k.p[of.n] == '.' && !memcmp(k.p, of.p, of.n); }

static Extra *xfind(Th t[static 1], Str key) {
    for (size_t i = 0; i < t->x.n; i++)
        if (str_eq(t->x.v[i].key, key)) return &t->x.v[i];
    return nullptr;
}

static size_t xobj_of(const Th t[static 1], Str key) { /* its index in xobj, xobj.n if none */
    size_t i = 0;
    while (i < t->xobj.n && !str_eq(t->xobj.v[i], key)) i++;
    return i;
}

static void xdrop_fields(Th t[static 1], Str root) {
    size_t j = 0;
    for (size_t i = 0; i < t->x.n; i++)
        if (!is_field(t->x.v[i].key, root)) t->x.v[j++] = t->x.v[i];
    t->x.n = j;
}

/* ---- JavaScript values ---- */

static Val vstr(Str s) { return (Val){.k = V_STR, .n = (uint32_t)s.n, .p = s.p}; } /* s.n < 4 GiB */
static Str sv(Val v) { return (Str){.p = v.p, .n = v.n}; }
static Val vnum(double d) { return (Val){.k = V_NUM, .num = d}; }
static Val vbool(bool b) { return (Val){.k = V_BOOL, .b = b}; }
static bool is_str(Val v) { return v.k == V_STR || v.k == V_COL; }

static bool truthy_num(double d) { return d != 0 && !isnan(d); }

[[gnu::noinline]] static bool truthy(Val v) {
    switch (v.k) {
    case V_STR: return v.n > 0;
    case V_NUM: return truthy_num(v.num);
    case V_BOOL: return v.b;
    case V_COL: return true; /* a colour's text is never empty */
    default: return false;
    }
}

static double to_num(Val v) {
    double d = NAN;
    switch (v.k) {
    case V_NUM: return v.num;
    case V_BOOL: return v.b;
    case V_STR: return mc_js_number(sv(v), &d) ? d : NAN;
    default: return NAN; /* nor is a colour's text ever a numeral */
    }
}

/* String(v), a number's in the arena */
static Str js_str(Arena a[static 1], Val v) {
    char b[32];
    switch (v.k) {
    case V_STR: return sv(v);
    case V_BOOL: return v.b ? S("true") : S("false");
    case V_NUM: return arena_strdup(a, (Str){.p = b, .n = fmt_js(v.num, b)});
    case V_COL: return mc_str(v.c);
    default: return S("undefined");
    }
}

static bool js_seq(Th t[static 1], Val a, Val b) {
    if (is_str(a) && is_str(b)) return str_eq(js_str(t->a, a), js_str(t->a, b));
    if (a.k != b.k) return false;
    switch (a.k) {
    case V_NUM: return a.num == b.num;
    case V_BOOL: return a.b == b.b;
    default: return true;
    }
}

/* ---- khroma ---- */

/* the colour khroma reads from v, nullptr where it throws: a string it cannot parse, or no string */
static MColor *parsed(Th t[static 1], Val v) {
    if (v.k == V_COL) return v.c->bad ? nullptr : v.c;
    MColor *c = v.k == V_STR ? ANEW(t->a, MColor, 1) : nullptr;
    return c && mc_parse(sv(v), c) ? c : nullptr;
}

static const MColor *colv(Th t[static 1], Val v) {
    const MColor *c = parsed(t, v);
    if (!c) t->ok = false;
    return c ? c : &t->dummy;
}

/* this[s] as khroma reads it; a string is parsed once, the slot keeping the colour */
[[gnu::noinline]] static const MColor *col(Th t[static 1], unsigned s) {
    MColor *c = parsed(t, t->v[s]);
    if (!c) return t->ok = false, &t->dummy;
    return t->v[s] = (Val){.k = V_COL, .c = c}, c;
}

/* adjust(c, {h, s, l, r, g, b}), mask saying which it names; flat, so the amounts go in registers */
[[gnu::noinline]] static Val cadjust(Th t[static 1], const MColor c[static 1], unsigned mask,
                                     double h, double s, double l, double r, double g, double b) {
    MColor *o = ANEW(t->a, MColor, 1);
    if (!o) return VUNDEF;
    mc_adjust(c, (MCAmounts){.mask = (uint8_t)mask, .h = h, .s = s, .l = l, .r = r, .g = g, .b = b}, o);
    return (Val){.k = V_COL, .c = o};
}

[[gnu::noinline]] static Val clighten(Th t[static 1], const MColor c[static 1], double amt) {
    MColor *o = ANEW(t->a, MColor, 1);
    if (!o) return VUNDEF;
    mc_lighten(c, amt, o);
    return (Val){.k = V_COL, .c = o};
}

[[gnu::noinline]] static Val cinvert(Th t[static 1], const MColor c[static 1]) {
    MColor *o = ANEW(t->a, MColor, 1);
    if (!o) return VUNDEF;
    mc_invert(c, 100, o);
    return (Val){.k = V_COL, .c = o};
}

/* theme-helpers.js (gen_mtheme.mjs checks it has not changed) */
static Val mkborder(Th t[static 1], const MColor c[static 1], bool dark) {
    return cadjust(t, c, 6, 0, -40, dark ? 10 : -10, 0, 0, 0);
}

/* ---- the themes ---- */

static Val getk(Th t[static 1], Key k);
static void putk(Th t[static 1], Key k, Val v);
static Key fam(Th t[static 1], unsigned f, double n);
#include "mtheme_gen.h"

static uint32_t hash(Str key) { /* FNV-1a; tools/gen_mtheme.mjs lays HT out with it */
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < key.n; i++) h = (h ^ (unsigned char)key.p[i]) * 16777619u;
    return h;
}

static unsigned slot_of(Str name) {
    constexpr size_t mask = countof(HT) - 1;
    for (size_t i = hash(name) & mask; HT[i]; i = (i + 1) & mask)
        if (str_eq(NAMES[HT[i] - 1], name)) return HT[i] - 1u;
    return NONE;
}

static Key key_of(Str name) { return (Key){.slot = slot_of(name), .name = name}; }

/* this[prefix + n] */
static Key fam(Th t[static 1], unsigned f, double n) {
    const Fam *m = &FAMS[f];
    size_t i = n >= 0 && n < m->n ? (size_t)n : 0;
    if (i == n && m->slot[i] != NONE) return (Key){.slot = m->slot[i]};
    char b[32];
    return key_of(STR_CAT(t->a, m->prefix, (Str){.p = b, .n = fmt_js(n, b)}));
}

static bool is_obj(const Th t[static 1], Key k) { return k.slot != NONE ? t->obj[k.slot] : xobj_of(t, k.name) < t->xobj.n; }

/* delete this[name] and, where it held an object, the object's fields ("name.*") */
static void drop(Th t[static 1], unsigned s) {
    t->v[s] = VUNDEF;
    if (!t->obj[s]) return;
    t->obj[s] = false;
    for (size_t i = 0; i < countof(OBJS); i++)
        if (OBJS[i].root == s)
            for (unsigned f = OBJS[i].lo; f < OBJS[i].hi; f++) t->v[f] = VUNDEF;
    xdrop_fields(t, NAMES[s]);
}

static void dropk(Th t[static 1], Key k) {
    if (k.slot != NONE) return drop(t, k.slot);
    Extra *e = xfind(t, k.name);
    if (e) *e = t->x.v[--t->x.n];
    size_t r = xobj_of(t, k.name);
    if (r == t->xobj.n) return;
    t->xobj.v[r] = t->xobj.v[--t->xobj.n];
    xdrop_fields(t, k.name);
}

static Val getk(Th t[static 1], Key k) {
    if (k.slot != NONE) return t->v[k.slot];
    const Extra *e = xfind(t, k.name);
    return e ? e->v : VUNDEF;
}

/* MAX_VARS variables defined already; the slots are counted only once the extras could get there */
static bool full(const Th t[static 1]) {
    if (t->x.n < MAX_VARS - NSLOT) return false;
    size_t n = t->x.n;
    for (size_t s = 0; s < NSLOT; s++) n += t->v[s].k != V_UNDEF;
    return n >= MAX_VARS;
}

/* this[key] = v: a plain value replaces whatever object key held; undefined is stored as absence;
 * a field ("key.field") makes "key" an object */
static void putk(Th t[static 1], Key k, Val v) {
    if (is_obj(t, k)) dropk(t, k);
    else if (k.slot != NONE) {
        if (t->v[k.slot].k != V_UNDEF) return (void)(t->v[k.slot] = v); /* a plain value */
    } else {
        Extra *e = xfind(t, k.name);
        if (e && v.k != V_UNDEF) return (void)(e->v = v);
        if (e) return (void)(*e = t->x.v[--t->x.n]);
    }
    if (v.k == V_UNDEF) return;
    Str name = k.slot != NONE ? NAMES[k.slot] : k.name;
    const char *dot = memchr(name.p, '.', name.n);
    if (dot) {
        Key top = key_of((Str){.p = name.p, .n = (size_t)(dot - name.p)});
        if (top.slot != NONE) t->obj[top.slot] = true;
        else if (xobj_of(t, top.name) == t->xobj.n && !VEC_PUSH(t->a, &t->xobj, top.name)) t->ok = false;
    }
    if (k.slot != NONE) t->v[k.slot] = v;
    else if (full(t) || !VEC_PUSH(t->a, &t->x, ((Extra){.key = k.name, .v = v}))) t->ok = false;
}

/* YAML's reading of a scalar: true/false, a number, else the string */
static Val scalar(Str s) {
    double d;
    if (str_eq(s, S("true")) || str_eq(s, S("false"))) return vbool(s.p[0] == 't');
    return str_trim(s).n && mc_js_number(s, &d) ? vnum(d) : vstr(s);
}

/* an override: this[k] = v; for a field, top is its object */
typedef struct Ov { Key k, top; bool field; Val v; } Ov;

/* the overrides calculate()'s keys.forEach((k) => { this[k] = overrides[k]; }) assigns, the last
 * value for each key winning: ov[*first..n), or n+1 past MAX_VARS unknown names */
static size_t overrides(size_t n, const Str keys[n], const Str vals[n], Ov ov[n]) {
    bool seen[NSLOT] = {};
    size_t w = n, nx = 0;
    for (size_t i = n; i-- > 0;) {
        Key k = key_of(keys[i]);
        bool later = k.slot != NONE && seen[k.slot];
        for (size_t m = w; m < n && !later && k.slot == NONE; m++) later = ov[m].k.slot == NONE && str_eq(ov[m].k.name, k.name);
        if (later) continue;
        if (k.slot != NONE) seen[k.slot] = true;
        else if (++nx > MAX_VARS) return n + 1; /* full() cannot hold more extras */
        const char *dot = memchr(keys[i].p, '.', keys[i].n);
        ov[--w] = (Ov){.k = k, .v = scalar(vals[i]), .field = dot,
                       .top = dot ? key_of((Str){.p = keys[i].p, .n = (size_t)(dot - keys[i].p)}) : (Key){}};
    }
    return w;
}

/* with one deliberate difference from mermaid, which assigns each top-level key, so a nested
 * override (xyChart.titleColor) replaces the theme's whole object and a dark xychart loses every
 * other dark colour; here it sets that field, and the object keeps the rest */
static void copy_overrides(Th t[static 1], size_t n, const Ov ov[n]) {
    for (size_t i = 0; i < n; i++) {
        if (ov[i].field && !is_obj(t, ov[i].top)) dropk(t, ov[i].top); /* a field turns a plain value into an object */
        putk(t, ov[i].k, ov[i].v);
    }
}

/* String() of the numbers and booleans; strings and colours stay as they are */
static void finish_val(Arena a[static 1], Val v[static 1]) {
    if (v->k == V_NUM || v->k == V_BOOL) *v = vstr(js_str(a, *v));
}

static MTheme *finish(Th t[static 1]) {
    MTheme *m = ANEW(t->a, MTheme, 1);
    if (!m) return nullptr;
    for (size_t s = 0; s < NSLOT; s++) finish_val(t->a, &t->v[s]);
    for (size_t i = 0; i < t->x.n; i++) finish_val(t->a, &t->x.v[i].v);
    *m = (MTheme){.v = t->v, .nx = t->x.n, .x = t->x.v};
    return t->a->oom ? nullptr : m;
}

MTheme *mtheme_new(Arena a[static 1], MThemeId id, size_t n, const Str *keys, const Str *vals) {
    if (id >= countof(THEMES)) return nullptr;
    for (size_t i = 0; i < n; i++)
        if (vals[i].n > UINT32_MAX) return nullptr; /* past what a Val holds */
    const ThemeDef *d = &THEMES[id];
    Val *v = ANEW(a, Val, NSLOT);
    bool obj[NSLOT] = {};
    if (!v) return nullptr;
    memcpy(v, d->init, NSLOT * sizeof *v);
    for (size_t i = 0; i < d->nroots; i++) obj[d->roots[i]] = true;
    Th t = {.a = a, .v = v, .obj = obj, .ok = true};
    Ov *ov = ANEW(a, Ov, n);
    if (!ov) return nullptr;
    size_t first = overrides(n, keys, vals, ov);
    if (first > n) return nullptr;
    size_t m = n - first;
    copy_overrides(&t, m, ov + first);
    d->update(&t);
    copy_overrides(&t, m, ov + first);
    if (d->calc == CALC_COPY_GRADIENT) {
        bool border = false, gradient = false;
        for (size_t i = first; i < n; i++) {
            unsigned s = ov[i].field ? ov[i].top.slot : ov[i].k.slot;
            border |= s == X_nodeBorder, gradient |= s == X_useGradient;
        }
        if (border && !gradient) put(&t, X_useGradient, vbool(false));
    }
    return t.ok && !a->oom ? finish(&t) : nullptr;
}

static Str text(Val v) { return v.k == V_STR ? sv(v) : v.k == V_COL ? mc_str(v.c) : (Str){}; }

Str mtheme_get(const MTheme *t, Str key) {
    unsigned s = slot_of(key);
    if (s != NONE) return text(t->v[s]);
    for (size_t i = 0; i < t->nx; i++)
        if (str_eq(t->x[i].key, key)) return text(t->x[i].v);
    return (Str){};
}

const MTheme *mtheme_preset(MThemeId id) { return id < countof(THEMES) ? &THEMES[id].preset : nullptr; }
