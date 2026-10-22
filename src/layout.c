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
    case SH_CIRCLE: w = h = fmax(tw, th) + 2 * PAD_NODE; break;
    case SH_DCIRCLE: w = h = fmax(tw, th) + 2 * PAD_NODE + 10; break;
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
    double nodesep;
    double cpad;
} Lay;

/* ---------- sorting helpers ---------- */
typedef struct Key {
    double bc[MAX_KEY_DEPTH];
    int32_t id[MAX_KEY_DEPTH];
    uint32_t len;
} Key;

static inline int key_cmp(const Key *a, const Key *b) {
    uint32_t n = a->len < b->len ? a->len : b->len;
    for (uint32_t i = 0; i < n; i++) {
        if (a->bc[i] < b->bc[i]) return -1;
        if (a->bc[i] > b->bc[i]) return 1;
        if (a->id[i] != b->id[i]) return a->id[i] < b->id[i] ? -1 : 1;
    }
    return (int)a->len - (int)b->len;
}

/* stable merge sort of idx[0..n) by keys[idx] */
static void msort(size_t n, uint32_t idx[static n], uint32_t tmp[static n], const Key keys[]) {
    for (size_t i = 1; i < n && n <= 16; i++) { /* insertion for tiny */
        uint32_t t = idx[i];
        size_t j = i;
        while (j && key_cmp(&keys[idx[j - 1]], &keys[t]) > 0) { idx[j] = idx[j - 1]; j--; }
        idx[j] = t;
    }
    if (n <= 16) return;
    for (size_t wdt = 1; wdt < n; wdt *= 2) {
        for (size_t lo = 0; lo < n; lo += 2 * wdt) {
            size_t mid = lo + wdt < n ? lo + wdt : n, hi = lo + 2 * wdt < n ? lo + 2 * wdt : n;
            size_t a = lo, b = mid, k = lo;
            while (a < mid && b < hi) tmp[k++] = key_cmp(&keys[idx[b]], &keys[idx[a]]) < 0 ? idx[b++] : idx[a++];
            while (a < mid) tmp[k++] = idx[a++];
            while (b < hi) tmp[k++] = idx[b++];
        }
        memcpy(idx, tmp, n * sizeof *idx);
    }
}

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
    {
        Graph *g = L->g;
        for (size_t i = 0; i < n; i++) {
            int32_t v = L->lay[a + i];
            if (L->border[v]) continue;
            for (int32_t c = L->cl[v]; c != NONE; c = g->sparent[c]) { csum[c] += L->bc[v]; ccnt[c]++; }
        }
    }
    for (size_t i = 0; i < n; i++) {
        int32_t v = L->lay[a + i];
        Key *k = &keys[i];
        uint32_t d = 0;
        if (clusters) {
            int32_t chain[64];
            uint32_t cn = 0;
            for (int32_t c = L->cl[v]; c != NONE && cn < countof(chain); c = L->g->sparent[c]) chain[cn++] = c;
            while (cn && d < MAX_KEY_DEPTH - 1) {
                int32_t c = chain[--cn];
                k->bc[d] = ccnt[c] ? csum[c] / ccnt[c] : 0;
                k->id[d] = -(c + 1);
                d++;
            }
        }
        k->bc[d] = L->bc[v];
        k->id[d] = v;
        k->len = d + 1;
        idx[i] = (uint32_t)i;
    }
    if (clusters) {
        Graph *g = L->g;
        for (size_t i = 0; i < n; i++) {
            int32_t v = L->lay[a + i];
            for (int32_t c = L->cl[v]; c != NONE; c = g->sparent[c]) { csum[c] = 0; ccnt[c] = 0; }
        }
    }
    msort(n, idx, tmp, keys);
    for (size_t i = 0; i < n; i++) tmp[i] = (uint32_t)L->lay[a + idx[i]];
    for (size_t i = 0; i < n; i++) {
        L->lay[a + i] = (int32_t)tmp[i];
        L->pos[tmp[i]] = (int32_t)i;
    }
}

/* ---------- transpose: adjacent swaps that strictly reduce crossings ---------- */
static uint64_t pair_cost(const Lay *L, int32_t u, int32_t v) {
    uint64_t c = 0;
    for (unsigned side = 0; side < 2; side++) {
        const uint32_t *off = side ? L->soff : L->poff, *adj = side ? L->sidx : L->pidx;
        for (uint32_t i = off[u]; i < off[u + 1]; i++) {
            int32_t pa = L->pos[adj[i]];
            for (uint32_t j = off[v]; j < off[v + 1]; j++) c += pa > L->pos[adj[j]];
        }
    }
    return c;
}

/* returns crossings removed; *budget bounds the pair-cost work (early stop on huge graphs) */
static uint64_t transpose(Lay *L, int64_t *budget) {
    uint64_t gain = 0;
    for (unsigned pass = 0; pass < 6; pass++) {
        bool improved = false;
        for (size_t r = 0; r < L->R; r++) {
            for (size_t i = L->loff[r]; i + 1 < L->loff[r + 1]; i++) {
                int32_t u = L->lay[i], v = L->lay[i + 1];
                if (L->border[u] || L->border[v] || L->cl[u] != L->cl[v]) continue;
                uint32_t du = (L->poff[u + 1] - L->poff[u]) + (L->soff[u + 1] - L->soff[u]);
                uint32_t dv = (L->poff[v + 1] - L->poff[v]) + (L->soff[v + 1] - L->soff[v]);
                *budget -= (int64_t)du * dv + 1;
                if (*budget < 0) return gain;
                uint64_t cuv = pair_cost(L, u, v), cvu = pair_cost(L, v, u);
                if (cvu < cuv) {
                    L->lay[i] = v, L->lay[i + 1] = u;
                    L->pos[v]--, L->pos[u]++;
                    gain += cuv - cvu;
                    improved = true;
                }
            }
        }
        if (!improved) break;
    }
    return gain;
}

/* ---------- Brandes-Köpf ---------- */
typedef struct PairSet { uint64_t *t; size_t cap, n; } PairSet;
static inline uint64_t pkey(int32_t a, int32_t b) {
    if (a > b) { int32_t t = a; a = b; b = t; }
    return ((uint64_t)(uint32_t)a << 32 | (uint32_t)b) + 1;
}
static inline size_t phash(uint64_t k, size_t cap) { return (size_t)((k * 0x9E3779B97F4A7C15ull) >> 17) & (cap - 1); }
[[nodiscard]] static bool pset_add(Arena *A, PairSet *s, int32_t a, int32_t b) {
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
    uint64_t k = pkey(a, b);
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
static bool bk_pass(Lay *L, const PairSet *conf, bool up, bool left, BKWork *W, double *out) {
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
            /* neighbours sorted by pass position */
            int32_t *ws = W->nbuf;
            for (uint32_t k = 0; k < m; k++) {
                int32_t w = (int32_t)adj[k0 + k];
                uint32_t j = k;
                while (j && W->posl[ws[j - 1]] > W->posl[w]) { ws[j] = ws[j - 1]; j--; }
                ws[j] = w;
            }
            double mp = (m - 1) / 2.0;
            for (uint32_t j = (uint32_t)floor(mp); j <= (uint32_t)ceil(mp); j++) {
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
            W->bw[k] = sep(L, u, v);
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
    for (unsigned k = 0; k < 4; k++) { (void)bk_pass(L, &conf, k < 2, (k & 1u) == 0, &W, xss[k]); PROF("bk-pass"); }
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
        double a[4] = {xss[0][v], xss[1][v], xss[2][v], xss[3][v]};
        for (unsigned i = 1; i < 4; i++)
            for (unsigned j = i; j && a[j - 1] > a[j]; j--) { double t = a[j]; a[j] = a[j - 1]; a[j - 1] = t; }
        L->x[v] = (a[1] + a[2]) / 2;
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
    if (px < x0 && dx > 0) t = fmax(t, (x0 - px) / dx);
    if (px > x1 && dx < 0) t = fmax(t, (x1 - px) / dx);
    if (py < y0 && dy > 0) t = fmax(t, (y0 - py) / dy);
    if (py > y1 && dy < 0) t = fmax(t, (y1 - py) / dy);
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
        if (!greedy_fas(A, N, E, ef, et, valid, hid, rev)) return CMM_ERR_NOMEM;
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
    memset(csum, 0, (S + 1) * sizeof *csum);
    memset(ccnt, 0, (S + 1) * sizeof *ccnt);
    bool clusters = S > 0;
    if (clusters) /* establish cluster contiguity (borders at extremes) before any scoring */
        for (size_t r = 0; r < R; r++) sweep_layer(&L, r, false, true, keys, idx, tmp, csum, ccnt, clusters);
    uint64_t bestc = cross_all(&L, cbuf, fen);
    memcpy(best, L.lay, placed * sizeof *best);
    unsigned since = 0;
    /* transpose budget: generous for small graphs, bounded (~linear) for big ones */
    int64_t tbudget = 2000000 + 40 * (int64_t)nseg;
    bool per_iter_tr = V <= 4000;
    for (unsigned iter = 0; iter < 24 && since < 4 && bestc > 0; iter++) {
        bool down = (iter & 1u) == 0;
        if (down) for (size_t r = 1; r < R; r++) sweep_layer(&L, r, true, false, keys, idx, tmp, csum, ccnt, clusters);
        else for (size_t r = R - 1; r-- > 0;) sweep_layer(&L, r, false, false, keys, idx, tmp, csum, ccnt, clusters);
        uint64_t c = cross_all(&L, cbuf, fen);
        if (per_iter_tr && c > 0 && tbudget > 0) {
            uint64_t gn = transpose(&L, &tbudget);
            c = gn <= c ? c - gn : 0;
        }
        if (c < bestc) { bestc = c; memcpy(best, L.lay, placed * sizeof *best); since = 0; }
        else since++;
    }
    memcpy(L.lay, best, placed * sizeof *best);
    for (size_t r = 0; r < R; r++)
        for (size_t i = L.loff[r]; i < L.loff[r + 1]; i++) L.pos[L.lay[i]] = (int32_t)(i - L.loff[r]);
    if (bestc > 0 && tbudget > 0) { /* final polish on the kept-best order */
        uint64_t gn = transpose(&L, &tbudget);
        bestc = gn <= bestc ? bestc - gn : 0;
    }

    PROF("order");
    /* ---- x via Brandes-Köpf ---- */
    if (!bk(&L)) return CMM_ERR_NOMEM;

    PROF("bk");
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
                double step = fmin(EDGE_SEP * 0.6, L.w[u] * frac / (double)(k - 1));
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
        if (a == b) { /* self loop: handled by the SVG writer around the node */
            g->elx[e] = g->nx[a] + g->nw[a] / 2 + 30 + g->elw[e] / 2;
            g->ely[e] = g->ny[a];
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
    *d = (Graph){.arena = g->arena, .dir = dir, .theme = g->theme, .flags = g->flags, .node_sep = g->node_sep,
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
        if (b.x0 == INFINITY) b = (Box){0, 0, fmax(tw, 40), 20};
        cb[k] = b;
        double w = fmax(b.x1 - b.x0, tw) + 2 * CLUSTER_PAD, h = b.y1 - b.y0 + 2 * CLUSTER_PAD + CLUSTER_TITLE;
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

cmm_status flow_layout(Graph *g) {
    cmm_status st = layout_rec(g, 0);
    if (st) return st;
    return CMM_OK;
}
