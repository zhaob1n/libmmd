/* Sugiyama layered layout:
 *   1. DFS cycle removal            2. longest-path ranking + greedy tightening
 *   3. long-edge normalisation      4. barycenter sweeps, cluster-contiguous, Fenwick crossing count
 *   5. Brandes-Köpf coordinate assignment (4 alignments + balancing)
 *   6. rank-axis placement, direction transform, edge polyline + clipping, cluster boxes */
#include <math.h>
#ifdef CMM_PROFILE
#include <stdio.h>
#include <time.h>

static double prof_t0;
static double prof_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6; }
#define PROF(name) do { double t_ = prof_now(); if (*name) fprintf(stderr, "  %-10s %8.3f ms\n", name, t_ - prof_t0); prof_t0 = t_; } while (0)
#else
#define PROF(name) do { } while (0)
#endif

#include <stdlib.h>

#include "graph.h"
#include "nsimplex.h"
#include "rich.h"
#include "text.h"

/* inline min/max: fmin/fmax are libm calls at -O2 (NaN semantics) */
static inline double dmin(double a, double b) { return a < b ? a : b; }
static inline double dmax(double a, double b) { return a > b ? a : b; }

constexpr double PAD_NODE = 15.0;
constexpr double CLUSTER_PAD = 15.0;
constexpr double CLUSTER_TITLE = 24.0;
constexpr double EDGE_SEP = 20.0;
constexpr unsigned MAX_KEY_DEPTH = 8;
static_assert(sizeof(double) * MAX_KEY_DEPTH >= 2 * 16, "Key buffer reused for BcItem pairs");

/* measure a label; markdown labels are word-wrapped in place first */
static void measure(Graph *g, Str *lab, double *w, unsigned *lines) {
    double fs = g->font_size;
    if (lab->n && rich_needed(*lab)) {
        bool ok;
        Str wr = rich_wrap(g->arena, *lab, fs, RICH_WRAP_PX * fs / 16, &ok);
        if (ok) *lab = wr;
        rich_measure(*lab, fs, w, lines);
        return;
    }
    label_measure(*lab, fs, w, lines);
    double maxw = RICH_WRAP_PX * fs / 16;
    if (*w <= maxw) return;
    /* mermaid htmlLabels wrap plain labels at wrappingWidth too (greedy, at spaces) */
    char *o = arena_alloc(g->arena, lab->n + 1, 1);
    if (!o) return;
    memcpy(o, lab->p, lab->n);
    o[lab->n] = 0;
    size_t ls = 0, lastsp = SIZE_MAX, n = lab->n;
    for (size_t i = 0; i <= n; i++) {
        if (i < n && o[i] != ' ' && o[i] != '\n') continue;
        if (lastsp != SIZE_MAX && text_width((Str){.p = o + ls, .n = i - ls}, fs) > maxw) {
            o[lastsp] = '\n';
            ls = lastsp + 1;
        }
        if (i == n) break;
        if (o[i] == '\n') ls = i + 1, lastsp = SIZE_MAX;
        else lastsp = i;
    }
    *lab = (Str){.p = o, .n = n};
    label_measure(*lab, fs, w, lines);
}

void node_size(Graph *g, size_t v) {
    if (g->nflags[v] & NF_FIXED) return;
    double fs = g->font_size, tw;
    unsigned lines;
    measure(g, &g->nlabel[v], &tw, &lines);
    if (lines == 0) lines = 1;
    double th = lines * fs * 1.5;
    double w = tw + 2 * PAD_NODE, h = th + 2 * PAD_NODE;
    switch (g->nshape[v]) {
    case SH_RECT: case SH_ROUND: break;
    case SH_STADIUM: w = tw + h; break;
    case SH_SUBROUTINE: w += 16; break;
    case SH_CYLINDER: {
        double rx = w / 2, ry = rx / (2.5 + w / 50);
        h = th + 2 * PAD_NODE + 2 * ry;
        break;
    }
    case SH_CIRCLE: w = h = dmax(tw, th) + 2 * PAD_NODE; break;
    case SH_DCIRCLE: w = h = dmax(tw, th) + 2 * PAD_NODE + 10; break;
    case SH_ASYM: w = tw + 2 * PAD_NODE + h / 4; break;
    case SH_RHOMBUS: { double s = (tw + 16) + (th + 16); w = h = s; break; }
    case SH_HEXAGON: w = tw + 2 * PAD_NODE + h / 2; break;
    case SH_LEAN_R: case SH_LEAN_L: case SH_TRAP: case SH_TRAP_ALT: w = tw + 2 * PAD_NODE + h * 2 / 3; break;
    default: break;
    }
    g->nw[v] = w;
    g->nh[v] = h;
}

typedef struct Lay {
    Graph *g;
    Arena *A;
    size_t N, V, R;
    int32_t *rank, *cl, *pos, *lay;
    uint32_t *loff;
    uint8_t *dummy, *border;
    double *w, *h, *x, *bc;
    uint32_t *poff, *pidx, *soff, *sidx;
    int32_t *sdepth;
    double *lsum, *lval; /* per-cluster scratch for compound ordering (S+1) */
    uint32_t *lcnt;
    int32_t *plist;
    size_t S;
    struct BcItem *items;
    uint32_t *bstart, *bcur, *bstk;
    double nodesep;
    double cpad;
    double *rpad; /* extra right-side room (self-loops + their labels), nullptr if none */
} Lay;

/* ---------- sorting helpers ---------- */
typedef struct Key {
    double bc[MAX_KEY_DEPTH];
    int32_t id[MAX_KEY_DEPTH];
    uint32_t len;
} Key;

static void sort_u32_small(size_t n, uint32_t a[static n]) {
    for (size_t i = 1; i < n; i++) {
        uint32_t t = a[i];
        size_t j = i;
        while (j && a[j - 1] > t) { a[j] = a[j - 1]; j--; }
        a[j] = t;
    }
}

/* ---------- crossing counting (Barth/Jünger/Mutzel accumulator via Fenwick) ---------- */
static uint64_t cross_layer(const Lay *L, size_t r, uint32_t *buf, uint32_t *fen) {
    size_t a0 = L->loff[r], a1 = L->loff[r + 1], b0 = L->loff[r + 1], b1 = L->loff[r + 2];
    size_t m = 0, nb = b1 - b0;
    for (size_t i = a0; i < a1; i++) {
        int32_t u = L->lay[i];
        size_t s = m;
        for (uint32_t k = L->soff[u]; k < L->soff[u + 1]; k++) buf[m++] = (uint32_t)L->pos[L->sidx[k]];
        if (m > s) sort_u32_small(m - s, buf + s);
    }
    memset(fen, 0, (nb + 1) * sizeof *fen);
    uint64_t cr = 0;
    for (size_t i = 0; i < m; i++) {
        uint32_t p = buf[i] + 1; /* count already-inserted with pos > p */
        uint64_t le = 0;
        for (uint32_t q = p; q; q -= q & -q) le += fen[q];
        cr += i - le;
        for (uint32_t q = p; q <= nb; q += q & -q) fen[q]++;
    }
    return cr;
}

static uint64_t cross_all(const Lay *L, uint32_t *buf, uint32_t *fen) {
    uint64_t c = 0;
    for (size_t r = 0; r + 1 < L->R; r++) c += cross_layer(L, r, buf, fen);
    return c;
}

typedef struct BcItem { double bc; int32_t id; uint32_t i; } BcItem;
static inline bool bc_lt(BcItem a, BcItem b) { return a.bc < b.bc || (a.bc == b.bc && a.id < b.id); }
static void bc_sort(size_t n, BcItem a[static n], BcItem tmp[static n]) {
    if (n <= 24) {
        for (size_t i = 1; i < n; i++) {
            BcItem t = a[i];
            size_t j = i;
            while (j && bc_lt(t, a[j - 1])) { a[j] = a[j - 1]; j--; }
            a[j] = t;
        }
        return;
    }
    size_t h = n / 2;
    bc_sort(h, a, tmp);
    bc_sort(n - h, a + h, tmp);
    if (!bc_lt(a[h], a[h - 1])) return; /* already ordered: common after convergence */
    memcpy(tmp, a, h * sizeof *a);
    size_t i = 0, j = h, k = 0;
    while (i < h && j < n) a[k++] = bc_lt(a[j], tmp[i]) ? a[j++] : tmp[i++];
    while (i < h) a[k++] = tmp[i++];
}

/* one key per cluster, shared by all layers (mean normalised slot of its members), so sibling clusters keep
 * the same relative order in every layer -> cluster boxes can be made rectangular without overlap */
static void cluster_keys(const Lay *L, size_t S, double gk[static S + 1], uint32_t cnt[static S + 1]) {
    memset(gk, 0, (S + 1) * sizeof *gk);
    memset(cnt, 0, (S + 1) * sizeof *cnt);
    for (size_t r = 0; r < L->R; r++) {
        size_t a = L->loff[r], n = L->loff[r + 1] - a;
        for (size_t i = 0; i < n; i++) {
            int32_t v = L->lay[a + i];
            double t = ((double)i + 0.5) / (double)n;
            for (int32_t c = L->cl[v]; c != NONE; c = L->g->sparent[c]) gk[c] += t, cnt[c]++;
        }
    }
    for (size_t c = 0; c < S; c++) if (cnt[c]) gk[c] /= cnt[c];
}

/* ---------- one barycenter sweep over layer r ---------- */
static void sweep_layer(Lay *L, size_t r, bool down, bool self, Key *keys, uint32_t *idx, uint32_t *tmp,
                        double *csum, uint32_t *ccnt, bool clusters) {
    size_t a = L->loff[r], b = L->loff[r + 1], n = b - a;
    if (n < 2) return;
    const uint32_t *off = down ? L->poff : L->soff, *adj = down ? L->pidx : L->sidx;
    for (size_t i = 0; i < n; i++) {
        int32_t v = L->lay[a + i];
        uint32_t k0 = off[v], k1 = off[v + 1];
        double bcv;
        if (k1 > k0 && !self) {
            double s = 0;
            for (uint32_t k = k0; k < k1; k++) s += L->pos[adj[k]];
            bcv = s / (k1 - k0);
        } else {
            bcv = (double)i * 1.0; /* no neighbours: keep current slot */
        }
        if (L->border[v]) bcv = L->border[v] == 1 ? -1e18 : 1e18;
        L->bc[v] = bcv;
    }
    if (!clusters) {
        BcItem *it = (BcItem *)keys, *tb = it + n; /* keys buffer is large enough (sizeof Key >= 2*sizeof BcItem) */
        for (size_t i = 0; i < n; i++) {
            int32_t v = L->lay[a + i];
            it[i] = (BcItem){.bc = L->bc[v], .id = v, .i = (uint32_t)i};
        }
        bc_sort(n, it, tb);
        for (size_t i = 0; i < n; i++) {
            L->lay[a + i] = it[i].id;
            L->pos[it[i].id] = (int32_t)i;
        }
        (void)idx; (void)tmp;
        return;
    }
    (void)ccnt; /* csum holds layer-independent cluster keys (see cluster_keys) */
    /* compound ordering: a cluster is placed among its siblings by the local barycenter of its members
     * (so long edges pass on the near side), made monotone in the global cluster order by pool-adjacent-
     * violators so siblings never swap between layers (keeps boxes disjoint). */
    size_t np = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t v = L->lay[a + i];
        for (int32_t c = L->cl[v]; c != NONE; c = L->g->sparent[c]) {
            if (!(L->lcnt[c] & 0x80000000u)) { L->plist[np++] = c; L->lcnt[c] = 0x80000000u; }
            if (!L->border[v]) L->lsum[c] += L->bc[v], L->lcnt[c]++;
        }
    }
    for (size_t i = 0; i < np; i++) { /* mark listed */
        int32_t c = L->plist[i];
        uint32_t cn = L->lcnt[c] & 0x7fffffffu;
        L->lval[c] = cn ? L->lsum[c] / cn : csum[c] * (double)n;
    }
    /* sort listed clusters by (parent, global key) */
    for (size_t i = 1; i < np; i++) {
        int32_t t = L->plist[i];
        size_t j = i;
        while (j) {
            int32_t u = L->plist[j - 1];
            int32_t pu = L->g->sparent[u], pt = L->g->sparent[t];
            if (pu < pt || (pu == pt && (csum[u] < csum[t] || (csum[u] == csum[t] && u < t)))) break;
            L->plist[j] = u; j--;
        }
        L->plist[j] = t;
    }
    for (size_t g0 = 0; g0 < np;) { /* PAV per sibling group */
        size_t g1 = g0 + 1;
        while (g1 < np && L->g->sparent[L->plist[g1]] == L->g->sparent[L->plist[g0]]) g1++;
        /* blocks stored in-place: lsum reused as block value, lcnt as block weight */
        size_t nb = 0;
        int32_t *pl = L->plist + g0;
        double bv[64]; double bw[64]; size_t be[64];
        for (size_t i = 0; i < g1 - g0; i++) {
            if (nb == countof(bv)) break;
            bv[nb] = L->lval[pl[i]], bw[nb] = 1, be[nb] = i + 1, nb++;
            while (nb > 1 && bv[nb - 2] >= bv[nb - 1]) {
                bv[nb - 2] = (bv[nb - 2] * bw[nb - 2] + bv[nb - 1] * bw[nb - 1]) / (bw[nb - 2] + bw[nb - 1]);
                bw[nb - 2] += bw[nb - 1], be[nb - 2] = be[nb - 1], nb--;
            }
        }
        size_t s0 = 0;
        for (size_t b2 = 0; b2 < nb; b2++) {
            for (size_t i = s0; i < be[b2]; i++) L->lval[pl[i]] = bv[b2] + 1e-6 * (double)(i - s0);
            s0 = be[b2];
        }
        g0 = g1;
    }
    for (size_t i = 0; i < np; i++) { int32_t c = L->plist[i]; L->lsum[c] = 0, L->lcnt[c] = 0; }
    /* hierarchical sort: bucket items (nodes + present clusters) by parent, sort buckets, emit depth-first */
    size_t S = L->S, m = n + np;
    BcItem *it = L->items, *tb = L->items + m;
    uint32_t *bs = L->bstart; /* bucket starts, S+2 entries; bucket S = root */
    memset(bs, 0, (S + 2) * sizeof *bs);
    for (size_t i = 0; i < n; i++) { int32_t c = L->cl[L->lay[a + i]]; bs[(c == NONE ? S : (size_t)c) + 1]++; }
    for (size_t i = 0; i < np; i++) { int32_t c = L->g->sparent[L->plist[i]]; bs[(c == NONE ? S : (size_t)c) + 1]++; }
    for (size_t c = 0; c <= S; c++) bs[c + 1] += bs[c];
    uint32_t *cur = L->bcur;
    memcpy(cur, bs, (S + 1) * sizeof *cur);
    for (size_t i = 0; i < n; i++) {
        int32_t v = L->lay[a + i], c = L->cl[v];
        it[cur[c == NONE ? S : (size_t)c]++] = (BcItem){.bc = L->bc[v], .id = v};
    }
    for (size_t i = 0; i < np; i++) {
        int32_t c0 = L->plist[i], c = L->g->sparent[c0];
        it[cur[c == NONE ? S : (size_t)c]++] = (BcItem){.bc = L->lval[c0], .id = -(c0 + 1)};
    }
    for (size_t c = 0; c <= S; c++) if (bs[c + 1] - bs[c] > 1) bc_sort(bs[c + 1] - bs[c], it + bs[c], tb);
    /* iterative DFS from the root bucket */
    uint32_t *stk = L->bstk;
    size_t sp = 0, o = 0;
    cur[S] = bs[S];
    stk[sp++] = (uint32_t)S;
    while (sp) {
        uint32_t c = stk[sp - 1];
        if (cur[c] == bs[c + 1]) { sp--; continue; }
        BcItem x = it[cur[c]++];
        if (x.id >= 0) { L->lay[a + o] = x.id; L->pos[x.id] = (int32_t)o; o++; }
        else { uint32_t cc = (uint32_t)(-x.id - 1); cur[cc] = bs[cc]; stk[sp++] = cc; }
    }
    (void)keys; (void)idx; (void)tmp;
}

/* ---------- transpose: adjacent swaps that strictly reduce crossings ---------- */

/* returns crossings removed; *budget bounds the pair-cost work (early stop on huge graphs) */
/* both orders at once: *cuv = crossings with u left of v, *cvu = with v left of u */
static void pair_cost2(const Lay *L, int32_t u, int32_t v, uint64_t *cuv, uint64_t *cvu) {
    uint64_t a = 0, b = 0;
    for (unsigned side = 0; side < 2; side++) {
        const uint32_t *off = side ? L->soff : L->poff, *adj = side ? L->sidx : L->pidx;
        for (uint32_t i = off[u]; i < off[u + 1]; i++) {
            int32_t pa = L->pos[adj[i]];
            for (uint32_t j = off[v]; j < off[v + 1]; j++) {
                int32_t pb = L->pos[adj[j]];
                a += pa > pb, b += pb > pa;
            }
        }
    }
    *cuv = a, *cvu = b;
}
static uint64_t transpose(Lay *L, int64_t *budget, uint8_t dirty[static L->R + 2]) {
    /* dirty[r+1]: layer r changed in the last pass (dirty[0], dirty[R+1] are sentinels) */
    uint64_t gain = 0;
    memset(dirty, 1, L->R + 2);
    dirty[0] = dirty[L->R + 1] = 0;
    for (unsigned pass = 0; pass < 6; pass++) {
        bool improved = false;
        for (size_t r = 0; r < L->R; r++) {
            if (!(dirty[r] | dirty[r + 1] | dirty[r + 2])) continue; /* neighbourhood unchanged: same verdicts */
            bool ch = false;
            for (size_t i = L->loff[r]; i + 1 < L->loff[r + 1]; i++) {
                int32_t u = L->lay[i], v = L->lay[i + 1];
                if (L->border[u] || L->border[v] || L->cl[u] != L->cl[v]) continue;
                uint32_t du = (L->poff[u + 1] - L->poff[u]) + (L->soff[u + 1] - L->soff[u]);
                uint32_t dv = (L->poff[v + 1] - L->poff[v]) + (L->soff[v + 1] - L->soff[v]);
                if (!du || !dv) continue; /* isolated in both directions: swap never matters */
                *budget -= (int64_t)du * dv + 1;
                if (*budget < 0) return gain;
                uint64_t cuv, cvu;
                pair_cost2(L, u, v, &cuv, &cvu);
                if (cvu < cuv) {
                    L->lay[i] = v, L->lay[i + 1] = u;
                    L->pos[v]--, L->pos[u]++;
                    gain += cuv - cvu;
                    ch = true;
                }
            }
            dirty[r + 1] = (uint8_t)(ch ? 2 : (dirty[r + 1] == 2 ? 1 : 0));
            improved |= ch;
        }
        if (!improved) break;
        /* only layers changed this pass (value 2) stay dirty */
        for (size_t r = 1; r <= L->R; r++) dirty[r] = dirty[r] == 2;
    }
    return gain;
}

/* ---------- Brandes-Köpf ---------- */
typedef struct PairSet { uint64_t *t; size_t cap, n; uint64_t bloom[64]; /* 4096-bit prefilter for pset_has */ } PairSet;
static inline uint64_t pkey(int32_t a, int32_t b) {
    if (a > b) { int32_t t = a; a = b; b = t; }
    return ((uint64_t)(uint32_t)a << 32 | (uint32_t)b) + 1;
}
static inline size_t phash(uint64_t k, size_t cap) { return (size_t)((k * 0x9E3779B97F4A7C15ull) >> 17) & (cap - 1); }
[[nodiscard]] static bool pset_add(Arena *A, PairSet *s, int32_t a, int32_t b) {
    { uint64_t bk = pkey(a, b) * 0x9E3779B97F4A7C15ull >> 52; s->bloom[bk >> 6] |= 1ull << (bk & 63); }
    if ((s->n + 1) * 2 > s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 64;
        uint64_t *nt = ANEW(A, uint64_t, nc);
        if (!nt) return false;
        memset(nt, 0, nc * sizeof *nt);
        for (size_t i = 0; i < s->cap; i++)
            if (s->t[i]) {
                size_t h = phash(s->t[i], nc);
                while (nt[h]) h = (h + 1) & (nc - 1);
                nt[h] = s->t[i];
            }
        s->t = nt;
        s->cap = nc;
    }
    uint64_t k = pkey(a, b);
    size_t h = phash(k, s->cap);
    while (s->t[h]) {
        if (s->t[h] == k) return true;
        h = (h + 1) & (s->cap - 1);
    }
    s->t[h] = k;
    s->n++;
    return true;
}
static inline bool pset_has(const PairSet *s, int32_t a, int32_t b) {
    if (!s->cap) return false;
    uint64_t k = pkey(a, b), bk = k * 0x9E3779B97F4A7C15ull >> 52;
    if (!(s->bloom[bk >> 6] >> (bk & 63) & 1)) return false;
    size_t h = phash(k, s->cap);
    while (s->t[h]) {
        if (s->t[h] == k) return true;
        h = (h + 1) & (s->cap - 1);
    }
    return false;
}

static int32_t cluster_lca(const Graph *g, const int32_t *depth, int32_t a, int32_t b) {
    while (a != b) {
        int32_t da = a == NONE ? 0 : depth[a], db = b == NONE ? 0 : depth[b];
        if (da >= db) a = a == NONE ? NONE : g->sparent[a];
        else b = g->sparent[b];
        if (a == NONE && b == NONE) return NONE;
    }
    return a;
}

static double sep(const Lay *L, int32_t u, int32_t v) {
    double s = (L->w[u] + L->w[v]) / 2;
    if (L->rpad) s += L->rpad[u];
    s += (L->dummy[u] ? EDGE_SEP : L->nodesep) / 2 + (L->dummy[v] ? EDGE_SEP : L->nodesep) / 2;
    int32_t cu = L->cl[u], cv = L->cl[v];
    if (cu != cv) {
        int32_t l = cluster_lca(L->g, L->sdepth, cu, cv);
        int32_t dl = l == NONE ? 0 : L->sdepth[l];
        int32_t du = cu == NONE ? 0 : L->sdepth[cu], dv = cv == NONE ? 0 : L->sdepth[cv];
        s += (double)((du - dl) + (dv - dl)) * L->cpad;
    }
    return s;
}

typedef struct BKWork {
    int32_t *root, *align, *posl, *order; /* order: layers flattened in this pass's order */
    uint32_t *indeg, *qbuf, *boff, *bidx;
    double *bw, *xs;
    int32_t *nbuf;
} BKWork;

/* vertical alignment + horizontal compaction for one of the 4 variants */
/* sepv[i]: separation between lay[i-1] and lay[i] (symmetric, so shared by all 4 passes) */
static bool bk_pass(Lay *L, const PairSet *conf, bool up, bool left, BKWork *W, const double *sepv, double *out) {
    size_t V = L->V, R = L->R;
    /* build pass layer order */
    for (size_t r = 0; r < R; r++) {
        size_t a = L->loff[r], n = L->loff[r + 1] - a;
        for (size_t i = 0; i < n; i++) {
            int32_t v = L->lay[a + (left ? i : n - 1 - i)];
            W->order[a + i] = v;
            W->posl[v] = (int32_t)i;
        }
    }
    for (size_t v = 0; v < V; v++) W->root[v] = W->align[v] = (int32_t)v;
    const uint32_t *off = up ? L->poff : L->soff, *adj = up ? L->pidx : L->sidx;
    for (size_t ri = 0; ri < R; ri++) {
        size_t r = up ? ri : R - 1 - ri;
        size_t a = L->loff[r], n = L->loff[r + 1] - a;
        int32_t prev = -1;
        for (size_t i = 0; i < n; i++) {
            int32_t v = W->order[a + i];
            uint32_t k0 = off[v], k1 = off[v + 1], m = k1 - k0;
            if (!m) continue;
            if (L->border[v]) { /* cluster side chains form one rigid block */
                int32_t w = (int32_t)adj[k0];
                if (prev < W->posl[w]) {
                    W->align[w] = v;
                    W->align[v] = W->root[v] = W->root[w];
                    prev = W->posl[w];
                }
                continue;
            }
            /* neighbours sorted by pass position */
            int32_t *ws = W->nbuf;
            for (uint32_t k = 0; k < m; k++) {
                int32_t w = (int32_t)adj[k0 + k];
                uint32_t j = k;
                while (j && W->posl[ws[j - 1]] > W->posl[w]) { ws[j] = ws[j - 1]; j--; }
                ws[j] = w;
            }
            for (uint32_t j = (m - 1) / 2; j <= m / 2; j++) { /* lower/upper median */
                int32_t w = ws[j];
                if (W->align[v] == v && prev < W->posl[w] && !pset_has(conf, v, w)) {
                    W->align[w] = v;
                    W->align[v] = W->root[v] = W->root[w];
                    prev = W->posl[w];
                }
            }
        }
    }
    /* block graph: root(u) -> root(v) for horizontally adjacent u,v */
    memset(W->indeg, 0, V * sizeof *W->indeg);
    memset(W->boff, 0, (V + 1) * sizeof *W->boff);
    for (size_t r = 0; r < R; r++)
        for (size_t i = L->loff[r] + 1; i < L->loff[r + 1]; i++) W->boff[W->root[W->order[i - 1]] + 1]++;
    for (size_t v = 0; v < V; v++) W->boff[v + 1] += W->boff[v];
    for (size_t v = 0; v < V; v++) W->qbuf[v] = W->boff[v];
    for (size_t r = 0; r < R; r++)
        for (size_t i = L->loff[r] + 1; i < L->loff[r + 1]; i++) {
            int32_t u = W->order[i - 1], v = W->order[i];
            int32_t ru = W->root[u], rv = W->root[v];
            uint32_t k = W->qbuf[ru]++;
            W->bidx[k] = (uint32_t)rv;
            W->bw[k] = sepv[left ? i : L->loff[r] + L->loff[r + 1] - i];
            W->indeg[rv]++;
        }
    /* Kahn over roots */
    size_t qh = 0, qt = 0;
    for (size_t v = 0; v < V; v++) {
        W->xs[v] = 0;
        if (W->root[v] == (int32_t)v && !W->indeg[v]) W->qbuf[qt++] = (uint32_t)v;
    }
    while (qh < qt) {
        uint32_t u = W->qbuf[qh++];
        for (uint32_t k = W->boff[u]; k < W->boff[u + 1]; k++) {
            uint32_t v = W->bidx[k];
            double c = W->xs[u] + W->bw[k];
            if (c > W->xs[v]) W->xs[v] = c;
            if (--W->indeg[v] == 0) W->qbuf[qt++] = v;
        }
    }
    /* pass 2: pull right towards successors (reverse topological) */
    for (size_t q = qt; q-- > 0;) {
        uint32_t u = W->qbuf[q];
        double mn = INFINITY;
        for (uint32_t k = W->boff[u]; k < W->boff[u + 1]; k++) {
            double c = W->xs[W->bidx[k]] - W->bw[k];
            if (c < mn) mn = c;
        }
        if (mn != INFINITY && mn > W->xs[u]) W->xs[u] = mn;
    }
    for (size_t v = 0; v < V; v++) out[v] = left ? W->xs[W->root[v]] : -W->xs[W->root[v]];
    return qt > 0 || V == 0;
}

[[nodiscard]] static bool bk(Lay *L) {
    size_t V = L->V, R = L->R;
    Arena *A = L->A;
    PairSet conf = {};
    /* type-1 conflicts: inner segment (dummy-dummy) crossing a non-inner one */
    for (size_t r = 1; r < R; r++) {
        size_t a = L->loff[r], n = L->loff[r + 1] - a, prevLen = L->loff[r] - L->loff[r - 1];
        int32_t k0 = 0;
        size_t scan = 0;
        for (size_t i = 0; i < n; i++) {
            int32_t v = L->lay[a + i], wv = NONE;
            if (L->dummy[v])
                for (uint32_t k = L->poff[v]; k < L->poff[v + 1]; k++)
                    if (L->dummy[L->pidx[k]]) { wv = (int32_t)L->pidx[k]; break; }
            int32_t k1 = wv != NONE ? L->pos[wv] : (int32_t)prevLen;
            if (wv != NONE || i == n - 1) {
                for (; scan <= i; scan++) {
                    int32_t sv = L->lay[a + scan];
                    for (uint32_t k = L->poff[sv]; k < L->poff[sv + 1]; k++) {
                        int32_t u = (int32_t)L->pidx[k];
                        int32_t up = L->pos[u];
                        if ((up < k0 || k1 < up) && !(L->dummy[u] && L->dummy[sv]))
                            if (!pset_add(A, &conf, u, sv)) return false;
                    }
                }
                k0 = k1;
            }
        }
    }
    /* segments crossing a cluster side chain may not be aligned (the chain must stay straight) */
    {
        /* chains never cross each other, so an edge crosses one iff the number of chain
           borders right of its head differs from the number right of its tail (O(V+E)) */
        int32_t *sr = ANEW(A, int32_t, V + 1), *sp = ANEW(A, int32_t, V + 1);
        if (!sr || !sp) return false;
        for (size_t r = 1; r < R; r++) {
            size_t a = L->loff[r], n = L->loff[r + 1] - a, pn = a - L->loff[r - 1];
            bool any = false;
            for (size_t i = 0; i <= n; i++) sr[i] = 0;
            for (size_t i = 0; i <= pn; i++) sp[i] = 0;
            for (size_t i = 0; i < n; i++) {
                int32_t b = L->lay[a + i];
                if (!L->border[b] || L->poff[b + 1] == L->poff[b]) continue;
                sr[i] = 1;
                sp[L->pos[L->pidx[L->poff[b]]]] = 1;
                any = true;
            }
            if (!any) continue;
            for (size_t i = n; i-- > 0;) sr[i] += sr[i + 1];
            for (size_t i = pn; i-- > 0;) sp[i] += sp[i + 1];
            for (size_t j = 0; j < n; j++) {
                int32_t v = L->lay[a + j];
                if (L->border[v]) continue;
                for (uint32_t k = L->poff[v]; k < L->poff[v + 1]; k++) {
                    int32_t u = (int32_t)L->pidx[k];
                    if (sr[j + 1] != sp[L->pos[u] + 1] && !pset_add(A, &conf, u, v)) return false;
                }
            }
        }
    }
    PROF("bk-conf");
    size_t E = L->soff[V];
    size_t maxdeg = 1;
    for (size_t v = 0; v < V; v++) {
        size_t d1 = L->poff[v + 1] - L->poff[v], d2 = L->soff[v + 1] - L->soff[v];
        if (d1 > maxdeg) maxdeg = d1;
        if (d2 > maxdeg) maxdeg = d2;
    }
    BKWork W = {
        .root = ANEW(A, int32_t, V), .align = ANEW(A, int32_t, V), .posl = ANEW(A, int32_t, V),
        .order = ANEW(A, int32_t, V), .indeg = ANEW(A, uint32_t, V), .qbuf = ANEW(A, uint32_t, V + 1),
        .boff = ANEW(A, uint32_t, V + 1), .bidx = ANEW(A, uint32_t, V + 1), .bw = ANEW(A, double, V + 1),
        .xs = ANEW(A, double, V), .nbuf = ANEW(A, int32_t, maxdeg),
    };
    double *xss[4];
    for (unsigned k = 0; k < 4; k++) xss[k] = ANEW(A, double, V);
    if (!W.root || !W.align || !W.posl || !W.order || !W.indeg || !W.qbuf || !W.boff || !W.bidx ||
        !W.bw || !W.xs || !W.nbuf || !xss[0] || !xss[1] || !xss[2] || !xss[3])
        return false;
    (void)E;
    double *sepv = ANEW(A, double, V + 1);
    if (!sepv) return false;
    for (size_t r = 0; r < R; r++)
        for (size_t i = L->loff[r] + 1; i < L->loff[r + 1]; i++) sepv[i] = sep(L, L->lay[i - 1], L->lay[i]);
    for (unsigned k = 0; k < 4; k++) { (void)bk_pass(L, &conf, k < 2, (k & 1u) == 0, &W, sepv, xss[k]); PROF("bk-pass"); }
    /* align to the narrowest layout */
    double mins[4], maxs[4], best = INFINITY;
    unsigned bi = 0;
    for (unsigned k = 0; k < 4; k++) {
        double mn = INFINITY, mx = -INFINITY;
        for (size_t v = 0; v < V; v++) {
            double l = xss[k][v] - L->w[v] / 2, r = xss[k][v] + L->w[v] / 2;
            if (l < mn) mn = l;
            if (r > mx) mx = r;
        }
        mins[k] = mn, maxs[k] = mx;
        if (mx - mn < best) { best = mx - mn; bi = k; }
    }
    for (unsigned k = 0; k < 4; k++) {
        double d = (k & 1u) == 0 ? mins[bi] - mins[k] : maxs[bi] - maxs[k];
        for (size_t v = 0; v < V; v++) xss[k][v] += d;
    }
    for (size_t v = 0; v < V; v++) {
        /* median of 4 = mean of the two middle values: max of the pair minima, min of the pair maxima */
        double a = xss[0][v], b = xss[1][v], c = xss[2][v], d = xss[3][v];
        double m1 = dmax(dmin(a, b), dmin(c, d)), m2 = dmin(dmax(a, b), dmax(c, d));
        L->x[v] = (m1 + m2) / 2;
    }
    return true;
}

/* ---------- geometry helpers ---------- */
static void clip_to_node(const Graph *g, int32_t v, double px, double py, double *ox, double *oy) {
    double cx = g->nx[v], cy = g->ny[v], hw = g->nw[v] / 2, hh = g->nh[v] / 2;
    double dx = px - cx, dy = py - cy;
    if (fabs(dx) < 1e-9 && fabs(dy) < 1e-9) { *ox = cx; *oy = cy + hh; return; }
    double t;
    switch (g->nshape[v]) {
    case SH_CIRCLE: case SH_DCIRCLE: t = hw / sqrt(dx * dx + dy * dy); break;
    case SH_RHOMBUS: t = 1.0 / (fabs(dx) / hw + fabs(dy) / hh); break;
    default: {
        double tx = fabs(dx) > 1e-9 ? hw / fabs(dx) : INFINITY, ty = fabs(dy) > 1e-9 ? hh / fabs(dy) : INFINITY;
        t = tx < ty ? tx : ty;
        break;
    }
    }
    if (t > 1) t = 1;
    *ox = cx + dx * t;
    *oy = cy + dy * t;
}

static void reverse_pts(size_t np, double P[static 2 * np]) {
    for (size_t i = 0; i < np / 2; i++) {
        size_t j = np - 1 - i;
        double tx = P[2 * i], ty = P[2 * i + 1];
        P[2 * i] = P[2 * j], P[2 * i + 1] = P[2 * j + 1];
        P[2 * j] = tx, P[2 * j + 1] = ty;
    }
}

/* polyline starts outside box and ends inside it: cut at the first entry. Returns new count. */
static size_t clip_poly_box(size_t np, double P[static 2 * np], double x0, double y0, double x1, double y1) {
    #define INSIDE(i) (P[2 * (i)] >= x0 && P[2 * (i)] <= x1 && P[2 * (i) + 1] >= y0 && P[2 * (i) + 1] <= y1)
    if (np < 2 || INSIDE(0)) return np;
    size_t j = 1;
    while (j < np && !INSIDE(j)) j++;
    if (j == np) return np;
    double px = P[2 * j - 2], py = P[2 * j - 1], dx = P[2 * j] - px, dy = P[2 * j + 1] - py, t = 0;
    if (px < x0 && dx > 0) t = dmax(t, (x0 - px) / dx);
    if (px > x1 && dx < 0) t = dmax(t, (x1 - px) / dx);
    if (py < y0 && dy > 0) t = dmax(t, (y0 - py) / dy);
    if (py > y1 && dy < 0) t = dmax(t, (y1 - py) / dy);
    P[2 * j] = px + dx * t, P[2 * j + 1] = py + dy * t;
    return j + 1;
    #undef INSIDE
}

typedef struct PortKey { double k1, k2; uint32_t e; } PortKey;
static inline bool port_lt(const PortKey *a, const PortKey *b) {
    if (a->k1 != b->k1) return a->k1 < b->k1;
    if (a->k2 != b->k2) return a->k2 < b->k2;
    return a->e < b->e;
}
static int port_cmp(const void *a, const void *b) { return port_lt(a, b) ? -1 : port_lt(b, a) ? 1 : 0; }
static void port_sort(size_t n, PortKey a[static n]) {
    if (n > 24) { qsort(a, n, sizeof *a, port_cmp); return; }
    for (size_t i = 1; i < n; i++) {
        PortKey t = a[i];
        size_t j = i;
        while (j && port_lt(&t, &a[j - 1])) { a[j] = a[j - 1]; j--; }
        a[j] = t;
    }
}

/* hybrid cycle removal thresholds (see results_v7): greedy only for big graphs with many back edges */
constexpr size_t ACYC_GREEDY_MIN_N = 1500;
constexpr size_t ACYC_GREEDY_REV_DIV = 20; /* DFS reverses >= 5% of nodes' worth of edges */

static cmm_status layout_core(Graph *g) {
    Arena *A = g->arena;
    size_t N = g->nn, E = g->ne, S = g->ns;
    bool horiz = g->dir == DIR_LR || g->dir == DIR_RL;
    if (!g->nw) { g->nw = ANEW(A, double, N); g->nh = ANEW(A, double, N); }
    g->nx = ANEW(A, double, N); g->ny = ANEW(A, double, N);
    g->eptoff = ANEW(A, uint32_t, E); g->eptn = ANEW(A, uint32_t, E);
    g->elx = ANEW(A, double, E); g->ely = ANEW(A, double, E);
    g->elw = ANEW(A, double, E); g->elh = ANEW(A, double, E);
    g->sx = ANEW(A, double, S); g->sy = ANEW(A, double, S);
    g->sw = ANEW(A, double, S); g->sh = ANEW(A, double, S);
    int32_t *sdepth = ANEW(A, int32_t, S), *srep = ANEW(A, int32_t, S);
    int32_t *ef = ANEW(A, int32_t, E), *et = ANEW(A, int32_t, E), *mlen = ANEW(A, int32_t, E);
    uint8_t *rev = ANEW(A, uint8_t, E);
    if (!g->nw || !g->nh || !g->nx || !g->ny || !g->eptoff || !g->eptn || !g->elx || !g->ely || !g->elw ||
        !g->elh || !g->sx || !g->sy || !g->sw || !g->sh || !sdepth || !srep || !ef || !et || !mlen || !rev)
        return CMM_ERR_NOMEM;

    PROF("");
    for (size_t v = 0; v < N; v++) node_size(g, v);
    for (size_t s = 0; s < S; s++) {
        int32_t d = 1;
        for (int32_t c = g->sparent[s]; c != NONE; c = g->sparent[c]) d++;
        sdepth[s] = d;
        srep[s] = NONE;
    }
    for (size_t v = 0; v < N; v++) {
        if (g->nflags[v] & NF_HIDDEN) continue;
        for (int32_t c = g->nparent[v]; c != NONE; c = g->sparent[c])
            if (srep[c] == NONE) srep[c] = (int32_t)v;
    }
    bool has_labels = false;
    int32_t *esub = ANEW(A, int32_t, 2 * E + 1);
    if (!esub) return CMM_ERR_NOMEM;
    for (size_t e = 0; e < E; e++) {
        int32_t a = g->efrom[e], b = g->eto[e];
        for (unsigned k = 0; k < 2; k++) {
            int32_t *p = k ? &b : &a;
            esub[2 * e + k] = NONE;
            if (g->nflags[*p] & NF_SUBREF) {
                int32_t s = NONE;
                for (size_t i = 0; i < S; i++) if (str_eq(g->sid[i], g->nid[*p])) { s = (int32_t)i; break; }
                *p = s == NONE ? NONE : srep[s];
                esub[2 * e + k] = s;
            }
        }
        ef[e] = a, et[e] = b;
        double lw = 0;
        unsigned ln = 0;
        measure(g, &g->elabel[e], &lw, &ln);
        g->elw[e] = ln ? lw + 8 : 0;
        g->elh[e] = ln ? ln * g->font_size * 1.5 + 4 : 0;
        if (ln && a != NONE && b != NONE && a != b) has_labels = true;
    }
    for (size_t e = 0; e < E; e++) mlen[e] = g->elen[e] * (has_labels ? 2 : 1);
    double ranksep = has_labels ? g->rank_sep / 2 : g->rank_sep;

    /* layout-space sizes (transposed for LR/RL) */
    #define LW(v) (horiz ? g->nh[v] : g->nw[v])
    #define LH(v) (horiz ? g->nw[v] : g->nh[v])

    PROF("sizes");
    /* ---- out-CSR on real nodes ---- */
    uint32_t *ooff = ANEW(A, uint32_t, N + 1), *oidx = ANEW(A, uint32_t, E + 1);
    int32_t *rank = ANEW(A, int32_t, N);
    uint32_t *stk = ANEW(A, uint32_t, N + 1), *it = ANEW(A, uint32_t, N + 1), *indeg = ANEW(A, uint32_t, N + 1);
    uint8_t *state = ANEW(A, uint8_t, N + 1);
    if (!ooff || !oidx || !rank || !stk || !it || !indeg || !state) return CMM_ERR_NOMEM;
    #define EVALID(e) (ef[e] != NONE && et[e] != NONE && ef[e] != et[e])
    memset(ooff, 0, (N + 1) * sizeof *ooff);
    for (size_t e = 0; e < E; e++) if (EVALID(e)) ooff[ef[e] + 1]++;
    for (size_t v = 0; v < N; v++) ooff[v + 1] += ooff[v];
    for (size_t v = 0; v < N; v++) it[v] = ooff[v];
    for (size_t e = 0; e < E; e++) if (EVALID(e)) oidx[it[ef[e]]++] = (uint32_t)e;
    /* cycle removal: greedy feedback arc set */
    {
        uint8_t *valid = ANEW(A, uint8_t, E + 1), *hid = ANEW(A, uint8_t, N + 1);
        if (!valid || !hid) return CMM_ERR_NOMEM;
        for (size_t e = 0; e < E; e++) valid[e] = EVALID(e);
        for (size_t v = 0; v < N; v++) hid[v] = (g->nflags[v] & NF_HIDDEN) ? 1 : 0;
        /* dagre default (mermaid): DFS feedback arc set in node/edge insertion order */
        memset(state, 0, N * sizeof *state);
        for (size_t e = 0; e < E; e++) rev[e] = 0;
        size_t nrev = 0;
        for (size_t r = 0; r < N; r++) {
            if (state[r] || hid[r]) continue;
            size_t sp = 0;
            stk[sp++] = (uint32_t)r; state[r] = 1; it[r] = ooff[r];
            while (sp) {
                uint32_t u = stk[sp - 1];
                if (it[u] == ooff[u + 1]) { state[u] = 2; sp--; continue; }
                uint32_t e = oidx[it[u]++], w = (uint32_t)et[e];
                if (hid[w]) continue;
                if (state[w] == 1) rev[e] = 1, nrev++;
                else if (!state[w]) { state[w] = 1; it[w] = ooff[w]; stk[sp++] = w; }
            }
        }
        /* hybrid: DFS reverses arbitrary back edges, which on big cyclic graphs stretches the ranking
         * (xlarge: 335 -> 1277 ranks, 8x dummies). Above the threshold use greedy Eades FAS instead. */
        bool greedy = g->acyclicer == CMM_ACYC_GREEDY ||
                      (g->acyclicer == CMM_ACYC_AUTO && N >= ACYC_GREEDY_MIN_N && nrev * ACYC_GREEDY_REV_DIV >= N);
#ifdef CMM_PROFILE
        fprintf(stderr, "  fas: N=%zu dfs_rev=%zu -> %s\n", N, nrev, greedy ? "greedy" : "dfs");
#endif
        if (greedy && !greedy_fas(A, N, E, ef, et, valid, hid, rev)) return CMM_ERR_NOMEM;
    }
    /* effective DAG CSR */
    #define EU(e) (rev[e] ? et[e] : ef[e])
    #define EV(e) (rev[e] ? ef[e] : et[e])
    memset(ooff, 0, (N + 1) * sizeof *ooff);
    memset(indeg, 0, N * sizeof *indeg);
    for (size_t e = 0; e < E; e++) if (EVALID(e)) { ooff[EU(e) + 1]++; indeg[EV(e)]++; }
    for (size_t v = 0; v < N; v++) ooff[v + 1] += ooff[v];
    for (size_t v = 0; v < N; v++) it[v] = ooff[v];
    for (size_t e = 0; e < E; e++) if (EVALID(e)) oidx[it[EU(e)]++] = (uint32_t)e;
    /* longest path (Kahn) */
    uint32_t *topo = stk, *indeg2 = it;
    size_t qh = 0, qt = 0;
    for (size_t v = 0; v < N; v++) {
        rank[v] = 0;
        indeg2[v] = indeg[v];
        if (!(g->nflags[v] & NF_HIDDEN) && !indeg[v]) topo[qt++] = (uint32_t)v;
    }
    while (qh < qt) {
        uint32_t u = topo[qh++];
        for (uint32_t k = ooff[u]; k < ooff[u + 1]; k++) {
            uint32_t e = oidx[k], w = (uint32_t)EV(e);
            if (rank[u] + mlen[e] > rank[w]) rank[w] = rank[u] + mlen[e];
            if (--indeg2[w] == 0) topo[qt++] = w;
        }
    }
    {
        int32_t *eu = ANEW(A, int32_t, E + 1), *ev = ANEW(A, int32_t, E + 1);
        uint8_t *valid = ANEW(A, uint8_t, E + 1), *hid = ANEW(A, uint8_t, N + 1);
        if (!eu || !ev || !valid || !hid) return CMM_ERR_NOMEM;
        for (size_t e = 0; e < E; e++) {
            valid[e] = EVALID(e);
            eu[e] = valid[e] ? EU(e) : 0;
            ev[e] = valid[e] ? EV(e) : 0;
        }
        for (size_t v = 0; v < N; v++) hid[v] = (g->nflags[v] & NF_HIDDEN) ? 1 : 0;
        if (!network_simplex(A, N, E, eu, ev, mlen, valid, hid, rank, 20 * N + 100)) return CMM_ERR_NOMEM;
    }
    int32_t minr = INT32_MAX, maxr = 0;
    for (size_t v = 0; v < N; v++) if (!(g->nflags[v] & NF_HIDDEN) && rank[v] < minr) minr = rank[v];
    if (minr == INT32_MAX) minr = 0;
    for (size_t v = 0; v < N; v++) {
        rank[v] -= minr;
        if (!(g->nflags[v] & NF_HIDDEN) && rank[v] > maxr) maxr = rank[v];
    }

    PROF("rank");
    /* ---- normalisation: dummies ---- */
    size_t D = 0;
    uint32_t *choff = ANEW(A, uint32_t, E + 1);
    if (!choff) return CMM_ERR_NOMEM;
    for (size_t e = 0; e < E; e++) {
        choff[e] = (uint32_t)D;
        if (EVALID(e)) D += (size_t)(rank[EV(e)] - rank[EU(e)] - 1);
    }
    choff[E] = (uint32_t)D;
    /* cluster rank spans (over real members) and border-node budget */
    int32_t *cmin = ANEW(A, int32_t, S + 1), *cmax = ANEW(A, int32_t, S + 1);
    uint32_t *bbase = ANEW(A, uint32_t, S + 1);
    if (!cmin || !cmax || !bbase) return CMM_ERR_NOMEM;
    for (size_t s = 0; s < S; s++) cmin[s] = INT32_MAX, cmax[s] = -1;
    for (size_t v = 0; v < N; v++) {
        if (g->nflags[v] & NF_HIDDEN) continue;
        for (int32_t c = g->nparent[v]; c != NONE; c = g->sparent[c]) {
            if (rank[v] < cmin[c]) cmin[c] = rank[v];
            if (rank[v] > cmax[c]) cmax[c] = rank[v];
        }
    }
    size_t B = 0;
    for (size_t s = 0; s < S; s++) {
        bbase[s] = (uint32_t)(N + D + B);
        if (cmax[s] >= cmin[s]) B += 2 * (size_t)(cmax[s] - cmin[s] + 1);
    }
    size_t V = N + D + B;
    Lay L = {.g = g, .A = A, .N = N, .V = V, .R = (size_t)maxr + 1, .sdepth = sdepth,
             .nodesep = g->node_sep, .cpad = horiz ? CLUSTER_PAD + CLUSTER_TITLE / 2 : CLUSTER_PAD};
    L.rank = ANEW(A, int32_t, V); L.cl = ANEW(A, int32_t, V); L.pos = ANEW(A, int32_t, V);
    L.lay = ANEW(A, int32_t, V); L.loff = ANEW(A, uint32_t, L.R + 2); L.dummy = ANEW(A, uint8_t, V);
    L.w = ANEW(A, double, V); L.h = ANEW(A, double, V); L.x = ANEW(A, double, V); L.bc = ANEW(A, double, V);
    L.poff = ANEW(A, uint32_t, V + 2); L.soff = ANEW(A, uint32_t, V + 2);
    int32_t *dedge = ANEW(A, int32_t, D + 1);
    if (!L.rank || !L.cl || !L.pos || !L.lay || !L.loff || !L.dummy || !L.w || !L.h || !L.x || !L.bc ||
        !L.poff || !L.soff || !dedge)
        return CMM_ERR_NOMEM;
    for (size_t v = 0; v < N; v++) {
        bool hid = g->nflags[v] & NF_HIDDEN;
        L.rank[v] = hid ? -1 : rank[v];
        L.cl[v] = g->nparent[v];
        L.dummy[v] = 0;
        L.w[v] = LW(v);
        L.h[v] = LH(v);
    }
    /* self-loops are drawn right of the node (TB/BT): reserve room for the outermost loop and the
     * stacked labels so neighbours in the layer don't collide (dagre does this with selfedge dummies) */
    if (!horiz) {
        unsigned *nl = nullptr;
        for (size_t e = 0; e < E; e++) {
            int32_t a = g->efrom[e];
            if (a == NONE || a != g->eto[e] || g->estroke[e] == ST_INVISIBLE) continue;
            if (!L.rpad) {
                L.rpad = ANEW(A, double, V);
                nl = ANEW(A, unsigned, N);
                if (!L.rpad || !nl) return CMM_ERR_NOMEM;
                memset(L.rpad, 0, V * sizeof *L.rpad);
                memset(nl, 0, N * sizeof *nl);
            }
            unsigned k = nl[a]++;
            double ext = SELFLOOP_BULGE * selfloop_r(k) + (g->elabel[e].n ? SELFLOOP_GAP + g->elw[e] : 0);
            if (ext > L.rpad[a]) L.rpad[a] = ext;
        }
    }
    size_t nseg = 0;
    for (size_t e = 0; e < E; e++) {
        if (!EVALID(e)) continue;
        int32_t u = EU(e), v = EV(e), span = rank[v] - rank[u];
        int32_t lca = cluster_lca(g, sdepth, g->nparent[u], g->nparent[v]);
        for (int32_t k = 1; k < span; k++) {
            size_t d = N + choff[e] + (size_t)(k - 1);
            L.rank[d] = rank[u] + k;
            L.cl[d] = lca;
            L.dummy[d] = 1;
            dedge[d - N] = (int32_t)e;
            bool lab = has_labels && g->elw[e] > 0 && k == span / 2;
            L.w[d] = lab ? (horiz ? g->elh[e] : g->elw[e]) : 0;
            L.h[d] = lab ? (horiz ? g->elw[e] : g->elh[e]) : 0;
        }
        nseg += (size_t)span;
    }
    L.border = ANEW(A, uint8_t, V + 1);
    if (!L.border) return CMM_ERR_NOMEM;
    memset(L.border, 0, V + 1);
    for (size_t c = 0; c < S; c++) {
        if (cmax[c] < cmin[c]) continue;
        for (int32_t r = cmin[c]; r <= cmax[c]; r++)
            for (unsigned side = 0; side < 2; side++) {
                size_t d = bbase[c] + 2 * (size_t)(r - cmin[c]) + side;
                L.rank[d] = r;
                L.cl[d] = (int32_t)c;
                L.dummy[d] = 1;
                L.border[d] = (uint8_t)(1 + side);
                L.w[d] = L.h[d] = 0;
            }
        nseg += 2 * (size_t)(cmax[c] - cmin[c]);
    }
    L.pidx = ANEW(A, uint32_t, nseg + 1);
    L.sidx = ANEW(A, uint32_t, nseg + 1);
    uint32_t *pfill = ANEW(A, uint32_t, V + 1), *sfill = ANEW(A, uint32_t, V + 1);
    if (!L.pidx || !L.sidx || !pfill || !sfill) return CMM_ERR_NOMEM;
    memset(L.poff, 0, (V + 2) * sizeof *L.poff);
    memset(L.soff, 0, (V + 2) * sizeof *L.soff);
    #define CHAIN(e, k, span) ((k) == 0 ? EU(e) : (k) == (span) ? EV(e) : (int32_t)(N + choff[e] + (size_t)(k) - 1))
    for (size_t e = 0; e < E; e++) {
        if (!EVALID(e)) continue;
        int32_t span = rank[EV(e)] - rank[EU(e)];
        for (int32_t k = 0; k < span; k++) {
            L.soff[CHAIN(e, k, span) + 1]++;
            L.poff[CHAIN(e, k + 1, span) + 1]++;
        }
    }
    for (size_t c = 0; c < S; c++)
        for (int32_t r = cmin[c]; r < cmax[c]; r++)
            for (unsigned side = 0; side < 2; side++) {
                size_t d = bbase[c] + 2 * (size_t)(r - cmin[c]) + side;
                L.soff[d + 1]++;
                L.poff[d + 2 + 1]++;
            }
    for (size_t v = 0; v < V; v++) { L.poff[v + 1] += L.poff[v]; L.soff[v + 1] += L.soff[v]; }
    for (size_t v = 0; v < V; v++) { pfill[v] = L.poff[v]; sfill[v] = L.soff[v]; }
    for (size_t e = 0; e < E; e++) {
        if (!EVALID(e)) continue;
        int32_t span = rank[EV(e)] - rank[EU(e)];
        for (int32_t k = 0; k < span; k++) {
            int32_t a = CHAIN(e, k, span), b = CHAIN(e, k + 1, span);
            L.sidx[sfill[a]++] = (uint32_t)b;
            L.pidx[pfill[b]++] = (uint32_t)a;
        }
    }
    for (size_t c = 0; c < S; c++)
        for (int32_t r = cmin[c]; r < cmax[c]; r++)
            for (unsigned side = 0; side < 2; side++) {
                size_t d = bbase[c] + 2 * (size_t)(r - cmin[c]) + side;
                L.sidx[sfill[d]++] = (uint32_t)(d + 2);
                L.pidx[pfill[d + 2]++] = (uint32_t)d;
            }

    PROF("normalize");
    /* ---- initial order: DFS by rank ---- */
    size_t R = L.R;
    uint32_t *cnt = ANEW(A, uint32_t, R + 1), *byrank = ANEW(A, uint32_t, V + 1);
    uint32_t *dstk = ANEW(A, uint32_t, V + 1), *dit = ANEW(A, uint32_t, V + 1);
    uint8_t *vis = ANEW(A, uint8_t, V + 1);
    if (!cnt || !byrank || !dstk || !dit || !vis) return CMM_ERR_NOMEM;
    memset(cnt, 0, (R + 1) * sizeof *cnt);
    for (size_t v = 0; v < V; v++) if (L.rank[v] >= 0) cnt[L.rank[v] + 1]++;
    for (size_t r = 0; r < R; r++) cnt[r + 1] += cnt[r];
    for (size_t r = 0; r <= R; r++) L.loff[r] = cnt[r];
    L.loff[R + 1] = L.loff[R];
    for (size_t v = 0; v < V; v++) if (L.rank[v] >= 0) byrank[cnt[L.rank[v]]++] = (uint32_t)v;
    size_t placed = L.loff[R];
    memset(vis, 0, V);
    for (size_t r = 0; r < R; r++) cnt[r] = L.loff[r];
    for (size_t i = 0; i < placed; i++) {
        uint32_t s0 = byrank[i];
        if (vis[s0]) continue;
        size_t sp = 0;
        dstk[sp++] = s0;
        dit[s0] = L.soff[s0];
        vis[s0] = 1;
        L.pos[s0] = (int32_t)(cnt[L.rank[s0]] - L.loff[L.rank[s0]]);
        L.lay[cnt[L.rank[s0]]++] = (int32_t)s0;
        while (sp) {
            uint32_t u = dstk[sp - 1];
            if (dit[u] < L.soff[u + 1]) {
                uint32_t w = L.sidx[dit[u]++];
                if (vis[w]) continue;
                vis[w] = 1;
                dit[w] = L.soff[w];
                L.pos[w] = (int32_t)(cnt[L.rank[w]] - L.loff[L.rank[w]]);
                L.lay[cnt[L.rank[w]]++] = (int32_t)w;
                dstk[sp++] = w;
            } else sp--;
        }
    }

    /* ---- crossing minimisation ---- */
    size_t maxw = 1;
    for (size_t r = 0; r < R; r++) if (L.loff[r + 1] - L.loff[r] > maxw) maxw = L.loff[r + 1] - L.loff[r];
    Key *keys = ANEW(A, Key, maxw);
    uint32_t *idx = ANEW(A, uint32_t, maxw), *tmp = ANEW(A, uint32_t, maxw);
    uint32_t *cbuf = ANEW(A, uint32_t, nseg + 1), *fen = ANEW(A, uint32_t, maxw + 2);
    double *csum = ANEW(A, double, S + 1);
    uint32_t *ccnt = ANEW(A, uint32_t, S + 1);
    int32_t *best = ANEW(A, int32_t, V + 1);
    if (!keys || !idx || !tmp || !cbuf || !fen || !csum || !ccnt || !best) return CMM_ERR_NOMEM;
    L.lsum = ANEW(A, double, S + 1), L.lval = ANEW(A, double, S + 1);
    L.lcnt = ANEW(A, uint32_t, S + 1), L.plist = ANEW(A, int32_t, S + 1);
    L.S = S;
    L.items = ANEW(A, BcItem, 2 * (maxw + S + 1));
    L.bstart = ANEW(A, uint32_t, S + 2), L.bcur = ANEW(A, uint32_t, S + 2), L.bstk = ANEW(A, uint32_t, S + 2);
    if (!L.lsum || !L.lval || !L.lcnt || !L.plist || !L.items || !L.bstart || !L.bcur || !L.bstk) return CMM_ERR_NOMEM;
    memset(L.lsum, 0, (S + 1) * sizeof *L.lsum);
    memset(L.lcnt, 0, (S + 1) * sizeof *L.lcnt);
    memset(csum, 0, (S + 1) * sizeof *csum);
    memset(ccnt, 0, (S + 1) * sizeof *ccnt);
    bool clusters = S > 0;
    if (clusters) cluster_keys(&L, S, csum, ccnt);
    if (clusters) /* establish cluster contiguity (borders at extremes) before any scoring */
        for (size_t r = 0; r < R; r++) sweep_layer(&L, r, false, true, keys, idx, tmp, csum, ccnt, clusters);
    uint64_t bestc = cross_all(&L, cbuf, fen);
    memcpy(best, L.lay, placed * sizeof *best);
    unsigned since = 0;
    /* patience: small graphs afford one extra non-improving sweep; on big ones the sweeps settle into a
     * period-2 oscillation (down/up alternate) and the extra sweep only repeats a seen order */
    const unsigned patience = nseg <= 256 ? 2 : 1;
    /* transpose (final polish only; per-sweep transpose measured no gain on bench inputs)
     * budget: generous for small graphs, bounded (~linear) for big ones */
    int64_t tbudget = 2000000 + 40 * (int64_t)nseg;
    uint8_t *tdirty = ANEW(A, uint8_t, R + 2);
    if (!tdirty) return CMM_ERR_NOMEM;
    for (unsigned iter = 0; iter < 24 && since < patience && bestc > 0; iter++) {
        bool down = (iter & 1u) == 0;
        if (clusters) cluster_keys(&L, S, csum, ccnt);
        if (down) for (size_t r = 1; r < R; r++) sweep_layer(&L, r, true, false, keys, idx, tmp, csum, ccnt, clusters);
        else for (size_t r = R - 1; r-- > 0;) sweep_layer(&L, r, false, false, keys, idx, tmp, csum, ccnt, clusters);
        uint64_t c = cross_all(&L, cbuf, fen);
        if (c < bestc) { bestc = c; memcpy(best, L.lay, placed * sizeof *best); since = 0; }
        else since++;
#ifdef CMM_PROFILE
        fprintf(stderr, "  iter %u c=%llu\n", iter, (unsigned long long)c);
#endif
    }
    memcpy(L.lay, best, placed * sizeof *best);
    for (size_t r = 0; r < R; r++)
        for (size_t i = L.loff[r]; i < L.loff[r + 1]; i++) L.pos[L.lay[i]] = (int32_t)(i - L.loff[r]);
    if (bestc > 0 && tbudget > 0) { /* final polish on the kept-best order */
        uint64_t gn = transpose(&L, &tbudget, tdirty);
#ifdef CMM_PROFILE
        fprintf(stderr, "  transpose gain=%llu\n", (unsigned long long)gn);
#endif
        bestc = gn <= bestc ? bestc - gn : 0;
    }

#ifdef CMM_PROFILE
    fprintf(stderr, "  crossings=%llu tbudget_left=%lld\n", (unsigned long long)bestc, (long long)tbudget);
#endif
    PROF("order");
    /* ---- x via Brandes-Köpf ---- */
    if (!bk(&L)) return CMM_ERR_NOMEM;
    /* pull long-edge dummy chains (as a rigid block) toward the endpoints' mean x, order-preserving */
    for (size_t e = 0; e < E; e++) {
        if (!EVALID(e)) continue;
        int32_t u = EU(e), v = EV(e), span = rank[v] - rank[u];
        if (span < 2) continue;
        double lo = -INFINITY, hi = INFINITY, x0 = L.x[N + choff[e]];
        bool rigid = true;
        for (int32_t k = 1; k < span && rigid; k++) {
            int32_t d = (int32_t)(N + choff[e] + (size_t)(k - 1));
            if (fabs(L.x[d] - x0) > 0.5 || L.w[d] > 0) { rigid = false; break; }
            int32_t r = L.rank[d], i = L.pos[d];
            size_t a = L.loff[r], n = L.loff[r + 1] - a;
            if (i > 0) { int32_t q = L.lay[a + (size_t)i - 1]; lo = dmax(lo, L.x[q] + sep(&L, q, d)); }
            if ((size_t)i + 1 < n) { int32_t q = L.lay[a + (size_t)i + 1]; hi = dmin(hi, L.x[q] - sep(&L, d, q)); }
        }
        if (!rigid || lo > hi) continue;
        double t = dmax(lo, dmin(hi, (L.x[u] + L.x[v]) / 2));
        for (int32_t k = 1; k < span; k++) L.x[N + choff[e] + (size_t)(k - 1)] = t;
    }

    PROF("bk");
#ifdef CMM_PROFILE
    for (size_t c = 0; c < S; c++) {
        if (cmax[c] < cmin[c]) continue;
        double mn[2] = {1e18, 1e18}, mx[2] = {-1e18, -1e18};
        for (int32_t r = cmin[c]; r <= cmax[c]; r++)
            for (unsigned side = 0; side < 2; side++) {
                double x = L.x[bbase[c] + 2 * (size_t)(r - cmin[c]) + side];
                mn[side] = dmin(mn[side], x), mx[side] = dmax(mx[side], x);
            }
        fprintf(stderr, "cl %zu ranks %d-%d left %.0f..%.0f right %.0f..%.0f\n", c, cmin[c], cmax[c], mn[0], mx[0], mn[1], mx[1]);
    }
#endif
    /* ---- rank axis ---- */
    double *ry = ANEW(A, double, R + 1), *rh = ANEW(A, double, R + 1);
    double *gtop = ANEW(A, double, R + 1), *gbot = ANEW(A, double, R + 1);
    if (!ry || !rh || !gtop || !gbot) return CMM_ERR_NOMEM;
    for (size_t r = 0; r <= R; r++) rh[r] = 0, gtop[r] = 0, gbot[r] = 0;
    for (size_t v = 0; v < V; v++) {
        if (L.rank[v] < 0) continue;
        if (L.h[v] > rh[L.rank[v]]) rh[L.rank[v]] = L.h[v];
        if (v < N) {
            unsigned t = 0, b = 0;
            for (int32_t c = g->nparent[v]; c != NONE; c = g->sparent[c]) {
                if (cmin[c] == L.rank[v]) t++;
                if (cmax[c] == L.rank[v]) b++;
            }
            double tt = t * (CLUSTER_PAD + (horiz ? 0 : CLUSTER_TITLE)), bb = b * CLUSTER_PAD;
            if (tt > gtop[L.rank[v]]) gtop[L.rank[v]] = tt;
            if (bb > gbot[L.rank[v]]) gbot[L.rank[v]] = bb;
        }
    }
    double cy = 0;
    for (size_t r = 0; r < R; r++) {
        if (r) cy += rh[r - 1] / 2 + ranksep + gbot[r - 1] + gtop[r] + rh[r] / 2;
        else cy = gtop[0] + rh[0] / 2;
        ry[r] = cy;
    }
    /* write back real-node coords in final orientation */
    double *X = L.x;
    #define FX(xx, yy) (horiz ? (yy) : (xx))
    #define FY(xx, yy) (horiz ? (xx) : (yy))
    double sgx = g->dir == DIR_RL ? -1 : 1, sgy = g->dir == DIR_BT ? -1 : 1;
    for (size_t v = 0; v < N; v++) {
        if (L.rank[v] < 0) { g->nx[v] = g->ny[v] = 0; continue; }
        double lx = X[v], ly = ry[L.rank[v]];
        g->nx[v] = FX(lx, ly) * sgx;
        g->ny[v] = FY(lx, ly) * sgy;
    }
    /* clusters: deepest first (before edges: edges to subgraph ids clip against the boxes) */
    for (size_t s = 0; s < S; s++) { g->sx[s] = INFINITY; g->sy[s] = INFINITY; g->sw[s] = -INFINITY; g->sh[s] = -INFINITY; }
    for (size_t v = 0; v < N; v++) {
        if (L.rank[v] < 0) continue;
        for (int32_t c = g->nparent[v]; c != NONE; c = g->sparent[c]) {
            double x0 = g->nx[v] - g->nw[v] / 2, x1 = g->nx[v] + g->nw[v] / 2;
            double y0 = g->ny[v] - g->nh[v] / 2, y1 = g->ny[v] + g->nh[v] / 2;
            if (x0 < g->sx[c]) g->sx[c] = x0;
            if (y0 < g->sy[c]) g->sy[c] = y0;
            if (x1 > g->sw[c]) g->sw[c] = x1;
            if (y1 > g->sh[c]) g->sh[c] = y1;
        }
    }
    int32_t maxd = 0;
    for (size_t s = 0; s < S; s++) if (sdepth[s] > maxd) maxd = sdepth[s];
    for (int32_t d = maxd; d >= 1; d--)
        for (size_t s = 0; s < S; s++) {
            if (sdepth[s] != d || g->sx[s] == INFINITY) continue;
            double tw;
            unsigned tl;
            measure(g, &g->stitle[s], &tw, &tl);
            tw += 2 * CLUSTER_PAD;
            g->sx[s] -= CLUSTER_PAD; g->sw[s] += CLUSTER_PAD;
            g->sy[s] -= CLUSTER_PAD + CLUSTER_TITLE; g->sh[s] += CLUSTER_PAD;
            if (g->sw[s] - g->sx[s] < tw) { double c = (g->sw[s] + g->sx[s]) / 2; g->sx[s] = c - tw / 2; g->sw[s] = c + tw / 2; }
            int32_t p = g->sparent[s];
            if (p != NONE) {
                if (g->sx[s] < g->sx[p]) g->sx[p] = g->sx[s];
                if (g->sy[s] < g->sy[p]) g->sy[p] = g->sy[s];
                if (g->sw[s] > g->sw[p]) g->sw[p] = g->sw[s];
                if (g->sh[s] > g->sh[p]) g->sh[p] = g->sh[s];
            }
        }
    for (size_t s = 0; s < S; s++) { /* convert max corner -> size */
        if (g->sx[s] == INFINITY) { g->sx[s] = g->sy[s] = 0; g->sw[s] = g->sh[s] = 0; continue; }
        g->sw[s] -= g->sx[s];
        g->sh[s] -= g->sy[s];
    }
    PROF("clusters");

    /* ---- ports: spread attachment points of fan-outs/fan-ins (and parallel edges) ---- */
    double *eport = ANEW(A, double, 2 * E + 1);
    uint32_t *ioff = ANEW(A, uint32_t, N + 1), *iidx = ANEW(A, uint32_t, E + 1);
    if (!eport || !ioff || !iidx) return CMM_ERR_NOMEM;
    for (size_t i = 0; i < 2 * E; i++) eport[i] = NAN;
    memset(ioff, 0, (N + 1) * sizeof *ioff);
    for (size_t e = 0; e < E; e++) if (EVALID(e)) ioff[EV(e) + 1]++;
    for (size_t v = 0; v < N; v++) ioff[v + 1] += ioff[v];
    for (size_t v = 0; v < N; v++) it[v] = ioff[v];
    for (size_t e = 0; e < E; e++) if (EVALID(e)) iidx[it[EV(e)]++] = (uint32_t)e;
    {
        size_t maxk = 0;
        for (size_t v = 0; v < N; v++) {
            if (ooff[v + 1] - ooff[v] > maxk) maxk = ooff[v + 1] - ooff[v];
            if (ioff[v + 1] - ioff[v] > maxk) maxk = ioff[v + 1] - ioff[v];
        }
        PortKey *pk = ANEW(A, PortKey, maxk + 1);
        if (!pk) return CMM_ERR_NOMEM;
        for (unsigned side = 0; side < 2; side++)
            for (size_t u = 0; u < N; u++) {
                const uint32_t *off = side ? ioff : ooff, *lst = side ? iidx : oidx;
                size_t k = off[u + 1] - off[u];
                if (k < 2) continue;
                for (size_t i = 0; i < k; i++) {
                    uint32_t e = lst[off[u] + i];
                    int32_t span = rank[EV(e)] - rank[EU(e)];
                    int32_t nb = side ? CHAIN(e, span - 1, span) : CHAIN(e, 1, span);
                    int32_t other = side ? EU(e) : EV(e);
                    pk[i] = (PortKey){.k1 = X[nb], .k2 = X[other], .e = e};
                }
                port_sort(k, pk);
                Shape sh = g->nshape[u];
                double frac = sh == SH_RHOMBUS || sh == SH_CIRCLE || sh == SH_DCIRCLE ? 0.45 : 0.7;
                double step = dmin(EDGE_SEP * 0.6, L.w[u] * frac / (double)(k - 1));
                for (size_t i = 0; i < k; i++) eport[2 * pk[i].e + side] = ((double)i - (double)(k - 1) / 2) * step;
            }
    }
    PROF("ports");

    /* edges */
    g->pts.n = 0;
    for (size_t e = 0; e < E; e++) {
        g->eptoff[e] = (uint32_t)g->pts.n;
        g->eptn[e] = 0;
        g->elx[e] = g->ely[e] = 0;
        int32_t a = ef[e], b = et[e];
        if (a == NONE || b == NONE) continue;
        if (a == b) { /* self loop: drawn by the SVG writer; labels right of the outermost loop, stacked */
            unsigned k = 0, cnt = 0;
            for (size_t f = 0; f < E; f++)
                if (g->efrom[f] == a && g->eto[f] == a) { if (f < e) k++; cnt++; }
            double lh = g->elh[e] + 4;
            g->elx[e] = g->nx[a] + g->nw[a] / 2 + SELFLOOP_BULGE * selfloop_r(cnt - 1) + SELFLOOP_GAP + g->elw[e] / 2;
            g->ely[e] = g->ny[a] + ((double)k - (double)(cnt - 1) / 2) * lh;
            continue;
        }
        int32_t span = rank[EV(e)] - rank[EU(e)];
        size_t base = g->pts.n;
        double lx = 0, ly = 0;
        bool labset = false, fixed0 = false, fixed1 = false;
        for (int32_t k = 0; k <= span; k++) {
            int32_t vv = CHAIN(e, k, span);
            double lxx = X[vv], lyy = ry[L.rank[vv]];
            double px = FX(lxx, lyy) * sgx, py = FY(lxx, lyy) * sgy;
            if ((k == 0 || k == span) && !isnan(eport[2 * e + (k ? 1 : 0)])) {
                double tx = lxx + eport[2 * e + (k ? 1 : 0)], ty = lyy + (k ? -1 : 1) * L.h[vv] / 2;
                clip_to_node(g, vv, FX(tx, ty) * sgx, FY(tx, ty) * sgy, &px, &py);
                if (k) fixed1 = true; else fixed0 = true;
            }
            if (k > 0 && k < span && has_labels && g->elw[e] > 0 && k == span / 2) { lx = px; ly = py; labset = true; }
            if (!VEC_PUSH(A, &g->pts, px) || !VEC_PUSH(A, &g->pts, py)) return CMM_ERR_NOMEM;
        }
        size_t np = (size_t)span + 1;
        double *P = g->pts.v + base;
        if (rev[e]) {
            reverse_pts(np, P);
            bool t = fixed0; fixed0 = fixed1; fixed1 = t;
        }
        /* clip endpoints at node boundaries */
        if (!fixed0) clip_to_node(g, a, P[2], P[3], &P[0], &P[1]);
        if (!fixed1) clip_to_node(g, b, P[2 * np - 4], P[2 * np - 3], &P[2 * np - 2], &P[2 * np - 1]);
        /* edges to subgraph ids attach to the cluster box */
        for (unsigned end = 0; end < 2; end++) {
            int32_t sg = esub[2 * e + end];
            if (sg == NONE || g->sw[sg] <= 0) continue;
            if (end == 0) reverse_pts(np, P);
            np = clip_poly_box(np, P, g->sx[sg], g->sy[sg], g->sx[sg] + g->sw[sg], g->sy[sg] + g->sh[sg]);
            if (end == 0) reverse_pts(np, P);
        }
        g->pts.n = base + 2 * np;
        if (!labset) { lx = (P[0] + P[2 * np - 2]) / 2; ly = (P[1] + P[2 * np - 1]) / 2; }
        g->elx[e] = lx, g->ely[e] = ly;
        g->eptn[e] = (uint32_t)np;
    }
    PROF("edges");
#ifdef CMM_PROFILE
    fprintf(stderr, "  V=%zu N=%zu D=%zu R=%zu segs=%zu\n", V, N, D, R, nseg);
#endif
    return CMM_OK;
}

/* ---------- recursive cluster layout: subgraphs with their own `direction` ----------
 * A cluster whose direction differs from its context is laid out on its own (recursively),
 * then stands in the parent layout as one fixed-size proxy node. Edges to the cluster id
 * attach to the proxy box; edges to member nodes are extended from the box to the member. */
typedef struct Box { double x0, y0, x1, y1; } Box;
static inline void box_add(Box *b, double x0, double y0, double x1, double y1) {
    if (x0 < b->x0) b->x0 = x0;
    if (y0 < b->y0) b->y0 = y0;
    if (x1 > b->x1) b->x1 = x1;
    if (y1 > b->y1) b->y1 = y1;
}
static Box content_box(const Graph *g) {
    Box b = {INFINITY, INFINITY, -INFINITY, -INFINITY};
    for (size_t v = 0; v < g->nn; v++)
        if (!(g->nflags[v] & NF_HIDDEN))
            box_add(&b, g->nx[v] - g->nw[v] / 2, g->ny[v] - g->nh[v] / 2, g->nx[v] + g->nw[v] / 2, g->ny[v] + g->nh[v] / 2);
    for (size_t s = 0; s < g->ns; s++)
        if (g->sw[s] > 0) box_add(&b, g->sx[s], g->sy[s], g->sx[s] + g->sw[s], g->sy[s] + g->sh[s]);
    for (size_t e = 0; e < g->ne; e++) {
        const double *P = g->pts.v + g->eptoff[e];
        for (uint32_t i = 0; i < g->eptn[e]; i++) box_add(&b, P[2 * i], P[2 * i + 1], P[2 * i], P[2 * i + 1]);
        if (g->elw[e] > 0 && g->eptn[e])
            box_add(&b, g->elx[e] - g->elw[e] / 2, g->ely[e] - g->elh[e] / 2, g->elx[e] + g->elw[e] / 2, g->ely[e] + g->elh[e] / 2);
        if (g->efrom[e] == g->eto[e] && g->efrom[e] != NONE) {
            int32_t v = g->efrom[e];
            box_add(&b, g->nx[v], g->ny[v] - g->nh[v] / 2, g->nx[v] + g->nw[v] / 2 + 40 + g->elw[e], g->ny[v] + g->nh[v] / 2);
        }
    }
    return b;
}

[[nodiscard]] static bool gsub_alloc(Graph *d, const Graph *g, Dir dir, size_t nn, size_t ne, size_t ns) {
    Arena *A = g->arena;
    *d = (Graph){.arena = g->arena, .dir = dir, .theme = g->theme, .flags = g->flags, .acyclicer = g->acyclicer, .node_sep = g->node_sep,
                 .rank_sep = g->rank_sep, .font_size = g->font_size, .nn = nn, .ne = ne, .ns = ns,
                 .ncap = nn, .ecap = ne, .scap = ns};
    d->nid = ANEW(A, Str, nn + 1); d->nlabel = ANEW(A, Str, nn + 1); d->nclass = ANEW(A, Str, nn + 1);
    d->nstyle = ANEW(A, Str, nn + 1); d->nshape = ANEW(A, Shape, nn + 1); d->nparent = ANEW(A, int32_t, nn + 1);
    d->nflags = ANEW(A, uint8_t, nn + 1); d->nw = ANEW(A, double, nn + 1); d->nh = ANEW(A, double, nn + 1);
    d->efrom = ANEW(A, int32_t, ne + 1); d->eto = ANEW(A, int32_t, ne + 1); d->elabel = ANEW(A, Str, ne + 1);
    d->estroke = ANEW(A, Stroke, ne + 1); d->estart = ANEW(A, Arrow, ne + 1); d->eend = ANEW(A, Arrow, ne + 1);
    d->elen = ANEW(A, uint16_t, ne + 1); d->eid = ANEW(A, Str, ne + 1); d->eflags = ANEW(A, uint8_t, ne + 1);
    d->sid = ANEW(A, Str, ns + 1); d->stitle = ANEW(A, Str, ns + 1); d->sclass = ANEW(A, Str, ns + 1);
    d->sstyle = ANEW(A, Str, ns + 1); d->sparent = ANEW(A, int32_t, ns + 1); d->sdir = ANEW(A, Dir, ns + 1);
    d->sflags = ANEW(A, uint8_t, ns + 1);
    return d->nid && d->nlabel && d->nclass && d->nstyle && d->nshape && d->nparent && d->nflags && d->nw &&
           d->nh && d->efrom && d->eto && d->elabel && d->estroke && d->estart && d->eend && d->elen && d->eid &&
           d->eflags && d->sid && d->stitle && d->sclass && d->sstyle && d->sparent && d->sdir && d->sflags;
}

[[nodiscard]] static bool alloc_outputs(Graph *g) {
    Arena *A = g->arena;
    size_t N = g->nn, E = g->ne, S = g->ns;
    g->nw = ANEW(A, double, N + 1); g->nh = ANEW(A, double, N + 1);
    g->nx = ANEW(A, double, N + 1); g->ny = ANEW(A, double, N + 1);
    g->eptoff = ANEW(A, uint32_t, E + 1); g->eptn = ANEW(A, uint32_t, E + 1);
    g->elx = ANEW(A, double, E + 1); g->ely = ANEW(A, double, E + 1);
    g->elw = ANEW(A, double, E + 1); g->elh = ANEW(A, double, E + 1);
    g->sx = ANEW(A, double, S + 1); g->sy = ANEW(A, double, S + 1);
    g->sw = ANEW(A, double, S + 1); g->sh = ANEW(A, double, S + 1);
    return g->nw && g->nh && g->nx && g->ny && g->eptoff && g->eptn && g->elx && g->ely && g->elw && g->elh &&
           g->sx && g->sy && g->sw && g->sh;
}

static cmm_status layout_rec(Graph *g, unsigned depth) {
    size_t N = g->nn, E = g->ne, S = g->ns;
    if (!S || depth > 16) return layout_core(g);
    Arena *A = g->arena;
    int32_t *cand = ANEW(A, int32_t, S + 1), *gix = ANEW(A, int32_t, S + 1);
    if (!cand || !gix) return CMM_ERR_NOMEM;
    size_t K = 0;
    for (size_t s = 0; s < S; s++) { /* parents precede children */
        int32_t p = g->sparent[s], pc = p == NONE ? NONE : cand[p];
        gix[s] = NONE;
        if (pc != NONE) cand[s] = pc;
        else if ((g->sflags[s] & SF_DIRSET) && g->sdir[s] != g->dir) { cand[s] = (int32_t)s; gix[s] = (int32_t)++K; }
        else cand[s] = NONE;
    }
    if (!K) return layout_core(g);

    /* classify nodes: owner graph (0 = outer, k = candidate k), cluster-id endpoints */
    int32_t *owner = ANEW(A, int32_t, N + 1), *sref = ANEW(A, int32_t, N + 1), *nmap = ANEW(A, int32_t, N + 1);
    int32_t *smap = ANEW(A, int32_t, S + 1), *eown = ANEW(A, int32_t, E + 1), *emap = ANEW(A, int32_t, E + 1);
    int32_t *cands = ANEW(A, int32_t, K + 1);
    size_t *cn = ANEW(A, size_t, K + 1), *ce = ANEW(A, size_t, K + 1), *cs = ANEW(A, size_t, K + 1);
    Graph *G = ANEW(A, Graph, K + 1);
    if (!owner || !sref || !nmap || !smap || !eown || !emap || !cands || !cn || !ce || !cs || !G) return CMM_ERR_NOMEM;
    for (size_t k = 0; k <= K; k++) cn[k] = ce[k] = cs[k] = 0;
    for (size_t s = 0; s < S; s++) if (gix[s] != NONE) cands[gix[s]] = (int32_t)s;
    for (size_t v = 0; v < N; v++) {
        sref[v] = NONE;
        int32_t loc = g->nparent[v];
        if (g->nflags[v] & NF_SUBREF) {
            for (size_t s = 0; s < S; s++) if (str_eq(g->sid[s], g->nid[v])) { sref[v] = (int32_t)s; break; }
            loc = sref[v];
        }
        int32_t c = loc == NONE ? NONE : cand[loc];
        if (c == NONE) owner[v] = 0;
        else if (sref[v] == c) owner[v] = -gix[c]; /* the cluster box itself */
        else owner[v] = gix[c];
        if (owner[v] >= 0) nmap[v] = (int32_t)cn[owner[v]]++;
    }
    for (size_t k = 1; k <= K; k++) cn[0]++; /* proxies */
    for (size_t s = 0; s < S; s++) {
        int32_t k = cand[s] == NONE ? 0 : gix[cand[s]];
        smap[s] = cand[s] == (int32_t)s ? NONE : (int32_t)cs[k]++;
    }
    #define GOF(v) (owner[v] >= 0 ? owner[v] : -owner[v])
    #define INNER(v) (owner[v] > 0)
    for (size_t e = 0; e < E; e++) {
        int32_t a = g->efrom[e], b = g->eto[e];
        int32_t ga = GOF(a), gb = GOF(b);
        if (ga == gb && INNER(a) && INNER(b)) eown[e] = ga;
        else if (ga == gb && ga != 0) eown[e] = NONE; /* member <-> own cluster: no geometry */
        else eown[e] = 0;
        if (eown[e] != NONE) emap[e] = (int32_t)ce[eown[e]]++;
    }
    for (size_t k = 0; k <= K; k++)
        if (!gsub_alloc(&G[k], g, k ? g->sdir[cands[k]] : g->dir, cn[k], ce[k], cs[k])) return CMM_ERR_NOMEM;
    for (size_t v = 0; v < N; v++) {
        if (owner[v] < 0) continue;
        Graph *d = &G[owner[v]];
        int32_t i = nmap[v], p = g->nparent[v];
        d->nid[i] = g->nid[v]; d->nlabel[i] = g->nlabel[v]; d->nclass[i] = g->nclass[v];
        d->nstyle[i] = g->nstyle[v]; d->nshape[i] = g->nshape[v]; d->nflags[i] = g->nflags[v];
        d->nparent[i] = p == NONE || cand[p] == p ? NONE : smap[p];
    }
    size_t *pfill = cn; /* proxies go after the outer graph's real nodes */
    pfill[0] -= K;
    int32_t *proxy = ANEW(A, int32_t, K + 1);
    if (!proxy) return CMM_ERR_NOMEM;
    for (size_t k = 1; k <= K; k++) {
        int32_t s = cands[k], i = (int32_t)pfill[0]++, p = g->sparent[s];
        Graph *d = &G[0];
        proxy[k] = i;
        d->nid[i] = g->sid[s]; d->nlabel[i] = (Str){}; d->nclass[i] = d->nstyle[i] = (Str){};
        d->nshape[i] = SH_RECT; d->nflags[i] = NF_DEFINED | NF_FIXED;
        d->nparent[i] = p == NONE ? NONE : smap[p];
    }
    for (size_t s = 0; s < S; s++) {
        if (smap[s] == NONE) continue;
        Graph *d = &G[cand[s] == NONE ? 0 : gix[cand[s]]];
        int32_t i = smap[s], p = g->sparent[s];
        d->sid[i] = g->sid[s]; d->stitle[i] = g->stitle[s]; d->sclass[i] = g->sclass[s];
        d->sstyle[i] = g->sstyle[s]; d->sdir[i] = g->sdir[s]; d->sflags[i] = g->sflags[s];
        d->sparent[i] = p == NONE || cand[p] == p ? NONE : smap[p];
    }
    for (size_t e = 0; e < E; e++) {
        if (eown[e] == NONE) continue;
        Graph *d = &G[eown[e]];
        int32_t i = emap[e], a = g->efrom[e], b = g->eto[e];
        d->efrom[i] = owner[a] == eown[e] ? nmap[a] : proxy[GOF(a)];
        d->eto[i] = owner[b] == eown[e] ? nmap[b] : proxy[GOF(b)];
        d->elabel[i] = g->elabel[e]; d->estroke[i] = g->estroke[e]; d->estart[i] = g->estart[e];
        d->eend[i] = g->eend[e]; d->elen[i] = g->elen[e]; d->eid[i] = g->eid[e]; d->eflags[i] = g->eflags[e];
    }
    /* inner layouts first: they size the proxies */
    Box *cb = ANEW(A, Box, K + 1);
    if (!cb) return CMM_ERR_NOMEM;
    for (size_t k = 1; k <= K; k++) {
        cmm_status st = layout_rec(&G[k], depth + 1);
        if (st) return st;
        int32_t s = cands[k];
        Box b = content_box(&G[k]);
        double tw;
        unsigned tl;
        measure(g, &g->stitle[s], &tw, &tl);
        if (b.x0 == INFINITY) b = (Box){0, 0, dmax(tw, 40), 20};
        cb[k] = b;
        double w = dmax(b.x1 - b.x0, tw) + 2 * CLUSTER_PAD, h = b.y1 - b.y0 + 2 * CLUSTER_PAD + CLUSTER_TITLE;
        G[0].nw[proxy[k]] = w;
        G[0].nh[proxy[k]] = h;
    }
    cmm_status st = layout_core(&G[0]);
    if (st) return st;

    /* merge back */
    if (!alloc_outputs(g)) return CMM_ERR_NOMEM;
    double *ox = ANEW(A, double, K + 1), *oy = ANEW(A, double, K + 1);
    if (!ox || !oy) return CMM_ERR_NOMEM;
    ox[0] = oy[0] = 0;
    for (size_t k = 1; k <= K; k++) {
        int32_t s = cands[k], p = proxy[k];
        double w = G[0].nw[p], h = G[0].nh[p], x0 = G[0].nx[p] - w / 2, y0 = G[0].ny[p] - h / 2;
        g->sx[s] = x0, g->sy[s] = y0, g->sw[s] = w, g->sh[s] = h;
        ox[k] = G[0].nx[p] - (cb[k].x0 + cb[k].x1) / 2;
        oy[k] = y0 + CLUSTER_TITLE + CLUSTER_PAD - cb[k].y0;
    }
    for (size_t v = 0; v < N; v++) {
        if (owner[v] < 0) { g->nw[v] = g->nh[v] = g->nx[v] = g->ny[v] = 0; continue; }
        const Graph *d = &G[owner[v]];
        int32_t i = nmap[v];
        g->nlabel[v] = d->nlabel[i]; /* may have been wrapped */
        g->nw[v] = d->nw[i]; g->nh[v] = d->nh[i];
        g->nx[v] = d->nx[i] + ox[owner[v]]; g->ny[v] = d->ny[i] + oy[owner[v]];
    }
    for (size_t s = 0; s < S; s++) {
        if (smap[s] == NONE) continue;
        int32_t k = cand[s] == NONE ? 0 : gix[cand[s]];
        const Graph *d = &G[k];
        int32_t i = smap[s];
        g->stitle[s] = d->stitle[i];
        g->sx[s] = d->sx[i] + ox[k]; g->sy[s] = d->sy[i] + oy[k]; g->sw[s] = d->sw[i]; g->sh[s] = d->sh[i];
    }
    g->pts.n = 0;
    for (size_t e = 0; e < E; e++) {
        g->eptoff[e] = (uint32_t)g->pts.n;
        g->eptn[e] = 0;
        g->elx[e] = g->ely[e] = g->elw[e] = g->elh[e] = 0;
        int32_t k = eown[e];
        if (k == NONE) continue;
        const Graph *d = &G[k];
        int32_t i = emap[e];
        g->elabel[e] = d->elabel[i];
        g->elx[e] = d->elx[i] + ox[k]; g->ely[e] = d->ely[i] + oy[k];
        g->elw[e] = d->elw[i]; g->elh[e] = d->elh[i];
        uint32_t np = d->eptn[i];
        if (!np) continue;
        const double *P = d->pts.v + d->eptoff[i];
        int32_t a = g->efrom[e], b = g->eto[e];
        bool exta = k == 0 && INNER(a) && !(g->nflags[a] & NF_SUBREF);
        bool extb = k == 0 && INNER(b) && !(g->nflags[b] & NF_SUBREF);
        if (exta) {
            double px, py;
            clip_to_node(g, a, P[0], P[1], &px, &py);
            if (!VEC_PUSH(A, &g->pts, px) || !VEC_PUSH(A, &g->pts, py)) return CMM_ERR_NOMEM;
        }
        for (uint32_t j = 0; j < np; j++)
            if (!VEC_PUSH(A, &g->pts, P[2 * j] + ox[k]) || !VEC_PUSH(A, &g->pts, P[2 * j + 1] + oy[k])) return CMM_ERR_NOMEM;
        if (extb) {
            double px, py;
            clip_to_node(g, b, P[2 * np - 2], P[2 * np - 1], &px, &py);
            if (!VEC_PUSH(A, &g->pts, px) || !VEC_PUSH(A, &g->pts, py)) return CMM_ERR_NOMEM;
        }
        g->eptn[e] = (uint32_t)((g->pts.n - g->eptoff[e]) / 2);
    }
    #undef GOF
    #undef INNER
    return CMM_OK;
}

/* ---------- post passes on the final geometry ---------- */
/* segment p->q vs box: entry parameter in [0,1] or -1 */
static double seg_box(double px, double py, double qx, double qy, const Box *b) {
    double t0 = 0, t1 = 1, d[2] = {qx - px, qy - py}, p0[2] = {px, py}, lo[2] = {b->x0, b->y0}, hi[2] = {b->x1, b->y1};
    for (unsigned k = 0; k < 2; k++) {
        if (fabs(d[k]) < 1e-12) { if (p0[k] < lo[k] || p0[k] > hi[k]) return -1; continue; }
        double id = 1 / d[k], ta = (lo[k] - p0[k]) * id, tb = (hi[k] - p0[k]) * id;
        if (ta > tb) { double t = ta; ta = tb; tb = t; }
        if (ta > t0) t0 = ta;
        if (tb < t1) t1 = tb;
        if (t0 > t1) return -1;
    }
    return t0;
}

/* ---- obstacle-avoiding re-route (default on; CMM_F_NO_AVOID disables) ----
 * Edges whose polyline passes through a non-incident node get the offending stretch replaced by a
 * shortest path in a local visibility graph over inflated node corners. A uniform grid indexes the
 * boxes; total work is budgeted so huge graphs degrade to the plain layout route. */
typedef struct Grid { double x0, y0, cs, ics; size_t gw, gh; uint32_t *off, *idx; } Grid;

[[nodiscard]] static bool grid_build(Arena *A, Grid *G, size_t N, const Box nb[static N + 1], const bool *use) {
    double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY, area = 0;
    size_t k = 0;
    for (size_t v = 0; v < N; v++) {
        if (!use[v]) continue;
        x0 = dmin(x0, nb[v].x0), y0 = dmin(y0, nb[v].y0), x1 = dmax(x1, nb[v].x1), y1 = dmax(y1, nb[v].y1);
        area += (nb[v].x1 - nb[v].x0) * (nb[v].y1 - nb[v].y0), k++;
    }
    if (!k) { G->gw = G->gh = 0; return true; }
    double cs = dmax(sqrt(area / (double)k) * 2, 32);
    size_t gw = (size_t)((x1 - x0) / cs) + 1, gh = (size_t)((y1 - y0) / cs) + 1;
    while (gw * gh > 4 * k + 64) cs *= 1.5, gw = (size_t)((x1 - x0) / cs) + 1, gh = (size_t)((y1 - y0) / cs) + 1;
    *G = (Grid){x0, y0, cs, 1 / cs, gw, gh, ANEW(A, uint32_t, gw * gh + 1), nullptr};
    if (!G->off) return false;
    memset(G->off, 0, (gw * gh + 1) * sizeof *G->off);
    size_t tot = 0;
    for (unsigned pass = 0; pass < 2; pass++) {
        for (size_t v = 0; v < N; v++) {
            if (!use[v]) continue;
            size_t cx0 = (size_t)((nb[v].x0 - x0) / cs), cx1 = (size_t)((nb[v].x1 - x0) / cs);
            size_t cy0 = (size_t)((nb[v].y0 - y0) / cs), cy1 = (size_t)((nb[v].y1 - y0) / cs);
            for (size_t cy = cy0; cy <= cy1 && cy < gh; cy++)
                for (size_t cx = cx0; cx <= cx1 && cx < gw; cx++) {
                    if (!pass) { G->off[cy * gw + cx + 1]++, tot++; }
                    else G->idx[G->off[cy * gw + cx]++] = (uint32_t)v;
                }
        }
        if (!pass) {
            for (size_t c = 0; c < gw * gh; c++) G->off[c + 1] += G->off[c];
            G->idx = ANEW(A, uint32_t, tot + 1);
            if (!G->idx) return false;
        } else {
            for (size_t c = gw * gh; c > 0; c--) G->off[c] = G->off[c - 1];
            G->off[0] = 0;
        }
    }
    return true;
}

typedef struct Router {
    const Graph *g;
    const Box *nb;
    Grid G;
    uint32_t *stamp; /* per-node visit stamp for de-duplicating grid hits */
    uint32_t cur;
    int64_t budget;
} Router;

/* first obstacle (other than skip1/skip2) whose interior the segment p->q enters; NONE if clear */
static int32_t seg_hit_m(Router *R, double px, double py, double qx, double qy, int32_t s1, int32_t s2, double sh) {
    const Grid *G = &R->G;
    if (!G->gw) return NONE;
    double lx = dmin(px, qx), hx = dmax(px, qx), ly = dmin(py, qy), hy = dmax(py, qy);
    if (hx < G->x0 || hy < G->y0) return NONE;
    size_t cx0 = lx < G->x0 ? 0 : (size_t)((lx - G->x0) * G->ics), cy0 = ly < G->y0 ? 0 : (size_t)((ly - G->y0) * G->ics);
    size_t cx1 = (size_t)((hx - G->x0) * G->ics), cy1 = (size_t)((hy - G->y0) * G->ics);
    if (cx1 >= G->gw) cx1 = G->gw - 1;
    if (cy1 >= G->gh) cy1 = G->gh - 1;
    if (++R->cur == 0) { memset(R->stamp, 0, R->g->nn * sizeof *R->stamp); R->cur = 1; }
    int32_t hit = NONE;
    double bt = 2;
    double dy = qy - py, idy = fabs(dy) > 1e-12 ? 1 / dy : 0;
    for (size_t cy = cy0; cy <= cy1; cy++) {
        /* x-extent of the segment inside this row band: only those cells can be touched */
        size_t ax = cx0, bx = cx1;
        if (cy0 != cy1 && fabs(dy) > 1e-12) {
            double y0 = G->y0 + (double)cy * G->cs, y1 = y0 + G->cs;
            double t0 = (y0 - py) * idy, t1 = (y1 - py) * idy;
            if (t0 > t1) { double tt = t0; t0 = t1; t1 = tt; }
            t0 = dmax(t0, 0), t1 = dmin(t1, 1);
            double xa = px + (qx - px) * t0, xb = px + (qx - px) * t1;
            double la = dmin(xa, xb) - 1, lb = dmax(xa, xb) + 1;
            size_t na = la < G->x0 ? 0 : (size_t)((la - G->x0) * G->ics);
            size_t nb2 = lb < G->x0 ? 0 : (size_t)((lb - G->x0) * G->ics);
            if (na > ax) ax = na;
            if (nb2 < bx) bx = nb2;
        }
        for (size_t cx = ax; cx <= bx; cx++)
            for (uint32_t k = G->off[cy * G->gw + cx]; k < G->off[cy * G->gw + cx + 1]; k++) {
                uint32_t v = G->idx[k];
                if (R->stamp[v] == R->cur || (int32_t)v == s1 || (int32_t)v == s2) continue;
                R->stamp[v] = R->cur;
                R->budget--;
                Box b = R->nb[v];
                b.x0 += sh, b.y0 += sh, b.x1 -= sh, b.y1 -= sh; /* grazing along a side is fine */
                if (hx < b.x0 || lx > b.x1 || hy < b.y0 || ly > b.y1) continue;
                double t = seg_box(px, py, qx, qy, &b);
                if (t >= 0 && t < bt) return (int32_t)v; /* any hit suffices */
            }
    }
    return hit;
}

static int32_t seg_hit(Router *R, double px, double py, double qx, double qy, int32_t s1, int32_t s2) {
    return seg_hit_m(R, px, py, qx, qy, s1, s2, 0.5);
}

static bool pt_in(const Box *b, double x, double y) { return x > b->x0 && x < b->x1 && y > b->y0 && y < b->y1; }

/* is (x,y) strictly inside some obstacle other than s1/s2? (one grid cell, no segment test) */
static bool pt_buried(const Router *R, double x, double y, int32_t s1, int32_t s2) {
    const Grid *G = &R->G;
    if (x < G->x0 || y < G->y0) return false;
    size_t cx = (size_t)((x - G->x0) / G->cs), cy = (size_t)((y - G->y0) / G->cs);
    if (cx >= G->gw || cy >= G->gh) return false;
    for (uint32_t k = G->off[cy * G->gw + cx]; k < G->off[cy * G->gw + cx + 1]; k++) {
        int32_t v = (int32_t)G->idx[k];
        if (v != s1 && v != s2 && pt_in(&R->nb[v], x, y)) return true;
    }
    return false;
}

/* shortest visibility path S->T; writes interior corners to out (max cap pts). Returns count or -1. */
static inline double edist(double dx, double dy) { return sqrt(dx * dx + dy * dy); }

/* indexed binary min-heap over A* keys f = dist + h (ties by index, matching a linear argmin scan) */
typedef struct VHeap { size_t n; int32_t *h, *at; const double *dist, *hh; } VHeap;
static inline bool vh_lt(const VHeap *H, int32_t a, int32_t b) {
    double fa = H->dist[a] + H->hh[a], fb = H->dist[b] + H->hh[b];
    return fa < fb || (fa == fb && a < b);
}
static void vh_swap(VHeap *H, size_t i, size_t j) {
    int32_t t = H->h[i]; H->h[i] = H->h[j], H->h[j] = t;
    H->at[H->h[i]] = (int32_t)i, H->at[H->h[j]] = (int32_t)j;
}
static void vh_fix(VHeap *H, size_t i) {
    while (i && vh_lt(H, H->h[i], H->h[(i - 1) / 2])) vh_swap(H, i, (i - 1) / 2), i = (i - 1) / 2;
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < H->n && vh_lt(H, H->h[l], H->h[m])) m = l;
        if (r < H->n && vh_lt(H, H->h[r], H->h[m])) m = r;
        if (m == i) return;
        vh_swap(H, i, m), i = m;
    }
}
static void vh_set(VHeap *H, int32_t v) { /* insert or re-key */
    if (H->at[v] < 0) { H->h[H->n] = v, H->at[v] = (int32_t)H->n, H->n++; }
    vh_fix(H, (size_t)H->at[v]);
}
static void vh_del(VHeap *H, int32_t v) {
    size_t i = (size_t)H->at[v];
    H->n--;
    if (i != H->n) { H->h[i] = H->h[H->n], H->at[H->h[i]] = (int32_t)i; }
    H->at[v] = -1;
    if (i < H->n) vh_fix(H, i);
}

static int vis_path(Router *R, Arena *A, double sx, double sy, double tx, double ty, int32_t s1, int32_t s2,
                    size_t cap, double out[static 2 * cap]) {
    const Graph *g = R->g;
    double pad = 0;
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        pad = attempt == 0 ? 24 : pad * 3;
        double lx = dmin(sx, tx) - pad, hx = dmax(sx, tx) + pad, ly = dmin(sy, ty) - pad, hy = dmax(sy, ty) + pad;
        /* candidate corners */
        constexpr size_t MAXC = 96;
        double cx[MAXC + 2], cy[MAXC + 2];
        size_t nc = 0;
        cx[nc] = sx, cy[nc] = sy, nc++;
        cx[nc] = tx, cy[nc] = ty, nc++;
        const Grid *G = &R->G;
        size_t gx0 = lx < G->x0 ? 0 : (size_t)((lx - G->x0) / G->cs), gy0 = ly < G->y0 ? 0 : (size_t)((ly - G->y0) / G->cs);
        size_t gx1 = hx < G->x0 ? 0 : (size_t)((hx - G->x0) / G->cs), gy1 = hy < G->y0 ? 0 : (size_t)((hy - G->y0) / G->cs);
        if (gx1 >= G->gw) gx1 = G->gw - 1;
        if (gy1 >= G->gh) gy1 = G->gh - 1;
        if (++R->cur == 0) { memset(R->stamp, 0, g->nn * sizeof *R->stamp); R->cur = 1; }
        bool full = false;
        for (size_t yy = gy0; yy <= gy1 && !full; yy++)
            for (size_t xx = gx0; xx <= gx1 && !full; xx++)
                for (uint32_t k = G->off[yy * G->gw + xx]; k < G->off[yy * G->gw + xx + 1]; k++) {
                    uint32_t v = G->idx[k];
                    if (R->stamp[v] == R->cur || (int32_t)v == s1 || (int32_t)v == s2) continue;
                    R->stamp[v] = R->cur;
                    const Box *b = &R->nb[v];
                    if (b->x1 < lx || b->x0 > hx || b->y1 < ly || b->y0 > hy) continue;
                    if (nc + 4 > MAXC + 2) { full = true; break; }
                    double xs[2] = {b->x0, b->x1}, ys[2] = {b->y0, b->y1};
                    for (unsigned c = 0; c < 4; c++) cx[nc] = xs[c & 1], cy[nc] = ys[c >> 1], nc++;
                }
        /* drop corners buried in another obstacle */
        size_t m = 2;
        for (size_t i = 2; i < nc; i++) {
            if (!pt_buried(R, cx[i], cy[i], s1, s2)) cx[m] = cx[i], cy[m] = cy[i], m++;
        }
        nc = m;
        /* A* over the complete visibility graph, binary heap, lazy visibility */
        double *dist = ANEW(A, double, nc);
        int32_t *prev = ANEW(A, int32_t, nc);
        uint8_t *done = ANEW(A, uint8_t, nc);
        if (!dist || !prev || !done) return -1;
        for (size_t i = 0; i < nc; i++) dist[i] = INFINITY, prev[i] = -1, done[i] = 0;
        dist[0] = 0;
        /* A*: Euclidean distance to T is consistent on a visibility graph */
        double *hh = ANEW(A, double, nc);
        if (!hh) return -1;
        for (size_t i = 0; i < nc; i++) hh[i] = edist(cx[i] - tx, cy[i] - ty);
        VHeap H = {0, ANEW(A, int32_t, nc), ANEW(A, int32_t, nc), dist, hh};
        if (!H.h || !H.at) return -1;
        for (size_t i = 0; i < nc; i++) H.at[i] = -1;
        vh_set(&H, 0);
        bool reached = false;
        /* lazy A*: relax without visibility tests, validate a node's parent link only when it is popped */
        for (;;) {
            if (!H.n) break;
            size_t u = (size_t)H.h[0];
            if (u != 0 && !done[u]) {
                int32_t p = prev[u];
                if (R->budget < 0) return -1;
                if (seg_hit(R, cx[p], cy[p], cx[u], cy[u], s1, s2) != NONE) {
                    /* parent invisible: best visible already-settled parent, else drop for now */
                    /* candidates by tentative distance: the first visible one is the best (fewest tests) */
                    double cd[MAXC + 2];
                    int32_t cq[MAXC + 2];
                    size_t ncand = 0;
                    for (size_t q = 0; q < nc; q++) {
                        if (done[q] != 1 || (int32_t)q == p) continue;
                        double d = dist[q] + edist(cx[u] - cx[q], cy[u] - cy[q]);
                        size_t j = ncand++;
                        while (j && (cd[j - 1] > d || (cd[j - 1] == d && cq[j - 1] > (int32_t)q))) cd[j] = cd[j - 1], cq[j] = cq[j - 1], j--;
                        cd[j] = d, cq[j] = (int32_t)q;
                    }
                    double nd = INFINITY;
                    int32_t np2 = -1;
                    for (size_t i = 0; i < ncand; i++)
                        if (seg_hit(R, cx[cq[i]], cy[cq[i]], cx[u], cy[u], s1, s2) == NONE) { nd = cd[i], np2 = cq[i]; break; }
                    dist[u] = nd, prev[u] = np2;
                    if (np2 < 0) { done[u] = 2; vh_del(&H, (int32_t)u); continue; } /* unreachable via settled nodes */
                    vh_fix(&H, (size_t)H.at[u]);
                    continue; /* re-select with the corrected key */
                }
            }
            if (u == 1) { reached = true; break; }
            done[u] = 1;
            vh_del(&H, (int32_t)u);
            double du = dist[u];
            for (size_t w = 1; w < nc; w++) {
                if (done[w] == 1) continue;
                double d = du + edist(cx[w] - cx[u], cy[w] - cy[u]);
                if (done[w] == 0 && d >= dist[w]) continue;
                dist[w] = d, prev[w] = (int32_t)u, done[w] = 0;
                vh_set(&H, (int32_t)w);
            }
        }
        if (!reached) { if (full) break; continue; }
        size_t k = 0;
        for (int32_t v = prev[1]; v > 0; v = prev[v]) k++;
        if (k > cap) return -1;
        size_t j = k;
        for (int32_t v = prev[1]; v > 0; v = prev[v]) { j--; out[2 * j] = cx[v], out[2 * j + 1] = cy[v]; }
        return (int)k;
    }
    return -1;
}

/* cheap detour: hug one side of the single blocking box (2 corners). Returns corner count or -1. */
static int side_path(Router *R, double sx, double sy, double tx, double ty, int32_t s1, int32_t s2,
                     double out[static 4]) {
    int32_t h = seg_hit(R, sx, sy, tx, ty, s1, s2);
    if (h == NONE) return 0;
    const Box *b = &R->nb[h];
    if (pt_in(b, sx, sy) || pt_in(b, tx, ty)) return -1;
    double c[4][4] = {
        {b->x0, b->y0, b->x0, b->y1}, {b->x1, b->y0, b->x1, b->y1}, /* left, right side */
        {b->x0, b->y0, b->x1, b->y0}, {b->x0, b->y1, b->x1, b->y1}, /* top, bottom side */
    };
    double len[4];
    unsigned ord[4] = {0, 1, 2, 3};
    for (unsigned k = 0; k < 4; k++) {
        double *q = c[k];
        /* visit the corner nearer S first */
        if (edist(q[0] - sx, q[1] - sy) > edist(q[2] - sx, q[3] - sy)) {
            double t0 = q[0], t1 = q[1];
            q[0] = q[2], q[1] = q[3], q[2] = t0, q[3] = t1;
        }
        len[k] = edist(q[0] - sx, q[1] - sy) + edist(q[2] - q[0], q[3] - q[1]) + edist(tx - q[2], ty - q[3]);
    }
    for (unsigned i = 1; i < 4; i++) /* stable insertion sort by length: first clear one is the best */
        for (unsigned j = i; j > 0 && len[ord[j]] < len[ord[j - 1]]; j--) { unsigned t = ord[j]; ord[j] = ord[j - 1]; ord[j - 1] = t; }
    int bk = -1;
    for (unsigned i = 0; i < 4 && bk < 0; i++) {
        const double *q = c[ord[i]];
        if (seg_hit(R, sx, sy, q[0], q[1], s1, s2) != NONE || seg_hit(R, q[0], q[1], q[2], q[3], s1, s2) != NONE ||
            seg_hit(R, q[2], q[3], tx, ty, s1, s2) != NONE) continue;
        bk = (int)ord[i];
    }
    if (bk < 0) return -1;
    memcpy(out, c[bk], sizeof c[bk]);
    return 2;
}

[[nodiscard]] static bool route_avoid(Graph *g) {
    Arena *A = g->arena;
    size_t N = g->nn, E = g->ne;
    constexpr double M = 10;
    Box *nb = ANEW(A, Box, N + 1);
    bool *use = ANEW(A, bool, N + 1);
    uint32_t *stamp = ANEW(A, uint32_t, N + 1);
    if (!nb || !use || !stamp) return false;
    memset(stamp, 0, (N + 1) * sizeof *stamp);
    for (size_t v = 0; v < N; v++) {
        use[v] = !(g->nflags[v] & NF_HIDDEN) && g->nw[v] > 0 && g->nh[v] > 0;
        nb[v] = (Box){g->nx[v] - g->nw[v] / 2 - M, g->ny[v] - g->nh[v] / 2 - M, g->nx[v] + g->nw[v] / 2 + M, g->ny[v] + g->nh[v] / 2 + M};
    }
    Router R = {.g = g, .nb = nb, .stamp = stamp, .budget = 4000000 + 200 * (int64_t)(N + E)};
    if (!grid_build(A, &R.G, N, nb, use)) return false;
    if (!R.G.gw) return true;
    VEC(double) out = {}; /* points of detoured edges only; untouched edges keep theirs */
    uint8_t *moved = ANEW(A, uint8_t, E + 1);
    if (!moved) return false;
    memset(moved, 0, E + 1);
    for (size_t e = 0; e < E; e++) {
        uint32_t np = g->eptn[e];
        const double *P = g->pts.v + g->eptoff[e];
        size_t base = out.n;
        int32_t a = g->efrom[e], b = g->eto[e];
        /* find first / last offending segment */
        size_t fi = SIZE_MAX, li = 0;
        for (uint32_t i = 0; i + 1 < np && R.budget > 0; i++)
            if (seg_hit_m(&R, P[2 * i], P[2 * i + 1], P[2 * i + 2], P[2 * i + 3], a, b, M - 3) != NONE) {
                if (fi == SIZE_MAX) fi = i;
                li = i;
            }
        double tmp[2 * 64];
        int k = -1;
        if (fi != SIZE_MAX) {
            /* endpoints of the detour: step back to points that are themselves outside obstacles */
            k = side_path(&R, P[2 * fi], P[2 * fi + 1], P[2 * li + 2], P[2 * li + 3], a, b, tmp);
            if (k < 0) k = vis_path(&R, A, P[2 * fi], P[2 * fi + 1], P[2 * li + 2], P[2 * li + 3], a, b, 64, tmp);
        }
        if (k < 0) continue;
        {
            for (size_t i = 0; i <= fi; i++)
                if (!VEC_PUSH(A, &out, P[2 * i]) || !VEC_PUSH(A, &out, P[2 * i + 1])) return false;
            for (int i = 0; i < k; i++)
                if (!VEC_PUSH(A, &out, tmp[2 * i]) || !VEC_PUSH(A, &out, tmp[2 * i + 1])) return false;
            for (size_t i = li + 1; i < np; i++)
                if (!VEC_PUSH(A, &out, P[2 * i]) || !VEC_PUSH(A, &out, P[2 * i + 1])) return false;
        }
        g->eptoff[e] = (uint32_t)base;
        g->eptn[e] = (uint32_t)((out.n - base) / 2);
        moved[e] = 1;
    }
    if (!out.n) return true;
    /* rebuild in edge order (later passes compact in place and rely on it) */
    size_t tot = 0;
    for (size_t e = 0; e < E; e++) tot += 2 * (size_t)g->eptn[e];
    double *nv = ANEW(A, double, tot + 1);
    if (!nv) return false;
    size_t w = 0;
    for (size_t e = 0; e < E; e++) {
        const double *src = (moved[e] ? out.v : g->pts.v) + g->eptoff[e];
        memcpy(nv + w, src, 2 * (size_t)g->eptn[e] * sizeof *nv);
        g->eptoff[e] = (uint32_t)w;
        w += 2 * (size_t)g->eptn[e];
    }
    g->pts.v = nv, g->pts.n = w, g->pts.cap = tot + 1;
    return true;
}

/* drop interior polyline points collinear with their neighbours (dummy-node runs) */
static void simplify_pts(Graph *g) {
    size_t w = 0;
    double *V = g->pts.v;
    for (size_t e = 0; e < g->ne; e++) {
        uint32_t np = g->eptn[e], off = g->eptoff[e];
        size_t base = w;
        for (uint32_t i = 0; i < np; i++) {
            double x = V[off + 2 * i], y = V[off + 2 * i + 1];
            size_t k = (w - base) / 2;
            if (k >= 1 && fabs(x - V[w - 2]) < 1e-6 && fabs(y - V[w - 1]) < 1e-6) continue; /* duplicate */
            if (k >= 2) {
                double ax = V[w - 4], ay = V[w - 3], bx = V[w - 2], by = V[w - 1];
                double cr = (bx - ax) * (y - by) - (by - ay) * (x - bx);
                double dt = (bx - ax) * (x - bx) + (by - ay) * (y - by);
                double l2 = edist(bx - ax, by - ay) * edist(x - bx, y - by);
                if (dt > 0 && fabs(cr) <= 1e-3 * l2) { V[w - 2] = x, V[w - 1] = y; continue; } /* extend run */
            }
            V[w++] = x, V[w++] = y;
        }
        g->eptoff[e] = (uint32_t)base;
        g->eptn[e] = (uint32_t)((w - base) / 2);
    }
    g->pts.n = w;
}

/* move edge labels onto the drawn curve (B-spline of the polyline), nearest point to the reserved slot */
static bool lab_overlap(const Graph *g, size_t a, size_t b) {
    return fabs(g->elx[a] - g->elx[b]) * 2 < g->elw[a] + g->elw[b] + 4 &&
           fabs(g->ely[a] - g->ely[b]) * 2 < g->elh[a] + g->elh[b] + 2;
}

static void labels_on_curve(Graph *g) {
    size_t E = g->ne;
    double *ox = ANEW(g->arena, double, 2 * E + 2);
    if (ox) for (size_t e = 0; e < E; e++) ox[2 * e] = g->elx[e], ox[2 * e + 1] = g->ely[e];
    for (size_t e = 0; e < g->ne; e++) {
        uint32_t np = g->eptn[e];
        if (!g->elabel[e].n || np < 3) continue;
        const double *P = g->pts.v + g->eptoff[e];
        double lx = g->elx[e], ly = g->ely[e], bx = lx, by = ly, bd = INFINITY;
        /* curveBasis: segment i (1..np-2 interior) uses points i-1,i,i+1 with endpoints clamped */
        for (uint32_t i = 0; i + 2 < np; i++) {
            const double *p0 = P + 2 * i, *p1 = p0 + 2, *p2 = p0 + 4;
            /* quadratic B-spline piece between midpoints (start/end pieces anchored at the ends) */
            double sx = i == 0 ? p0[0] : (p0[0] + p1[0]) / 2, sy = i == 0 ? p0[1] : (p0[1] + p1[1]) / 2;
            double ex = i + 3 == np ? p2[0] : (p1[0] + p2[0]) / 2, ey = i + 3 == np ? p2[1] : (p1[1] + p2[1]) / 2;
            for (unsigned s = 0; s <= 8; s++) {
                double t = s / 8.0, u = 1 - t;
                double x = u * u * sx + 2 * u * t * p1[0] + t * t * ex, y = u * u * sy + 2 * u * t * p1[1] + t * t * ey;
                double d = (x - lx) * (x - lx) + (y - ly) * (y - ly);
                if (d < bd) bd = d, bx = x, by = y;
            }
        }
        g->elx[e] = bx, g->ely[e] = by;
    }
    if (!ox) return;
    /* labelled edges are few; resolve overlaps the snap introduced (e.g. A->B / B->A pairs) */
    size_t *L = ANEW(g->arena, size_t, E + 1), nl = 0;
    if (!L) return;
    for (size_t e = 0; e < E; e++) if (g->elabel[e].n) L[nl++] = e;
    if (nl > 4000) return;
    for (size_t i = 0; i < nl; i++)
        for (size_t j = i + 1; j < nl; j++) {
            size_t a = L[i], b = L[j];
            if (!lab_overlap(g, a, b)) continue;
            g->elx[a] = ox[2 * a], g->ely[a] = ox[2 * a + 1];
            g->elx[b] = ox[2 * b], g->ely[b] = ox[2 * b + 1];
            if (!lab_overlap(g, a, b)) continue;
            double need = (g->elw[a] + g->elw[b]) / 2 + 4 - fabs(g->elx[a] - g->elx[b]);
            double sgn = g->elx[a] < g->elx[b] || (g->elx[a] == g->elx[b] && a < b) ? -1 : 1;
            g->elx[a] += sgn * need / 2, g->elx[b] -= sgn * need / 2;
        }
}

/* place each cluster title where no edge crosses the title band (prefer centred) */
[[nodiscard]] static bool place_titles(Graph *g) {
    size_t S = g->ns, E = g->ne;
    g->stx = ANEW(g->arena, double, S + 1);
    if (!g->stx) return false;
    for (size_t s = 0; s < S; s++) {
        double mid = g->sx[s] + g->sw[s] / 2;
        g->stx[s] = mid;
        if (g->sw[s] <= 0 || !g->stitle[s].n) continue;
        double tw;
        unsigned tl;
        measure(g, &g->stitle[s], &tw, &tl);
        double half = tw / 2 + 6, lo = g->sx[s] + half, hi = g->sx[s] + g->sw[s] - half;
        if (hi < lo) continue;
        double y0 = g->sy[s] + 4, y1 = g->sy[s] + CLUSTER_TITLE + 8;
        double best = mid, bestd = INFINITY;
        /* crossing x-intervals; candidates: centred, then just beside each crossing */
        double cand[64];
        double iv[2 * 64];
        size_t ni = 0, nc = 0;
        cand[nc++] = mid;
        for (size_t e = 0; e < E && ni < 64; e++) {
            for (uint32_t i = 0; i + 1 < g->eptn[e] && ni < 64; i++) {
                const double *P = g->pts.v + g->eptoff[e] + 2 * i;
                double ya = P[1], yb = P[3];
                if (dmax(ya, yb) < y0 || dmin(ya, yb) > y1) continue;
                double xa = P[0], xb = P[2];
                if (fabs(yb - ya) > 1e-9) { /* restrict to the band */
                    double ta = (y0 - ya) / (yb - ya), tb = (y1 - ya) / (yb - ya);
                    ta = dmin(dmax(ta, 0), 1), tb = dmin(dmax(tb, 0), 1);
                    double x1 = xa + (xb - xa) * ta, x2 = xa + (xb - xa) * tb;
                    xa = x1, xb = x2;
                }
                double l = dmin(xa, xb) - 3, r = dmax(xa, xb) + 3;
                if (r < g->sx[s] || l > g->sx[s] + g->sw[s]) continue;
                iv[2 * ni] = l, iv[2 * ni + 1] = r, ni++;
                if (nc + 2 <= countof(cand)) { cand[nc++] = l - half; cand[nc++] = r + half; }
            }
        }
        if (!ni) continue;
        for (size_t c = 0; c < nc; c++) {
            double x = dmin(dmax(cand[c], lo), hi);
            bool free_ = true;
            for (size_t i = 0; i < ni && free_; i++) if (x + half > iv[2 * i] && x - half < iv[2 * i + 1]) free_ = false;
            if (free_ && fabs(x - mid) < bestd) { bestd = fabs(x - mid); best = x; }
        }
        g->stx[s] = best;
    }
    return true;
}

cmm_status flow_layout(Graph *g) {
    cmm_status st = layout_rec(g, 0);
    if (st) return st;
    PROF("");
    simplify_pts(g); /* fewer segments for the obstacle test */
    if (!(g->flags & CMM_F_NO_AVOID)) {
        if (!route_avoid(g)) return CMM_ERR_NOMEM;
        PROF("route");
        simplify_pts(g);
    }
    labels_on_curve(g);
    if (!place_titles(g)) return CMM_ERR_NOMEM;
    PROF("post");
    return CMM_OK;
}
