/* dagre order() port; see dorder.h. Mirrors lib/order/ sources of dagre-d3-es 7.0.x step by step, including
 * JS object-key enumeration order (array-index keys first, ascending) where dagre depends on it. */
#include "dorder.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

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
    bool down, bias;
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

static bool cmp_bias(const Ent *a, const Ent *b, bool bias) { /* a before b? (sort.js compareWithBias) */
    if (a->bc < b->bc) return true;
    if (a->bc > b->bc) return false;
    return bias ? a->i > b->i : a->i < b->i;
}
static void msort(Ent **a, Ent **t, size_t n, bool bias) { /* stable merge sort, like TimSort's result */
    if (n < 2) return;
    if (n <= 12) {
        for (size_t i = 1; i < n; i++) {
            Ent *x = a[i];
            size_t j = i;
            while (j > 0 && cmp_bias(x, a[j - 1], bias)) { a[j] = a[j - 1]; j--; }
            a[j] = x;
        }
        return;
    }
    size_t h = n / 2;
    msort(a, t, h, bias);
    msort(a + h, t, n - h, bias);
    size_t i = 0, j = h, k = 0;
    while (i < h && j < n) t[k++] = cmp_bias(a[j], a[i], bias) ? a[j++] : a[i++];
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
    uint32_t gen = ++c->gen;
    for (uint32_t j = 0; j < n; j++) c->mtag[E[j].v] = gen, c->mix[E[j].v] = j;
    for (size_t q = 0; q < c->cgn; q++) {
        int32_t a = c->cga[q] + (int32_t)V, b = c->cgb[q] + (int32_t)V;
        if (c->mtag[a] != gen || c->mtag[b] != gen) continue;
        Ent *ea = &E[c->mix[a]], *eb = &E[c->mix[b]];
        eb->indeg++;
        int32_t l = new_lk(c, (int32_t)c->mix[b], -1);
        if (l < 0) return out;
        E = c->ent + base;
        ea = &E[c->mix[a]];
        if (ea->outt < 0) ea->outh = l; else c->lk[ea->outt].next = l;
        ea->outt = l;
    }
    /* sourceSet = entries with indegree 0, in JS key order of mappedEntries */
    size_t ns = 0;
    for (uint32_t j = 0; j < n; j++) if (!E[j].indeg) c->src[ns++] = j;
    for (size_t a = 1; a < ns; a++) { /* insertion sort by key order (almost always already sorted) */
        uint32_t x = c->src[a];
        size_t b = a;
        while (b > 0 && key_lt(c, &E[x], &E[c->src[b - 1]])) { c->src[b] = c->src[b - 1]; b--; }
        c->src[b] = x;
    }
    size_t no = 0;
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
    /* sort.js: unsortable by -i (stable); consume from the end (smallest i) */
    for (size_t a = 1; a < nun; a++) {
        Ent *x = uns[a];
        size_t b = a;
        while (b > 0 && uns[b - 1]->i < x->i) { uns[b] = uns[b - 1]; b--; }
        uns[b] = x;
    }
    msort(srt, c->stmp, nsort, c->bias);
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
}

static uint64_t cross_count(const DOrder *d, const int32_t *lay, uint32_t *tree, uint32_t *buf) {
    uint64_t cc = 0;
    for (size_t r = 0; r + 1 < d->R; r++) {
        uint32_t a = d->loff[r], na = d->loff[r + 1] - a, b = d->loff[r + 1], nb = d->loff[r + 2] - b;
        if (!na || !nb) continue;
        uint32_t fi = 1;
        while (fi < nb) fi <<= 1;
        uint32_t tsz = 2 * fi - 1;
        fi -= 1;
        memset(tree, 0, tsz * sizeof *tree);
        for (uint32_t i = 0; i < na; i++) {
            int32_t v = lay[a + i];
            uint32_t m = 0;
            for (uint32_t j = d->soff[v]; j < d->soff[v + 1]; j++) {
                uint32_t p = (uint32_t)d->pos[d->sidx[j]], q = m++;
                while (q > 0 && buf[q - 1] > p) { buf[q] = buf[q - 1]; q--; }
                buf[q] = p;
            }
            for (uint32_t j = 0; j < m; j++) {
                uint32_t idx = buf[j] + fi;
                tree[idx] += 1;
                uint64_t ws = 0;
                while (idx > 0) {
                    if (idx % 2) ws += tree[idx + 1];
                    idx = (idx - 1) >> 1;
                    tree[idx] += 1;
                }
                cc += ws;
            }
        }
    }
    return cc;
}

bool dagre_order(Arena *A, const DOrder *d, uint64_t *crossings) {
    (void)A;
    size_t V = d->V, S = d->S, R = d->R, NS = V + S;
    Ctx c = {.d = d, .V = V, .S = S, .NS = NS};
    bool ok = false;
    uint32_t *rioff = calloc(R + 2, sizeof *rioff), *ri = nullptr, *dstk = nullptr, *dit = nullptr;
    uint32_t *tree = nullptr, *cbuf = nullptr;
    int32_t *best = nullptr;
    uint8_t *vis = nullptr;
    size_t maxdeg = 1, maxw = 1;
    for (size_t v = 0; v < V; v++) if (d->soff[v + 1] - d->soff[v] > maxdeg) maxdeg = d->soff[v + 1] - d->soff[v];
    for (size_t r = 0; r < R; r++) if (d->loff[r + 1] - d->loff[r] > maxw) maxw = d->loff[r + 1] - d->loff[r];
    c.nx = malloc(NS * sizeof *c.nx);
    c.khead = malloc((S + 1) * sizeof *c.khead);
    c.ktail = malloc((S + 1) * sizeof *c.ktail);
    c.knext = malloc(NS * sizeof *c.knext);
    c.mtag = calloc(NS, sizeof *c.mtag);
    c.mix = malloc(NS * sizeof *c.mix);
    c.sub = malloc((S + 1) * sizeof *c.sub);
    c.ord = malloc(NS * sizeof *c.ord);
    c.src = malloc(NS * sizeof *c.src);
    c.outl = malloc(NS * sizeof *c.outl);
    c.sbuf = malloc(2 * NS * sizeof *c.sbuf);
    c.stmp = malloc(NS * sizeof *c.stmp);
    c.cga = malloc((S * S + 1) * sizeof *c.cga);
    c.cgb = malloc((S * S + 1) * sizeof *c.cgb);
    c.cgbits = calloc((S * S + 63) / 64 + 1, sizeof *c.cgbits);
    c.prev = malloc((S + 1) * sizeof *c.prev);
    c.ptag = calloc(S + 1, sizeof *c.ptag);
    dstk = malloc((V + 1) * sizeof *dstk);
    dit = malloc((V + 1) * sizeof *dit);
    vis = calloc(V + 1, 1);
    best = malloc((V + 1) * sizeof *best);
    tree = malloc(4 * (maxw + 1) * sizeof *tree);
    cbuf = malloc((maxdeg + 1) * sizeof *cbuf);
    if (!rioff || !c.nx || !c.khead || !c.ktail || !c.knext || !c.mtag || !c.mix || !c.sub || !c.ord || !c.src ||
        !c.outl || !c.sbuf || !c.stmp || !c.cga || !c.cgb || !c.cgbits || !c.prev || !c.ptag || !dstk || !dit ||
        !vis || !best || !tree || !cbuf)
        goto done;
    /* per-rank layer-graph membership in g.nodes() order: rank-r nodes + clusters spanning r */
    for (size_t q = 0; q < d->nn; q++) {
        uint32_t x = d->nord[q];
        if (x < V) { if (d->rank[x] >= 0) rioff[d->rank[x] + 1]++; }
        else for (int32_t r = d->cmin[x - V]; r <= d->cmax[x - V]; r++) rioff[r + 1]++;
    }
    for (size_t r = 0; r < R; r++) rioff[r + 1] += rioff[r];
    ri = malloc((rioff[R] + 1) * sizeof *ri);
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
        uint32_t *fillp = malloc((R + 1) * sizeof *fillp);
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
        free(fillp);
        for (size_t r = 0; r < R; r++)
            for (uint32_t i = d->loff[r]; i < d->loff[r + 1]; i++) d->pos[d->lay[i]] = (int32_t)(i - d->loff[r]);
    }
    uint64_t bestcc = UINT64_MAX;
    for (unsigned it = 0, last = 0; last < 4; it++, last++) {
        c.down = it % 2;
        c.bias = it % 4 >= 2;
        for (size_t q = 0; q < c.cgn; q++) {
            size_t bit = (size_t)c.cga[q] * S + (size_t)c.cgb[q];
            c.cgbits[bit >> 6] = 0;
        }
        c.cgn = 0;
        for (size_t step = 0; step + 1 < R; step++) {
            int32_t r = c.down ? (int32_t)step + 1 : (int32_t)(R - 2 - step);
            c.rank = r;
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
            Res rr = sort_subgraph(&c, (int32_t)S);
            if (c.oom) goto done;
            uint32_t i = 0;
            for (int32_t x = rr.head; x >= 0; x = c.nx[x]) d->pos[x] = (int32_t)i++;
            /* addSubgraphConstraints */
            uint32_t pg = ++c.pgen;
            int32_t rootPrev = -1;
            for (int32_t x = rr.head; x >= 0; x = c.nx[x]) {
                int32_t child = d->par[x];
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
        /* buildLayerMatrix */
        for (size_t v = 0; v < V; v++) if (d->rank[v] >= 0) d->lay[d->loff[d->rank[v]] + (uint32_t)d->pos[v]] = (int32_t)v;
        uint64_t cc = cross_count(d, d->lay, tree, cbuf);
        if (cc < bestcc) { last = 0; bestcc = cc; memcpy(best, d->pos, V * sizeof *best); }
    }
    memcpy(d->pos, best, V * sizeof *best);
    for (size_t v = 0; v < V; v++) if (d->rank[v] >= 0) d->lay[d->loff[d->rank[v]] + (uint32_t)d->pos[v]] = (int32_t)v;
    if (crossings) *crossings = bestcc;
    ok = true;
done:
    free(rioff); free(ri); free(dstk); free(dit); free(vis); free(best); free(tree); free(cbuf);
    free(c.nx); free(c.khead); free(c.ktail); free(c.knext); free(c.mtag); free(c.mix); free(c.sub); free(c.ord);
    free(c.src); free(c.outl); free(c.sbuf); free(c.stmp); free(c.cga); free(c.cgb); free(c.cgbits); free(c.prev);
    free(c.ptag); free(c.ent); free(c.lk);
    return ok;
}
