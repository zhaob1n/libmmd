/* swimlane-beta: mermaid's "layout-variant" of flowchart. Parsing, node sizing and drawing are the
 * flowchart's; this file only replaces the layout: top-level subgraphs become full-width lanes
 * (rows for LR/RL, columns for TB/BT) with a rotated title band, nodes sit in rank columns where only
 * intra-lane edges advance the rank (cross-lane edges may stay in the same column, as in mermaid),
 * and edges are routed orthogonally. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "diagrams.h"
#include "graph.h"

constexpr double SW_HEAD = 30;
constexpr double SW_PAD = 20;

static int32_t top_lane(const Graph *g, int32_t s) {
    while (s != NONE && g->sparent[s] != NONE) s = g->sparent[s];
    return s;
}

cmm_status swim_layout(Graph *g, Str **keep) {
    Arena *a = g->arena;
    size_t nn = g->nn, ns = g->ns;
    bool horiz = g->dir == DIR_LR || g->dir == DIR_RL; /* lanes are rows */
    int32_t *lane = ANEW(a, int32_t, nn + 1), *rank = ANEW(a, int32_t, nn + 1), *lix = ANEW(a, int32_t, ns + 1);
    uint8_t *back = ANEW(a, uint8_t, g->ne + 1), *mark = ANEW(a, uint8_t, nn + 1);
    int32_t *stk = ANEW(a, int32_t, nn + 1), *eit = ANEW(a, int32_t, nn + 1);
    if (!lane || !rank || !lix || !back || !mark || !stk || !eit) return CMM_ERR_NOMEM;
    memset(rank, 0, (nn + 1) * sizeof *rank), memset(back, 0, g->ne + 1), memset(mark, 0, nn + 1);
    /* lane order: top-level subgraphs in declaration order, then an implicit lane for loose nodes */
    int32_t nl = 0;
    for (size_t s = 0; s < ns; s++) lix[s] = g->sparent[s] == NONE ? nl++ : -1;
    bool loose = false;
    for (size_t v = 0; v < nn; v++) {
        int32_t t = top_lane(g, g->nparent[v]);
        lane[v] = t == NONE ? -1 : lix[t];
        if (lane[v] < 0 && !(g->nflags[v] & NF_HIDDEN)) loose = true;
    }
    int32_t L = nl + (loose ? 1 : 0);
    if (L == 0) L = 1;
    for (size_t v = 0; v < nn; v++) if (lane[v] < 0) lane[v] = L - 1;
    /* back edges by iterative DFS (state 1 = on stack) */
    for (size_t r = 0; r < nn; r++) {
        if (mark[r]) continue;
        size_t sp = 0;
        stk[sp] = (int32_t)r, eit[sp++] = 0, mark[r] = 1;
        while (sp) {
            int32_t v = stk[sp - 1];
            int32_t e = eit[sp - 1]++;
            if ((size_t)e >= g->ne) { mark[v] = 2, sp--; continue; }
            if (g->efrom[e] != v) continue;
            int32_t t = g->eto[e];
            if (t < 0 || (size_t)t >= nn) continue;
            if (mark[t] == 1) back[e] = 1;
            else if (!mark[t]) mark[t] = 1, stk[sp] = t, eit[sp++] = 0;
        }
    }
    /* longest path; intra-lane edges weigh 1, cross-lane 0 */
    for (size_t it = 0; it <= nn; it++) {
        bool ch = false;
        for (size_t e = 0; e < g->ne; e++) {
            int32_t f = g->efrom[e], t = g->eto[e];
            if (back[e] || f == t || f < 0 || t < 0 || (size_t)f >= nn || (size_t)t >= nn) continue;
            int32_t r = rank[f] + (lane[f] == lane[t] || g->elabel[e].n ? 1 : 0); /* labelled hops need room */
            if (r > rank[t]) rank[t] = r, ch = true;
        }
        if (!ch) break;
    }
    /* mermaid stacks edge-less nodes of a lane along the rank axis instead of side by side */
    memset(mark, 0, nn + 1);
    for (size_t e = 0; e < g->ne; e++) {
        int32_t f = g->efrom[e], t = g->eto[e];
        if (f >= 0 && (size_t)f < nn) mark[f] = 1;
        if (t >= 0 && (size_t)t < nn) mark[t] = 1;
    }
    for (size_t v = 0; v < nn; v++) {
        if (mark[v] || (g->nflags[v] & NF_HIDDEN)) continue;
        int32_t r = 0;
        for (size_t u = 0; u < v; u++) if (!mark[u] && !(g->nflags[u] & NF_HIDDEN) && lane[u] == lane[v] && rank[u] + 1 > r) r = rank[u] + 1;
        rank[v] = r;
    }
    int32_t nr = 1;
    for (size_t v = 0; v < nn; v++) if (rank[v] + 1 > nr) nr = rank[v] + 1;
    /* main = rank axis, cross = lane axis */
    double *colw = ANEW(a, double, (size_t)nr), *col = ANEW(a, double, (size_t)nr);
    int32_t *cnt = ANEW(a, int32_t, (size_t)L * (size_t)nr), *slot = ANEW(a, int32_t, nn + 1);
    int32_t *lrows = ANEW(a, int32_t, (size_t)L);
    double *lpos = ANEW(a, double, (size_t)L + 1);
    if (!colw || !col || !cnt || !slot || !lrows || !lpos) return CMM_ERR_NOMEM;
    memset(colw, 0, (size_t)nr * sizeof *colw), memset(lrows, 0, (size_t)L * sizeof *lrows);
    memset(cnt, 0, (size_t)L * (size_t)nr * sizeof *cnt), memset(slot, 0, (nn + 1) * sizeof *slot);
    double rowh = 0;
    for (size_t v = 0; v < nn; v++) {
        if (g->nflags[v] & NF_HIDDEN) continue;
        double m = horiz ? g->nw[v] : g->nh[v], c = horiz ? g->nh[v] : g->nw[v];
        colw[rank[v]] = fmax(colw[rank[v]], m), rowh = fmax(rowh, c);
        int32_t *k = &cnt[(size_t)lane[v] * (size_t)nr + (size_t)rank[v]];
        slot[v] = (*k)++;
        if (*k > lrows[lane[v]]) lrows[lane[v]] = *k;
    }
    rowh += horiz ? g->node_sep : g->node_sep * 1.5;
    double gap = g->rank_sep, x = SW_HEAD + SW_PAD;
    for (int32_t r = 0; r < nr; r++) col[r] = x + colw[r] / 2, x += colw[r] + gap;
    double mainlen = x - gap + SW_PAD;
    lpos[0] = 0;
    for (int32_t l = 0; l < L; l++) lpos[l + 1] = lpos[l] + fmax(1, lrows[l]) * rowh + 2 * SW_PAD - g->node_sep;
    bool rev = g->dir == DIR_RL || g->dir == DIR_BT;
    for (size_t v = 0; v < nn; v++) {
        double u = col[rank[v]], w = lpos[lane[v]] + SW_PAD + slot[v] * rowh + (rowh - g->node_sep) / 2;
        if (rev) u = mainlen + SW_HEAD - u; /* keep the title band in place, flip ranks */
        if (horiz) g->nx[v] = u, g->ny[v] = w; else g->nx[v] = w, g->ny[v] = u;
    }
    /* lanes */
    for (size_t s = 0; s < ns; s++) {
        if (lix[s] < 0) { g->sw[s] = 0, g->sh[s] = 0; continue; }
        double c0 = lpos[lix[s]], c1 = lpos[lix[s] + 1];
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
        double cx = g->sx[s] + SW_HEAD / 2, cy = g->sy[s] + g->sh[s] / 2;
        buf_lit(&t, "<text class=\"swimlane-title\" fill=\"#333\" font-size=\"14\" text-anchor=\"middle\" dominant-baseline=\"central\" transform=\"translate(");
        buf_pt(&t, cx, cy); buf_lit(&t, ") rotate(-90)\">");
        buf_xml(&t, keep[s]); buf_lit(&t, "</text>");
        if (g->sw[s] > 0) {
            buf_lit(&t, "<line class=\"swimlane-sep\" stroke=\"#9370DB\" stroke-width=\"1\" x1=\""); buf_num(&t, g->sx[s] + SW_HEAD);
            buf_lit(&t, "\" y1=\""); buf_num(&t, g->sy[s]); buf_lit(&t, "\" x2=\""); buf_num(&t, g->sx[s] + SW_HEAD);
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
