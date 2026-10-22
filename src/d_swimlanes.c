/* swimlane-beta: mermaid's "layout-variant" of flowchart. Parsing, node sizing and drawing are the
 * flowchart's; this file only replaces the layout: top-level subgraphs become full-width lanes
 * (rows for LR/RL, columns for TB/BT) with a rotated title band, nodes sit in rank columns where only
 * intra-lane edges advance the rank (cross-lane edges may stay in the same column, as in mermaid),
 * and edges are routed orthogonally. */
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "diagrams.h"
#include "graph.h"

constexpr double SW_TITLE = 32; /* drawn title rect (layout band is 36) */

static int32_t top_lane(const Graph *g, int32_t s) {
    while (s != NONE && g->sparent[s] != NONE) s = g->sparent[s];
    return s;
}

/* mermaid's String.localeCompare, approximated: case-insensitive first, then lower before upper */
static int key_cmp(Str a, Str b) {
    size_t n = a.n < b.n ? a.n : b.n;
    for (size_t i = 0; i < n; i++) {
        int x = tolower((unsigned char)a.p[i]), y = tolower((unsigned char)b.p[i]);
        if (x != y) return x - y;
    }
    if (a.n != b.n) return a.n < b.n ? -1 : 1;
    for (size_t i = 0; i < n; i++)
        if (a.p[i] != b.p[i]) return islower((unsigned char)a.p[i]) ? -1 : 1;
    return 0;
}

static const Str *sort_keys;
static int idx_cmp(const void *x, const void *y) {
    int32_t i = *(const int32_t *)x, j = *(const int32_t *)y;
    int c = key_cmp(sort_keys[i], sort_keys[j]);
    return c ? c : i - j;
}
static void sort_idx(size_t n, int32_t v[static n], const Str *keys) {
    if (n < 2) return;
    sort_keys = keys;
    qsort(v, n, sizeof *v, idx_cmp);
}

static Str cat_key(Arena *a, Str p0, Str p1, Str p2, Str p3) {
    size_t n = 11 + p1.n + 1 + p2.n + 1 + p3.n;
    char *s = ANEW(a, char, n + 1);
    if (!s) return (Str){};
    size_t k = 0;
    memcpy(s, p0.p, 11), k = 11;
    memcpy(s + k, p1.p, p1.n), k += p1.n, s[k++] = '-';
    memcpy(s + k, p2.p, p2.n), k += p2.n, s[k++] = '-';
    memcpy(s + k, p3.p, p3.n), k += p3.n;
    return (Str){s, k};
}

cmm_status swim_layout(Graph *g, Str **keep) {
    Arena *a = g->arena;
    size_t nn = g->nn, ns = g->ns, ne = g->ne;
    bool horiz = g->dir == DIR_LR || g->dir == DIR_RL; /* lanes are rows */
    size_t cap = nn + ne + 1;
    /* layout items: visible nodes plus one node per labelled edge (mermaid's edge-label nodes) */
    int32_t *lix = ANEW(a, int32_t, ns + 1), *inode = ANEW(a, int32_t, cap), *iof = ANEW(a, int32_t, nn + 1);
    int32_t *ilane = ANEW(a, int32_t, cap), *rank = ANEW(a, int32_t, cap), *base = ANEW(a, int32_t, cap);
    int32_t *es = ANEW(a, int32_t, 2 * ne + 1), *ed = ANEW(a, int32_t, 2 * ne + 1);
    double *iw = ANEW(a, double, cap), *ih = ANEW(a, double, cap), *ix = ANEW(a, double, cap), *iy = ANEW(a, double, cap);
    Str *ikey = ANEW(a, Str, cap);
    if (!lix || !inode || !iof || !ilane || !rank || !base || !es || !ed || !iw || !ih || !ix || !iy || !ikey) return CMM_ERR_NOMEM;
    int32_t nl = 0;
    for (size_t s = 0; s < ns; s++) lix[s] = g->sparent[s] == NONE ? nl++ : -1;
    size_t m = 0;
    for (size_t v = 0; v < nn; v++) {
        iof[v] = -1;
        if (g->nflags[v] & NF_HIDDEN) continue;
        int32_t t = top_lane(g, g->nparent[v]);
        iof[v] = (int32_t)m, inode[m] = (int32_t)v, ilane[m] = t == NONE ? -1 : lix[t];
        iw[m] = g->nw[v], ih[m] = g->nh[v], ikey[m] = g->nid[v], m++;
    }
    size_t me = 0;
    for (size_t e = 0; e < ne; e++) {
        int32_t f = g->efrom[e], t = g->eto[e];
        if (f < 0 || t < 0 || (size_t)f >= nn || (size_t)t >= nn || iof[f] < 0 || iof[t] < 0) continue;
        if (!g->elabel[e].n) { es[me] = iof[f], ed[me++] = iof[t]; continue; }
        int32_t lf = ilane[iof[f]], lt = ilane[iof[t]];
        inode[m] = -1, ilane[m] = lf != lt ? lt : lf, iw[m] = g->elw[e], ih[m] = g->elh[e];
        ikey[m] = cat_key(a, S("edge-label-"), g->nid[f], g->nid[t], g->eid[e].n ? g->eid[e] : S("L"));
        if (!ikey[m].p) return CMM_ERR_NOMEM;
        es[me] = iof[f], ed[me++] = (int32_t)m, es[me] = (int32_t)m, ed[me++] = iof[t], m++;
    }
    /* CSR adjacency (successors sorted by key) */
    int32_t *off = ANEW(a, int32_t, m + 2), *adj = ANEW(a, int32_t, me + 1), *aed = ANEW(a, int32_t, me + 1);
    int32_t *perm = ANEW(a, int32_t, m + 1), *pos = ANEW(a, int32_t, m + 1), *stk = ANEW(a, int32_t, m + 1), *it = ANEW(a, int32_t, m + 1);
    uint8_t *col = ANEW(a, uint8_t, m + 1);
    if (!off || !adj || !aed || !perm || !pos || !stk || !it || !col) return CMM_ERR_NOMEM;
    for (size_t i = 0; i < m; i++) perm[i] = (int32_t)i;
    sort_idx(m, perm, ikey);
    for (size_t i = 0; i < m; i++) pos[perm[i]] = (int32_t)i;
    memset(off, 0, (m + 2) * sizeof *off);
    for (size_t k = 0; k < me; k++) off[es[k] + 1]++;
    for (size_t i = 0; i < m; i++) off[i + 1] += off[i];
    for (size_t i = 0; i < m; i++) it[i] = off[i];
    for (size_t k = 0; k < me; k++) aed[it[es[k]]++] = (int32_t)k;
    for (size_t i = 0; i < m; i++) /* insertion sort by destination key rank */
        for (int32_t p = off[i] + 1; p < off[i + 1]; p++)
            for (int32_t q = p; q > off[i] && pos[ed[aed[q - 1]]] > pos[ed[aed[q]]]; q--) {
                int32_t tmp = aed[q]; aed[q] = aed[q - 1], aed[q - 1] = tmp;
            }
    /* removeCycles_DFS: reverse edges closing a cycle */
    memset(col, 0, m + 1);
    for (size_t r = 0; r < m; r++) {
        int32_t s0 = perm[r];
        if (col[s0]) continue;
        size_t sp = 0;
        stk[sp] = s0, it[sp++] = off[s0], col[s0] = 1;
        while (sp) {
            int32_t u = stk[sp - 1];
            if (it[sp - 1] >= off[u + 1]) { col[u] = 2, sp--; continue; }
            int32_t k = aed[it[sp - 1]++], v = ed[k];
            if (col[v] == 1) { if (v != u) ed[k] = u, es[k] = v; else es[k] = -1; }
            else if (!col[v]) stk[sp] = v, it[sp++] = off[v], col[v] = 1;
        }
    }
    /* rebuild adjacency after reversal */
    memset(off, 0, (m + 2) * sizeof *off);
    for (size_t k = 0; k < me; k++) if (es[k] >= 0) off[es[k] + 1]++;
    for (size_t i = 0; i < m; i++) off[i + 1] += off[i];
    for (size_t i = 0; i < m; i++) it[i] = off[i];
    for (size_t k = 0; k < me; k++) if (es[k] >= 0) aed[it[es[k]]++] = (int32_t)k;
    /* topological order: generations for LR/RL, a key-ordered queue otherwise; then lane-aware compact
     * layering (rank = max(pred + same-lane?1:0, next free slot of the lane)) */
    int32_t *indeg = it, *order = stk, *nextfree = ANEW(a, int32_t, (size_t)nl + 1);
    if (!nextfree) return CMM_ERR_NOMEM;
    memset(indeg, 0, (m + 1) * sizeof *indeg), memset(nextfree, 0, ((size_t)nl + 1) * sizeof *nextfree);
    memset(base, 0, m * sizeof *base);
    for (size_t k = 0; k < me; k++) if (es[k] >= 0) indeg[ed[k]]++;
    size_t no = 0, head = 0;
    for (size_t r = 0; r < m; r++) if (!indeg[perm[r]]) order[no++] = perm[r];
    while (head < no) {
        size_t gen_end = horiz ? no : head + 1, gen_start = no;
        for (; head < gen_end; head++) {
            int32_t u = order[head];
            int32_t L = base[u];
            if (ilane[u] >= 0 && nextfree[ilane[u]] > L) L = nextfree[ilane[u]];
            rank[u] = L;
            if (ilane[u] >= 0) nextfree[ilane[u]] = L + 1;
            for (int32_t p = off[u]; p < off[u + 1]; p++) {
                int32_t v = ed[aed[p]];
                int32_t w = ilane[u] >= 0 && ilane[u] == ilane[v] ? 1 : 0;
                if (L + w > base[v]) base[v] = L + w;
                if (--indeg[v] == 0) {
                    if (horiz) order[no++] = v;
                    else { /* sorted insert into the pending queue */
                        size_t q = no++;
                        while (q > head + 1 && pos[order[q - 1]] > pos[v]) order[q] = order[q - 1], q--;
                        order[q] = v;
                    }
                }
            }
        }
        if (horiz) sort_idx(no - gen_start, order + gen_start, ikey);
    }
    for (size_t i = no; i < m; i++) rank[i] = 0; /* unreachable (cannot happen once acyclic) */
    int32_t nr = 1;
    for (size_t i = 0; i < m; i++) if (rank[i] + 1 > nr) nr = rank[i] + 1;
    /* assignCoordinates (TB space: lanes are columns of width = widest lane slice, layers are rows) */
    size_t NL = (size_t)nl + 1; /* column 0 = loose nodes */
    double *lh = ANEW(a, double, (size_t)nr), *lw = ANEW(a, double, (size_t)nr), *ly = ANEW(a, double, (size_t)nr);
    double *slice = ANEW(a, double, NL * (size_t)nr), *colw = ANEW(a, double, NL), *colc = ANEW(a, double, NL);
    int32_t *scnt = ANEW(a, int32_t, NL * (size_t)nr);
    if (!lh || !lw || !ly || !slice || !colw || !colc || !scnt) return CMM_ERR_NOMEM;
    memset(lh, 0, (size_t)nr * sizeof *lh), memset(lw, 0, (size_t)nr * sizeof *lw);
    memset(slice, 0, NL * (size_t)nr * sizeof *slice), memset(colw, 0, NL * sizeof *colw);
    memset(scnt, 0, NL * (size_t)nr * sizeof *scnt);
    double ng = g->node_sep, lg = g->rank_sep;
    for (size_t i = 0; i < m; i++) {
        size_t c = (size_t)(ilane[i] + 1) * (size_t)nr + (size_t)rank[i];
        lh[rank[i]] = fmax(lh[rank[i]], ih[i]), lw[rank[i]] = fmax(lw[rank[i]], iw[i]);
        slice[c] += iw[i] + (scnt[c]++ ? ng : 0);
    }
    for (size_t c = 0; c < NL; c++)
        for (int32_t r = 0; r < nr; r++) colw[c] = fmax(colw[c], slice[c * (size_t)nr + (size_t)r]);
    double cur = 0;
    for (size_t c = 0; c < NL; c++) {
        if (colw[c] <= 0 && c == 0) { colc[c] = 0; continue; }
        colc[c] = cur + colw[c] / 2, cur += colw[c] + 2 * ng;
    }
    double yo = 0;
    for (int32_t r = 0; r < nr; r++) {
        ly[r] = yo + lh[r] / 2;
        double extra = 0;
        if (horiz && r + 1 < nr) extra = fmax(0, (lw[r] + lw[r + 1]) / 2 - (lh[r] + lh[r + 1]) / 2 - lg);
        yo += lh[r] + lg + extra;
    }
    memset(slice, 0, NL * (size_t)nr * sizeof *slice);
    for (size_t k = 0; k < no; k++) { /* spread a lane slice around the lane centre, in layering order */
        int32_t i = order[k];
        size_t c = (size_t)(ilane[i] + 1) * (size_t)nr + (size_t)rank[i];
        double tot = 0;
        for (size_t j = 0; j < m; j++) if (ilane[j] == ilane[i] && rank[j] == rank[i]) tot += iw[j];
        tot += ng * (scnt[c] - 1);
        ix[i] = colc[ilane[i] + 1] - tot / 2 + slice[c] + iw[i] / 2, slice[c] += iw[i] + ng;
        iy[i] = ly[rank[i]];
    }
    /* LR: rotate (layers -> x, scaled by the average aspect ratio; lanes -> y) */
    double sc = 1;
    if (horiz && m) {
        double tw = 0, th = 0;
        for (size_t i = 0; i < m; i++) tw += iw[i], th += ih[i];
        if (th > 0) sc = fmax(1, tw / th);
    }
    double *ext_lo = ANEW(a, double, NL), *ext_hi = ANEW(a, double, NL);
    if (!ext_lo || !ext_hi) return CMM_ERR_NOMEM;
    double flo0 = INFINITY, fhi0 = -INFINITY;
    for (size_t c = 0; c < NL; c++) ext_lo[c] = INFINITY, ext_hi[c] = -INFINITY;
    for (size_t i = 0; i < m; i++) {
        double u = iy[i] * sc, w = ix[i]; /* u = flow axis, w = lane axis */
        if (g->dir == DIR_RL || g->dir == DIR_BT) u = -u;
        ix[i] = u, iy[i] = w;
        double fm = (horiz ? iw[i] : ih[i]) / 2, cm = (horiz ? ih[i] : iw[i]) / 2;
        flo0 = fmin(flo0, u - fm), fhi0 = fmax(fhi0, u + fm);
        size_t c = (size_t)(ilane[i] + 1);
        ext_lo[c] = fmin(ext_lo[c], w - cm), ext_hi[c] = fmax(ext_hi[c], w + cm);
    }
    if (!m) flo0 = 0, fhi0 = 0;
    /* lane bands: boundaries halfway between neighbouring lanes' content, outer margin 36; along the
     * flow axis a 36px title band + 10px body margin */
    constexpr double SW_BAND = 36;
    constexpr double SW_MARG = 10;
    double *b0 = ANEW(a, double, NL), *b1 = ANEW(a, double, NL);
    if (!b0 || !b1) return CMM_ERR_NOMEM;
    double prev_hi = NAN, minc = INFINITY;
    int32_t prev = -1;
    for (size_t c = 1; c < NL; c++) {
        if (ext_lo[c] > ext_hi[c]) { /* empty lane */
            double at = isnan(prev_hi) ? (ext_hi[0] > ext_lo[0] ? ext_hi[0] : 0) + SW_BAND : prev_hi + SW_BAND;
            ext_lo[c] = at, ext_hi[c] = at;
        }
        if (prev < 0) b0[c] = ext_lo[c] - SW_BAND;
        else b0[c] = b1[prev] = (prev_hi + ext_lo[c]) / 2;
        prev_hi = ext_hi[c], prev = (int32_t)c;
    }
    if (prev >= 0) b1[prev] = prev_hi + SW_BAND;
    double flo = flo0 - SW_MARG - SW_BAND, fhi = fhi0 + SW_MARG;
    for (size_t c = 1; c < NL; c++) minc = fmin(minc, b0[c]);
    if (ext_hi[0] >= ext_lo[0]) minc = fmin(minc, ext_lo[0]);
    if (!isfinite(minc)) minc = 0;
    for (size_t i = 0; i < m; i++) {
        double u = ix[i] - flo, w = iy[i] - minc;
        if (inode[i] < 0) continue;
        if (horiz) g->nx[inode[i]] = u, g->ny[inode[i]] = w;
        else g->nx[inode[i]] = w, g->ny[inode[i]] = u;
    }
    double mainlen = fhi - flo;
    for (size_t s = 0; s < ns; s++) {
        if (lix[s] < 0) { g->sw[s] = 0, g->sh[s] = 0; continue; }
        size_t c = (size_t)lix[s] + 1;
        double c0 = b0[c] - minc, c1 = b1[c] - minc;
        if (horiz) g->sx[s] = 0, g->sy[s] = c0, g->sw[s] = mainlen, g->sh[s] = c1 - c0;
        else g->sx[s] = c0, g->sy[s] = 0, g->sw[s] = c1 - c0, g->sh[s] = mainlen;
        if (g->stx) g->stx[s] = g->sx[s] + g->sw[s] / 2;
    }
    /* orthogonal edges, in (main, cross) space then mapped */
    g->curve = CURVE_LINEAR;
    for (size_t e = 0; e < g->ne; e++) {
        int32_t f = g->efrom[e], t = g->eto[e];
        if (f == t || f < 0 || t < 0 || (size_t)f >= nn || (size_t)t >= nn) continue;
        double fu = horiz ? g->nx[f] : g->ny[f], fw = horiz ? g->ny[f] : g->nx[f];
        double tu = horiz ? g->nx[t] : g->ny[t], tw = horiz ? g->ny[t] : g->nx[t];
        double fm = (horiz ? g->nw[f] : g->nh[f]) / 2, fc = (horiz ? g->nh[f] : g->nw[f]) / 2;
        double tm = (horiz ? g->nw[t] : g->nh[t]) / 2, tc = (horiz ? g->nh[t] : g->nw[t]) / 2;
        double P[8];
        uint32_t np;
        if (fabs(fw - tw) < 0.5) {
            double sg = tu >= fu ? 1 : -1;
            P[0] = fu + sg * fm, P[1] = fw, P[2] = tu - sg * tm, P[3] = tw, np = 2;
        } else if (fabs(fu - tu) < 0.5) {
            double sg = tw >= fw ? 1 : -1;
            P[0] = fu, P[1] = fw + sg * fc, P[2] = tu, P[3] = tw - sg * tc, np = 2;
        } else {
            double sg = tu >= fu ? 1 : -1, sw = tw >= fw ? 1 : -1;
            P[0] = fu + sg * fm, P[1] = fw, P[2] = tu, P[3] = fw, P[4] = tu, P[5] = tw - sw * tc, np = 3;
        }
        g->eptoff[e] = (uint32_t)g->pts.n, g->eptn[e] = np;
        for (uint32_t k = 0; k < np; k++) {
            double px = horiz ? P[2 * k] : P[2 * k + 1], py = horiz ? P[2 * k + 1] : P[2 * k];
            if (!VEC_PUSH(a, &g->pts, px) || !VEC_PUSH(a, &g->pts, py)) return CMM_ERR_NOMEM;
        }
        /* label: middle of the longest segment */
        const double *Q = g->pts.v + g->eptoff[e];
        double best = -1;
        for (uint32_t k = 0; k + 1 < np; k++) {
            double d = fabs(Q[2 * k + 2] - Q[2 * k]) + fabs(Q[2 * k + 3] - Q[2 * k + 1]);
            if (d > best) best = d, g->elx[e] = (Q[2 * k] + Q[2 * k + 2]) / 2, g->ely[e] = (Q[2 * k + 1] + Q[2 * k + 3]) / 2;
        }
    }
    /* titles are drawn by swim_titles (rotated for row lanes) */
    Str *k = ANEW(a, Str, ns + 1);
    if (!k) return CMM_ERR_NOMEM;
    for (size_t s = 0; s < ns; s++) {
        k[s] = g->stitle[s];
        if (lix[s] >= 0 && horiz) g->stitle[s] = (Str){};
    }
    *keep = k;
    return CMM_OK;
}

/* Insert the rotated lane titles of row lanes before </svg>. */
cmm_status swim_titles(const Graph *g, const Str *keep, Buf *b) {
    bool horiz = g->dir == DIR_LR || g->dir == DIR_RL;
    if (!horiz || b->n < 6) return CMM_OK;
    Buf t = {};
    for (size_t s = 0; s < g->ns; s++) {
        if (g->sparent[s] != NONE || g->sw[s] <= 0) continue;
        double cx = g->sx[s] + SW_TITLE / 2, cy = g->sy[s] + g->sh[s] / 2;
        buf_lit(&t, "<text class=\"swimlane-title\" fill=\"#333\" font-size=\"14\" text-anchor=\"middle\" dominant-baseline=\"central\" transform=\"translate(");
        buf_pt(&t, cx, cy); buf_lit(&t, ") rotate(-90)\">");
        buf_xml(&t, keep[s]); buf_lit(&t, "</text>");
        if (g->sw[s] > 0) {
            buf_lit(&t, "<line class=\"swimlane-sep\" stroke=\"#9370DB\" stroke-width=\"1\" x1=\""); buf_num(&t, g->sx[s] + SW_TITLE);
            buf_lit(&t, "\" y1=\""); buf_num(&t, g->sy[s]); buf_lit(&t, "\" x2=\""); buf_num(&t, g->sx[s] + SW_TITLE);
            buf_lit(&t, "\" y2=\""); buf_num(&t, g->sy[s] + g->sh[s]); buf_lit(&t, "\"/>");
        }
    }
    bool oom = t.oom;
    size_t at = b->n - 6; /* last "</svg>" */
    while (at > 0 && memcmp(b->p + at, "</svg>", 6) != 0) at--;
    if (!oom && t.n && buf_reserve(b, t.n + 1)) {
        memmove(b->p + at + t.n, b->p + at, b->n - at);
        memcpy(b->p + at, t.p, t.n);
        b->n += t.n;
    } else if (t.n) oom = true;
    free(t.p);
    return oom ? CMM_ERR_NOMEM : CMM_OK;
}
