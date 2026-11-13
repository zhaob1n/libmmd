/* usecase-beta (mermaid 12): actors, use cases (ellipse, [rect], business), system boundaries
 * (rect or package), notes, JSON tables; associations with their markers, include / extend,
 * generalization. mermaid lays it out with dagre, so the layered layout places it; it is drawn in
 * mermaid's default neo look on redux-color: one colour per role (or per element with
 * `colorScheme: rotate`), a palette slot per boundary, orthogonal rounded edges. `look: classic`
 * with a classic theme draws the theme's colours and dagre's curves instead. */
#include <math.h>
#include <string.h>

#include "dcfg.h"
#include "dgraph.h"
#include "diagrams.h"
#include "rich.h"
#include "text.h"

typedef enum : uint8_t { K_ACTOR, K_ELLIPSE, K_RECT, K_NOTE, K_JSON } Kind;
typedef enum : uint8_t { A_NORMAL, A_HOLLOW, A_AWESOME, A_ICON } ActorType;
typedef enum : uint8_t { R_ASSOC, R_INCLUDE, R_EXTEND, R_GEN, R_NOTE } RelType;
typedef struct Row { Str key, val; } Row;
typedef struct Ent {
    Str id, label, stereo, cls, style;
    Kind kind;
    ActorType at;
    bool business;
    bool labelled; /* a declaration gave the label */
    int32_t parent; /* boundary, or NONE */
    VEC(Row) rows;  /* json */
} Ent;
typedef struct Bnd { Str id, title, cls, style; bool package; } Bnd;
typedef struct Rel { int32_t a, b, len; Str label, id, cls, style; RelType type; Arrow s, e; } Rel;

static const char OOM[] = "out of memory";

typedef struct U {
    Arena *a;
    Graph ids; /* id -> index into ents (the interning table only) */
    VEC(Ent) ents;
    VEC(Bnd) bnds;
    VEC(Rel) rels;
    Graph *g;  /* collects classDefs */
    int32_t bnd; /* open systemBoundary, or NONE */
    size_t notes;
} U;

/* a node reference / declaration: Id or "Quoted", then optional (label) / [label], @{ meta },
 * <<stereotype>>, :::classes */
typedef struct Decl { Str id, label, meta, stereo, cls; Kind shape; bool has_label; } Decl;

static bool id_ch(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || (unsigned char)c >= 0x80;
}

static void skip(Str l[static 1]) { while (l->n && is_sp(l->p[0])) l->p++, l->n--; }
static void adv(Str l[static 1], size_t k) { l->p += k, l->n -= k; }

/* `"…"` at l: the content, l past the closing quote; false if unterminated */
static bool quoted(Str l[static 1], Str out[static 1]) {
    const char *q = memchr(l->p + 1, '"', l->n - 1);
    if (!q) return false;
    *out = (Str){.p = l->p + 1, .n = (size_t)(q - l->p - 1)};
    adv(l, (size_t)(q - l->p) + 1);
    return true;
}

/* mermaid: a quoted name's id is the text with each non-word character as '_' */
static Str word_id(Arena a[static 1], Str s) {
    char *o = ANEW(a, char, s.n + 1);
    if (!o) return (Str){};
    for (size_t i = 0; i < s.n; i++) o[i] = (unsigned char)s.p[i] < 0x80 && id_ch(s.p[i]) ? s.p[i] : '_';
    return (Str){.p = o, .n = s.n};
}

/* mermaid draws a <br> (any spelling) in a usecase label as the text "<br>"; markdown strings are rich */
static Str label_of(Arena a[static 1], Str raw, bool ok[static 1]) {
    Str s = str_trim(raw);
    if (s.n >= 4 && s.p[0] == '"' && s.p[1] == '`') return rich_normalize(a, s, ok);
    *ok = true;
    if (s.n >= 2 && s.p[0] == '"' && s.p[s.n - 1] == '"') s.p++, s.n -= 2;
    if (!memchr(s.p, '<', s.n)) return s;
    char *o = arena_alloc(a, 2 * s.n + 1, 1); /* "<br/>" (5) -> "#lt;br>" (7) */
    if (!o) { *ok = false; return (Str){}; }
    size_t k = 0;
    for (size_t i = 0; i < s.n; i++) {
        size_t bl = s.p[i] == '<' ? label_br_len(s, i) : 0;
        if (bl) { memcpy(o + k, "#lt;br>", 7); k += 7; i += bl - 1; } /* kept literal by dg_wrap */
        else o[k++] = s.p[i];
    }
    return (Str){.p = o, .n = k};
}

/* parse a Decl at l (l advanced); nullptr when fine, else the error */
static const char *decl(U u[static 1], Str l[static 1], Decl d[static 1]) {
    *d = (Decl){.shape = K_ELLIPSE};
    skip(l);
    if (l->n && l->p[0] == '"') {
        Str q;
        if (!quoted(l, &q)) return "unterminated string";
        if (!q.n) return "empty name";
        d->id = word_id(u->a, q);
        bool ok;
        d->label = label_of(u->a, (Str){.p = q.p - 1, .n = q.n + 2}, &ok);
        if (!ok || !d->id.n) return OOM;
        d->has_label = true;
    } else {
        size_t k = 0;
        while (k < l->n && id_ch(l->p[k])) k++;
        if (!k) return "expected an identifier";
        d->id = (Str){.p = l->p, .n = k};
        adv(l, k);
    }
    if (l->n && (l->p[0] == '(' || l->p[0] == '[')) {
        char close = l->p[0] == '(' ? ')' : ']';
        d->shape = close == ')' ? K_ELLIPSE : K_RECT;
        adv(l, 1);
        const char *st = l->p;
        if (l->n && l->p[0] == '"') {
            Str q;
            if (!quoted(l, &q)) return "unterminated string";
        }
        const char *c = memchr(l->p, close, l->n);
        if (!c) return close == ')' ? "expected ')'" : "expected ']'";
        bool ok;
        d->label = label_of(u->a, (Str){.p = st, .n = (size_t)(c - st)}, &ok);
        if (!ok) return OOM;
        d->has_label = true;
        adv(l, (size_t)(c - l->p) + 1);
    }
    for (;;) {
        Str save = *l;
        skip(l);
        if (l->n >= 2 && l->p[0] == '@' && l->p[1] == '{') {
            int depth = 0;
            size_t k = 0;
            bool inq = false;
            for (; k < l->n; k++) {
                char c = l->p[k];
                if (c == '"') inq = !inq;
                else if (!inq && c == '{') depth++;
                else if (!inq && c == '}' && --depth == 0) break;
            }
            if (k >= l->n) return "unterminated @{ }";
            d->meta = (Str){.p = l->p + 2, .n = k - 2};
            adv(l, k + 1);
        } else if (l->n >= 2 && l->p[0] == '<' && l->p[1] == '<') {
            const char *e = nullptr;
            for (size_t k = 2; k + 1 < l->n; k++) if (l->p[k] == '>' && l->p[k + 1] == '>') { e = l->p + k; break; }
            if (!e) return "expected '>>'";
            d->stereo = str_trim((Str){.p = l->p + 2, .n = (size_t)(e - l->p - 2)});
            adv(l, (size_t)(e - l->p) + 2);
        } else if (l->n >= 3 && !memcmp(l->p, ":::", 3)) {
            adv(l, 3);
            size_t k = 0;
            while (k < l->n && (id_ch(l->p[k]) || l->p[k] == '-' || l->p[k] == ',')) k++;
            for (size_t i = 0, s = 0; i <= k; i++) { /* a,b → "a b" */
                if (i < k && l->p[i] != ',') continue;
                if (i > s && !graph_add_class(u->a, &d->cls, (Str){.p = l->p + s, .n = i - s})) return OOM;
                s = i + 1;
            }
            adv(l, k);
        } else { *l = save; return nullptr; }
    }
}

[[nodiscard]] static int32_t ent(U u[static 1], Str id) {
    bool fresh;
    int32_t v = graph_intern(&u->ids, id, &fresh);
    if (v == NONE) return NONE;
    if (fresh && !VEC_PUSH(u->a, &u->ents, ((Ent){.id = id, .label = id, .kind = K_ELLIPSE, .parent = NONE}))) return NONE;
    return v;
}

static int32_t find_bnd(const U u[static 1], Str id) {
    for (size_t i = 0; i < u->bnds.n; i++) if (str_eq(u->bnds.v[i].id, id)) return (int32_t)i;
    return NONE;
}
static int32_t find_rel(const U u[static 1], Str id) {
    for (size_t i = 0; i < u->rels.n; i++) if (u->rels.v[i].id.n && str_eq(u->rels.v[i].id, id)) return (int32_t)i;
    return NONE;
}

static bool truthy(Str v) { return str_eq(v, S("true")) || str_eq(v, S("\"true\"")); }
static Str unq(Str v) { return v.n >= 2 && v.p[0] == '"' && v.p[v.n - 1] == '"' ? (Str){.p = v.p + 1, .n = v.n - 2} : v; }

static void ent_meta(Ent e[static 1], Str meta) {
    Str k, v;
    while (graph_kv_next(&meta, &k, &v)) {
        v = unq(v);
        if (str_eq(k, S("business"))) e->business = truthy(v);
        else if (str_eq(k, S("type")) && e->kind == K_ACTOR)
            e->at = str_eq(v, S("hollow")) ? A_HOLLOW : str_eq(v, S("awesome")) ? A_AWESOME : e->at;
        else if (str_eq(k, S("icon")) && e->kind == K_ACTOR && v.n) e->at = A_ICON;
    }
}

/* apply a declaration (actor or use case) */
static const char *declare(U u[static 1], const Decl d[static 1], bool actor) {
    int32_t v = ent(u, d->id);
    if (v == NONE) return OOM;
    Ent *e = &u->ents.v[v];
    if (actor) e->kind = K_ACTOR;
    else if (d->has_label && e->kind != K_ACTOR) e->kind = d->shape;
    if (d->has_label) {
        if (e->labelled && !str_eq(e->label, d->label)) return "ID declared again with a different label";
        e->label = d->label, e->labelled = true;
    }
    if (d->stereo.n) e->stereo = d->stereo;
    if (d->cls.n && !graph_add_class(u->a, &e->cls, d->cls)) return OOM;
    if (u->bnd != NONE) e->parent = u->bnd;
    ent_meta(e, d->meta);
    return nullptr;
}

/* ---- JSON (json Id@{ … }): rows of key path / display value, in source order ---- */
typedef struct J { Str s; size_t i; U *u; Ent *e; unsigned depth; } J;

static void jsp(J j[static 1]) { while (j->i < j->s.n && (is_sp(j->s.p[j->i]) || j->s.p[j->i] == '\n')) j->i++; }

static bool jrow(J j[static 1], Str key, Str val) { return VEC_PUSH(j->u->a, &j->e->rows, ((Row){key, val})); }

static Str jcat(Arena a[static 1], Str p, const char *sep, Str k, const char *tail) {
    size_t sl = p.n ? strlen(sep) : 0, tl = strlen(tail);
    char *o = ANEW(a, char, p.n + sl + k.n + tl + 1);
    if (!o) return (Str){};
    if (p.n) memcpy(o, p.p, p.n);
    memcpy(o + p.n, sep, sl);
    if (k.n) memcpy(o + p.n + sl, k.p, k.n); /* an empty Str may have no pointer */
    memcpy(o + p.n + sl + k.n, tail, tl);
    return (Str){.p = o, .n = p.n + sl + k.n + tl};
}

/* a scalar at j->i: its display text (strings unescaped for the common escapes) */
static bool jscalar(J j[static 1], Str out[static 1]) {
    Str s = j->s;
    size_t i = j->i;
    if (i < s.n && s.p[i] == '"') {
        size_t st = ++i;
        bool esc = false;
        while (i < s.n && s.p[i] != '"') { if (s.p[i] == '\\') esc = true, i++; i++; }
        if (i >= s.n) return false;
        Str raw = {.p = s.p + st, .n = i - st};
        j->i = i + 1;
        if (!esc) { *out = raw; return true; }
        char *o = ANEW(j->u->a, char, raw.n + 1);
        if (!o) return false;
        size_t k = 0;
        for (size_t q = 0; q < raw.n; q++) {
            char c = raw.p[q];
            if (c == '\\' && q + 1 < raw.n) {
                c = raw.p[++q];
                c = c == 'n' ? '\n' : c == 't' ? ' ' : c;
            }
            o[k++] = c;
        }
        *out = (Str){.p = o, .n = k};
        return true;
    }
    size_t st = i;
    while (i < s.n && !strchr(",]} \t\r\n", s.p[i])) i++;
    if (i == st) return false;
    *out = (Str){.p = s.p + st, .n = i - st};
    j->i = i;
    return true;
}

static bool jvalue(J j[static 1], Str path);

static bool jarray(J j[static 1], Str path) {
    j->i++; /* '[' */
    size_t idx = 0; /* scalars: the first row carries the key; objects/arrays: path[i] */
    for (;;) {
        jsp(j);
        if (j->i >= j->s.n) return false;
        if (j->s.p[j->i] == ']') { j->i++; break; }
        if (idx) {
            if (j->s.p[j->i] != ',') return false;
            j->i++, jsp(j);
        }
        char c = j->i < j->s.n ? j->s.p[j->i] : 0;
        if (c == '{' || c == '[') {
            char num[24];
            int nl = snprintf(num, sizeof num, "%zu", idx);
            Str sub = jcat(j->u->a, path, "", (Str){.p = "[", .n = 1}, "");
            sub = jcat(j->u->a, sub, "", (Str){.p = num, .n = (size_t)nl}, "]");
            if (!sub.n || !jvalue(j, sub)) return false;
        } else {
            Str v;
            if (!jscalar(j, &v) || !jrow(j, idx ? (Str){} : path, v)) return false;
        }
        idx++;
    }
    return idx || jrow(j, path, S("[]"));
}

static bool jobject(J j[static 1], Str path) {
    j->i++; /* '{' */
    size_t n = 0;
    for (;;) {
        jsp(j);
        if (j->i >= j->s.n) return false;
        if (j->s.p[j->i] == '}') { j->i++; break; }
        if (n) {
            if (j->s.p[j->i] != ',') return false;
            j->i++, jsp(j);
        }
        Str key;
        if (j->i >= j->s.n || j->s.p[j->i] != '"' || !jscalar(j, &key)) return false;
        jsp(j);
        if (j->i >= j->s.n || j->s.p[j->i] != ':') return false;
        j->i++, jsp(j);
        Str sub = path.n ? jcat(j->u->a, path, ".", key, "") : key;
        if ((path.n && !sub.n) || !jvalue(j, sub)) return false;
        n++;
    }
    return n || jrow(j, path, S("{}"));
}

static bool jvalue(J j[static 1], Str path) {
    jsp(j);
    if (j->i >= j->s.n || ++j->depth > 64) return false;
    bool ok;
    char c = j->s.p[j->i];
    if (c == '{') ok = jobject(j, path);
    else if (c == '[') ok = jarray(j, path);
    else {
        Str v;
        ok = jscalar(j, &v) && jrow(j, path, v);
    }
    j->depth--;
    return ok;
}

/* ---- relationships ---- */
typedef struct Arw { size_t len; int dashes; Arrow s, e; RelType type; bool plain; } Arw;

static bool arrow_at(Str l, Arw o[static 1]) {
    *o = (Arw){.type = R_ASSOC};
    size_t n = l.n, i = 0;
    const char *p = l.p;
    if (n >= 4 && !memcmp(p, "--|>", 4)) { o->len = 4, o->dashes = 2, o->type = R_GEN; return true; }
    if (n >= 3 && !memcmp(p, "..>", 3)) { o->len = 3, o->dashes = 2, o->type = R_INCLUDE; return true; }
    if (n >= 3 && p[0] == '<' && p[1] == '-' && p[2] == '-') {
        for (i = 1; i < n && p[i] == '-'; i++) {}
        o->len = i, o->dashes = (int)i - 1, o->s = AH_ARROW;
        return true;
    }
    if (n >= 3 && (p[0] == 'o' || p[0] == 'x') && p[1] == '-' && p[2] == '-') {
        for (i = 1; i < n && p[i] == '-'; i++) {}
        o->len = i, o->dashes = (int)i - 1, o->s = p[0] == 'o' ? AH_CIRCLE : AH_CROSS;
        return true;
    }
    for (i = 0; i < n && p[i] == '-'; i++) {}
    if (i < 2) return false;
    o->dashes = (int)i;
    if (i < n && p[i] == '>') o->e = AH_ARROW, o->len = i + 1;
    else if (i < n && (p[i] == 'o' || p[i] == 'x') && (i + 1 == n || !id_ch(p[i + 1]))) o->e = p[i] == 'o' ? AH_CIRCLE : AH_CROSS, o->len = i + 1;
    else o->len = i, o->plain = true;
    return true;
}

static const char *relation(U u[static 1], const Decl from[static 1], Str l[static 1]) {
    Rel r = {.len = 1};
    skip(l);
    size_t k = 0; /* edge id: `name@` right before the arrow */
    while (k < l->n && id_ch(l->p[k])) k++;
    if (k && k < l->n && l->p[k] == '@' && k + 1 < l->n && l->p[k + 1] != '{') r.id = (Str){.p = l->p, .n = k}, adv(l, k + 1);
    Arw a;
    if (!arrow_at(*l, &a)) return "expected a relationship arrow";
    adv(l, a.len);
    if (a.plain) { /* `A -- label --> B`: a second arrow after the label */
        bool inq = l->n && l->p[0] == '"';
        for (size_t i = 1; i + 1 < l->n; i++) {
            if (l->p[i] == '"') inq = !inq; /* an arrow inside a quoted label is text */
            if (inq || l->p[i] != '-' || l->p[i + 1] != '-' || !is_sp(l->p[i - 1])) continue;
            Arw b;
            if (!arrow_at((Str){.p = l->p + i, .n = l->n - i}, &b) || b.type != R_ASSOC) continue;
            bool ok;
            r.label = label_of(u->a, (Str){.p = l->p, .n = i}, &ok);
            if (!ok) return OOM;
            adv(l, i + b.len);
            a = b;
            break;
        }
    }
    r.type = a.type, r.s = a.s, r.e = a.e;
    r.len = a.dashes > 2 ? a.dashes - 1 : 1;
    if (a.type == R_GEN) r.e = AH_NONE;
    if (a.type == R_INCLUDE) {
        skip(l);
        if (!l->n || l->p[0] != ':') return "expected ': include' or ': extend'";
        adv(l, 1);
        Str w = dg_word(l);
        if (str_eq(w, S("extend"))) r.type = R_EXTEND;
        else if (!str_eq(w, S("include"))) return "expected 'include' or 'extend'";
        r.label = r.type == R_EXTEND ? S("«extend»") : S("«include»");
        r.e = AH_ARROW;
    }
    Decl to;
    const char *err = decl(u, l, &to);
    if (err) return err;
    skip(l);
    if (l->n) return "unexpected text after relationship";
    if (find_bnd(u, from->id) != NONE || find_bnd(u, to.id) != NONE) return "a relationship cannot end at a systemBoundary";
    if ((err = declare(u, from, false)) || (err = declare(u, &to, false))) return err;
    r.a = graph_find(&u->ids, from->id), r.b = graph_find(&u->ids, to.id);
    return VEC_PUSH(u->a, &u->rels, r) ? nullptr : OOM;
}

/* class/style targets: a boundary, an element or a relationship id */
static Str *target(U u[static 1], Str id, bool style) {
    int32_t i = find_bnd(u, id);
    if (i != NONE) return style ? &u->bnds.v[i].style : &u->bnds.v[i].cls;
    if ((i = graph_find(&u->ids, id)) != NONE) return style ? &u->ents.v[i].style : &u->ents.v[i].cls;
    if ((i = find_rel(u, id)) != NONE) return style ? &u->rels.v[i].style : &u->rels.v[i].cls;
    return nullptr;
}

/* one statement (possibly spanning lines: open quotes or braces) */
static const char *stmt(U u[static 1], Str l) {
    Str rest = l, w = dg_word(&rest);
    if (str_eq(l, S("end"))) {
        if (u->bnd == NONE) return "'end' without systemBoundary";
        u->bnd = NONE;
        return nullptr;
    }
    if (str_eq(w, S("direction"))) {
        Dir d;
        if (!graph_dir(dg_word(&rest), &d)) return "invalid direction";
        u->g->dir = d;
        return nullptr;
    }
    if (str_eq(w, S("classDef"))) {
        Str names = dg_word(&rest), item;
        for (size_t i = 0, s = 0; i <= names.n; i++) {
            if (i < names.n && names.p[i] != ',') continue;
            item = (Str){.p = names.p + s, .n = i - s};
            s = i + 1;
            if (!item.n) continue;
            ClassDef cd = {.name = item};
            if (!graph_add_css(u->a, &cd.css, rest) || !VEC_PUSH(u->a, &u->g->classdefs, cd)) return OOM;
        }
        return nullptr;
    }
    if (str_eq(w, S("class")) || str_eq(w, S("style"))) {
        bool style = str_eq(w, S("style"));
        Str ids = dg_word(&rest), cls = style ? rest : str_trim(rest);
        for (size_t i = 0, s = 0; i <= ids.n; i++) {
            if (i < ids.n && ids.p[i] != ',') continue;
            Str *t = target(u, (Str){.p = ids.p + s, .n = i - s}, style);
            s = i + 1;
            if (!t) continue;
            if (style) { if (!graph_add_css(u->a, t, cls)) return OOM; continue; }
            for (size_t a = 0, b = 0; a <= cls.n; a++) {
                if (a < cls.n && cls.p[a] != ',') continue;
                if (a > b && !graph_add_class(u->a, t, str_trim((Str){.p = cls.p + b, .n = a - b}))) return OOM;
                b = a + 1;
            }
        }
        return nullptr;
    }
    if (str_eq(w, S("systemBoundary"))) {
        if (u->bnd != NONE) return "systemBoundary cannot nest";
        Decl d;
        const char *err = decl(u, &rest, &d);
        if (err) return err;
        Bnd b = {.id = d.id, .title = d.has_label ? d.label : d.id, .cls = d.cls};
        Str k, v, m = d.meta;
        while (graph_kv_next(&m, &k, &v)) if (str_eq(k, S("type"))) b.package = str_eq(unq(v), S("package"));
        if (!VEC_PUSH(u->a, &u->bnds, b)) return OOM;
        u->bnd = (int32_t)u->bnds.n - 1;
        skip(&rest);
        return rest.n ? "unexpected text after systemBoundary" : nullptr;
    }
    if (str_eq(w, S("actor"))) {
        Decl d;
        const char *err = decl(u, &rest, &d);
        if (err) return err;
        skip(&rest);
        if (rest.n) return "unexpected text after actor";
        return declare(u, &d, true);
    }
    if (str_eq(w, S("note")) && str_starts(rest, S("for "))) {
        adv(&rest, 4);
        Decl d;
        const char *err = decl(u, &rest, &d);
        if (err) return err;
        skip(&rest);
        if (!rest.n) return "expected note text";
        char nm[32];
        int nl = snprintf(nm, sizeof nm, "\x1fnote%zu", u->notes++);
        Str nid = {.p = arena_alloc(u->a, (size_t)nl, 1), .n = (size_t)nl};
        if (!nid.p) return OOM;
        memcpy((char *)nid.p, nm, (size_t)nl);
        int32_t t = ent(u, d.id), v = t == NONE ? NONE : ent(u, nid);
        if (v == NONE) return OOM;
        bool ok;
        Ent *e = &u->ents.v[v];
        e->kind = K_NOTE;
        e->label = label_of(u->a, rest, &ok);
        if (!ok) return OOM;
        Rel r = {.a = v, .b = t, .len = 1, .type = R_NOTE};
        return VEC_PUSH(u->a, &u->rels, r) ? nullptr : OOM;
    }
    if (str_eq(w, S("json"))) {
        Decl d;
        const char *err = decl(u, &rest, &d);
        if (err) return err;
        if (!d.meta.p) return "expected json Id@{ … }";
        int32_t v = ent(u, d.id);
        if (v == NONE) return OOM;
        Ent *e = &u->ents.v[v];
        e->kind = K_JSON, e->rows.n = 0;
        if (d.cls.n && !graph_add_class(u->a, &e->cls, d.cls)) return OOM;
        J j = {.s = {.p = d.meta.p - 1, .n = d.meta.n + 2}, .u = u, .e = e};
        if (!jvalue(&j, (Str){})) return "invalid JSON";
        jsp(&j);
        return j.i == j.s.n ? nullptr : "invalid JSON";
    }
    Decl d;
    const char *err = decl(u, &l, &d);
    if (err) return err;
    skip(&l);
    if (l.n) return relation(u, &d, &l);
    if (!d.has_label && d.meta.p) { /* Id@{ … } on a boundary or relationship */
        int32_t b = find_bnd(u, d.id);
        if (b != NONE) {
            Str k, v, m = d.meta;
            while (graph_kv_next(&m, &k, &v)) if (str_eq(k, S("type"))) u->bnds.v[b].package = str_eq(unq(v), S("package"));
            return nullptr;
        }
        if (find_rel(u, d.id) != NONE) return nullptr; /* animation: not drawn */
    }
    return declare(u, &d, false);
}

/* a statement continues while a quote or a brace is open */
static bool open_tail(Str s) {
    bool q = false;
    int depth = 0;
    for (size_t i = 0; i < s.n; i++) {
        if (s.p[i] == '"') q = !q;
        else if (!q && s.p[i] == '{') depth++;
        else if (!q && s.p[i] == '}') depth--;
    }
    return q || depth > 0;
}

/* ---- sizes and drawing ---- */
constexpr double WRAP = 120;
constexpr double ACT_W = 56;   /* actor glyph box, label gap, padding (mermaid usecaseActor) */
constexpr double ACT_H = 72;
constexpr double ACT_GAP = 8;
constexpr double ACT_PAD = 10;

typedef struct Fonts { double act, uc; } Fonts;

static void meas(Str s, double fs, double w[static 1], unsigned n[static 1]) {
    if (rich_needed(s)) rich_measure(s, fs, w, n);
    else label_measure(s, fs, w, n);
    if (!*n) *n = 1;
}

static Str wrap(Arena a[static 1], Str s, double fs) {
    if (s.n && s.p[0] == RICH_MD) {
        bool ok;
        Str r = rich_wrap(a, s, fs, WRAP, &ok);
        return ok ? r : (Str){};
    }
    return dg_wrap(a, s, fs, WRAP); /* empty only on OOM: s is not */
}

static Str guillemets(Arena a[static 1], Str s, bool nl) { /* «s» (then a line break before the label) */
    char *o = ANEW(a, char, s.n + 6);
    if (!o) return (Str){};
    memcpy(o, "\xc2\xab", 2), memcpy(o + 2, s.p, s.n), memcpy(o + 2 + s.n, "\xc2\xbb", 2);
    if (nl) o[s.n + 4] = '\n';
    return (Str){.p = o, .n = s.n + 4 + nl};
}

static Str concat(Arena a[static 1], Str x, Str y) {
    char *o = ANEW(a, char, x.n + y.n + 1);
    if (!o) return (Str){};
    if (x.n) memcpy(o, x.p, x.n);
    if (y.n) memcpy(o + x.n, y.p, y.n);
    return (Str){.p = o, .n = x.n + y.n};
}

#define UC_TAIL "#mermaid .bm{fill:none;}#mermaid .ic{fill:#1976d2;stroke:#0d47a1;stroke-width:1px;}#mermaid .iq{fill:#ffffff;font-size:20px;}"
static constexpr char NEO_CSS[] =
    "#mermaid{font-family:\"Recursive Variable\",arial,sans-serif;fill:#28253D;}"
    "#mermaid .gl{stroke-width:2px;}#mermaid .uc{stroke-width:2px;filter:url(#neo-shadow);}"
    "#mermaid .bd{stroke-width:1px;}#mermaid .bt{font-size:14px;fill:#000000;}"
    "#mermaid .tl,#mermaid .nl{font-size:14px;}#mermaid .el{font-size:10px;}"
    "#mermaid .nt{fill:#fff5ad;stroke:#FACC15;stroke-width:1.3px;}"
    "#mermaid .js{fill:#ffffff;stroke:#28253D;stroke-width:1px;}"
    "#mermaid .rel{fill:none;stroke:#000000;stroke-width:2px;}#mermaid .rel.d{stroke-dasharray:3;}"
    "#mermaid .rel.inc{stroke:#38BDF8;}#mermaid .rel.ext{stroke:#FB923C;}" UC_TAIL DG_NEO_CSS;
/* `look: classic` on a classic theme: the default theme's colours here, the others' (and any
 * themeVariables) from dcfg_theme_css; thin curved edges. Text: .al actor, .ul use case, .el edge,
 * .tl JSON table, .nl note, .bt boundary title. */
static constexpr char CLASSIC_CSS[] =
    "#mermaid{font-family:\"trebuchet ms\",verdana,arial,sans-serif;fill:#333;}"
    "#mermaid .gl,#mermaid .uc,#mermaid .bm{fill:#ECECFF;stroke:#9370DB;stroke-width:2px;}"
    "#mermaid .bd{fill:#ffffde;stroke:#aaaa33;stroke-width:1px;}#mermaid .bt{font-size:14px;fill:#333;}"
    "#mermaid .tl,#mermaid .nl{font-size:14px;}#mermaid .el{font-size:10px;}"
    "#mermaid .ul,#mermaid .el,#mermaid .tl{fill:#131300;}#mermaid .al,#mermaid .nl{fill:black;}"
    "#mermaid .nt{fill:#fff5ad;stroke:#aaaa33;stroke-width:1px;}"
    "#mermaid .js{fill:#ECECFF;stroke:#9370DB;stroke-width:1px;}"
    "#mermaid .rel{fill:none;stroke:#333333;stroke-width:1px;}#mermaid .rel.d{stroke-dasharray:3;}" UC_TAIL
    "#mermaid .mk{fill:#333333;stroke:#333333;stroke-width:1;}#mermaid .mkx{fill:none;stroke:#333333;stroke-width:2;}"
    "#mermaid .mke{fill:#ECECFF;stroke:#333333;stroke-width:1;}#mermaid .elbl{fill:rgba(232,232,232,0.8);}";

/* neo colours as attributes; the classic look takes them from CSS (c.fill null) */
static void color_attrs(Buf b[static 1], NeoColor c) {
    if (!c.fill) return;
    buf_lit(b, " fill=\""); buf_cstr(b, c.fill); buf_lit(b, "\" stroke=\""); buf_cstr(b, c.stroke); buf_ch(b, '"');
}

static void path_d(Buf b[static 1], const char *d, double x, double y) { /* glyph path at (x,y) */
    buf_lit(b, "<path transform=\"translate("); buf_pt(b, x, y); buf_lit(b, ")\" d=\""); buf_cstr(b, d); buf_lit(b, "\"/>");
}

/* the business slash across an actor's head (mermaid businessMarkerPathForCircle) */
static void head_slash(Buf b[static 1], double x, double y, double cy, double r) {
    double off = r * 0.6, half = r * 0.8, dx = half * 0.5, dy = -half * sqrt(3) / 2;
    double cx = x + off * sqrt(3) / 2, my = y + cy + off * 0.5;
    buf_lit(b, "<path class=\"bm\" d=\"M"); buf_pt(b, cx - dx, my - dy); buf_ch(b, 'L'); buf_pt(b, cx + dx, my + dy); buf_lit(b, "\"/>");
}

static void actor(Buf b[static 1], const Graph g[static 1], const Ent e[static 1], size_t v, NeoColor c, double fs, Str stereo) {
    static const char STICK[] = "M0-12C6.627-12 12-17.373 12-24C12-30.627 6.627-36 0-36C-6.627-36-12-30.627-12-24"
                                "C-12-17.373-6.627-12 0-12ZM0-12V8M-17-5H17M0 8L-15 28M0 8L15 28";
    static const char HOLLOW[] = "M-22-10H22V0H6L22 17L13 28L0 13L-13 28L-22 17L-6 0H-22Z";
    static const char AWESOME[] = "M0-34C7.18-34 13-28.18 13-21C13-13.82 7.18-8 0-8C-7.18-8-13-13.82-13-21C-13-28.18-7.18-34 0-34Z"
                                  "M-24 25C-24 7-14-3 0-3C14-3 24 7 24 25C24 28 21 30 18 30H-18C-21 30-24 28-24 25Z";
    double lw, sw = 0, sh = 0;
    unsigned ln, sn = 0;
    meas(g->nlabel[v], fs, &lw, &ln);
    if (stereo.n) meas(stereo, fs, &sw, &sn), sh = sn * fs * 1.5;
    double lh = ln * fs * 1.5, ch = ACT_H + ACT_GAP + lh + (stereo.n ? sh + 2 : 0);
    double x = g->nx[v], top = g->ny[v] - ch / 2, gy = top + ACT_H / 2;
    buf_lit(b, "<g class=\"gl\""); color_attrs(b, c);
    flow_style_attr(b, g, g->nclass[v], g->nstyle[v], false, true);
    buf_ch(b, '>');
    switch (e->at) {
    case A_HOLLOW:
        buf_lit(b, "<circle fill=\"none\" cx=\""); buf_num(b, x); buf_lit(b, "\" cy=\""); buf_num(b, gy - 23); buf_lit(b, "\" r=\"9\"/>");
        buf_lit(b, "<g fill=\"none\">"); path_d(b, HOLLOW, x, gy); buf_lit(b, "</g>");
        if (e->business) head_slash(b, x, gy, -23, 9);
        break;
    case A_AWESOME:
        path_d(b, AWESOME, x, gy);
        if (e->business) head_slash(b, x, gy, -21, 13);
        break;
    case A_ICON: break; /* drawn below */
    default:
        path_d(b, STICK, x, gy);
        if (e->business) head_slash(b, x, gy, -24, 12);
    }
    buf_lit(b, "</g>");
    if (e->at == A_ICON) { /* no icon packs here: mermaid's missing-icon box */
        buf_lit(b, "<rect class=\"ic\" x=\""); buf_num(b, x - 16); buf_lit(b, "\" y=\""); buf_num(b, gy - 26);
        buf_lit(b, "\" width=\"32\" height=\"32\" rx=\"3\"/>");
        dg_text(b, S("?"), 20, x, gy - 10, "middle", "iq");
    }
    double ty = top + ACT_H + ACT_GAP;
    if (stereo.n) dg_text(b, stereo, fs, x, ty + sh / 2, "middle", "al"), ty += sh + 2;
    bool wrapc = flow_text_style_open(b, g, g->nclass[v], g->nstyle[v]);
    dg_text(b, g->nlabel[v], fs, x, ty + lh / 2, "middle", "al");
    if (wrapc) buf_lit(b, "</g>");
}

DIAG_FN(usecase_render) {
    Graph g;
    dg_init(&g, a, DIR_LR, 10); /* font_size measures the 10px relationship labels */
    U u = {.a = a, .g = &g, .bnd = NONE};
    dg_init(&u.ids, a, DIR_LR, 10);
    LineIt it = {.s = src, .n = n, .i = off};
    Str line;
    size_t lo;
    if (!dg_next_line(&it, &line, &lo)) return diag_fail(err_off, msg, off, "empty usecase diagram");
    while (dg_next_line(&it, &line, &lo)) {
        while (open_tail(line) && it.i < it.n) { /* continue onto the next raw line */
            const char *nl = memchr(it.s + it.i, '\n', it.n - it.i);
            size_t e = nl ? (size_t)(nl - it.s) : it.n;
            line.n = e - (size_t)(line.p - it.s);
            it.i = nl ? e + 1 : e;
            line = str_trim(line);
        }
        const char *err = stmt(&u, line);
        if (err) {
            if (err == OOM) return MMD_ERR_NOMEM;
            return diag_fail(err_off, msg, lo, err);
        }
    }
    if (u.bnd != NONE) return diag_fail(err_off, msg, n, "unterminated systemBoundary (missing 'end')");
    g.node_sep = dcfg_num("usecase.nodeSpacing", 50);
    g.rank_sep = dcfg_num("usecase.rankSpacing", 50);
    Fonts f = {dcfg_num("usecase.actorFontSize", 14), dcfg_num("usecase.usecaseFontSize", 12)};
    bool rotate = str_eq(dcfg_get("usecase.colorScheme"), S("rotate")), neo = dcfg_neo();

    /* mermaid's node order: actors, use cases, notes, json tables, then the boundaries */
    size_t E = u.ents.n;
    int32_t *gv = ANEW(a, int32_t, E + 1), *slot = ANEW(a, int32_t, E + 1);
    if (!gv || !slot) return MMD_ERR_NOMEM;
    int32_t ci = 0;
    static const Kind ORDER[][2] = {{K_ACTOR, K_ACTOR}, {K_ELLIPSE, K_RECT}, {K_NOTE, K_NOTE}, {K_JSON, K_JSON}};
    for (size_t o = 0; o < countof(ORDER); o++)
        for (size_t i = 0; i < E; i++) {
            Ent *e = &u.ents.v[i];
            if (e->kind != ORDER[o][0] && e->kind != ORDER[o][1]) continue;
            int32_t v = dg_node(&g, e->id, nullptr);
            if (v == NONE) return MMD_ERR_NOMEM;
            gv[i] = v;
            slot[i] = o < 2 ? ci++ : NONE;
            g.nclass[v] = e->cls, g.nstyle[v] = e->style, g.nparent[v] = e->parent;
            double fs = e->kind == K_ACTOR ? f.act : e->kind == K_NOTE || e->kind == K_JSON ? 14 : f.uc, w, h = 0;
            Str lab = e->label;
            if (e->kind != K_JSON && lab.n && !(lab = wrap(a, lab, fs)).n) return MMD_ERR_NOMEM;
            if (e->stereo.n && e->kind != K_ACTOR) {
                Str st = guillemets(a, e->stereo, true);
                if (!st.n || !(lab = concat(a, st, lab)).n) return MMD_ERR_NOMEM;
            }
            g.nlabel[v] = lab;
            unsigned ln;
            meas(lab, fs, &w, &ln);
            double lh = ln * fs * 1.5;
            switch (e->kind) {
            case K_ACTOR: {
                double sw = 0, sh = 0;
                unsigned sn;
                if (e->stereo.n) meas(guillemets(a, e->stereo, false), fs, &sw, &sn), sh = sn * fs * 1.5 + 2;
                w = fmax(fmax(ACT_W, w), sw) + 2 * ACT_PAD, h = ACT_H + ACT_GAP + lh + sh + 2 * ACT_PAD;
                break;
            }
            case K_ELLIPSE: w = fmax(WRAP, w) + 40, h = lh + 40; break;
            case K_RECT: w = fmax(WRAP, w) + 32, h = lh + 24; break;
            case K_NOTE: w = fmax(WRAP, w) + 20, h = lh + 20; break;
            case K_JSON: {
                double kw = 0, vw = 0, rh = 0;
                for (size_t r = 0; r < e->rows.n; r++) {
                    double t;
                    unsigned tn;
                    meas(e->rows.v[r].key, 14, &t, &tn), kw = fmax(kw, t);
                    meas(e->rows.v[r].val, 14, &t, &tn), vw = fmax(vw, t);
                    rh += tn * 21 + 8;
                }
                w = fmax(fmax(WRAP, w) + 16, kw + vw + 32) + 2, h = lh + 8 + rh + 2;
                break;
            }
            }
            dg_size(&g, v, w, h);
        }
    for (size_t b = 0; b < u.bnds.n; b++) {
        int32_t s = dg_cluster(&g, u.bnds.v[b].id, u.bnds.v[b].title, NONE);
        if (s == NONE) return MMD_ERR_NOMEM;
        g.sclass[s] = u.bnds.v[b].cls, g.sstyle[s] = u.bnds.v[b].style;
    }
    /* relationships, then the note links (mermaid's edge order) */
    size_t R = u.rels.n;
    int32_t *rel = ANEW(a, int32_t, R + 1);
    if (!rel) return MMD_ERR_NOMEM;
    for (int pass = 0; pass < 2; pass++)
        for (size_t r = 0; r < R; r++) {
            const Rel *x = &u.rels.v[r];
            if ((x->type == R_NOTE) != (pass == 1)) continue;
            int32_t e = dg_edge(&g, gv[x->a], gv[x->b], x->label);
            if (e == NONE) return MMD_ERR_NOMEM;
            g.estart[e] = x->s, g.eend[e] = x->e, g.elen[e] = x->len, g.eid[e] = x->id;
            g.estroke[e] = x->type == R_ASSOC || x->type == R_GEN ? ST_NORMAL : ST_DOTTED;
            rel[e] = (int32_t)r;
        }
    g.clusters_last = true; /* mermaid's node order above */
    mmd_status st = flow_layout(&g);
    if (st) return st;

    BBox bb = dg_bbox(&g);
    if (title.n) bb.y0 -= 30;
    if (!buf_reserve(out, 4096 + g.nn * 800 + g.ne * 400 + g.ns * 300)) return MMD_ERR_NOMEM;
    { /* the actor / use case fonts are configurable (usecase.actorFont*, usecaseFont*) */
        Buf css = {};
        buf_cstr(&css, neo ? NEO_CSS : CLASSIC_CSS);
        if (!neo) dcfg_theme_css(&css, "usecase");
        static const char *const KEY[2][2] = {{"usecase.actorFontWeight", "usecase.actorFontFamily"},
                                              {"usecase.usecaseFontWeight", "usecase.usecaseFontFamily"}};
        for (int r = 0; r < 2; r++) {
            Str wt = dcfg_get(KEY[r][0]), fam = dcfg_get(KEY[r][1]);
            buf_cstr(&css, r ? "#mermaid .ul{font-size:" : "#mermaid .al{font-size:");
            buf_num(&css, r ? f.uc : f.act);
            buf_lit(&css, "px;");
            if (dg_css_ok(wt)) { buf_lit(&css, "font-weight:"); buf_str(&css, wt); buf_ch(&css, ';'); }
            if (dg_css_ok(fam)) { buf_lit(&css, "font-family:"); buf_str(&css, fam); buf_ch(&css, ';'); }
            buf_ch(&css, '}');
        }
        bool oom = css.oom;
        if (!oom) dg_head(out, bb, 8, "usecase", css.n, css.p);
        free(css.p);
        if (oom) return MMD_ERR_NOMEM;
    }
    dg_neo_defs(out);
    if (title.n) dg_text(out, title, 18, (bb.x0 + bb.x1) / 2, bb.y0 + 12, "middle", "bt");
    for (size_t s = 0; s < g.ns; s++) {
        if (g.sw[s] <= 0) continue;
        double x = g.sx[s], y = g.sy[s], w = g.sw[s], h = g.sh[s], tw;
        unsigned tl;
        meas(g.stitle[s], 14, &tw, &tl);
        double th = tl * 21, tab = u.bnds.v[s].package ? th + 10 : 0, tabw = fmin(w, fmax(80, tw + 20));
        NeoColor c = neo ? dg_redux(s) : (NeoColor){};
        for (int part = u.bnds.v[s].package ? 0 : 1; part < 2; part++) {
            buf_lit(out, "<rect class=\"bd\" x=\""); buf_num(out, x); buf_lit(out, "\" y=\""); buf_num(out, part ? y + tab : y);
            buf_lit(out, "\" width=\""); buf_num(out, part ? w : tabw); buf_lit(out, "\" height=\""); buf_num(out, part ? h - tab : tab);
            buf_ch(out, '"'); color_attrs(out, c);
            flow_style_attr(out, &g, g.sclass[s], g.sstyle[s], false, true);
            buf_lit(out, "/>");
        }
        if (tab) dg_text(out, g.stitle[s], 14, x + tabw / 2, y + tab / 2, "middle", "bt");
        else dg_text(out, g.stitle[s], 14, x + w / 2, y + th / 2 + 4, "middle", "bt");
    }
    const double *ports = neo ? dg_ports(&g) : nullptr;
    for (size_t e = 0; e < g.ne; e++) {
        const Rel *x = &u.rels.v[rel[e]];
        double mid[2];
        size_t mark = out->n;
        buf_lit(out, "<path class=\"rel");
        if (g.estroke[e] == ST_DOTTED) buf_lit(out, " d");
        if (x->type == R_INCLUDE) buf_lit(out, " inc");
        else if (x->type == R_EXTEND) buf_lit(out, " ext");
        buf_lit(out, "\" d=\"");
        if (!neo || !dg_ortho_d(out, &g, e, ports, mid)) {
            if (!g.eptn[e]) { out->n = mark; continue; }
            dg_edge_d(out, &g, e);
            mid[0] = g.elx[e], mid[1] = g.ely[e];
        }
        buf_ch(out, '"');
        for (int end = 0; end < 2; end++) {
            const char *m = x->type == R_GEN && end ? "neo-ext-end" : dg_neo_marker(end ? g.eend[e] : g.estart[e], end);
            if (!m) continue;
            buf_cstr(out, end ? " marker-end=\"url(#" : " marker-start=\"url(#"); buf_cstr(out, m); buf_lit(out, ")\"");
        }
        flow_style_attr(out, &g, x->cls, x->style, false, true); /* classDef default reaches relationships too */
        buf_lit(out, "/>");
        if (g.elabel[e].n && g.elw[e] > 0) {
            dg_rect(out, mid[0] - g.elw[e] / 2 - 2, mid[1] - g.elh[e] / 2, g.elw[e] + 4, g.elh[e], 0, "elbl");
            dg_text(out, g.elabel[e], 10, mid[0], mid[1], "middle", "el");
        }
    }
    for (size_t i = 0; i < E; i++) {
        const Ent *e = &u.ents.v[i];
        size_t v = (size_t)gv[i];
        double x = g.nx[v], y = g.ny[v], w = g.nw[v], h = g.nh[v];
        NeoColor c = !neo ? (NeoColor){}
                     : dg_redux(rotate && slot[i] != NONE ? (size_t)slot[i] : e->kind == K_ACTOR ? 5 : 1);
        if (e->kind == K_ACTOR) {
            actor(out, &g, e, v, c, f.act, e->stereo.n ? guillemets(a, e->stereo, false) : (Str){});
            continue;
        }
        double fs = e->kind == K_ELLIPSE || e->kind == K_RECT ? f.uc : 14;
        const char *tcls = e->kind <= K_RECT ? "ul" : "nl";
        if (e->kind == K_ELLIPSE) {
            buf_lit(out, "<ellipse class=\"uc\" cx=\""); buf_num(out, x); buf_lit(out, "\" cy=\""); buf_num(out, y);
            buf_lit(out, "\" rx=\""); buf_num(out, w / 2); buf_lit(out, "\" ry=\""); buf_num(out, h / 2); buf_ch(out, '"');
        } else {
            buf_lit(out, "<rect class=\""); buf_cstr(out, e->kind == K_RECT ? "uc" : e->kind == K_NOTE ? "nt" : "js");
            buf_lit(out, "\" x=\""); buf_num(out, x - w / 2); buf_lit(out, "\" y=\""); buf_num(out, y - h / 2);
            buf_lit(out, "\" width=\""); buf_num(out, w); buf_lit(out, "\" height=\""); buf_num(out, h); buf_ch(out, '"');
        }
        if (e->kind <= K_RECT) color_attrs(out, c);
        flow_style_attr(out, &g, g.nclass[v], g.nstyle[v], false, true);
        buf_lit(out, "/>");
        if (e->kind == K_ELLIPSE && e->business) { /* mermaid usecaseBusiness: a slash on the right */
            double lw;
            unsigned ln;
            meas(g.nlabel[v], fs, &lw, &ln);
            lw = fmax(WRAP, lw);
            double rx = w / 2, ry = h / 2, sx = lw / 2 + 2, ex = rx - 2;
            double sy = ry * sqrt(fmax(0, 1 - sx * sx / (rx * rx))), ey = -ry * sqrt(fmax(0, 1 - ex * ex / (rx * rx)));
            buf_lit(out, "<path class=\"bm\" d=\"M"); buf_pt(out, x + sx, y + sy); buf_ch(out, 'L'); buf_pt(out, x + ex, y + ey);
            buf_ch(out, '"'); color_attrs(out, c); buf_lit(out, " stroke-width=\"2\"/>");
        }
        bool wrapc = flow_text_style_open(out, &g, g.nclass[v], g.nstyle[v]);
        if (e->kind != K_JSON) dg_text(out, g.nlabel[v], fs, x, y, "middle", tcls);
        else { /* title row, then key | value rows */
            double x0 = x - w / 2 + 1, y0 = y - h / 2 + 1, iw = w - 2, kw = 0, tw;
            unsigned tl;
            meas(g.nlabel[v], 14, &tw, &tl);
            for (size_t r = 0; r < e->rows.n; r++) {
                double t;
                unsigned tn;
                meas(e->rows.v[r].key, 14, &t, &tn), kw = fmax(kw, t + 16);
            }
            double th = tl * 21 + 8;
            dg_rect(out, x0, y0, iw, th, 0, "js");
            dg_text(out, g.nlabel[v], 14, x, y0 + th / 2, "middle", "tl");
            y0 += th;
            for (size_t r = 0; r < e->rows.n; r++) {
                double t;
                unsigned kn, vn;
                meas(e->rows.v[r].key, 14, &t, &kn), meas(e->rows.v[r].val, 14, &t, &vn);
                double rh = (kn > vn ? kn : vn) * 21 + 8;
                dg_rect(out, x0, y0, kw, rh, 0, "js");
                dg_rect(out, x0 + kw, y0, iw - kw, rh, 0, "js");
                dg_text(out, e->rows.v[r].key, 14, x0 + kw / 2, y0 + rh / 2, "middle", "tl");
                dg_text(out, e->rows.v[r].val, 14, x0 + kw + (iw - kw) / 2, y0 + rh / 2, "middle", "tl");
                y0 += rh;
            }
        }
        if (wrapc) buf_lit(out, "</g>");
    }
    buf_lit(out, "</svg>\n");
    return out->oom ? MMD_ERR_NOMEM : MMD_OK;
}
