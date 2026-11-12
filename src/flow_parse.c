/* Hand-written single-pass flowchart parser. All strings are views into the source
 * unless normalisation requires a rewrite (then copied into the arena). */
#include <stdio.h>

#include "graph.h"
#include "rich.h"
#include "text.h"

typedef struct Mention { int32_t v, sg; } Mention; /* node v mentioned directly inside subgraph sg */
typedef struct P {
    Graph *g;
    const char *s;
    size_t n, i;
    size_t err;
    char *msg;
    int32_t sg[64];
    size_t sgm[64]; /* ments.n when sg[k] opened */
    unsigned sgd;
    VEC(Mention) ments;
    bool af; /* agentflow-beta: flow/global/connector, shape aliases, container metadata */
} P;
constexpr int32_t AF_GLOBAL = -2; /* sg[] entry for `global … end`, nparent of a node mentioned there */

/* character classes, table driven */
enum { C_ID = 1u, C_SP = 2u, C_LINK = 4u };
static constexpr uint8_t CC[256] = {
    [' '] = C_SP, ['\t'] = C_SP, ['\r'] = C_SP,
    ['-'] = C_LINK, ['='] = C_LINK, ['.'] = C_LINK, ['~'] = C_LINK,
    ['_'] = C_ID,
    ['0' ... '9'] = C_ID, ['A' ... 'Z'] = C_ID, ['a' ... 'z'] = C_ID,
    [0x80 ... 0xFF] = C_ID,
};
static inline bool cid(char c) { return CC[(unsigned char)c] & C_ID; }

static inline char pk(const P p[static 1], size_t k) { return p->i + k < p->n ? p->s[p->i + k] : 0; }
static inline void skip_sp(P p[static 1]) { if (p->i < p->n) p->i = scan_sp(p->n, p->s, p->i); }
static inline bool at_eos(const P p[static 1]) {
    char c = pk(p, 0);
    return c == 0 || c == '\n' || c == ';' || (c == '%' && pk(p, 1) == '%');
}

static mmd_status fail(P p[static 1], const char m[static 1]) {
    p->err = p->i;
    snprintf(p->msg, 160, "%s", m);
    return MMD_ERR_SYNTAX;
}

static Str read_word(P p[static 1]) {
    size_t st = p->i;
    if (st < p->n) p->i = scan_id(p->n, p->s, st);
    return (Str){.p = p->s + st, .n = p->i - st};
}
static Str rest_of_line(P p[static 1]) {
    size_t st = p->i;
    const char *nl = memchr(p->s + st, '\n', p->n - st);
    p->i = nl ? (size_t)(nl - p->s) : p->n;
    return str_trim((Str){.p = p->s + st, .n = p->i - st});
}

static int32_t find_sg(const Graph g[static 1], Str id) {
    for (size_t i = 0; i < g->ns; i++)
        if (str_eq(g->sid[i], id)) return (int32_t)i;
    return NONE;
}

[[nodiscard]] static bool append_css(Arena a[static 1], Str dst[static 1], Str add) {
    add = str_trim(add);
    while (add.n && add.p[add.n - 1] == ';') add.n--;
    if (!add.n) return true;
    if (!dst->n) { *dst = add; return true; }
    char *o = arena_alloc(a, dst->n + add.n + 2, 1);
    if (!o) return false;
    memcpy(o, dst->p, dst->n);
    o[dst->n] = ',';
    memcpy(o + dst->n + 1, add.p, add.n);
    *dst = (Str){.p = o, .n = dst->n + add.n + 1};
    return true;
}
[[nodiscard]] static bool append_class(Arena a[static 1], Str dst[static 1], Str add) {
    if (!dst->n) { *dst = add; return true; }
    char *o = arena_alloc(a, dst->n + add.n + 2, 1);
    if (!o) return false;
    memcpy(o, dst->p, dst->n);
    o[dst->n] = ' ';
    memcpy(o + dst->n + 1, add.p, add.n);
    *dst = (Str){.p = o, .n = dst->n + add.n + 1};
    return true;
}

/* ---- node shapes ---- */
typedef struct ShapeTok { char open[4]; char close[4]; Shape sh; uint8_t ol, cl; } ShapeTok;
#define ST(o, c, s) {o, c, s, sizeof o - 1, sizeof c - 1}
static constexpr ShapeTok SHAPES[] = {
    ST("(((", ")))", SH_DCIRCLE), ST("((", "))", SH_CIRCLE), ST("([", "])", SH_STADIUM),
    ST("(", ")", SH_ROUND),       ST("[[", "]]", SH_SUBROUTINE), ST("[(", ")]", SH_CYLINDER),
    ST("[/", "/]", SH_LEAN_R),    ST("[\\", "\\]", SH_LEAN_L),   ST("[", "]", SH_RECT),
    ST("{{", "}}", SH_HEXAGON), ST("{", "}", SH_RHOMBUS),     ST(">", "]", SH_ASYM),
};

static mmd_status parse_shape_text(P p[static 1], Graph g[static 1], int32_t v) {
    char c0 = pk(p, 0);
    if (c0 != '(' && c0 != '[' && c0 != '{' && c0 != '>') return MMD_OK;
    const ShapeTok *t = nullptr;
    for (size_t k = 0; k < countof(SHAPES); k++) {
        size_t ol = SHAPES[k].ol;
        if (p->s[p->i] == SHAPES[k].open[0] && p->i + ol <= p->n && memcmp(p->s + p->i, SHAPES[k].open, ol) == 0) { t = &SHAPES[k]; break; }
    }
    if (!t) return MMD_OK;
    size_t ol = t->ol;
    p->i += ol;
    size_t st = p->i;
    Shape sh = t->sh;
    size_t cl = t->cl;
    if (pk(p, 0) == '"') { /* quoted: skip to closing quote */
        const char *q = memchr(p->s + p->i + 1, '"', p->n - p->i - 1);
        if (!q) return fail(p, "unterminated string in node text");
        p->i = (size_t)(q - p->s) + 1;
    }
    for (;;) {
        if (p->i >= p->n || p->s[p->i] == '\n') return fail(p, "unterminated node shape");
        if (p->s[p->i] == t->close[0] && p->i + cl <= p->n && memcmp(p->s + p->i, t->close, cl) == 0) break;
        if (sh == SH_LEAN_R && p->s[p->i] == '\\' && pk(p, 1) == ']') { sh = SH_TRAP_ALT; cl = 2; break; }
        if (sh == SH_LEAN_L && p->s[p->i] == '/' && pk(p, 1) == ']') { sh = SH_TRAP; cl = 2; break; }
        p->i++;
    }
    /* [/x\] is trapezoid (wide bottom), [\x/] inverted */
    if (t->sh == SH_LEAN_R && sh == SH_TRAP_ALT) sh = SH_TRAP;
    else if (t->sh == SH_LEAN_L && sh == SH_TRAP) sh = SH_TRAP_ALT;
    Str raw = {.p = p->s + st, .n = p->i - st};
    p->i += cl;
    bool ok;
    Str lab = rich_normalize(g->arena, raw, &ok);
    if (!ok) return MMD_ERR_NOMEM;
    g->nlabel[v] = lab;
    g->nshape[v] = sh;
    g->nflags[v] |= NF_DEFINED;
    return MMD_OK;
}

typedef struct NamedShape { Str name; Shape sh; } NamedShape;
static Shape shape_by_name(Str n) {
    static const NamedShape M[] = {
        {S("rect"), SH_RECT}, {S("rounded"), SH_ROUND}, {S("stadium"), SH_STADIUM},
        {S("subroutine"), SH_SUBROUTINE}, {S("subproc"), SH_SUBROUTINE}, {S("cyl"), SH_CYLINDER},
        {S("database"), SH_CYLINDER}, {S("circle"), SH_CIRCLE}, {S("circ"), SH_CIRCLE},
        {S("dbl-circ"), SH_DCIRCLE}, {S("double-circle"), SH_DCIRCLE}, {S("diamond"), SH_RHOMBUS},
        {S("decision"), SH_RHOMBUS}, {S("diam"), SH_RHOMBUS}, {S("hex"), SH_HEXAGON},
        {S("hexagon"), SH_HEXAGON}, {S("lean-r"), SH_LEAN_R}, {S("lean-l"), SH_LEAN_L},
        {S("trap-b"), SH_TRAP}, {S("trap-t"), SH_TRAP_ALT}, {S("odd"), SH_ASYM},
    };
    for (size_t k = 0; k < countof(M); k++)
        if (str_eq(M[k].name, n)) return M[k].sh;
    return SH_RECT;
}

/* agentflow's shape vocabulary (mermaid agentflow/shapes.ts): its aliases and the canonical names it
 * allows; anything else, removed shapes included, falls back to the rounded default */
static Shape af_shape(Str n) {
    static const NamedShape M[] = {
        {S("tool"), SH_SUBROUTINE}, {S("subroutine"), SH_SUBROUTINE}, {S("subprocess"), SH_SUBROUTINE},
        {S("subproc"), SH_SUBROUTINE}, {S("framed-rectangle"), SH_SUBROUTINE}, {S("input"), SH_LEAN_R},
        {S("lean_right"), SH_LEAN_R}, {S("lean-right"), SH_LEAN_R}, {S("decision"), SH_RHOMBUS},
        {S("diamond"), SH_RHOMBUS}, {S("refdoc"), SH_DOC}, {S("lin-doc"), SH_DOC},
        {S("lined-document"), SH_DOC}, {S("action"), SH_HEXAGON}, {S("hexagon"), SH_HEXAGON},
        {S("hex"), SH_HEXAGON},
    };
    for (size_t k = 0; k < countof(M); k++)
        if (str_eq(M[k].name, n)) return M[k].sh;
    return SH_ROUND;
}

/* next `key: value` of an @{ } body (',' or newline separated; a quoted value keeps its quotes) */
[[nodiscard]] static bool kv_next(Str body[static 1], Str key[static 1], Str val[static 1]) {
    size_t i = 0, n = body->n;
    const char *s = body->p;
    while (i < n && (is_sp(s[i]) || s[i] == ',' || s[i] == '\n')) i++;
    size_t ks = i;
    while (i < n && s[i] != ':') i++;
    if (i >= n) return false;
    *key = str_trim((Str){.p = s + ks, .n = i - ks});
    i++;
    while (i < n && is_sp(s[i])) i++;
    size_t vs = i;
    if (i < n && s[i] == '"') {
        i++;
        while (i < n && s[i] != '"') i++;
        if (i < n) i++;
    } else {
        while (i < n && s[i] != ',' && s[i] != '\n') i++;
    }
    *val = str_trim((Str){.p = s + vs, .n = i - vs});
    *body = (Str){.p = s + i, .n = n - i};
    return true;
}

/* @{ … } at p->i: its body, p->i past the '}' */
static mmd_status at_body(P p[static 1], Str body[static 1]) {
    p->i += 2;
    size_t st = p->i;
    const char *e = memchr(p->s + st, '}', p->n - st);
    if (!e) return fail(p, "unterminated @{ }");
    *body = (Str){.p = p->s + st, .n = (size_t)(e - (p->s + st))};
    p->i = (size_t)(e - p->s) + 1;
    return MMD_OK;
}

/* A@{ shape: x, label: "y" } (v11 syntax, basic) */
static mmd_status parse_at_shape(P p[static 1], Graph g[static 1], int32_t v) {
    Str body, key, val;
    mmd_status st = at_body(p, &body);
    if (st) return st;
    while (kv_next(&body, &key, &val)) {
        if (str_eq(key, S("shape"))) g->nshape[v] = p->af ? af_shape(val) : shape_by_name(val);
        else if (str_eq(key, S("label"))) {
            bool ok;
            g->nlabel[v] = rich_normalize(g->arena, val, &ok);
            if (!ok) return MMD_ERR_NOMEM;
        }
    }
    g->nflags[v] |= NF_DEFINED;
    return MMD_OK;
}

static mmd_status parse_node(P p[static 1], int32_t out[static 1]) {
    Graph *g = p->g;
    Str id = read_word(p);
    if (!id.n) return fail(p, "expected node identifier");
    int32_t v = graph_intern(g, id, nullptr);
    if (v == NONE) return MMD_ERR_NOMEM;
    int32_t top = p->sgd ? p->sg[p->sgd - 1] : NONE;
    if (top == AF_GLOBAL) g->nparent[v] = AF_GLOBAL; /* wins over any flow, before or after */
    else if (top != NONE && !VEC_PUSH(g->arena, &p->ments, ((Mention){v, top}))) return MMD_ERR_NOMEM;
    mmd_status st = MMD_OK;
    if (pk(p, 0) == '@' && pk(p, 1) == '{') st = parse_at_shape(p, g, v);
    else if (!(st = parse_shape_text(p, g, v)) && p->af && pk(p, 0) == '@' && pk(p, 1) == '{')
        st = parse_at_shape(p, g, v); /* agentflow: id["label"]@{ shape: tool } */
    if (st) return st;
    if (pk(p, 0) == ':' && pk(p, 1) == ':' && pk(p, 2) == ':') {
        p->i += 3;
        Str cls = read_word(p);
        if (!cls.n) return fail(p, "expected class name after :::");
        if (!append_class(g->arena, &g->nclass[v], cls)) return MMD_ERR_NOMEM;
    }
    *out = v;
    return MMD_OK;
}

typedef VEC(int32_t) I32Vec;

static mmd_status parse_group(P p[static 1], I32Vec grp[static 1]) {
    grp->n = 0;
    for (;;) {
        skip_sp(p);
        int32_t v;
        mmd_status st = parse_node(p, &v);
        if (st) return st;
        if (!VEC_PUSH(p->g->arena, grp, v)) return MMD_ERR_NOMEM;
        skip_sp(p);
        if (pk(p, 0) != '&') return MMD_OK;
        p->i++;
    }
}

typedef struct Link { Stroke st; Arrow a0, a1; uint16_t len; Str label, id; } Link;

static bool link_start(const P p[static 1]) {
    char c = pk(p, 0), d = pk(p, 1);
    if (c == '-' || c == '=' || c == '~') return true;
    if (c == '.' && d == '-') return true;
    if (c == '<' && (d == '-' || d == '=' || d == '.')) return true;
    if ((c == 'o' || c == 'x') && (d == '-' || d == '=' || d == '.')) return true;
    return false;
}

/* parse a link body at p->i: [-=.~]+ followed by optional head */
[[nodiscard]] static bool link_body(P p[static 1], Link l[static 1], size_t nd[static 1], size_t ne[static 1],
                      size_t ndot[static 1], size_t nt[static 1]) {
    *nd = *ne = *ndot = *nt = 0;
    size_t st = p->i;
    while (p->i < p->n && (CC[(unsigned char)p->s[p->i]] & C_LINK)) {
        char c = p->s[p->i++];
        if (c == '-') ++*nd;
        else if (c == '=') ++*ne;
        else if (c == '.') ++*ndot;
        else ++*nt;
    }
    if (p->i == st) return false;
    char c = pk(p, 0);
    l->a1 = AH_NONE;
    if (c == '>') { l->a1 = AH_ARROW; p->i++; }
    else if (c == 'o' && !cid(pk(p, 1))) { l->a1 = AH_CIRCLE; p->i++; }
    else if (c == 'x' && !cid(pk(p, 1))) { l->a1 = AH_CROSS; p->i++; }
    else if ((c == 'o' || c == 'x') && cid(pk(p, 1)) && p->s[p->i - 1] != '.') {
        /* "--oB" : mermaid lexes the head greedily */
        l->a1 = c == 'o' ? AH_CIRCLE : AH_CROSS;
        p->i++;
    }
    return true;
}

static uint16_t link_len(Stroke s, bool head, size_t nd, size_t ne, size_t ndot, size_t nt) {
    size_t L = 1;
    switch (s) {
    case ST_DOTTED: L = ndot; break;
    case ST_THICK: L = head ? ne - 1 : ne - 2; if (ne < 2) L = 1; break;
    case ST_INVISIBLE: L = nt >= 2 ? nt - 2 : 1; break;
    default: L = head ? nd - 1 : (nd >= 2 ? nd - 2 : 1); break;
    }
    if (L < 1 || L > 1000) L = 1;
    return (uint16_t)L;
}

static mmd_status parse_link(P p[static 1], Link l[static 1]) {
    *l = (Link){};
    char c = pk(p, 0);
    if (c == '<') { l->a0 = AH_ARROW; p->i++; }
    else if ((c == 'o' || c == 'x') && pk(p, 1) != '.') { l->a0 = c == 'o' ? AH_CIRCLE : AH_CROSS; p->i++; }
    size_t bst = p->i, nd, ne, ndot, nt;
    if (!link_body(p, l, &nd, &ne, &ndot, &nt)) return fail(p, "expected link");
    l->st = ne ? ST_THICK : ndot ? ST_DOTTED : nt ? ST_INVISIBLE : ST_NORMAL;
    size_t blen = p->i - bst;
    bool open_label = l->a1 == AH_NONE && blen == 2 && l->a0 == AH_NONE ? true
                     : (l->a1 == AH_NONE && blen == 2);
    if (open_label && (is_sp(pk(p, 0)) || !link_start(p))) {
        /* "-- text -->" / "== text ==>" / "-. text .->" */
        size_t ls = p->i;
        for (;;) {
            if (p->i >= p->n || p->s[p->i] == '\n') return fail(p, "unterminated edge label");
            char d = p->s[p->i];
            if (d == '"') { /* a quoted label is opaque: -- "a--b" --> */
                size_t j = p->i + 1;
                while (j < p->n && p->s[j] != '"' && p->s[j] != '\n') j++;
                if (j < p->n && p->s[j] == '"') { p->i = j + 1; continue; }
            }
            if ((d == '-' || d == '=' || d == '.') && (pk(p, 1) == '-' || pk(p, 1) == '=' ||
                                                       pk(p, 1) == '>' || pk(p, 1) == '.'))
                break;
            p->i++;
        }
        Str raw = {.p = p->s + ls, .n = p->i - ls};
        Link l2 = {};
        if (!link_body(p, &l2, &nd, &ne, &ndot, &nt)) return fail(p, "expected link end");
        l->a1 = l2.a1;
        bool ok;
        l->label = rich_normalize(p->g->arena, raw, &ok);
        if (!ok) return MMD_ERR_NOMEM;
        if (l->st == ST_DOTTED && ndot == 0) ndot = 1;
    }
    l->len = link_len(l->st, l->a1 != AH_NONE, nd, ne, ndot, nt);
    skip_sp(p);
    if (pk(p, 0) == '|') {
        p->i++;
        size_t ls = p->i;
        while (p->i < p->n && p->s[p->i] != '|' && p->s[p->i] != '\n') p->i++;
        if (pk(p, 0) != '|') return fail(p, "unterminated |edge label|");
        bool ok;
        l->label = rich_normalize(p->g->arena, (Str){.p = p->s + ls, .n = p->i - ls}, &ok);
        if (!ok) return MMD_ERR_NOMEM;
        p->i++;
    }
    return MMD_OK;
}

[[nodiscard]] static bool add_edge(Graph g[static 1], int32_t a, int32_t b, const Link l[static 1]) {
    if (!graph_edge_room(g)) return false;
    size_t e = g->ne++;
    g->efrom[e] = a;
    g->eto[e] = b;
    g->elabel[e] = l->label;
    g->estroke[e] = l->st;
    g->estart[e] = l->a0;
    g->eend[e] = l->a1;
    g->elen[e] = l->len;
    g->eid[e] = l->id;
    g->eflags[e] = 0;
    return true;
}

static int32_t find_edge_id(const Graph g[static 1], Str id) {
    for (size_t e = 0; e < g->ne; e++)
        if (g->eid[e].n && str_eq(g->eid[e], id)) return (int32_t)e;
    return NONE;
}

/* e1@{ animate: true, animation: slow } */
static mmd_status parse_edge_props(P p[static 1], int32_t e) {
    p->i += 2;
    const char *end = memchr(p->s + p->i, '}', p->n - p->i);
    if (!end) return fail(p, "unterminated @{ }");
    Str body = {.p = p->s + p->i, .n = (size_t)(end - (p->s + p->i))};
    p->i = (size_t)(end - p->s) + 1;
    Graph *g = p->g;
    for (size_t i = 0; i + 4 <= body.n; i++) {
        if (i + 7 <= body.n && !memcmp(body.p + i, "animate", 7)) {
            size_t j = i + 7;
            while (j < body.n && (body.p[j] == ':' || is_sp(body.p[j]))) j++;
            if (j + 4 <= body.n && !memcmp(body.p + j, "true", 4)) g->eflags[e] |= EF_ANIM;
        }
        if (!memcmp(body.p + i, "slow", 4)) g->eflags[e] |= EF_ANIM | EF_ANIM_SLOW;
        if (!memcmp(body.p + i, "fast", 4)) g->eflags[e] |= EF_ANIM;
    }
    return MMD_OK;
}

/* "id@" immediately followed by a link: edge id. Returns id (empty if none), consumes it. */
static Str edge_id_prefix(P p[static 1]) {
    size_t save = p->i;
    Str w = read_word(p);
    if (w.n && pk(p, 0) == '@') {
        p->i++;
        if (link_start(p)) return w;
    }
    p->i = save;
    return (Str){};
}

static mmd_status parse_chain(P p[static 1]) {
    I32Vec a = {}, b = {};
    {   /* edge property statement: e1@{ ... } */
        size_t save = p->i;
        Str w = read_word(p);
        if (w.n && pk(p, 0) == '@' && pk(p, 1) == '{') {
            int32_t e = find_edge_id(p->g, w);
            if (e != NONE) return parse_edge_props(p, e);
            int32_t s = p->af ? find_sg(p->g, w) : NONE;
            if (s != NONE) { /* agentflow container metadata: only `view: collapsed` is drawn */
                Str body, key, val;
                mmd_status st = at_body(p, &body);
                if (st) return st;
                while (kv_next(&body, &key, &val))
                    if (str_eq(key, S("view")) && (str_eq(val, S("collapsed")) || str_eq(val, S("\"collapsed\"")))) p->g->sflags[s] |= SF_COLLAPSED;
                return MMD_OK;
            }
        }
        p->i = save;
    }
    mmd_status st = parse_group(p, &a);
    if (st) return st;
    for (;;) {
        skip_sp(p);
        if (at_eos(p)) return MMD_OK;
        Str eid = edge_id_prefix(p);
        if (!link_start(p)) return fail(p, "unexpected character, expected link or end of statement");
        Link l;
        if ((st = parse_link(p, &l))) return st;
        l.id = eid;
        if ((st = parse_group(p, &b))) return st;
        for (size_t i = 0; i < a.n; i++)
            for (size_t j = 0; j < b.n; j++)
                if (!add_edge(p->g, a.v[i], b.v[j], &l)) return MMD_ERR_NOMEM;
        I32Vec t = a; a = b; b = t;
    }
}

/* comma separated id list followed by whitespace */
static Str read_list(P p[static 1]) {
    skip_sp(p);
    size_t st = p->i;
    while (p->i < p->n && !is_sp(p->s[p->i]) && p->s[p->i] != '\n') p->i++;
    return (Str){.p = p->s + st, .n = p->i - st};
}
[[nodiscard]] static bool list_next(Str list[static 1], Str item[static 1]) {
    while (list->n && list->p[0] == ',') list->p++, list->n--;
    if (!list->n) return false;
    const char *c = memchr(list->p, ',', list->n);
    size_t len = c ? (size_t)(c - list->p) : list->n;
    *item = str_trim((Str){.p = list->p, .n = len});
    list->p += len;
    list->n -= len;
    return true;
}

static mmd_status parse_subgraph(P p[static 1]) {
    Graph *g = p->g;
    skip_sp(p);
    Str id = {}, title = {};
    bool ok;
    if (pk(p, 0) == '"') {
        size_t st = p->i;
        const char *q = memchr(p->s + st + 1, '"', p->n - st - 1);
        if (!q) return fail(p, "unterminated subgraph title");
        p->i = (size_t)(q - p->s) + 1;
        title = rich_normalize(g->arena, (Str){.p = p->s + st, .n = p->i - st}, &ok);
        if (!ok) return MMD_ERR_NOMEM;
        id = title;
    } else {
        id = read_word(p);
        if (!id.n) return fail(p, "expected subgraph id");
        size_t after_id = p->i;
        skip_sp(p);
        if (pk(p, 0) != '[') p->i = after_id;
        if (pk(p, 0) == '[') {
            p->i++;
            size_t st = p->i;
            while (p->i < p->n && p->s[p->i] != ']' && p->s[p->i] != '\n') p->i++;
            if (pk(p, 0) != ']') return fail(p, "unterminated subgraph title");
            title = rich_normalize(g->arena, (Str){.p = p->s + st, .n = p->i - st}, &ok);
            if (!ok) return MMD_ERR_NOMEM;
            p->i++;
        } else {
            size_t st = id.p - p->s;
            p->i = st;
            Str rest = rest_of_line(p);
            const char *sc = memchr(rest.p, ';', rest.n);
            if (sc) { rest.n = (size_t)(sc - rest.p); p->i = (size_t)(sc - p->s); }
            rest = str_trim(rest);
            title = rest;
            /* id with spaces: mermaid uses the whole text */
            id = rest;
        }
    }
    if (p->sgd >= countof(p->sg)) return fail(p, "subgraphs nested too deeply");
    if (!graph_sg_room(g)) return MMD_ERR_NOMEM;
    size_t s = g->ns++;
    g->sid[s] = id;
    g->stitle[s] = title;
    g->sclass[s] = (Str){};
    g->sstyle[s] = (Str){};
    g->sparent[s] = p->sgd && p->sg[p->sgd - 1] >= 0 ? p->sg[p->sgd - 1] : NONE;
    g->sdir[s] = g->dir;
    g->sflags[s] = 0;
    p->sgm[p->sgd] = p->ments.n;
    p->sg[p->sgd++] = (int32_t)s;
    return MMD_OK;
}

/* mermaid (flowDb makeUniq): subgraphs claim the nodes mentioned directly in them in the order they
 * end, so the innermost one wins over its parents and the first-closed sibling over later ones */
static void close_subgraph(P p[static 1]) {
    int32_t s = p->sg[--p->sgd];
    if (s == AF_GLOBAL) return;
    for (size_t k = p->sgm[p->sgd]; k < p->ments.n; k++) {
        const Mention *m = &p->ments.v[k];
        if (m->sg == s && p->g->nparent[m->v] == NONE) p->g->nparent[m->v] = s;
    }
}

static mmd_status parse_stmt(P p[static 1], Str w) {
    Graph *g = p->g;
    if (str_eq(w, S("subgraph")) || (p->af && str_eq(w, S("flow")))) return parse_subgraph(p);
    if (p->af && str_eq(w, S("global"))) {
        if (p->sgd >= countof(p->sg)) return fail(p, "subgraphs nested too deeply");
        p->sgm[p->sgd] = p->ments.n;
        p->sg[p->sgd++] = AF_GLOBAL;
        return MMD_OK;
    }
    if (p->af && str_eq(w, S("connector"))) {
        skip_sp(p);
        int32_t v;
        mmd_status st = parse_node(p, &v);
        if (st) return st;
        return append_class(g->arena, &g->nclass[v], S("af-kind-connector")) ? MMD_OK : MMD_ERR_NOMEM;
    }
    if (str_eq(w, S("end"))) {
        if (!p->sgd) return fail(p, "'end' without subgraph");
        close_subgraph(p);
        return MMD_OK;
    }
    if (str_eq(w, S("direction"))) {
        skip_sp(p);
        Dir d;
        if (!graph_dir(read_list(p), &d)) return fail(p, "invalid direction");
        int32_t s = p->sgd ? p->sg[p->sgd - 1] : NONE;
        if (s >= 0) { g->sdir[s] = d; g->sflags[s] |= SF_DIRSET; }
        else if (s == NONE) g->dir = d;
        return MMD_OK;
    }
    if (str_eq(w, S("classDef"))) {
        Str names = read_list(p);
        Str css = rest_of_line(p), item;
        while (list_next(&names, &item)) {
            ClassDef cd = {.name = item};
            if (!append_css(g->arena, &cd.css, css)) return MMD_ERR_NOMEM;
            if (!VEC_PUSH(g->arena, &g->classdefs, cd)) return MMD_ERR_NOMEM;
        }
        return MMD_OK;
    }
    if (str_eq(w, S("class"))) {
        Str ids = read_list(p);
        skip_sp(p);
        Str cls = read_word(p), item;
        if (!cls.n) return fail(p, "expected class name");
        while (list_next(&ids, &item)) {
            int32_t s = find_sg(g, item);
            if (s != NONE) { if (!append_class(g->arena, &g->sclass[s], cls)) return MMD_ERR_NOMEM; continue; }
            int32_t v = graph_intern(g, item, nullptr);
            if (v == NONE) return MMD_ERR_NOMEM;
            if (!append_class(g->arena, &g->nclass[v], cls)) return MMD_ERR_NOMEM;
        }
        return MMD_OK;
    }
    if (str_eq(w, S("style"))) {
        skip_sp(p);
        Str id = read_word(p);
        if (!id.n) return fail(p, "expected id after style");
        Str css = rest_of_line(p);
        int32_t s = find_sg(g, id);
        if (s != NONE) return append_css(g->arena, &g->sstyle[s], css) ? MMD_OK : MMD_ERR_NOMEM;
        int32_t v = graph_intern(g, id, nullptr);
        if (v == NONE) return MMD_ERR_NOMEM;
        return append_css(g->arena, &g->nstyle[v], css) ? MMD_OK : MMD_ERR_NOMEM;
    }
    if (str_eq(w, S("linkStyle"))) {
        Str idx = read_list(p), item;
        Str css = rest_of_line(p);
        while (list_next(&idx, &item)) {
            LinkStyle ls = {.idx = -1};
            if (!str_eq(item, S("default"))) {
                int64_t v = 0;
                if (!item.n) return fail(p, "invalid linkStyle index");
                for (size_t k = 0; k < item.n; k++) {
                    if ((unsigned)(item.p[k] - '0') >= 10u || v > 100000000) return fail(p, "invalid linkStyle index");
                    v = v * 10 + (item.p[k] - '0');
                }
                ls.idx = (int32_t)v;
            }
            if (!append_css(g->arena, &ls.css, css)) return MMD_ERR_NOMEM;
            if (!VEC_PUSH(g->arena, &g->linkstyles, ls)) return MMD_ERR_NOMEM;
        }
        return MMD_OK;
    }
    if (str_eq(w, S("click")) || str_eq(w, S("accTitle")) || str_eq(w, S("href")) || str_eq(w, S("callback"))) {
        (void)rest_of_line(p);
        return MMD_OK;
    }
    if (str_eq(w, S("accDescr"))) {
        skip_sp(p);
        if (pk(p, 0) == '{') {
            const char *e = memchr(p->s + p->i, '}', p->n - p->i);
            p->i = e ? (size_t)(e - p->s) + 1 : p->n;
        } else (void)rest_of_line(p);
        return MMD_OK;
    }
    return fail(p, "unknown statement");
}

static bool is_keyword(const P p[static 1], Str w) {
    static const Str K[] = {S("subgraph"), S("end"), S("direction"), S("classDef"), S("class"),
                            S("style"), S("linkStyle"), S("click"), S("accTitle"), S("accDescr")};
    for (size_t k = 0; k < countof(K); k++)
        if (str_eq(K[k], w)) return true;
    return p->af && (str_eq(w, S("flow")) || str_eq(w, S("global")) || str_eq(w, S("connector")));
}

mmd_status flow_parse(Graph g[static 1], size_t n, const char src[static n], size_t off, size_t err_off[static 1],
                      char msg[static 160]) {
    P p = {.g = g, .s = src, .n = n, .i = off, .msg = msg};
    mmd_status st = MMD_OK;
    /* header */
    Str kw = read_word(&p);
    if (str_eq(kw, S("swimlane")) && p.n - p.i >= 5 && !memcmp(src + p.i, "-beta", 5)) p.i += 5, kw = S("flowchart");
    if (str_eq(kw, S("agentflow")) && p.n - p.i >= 5 && !memcmp(src + p.i, "-beta", 5)) p.i += 5, kw = S("flowchart"), p.af = true;
    if (!str_eq(kw, S("graph")) && !str_eq(kw, S("flowchart")) && !str_eq(kw, S("flowchart-elk"))) {
        st = fail(&p, "expected 'graph' or 'flowchart'");
        goto out;
    }
    skip_sp(&p);
    if (!at_eos(&p)) {
        size_t st0 = p.i;
        while (p.i < p.n && !is_sp(p.s[p.i]) && p.s[p.i] != '\n' && p.s[p.i] != ';') p.i++;
        Dir d;
        if (graph_dir((Str){.p = p.s + st0, .n = p.i - st0}, &d)) g->dir = d;
        else { p.i = st0; st = fail(&p, "invalid direction"); goto out; }
    }
    for (;;) {
        while (p.i < p.n && (is_sp(p.s[p.i]) || p.s[p.i] == '\n' || p.s[p.i] == ';')) p.i++;
        if (p.i >= p.n) break;
        if (pk(&p, 0) == '%' && pk(&p, 1) == '%') { (void)rest_of_line(&p); continue; }
        size_t save = p.i;
        Str w = read_word(&p);
        bool kwb = w.n && (p.i >= p.n || is_sp(p.s[p.i]) || p.s[p.i] == '\n' || p.s[p.i] == ';' ||
                           ((p.s[p.i] == ':' || p.s[p.i] == '{') && (str_eq(w, S("accTitle")) || str_eq(w, S("accDescr")))));
        if (kwb && is_keyword(&p, w)) {
            if ((st = parse_stmt(&p, w))) goto out;
        } else {
            p.i = save;
            if ((st = parse_chain(&p))) goto out;
        }
        skip_sp(&p);
        if (pk(&p, 0) == '%' && pk(&p, 1) == '%') (void)rest_of_line(&p);
    }
    while (p.sgd) close_subgraph(&p);
    if (p.af) /* every node is one of agentflow's shapes; `global` leaves its nodes top-level */
        for (size_t v = 0; v < g->nn; v++) {
            if (g->nparent[v] == AF_GLOBAL) g->nparent[v] = NONE;
            Shape s = g->nshape[v];
            if (s != SH_SUBROUTINE && s != SH_LEAN_R && s != SH_RHOMBUS && s != SH_DOC && s != SH_HEXAGON) g->nshape[v] = SH_ROUND;
        }
    /* edges/refs to subgraph ids */
    for (size_t v = 0; v < g->nn; v++) {
        if (g->nflags[v] & NF_DEFINED) continue;
        if (find_sg(g, g->nid[v]) != NONE) g->nflags[v] |= NF_SUBREF | NF_HIDDEN;
    }
out:
    *err_off = p.err;
    return st;
}
