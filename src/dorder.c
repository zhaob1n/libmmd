/* dagre order() port; see dorder.h. Mirrors lib/order/ sources of dagre-d3-es 7.0.x step by step, including
 * JS object-key enumeration order (array-index keys first, ascending) where dagre depends on it. */
#include "dorder.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifdef MMD_PROFILE
#include <stdio.h>
#include <time.h>
static double dnow(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3; }
#define DT(x) double x = dnow()
#else
#define DT(x)
#endif

typedef struct Res { int32_t head, tail; uint32_t len; double bc, w; bool has; } Res;
typedef struct Ent {
    int32_t v, head, tail, inh, outh, outt;
    uint32_t i, len, indeg;
    double bc, w;
    bool has, merged;
} Ent;
typedef struct Lk { int32_t e, next; } Lk;

typedef struct Ctx {
    const DOrder *d;
    size_t V, S, NS;
    int32_t *nx;                 /* [V+S] vs linked lists */
    int32_t *khead, *ktail, *knext; /* per layer children lists ([S+1], [S+1], [V+S]) */
    Ent *ent; size_t etop, ecap;
    Lk *lk; size_t ltop, lcap;
    uint32_t *mtag, *mix, gen;   /* [V+S] entry mapping per resolveConflicts call */
    Res *sub;                    /* [S] subgraph results */
    uint32_t *ord, *src, *outl;  /* scratch [V+S] */
    Ent **sbuf, **stmp;          /* sort scratch */
    int32_t *cga, *cgb; size_t cgn; uint64_t *cgbits; /* constraint graph (cluster edges, insertion order) */
    int32_t *prev; uint32_t *ptag, pgen; /* addSubgraphConstraints */
    bool down, bias, tie;
    uint64_t cgh; /* hash of the constraint-graph edge sequence */
    int32_t rank;
    bool oom;
} Ctx;

static int32_t new_ent(Ctx *c) {
    if (c->etop == c->ecap) {
        size_t nc = c->ecap ? 2 * c->ecap : 64;
        Ent *p = realloc(c->ent, nc * sizeof *p);
        if (!p) { c->oom = true; return -1; }
        c->ent = p, c->ecap = nc;
    }
    return (int32_t)c->etop++;
}
static int32_t new_lk(Ctx *c, int32_t e, int32_t next) {
    if (c->ltop == c->lcap) {
        size_t nc = c->lcap ? 2 * c->lcap : 64;
        Lk *p = realloc(c->lk, nc * sizeof *p);
        if (!p) { c->oom = true; return -1; }
        c->lk = p, c->lcap = nc;
    }
    c->lk[c->ltop] = (Lk){e, next};
    return (int32_t)c->ltop++;
}

/* resolve-conflicts.js mergeEntries */
static void merge_ent(Ctx *c, Ent *t, Ent *s) {
    double sum = 0, w = 0;
    if (t->w != 0) sum += t->bc * t->w, w += t->w;
    if (s->w != 0) sum += s->bc * s->w, w += s->w;
    c->nx[s->tail] = t->head; /* target.vs = source.vs.concat(target.vs) */
    t->head = s->head;
    t->len += s->len;
    t->bc = sum / w; /* NaN when both weights are 0, like JS; still counts as "has barycenter" */
    t->w = w;
    t->has = true;
    if (s->i < t->i) t->i = s->i;
    s->merged = true;
}

static bool key_lt(const Ctx *c, const Ent *a, const Ent *b) { /* JS object key order of entry.v */
    int64_t ka = c->d->ikey[a->v], kb = c->d->ikey[b->v];
    if ((ka >= 0) != (kb >= 0)) return ka >= 0;
    if (ka >= 0 && ka != kb) return ka < kb;
    return a->i < b->i;
}

static bool cmp_bias(const Ent *a, const Ent *b, bool bias, bool *tie) { /* a before b? (sort.js compareWithBias) */
    if (a->bc < b->bc) return true;
    if (a->bc > b->bc) return false;
    *tie = true; /* result depends on biasRight */
    return bias ? a->i > b->i : a->i < b->i;
}
static void msort(Ent **a, Ent **t, size_t n, bool bias, bool *tie) { /* stable merge sort, like TimSort's result */
    if (n < 2) return;
    if (n <= 12) {
        for (size_t i = 1; i < n; i++) {
            Ent *x = a[i];
            size_t j = i;
            while (j > 0 && cmp_bias(x, a[j - 1], bias, tie)) { a[j] = a[j - 1]; j--; }
            a[j] = x;
        }
        return;
    }
    size_t h = n / 2;
    msort(a, t, h, bias, tie);
    msort(a + h, t, n - h, bias, tie);
    size_t i = 0, j = h, k = 0;
    while (i < h && j < n) t[k++] = cmp_bias(a[j], a[i], bias, tie) ? a[j++] : a[i++];
    while (i < h) t[k++] = a[i++];
    while (j < n) t[k++] = a[j++];
    memcpy(a, t, n * sizeof *a);
}

static inline void res_push(Ctx *c, Res *r, int32_t h, int32_t t, uint32_t n) {
    if (!n) return;
    if (r->head < 0) r->head = h; else c->nx[r->tail] = h;
    r->tail = t, r->len += n;
}

static Res sort_subgraph(Ctx *c, int32_t k);

static Res sort_subgraph(Ctx *c, int32_t k) {
    const DOrder *d = c->d;
    size_t V = c->V, S = c->S;
    Res out = {.head = -1, .tail = -1};
    int32_t bl = -1, br = -1;
    if (k < (int32_t)S) {
        bl = (int32_t)(d->bbase[k] + 2 * (uint32_t)(c->rank - d->cmin[k]));
        br = bl + 1;
    }
    /* movable children + barycenters */
    size_t base = c->etop;
    uint32_t n = 0;
    for (int32_t x = c->khead[k]; x >= 0; x = c->knext[x]) {
        if (x == bl || x == br) continue;
        int32_t ei = new_ent(c);
        if (ei < 0) return out;
        Ent *e = &c->ent[ei];
        *e = (Ent){.v = x, .head = x, .tail = x, .inh = -1, .outh = -1, .outt = -1, .i = n++, .len = 1};
        c->nx[x] = -1;
        if ((size_t)x < V) {
            const uint32_t *nb = c->down ? d->pidx + d->poff[x] : d->sidx + d->soff[x];
            uint32_t m = c->down ? d->poff[x + 1] - d->poff[x] : d->soff[x + 1] - d->soff[x];
            if (m) {
                double s = 0;
                for (uint32_t j = 0; j < m; j++) s += d->pos[nb[j]];
                e->bc = s / m, e->w = m, e->has = true;
            }
        }
    }
    for (uint32_t j = 0; j < n; j++) {
        int32_t x = c->ent[base + j].v;
        if ((size_t)x < V) continue;
        Res sr = sort_subgraph(c, x - (int32_t)V);
        if (c->oom) return out;
        c->sub[x - (int32_t)V] = sr;
        Ent *e = &c->ent[base + j];
        if (sr.has) {
            if (e->has) {
                e->bc = (e->bc * e->w + sr.bc * sr.w) / (e->w + sr.w);
                e->w += sr.w;
            } else e->bc = sr.bc, e->w = sr.w, e->has = true;
        }
    }
    c->etop = base + n; /* child entries are dead */
    Ent *E = c->ent + base;
    /* resolveConflicts */
    bool anycg = false;
    uint32_t gen = ++c->gen;
    if (c->cgn && n > 1)
        for (uint32_t j = 0; j < n; j++) c->mtag[E[j].v] = gen, c->mix[E[j].v] = j;
    for (size_t q = 0; n > 1 && q < c->cgn; q++) {
        int32_t a = c->cga[q] + (int32_t)V, b = c->cgb[q] + (int32_t)V;
        if (c->mtag[a] != gen || c->mtag[b] != gen) continue;
        Ent *ea = &E[c->mix[a]], *eb = &E[c->mix[b]];
        eb->indeg++, anycg = true;
        int32_t l = new_lk(c, (int32_t)c->mix[b], -1);
        if (l < 0) return out;
        E = c->ent + base;
        ea = &E[c->mix[a]];
        if (ea->outt < 0) ea->outh = l; else c->lk[ea->outt].next = l;
        ea->outt = l;
    }
    /* no constraint edges: nothing merges, and sort.js orders entries by (bc, i) / i regardless of the
     * resolved order, so skip the topological pass */
    size_t no = 0;
    if (!anycg) {
        for (uint32_t j = 0; j < n; j++) c->outl[j] = j;
        no = n;
        goto expand;
    }
    size_t ns = 0;
    for (uint32_t j = 0; j < n; j++) if (!E[j].indeg) c->src[ns++] = j;
    for (size_t a = 1; a < ns; a++) { /* insertion sort by key order (almost always already sorted) */
        uint32_t x = c->src[a];
        size_t b = a;
        while (b > 0 && key_lt(c, &E[x], &E[c->src[b - 1]])) { c->src[b] = c->src[b - 1]; b--; }
        c->src[b] = x;
    }
    while (ns) {
        uint32_t vi = c->src[--ns];
        Ent *ve = &E[vi];
        c->outl[no++] = vi;
        for (int32_t l = ve->inh; l >= 0; l = c->lk[l].next) { /* in.reverse(): list is push-front */
            Ent *ue = &E[c->lk[l].e];
            if (ue->merged) continue;
            if (!ue->has || !ve->has || ue->bc >= ve->bc) merge_ent(c, ve, ue);
        }
        for (int32_t l = ve->outh; l >= 0; l = c->lk[l].next) {
            uint32_t wi = (uint32_t)c->lk[l].e;
            int32_t nl = new_lk(c, (int32_t)vi, E[wi].inh);
            if (nl < 0) return out;
            E = c->ent + base;
            ve = &E[vi];
            E[wi].inh = nl;
            if (--E[wi].indeg == 0) c->src[ns++] = wi;
        }
    }
expand:;
    /* expandSubgraphs + partition for sort */
    size_t nsort = 0, nun = 0;
    Ent **srt = c->sbuf, **uns = c->sbuf + n;
    for (size_t q = 0; q < no; q++) {
        Ent *e = &E[c->outl[q]];
        if (e->merged) continue;
        int32_t pv = -1;
        for (int32_t x = e->head; x >= 0;) {
            int32_t nxt = c->nx[x];
            if ((size_t)x >= V) {
                Res sr = c->sub[x - (int32_t)V];
                if (pv < 0) e->head = sr.head; else c->nx[pv] = sr.head;
                c->nx[sr.tail] = nxt;
                if (e->tail == x) e->tail = sr.tail;
                e->len += sr.len - 1;
                pv = sr.tail;
            } else pv = x;
            x = nxt;
        }
        if (e->has) srt[nsort++] = e; else uns[nun++] = e;
    }
    /* sort.js: unsortable sorted by -i; unmerged entries have distinct i < n, so bucket them */
    if (nun > 1) {
        Ent **bk = c->stmp;
        for (uint32_t j = 0; j < n; j++) bk[j] = nullptr;
        for (size_t a = 0; a < nun; a++) bk[uns[a]->i] = uns[a];
        size_t k = 0;
        for (uint32_t j = n; j-- > 0;) if (bk[j]) uns[k++] = bk[j];
    }
    msort(srt, c->stmp, nsort, c->bias, &c->tie);
    double sum = 0, wt = 0;
    uint32_t vsi = 0;
    while (nun && uns[nun - 1]->i <= vsi) { Ent *u = uns[--nun]; res_push(c, &out, u->head, u->tail, u->len); vsi++; }
    for (size_t q = 0; q < nsort; q++) {
        Ent *e = srt[q];
        vsi += e->len;
        res_push(c, &out, e->head, e->tail, e->len);
        sum += e->bc * e->w, wt += e->w;
        while (nun && uns[nun - 1]->i <= vsi) { Ent *u = uns[--nun]; res_push(c, &out, u->head, u->tail, u->len); vsi++; }
    }
    if (wt != 0) out.bc = sum / wt, out.w = wt, out.has = true;
    c->etop = base;
    if (bl >= 0) {
        c->nx[bl] = out.head >= 0 ? out.head : br;
        if (out.head < 0) out.tail = bl;
        c->nx[out.tail] = br;
        c->nx[br] = -1;
        out.head = bl, out.tail = br, out.len += 2;
        bool haspred = c->down ? c->rank > d->cmin[k] : c->rank < d->cmax[k];
        if (haspred) {
            int32_t o = c->down ? -2 : 2;
            double pl = d->pos[bl + o], pr = d->pos[br + o];
            if (!out.has) out.bc = 0, out.w = 0;
            out.bc = (out.bc * out.w + pl + pr) / (out.w + 2);
            out.w += 2;
            out.has = true;
        }
    }
    return out;
}

static void cg_set(Ctx *c, int32_t a, int32_t b) {
    size_t bit = (size_t)a * c->S + (size_t)b;
    if (c->cgbits[bit >> 6] >> (bit & 63) & 1) return;
    c->cgbits[bit >> 6] |= 1ull << (bit & 63);
    c->cga[c->cgn] = a, c->cgb[c->cgn] = b, c->cgn++;
    c->cgh = (c->cgh ^ (uint64_t)(bit + 1)) * 0x100000001b3ull;
}

/* crossings between layers r and r+1 */
static uint64_t cross_pair(const DOrder *d, const int32_t *lay, size_t r, uint32_t *tree) {
    uint64_t cc = 0;
    uint32_t a = d->loff[r], na = d->loff[r + 1] - a, nb = d->loff[r + 2] - d->loff[r + 1];
    if (!na || !nb) return 0;
    memset(tree, 0, (nb + 1) * sizeof *tree);
    uint32_t ins = 0;
    for (uint32_t i = 0; i < na; i++) {
        int32_t v = lay[a + i];
        uint32_t j0 = d->soff[v], j1 = d->soff[v + 1];
        for (uint32_t j = j0; j < j1; j++) {
            uint32_t le = 0;
            for (uint32_t k = (uint32_t)d->pos[d->sidx[j]] + 1; k; k &= k - 1) le += tree[k];
            cc += ins - le;
        }
        for (uint32_t j = j0; j < j1; j++)
            for (uint32_t k = (uint32_t)d->pos[d->sidx[j]] + 1; k <= nb; k += k & -k) tree[k]++;
        ins += j1 - j0;
    }
    return cc;
}

/* sortSubgraph for a layer graph with no clusters and an empty constraint graph: resolveConflicts cannot merge,
 * every entry is a single node, so this is barycenters + sort.js on flat SoA arrays. Bit-identical result. */
static void sort_flat(Ctx *c, const uint32_t *xs, uint32_t n, double *bc, uint32_t *si, uint32_t *ui, int32_t *out) {
    const DOrder *d = c->d;
    const uint32_t *off = c->down ? d->poff : d->soff, *idx = c->down ? d->pidx : d->sidx;
    uint32_t ns = 0, nu = 0;
    for (uint32_t j = 0; j < n; j++) {
        uint32_t x = xs[j], a = off[x], b = off[x + 1];
        if (a == b) { ui[nu++] = j; continue; }
        double sum = 0;
        for (uint32_t q = a; q < b; q++) sum += d->pos[idx[q]];
        bc[j] = sum / (b - a);
        /* stable insertion by compareWithBias */
        uint32_t k = ns++;
        while (k > 0) {
            uint32_t y = si[k - 1];
            bool before;
            if (bc[j] < bc[y]) before = true;
            else if (bc[j] > bc[y]) before = false;
            else { c->tie = true; before = c->bias; } /* j > y always: bias puts the higher i first */
            if (!before) break;
            si[k] = y, k--;
        }
        si[k] = j;
    }
    uint32_t o = 0, vsi = 0, u = 0;
    while (u < nu && ui[u] <= vsi) out[o++] = (int32_t)xs[ui[u++]], vsi++;
    for (uint32_t q = 0; q < ns; q++) {
        out[o++] = (int32_t)xs[si[q]], vsi++;
        while (u < nu && ui[u] <= vsi) out[o++] = (int32_t)xs[ui[u++]], vsi++;
    }
    while (u < nu) out[o++] = (int32_t)xs[ui[u++]];
}

static void *dscratch(Arena *A, bool ua, size_t sz, bool zero) {
    void *p = ua ? arena_alloc(A, sz ? sz : 1, alignof(max_align_t)) : zero ? calloc(1, sz ? sz : 1) : malloc(sz ? sz : 1);
    if (p && ua && zero) memset(p, 0, sz);
    return p;
}

bool dagre_order(Arena *A, const DOrder *d, uint64_t *crossings) {
    size_t V = d->V, S = d->S, R = d->R, NS = V + S;
    DT(t_start);
    Ctx c = {.d = d, .V = V, .S = S, .NS = NS};
    bool ok = false;
    /* small graphs: scratch from the render arena (no per-call malloc/free churn) */
    bool ua = NS <= 1024 && S <= 32;
#define SA(sz, zero) dscratch(A, ua, (sz), (zero))
    uint32_t *rioff = SA((R + 2) * sizeof *rioff, true), *ri = nullptr, *dstk = nullptr, *dit = nullptr;
    uint32_t *tree = nullptr;
    int32_t *best = nullptr;
    uint8_t *vis = nullptr;
    size_t maxw = 1;
    for (size_t r = 0; r < R; r++) if (d->loff[r + 1] - d->loff[r] > maxw) maxw = d->loff[r + 1] - d->loff[r];
    c.nx = SA(NS * sizeof *c.nx, false);
    c.khead = SA((S + 1) * sizeof *c.khead, false);
    c.ktail = SA((S + 1) * sizeof *c.ktail, false);
    c.knext = SA(NS * sizeof *c.knext, false);
    c.mtag = SA((NS) * sizeof *c.mtag, true);
    c.mix = SA(NS * sizeof *c.mix, false);
    c.sub = SA((S + 1) * sizeof *c.sub, false);
    c.ord = SA(NS * sizeof *c.ord, false);
    c.src = SA(NS * sizeof *c.src, false);
    c.outl = SA(NS * sizeof *c.outl, false);
    c.sbuf = SA(2 * NS * sizeof *c.sbuf, false);
    c.stmp = SA(NS * sizeof *c.stmp, false);
    c.cga = SA((S * S + 1) * sizeof *c.cga, false);
    c.cgb = SA((S * S + 1) * sizeof *c.cgb, false);
    c.cgbits = SA(((S * S + 63) / 64 + 1) * sizeof *c.cgbits, true);
    c.prev = SA((S + 1) * sizeof *c.prev, false);
    c.ptag = SA((S + 1) * sizeof *c.ptag, true);
    dstk = SA((V + 1) * sizeof *dstk, false);
    dit = SA((V + 1) * sizeof *dit, false);
    vis = SA((V + 1) * 1, true);
    best = SA((V + 1) * sizeof *best, false);
    double *fbc = SA((maxw + 1) * sizeof *fbc, false);
    uint32_t *fsi = SA((maxw + 1) * sizeof *fsi, false), *fui = SA((maxw + 1) * sizeof *fui, false);
    /* sweep memo: per (layer, direction) the neighbour-layer order it was computed from and its result */
    size_t LT = d->loff[R] + 1;
    uint32_t *ver = SA((R + 1) * sizeof *ver, true), *pva = SA((R + 1) * sizeof *pva, false);
    uint32_t *pvb = SA((R + 1) * sizeof *pvb, false);
    uint64_t *pcc = SA((R + 1) * sizeof *pcc, false), *mcgh = SA((2 * R + 1) * sizeof *mcgh, false);
    uint8_t *mst = SA(2 * R + 1, true); /* bit0 valid, bit1 bias-dependent, bit2 bias, bit3 cg-independent */
    uint32_t *mcgn = SA((2 * R + 1) * sizeof *mcgn, false);
    int32_t *msnap = SA(2 * LT * sizeof *msnap, false), *mout = SA(2 * LT * sizeof *mout, false);
    tree = SA(4 * (maxw + 1) * sizeof *tree, false);
    if (!rioff || !c.nx || !c.khead || !c.ktail || !c.knext || !c.mtag || !c.mix || !c.sub || !c.ord || !c.src ||
        !c.outl || !c.sbuf || !c.stmp || !c.cga || !c.cgb || !c.cgbits || !c.prev || !c.ptag || !dstk || !dit ||
        !vis || !best || !tree || !fbc || !fsi || !fui || !ver || !pva || !pvb || !pcc || !mcgh || !mst || !mcgn || !msnap || !mout)
        goto done;
    /* per-rank layer-graph membership in g.nodes() order: rank-r nodes + clusters spanning r */
    for (size_t q = 0; q < d->nn; q++) {
        uint32_t x = d->nord[q];
        if (x < V) { if (d->rank[x] >= 0) rioff[d->rank[x] + 1]++; }
        else for (int32_t r = d->cmin[x - V]; r <= d->cmax[x - V]; r++) rioff[r + 1]++;
    }
    for (size_t r = 0; r < R; r++) rioff[r + 1] += rioff[r];
    ri = SA((rioff[R] + 1) * sizeof *ri, false);
    if (!ri) goto done;
    {
        uint32_t *fill = c.ord; /* reuse as [R] fill pointers when R <= NS, else separate */
        uint32_t *tmpf = R <= NS ? fill : malloc(R * sizeof *tmpf);
        if (!tmpf) goto done;
        for (size_t r = 0; r < R; r++) tmpf[r] = rioff[r];
        for (size_t q = 0; q < d->nn; q++) {
            uint32_t x = d->nord[q];
            if (x < V) { if (d->rank[x] >= 0) ri[tmpf[d->rank[x]]++] = x; }
            else for (int32_t r = d->cmin[x - V]; r <= d->cmax[x - V]; r++) ri[tmpf[r]++] = x;
        }
        if (tmpf != fill) free(tmpf);
    }
    /* initOrder: DFS from simple nodes sorted (stably) by rank, successors in dagre order */
    {
        uint32_t *fillp = SA((R + 1) * sizeof *fillp, false);
        if (!fillp) goto done;
        for (size_t r = 0; r < R; r++) fillp[r] = d->loff[r];
        for (size_t r = 0; r < R; r++)
            for (uint32_t q = rioff[r]; q < rioff[r + 1]; q++) {
                uint32_t s0 = ri[q];
                if (s0 >= V || vis[s0]) continue;
                size_t sp = 0;
                dstk[sp++] = s0, dit[s0] = d->doff[s0], vis[s0] = 1;
                d->lay[fillp[d->rank[s0]]++] = (int32_t)s0;
                while (sp) {
                    uint32_t u = dstk[sp - 1];
                    if (dit[u] == d->doff[u + 1]) { sp--; continue; }
                    uint32_t w = d->didx[dit[u]++];
                    if (vis[w]) continue;
                    vis[w] = 1, dit[w] = d->doff[w];
                    d->lay[fillp[d->rank[w]]++] = (int32_t)w;
                    dstk[sp++] = w;
                }
            }
        if (!ua) free(fillp);
        for (size_t r = 0; r < R; r++)
            for (uint32_t i = d->loff[r]; i < d->loff[r + 1]; i++) d->pos[d->lay[i]] = (int32_t)(i - d->loff[r]);
    }
    DT(t_init);
    PMARK("o-init");
#ifdef MMD_PROFILE
    double t_sw = 0, t_cc = 0;
#endif
    uint64_t bestcc = UINT64_MAX;
    for (unsigned it = 0, last = 0; last < 4; it++, last++) {
        DT(t0);
        c.down = it % 2;
        c.bias = it % 4 >= 2;
        for (size_t q = 0; q < c.cgn; q++) {
            size_t bit = (size_t)c.cga[q] * S + (size_t)c.cgb[q];
            c.cgbits[bit >> 6] = 0;
        }
        c.cgn = 0;
        c.cgh = 0xcbf29ce484222325ull;
        for (size_t step = 0; step + 1 < R; step++) {
            int32_t r = c.down ? (int32_t)step + 1 : (int32_t)(R - 2 - step);
            c.rank = r;
            uint32_t lo = d->loff[r], w = d->loff[r + 1] - lo;
            int32_t nr = c.down ? r - 1 : r + 1;
            uint32_t nlo = d->loff[nr], nw = d->loff[nr + 1] - nlo;
            size_t m = 2 * (size_t)r + c.down;
            int32_t *snap = msnap + (c.down ? LT : 0) + nlo, *mo = mout + (c.down ? LT : 0) + lo;
            /* sortSubgraph reads only the neighbour layer's order, the constraint graph and (on ties) biasRight;
             * the layer's own current order and everything else are fixed, so an identical input replays. */
            bool hit = (mst[m] & 1) && ((mst[m] & 8) || (mcgn[m] == c.cgn && mcgh[m] == c.cgh)) &&
                       (!(mst[m] & 2) || !!(mst[m] & 4) == c.bias) && !memcmp(snap, d->lay + nlo, nw * sizeof *snap);
            int32_t *no = mo;
            if (!hit && rioff[r + 1] - rioff[r] == w) {
                /* no cluster spans this layer: constraint-graph edges (cluster pairs) cannot touch it */
                c.tie = false;
                sort_flat(&c, ri + rioff[r], w, fbc, fsi, fui, mo);
                memcpy(snap, d->lay + nlo, nw * sizeof *snap);
                mst[m] = (uint8_t)(1 | (c.tie ? 2 : 0) | (c.bias ? 4 : 0) | 8);
                hit = true;
            }
            if (!hit) {
                /* buildLayerGraph children lists */
                c.khead[S] = c.ktail[S] = -1;
                for (uint32_t q = rioff[r]; q < rioff[r + 1]; q++)
                    if (ri[q] >= V) c.khead[ri[q] - V] = c.ktail[ri[q] - V] = -1;
                for (uint32_t q = rioff[r]; q < rioff[r + 1]; q++) {
                    int32_t x = (int32_t)ri[q], p = d->par[x] < 0 ? (int32_t)S : d->par[x];
                    c.knext[x] = -1;
                    if (c.ktail[p] < 0) c.khead[p] = x; else c.knext[c.ktail[p]] = x;
                    c.ktail[p] = x;
                }
                c.etop = c.ltop = 0;
                c.tie = false;
                uint32_t cgn0 = c.cgn;
                uint64_t cgh0 = c.cgh;
                Res rr = sort_subgraph(&c, (int32_t)S);
                if (c.oom) goto done;
                uint32_t i = 0;
                for (int32_t x = rr.head; x >= 0 && i < w; x = c.nx[x]) mo[i++] = x;
                memcpy(snap, d->lay + nlo, nw * sizeof *snap);
                mst[m] = (uint8_t)(1 | (c.tie ? 2 : 0) | (c.bias ? 4 : 0));
                mcgn[m] = cgn0, mcgh[m] = cgh0;
            }
            if (memcmp(no, d->lay + lo, w * sizeof *no)) {
                memcpy(d->lay + lo, no, w * sizeof *no);
                ver[r]++;
                for (uint32_t i = 0; i < w; i++) d->pos[no[i]] = (int32_t)i;
            }
            /* addSubgraphConstraints */
            uint32_t pg = ++c.pgen;
            int32_t rootPrev = -1;
            for (uint32_t q = 0; q < w; q++) {
                int32_t child = d->par[no[q]];
                while (child >= 0) {
                    int32_t parent = d->par[V + (size_t)child], pc;
                    if (parent >= 0) {
                        pc = c.ptag[parent] == pg ? c.prev[parent] : -1;
                        c.ptag[parent] = pg, c.prev[parent] = child;
                    } else pc = rootPrev, rootPrev = child;
                    if (pc >= 0 && pc != child) { cg_set(&c, pc, child); break; }
                    child = parent;
                }
            }
        }
        DT(t1);
        PMARK("o-sweep");
        /* only cc < bestcc matters (and the best's exact count): sum cached pairs first, then count stale
         * pairs and stop once the sum reaches bestcc; skipped pairs stay stale for the next sweep */
        uint64_t cc = 0;
        for (size_t r = 0; r + 1 < R; r++)
            if (it && pva[r] == ver[r] && pvb[r] == ver[r + 1]) cc += pcc[r];
        for (size_t r = 0; r + 1 < R && cc < bestcc; r++) {
            if (it && pva[r] == ver[r] && pvb[r] == ver[r + 1]) continue;
            pcc[r] = cross_pair(d, d->lay, r, tree), pva[r] = ver[r], pvb[r] = ver[r + 1];
            cc += pcc[r];
        }
        PMARK("o-cross");
#ifdef MMD_PROFILE
        t_sw += t1 - t0, t_cc += dnow() - t1;
#endif
#ifdef MMD_PROFILE
        if (getenv("MMD_PROFV")) fprintf(stderr, "  dsweep %u cc=%llu\n", it, (unsigned long long)cc);
#endif
        if (cc < bestcc) { last = 0; bestcc = cc; memcpy(best, d->pos, V * sizeof *best); }
        if (bestcc == 0) break; /* dagre keeps sweeping, but nothing can beat 0 (strict <) */
    }
    memcpy(d->pos, best, V * sizeof *best);
    for (size_t v = 0; v < V; v++) if (d->rank[v] >= 0) d->lay[d->loff[d->rank[v]] + (uint32_t)d->pos[v]] = (int32_t)v;
    if (crossings) *crossings = bestcc;
#ifdef MMD_PROFILE
    if (getenv("MMD_PROFV")) fprintf(stderr, "  dtime init=%.1f sweeps=%.1f cc=%.1f us R=%zu V=%zu E=%u\n", t_init - t_start, t_sw, t_cc, R, V, d->soff[V]);
#endif
    ok = true;
done:
#undef SA
    if (!ua) {
    free(rioff); free(ri); free(dstk); free(dit); free(vis); free(best); free(tree); free(fbc); free(fsi); free(fui);
    free(c.nx); free(c.khead); free(c.ktail); free(c.knext); free(c.mtag); free(c.mix); free(c.sub); free(c.ord);
    free(c.src); free(c.outl); free(c.sbuf); free(c.stmp); free(c.cga); free(c.cgb); free(c.cgbits); free(c.prev);
    free(c.ptag);
    free(ver); free(pva); free(pvb); free(pcc); free(mcgh); free(mst); free(mcgn); free(msnap); free(mout);
    }
    free(c.ent); free(c.lk);
    return ok;
}
