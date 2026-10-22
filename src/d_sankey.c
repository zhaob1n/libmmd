/* sankey(-beta): CSV rows `source,target,value` (RFC 4180 quoting, "" escapes, blank lines ok).
 * d3-sankey style layout: columns by longest path from sources with sinks justified to the last
 * column, node height = max(inflow, outflow) * ky, a few barycentric relaxation sweeps to order
 * nodes, links as cubic bands coloured by their source. Labels show the value (showValues). */
#include <math.h>
#include <stdio.h>

#include "dgraph.h"
#include "dcfg.h"
#include "diagrams.h"
#include "text.h"

typedef struct SNode { Str name; double in, out, v, x, y, h, yin, yout, key; uint32_t col; bool sink_ok; } SNode;
typedef struct SLink { uint32_t s, t; double v, y0, y1; } SLink;

static constexpr char CSS[] =
    "#mermaid .sl{fill:none;stroke-opacity:.5;}#mermaid .sn{stroke:none;}#mermaid .st{font-size:14px;fill:#333;}";
static const char *const PAL[] = {"#4e79a7", "#f28e2c", "#e15759", "#76b7b2", "#59a14f", "#edc949",
                                  "#af7aa1", "#ff9da7", "#9c755f", "#bab0ab"};

/* one CSV field from s (advances); quoted fields unescape "" into arena memory */
static bool field(Arena *a, Str *s, Str *f, bool *oom) {
    *s = (Str){.p = s->p, .n = s->n};
    size_t i = 0;
    while (i < s->n && (s->p[i] == ' ' || s->p[i] == '\t')) i++;
    if (i < s->n && s->p[i] == '"') {
        char *o = ANEW(a, char, s->n + 1);
        if (!o) return *oom = true, false;
        size_t k = 0;
        i++;
        for (;; i++) {
            if (i >= s->n) return false;
            if (s->p[i] == '"') {
                if (i + 1 < s->n && s->p[i + 1] == '"') { o[k++] = '"'; i++; continue; }
                i++;
                break;
            }
            o[k++] = s->p[i];
        }
        *f = (Str){.p = o, .n = k};
        while (i < s->n && s->p[i] != ',') if (s->p[i++] != ' ') return false;
    } else {
        size_t st = i;
        while (i < s->n && s->p[i] != ',') i++;
        *f = str_trim((Str){.p = s->p + st, .n = i - st});
    }
    if (i < s->n) i++; /* comma */
    s->p += i, s->n -= i;
    return true;
}

DIAG_FN(sankey_render) {
    (void)fs;
    LineIt it = {.s = src, .n = n, .i = off};
    Str line;
    size_t lo;
    if (!dg_next_line(&it, &line, &lo)) return diag_fail(err_off, msg, off, "empty sankey");
    VEC(SNode) nv = {};
    VEC(SLink) lv = {};
    bool oom = false;
    while (dg_next_line(&it, &line, &lo)) {
        Str s = line, f[3];
        for (int k = 0; k < 3; k++) {
            if (!s.n && k < 3) { if (k < 3) return diag_fail(err_off, msg, lo, "expected source,target,value"); }
            if (!field(a, &s, &f[k], &oom)) return oom ? CMM_ERR_NOMEM : diag_fail(err_off, msg, lo, "bad CSV field");
        }
        if (s.n) return diag_fail(err_off, msg, (size_t)(s.p - src), "too many CSV fields");
        double v;
        if (!dg_parse_num(f[2], &v) || !(v >= 0) || v > 1e15) return diag_fail(err_off, msg, lo, "value must be a non-negative number");
        if (!f[0].n || !f[1].n) return diag_fail(err_off, msg, lo, "empty node name");
        uint32_t id[2];
        for (int k = 0; k < 2; k++) {
            size_t j = 0;
            while (j < nv.n && !str_eq(nv.v[j].name, f[k])) j++;
            if (j == nv.n && !VEC_PUSH(a, &nv, ((SNode){.name = f[k]}))) return CMM_ERR_NOMEM;
            id[k] = (uint32_t)j;
        }
        if (id[0] == id[1]) return diag_fail(err_off, msg, lo, "sankey links cannot be self-loops");
        if (!VEC_PUSH(a, &lv, ((SLink){.s = id[0], .t = id[1], .v = v}))) return CMM_ERR_NOMEM;
    }
    if (!lv.n) return diag_fail(err_off, msg, off, "sankey has no links");
    SNode *N = nv.v;
    SLink *L = lv.v;
    size_t nn = nv.n, nl = lv.n;
    for (size_t i = 0; i < nl; i++) N[L[i].s].out += L[i].v, N[L[i].t].in += L[i].v;
    /* depth = longest path (Bellman-Ford style relaxation, bounded => cycles rejected) */
    uint32_t maxc = 0;
    for (size_t round = 0;; round++) {
        bool ch = false;
        for (size_t i = 0; i < nl; i++)
            if (N[L[i].t].col < N[L[i].s].col + 1) N[L[i].t].col = N[L[i].s].col + 1, ch = true;
        if (!ch) break;
        if (round > nn) return diag_fail(err_off, msg, off, "sankey links form a cycle");
    }
    for (size_t i = 0; i < nn; i++) if (N[i].col > maxc) maxc = N[i].col;
    for (size_t i = 0; i < nn; i++) { /* justify: sinks to the last column */
        N[i].v = fmax(N[i].in, N[i].out);
        if (N[i].out == 0 && N[i].in > 0) N[i].col = maxc;
    }
    const double W = fmin(fmax(dcfg_num("sankey.width", 600), 100), 4000);
    const double H = fmin(fmax(dcfg_num("sankey.height", 400), 100), 4000);
    constexpr double NW = 10;
    constexpr double PAD = 25;
    double ky = INFINITY;
    double *colsum = ANEW(a, double, maxc + 1);
    uint32_t *colcnt = ANEW(a, uint32_t, maxc + 1);
    uint32_t *lord = ANEW(a, uint32_t, nn);
    if (!colsum || !colcnt || !lord) return CMM_ERR_NOMEM;
    for (uint32_t c = 0; c <= maxc; c++) colsum[c] = 0, colcnt[c] = 0; /* arena memory is not zeroed */
    for (size_t i = 0; i < nn; i++) colsum[N[i].col] += N[i].v, colcnt[N[i].col]++;
    for (uint32_t c = 0; c <= maxc; c++)
        if (colsum[c] > 0) ky = fmin(ky, (H - (colcnt[c] - 1.0) * PAD) / colsum[c]);
    if (!isfinite(ky) || ky <= 0) ky = 1;
    for (size_t i = 0; i < nn; i++) {
        N[i].x = maxc ? (W - NW) * N[i].col / maxc : 0;
        N[i].h = N[i].v * ky;
        N[i].key = (double)i;
    }
    /* order nodes per column; relax by weighted barycentre of neighbours (left-to-right then back) */
    for (int sweep = 0; sweep < 6; sweep++) {
        for (uint32_t c = 0; c <= maxc; c++) {
            size_t k = 0;
            for (size_t i = 0; i < nn; i++) if (N[i].col == c) lord[k++] = (uint32_t)i;
            for (size_t i = 1; i < k; i++) { /* insertion sort by key */
                uint32_t t = lord[i];
                size_t j = i;
                while (j && N[lord[j - 1]].key > N[t].key) lord[j] = lord[j - 1], j--;
                lord[j] = t;
            }
            double y = 0, tot = 0;
            for (size_t i = 0; i < k; i++) tot += N[lord[i]].h;
            y = (H - tot - (k - 1.0) * PAD) / 2;
            for (size_t i = 0; i < k; i++) N[lord[i]].y = y, y += N[lord[i]].h + PAD;
        }
        for (size_t i = 0; i < nn; i++) { /* new key: barycentre of linked node centres */
            double sw = 0, sy = 0;
            for (size_t l = 0; l < nl; l++) {
                uint32_t o = L[l].s == i ? L[l].t : L[l].t == i ? L[l].s : UINT32_MAX;
                if (o == UINT32_MAX || ((sweep & 1) ? N[o].col < N[i].col : N[o].col > N[i].col)) continue;
                sw += L[l].v, sy += L[l].v * (N[o].y + N[o].h / 2);
            }
            if (sw > 0) N[i].key = sy / sw;
            else N[i].key = N[i].y + N[i].h / 2;
        }
    }
    /* link attachment points: sorted by the other end's y */
    uint32_t *lo_ = ANEW(a, uint32_t, nl);
    if (!lo_) return CMM_ERR_NOMEM;
    for (size_t i = 0; i < nl; i++) lo_[i] = (uint32_t)i;
    for (size_t i = 1; i < nl; i++) {
        uint32_t t = lo_[i];
        size_t j = i;
        while (j && N[L[lo_[j - 1]].t].y + N[L[lo_[j - 1]].s].y * 1e-9 > N[L[t].t].y + N[L[t].s].y * 1e-9) lo_[j] = lo_[j - 1], j--;
        lo_[j] = t;
    }
    for (size_t i = 0; i < nn; i++) N[i].yout = N[i].yin = N[i].y;
    for (size_t q = 0; q < nl; q++) { SLink *l = &L[lo_[q]]; l->y0 = N[l->s].yout + l->v * ky / 2; N[l->s].yout += l->v * ky; }
    for (size_t i = 1; i < nl; i++) { /* re-sort by source y for the incoming side */
        uint32_t t = lo_[i];
        size_t j = i;
        while (j && N[L[lo_[j - 1]].s].y > N[L[t].s].y) lo_[j] = lo_[j - 1], j--;
        lo_[j] = t;
    }
    for (size_t q = 0; q < nl; q++) { SLink *l = &L[lo_[q]]; l->y1 = N[l->t].yin + l->v * ky / 2; N[l->t].yin += l->v * ky; }
    double lw = 0, rw = 0;
    char nb[48];
    for (size_t i = 0; i < nn; i++) {
        snprintf(nb, sizeof nb, " %g", round(N[i].v * 100) / 100);
        double w = text_width(N[i].name, 14) + text_width((Str){.p = nb, .n = strlen(nb)}, 14) + 8;
        if (N[i].x < W / 2) rw = fmax(rw, N[i].x + NW + w - W); else lw = fmax(lw, w - N[i].x);
    }
    double top = title.n ? 40 : 0;
    BBox bb = {-fmax(lw, 0), -top - 5 - 2 * nn, W + fmax(rw, 0), H + 5 + 2 * nn};
    if (!buf_reserve(out, 1024 + nl * 300 + nn * 300)) return CMM_ERR_NOMEM;
    DG_HEAD(out, bb, 6, "sankey", CSS);
    for (size_t i = 0; i < nl; i++) {
        const SLink *l = &L[i];
        double x0 = N[l->s].x + NW, x1 = N[l->t].x, xm = (x0 + x1) / 2;
        const char *col = PAL[l->s % countof(PAL)];
        buf_lit(out, "<path class=\"sl\" stroke=\""); buf_put(out, strlen(col), col);
        buf_lit(out, "\" stroke-width=\""); buf_num(out, fmax(1, l->v * ky)); buf_lit(out, "\" d=\"M");
        dg_pt(out, x0, l->y0); buf_ch(out, 'C'); dg_pt(out, xm, l->y0); buf_ch(out, ' '); dg_pt(out, xm, l->y1);
        buf_ch(out, ' '); dg_pt(out, x1, l->y1); buf_lit(out, "\"/>");
    }
    /* label collision avoidance: boxes overlapping horizontally are pushed apart vertically */
    double *lx0 = ANEW(a, double, nn), *lx1 = ANEW(a, double, nn), *ly = ANEW(a, double, nn);
    uint32_t *lbo = ANEW(a, uint32_t, nn);
    if (!lx0 || !lx1 || !ly || !lbo) return CMM_ERR_NOMEM;
    constexpr double LH = 17;
    for (size_t i = 0; i < nn; i++) {
        bool right = N[i].x < W / 2;
        snprintf(nb, sizeof nb, " %g", round(N[i].v * 100) / 100);
        double w = text_width(N[i].name, 14) + text_width((Str){.p = nb, .n = strlen(nb)}, 14);
        lx0[i] = right ? N[i].x + NW + 6 : N[i].x - 6 - w;
        lx1[i] = lx0[i] + w, ly[i] = N[i].y + N[i].h / 2, lbo[i] = (uint32_t)i;
    }
    for (size_t i = 1; i < nn; i++) {
        uint32_t t = lbo[i];
        size_t j = i;
        while (j && ly[lbo[j - 1]] > ly[t]) lbo[j] = lbo[j - 1], j--;
        lbo[j] = t;
    }
    for (int it = 0; it < 8; it++) {
        bool moved = false;
        for (size_t p = 0; p < nn; p++)
            for (size_t q = p + 1; q < nn; q++) {
                uint32_t u = lbo[p], v = lbo[q];
                if (lx1[u] <= lx0[v] || lx1[v] <= lx0[u]) continue;
                double d = ly[v] - ly[u];
                if (fabs(d) >= LH) continue;
                double push = (LH - fabs(d)) / 2 + 0.01;
                if (d >= 0) ly[u] -= push, ly[v] += push; else ly[u] += push, ly[v] -= push;
                moved = true;
            }
        if (!moved) break;
    }
    for (size_t i = 0; i < nn; i++) {
        dg_rect_fill(out, N[i].x, N[i].y, NW, N[i].h, 0, "sn", PAL[i % countof(PAL)]);
        int k = snprintf(nb, sizeof nb, " %g", round(N[i].v * 100) / 100);
        if (N[i].x < W / 2) dg_text_open(out, lx0[i], ly[i], "start", "st");
        else dg_text_open(out, lx1[i], ly[i], "end", "st");
        buf_xml(out, N[i].name);
        buf_xml(out, (Str){.p = nb, .n = (size_t)k});
        buf_lit(out, "</text>");
    }
    if (title.n) dg_text(out, title, 18, W / 2, -top / 2 - 5, "middle", "st");
    buf_lit(out, "</svg>\n");
    return out->oom ? CMM_ERR_NOMEM : CMM_OK;
}
