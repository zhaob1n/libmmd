/* Sugiyama layered layout:
 *   1. DFS cycle removal            2. longest-path ranking + greedy tightening
 *   3. long-edge normalisation      4. barycenter sweeps, cluster-contiguous, Fenwick crossing count
 *   5. Brandes-Köpf coordinate assignment (4 alignments + balancing)
 *   6. rank-axis placement, direction transform, edge polyline + clipping, cluster boxes */
#include <math.h>

#include "graph.h"
#include "text.h"

constexpr double PAD_NODE = 15.0;
constexpr double CLUSTER_PAD = 15.0;
constexpr double CLUSTER_TITLE = 24.0;
constexpr double EDGE_SEP = 20.0;
constexpr unsigned MAX_KEY_DEPTH = 8;

void node_size(Graph *g, size_t v) {
    double fs = g->font_size, tw;
    unsigned lines;
    label_measure(g->nlabel[v], fs, &tw, &lines);
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
    uint8_t *dummy;
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
        sort_u32_small(m - s, buf + s);
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

/* ---------- one barycenter sweep over layer r ---------- */
static void sweep_layer(Lay *L, size_t r, bool down, Key *keys, uint32_t *idx, uint32_t *tmp,
                        double *csum, uint32_t *ccnt, bool clusters) {
    size_t a = L->loff[r], b = L->loff[r + 1], n = b - a;
    if (n < 2) return;
    const uint32_t *off = down ? L->poff : L->soff, *adj = down ? L->pidx : L->sidx;
    for (size_t i = 0; i < n; i++) {
        int32_t v = L->lay[a + i];
        uint32_t k0 = off[v], k1 = off[v + 1];
        double bcv;
        if (k1 > k0) {
            double s = 0;
            for (uint32_t k = k0; k < k1; k++) s += L->pos[adj[k]];
            bcv = s / (k1 - k0);
        } else {
            bcv = (double)i * 1.0; /* no neighbours: keep current slot */
        }
        L->bc[v] = bcv;
    }
    if (clusters) {
        Graph *g = L->g;
        for (size_t i = 0; i < n; i++) {
            int32_t v = L->lay[a + i];
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
                k->bc[d] = csum[c] / ccnt[c];
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
    for (unsigned k = 0; k < 4; k++) (void)bk_pass(L, &conf, k < 2, (k & 1u) == 0, &W, xss[k]);
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

cmm_status flow_layout(Graph *g) {
    Arena *A = g->arena;
    size_t N = g->nn, E = g->ne, S = g->ns;
    bool horiz = g->dir == DIR_LR || g->dir == DIR_RL;
    g->nw = ANEW(A, double, N); g->nh = ANEW(A, double, N);
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
    for (size_t e = 0; e < E; e++) {
        int32_t a = g->efrom[e], b = g->eto[e];
        for (unsigned k = 0; k < 2; k++) {
            int32_t *p = k ? &b : &a;
            if (g->nflags[*p] & NF_SUBREF) {
                int32_t s = NONE;
                for (size_t i = 0; i < S; i++) if (str_eq(g->sid[i], g->nid[*p])) { s = (int32_t)i; break; }
                *p = s == NONE ? NONE : srep[s];
            }
        }
        ef[e] = a, et[e] = b;
        double lw = 0;
        unsigned ln = 0;
        label_measure(g->elabel[e], g->font_size, &lw, &ln);
        g->elw[e] = ln ? lw + 8 : 0;
        g->elh[e] = ln ? ln * g->font_size * 1.5 + 4 : 0;
        if (ln && a != NONE && b != NONE && a != b) has_labels = true;
    }
    for (size_t e = 0; e < E; e++) mlen[e] = g->elen[e] * (has_labels ? 2 : 1);
    double ranksep = has_labels ? g->rank_sep / 2 : g->rank_sep;

    /* layout-space sizes (transposed for LR/RL) */
    #define LW(v) (horiz ? g->nh[v] : g->nw[v])
    #define LH(v) (horiz ? g->nw[v] : g->nh[v])

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
    /* DFS cycle removal */
    memset(state, 0, N);
    memset(rev, 0, E);
    for (size_t s0 = 0; s0 < N; s0++) {
        if (state[s0] || (g->nflags[s0] & NF_HIDDEN)) continue;
        size_t sp = 0;
        stk[sp++] = (uint32_t)s0;
        it[s0] = ooff[s0];
        state[s0] = 1;
        while (sp) {
            uint32_t u = stk[sp - 1];
            if (it[u] < ooff[u + 1]) {
                uint32_t e = oidx[it[u]++];
                uint32_t w = (uint32_t)et[e];
                if (state[w] == 1) rev[e] = 1;
                else if (!state[w]) { state[w] = 1; it[w] = ooff[w]; stk[sp++] = w; }
            } else {
                state[u] = 2;
                sp--;
            }
        }
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
    /* greedy tightening: move nodes down towards successors when it shortens total edge length */
    for (size_t q = qt; q-- > 0;) {
        uint32_t v = topo[q];
        uint32_t od = ooff[v + 1] - ooff[v];
        if (!od || indeg[v] >= od) continue;
        int32_t hi = INT32_MAX;
        for (uint32_t k = ooff[v]; k < ooff[v + 1]; k++) {
            uint32_t e = oidx[k];
            int32_t c = rank[EV(e)] - mlen[e];
            if (c < hi) hi = c;
        }
        if (hi > rank[v]) rank[v] = hi;
    }
    int32_t minr = INT32_MAX, maxr = 0;
    for (size_t v = 0; v < N; v++) if (!(g->nflags[v] & NF_HIDDEN) && rank[v] < minr) minr = rank[v];
    if (minr == INT32_MAX) minr = 0;
    for (size_t v = 0; v < N; v++) {
        rank[v] -= minr;
        if (!(g->nflags[v] & NF_HIDDEN) && rank[v] > maxr) maxr = rank[v];
    }

    /* ---- normalisation: dummies ---- */
    size_t D = 0;
    uint32_t *choff = ANEW(A, uint32_t, E + 1);
    if (!choff) return CMM_ERR_NOMEM;
    for (size_t e = 0; e < E; e++) {
        choff[e] = (uint32_t)D;
        if (EVALID(e)) D += (size_t)(rank[EV(e)] - rank[EU(e)] - 1);
    }
    choff[E] = (uint32_t)D;
    size_t V = N + D;
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
    uint64_t bestc = cross_all(&L, cbuf, fen);
    memcpy(best, L.lay, placed * sizeof *best);
    unsigned since = 0;
    for (unsigned iter = 0; iter < 24 && since < 4 && bestc > 0; iter++) {
        bool down = (iter & 1u) == 0;
        if (down) for (size_t r = 1; r < R; r++) sweep_layer(&L, r, true, keys, idx, tmp, csum, ccnt, clusters);
        else for (size_t r = R - 1; r-- > 0;) sweep_layer(&L, r, false, keys, idx, tmp, csum, ccnt, clusters);
        uint64_t c = cross_all(&L, cbuf, fen);
        if (c < bestc) { bestc = c; memcpy(best, L.lay, placed * sizeof *best); since = 0; }
        else since++;
    }
    memcpy(L.lay, best, placed * sizeof *best);
    for (size_t r = 0; r < R; r++)
        for (size_t i = L.loff[r]; i < L.loff[r + 1]; i++) L.pos[L.lay[i]] = (int32_t)(i - L.loff[r]);

    /* ---- x via Brandes-Köpf ---- */
    if (!bk(&L)) return CMM_ERR_NOMEM;

    /* ---- rank axis ---- */
    double *ry = ANEW(A, double, R + 1), *rh = ANEW(A, double, R + 1);
    double *gtop = ANEW(A, double, R + 1), *gbot = ANEW(A, double, R + 1);
    int32_t *cmin = ANEW(A, int32_t, S + 1), *cmax = ANEW(A, int32_t, S + 1);
    if (!ry || !rh || !gtop || !gbot || !cmin || !cmax) return CMM_ERR_NOMEM;
    for (size_t s = 0; s < S; s++) cmin[s] = INT32_MAX, cmax[s] = -1;
    for (size_t v = 0; v < N; v++) {
        if (L.rank[v] < 0) continue;
        for (int32_t c = g->nparent[v]; c != NONE; c = g->sparent[c]) {
            if (L.rank[v] < cmin[c]) cmin[c] = L.rank[v];
            if (L.rank[v] > cmax[c]) cmax[c] = L.rank[v];
        }
    }
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
        bool labset = false;
        for (int32_t k = 0; k <= span; k++) {
            int32_t vv = CHAIN(e, k, span);
            double px = FX(X[vv], ry[L.rank[vv]]) * sgx, py = FY(X[vv], ry[L.rank[vv]]) * sgy;
            if (k > 0 && k < span && has_labels && g->elw[e] > 0 && k == span / 2) { lx = px; ly = py; labset = true; }
            if (!VEC_PUSH(A, &g->pts, px) || !VEC_PUSH(A, &g->pts, py)) return CMM_ERR_NOMEM;
        }
        size_t np = (size_t)span + 1;
        double *P = g->pts.v + base;
        if (rev[e]) {
            for (size_t i = 0; i < np / 2; i++) {
                double tx = P[2 * i], ty = P[2 * i + 1];
                P[2 * i] = P[2 * (np - 1 - i)], P[2 * i + 1] = P[2 * (np - 1 - i) + 1];
                P[2 * (np - 1 - i)] = tx, P[2 * (np - 1 - i) + 1] = ty;
            }
        }
        /* clip endpoints at node boundaries */
        clip_to_node(g, a, P[2], P[3], &P[0], &P[1]);
        clip_to_node(g, b, P[2 * np - 4], P[2 * np - 3], &P[2 * np - 2], &P[2 * np - 1]);
        if (!labset) { lx = (P[0] + P[2 * np - 2]) / 2; ly = (P[1] + P[2 * np - 1]) / 2; }
        g->elx[e] = lx, g->ely[e] = ly;
        g->eptn[e] = (uint32_t)np;
    }
    /* clusters: deepest first */
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
            double tw = text_width(g->stitle[s], g->font_size) + 2 * CLUSTER_PAD;
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
    return CMM_OK;
}
