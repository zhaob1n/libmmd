/* Faithful port of dagre's compound ranking (see drank.h). Iteration orders follow graphlib:
 * g.nodes()/children() and undirected-tree neighbour lists put array-index keys first (ascending)
 * then insertion order; edge lists keep insertion order; _.minBy/_.find keep the first hit. */
#include "drank.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct RG {
    Arena *A;
    size_t M, SE, maxte;
    const int64_t *key;    /* [M] */
    const uint32_t *sord;  /* [M] position in JS string order */
    int32_t *sv, *sw, *wt, *ml;
    uint32_t *eoff, *eidx; /* nodeEdges: inEdges then outEdges, each in edge order */
    int64_t *r;
    /* undirected tree */
    uint8_t *tin;
    uint32_t *tord, nint, nnon; /* t.nodes(): integer keys sorted, then insertion order */
    uint32_t *tnon;
    int32_t *ta, *tb, *tge;
    uint8_t *talive;
    int64_t *tcut;
    size_t nte;
    int64_t *low, *lim;
    int32_t *par, *pe;
    uint32_t *post, *stk, *sit, *nboff, *nbt;
    uint64_t *nbk;
    uint32_t *acnt, *adj, *path; /* incremental tree adjacency (capacity = graph degree) */
} RG;

static int64_t slack(const RG *g, size_t e) { return g->r[g->sw[e]] - g->r[g->sv[e]] - g->ml[e]; }

static void t_add_node(RG *g, uint32_t v) {
    g->tin[v] = 1;
    if (g->key[v] >= 0) {
        uint32_t i = g->nint++;
        while (i > 0 && g->key[g->tord[i - 1]] > g->key[v]) { g->tord[i] = g->tord[i - 1]; i--; }
        g->tord[i] = v;
    } else
        g->tnon[g->nnon++] = v;
}

static void t_nodes(const RG *g, uint32_t *out) {
    memcpy(out, g->tord, g->nint * sizeof *out);
    memcpy(out + g->nint, g->tnon, g->nnon * sizeof *out);
}

static bool t_add_edge(RG *g, int32_t a, int32_t b, int32_t ge) {
    if (g->nte >= g->maxte) return false;
    if (g->sord[a] > g->sord[b]) { int32_t t = a; a = b; b = t; }
    g->ta[g->nte] = a, g->tb[g->nte] = b, g->tge[g->nte] = ge, g->talive[g->nte] = 1, g->tcut[g->nte] = 0;
    g->nte++;
    return true;
}

static int cmp_u64pair(const void *x, const void *y) {
    uint64_t a = *(const uint64_t *)x, b = *(const uint64_t *)y;
    return (a > b) - (a < b);
}

/* neighbour CSR of the tree: preds (smaller id) then succs, each integer keys first then key insertion */
static void t_neighbors(RG *g) {
    size_t M = g->M;
    memset(g->nboff, 0, (M + 2) * sizeof *g->nboff);
    for (size_t t = 0; t < g->nte; t++)
        if (g->talive[t]) g->nboff[g->ta[t] + 2]++, g->nboff[g->tb[t] + 2]++;
    for (size_t v = 0; v < M; v++) g->nboff[v + 2] += g->nboff[v + 1];
    for (size_t t = 0; t < g->nte; t++) {
        if (!g->talive[t]) continue;
        for (unsigned s = 0; s < 2; s++) {
            int32_t x = s ? g->tb[t] : g->ta[t], o = s ? g->ta[t] : g->tb[t];
            uint64_t grp = s ? 0 : 1; /* for tb, ta is a predecessor */
            uint64_t k = g->key[o] >= 0 ? (uint64_t)g->key[o] : ((uint64_t)1 << 40) + t;
            uint32_t i = g->nboff[x + 1]++;
            g->nbk[2 * i] = grp << 62 | k;
            g->nbk[2 * i + 1] = (uint64_t)o << 32 | (uint32_t)t;
        }
    }
    for (size_t v = 0; v < M; v++) {
        uint32_t a = g->nboff[v], n = g->nboff[v + 1] - a;
        if (n > 1) qsort(g->nbk + 2 * a, n, 2 * sizeof(uint64_t), cmp_u64pair);
    }
}

/* initLowLimValues(t, t.nodes()[0]) + postorder; sets par/pe */
static void lowlim(RG *g) {
    t_neighbors(g);
    t_nodes(g, g->post); /* temp: root = first */
    uint32_t root = g->post[0];
    size_t M = g->M, sp = 0, np = 0;
    int64_t next = 1;
    for (size_t v = 0; v < M; v++) g->lim[v] = -1, g->par[v] = -1, g->pe[v] = -1;
    g->stk[sp] = root, g->sit[sp] = g->nboff[root], sp++;
    g->low[root] = next;
    g->lim[root] = 0; /* visited marker */
    while (sp) {
        uint32_t v = g->stk[sp - 1];
        if (g->sit[sp - 1] == g->nboff[v + 1]) {
            g->lim[v] = next++;
            g->post[np++] = v;
            sp--;
            continue;
        }
        uint32_t i = g->sit[sp - 1]++;
        uint32_t w = (uint32_t)(g->nbk[2 * i + 1] >> 32), t = (uint32_t)g->nbk[2 * i + 1];
        if (g->lim[w] != -1) continue;
        g->lim[w] = 0;
        g->low[w] = next;
        g->par[w] = (int32_t)v, g->pe[w] = (int32_t)t;
        g->stk[sp] = w, g->sit[sp] = g->nboff[w], sp++;
    }
}

static void cut_one(RG *g, uint32_t c) {
    int32_t p = g->par[c], ge = g->tge[g->pe[c]];
    bool tail = g->sv[ge] == (int32_t)c;
    int64_t cut = g->wt[ge];
    for (uint32_t k = g->eoff[c]; k < g->eoff[c + 1]; k++) {
        uint32_t e = g->eidx[k];
        bool out = g->sv[e] == (int32_t)c;
        int32_t o = out ? g->sw[e] : g->sv[e];
        if (o == p) continue;
        bool head = out == tail;
        cut += head ? g->wt[e] : -g->wt[e];
        if (g->par[o] == (int32_t)c) {
            int64_t oc = g->tcut[g->pe[o]];
            cut += head ? -oc : oc;
        }
    }
    g->tcut[g->pe[c]] = cut;
}

static void cutvalues(RG *g) {
    for (size_t i = 0; i + 1 < g->M; i++) cut_one(g, g->post[i]);
}

static void adj_add(RG *g, uint32_t v, uint32_t t) { g->adj[g->eoff[v] + g->acnt[v]++] = t; }
static void adj_del(RG *g, uint32_t v, uint32_t t) {
    uint32_t *a = g->adj + g->eoff[v], n = g->acnt[v];
    for (uint32_t i = 0; i < n; i++) if (a[i] == t) { a[i] = a[n - 1]; g->acnt[v] = n - 1; return; }
}
static inline bool is_anc(const RG *g, uint32_t x, uint32_t y) { return g->low[x] <= g->lim[y] && g->lim[y] <= g->lim[x]; }

/* Exchange tree edge e for graph edge f (tree index nf already appended). Only the subtree of the
 * LCA of f's endpoints changes: renumber low/lim inside its [low, lim] range, recompute par/pe and
 * ranks there, and recompute cut values on the cycle. Results equal dagre's full recomputation:
 * cut values depend only on the tree, subtree membership only on the fixed root, ranks on both. */
static void exchange(RG *g, uint32_t e, uint32_t nf) {
    uint32_t a = (uint32_t)g->ta[nf], b = (uint32_t)g->tb[nf];
    uint32_t L = a;
    while (!is_anc(g, L, b)) L = (uint32_t)g->par[L];
    /* cycle nodes (old tree paths to L): exactly the nodes whose subtree set changes */
    size_t np = 0;
    for (uint32_t x = a; x != L; x = (uint32_t)g->par[x]) g->path[np++] = x;
    for (uint32_t x = b; x != L; x = (uint32_t)g->par[x]) g->path[np++] = x;
    adj_del(g, (uint32_t)g->ta[e], e), adj_del(g, (uint32_t)g->tb[e], e);
    adj_add(g, a, nf), adj_add(g, b, nf);
    int64_t next = g->low[L];
    size_t sp = 0;
    int32_t skip = g->pe[L];
    g->stk[sp] = L, g->sit[sp] = 0, sp++;
    g->low[L] = next;
    while (sp) {
        uint32_t v = g->stk[sp - 1];
        if (g->sit[sp - 1] == g->acnt[v]) { g->lim[v] = next++; sp--; continue; }
        uint32_t t = g->adj[g->eoff[v] + g->sit[sp - 1]++];
        if ((int32_t)t == g->pe[v] && (v != L || (int32_t)t == skip)) continue;
        uint32_t w = (uint32_t)(g->ta[t] == (int32_t)v ? g->tb[t] : g->ta[t]);
        g->par[w] = (int32_t)v, g->pe[w] = (int32_t)t;
        int32_t ge = g->tge[t];
        g->r[w] = g->sv[ge] == (int32_t)w ? g->r[v] - g->ml[ge] : g->r[v] + g->ml[ge];
        g->low[w] = next;
        g->stk[sp] = w, g->sit[sp] = 0, sp++;
    }
    /* children before parents: ascending lim (postorder); disjoint */
    for (size_t i = 1; i < np; i++) {
        uint32_t x = g->path[i];
        size_t j = i;
        while (j > 0 && g->lim[g->path[j - 1]] > g->lim[x]) { g->path[j] = g->path[j - 1]; j--; }
        g->path[j] = x;
    }
    for (size_t i = 0; i < np; i++)
        cut_one(g, g->path[i]);
}

/* adds v to the tree; nout[x] counts x's incident edges whose other end is outside the tree */
static void ft_add(RG *g, uint32_t *nout, uint32_t v) {
    t_add_node(g, v);
    uint32_t n = 0;
    for (uint32_t k = g->eoff[v]; k < g->eoff[v + 1]; k++) {
        uint32_t e = g->eidx[k];
        uint32_t w = (uint32_t)(g->sv[e] == (int32_t)v ? g->sw[e] : g->sv[e]);
        if (g->tin[w]) nout[w]--; else n++;
    }
    nout[v] = n;
}

static bool feasible_tree(RG *g, uint32_t *snap) {
    size_t M = g->M;
    uint32_t *nout = g->acnt; /* free until the simplex loop */
    ft_add(g, nout, 0);
    for (;;) {
        size_t n0 = g->nint + g->nnon;
        t_nodes(g, snap);
        for (size_t i = 0; i < n0; i++) {
            if (!nout[snap[i]]) continue; /* every neighbour already in the tree: dfs adds nothing */
            size_t sp = 0;
            g->stk[sp] = snap[i], g->sit[sp] = g->eoff[snap[i]], sp++;
            while (sp) {
                uint32_t u = g->stk[sp - 1];
                if (g->sit[sp - 1] == g->eoff[u + 1]) { sp--; continue; }
                uint32_t e = g->eidx[g->sit[sp - 1]++];
                uint32_t w = (uint32_t)(g->sv[e] == (int32_t)u ? g->sw[e] : g->sv[e]);
                if (g->tin[w] || slack(g, e)) continue;
                ft_add(g, nout, w);
                if (!t_add_edge(g, (int32_t)u, (int32_t)w, (int32_t)e)) return false;
                g->stk[sp] = w, g->sit[sp] = g->eoff[w], sp++;
            }
        }
        if (g->nint + g->nnon >= M) return true;
        size_t f = SIZE_MAX;
        int64_t best = 0;
        for (size_t e = 0; e < g->SE; e++) {
            if (g->tin[g->sv[e]] == g->tin[g->sw[e]]) continue;
            int64_t s = slack(g, e);
            if (f == SIZE_MAX || s < best) f = e, best = s;
        }
        if (f == SIZE_MAX) return false; /* disconnected: cannot happen with the nesting root */
        int64_t delta = g->tin[g->sv[f]] ? best : -best;
        for (size_t v = 0; v < M; v++) if (g->tin[v]) g->r[v] += delta;
    }
}

static bool network_simplex_dagre(RG *g, size_t maxit) {
    size_t M = g->M;
    /* longestPath: rank(v) = min over out-edges (rank(w) - minlen), sinks 0 */
    {
        uint32_t *outdeg = g->sit, *q = g->stk;
        size_t qh = 0, qt = 0;
        memset(outdeg, 0, M * sizeof *outdeg);
        for (size_t e = 0; e < g->SE; e++) outdeg[g->sv[e]]++;
        for (size_t v = 0; v < M; v++) { g->r[v] = INT64_MAX; if (!outdeg[v]) q[qt++] = (uint32_t)v, g->r[v] = 0; }
        while (qh < qt) {
            uint32_t w = q[qh++];
            for (uint32_t k = g->eoff[w]; k < g->eoff[w + 1]; k++) {
                uint32_t e = g->eidx[k];
                if (g->sw[e] != (int32_t)w) continue;
                int32_t v = g->sv[e];
                if (g->r[w] - g->ml[e] < g->r[v]) g->r[v] = g->r[w] - g->ml[e];
                if (--outdeg[v] == 0) q[qt++] = (uint32_t)v;
            }
        }
        if (qt != M) return false; /* cycle */
    }
    if (!feasible_tree(g, g->post)) return false;
    lowlim(g);
    cutvalues(g);
    memset(g->acnt, 0, M * sizeof *g->acnt);
    for (size_t t = 0; t < g->nte; t++)
        if (g->talive[t]) adj_add(g, (uint32_t)g->ta[t], (uint32_t)t), adj_add(g, (uint32_t)g->tb[t], (uint32_t)t);
    for (size_t it = 0; it < maxit; it++) {
        size_t e = SIZE_MAX;
        for (size_t t = 0; t < g->nte; t++)
            if (g->talive[t] && g->tcut[t] < 0) { e = t; break; }
        if (e == SIZE_MAX) break;
        int32_t ge = g->tge[e];
        int32_t v = g->sv[ge], w = g->sw[ge]; /* g.hasEdge orientation */
        int32_t tail = v;
        bool flip = false;
        if (g->lim[v] > g->lim[w]) tail = w, flip = true;
        int64_t tl = g->low[tail], tm = g->lim[tail];
        size_t f = SIZE_MAX;
        int64_t best = 0;
        if (2 * (size_t)(tm - tl + 1) < M) {
            /* small tail subtree: only its incident edges can cross; same min (slack, edge index) */
            size_t sp = 0;
            g->stk[sp++] = (uint32_t)tail;
            while (sp) {
                uint32_t u = g->stk[--sp];
                for (uint32_t k = g->eoff[u]; k < g->eoff[u + 1]; k++) {
                    uint32_t x = g->eidx[k];
                    bool out = g->sv[x] == (int32_t)u;
                    int64_t lo = g->lim[out ? g->sw[x] : g->sv[x]];
                    if (tl <= lo && lo <= tm) continue;
                    if (out != flip) continue; /* into the subtree, or out of it when flipped */
                    int64_t s = slack(g, x);
                    if (f == SIZE_MAX || s < best || (s == best && x < f)) f = x, best = s;
                }
                for (uint32_t i = 0; i < g->acnt[u]; i++) {
                    uint32_t t = g->adj[g->eoff[u] + i];
                    if ((int32_t)t == g->pe[u]) continue;
                    g->stk[sp++] = (uint32_t)(g->ta[t] == (int32_t)u ? g->tb[t] : g->ta[t]);
                }
            }
        } else
            for (size_t x = 0; x < g->SE; x++) {
                int64_t lv = g->lim[g->sv[x]], lw = g->lim[g->sw[x]];
                bool dv = tl <= lv && lv <= tm, dw = tl <= lw && lw <= tm;
                if (flip != dv || flip == dw) continue;
                int64_t s = slack(g, x);
                if (f == SIZE_MAX || s < best) f = x, best = s;
            }
        if (f == SIZE_MAX) break;
        g->talive[e] = 0;
        if (!t_add_edge(g, g->sv[f], g->sw[f], (int32_t)f)) break;
        exchange(g, (uint32_t)e, (uint32_t)(g->nte - 1));
    }
    return true;
}

typedef struct NB { /* nesting-graph builder */
    size_t M, E, cap;
    int32_t *u, *v, *wt, *ml;
} NB;

static void nb_edge(NB *b, int32_t u, int32_t v, int32_t wt, int32_t ml) {
    if (b->E >= b->cap) return;
    b->u[b->E] = u, b->v[b->E] = v, b->wt[b->E] = wt, b->ml[b->E] = ml, b->E++;
}

typedef struct Ctx {
    const DRankIn *in;
    NB *b;
    int32_t *rn;       /* item -> rank-graph node (reals) */
    int32_t *bt, *bb;  /* cluster -> border nodes */
    int32_t *depth;    /* [N+S] */
    int32_t *corder;
    size_t nco;
    int32_t root, nodesep, weight, height;
} Ctx;

static void tree_depths(Ctx *c, uint32_t x, int32_t d) {
    const DRankIn *in = c->in;
    if (x >= in->N) {
        uint32_t s = (uint32_t)(x - in->N);
        for (uint32_t k = in->koff[s]; k < in->koff[s + 1]; k++) tree_depths(c, in->kid[k], d + 1);
    }
    c->depth[x] = d;
}

static void nest_dfs(Ctx *c, uint32_t x) {
    const DRankIn *in = c->in;
    NB *b = c->b;
    bool cl = x >= in->N && in->koff[x - in->N + 1] > in->koff[x - in->N];
    if (!cl) {
        if (x < in->N) nb_edge(b, c->root, c->rn[x], 0, c->nodesep);
        return;
    }
    uint32_t s = (uint32_t)(x - in->N);
    int32_t top = (int32_t)b->M++, bot = (int32_t)b->M++;
    c->bt[s] = top, c->bb[s] = bot;
    c->corder[c->nco++] = (int32_t)s;
    for (uint32_t k = in->koff[s]; k < in->koff[s + 1]; k++) {
        uint32_t ch = in->kid[k];
        nest_dfs(c, ch);
        bool chc = ch >= in->N && c->bt[ch - in->N] >= 0;
        int32_t ct = chc ? c->bt[ch - in->N] : c->rn[ch], cb = chc ? c->bb[ch - in->N] : c->rn[ch];
        if (!chc && ch >= in->N) continue; /* empty sub-cluster: not ranked */
        int32_t w = chc ? c->weight : 2 * c->weight;
        int32_t ml = ct != cb ? 1 : c->height - c->depth[x] + 1;
        nb_edge(b, top, ct, w, ml);
        nb_edge(b, cb, bot, w, ml);
    }
    /* root-level clusters are listed under parent S */
    bool toplevel = false;
    for (uint32_t k = in->koff[in->S]; k < in->koff[in->S + 1]; k++) if (in->kid[k] == x) { toplevel = true; break; }
    if (toplevel) nb_edge(b, c->root, top, 0, c->height + c->depth[x]);
}

typedef struct HE { uint64_t k; int32_t e; } HE;

bool dagre_rank(Arena *A, const DRankIn *in, int32_t rank[], int32_t top[], int32_t bot[], int32_t corder[]) {
    size_t N = in->N, S = in->S, NR = in->nreal;
    size_t M = NR + 1 + 2 * S;
    size_t kn = in->koff[S + 1];
    size_t ecap = in->E + 3 * (kn + S) + NR + 4;
    NB b = {.M = NR + 1, .cap = ecap, .u = ANEW(A, int32_t, ecap), .v = ANEW(A, int32_t, ecap),
            .wt = ANEW(A, int32_t, ecap), .ml = ANEW(A, int32_t, ecap)};
    Ctx c = {.in = in, .b = &b, .rn = ANEW(A, int32_t, N + 1), .bt = ANEW(A, int32_t, S + 1),
             .bb = ANEW(A, int32_t, S + 1), .depth = ANEW(A, int32_t, N + S + 1), .root = (int32_t)NR, .corder = corder};
    if (!b.u || !b.v || !b.wt || !b.ml || !c.rn || !c.bt || !c.bb || !c.depth) return false;
    for (size_t v = 0; v < N; v++) c.rn[v] = -1;
    for (size_t i = 0; i < NR; i++) c.rn[in->real[i]] = (int32_t)i;
    for (size_t s = 0; s < S; s++) c.bt[s] = c.bb[s] = -1, corder[s] = -1;
    int32_t maxd = 1;
    for (size_t i = 0; i < N + S; i++) c.depth[i] = 0;
    for (uint32_t k = in->koff[S]; k < in->koff[S + 1]; k++) tree_depths(&c, in->kid[k], 1);
    for (size_t i = 0; i < N + S; i++) if (c.depth[i] > maxd) maxd = c.depth[i];
    c.height = maxd - 1;
    c.nodesep = 2 * c.height + 1;
    c.weight = 1;
    for (size_t e = 0; e < in->E; e++) {
        nb_edge(&b, c.rn[in->eu[e]], c.rn[in->ev[e]], 1, in->minlen[e] * c.nodesep);
        c.weight++;
    }
    for (uint32_t k = in->koff[S]; k < in->koff[S + 1]; k++) nest_dfs(&c, in->kid[k]);
    M = b.M;
    /* names in JS string order: reals by id, generated _root/_bt/_bb with equal-width counters */
    RG g = {.A = A, .M = M};
    int64_t *key = ANEW(A, int64_t, M + 1);
    uint32_t *sord = ANEW(A, uint32_t, M + 1), *ordv = ANEW(A, uint32_t, M + 1);
    Str *nm = ANEW(A, Str, M + 1);
    char *gen = ANEW(A, char, 16 * (M - NR) + 1);
    if (!key || !sord || !ordv || !nm || !gen) return false;
    for (size_t i = 0; i < M; i++) key[i] = -1;
    for (size_t i = 0; i < NR; i++) key[i] = in->ikey[in->real[i]], nm[i] = in->id[in->real[i]];
    for (size_t i = NR; i < M; i++) {
        char *p = gen + 16 * (i - NR);
        const char *pre = i == NR ? "_root" : ((i - NR - 1) % 2 ? "_bb" : "_bt");
        int n = snprintf(p, 16, "%s%u", pre, (unsigned)(1000000 + i));
        nm[i] = (Str){p, (size_t)n};
    }
    for (size_t i = 0; i < M; i++) ordv[i] = (uint32_t)i;
    /* insertion sort is fine for small M; use a simple merge via qsort on index with static ctx */
    for (size_t i = 1; i < M; i++) {
        uint32_t x = ordv[i];
        size_t j = i;
        while (j > 0) {
            Str a = nm[ordv[j - 1]], bs = nm[x];
            size_t n = a.n < bs.n ? a.n : bs.n;
            int r = memcmp(a.p, bs.p, n);
            if (r < 0 || (r == 0 && a.n <= bs.n)) break;
            ordv[j] = ordv[j - 1];
            j--;
        }
        ordv[j] = x;
    }
    for (size_t i = 0; i < M; i++) sord[ordv[i]] = (uint32_t)i;
    g.key = key, g.sord = sord;
    /* simplify: merge multi-edges (weight sum, minlen max), first-occurrence order */
    size_t hcap = 16;
    while (hcap < 2 * b.E + 2) hcap <<= 1;
    HE *ht = ANEW(A, HE, hcap);
    g.sv = ANEW(A, int32_t, b.E + 1), g.sw = ANEW(A, int32_t, b.E + 1);
    g.wt = ANEW(A, int32_t, b.E + 1), g.ml = ANEW(A, int32_t, b.E + 1);
    if (!ht || !g.sv || !g.sw || !g.wt || !g.ml) return false;
    for (size_t i = 0; i < hcap; i++) ht[i].e = -1;
    for (size_t e = 0; e < b.E; e++) {
        uint64_t k = (uint64_t)(uint32_t)b.u[e] << 32 | (uint32_t)b.v[e];
        size_t h = (size_t)((k * 0x9E3779B97F4A7C15ull) >> 20) & (hcap - 1);
        while (ht[h].e >= 0 && ht[h].k != k) h = (h + 1) & (hcap - 1);
        if (ht[h].e < 0) {
            ht[h].k = k, ht[h].e = (int32_t)g.SE;
            g.sv[g.SE] = b.u[e], g.sw[g.SE] = b.v[e], g.wt[g.SE] = 0, g.ml[g.SE] = 1, g.SE++;
        }
        int32_t s = ht[h].e;
        g.wt[s] += b.wt[e];
        if (b.ml[e] > g.ml[s]) g.ml[s] = b.ml[e];
    }
    size_t SE = g.SE;
    g.eoff = ANEW(A, uint32_t, M + 2), g.eidx = ANEW(A, uint32_t, 2 * SE + 1);
    uint32_t *fill = ANEW(A, uint32_t, M + 1);
    if (!g.eoff || !g.eidx || !fill) return false;
    memset(g.eoff, 0, (M + 2) * sizeof *g.eoff);
    for (size_t e = 0; e < SE; e++) g.eoff[g.sv[e] + 1]++, g.eoff[g.sw[e] + 1]++;
    for (size_t v = 0; v < M; v++) g.eoff[v + 1] += g.eoff[v];
    { /* in-edges first, then out-edges */
        uint32_t *indeg = fill;
        memset(indeg, 0, M * sizeof *indeg);
        for (size_t e = 0; e < SE; e++) indeg[g.sw[e]]++;
        uint32_t *ip = ANEW(A, uint32_t, M + 1), *op = ANEW(A, uint32_t, M + 1);
        if (!ip || !op) return false;
        for (size_t v = 0; v < M; v++) ip[v] = g.eoff[v], op[v] = g.eoff[v] + indeg[v];
        for (size_t e = 0; e < SE; e++) g.eidx[ip[g.sw[e]]++] = (uint32_t)e, g.eidx[op[g.sv[e]]++] = (uint32_t)e;
    }
    size_t maxit = 20 * M + 200;
    g.maxte = M + maxit + 1;
    g.r = ANEW(A, int64_t, M + 1);
    g.tin = ANEW(A, uint8_t, M + 1);
    g.tord = ANEW(A, uint32_t, M + 1), g.tnon = ANEW(A, uint32_t, M + 1);
    g.ta = ANEW(A, int32_t, g.maxte), g.tb = ANEW(A, int32_t, g.maxte), g.tge = ANEW(A, int32_t, g.maxte);
    g.talive = ANEW(A, uint8_t, g.maxte);
    g.tcut = ANEW(A, int64_t, g.maxte);
    g.low = ANEW(A, int64_t, M + 1), g.lim = ANEW(A, int64_t, M + 1);
    g.par = ANEW(A, int32_t, M + 1), g.pe = ANEW(A, int32_t, M + 1);
    g.post = ANEW(A, uint32_t, M + 1), g.stk = ANEW(A, uint32_t, M + 1), g.sit = ANEW(A, uint32_t, M + 1);
    g.nboff = ANEW(A, uint32_t, M + 2);
    g.nbk = ANEW(A, uint64_t, 4 * M + 4);
    g.acnt = ANEW(A, uint32_t, M + 1), g.adj = ANEW(A, uint32_t, 2 * SE + 1), g.path = ANEW(A, uint32_t, 2 * M + 2);
    if (!g.r || !g.tin || !g.tord || !g.tnon || !g.ta || !g.tb || !g.tge || !g.talive || !g.tcut || !g.low ||
        !g.lim || !g.par || !g.pe || !g.post || !g.stk || !g.sit || !g.nboff || !g.nbk || !g.acnt || !g.adj || !g.path)
        return false;
    memset(g.tin, 0, M);
    if (!network_simplex_dagre(&g, maxit)) return false;
    /* injectEdgeLabelProxies + removeEmptyRanks (nodeRankFactor = nodeSep) */
    int64_t mn = INT64_MAX, mx = INT64_MIN;
    for (size_t v = 0; v < M; v++) { if (g.r[v] < mn) mn = g.r[v]; if (g.r[v] > mx) mx = g.r[v]; }
    size_t nl = (size_t)(mx - mn) + 1;
    uint8_t *occ = ANEW(A, uint8_t, nl + 1);
    int64_t *dl = ANEW(A, int64_t, nl + 1);
    if (!occ || !dl) return false;
    memset(occ, 0, nl);
    for (size_t v = 0; v < M; v++) occ[g.r[v] - mn] = 1;
    for (size_t e = 0; e < in->E; e++) {
        if (!in->lab[e]) continue;
        int64_t ru = g.r[c.rn[in->eu[e]]], rw = g.r[c.rn[in->ev[e]]];
        if ((rw - ru) % 2 == 0) occ[(rw - ru) / 2 + ru - mn] = 1;
    }
    int64_t delta = 0;
    for (size_t i = 0; i < nl; i++) {
        if (!occ[i] && (int64_t)i % c.nodesep != 0) --delta;
        dl[i] = delta;
    }
    if (in->lrank)
        for (size_t e = 0; e < in->E; e++) {
            int64_t ru = g.r[c.rn[in->eu[e]]], rw = g.r[c.rn[in->ev[e]]];
            /* a fractional proxy rank never matches a dummy rank in normalize.run: the label gets no node */
            in->lrank[e] = in->lab[e] && (rw - ru) % 2 == 0 ? (int32_t)((rw - ru) / 2 + ru + dl[(rw - ru) / 2 + ru - mn]) : INT32_MIN;
        }
    for (size_t v = 0; v < M; v++) g.r[v] += dl[g.r[v] - mn];
    /* cleanup (drop root) + normalizeRanks */
    int64_t m2 = INT64_MAX;
    for (size_t v = 0; v < M; v++) if (v != NR && g.r[v] < m2) m2 = g.r[v];
    if (in->lrank)
        for (size_t e = 0; e < in->E; e++) in->lrank[e] = in->lrank[e] == INT32_MIN ? -1 : (int32_t)(in->lrank[e] - m2);
    for (size_t i = 0; i < NR; i++) rank[in->real[i]] = (int32_t)(g.r[i] - m2);
    for (size_t s = 0; s < S; s++) {
        top[s] = c.bt[s] >= 0 ? (int32_t)(g.r[c.bt[s]] - m2) : INT32_MAX;
        bot[s] = c.bb[s] >= 0 ? (int32_t)(g.r[c.bb[s]] - m2) : -1;
    }
    return true;
}
