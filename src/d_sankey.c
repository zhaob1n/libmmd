/* sankey(-beta): CSV rows `source,target,value` (RFC 4180 quoting, "" escapes, blank lines ok).
 * Layout is a port of d3-sankey 0.12 as configured by mermaid 11 (justify alignment, node width 10,
 * padding 12 (+15 with showValues), 6 relaxation iterations with collision resolution); links are
 * cubic bands coloured by their source. Labels show the value (showValues). */
#include <math.h>
#include <stdio.h>

#include "dgraph.h"
#include "dcfg.h"
#include "diagrams.h"
#include "text.h"

typedef struct SNode { Str name; double in, out, v, x, y, h; uint32_t col; } SNode;
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
    /* --- port of d3-sankey 0.12 (mermaid 11: justify, nodeWidth 10, nodePadding 12 (+15 with
       showValues), extent [[0,0],[width,height]], 6 iterations) --- */
    const bool SHOWV = dcfg_bool("sankey.showValues", true);
    uint32_t *soff = ANEW(a, uint32_t, nn + 1), *toff = ANEW(a, uint32_t, nn + 1);
    uint32_t *sl = ANEW(a, uint32_t, nl), *tl = ANEW(a, uint32_t, nl), *fill = ANEW(a, uint32_t, nn);
    if (!soff || !toff || !sl || !tl || !fill) return CMM_ERR_NOMEM;
    for (size_t i = 0; i <= nn; i++) soff[i] = toff[i] = 0;
    for (size_t l = 0; l < nl; l++) soff[L[l].s + 1]++, toff[L[l].t + 1]++;
    for (size_t i = 0; i < nn; i++) soff[i + 1] += soff[i], toff[i + 1] += toff[i];
    for (size_t i = 0; i < nn; i++) fill[i] = soff[i];
    for (size_t l = 0; l < nl; l++) sl[fill[L[l].s]++] = (uint32_t)l;
    for (size_t i = 0; i < nn; i++) fill[i] = toff[i];
    for (size_t l = 0; l < nl; l++) tl[fill[L[l].t]++] = (uint32_t)l;
    for (size_t i = 0; i < nn; i++) { /* justify: nodes without outgoing links to the last column */
        N[i].v = fmax(N[i].in, N[i].out);
        if (soff[i + 1] == soff[i]) N[i].col = maxc;
    }
    const double W = fmin(fmax(dcfg_num("sankey.width", 600), 100), 4000);
    const double H = fmin(fmax(dcfg_num("sankey.height", 400), 100), 4000);
    constexpr double NW = 10;
    uint32_t *colcnt = ANEW(a, uint32_t, maxc + 2), *coff = ANEW(a, uint32_t, maxc + 2), *cn = ANEW(a, uint32_t, nn);
    if (!colcnt || !coff || !cn) return CMM_ERR_NOMEM;
    for (uint32_t c = 0; c <= maxc + 1; c++) colcnt[c] = coff[c] = 0;
    for (size_t i = 0; i < nn; i++) colcnt[N[i].col]++;
    uint32_t maxcnt = 0;
    for (uint32_t c = 0; c <= maxc; c++) coff[c + 1] = coff[c] + colcnt[c], maxcnt = colcnt[c] > maxcnt ? colcnt[c] : maxcnt;
    for (uint32_t c = 0; c <= maxc; c++) colcnt[c] = 0;
    for (size_t i = 0; i < nn; i++) cn[coff[N[i].col] + colcnt[N[i].col]++] = (uint32_t)i; /* input order */
    const double py = fmin(12.0 + (SHOWV ? 15 : 0), maxcnt > 1 ? H / (maxcnt - 1.0) : INFINITY);
    double ky = INFINITY;
    for (uint32_t c = 0; c <= maxc; c++) {
        double sum = 0;
        for (uint32_t k = coff[c]; k < coff[c + 1]; k++) sum += N[cn[k]].v;
        if (sum > 0) ky = fmin(ky, (H - (coff[c + 1] - coff[c] - 1.0) * py) / sum);
    }
    if (!isfinite(ky) || ky <= 0) ky = 1;
    #define Y1(i) (N[i].y + N[i].h)
    #define LW(l) (L[l].v * ky)
    for (uint32_t c = 0; c <= maxc; c++) {
        double y = 0;
        uint32_t k0 = coff[c], k1 = coff[c + 1];
        for (uint32_t k = k0; k < k1; k++) { SNode *q = &N[cn[k]]; q->x = maxc ? (W - NW) * c / maxc : 0; q->y = y, q->h = q->v * ky, y = Y1(cn[k]) + py; }
        y = (H - y + py) / (k1 - k0 + 1.0);
        for (uint32_t k = k0; k < k1; k++) N[cn[k]].y += y * (k - k0 + 1);
    }
    /* link lists sorted by the other end's breadth (ties: link index) */
    #define SORT_LINKS(arr, o0, o1, END)                                                       \
        for (uint32_t i_ = (o0) + 1; i_ < (o1); i_++) {                                       \
            uint32_t t_ = arr[i_], j_ = i_;                                                   \
            while (j_ > (o0) && (N[L[arr[j_ - 1]].END].y > N[L[t_].END].y ||                    \
                                 (N[L[arr[j_ - 1]].END].y == N[L[t_].END].y && arr[j_ - 1] > t_))) \
                arr[j_] = arr[j_ - 1], j_--;                                                  \
            arr[j_] = t_;                                                                     \
        }
    #define REORDER_NODE(i)                                                                    \
        do {                                                                                  \
            for (uint32_t q_ = toff[i]; q_ < toff[(i) + 1]; q_++) { uint32_t sn_ = L[tl[q_]].s; SORT_LINKS(sl, soff[sn_], soff[sn_ + 1], t); } \
            for (uint32_t q_ = soff[i]; q_ < soff[(i) + 1]; q_++) { uint32_t tn_ = L[sl[q_]].t; SORT_LINKS(tl, toff[tn_], toff[tn_ + 1], s); } \
        } while (0)
    for (size_t i = 0; i < nn; i++) { SORT_LINKS(sl, soff[i], soff[i + 1], t); SORT_LINKS(tl, toff[i], toff[i + 1], s); }
    for (int it = 0; it < 6; it++) {
        double alpha = pow(0.99, it), beta = fmax(1 - alpha, (it + 1) / 6.0);
        for (int pass = 0; pass < 2; pass++) { /* right-to-left, then left-to-right */
            bool r2l = pass == 0;
            for (int64_t ci = r2l ? (int64_t)maxc - 1 : 1; r2l ? ci >= 0 : ci <= (int64_t)maxc; ci += r2l ? -1 : 1) {
                uint32_t c = (uint32_t)ci, k0 = coff[c], k1 = coff[c + 1];
                for (uint32_t k = k0; k < k1; k++) {
                    uint32_t i = cn[k];
                    double y = 0, w = 0;
                    if (r2l) {
                        for (uint32_t q = soff[i]; q < soff[i + 1]; q++) {
                            uint32_t l = sl[q], t = L[l].t;
                            double v = L[l].v * (N[t].col - (double)N[i].col);
                            /* sourceTop(i, t) */
                            double yy = N[t].y - (toff[t + 1] - toff[t] - 1.0) * py / 2;
                            for (uint32_t r = toff[t]; r < toff[t + 1] && L[tl[r]].s != i; r++) yy += LW(tl[r]) + py;
                            for (uint32_t r = soff[i]; r < soff[i + 1] && L[sl[r]].t != t; r++) yy -= LW(sl[r]);
                            y += yy * v, w += v;
                        }
                    } else {
                        for (uint32_t q = toff[i]; q < toff[i + 1]; q++) {
                            uint32_t l = tl[q], sn = L[l].s;
                            double v = L[l].v * (N[i].col - (double)N[sn].col);
                            /* targetTop(sn, i) */
                            double yy = N[sn].y - (soff[sn + 1] - soff[sn] - 1.0) * py / 2;
                            for (uint32_t r = soff[sn]; r < soff[sn + 1] && L[sl[r]].t != i; r++) yy += LW(sl[r]) + py;
                            for (uint32_t r = toff[i]; r < toff[i + 1] && L[tl[r]].s != sn; r++) yy -= LW(tl[r]);
                            y += yy * v, w += v;
                        }
                    }
                    if (!(w > 0)) continue;
                    N[i].y += (y / w - N[i].y) * alpha;
                    REORDER_NODE(i);
                }
                for (uint32_t k = k0 + 1; k < k1; k++) { /* stable sort by breadth */
                    uint32_t t = cn[k], j = k;
                    while (j > k0 && N[cn[j - 1]].y > N[t].y) cn[j] = cn[j - 1], j--;
                    cn[j] = t;
                }
                /* resolveCollisions(column, beta) */
                uint32_t m = k0 + ((k1 - k0) >> 1), sub = cn[m];
                double yc = N[sub].y - py;
                for (int64_t k = (int64_t)m - 1; k >= (int64_t)k0; k--) { uint32_t i = cn[k]; double dy = (Y1(i) - yc) * beta; if (dy > 1e-6) N[i].y -= dy; yc = N[i].y - py; }
                yc = Y1(sub) + py;
                for (uint32_t k = m + 1; k < k1; k++) { uint32_t i = cn[k]; double dy = (yc - N[i].y) * beta; if (dy > 1e-6) N[i].y += dy; yc = Y1(i) + py; }
                yc = H;
                for (int64_t k = (int64_t)k1 - 1; k >= (int64_t)k0; k--) { uint32_t i = cn[k]; double dy = (Y1(i) - yc) * beta; if (dy > 1e-6) N[i].y -= dy; yc = N[i].y - py; }
                yc = 0;
                for (uint32_t k = k0; k < k1; k++) { uint32_t i = cn[k]; double dy = (yc - N[i].y) * beta; if (dy > 1e-6) N[i].y += dy; yc = Y1(i) + py; }
            }
        }
    }
    for (size_t i = 0; i < nn; i++) { /* computeLinkBreadths */
        double y0 = N[i].y, y1 = N[i].y;
        for (uint32_t q = soff[i]; q < soff[i + 1]; q++) L[sl[q]].y0 = y0 + LW(sl[q]) / 2, y0 += LW(sl[q]);
        for (uint32_t q = toff[i]; q < toff[i + 1]; q++) L[tl[q]].y1 = y1 + LW(tl[q]) / 2, y1 += LW(tl[q]);
    }
    #undef REORDER_NODE
    #undef SORT_LINKS
    #undef LW
    #undef Y1
    double lw = 0, rw = 0;
    char nb[48];
    for (size_t i = 0; i < nn; i++) {
        (SHOWV ? snprintf(nb, sizeof nb, " %g", round(N[i].v * 100) / 100) : (nb[0] = 0));
        double w = text_width(N[i].name, 14) + text_width((Str){.p = nb, .n = strlen(nb)}, 14) + 8;
        if (N[i].x < W / 2) rw = fmax(rw, N[i].x + NW + w - W); else lw = fmax(lw, w - N[i].x);
    }
    double top = title.n ? 40 : 0;
    /* mermaid: viewBox = content bbox (nodes, link bands, labels ~ +-8px about the node centre) */
    double ymin = 0, ymax = H;
    for (size_t i = 0; i < nn; i++) {
        double cy = N[i].y + N[i].h / 2;
        ymin = fmin(ymin, fmin(N[i].y, cy - 8)), ymax = fmax(ymax, fmax(N[i].y + N[i].h, cy + 8));
    }
    BBox bb = {-fmax(lw, 0), ymin - top, W + fmax(rw, 0), ymax};
    if (!buf_reserve(out, 1024 + nl * 300 + nn * 300)) return CMM_ERR_NOMEM;
    DG_HEAD(out, bb, 0, "sankey", CSS);
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
        (SHOWV ? snprintf(nb, sizeof nb, " %g", round(N[i].v * 100) / 100) : (nb[0] = 0));
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
        int k = (SHOWV ? snprintf(nb, sizeof nb, " %g", round(N[i].v * 100) / 100) : (nb[0] = 0));
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
