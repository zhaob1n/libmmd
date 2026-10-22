/* Hand-written single-pass flowchart parser. All strings are views into the source
 * unless normalisation requires a rewrite (then copied into the arena). */
#include <stdio.h>

#include "graph.h"
#include "rich.h"
#include "text.h"

typedef struct P {
    Graph *g;
    const char *s;
    size_t n, i;
    size_t err;
    char *msg;
    int32_t sg[64];
    unsigned sgd;
} P;

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

static inline char pk(const P *p, size_t k) { return p->i + k < p->n ? p->s[p->i + k] : 0; }
static inline void skip_sp(P *p) { while (p->i < p->n && (CC[(unsigned char)p->s[p->i]] & C_SP)) p->i++; }
static inline bool at_eos(const P *p) {
    char c = pk(p, 0);
    return c == 0 || c == '\n' || c == ';' || (c == '%' && pk(p, 1) == '%');
}

static cmm_status fail(P *p, const char *m) {
    p->err = p->i;
    snprintf(p->msg, 160, "%s", m);
    return CMM_ERR_SYNTAX;
}

static Str read_word(P *p) {
    size_t st = p->i;
    while (p->i < p->n && cid(p->s[p->i])) p->i++;
    return (Str){.p = p->s + st, .n = p->i - st};
}
static Str rest_of_line(P *p) {
    size_t st = p->i;
    const char *nl = memchr(p->s + st, '\n', p->n - st);
    p->i = nl ? (size_t)(nl - p->s) : p->n;
    return str_trim((Str){.p = p->s + st, .n = p->i - st});
}

/* ---- hashing / interning ---- */
static inline uint64_t hash_str(Str s) {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ s.n;
    size_t i = 0;
    for (; i + 8 <= s.n; i += 8) {
        uint64_t w;
        memcpy(&w, s.p + i, 8);
        h = (h ^ w) * 0xff51afd7ed558ccdull;
        h ^= h >> 32;
    }
    uint64_t t = 0;
    for (size_t k = 0; i < s.n; i++, k++) t |= (uint64_t)(unsigned char)s.p[i] << (k * 8);
    h = (h ^ t) * 0xc4ceb9fe1a85ec53ull;
    return h ^ (h >> 29);
}

#define GROW(A, arr, oldc, newc)                                                              \
    do {                                                                                      \
        typeof(arr) na_ = arena_alloc_array((A), (newc), sizeof *(arr), alignof(typeof(*(arr)))); \
        if (!na_) return false;                                                               \
        if ((oldc)) memcpy(na_, (arr), (oldc) * sizeof *(arr));                               \
        (arr) = na_;                                                                          \
    } while (0)

[[nodiscard]] static bool node_grow(Graph *g) {
    size_t oc = g->ncap, nc = oc ? oc * 2 : 64;
    Arena *A = g->arena;
    GROW(A, g->nid, oc, nc); GROW(A, g->nlabel, oc, nc); GROW(A, g->nclass, oc, nc);
    GROW(A, g->nstyle, oc, nc); GROW(A, g->nshape, oc, nc); GROW(A, g->nparent, oc, nc);
    GROW(A, g->nflags, oc, nc);
    g->ncap = nc;
    return true;
}
[[nodiscard]] static bool edge_grow(Graph *g) {
    size_t oc = g->ecap, nc = oc ? oc * 2 : 64;
    Arena *A = g->arena;
    GROW(A, g->efrom, oc, nc); GROW(A, g->eto, oc, nc); GROW(A, g->elabel, oc, nc);
    GROW(A, g->estroke, oc, nc); GROW(A, g->estart, oc, nc); GROW(A, g->eend, oc, nc);
    GROW(A, g->elen, oc, nc); GROW(A, g->eid, oc, nc); GROW(A, g->eflags, oc, nc);
    g->ecap = nc;
    return true;
}
[[nodiscard]] static bool sg_grow(Graph *g) {
    size_t oc = g->scap, nc = oc ? oc * 2 : 16;
    Arena *A = g->arena;
    GROW(A, g->sid, oc, nc); GROW(A, g->stitle, oc, nc); GROW(A, g->sclass, oc, nc);
    GROW(A, g->sstyle, oc, nc); GROW(A, g->sparent, oc, nc); GROW(A, g->sdir, oc, nc);
    GROW(A, g->sflags, oc, nc);
    g->scap = nc;
    return true;
}

[[nodiscard]] static bool htab_rehash(Graph *g) {
    size_t nc = g->hcap ? g->hcap * 2 : 128;
    uint32_t *t = ANEW(g->arena, uint32_t, nc);
    if (!t) return false;
    memset(t, 0, nc * sizeof *t);
    for (size_t v = 0; v < g->nn; v++) {
        size_t h = hash_str(g->nid[v]) & (nc - 1);
        while (t[h]) h = (h + 1) & (nc - 1);
        t[h] = (uint32_t)v + 1;
    }
    g->htab = t;
    g->hcap = nc;
    return true;
}

/* returns index or NONE on OOM */
static int32_t intern(Graph *g, Str id) {
    if (g->hcap) {
        size_t h = hash_str(id) & (g->hcap - 1);
        for (;;) {
            uint32_t e = g->htab[h];
            if (!e) break;
            if (str_eq(g->nid[e - 1], id)) return (int32_t)e - 1;
            h = (h + 1) & (g->hcap - 1);
        }
    }
    if (g->nn >= INT32_MAX - 1) return NONE;
    if (g->nn == g->ncap && !node_grow(g)) return NONE;
    if ((g->nn + 1) * 2 > g->hcap && !htab_rehash(g)) return NONE;
    size_t v = g->nn++;
    g->nid[v] = id;
    g->nlabel[v] = id;
    g->nclass[v] = (Str){};
    g->nstyle[v] = (Str){};
    g->nshape[v] = SH_RECT;
    g->nparent[v] = NONE;
    g->nflags[v] = 0;
    size_t h = hash_str(id) & (g->hcap - 1);
    while (g->htab[h]) h = (h + 1) & (g->hcap - 1);
    g->htab[h] = (uint32_t)v + 1;
    return (int32_t)v;
}

static int32_t find_sg(const Graph *g, Str id) {
    for (size_t i = 0; i < g->ns; i++)
        if (str_eq(g->sid[i], id)) return (int32_t)i;
    return NONE;
}

[[nodiscard]] static bool append_css(Arena *a, Str *dst, Str add) {
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
[[nodiscard]] static bool append_class(Arena *a, Str *dst, Str add) {
    if (!dst->n) { *dst = add; return true; }
    char *o = arena_alloc(a, dst->n + add.n + 2, 1);
    if (!o) return false;
    memcpy(o, dst->p, dst->n);
    o[dst->n] = ' ';
    memcpy(o + dst->n + 1, add.p, add.n);
    *dst = (Str){.p = o, .n = dst->n + add.n + 1};
    return true;
}

static bool parse_dir(Str w, Dir *d) {
    if (str_eq(w, S("TD")) || str_eq(w, S("TB")) || str_eq(w, S("v"))) { *d = DIR_TB; return true; }
    if (str_eq(w, S("BT")) || str_eq(w, S("^"))) { *d = DIR_BT; return true; }
    if (str_eq(w, S("LR")) || str_eq(w, S(">"))) { *d = DIR_LR; return true; }
    if (str_eq(w, S("RL")) || str_eq(w, S("<"))) { *d = DIR_RL; return true; }
    return false;
}

/* ---- node shapes ---- */
typedef struct ShapeTok { char open[4]; char close[4]; Shape sh; } ShapeTok;
static constexpr ShapeTok SHAPES[] = {
    {"(((", ")))", SH_DCIRCLE}, {"((", "))", SH_CIRCLE}, {"([", "])", SH_STADIUM},
    {"(", ")", SH_ROUND},       {"[[", "]]", SH_SUBROUTINE}, {"[(", ")]", SH_CYLINDER},
    {"[/", "/]", SH_LEAN_R},    {"[\\", "\\]", SH_LEAN_L},   {"[", "]", SH_RECT},
    {"{{", "}}", SH_HEXAGON},   {"{", "}", SH_RHOMBUS},      {">", "]", SH_ASYM},
};

static cmm_status parse_shape_text(P *p, Graph *g, int32_t v) {
    char c0 = pk(p, 0);
    if (c0 != '(' && c0 != '[' && c0 != '{' && c0 != '>') return CMM_OK;
    const ShapeTok *t = nullptr;
    for (size_t k = 0; k < countof(SHAPES); k++) {
        size_t ol = strlen(SHAPES[k].open);
        if (p->i + ol <= p->n && memcmp(p->s + p->i, SHAPES[k].open, ol) == 0) { t = &SHAPES[k]; break; }
    }
    if (!t) return CMM_OK;
    size_t ol = strlen(t->open);
    p->i += ol;
    size_t st = p->i;
    Shape sh = t->sh;
    size_t cl = strlen(t->close);
    if (pk(p, 0) == '"') { /* quoted: skip to closing quote */
        const char *q = memchr(p->s + p->i + 1, '"', p->n - p->i - 1);
        if (!q) return fail(p, "unterminated string in node text");
        p->i = (size_t)(q - p->s) + 1;
    }
    for (;;) {
        if (p->i >= p->n || p->s[p->i] == '\n') return fail(p, "unterminated node shape");
        if (p->i + cl <= p->n && memcmp(p->s + p->i, t->close, cl) == 0) break;
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
    if (!ok) return CMM_ERR_NOMEM;
    g->nlabel[v] = lab;
    g->nshape[v] = sh;
    g->nflags[v] |= NF_DEFINED;
    return CMM_OK;
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

/* A@{ shape: x, label: "y" } (v11 syntax, basic) */
static cmm_status parse_at_shape(P *p, Graph *g, int32_t v) {
    p->i += 2;
    size_t st = p->i;
    const char *e = memchr(p->s + st, '}', p->n - st);
    if (!e) return fail(p, "unterminated @{ }");
    Str body = {.p = p->s + st, .n = (size_t)(e - (p->s + st))};
    p->i = (size_t)(e - p->s) + 1;
    size_t i = 0;
    while (i < body.n) {
        while (i < body.n && (is_sp(body.p[i]) || body.p[i] == ',' || body.p[i] == '\n')) i++;
        size_t ks = i;
        while (i < body.n && body.p[i] != ':') i++;
        Str key = str_trim((Str){.p = body.p + ks, .n = i - ks});
        if (i >= body.n) break;
        i++;
        while (i < body.n && is_sp(body.p[i])) i++;
        size_t vs = i;
        if (i < body.n && body.p[i] == '"') {
            i++;
            while (i < body.n && body.p[i] != '"') i++;
            if (i < body.n) i++;
        } else {
            while (i < body.n && body.p[i] != ',' && body.p[i] != '\n') i++;
        }
        Str val = str_trim((Str){.p = body.p + vs, .n = i - vs});
        if (str_eq(key, S("shape"))) g->nshape[v] = shape_by_name(val);
        else if (str_eq(key, S("label"))) {
            bool ok;
            g->nlabel[v] = rich_normalize(g->arena, val, &ok);
            if (!ok) return CMM_ERR_NOMEM;
        }
    }
    g->nflags[v] |= NF_DEFINED;
    return CMM_OK;
}

static cmm_status parse_node(P *p, int32_t *out) {
    Graph *g = p->g;
    Str id = read_word(p);
    if (!id.n) return fail(p, "expected node identifier");
    int32_t v = intern(g, id);
    if (v == NONE) return CMM_ERR_NOMEM;
    if (p->sgd && g->nparent[v] == NONE) g->nparent[v] = p->sg[p->sgd - 1];
    cmm_status st = CMM_OK;
    if (pk(p, 0) == '@' && pk(p, 1) == '{') st = parse_at_shape(p, g, v);
    else st = parse_shape_text(p, g, v);
    if (st) return st;
    if (pk(p, 0) == ':' && pk(p, 1) == ':' && pk(p, 2) == ':') {
        p->i += 3;
        Str cls = read_word(p);
        if (!cls.n) return fail(p, "expected class name after :::");
        if (!append_class(g->arena, &g->nclass[v], cls)) return CMM_ERR_NOMEM;
    }
    *out = v;
    return CMM_OK;
}

typedef VEC(int32_t) I32Vec;

static cmm_status parse_group(P *p, I32Vec *grp) {
    grp->n = 0;
    for (;;) {
        skip_sp(p);
        int32_t v;
        cmm_status st = parse_node(p, &v);
        if (st) return st;
        if (!VEC_PUSH(p->g->arena, grp, v)) return CMM_ERR_NOMEM;
        skip_sp(p);
        if (pk(p, 0) != '&') return CMM_OK;
        p->i++;
    }
}

typedef struct Link { Stroke st; Arrow a0, a1; uint16_t len; Str label, id; } Link;

static bool link_start(const P *p) {
    char c = pk(p, 0), d = pk(p, 1);
    if (c == '-' || c == '=' || c == '~') return true;
    if (c == '.' && d == '-') return true;
    if (c == '<' && (d == '-' || d == '=' || d == '.')) return true;
    if ((c == 'o' || c == 'x') && (d == '-' || d == '=' || d == '.')) return true;
    return false;
}

/* parse a link body at p->i: [-=.~]+ followed by optional head */
static bool link_body(P *p, Link *l, size_t *nd, size_t *ne, size_t *ndot, size_t *nt) {
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

static cmm_status parse_link(P *p, Link *l) {
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
        if (!ok) return CMM_ERR_NOMEM;
        if (l->st == ST_DOTTED && ndot == 0) ndot = 1;
        if (l->st == ST_DOTTED) ndot = ndot ? ndot : 1;
        if (l->st == ST_NORMAL) nd = nd + 0;
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
        if (!ok) return CMM_ERR_NOMEM;
        p->i++;
    }
    return CMM_OK;
}

[[nodiscard]] static bool add_edge(Graph *g, int32_t a, int32_t b, const Link *l) {
    if (g->ne == g->ecap && !edge_grow(g)) return false;
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

static int32_t find_edge_id(const Graph *g, Str id) {
    for (size_t e = 0; e < g->ne; e++)
        if (g->eid[e].n && str_eq(g->eid[e], id)) return (int32_t)e;
    return NONE;
}

/* e1@{ animate: true, animation: slow } */
static cmm_status parse_edge_props(P *p, int32_t e) {
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
    return CMM_OK;
}

/* "id@" immediately followed by a link: edge id. Returns id (empty if none), consumes it. */
static Str edge_id_prefix(P *p) {
    size_t save = p->i;
    Str w = read_word(p);
    if (w.n && pk(p, 0) == '@') {
        p->i++;
        if (link_start(p)) return w;
    }
    p->i = save;
    return (Str){};
}

static cmm_status parse_chain(P *p) {
    I32Vec a = {}, b = {};
    {   /* edge property statement: e1@{ ... } */
        size_t save = p->i;
        Str w = read_word(p);
        if (w.n && pk(p, 0) == '@' && pk(p, 1) == '{') {
            int32_t e = find_edge_id(p->g, w);
            if (e != NONE) return parse_edge_props(p, e);
        }
        p->i = save;
    }
    cmm_status st = parse_group(p, &a);
    if (st) return st;
    for (;;) {
        skip_sp(p);
        if (at_eos(p)) return CMM_OK;
        Str eid = edge_id_prefix(p);
        if (!link_start(p)) return fail(p, "unexpected character, expected link or end of statement");
        Link l;
        if ((st = parse_link(p, &l))) return st;
        l.id = eid;
        if ((st = parse_group(p, &b))) return st;
        for (size_t i = 0; i < a.n; i++)
            for (size_t j = 0; j < b.n; j++)
                if (!add_edge(p->g, a.v[i], b.v[j], &l)) return CMM_ERR_NOMEM;
        I32Vec t = a; a = b; b = t;
    }
}

/* comma separated id list followed by whitespace */
static Str read_list(P *p) {
    skip_sp(p);
    size_t st = p->i;
    while (p->i < p->n && !is_sp(p->s[p->i]) && p->s[p->i] != '\n') p->i++;
    return (Str){.p = p->s + st, .n = p->i - st};
}
static bool list_next(Str *list, Str *item) {
    while (list->n && list->p[0] == ',') list->p++, list->n--;
    if (!list->n) return false;
    const char *c = memchr(list->p, ',', list->n);
    size_t len = c ? (size_t)(c - list->p) : list->n;
    *item = str_trim((Str){.p = list->p, .n = len});
    list->p += len;
    list->n -= len;
    return true;
}

static cmm_status parse_subgraph(P *p) {
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
        if (!ok) return CMM_ERR_NOMEM;
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
            if (!ok) return CMM_ERR_NOMEM;
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
    if (g->ns == g->scap && !sg_grow(g)) return CMM_ERR_NOMEM;
    size_t s = g->ns++;
    g->sid[s] = id;
    g->stitle[s] = title;
    g->sclass[s] = (Str){};
    g->sstyle[s] = (Str){};
    g->sparent[s] = p->sgd ? p->sg[p->sgd - 1] : NONE;
    g->sdir[s] = g->dir;
    g->sflags[s] = 0;
    p->sg[p->sgd++] = (int32_t)s;
    return CMM_OK;
}

static cmm_status parse_stmt(P *p, Str w) {
    Graph *g = p->g;
    if (str_eq(w, S("subgraph"))) return parse_subgraph(p);
    if (str_eq(w, S("end"))) {
        if (!p->sgd) return fail(p, "'end' without subgraph");
        p->sgd--;
        return CMM_OK;
    }
    if (str_eq(w, S("direction"))) {
        skip_sp(p);
        Dir d;
        if (!parse_dir(read_list(p), &d)) return fail(p, "invalid direction");
        if (p->sgd) { int32_t s = p->sg[p->sgd - 1]; g->sdir[s] = d; g->sflags[s] |= SF_DIRSET; }
        else g->dir = d;
        return CMM_OK;
    }
    if (str_eq(w, S("classDef"))) {
        Str names = read_list(p);
        Str css = rest_of_line(p), item;
        while (list_next(&names, &item)) {
            ClassDef cd = {.name = item};
            if (!append_css(g->arena, &cd.css, css)) return CMM_ERR_NOMEM;
            if (!VEC_PUSH(g->arena, &g->classdefs, cd)) return CMM_ERR_NOMEM;
        }
        return CMM_OK;
    }
    if (str_eq(w, S("class"))) {
        Str ids = read_list(p);
        skip_sp(p);
        Str cls = read_word(p), item;
        if (!cls.n) return fail(p, "expected class name");
        while (list_next(&ids, &item)) {
            int32_t s = find_sg(g, item);
            if (s != NONE) { if (!append_class(g->arena, &g->sclass[s], cls)) return CMM_ERR_NOMEM; continue; }
            int32_t v = intern(g, item);
            if (v == NONE) return CMM_ERR_NOMEM;
            if (!append_class(g->arena, &g->nclass[v], cls)) return CMM_ERR_NOMEM;
        }
        return CMM_OK;
    }
    if (str_eq(w, S("style"))) {
        skip_sp(p);
        Str id = read_word(p);
        if (!id.n) return fail(p, "expected id after style");
        Str css = rest_of_line(p);
        int32_t s = find_sg(g, id);
        if (s != NONE) return append_css(g->arena, &g->sstyle[s], css) ? CMM_OK : CMM_ERR_NOMEM;
        int32_t v = intern(g, id);
        if (v == NONE) return CMM_ERR_NOMEM;
        return append_css(g->arena, &g->nstyle[v], css) ? CMM_OK : CMM_ERR_NOMEM;
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
            if (!append_css(g->arena, &ls.css, css)) return CMM_ERR_NOMEM;
            if (!VEC_PUSH(g->arena, &g->linkstyles, ls)) return CMM_ERR_NOMEM;
        }
        return CMM_OK;
    }
    if (str_eq(w, S("click")) || str_eq(w, S("accTitle")) || str_eq(w, S("href")) || str_eq(w, S("callback"))) {
        (void)rest_of_line(p);
        return CMM_OK;
    }
    if (str_eq(w, S("accDescr"))) {
        skip_sp(p);
        if (pk(p, 0) == '{') {
            const char *e = memchr(p->s + p->i, '}', p->n - p->i);
            p->i = e ? (size_t)(e - p->s) + 1 : p->n;
        } else (void)rest_of_line(p);
        return CMM_OK;
    }
    return fail(p, "unknown statement");
}

static bool is_keyword(Str w) {
    static const Str K[] = {S("subgraph"), S("end"), S("direction"), S("classDef"), S("class"),
                            S("style"), S("linkStyle"), S("click"), S("accTitle"), S("accDescr")};
    for (size_t k = 0; k < countof(K); k++)
        if (str_eq(K[k], w)) return true;
    return false;
}

cmm_status flow_parse(Graph *g, size_t n, const char src[static n], size_t off, size_t *err_off,
                      char msg[static 160]) {
    P p = {.g = g, .s = src, .n = n, .i = off, .msg = msg};
    cmm_status st = CMM_OK;
    /* header */
    Str kw = read_word(&p);
    if (!str_eq(kw, S("graph")) && !str_eq(kw, S("flowchart")) && !str_eq(kw, S("flowchart-elk"))) {
        st = fail(&p, "expected 'graph' or 'flowchart'");
        goto out;
    }
    skip_sp(&p);
    if (!at_eos(&p)) {
        size_t st0 = p.i;
        while (p.i < p.n && !is_sp(p.s[p.i]) && p.s[p.i] != '\n' && p.s[p.i] != ';') p.i++;
        Dir d;
        if (parse_dir((Str){.p = p.s + st0, .n = p.i - st0}, &d)) g->dir = d;
        else { p.i = st0; st = fail(&p, "invalid direction"); goto out; }
    }
    for (;;) {
        while (p.i < p.n && (is_sp(p.s[p.i]) || p.s[p.i] == '\n' || p.s[p.i] == ';')) p.i++;
        if (p.i >= p.n) break;
        if (pk(&p, 0) == '%' && pk(&p, 1) == '%') { (void)rest_of_line(&p); continue; }
        size_t save = p.i;
        Str w = read_word(&p);
        bool kwb = w.n && (p.i >= p.n || is_sp(p.s[p.i]) || p.s[p.i] == '\n' || p.s[p.i] == ';');
        if (kwb && is_keyword(w)) {
            if ((st = parse_stmt(&p, w))) goto out;
        } else {
            p.i = save;
            if ((st = parse_chain(&p))) goto out;
        }
        skip_sp(&p);
        if (pk(&p, 0) == '%' && pk(&p, 1) == '%') (void)rest_of_line(&p);
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
