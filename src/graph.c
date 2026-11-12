/* the Graph tables (graph.h) shared by the flowchart parser and dgraph: growth, node interning,
 * direction words and the small parse helpers (subgraph lookup, class/style lists, @{ } bodies).
 * One place grows every parse-time SoA array, so a new field can't be missed. */
#include "graph.h"

/* 8 bytes a step; as well spread as FNV-1a and faster on ids longer than a few bytes */
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

#define GROW(A, arr, oldc, newc)                                                                  \
    do {                                                                                          \
        typeof(arr) na_ = arena_alloc_array((A), (newc), sizeof *(arr), alignof(typeof(*(arr)))); \
        if (!na_) return false;                                                                   \
        if ((oldc)) memcpy(na_, (arr), (oldc) * sizeof *(arr));                                   \
        (arr) = na_;                                                                              \
    } while (0)

/* every per-node array the parsers fill (the layout allocates nx, ny itself) */
[[nodiscard]] static bool node_cap(Graph g[static 1], size_t nc) {
    size_t oc = g->ncap;
    Arena *A = g->arena;
    GROW(A, g->nid, oc, nc); GROW(A, g->nlabel, oc, nc); GROW(A, g->nclass, oc, nc);
    GROW(A, g->nstyle, oc, nc); GROW(A, g->nshape, oc, nc); GROW(A, g->nparent, oc, nc);
    GROW(A, g->nflags, oc, nc); GROW(A, g->nw, oc, nc); GROW(A, g->nh, oc, nc);
    g->ncap = nc;
    return true;
}

[[nodiscard]] static bool edge_cap(Graph g[static 1], size_t nc) {
    size_t oc = g->ecap;
    Arena *A = g->arena;
    GROW(A, g->efrom, oc, nc); GROW(A, g->eto, oc, nc); GROW(A, g->elabel, oc, nc);
    GROW(A, g->estroke, oc, nc); GROW(A, g->estart, oc, nc); GROW(A, g->eend, oc, nc);
    GROW(A, g->elen, oc, nc); GROW(A, g->eid, oc, nc); GROW(A, g->eflags, oc, nc);
    g->ecap = nc;
    return true;
}

[[nodiscard]] static bool sg_cap(Graph g[static 1], size_t nc) {
    size_t oc = g->scap;
    Arena *A = g->arena;
    GROW(A, g->sid, oc, nc); GROW(A, g->stitle, oc, nc); GROW(A, g->sclass, oc, nc);
    GROW(A, g->sstyle, oc, nc); GROW(A, g->sparent, oc, nc); GROW(A, g->sdir, oc, nc);
    GROW(A, g->sflags, oc, nc);
    g->scap = nc;
    return true;
}

bool graph_reserve(Graph g[static 1], size_t nn, size_t ne, size_t ns) {
    return node_cap(g, nn) && edge_cap(g, ne) && sg_cap(g, ns);
}
bool graph_edge_room(Graph g[static 1]) {
    return g->ne < g->ecap || (g->ne < INT32_MAX / 2 && edge_cap(g, g->ecap ? g->ecap * 2 : 64));
}
bool graph_sg_room(Graph g[static 1]) {
    return g->ns < g->scap || (g->ns < 1u << 20 && sg_cap(g, g->scap ? g->scap * 2 : 16));
}

[[nodiscard]] static bool rehash(Graph g[static 1]) {
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

int32_t graph_find(const Graph g[static 1], Str id) {
    if (!g->hcap) return NONE;
    for (size_t h = hash_str(id) & (g->hcap - 1);; h = (h + 1) & (g->hcap - 1)) {
        uint32_t e = g->htab[h];
        if (!e) return NONE;
        if (str_eq(g->nid[e - 1], id)) return (int32_t)e - 1;
    }
}

int32_t graph_intern(Graph g[static 1], Str id, bool *fresh) {
    if (fresh) *fresh = false;
    int32_t f = graph_find(g, id);
    if (f != NONE) return f;
    if (g->nn >= INT32_MAX / 2) return NONE;
    if (g->nn == g->ncap && !node_cap(g, g->ncap ? g->ncap * 2 : 64)) return NONE;
    if ((g->nn + 1) * 2 > g->hcap && !rehash(g)) return NONE;
    size_t v = g->nn++;
    g->nid[v] = g->nlabel[v] = id;
    g->nclass[v] = g->nstyle[v] = (Str){};
    g->nshape[v] = SH_RECT;
    g->nparent[v] = NONE;
    g->nflags[v] = 0;
    g->nw[v] = g->nh[v] = 0;
    size_t h = hash_str(id) & (g->hcap - 1);
    while (g->htab[h]) h = (h + 1) & (g->hcap - 1);
    g->htab[h] = (uint32_t)v + 1;
    if (fresh) *fresh = true;
    return (int32_t)v;
}

bool graph_dir(Str w, Dir d[static 1]) {
    if (str_eq(w, S("TD")) || str_eq(w, S("TB")) || str_eq(w, S("v"))) return *d = DIR_TB, true;
    if (str_eq(w, S("BT")) || str_eq(w, S("^"))) return *d = DIR_BT, true;
    if (str_eq(w, S("LR")) || str_eq(w, S(">"))) return *d = DIR_LR, true;
    if (str_eq(w, S("RL")) || str_eq(w, S("<"))) return *d = DIR_RL, true;
    return false;
}

int32_t graph_find_sg(const Graph g[static 1], Str id) {
    for (size_t i = 0; i < g->ns; i++)
        if (str_eq(g->sid[i], id)) return (int32_t)i;
    return NONE;
}

bool graph_add_css(Arena a[static 1], Str dst[static 1], Str add) {
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

bool graph_add_class(Arena a[static 1], Str dst[static 1], Str add) {
    if (!dst->n) { *dst = add; return true; }
    char *o = arena_alloc(a, dst->n + add.n + 2, 1);
    if (!o) return false;
    memcpy(o, dst->p, dst->n);
    o[dst->n] = ' ';
    memcpy(o + dst->n + 1, add.p, add.n);
    *dst = (Str){.p = o, .n = dst->n + add.n + 1};
    return true;
}

bool graph_kv_next(Str body[static 1], Str key[static 1], Str val[static 1]) {
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
