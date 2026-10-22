/* Network simplex rank assignment (Gansner et al.), following dagre's formulation:
 * feasible tight tree -> low/lim -> cut values -> leave/enter/exchange until optimal. */
#include "nsimplex.h"
#ifdef CMM_PROFILE
#include <stdio.h>
#include <time.h>
static double ns_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6; }
#endif

typedef struct NS {
    size_t N, E;
    const int32_t *eu, *ev, *minlen;
    const uint8_t *valid;
    int32_t *rank;
    const uint32_t *ioff, *iidx; /* incidence CSR */
    uint8_t *intree_e, *intree_v;
    int32_t *pe;    /* parent edge of node in tree, -1 for root */
    int32_t *low, *lim, *pre;
    int64_t *cut;   /* per edge, valid for tree edges */
    uint32_t *stk, *it, *order;
    const int32_t *comp; size_t nc; /* nodes of current component */
    const uint32_t *cedges; size_t ncE;
} NS;

static inline int32_t slack(const NS *s, uint32_t e) { return s->rank[s->ev[e]] - s->rank[s->eu[e]] - s->minlen[e]; }
static inline int32_t other(const NS *s, uint32_t e, int32_t v) { return s->eu[e] == v ? s->ev[e] : s->eu[e]; }

static size_t tight_tree(NS *s) {
    size_t sp = 0, cnt = 0;
    for (size_t i = 0; i < s->nc; i++) if (s->intree_v[s->comp[i]]) { s->stk[sp++] = (uint32_t)s->comp[i]; cnt++; }
    while (sp) {
        int32_t v = (int32_t)s->stk[--sp];
        for (uint32_t k = s->ioff[v]; k < s->ioff[v + 1]; k++) {
            uint32_t e = s->iidx[k];
            int32_t w = other(s, e, v);
            if (!s->intree_v[w] && slack(s, e) == 0) {
                s->intree_v[w] = 1;
                s->intree_e[e] = 1;
                s->stk[sp++] = (uint32_t)w;
                cnt++;
            }
        }
    }
    return cnt;
}

/* iterative DFS over tree edges from `root` (keeping pe[root]): low/lim from `next`,
 * preorder written to order[pos..] */
static void low_lim(NS *s, int32_t root, int32_t next, size_t pos) {
    size_t sp = 0, no = pos;
    s->stk[sp++] = (uint32_t)root;
    s->it[root] = s->ioff[root];
    s->low[root] = next;
    s->pre[root] = (int32_t)no;
    s->order[no++] = (uint32_t)root;
    while (sp) {
        int32_t v = (int32_t)s->stk[sp - 1];
        bool pushed = false;
        while (s->it[v] < s->ioff[v + 1]) {
            uint32_t e = s->iidx[s->it[v]++];
            if (!s->intree_e[e] || (int32_t)e == s->pe[v]) continue;
            int32_t w = other(s, e, v);
            s->pe[w] = (int32_t)e;
            s->it[w] = s->ioff[w];
            s->low[w] = next;
            s->pre[w] = (int32_t)no;
            s->order[no++] = (uint32_t)w;
            s->stk[sp++] = (uint32_t)w;
            pushed = true;
            break;
        }
        if (!pushed) {
            s->lim[v] = next++;
            sp--;
        }
    }
}

/* cut values for preorder segment [a, b), children before parents; order[a] is the subtree root */
static void cut_values(NS *s, size_t a, size_t b) {
    for (size_t i = b; i-- > a + 1;) {
        int32_t c = (int32_t)s->order[i];
        uint32_t pe = (uint32_t)s->pe[c];
        bool child_is_tail = s->eu[pe] == c;
        int64_t cv = 1;
        for (uint32_t k = s->ioff[c]; k < s->ioff[c + 1]; k++) {
            uint32_t e = s->iidx[k];
            if (e == pe) continue;
            bool is_out = s->eu[e] == c;
            bool to_head = is_out == child_is_tail;
            cv += to_head ? 1 : -1;
            if (s->intree_e[e] && s->pe[other(s, e, c)] == (int32_t)e) cv += to_head ? -s->cut[e] : s->cut[e];
        }
        s->cut[pe] = cv;
    }
}

/* graphviz-style incremental low/lim: unchanged subtrees are skipped */
static void dfs_range(NS *s, int32_t root, int32_t par, int32_t low) {
    if (s->pe[root] == par && s->low[root] == low) return;
    size_t sp = 0;
    int32_t next = low;
    s->pe[root] = par;
    s->low[root] = next;
    s->it[root] = s->ioff[root];
    s->stk[sp++] = (uint32_t)root;
    while (sp) {
        int32_t v = (int32_t)s->stk[sp - 1];
        bool pushed = false;
        while (s->it[v] < s->ioff[v + 1]) {
            uint32_t e = s->iidx[s->it[v]++];
            if (!s->intree_e[e] || (int32_t)e == s->pe[v]) continue;
            int32_t w = other(s, e, v);
            if (s->pe[w] == (int32_t)e && s->low[w] == next) { next = s->lim[w] + 1; continue; }
            s->pe[w] = (int32_t)e;
            s->low[w] = next;
            s->it[w] = s->ioff[w];
            s->stk[sp++] = (uint32_t)w;
            pushed = true;
            break;
        }
        if (!pushed) { s->lim[v] = next++; sp--; }
    }
}

/* enumerate subtree(v) into buf via tree edges; returns count */
static size_t subtree(NS *s, int32_t v, uint32_t *buf) {
    size_t n = 0, sp = 0;
    buf[n++] = (uint32_t)v;
    s->stk[sp++] = (uint32_t)v;
    while (sp) {
        int32_t x = (int32_t)s->stk[--sp];
        for (uint32_t k = s->ioff[x]; k < s->ioff[x + 1]; k++) {
            uint32_t e = s->iidx[k];
            if (!s->intree_e[e] || (int32_t)e == s->pe[x]) continue;
            int32_t w = other(s, e, x);
            buf[n++] = (uint32_t)w;
            s->stk[sp++] = (uint32_t)w;
        }
    }
    return n;
}

static void invalidate_path(NS *s, int32_t lca, int32_t v) {
    for (;;) {
        if (s->low[v] == -1) break;
        s->low[v] = -1;
        if (s->pe[v] < 0 || v == lca) break;
        uint32_t e = (uint32_t)s->pe[v];
        int32_t a = s->eu[e], b = s->ev[e];
        v = s->lim[a] > s->lim[b] ? a : b;
    }
}

static inline bool in_sub(const NS *s, int32_t v, int32_t root) {
    return s->low[root] <= s->lim[v] && s->lim[v] <= s->lim[root];
}

static int32_t tree_update(NS *s, int32_t v, int32_t w, int64_t cv, bool dir) {
    while (!in_sub(s, w, v)) {
        uint32_t e = (uint32_t)s->pe[v];
        bool d = s->eu[e] == v ? dir : !dir;
        s->cut[e] += d ? cv : -cv;
        int32_t a = s->eu[e], b = s->ev[e];
        v = s->lim[a] > s->lim[b] ? a : b;
    }
    return v;
}

static inline bool is_desc(const NS *s, int32_t v, int32_t root) {
    return s->low[root] <= s->lim[v] && s->lim[v] <= s->lim[root];
}

static void run_component(NS *s, size_t max_iter) {
#ifdef CMM_PROFILE
    double t0 = ns_now();
    size_t iters = 0;
#endif
    int32_t root = s->comp[0];
    s->intree_v[root] = 1;
    size_t intree = tight_tree(s);
    size_t tn = 0;
    for (size_t i = 0; i < s->nc; i++) if (s->intree_v[s->comp[i]]) s->it[tn++] = (uint32_t)s->comp[i];
    while (intree < s->nc) {
        int32_t best = INT32_MAX;
        uint32_t be = 0;
        for (size_t i = 0; i < s->ncE; i++) {
            uint32_t e = s->cedges[i];
            if (s->intree_v[s->eu[e]] == s->intree_v[s->ev[e]]) continue;
            int32_t sl = slack(s, e);
            if (sl < best) { best = sl; be = e; }
        }
        if (best == INT32_MAX) return; /* disconnected: should not happen */
        int32_t delta = s->intree_v[s->eu[be]] ? best : -best;
        if (delta)
            for (size_t i = 0; i < tn; i++) s->rank[s->it[i]] += delta;
        /* grow the tree from the newly tight edge's outside endpoint */
        int32_t nv = s->intree_v[s->eu[be]] ? s->ev[be] : s->eu[be];
        s->intree_v[nv] = 1;
        s->intree_e[be] = 1;
        s->it[tn++] = (uint32_t)nv;
        intree++;
        size_t sp = 0;
        s->stk[sp++] = (uint32_t)nv;
        while (sp) {
            int32_t v = (int32_t)s->stk[--sp];
            for (uint32_t k = s->ioff[v]; k < s->ioff[v + 1]; k++) {
                uint32_t e = s->iidx[k];
                int32_t w = other(s, e, v);
                if (!s->intree_v[w] && slack(s, e) == 0) {
                    s->intree_v[w] = 1;
                    s->intree_e[e] = 1;
                    s->stk[sp++] = (uint32_t)w;
                    s->it[tn++] = (uint32_t)w;
                    intree++;
                }
            }
        }
    }
#ifdef CMM_PROFILE
    double t1 = ns_now();
#endif
    s->pe[root] = -1;
    low_lim(s, root, 1, 0);
    cut_values(s, 0, s->nc);
    size_t lptr = 0;
    for (size_t iter = 0; iter < max_iter; iter++) {
        uint32_t le = UINT32_MAX;
        for (size_t k = 0; k < s->ncE; k++) {
            uint32_t e = s->cedges[(lptr + k) % s->ncE];
            if (s->intree_e[e] && s->cut[e] < 0) { le = e; lptr = (lptr + k + 1) % s->ncE; break; }
        }
#ifdef CMM_PROFILE
        iters++;
#endif
        if (le == UINT32_MAX) break;
        int32_t v = s->eu[le], w = s->ev[le], tail = v;
        bool flip = false;
        if (s->lim[v] > s->lim[w]) { tail = w; flip = true; }
        /* candidates have exactly one endpoint inside subtree(tail): scan that subtree only */
        int32_t best = INT32_MAX;
        uint32_t fe = UINT32_MAX;
        size_t sn = subtree(s, tail, s->order);
        for (size_t i = 0; i < sn; i++) {
            int32_t x = (int32_t)s->order[i];
            for (uint32_t k = s->ioff[x]; k < s->ioff[x + 1]; k++) {
                uint32_t e = s->iidx[k];
                if (flip == is_desc(s, s->eu[e], tail) && flip != is_desc(s, s->ev[e], tail)) {
                    int32_t sl = slack(s, e);
                    if (sl < best || (sl == best && e < fe)) { best = sl; fe = e; }
                }
            }
        }
        if (fe == UINT32_MAX) break;
        int32_t fu = s->eu[fe], fw = s->ev[fe];
        int32_t sh = is_desc(s, fu, tail) ? best : -best;
        if (sh)
            for (size_t i = 0; i < sn; i++) s->rank[s->order[i]] += sh;
        int64_t cv = s->cut[le];
        int32_t l = tree_update(s, fu, fw, cv, true);
        (void)tree_update(s, fw, fu, cv, false);
        s->cut[fe] = -cv;
        s->cut[le] = 0;
        int32_t llow = s->low[l];
        invalidate_path(s, l, fu);
        invalidate_path(s, l, fw);
        s->intree_e[le] = 0;
        s->intree_e[fe] = 1;
        dfs_range(s, l, s->pe[l], llow);
    }
#ifdef CMM_PROFILE
    if (s->nc > 100 && getenv("CMM_PROFV")) fprintf(stderr, "  ns: nc=%zu feasible=%.2fms simplex=%.2fms iters=%zu\n", s->nc, t1 - t0, ns_now() - t1, iters);
#endif
}

bool network_simplex(Arena *A, size_t N, size_t E, const int32_t eu[], const int32_t ev[],
                     const int32_t minlen[], const uint8_t valid[], const uint8_t hidden[],
                     int32_t rank[], size_t max_iter) {
    NS s = {.N = N, .E = E, .eu = eu, .ev = ev, .minlen = minlen, .valid = valid, .rank = rank};
    uint32_t *ioff = ANEW(A, uint32_t, N + 1), *iidx = ANEW(A, uint32_t, 2 * E + 1);
    uint32_t *fill = ANEW(A, uint32_t, N + 1);
    s.intree_e = ANEW(A, uint8_t, E + 1); s.intree_v = ANEW(A, uint8_t, N + 1);
    s.pre = ANEW(A, int32_t, N + 1); s.pe = ANEW(A, int32_t, N + 1); s.low = ANEW(A, int32_t, N + 1); s.lim = ANEW(A, int32_t, N + 1);
    s.cut = ANEW(A, int64_t, E + 1);
    s.stk = ANEW(A, uint32_t, N + 1); s.it = ANEW(A, uint32_t, N + 1); s.order = ANEW(A, uint32_t, N + 1);
    int32_t *comp = ANEW(A, int32_t, N + 1);
    uint32_t *cedges = ANEW(A, uint32_t, E + 1);
    uint8_t *seen = ANEW(A, uint8_t, N + 1), *eseen = ANEW(A, uint8_t, E + 1);
    if (!ioff || !iidx || !fill || !s.intree_e || !s.intree_v || !s.pre || !s.pe || !s.low || !s.lim || !s.cut ||
        !s.stk || !s.it || !s.order || !comp || !cedges || !seen || !eseen)
        return false;
    memset(ioff, 0, (N + 1) * sizeof *ioff);
    for (size_t e = 0; e < E; e++) if (valid[e]) { ioff[eu[e] + 1]++; ioff[ev[e] + 1]++; }
    for (size_t v = 0; v < N; v++) ioff[v + 1] += ioff[v];
    memcpy(fill, ioff, N * sizeof *fill);
    for (size_t e = 0; e < E; e++) if (valid[e]) { iidx[fill[eu[e]]++] = (uint32_t)e; iidx[fill[ev[e]]++] = (uint32_t)e; }
    s.ioff = ioff, s.iidx = iidx;
    memset(s.intree_e, 0, E + 1);
    memset(s.intree_v, 0, N + 1);
    memset(seen, 0, N + 1);
    memset(eseen, 0, E + 1);
    for (size_t r = 0; r < N; r++) {
        if (seen[r] || hidden[r]) continue;
        /* gather component via BFS (comp doubles as queue) */
        size_t nc = 0, ne = 0, qh = 0;
        comp[nc++] = (int32_t)r;
        seen[r] = 1;
        while (qh < nc) {
            int32_t v = comp[qh++];
            for (uint32_t k = ioff[v]; k < ioff[v + 1]; k++) {
                uint32_t e = iidx[k];
                if (!eseen[e]) { eseen[e] = 1; cedges[ne++] = e; }
                int32_t w = eu[e] == v ? ev[e] : eu[e];
                if (!seen[w]) { seen[w] = 1; comp[nc++] = w; }
            }
        }
        if (nc < 2) continue;
        s.comp = comp, s.nc = nc, s.cedges = cedges, s.ncE = ne;
        run_component(&s, max_iter);
    }
    return true;
}

/* ---- Eades–Lin–Smyth greedy feedback arc set, O(V+E) with degree buckets ---- */
typedef struct FAS {
    int32_t *head, *nxt, *prv, *bk;
    int32_t *in, *out;
    int32_t maxd;
} FAS;

static inline int32_t fas_bucket(const FAS *f, int32_t v) {
    if (f->out[v] == 0) return 0;
    if (f->in[v] == 0) return 1;
    return 2 + f->out[v] - f->in[v] + f->maxd;
}
static inline void fas_unlink(FAS *f, int32_t v) {
    int32_t b = f->bk[v];
    if (f->prv[v] >= 0) f->nxt[f->prv[v]] = f->nxt[v]; else f->head[b] = f->nxt[v];
    if (f->nxt[v] >= 0) f->prv[f->nxt[v]] = f->prv[v];
}
static inline void fas_link(FAS *f, int32_t v) {
    int32_t b = fas_bucket(f, v);
    f->bk[v] = b;
    f->prv[v] = -1;
    f->nxt[v] = f->head[b];
    if (f->head[b] >= 0) f->prv[f->head[b]] = v;
    f->head[b] = v;
}

bool greedy_fas(Arena *A, size_t N, size_t E, const int32_t ef[], const int32_t et[], const uint8_t valid[],
                const uint8_t hidden[], uint8_t rev[]) {
    uint32_t *ooff = ANEW(A, uint32_t, N + 1), *ioff = ANEW(A, uint32_t, N + 1);
    uint32_t *oidx = ANEW(A, uint32_t, E + 1), *iidx = ANEW(A, uint32_t, E + 1), *fill = ANEW(A, uint32_t, N + 1);
    int32_t *ord = ANEW(A, int32_t, N + 1);
    uint8_t *gone = ANEW(A, uint8_t, N + 1);
    FAS f = {.nxt = ANEW(A, int32_t, N + 1), .prv = ANEW(A, int32_t, N + 1), .bk = ANEW(A, int32_t, N + 1),
             .in = ANEW(A, int32_t, N + 1), .out = ANEW(A, int32_t, N + 1)};
    if (!ooff || !ioff || !oidx || !iidx || !fill || !ord || !gone || !f.nxt || !f.prv || !f.bk || !f.in || !f.out)
        return false;
    memset(ooff, 0, (N + 1) * sizeof *ooff);
    memset(ioff, 0, (N + 1) * sizeof *ioff);
    for (size_t e = 0; e < E; e++) if (valid[e]) { ooff[ef[e] + 1]++; ioff[et[e] + 1]++; }
    int32_t maxd = 0;
    for (size_t v = 0; v < N; v++) {
        f.out[v] = (int32_t)ooff[v + 1];
        f.in[v] = (int32_t)ioff[v + 1];
        if (f.out[v] > maxd) maxd = f.out[v];
        if (f.in[v] > maxd) maxd = f.in[v];
        ooff[v + 1] += ooff[v];
        ioff[v + 1] += ioff[v];
    }
    memcpy(fill, ooff, N * sizeof *fill);
    for (size_t e = 0; e < E; e++) if (valid[e]) oidx[fill[ef[e]]++] = (uint32_t)e;
    memcpy(fill, ioff, N * sizeof *fill);
    for (size_t e = 0; e < E; e++) if (valid[e]) iidx[fill[et[e]]++] = (uint32_t)e;
    f.maxd = maxd;
    size_t nb = 2 * (size_t)maxd + 3;
    f.head = ANEW(A, int32_t, nb);
    if (!f.head) return false;
    for (size_t b = 0; b < nb; b++) f.head[b] = -1;
    size_t live = 0;
    for (size_t v = 0; v < N; v++) {
        gone[v] = hidden[v];
        if (!hidden[v]) { fas_link(&f, (int32_t)v); live++; }
    }
    int32_t left = 0, right = (int32_t)live - 1;
    int32_t top = (int32_t)nb - 1;
    while (live) {
        int32_t v;
        bool to_right = false;
        if (f.head[0] >= 0) { v = f.head[0]; to_right = true; }
        else if (f.head[1] >= 0) v = f.head[1];
        else {
            while (top > 1 && f.head[top] < 0) top--;
            v = f.head[top];
            if (v < 0) break;
        }
        fas_unlink(&f, v);
        gone[v] = 1;
        live--;
        ord[v] = to_right ? right-- : left++;
        for (uint32_t k = ooff[v]; k < ooff[v + 1]; k++) {
            int32_t w = et[oidx[k]];
            if (gone[w]) continue;
            fas_unlink(&f, w); f.in[w]--; fas_link(&f, w);
        }
        for (uint32_t k = ioff[v]; k < ioff[v + 1]; k++) {
            int32_t u = ef[iidx[k]];
            if (gone[u]) continue;
            fas_unlink(&f, u); f.out[u]--; fas_link(&f, u);
            if (f.bk[u] > top) top = f.bk[u];
        }
    }
    for (size_t e = 0; e < E; e++) rev[e] = valid[e] && ord[ef[e]] > ord[et[e]];
    return true;
}
