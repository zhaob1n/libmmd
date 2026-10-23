/* ishikawa(-beta): fishbone cause/effect diagram. First line = effect (head), indented lines =
 * causes; deeper indentation nests sub-causes. Port of mermaid's ishikawaRenderer: causes
 * alternate above/below the spine at 82 degrees, even-depth sub-causes are horizontal bones
 * hanging off their parent diagonal, odd-depth ones are diagonals off their parent bone. */
#include <math.h>
#include <stdlib.h>

#include "dgraph.h"
#include "dcfg.h"
#include "diagrams.h"
#include "text.h"

typedef struct INode { Str text; int32_t par; uint32_t c0, nc; } INode;
typedef struct IEnt { Str text; uint32_t depth, cc; int32_t pid; } IEnt;
typedef struct Bone { double x0, y0, x1, y1; uint32_t cc, drawn; } Bone;

static constexpr char CSS[] =
    "#mermaid .isp{stroke:#333333;stroke-width:2;fill:none;}#mermaid .isb{stroke:#333333;stroke-width:1;fill:none;}"
    "#mermaid .iar{fill:#333333;}#mermaid .ihd,#mermaid .ilb{fill:#ECECFF;stroke:#333333;stroke-width:2;}"
    "#mermaid .iht{font-weight:600;font-size:14px;}";

static constexpr double SPINE = 250;
static constexpr double STUB = 30;
static constexpr double BASE = 60;
static constexpr double PERCHILD = 5;
static constexpr unsigned MAXD = 64;

typedef struct IS {
    Arena *A;
    INode *nd;
    uint32_t *kids;
    Buf *b;
    BBox bb;
    double fs, lh, cosA, sinA;
    IEnt *ent;
    uint32_t *yorder;
    size_t ne, ny;
} IS;

static void line(IS *s, double x1, double y1, double x2, double y2, const char *cls) {
    buf_lit(s->b, "<line class=\""); buf_put(s->b, strlen(cls), cls);
    buf_lit(s->b, "\" x1=\""); buf_num(s->b, x1); buf_lit(s->b, "\" y1=\""); buf_num(s->b, y1);
    buf_lit(s->b, "\" x2=\""); buf_num(s->b, x2); buf_lit(s->b, "\" y2=\""); buf_num(s->b, y2);
    buf_lit(s->b, "\" marker-start=\"url(#ish-arrow)\"/>");
    bb_add(&s->bb, fmin(x1, x2), fmin(y1, y2), fmax(x1, x2), fmax(y1, y2));
}

static size_t count_desc(const IS *s, uint32_t v) {
    size_t t = 0;
    for (uint32_t k = 0; k < s->nd[v].nc; k++) t += 1 + count_desc(s, s->kids[s->nd[v].c0 + k]);
    return t;
}

static void walk(IS *s, uint32_t v, int32_t pid, uint32_t depth, int dir) {
    const INode *n = &s->nd[v];
    for (uint32_t q = 0; q < n->nc; q++) {
        uint32_t c = s->kids[n->c0 + (dir < 0 ? n->nc - 1 - q : q)];
        size_t idx = s->ne++;
        Str t = dg_wrapc(s->A, s->nd[c].text, 15);
        s->ent[idx] = (IEnt){.text = t.p ? t : s->nd[c].text, .depth = depth, .cc = s->nd[c].nc, .pid = pid};
        if (depth % 2 == 0) { s->yorder[s->ny++] = (uint32_t)idx; walk(s, c, (int32_t)idx, depth + 1, dir); }
        else { walk(s, c, (int32_t)idx, depth + 1, dir); s->yorder[s->ny++] = (uint32_t)idx; }
    }
}

static double lerp(double a, double b, double t) { return a + (b - a) * t; }

/* draws one cause and its sub-tree; returns leftmost text x */
static double branch(IS *s, uint32_t v, double sx, double sy, int dir, double len) {
    const INode *n = &s->nd[v];
    double ll = len * (n->nc ? 1 : 0.2), dx = -s->cosA * ll, dy = s->sinA * ll * dir, ex = sx + dx, ey = sy + dy;
    line(s, sx, sy, ex, ey, "isp");
    BBox t = dg_tbox(n->text, s->fs, s->lh, ex, ey + 11 * dir, true, "middle", TB_MIDDLE);
    dg_rect(s->b, t.x0 - 20, t.y0 - 2, t.x1 - t.x0 + 40, t.y1 - t.y0 + 4, 0, "ilb");
    bb_add(&s->bb, t.x0 - 20, t.y0 - 2, t.x1 + 20, t.y1 + 2);
    dg_tspans(s->b, &s->bb, n->text, s->fs, s->lh, ex, ey + 11 * dir, true, "middle", TB_MIDDLE, nullptr);
    double left = t.x0;
    if (!n->nc) return left;
    s->ne = s->ny = 0;
    walk(s, v, -1, 2, dir);
    size_t N = s->ne;
    double *ys = ANEW(s->A, double, N + 1);
    Bone *bo = ANEW(s->A, Bone, N + 1); /* bo[0] = cause, bo[i + 1] = entry i */
    if (!ys || !bo) { s->b->oom = true; return left; }
    for (size_t k = 0; k < s->ny; k++) ys[s->yorder[k]] = sy + dy * ((double)(k + 1) / (double)(N + 1));
    bo[0] = (Bone){sx, sy, ex, ey, n->nc, 0};
    for (size_t i = 0; i < N; i++) {
        const IEnt *e = &s->ent[i];
        double y = ys[i], bx0, by0, bx1;
        Bone *par = &bo[e->pid + 1];
        BBox tb;
        if (e->depth % 2 == 0) {
            double dyp = par->y1 - par->y0;
            bx0 = lerp(par->x0, par->x1, dyp != 0 ? (y - par->y0) / dyp : 0.5);
            by0 = y;
            bx1 = bx0 - (e->cc ? BASE + e->cc * PERCHILD : STUB);
            line(s, bx0, y, bx1, y, "isb");
            tb = dg_tbox(e->text, s->fs, s->lh, bx1, y, true, "end", TB_MIDDLE);
            dg_tspans(s->b, &s->bb, e->text, s->fs, s->lh, bx1, y, true, "end", TB_MIDDLE, nullptr);
        } else {
            uint32_t k = par->drawn++;
            bx0 = lerp(par->x0, par->x1, (double)(par->cc - k) / (double)(par->cc + 1));
            by0 = par->y0;
            bx1 = bx0 + -s->cosA * ((y - by0) / (s->sinA * dir));
            line(s, bx0, by0, bx1, y, "isb");
            TBase base = dir < 0 ? TB_ALPHA : TB_HANG;
            tb = dg_tbox(e->text, s->fs, s->lh, bx1, y, true, "end", base);
            dg_tspans(s->b, &s->bb, e->text, s->fs, s->lh, bx1, y, true, "end", base, nullptr);
        }
        if (tb.x0 < left) left = tb.x0;
        bo[i + 1] = (Bone){bx0, by0, bx1, y, e->cc, 0};
    }
    return left;
}

DIAG_FN(ishikawa_render) {
    (void)title;
    LineIt it = {.s = src, .n = n, .i = off};
    Str line_;
    size_t lo;
    if (!dg_next_line(&it, &line_, &lo)) return diag_fail(err_off, msg, off, "empty ishikawa");
    VEC(INode) nv = {};
    int32_t stk[MAXD];
    unsigned lev[MAXD], sn = 0, base = 0;
    bool have_base = false;
    while (dg_next_line(&it, &line_, &lo)) {
        bool ok;
        Str t = label_normalize(a, line_, &ok);
        if (!ok) return MMD_ERR_NOMEM;
        INode nd = {.text = t, .par = NONE};
        if (!nv.n) {
            if (!VEC_PUSH(a, &nv, nd)) return MMD_ERR_NOMEM;
            stk[0] = 0, lev[0] = 0, sn = 1;
            continue;
        }
        unsigned raw = dg_indent(&it, lo);
        if (!have_base) base = raw, have_base = true;
        unsigned l = raw > base ? raw - base + 1 : 1;
        while (sn > 1 && lev[sn - 1] >= l) sn--;
        nd.par = stk[sn - 1];
        if (!VEC_PUSH(a, &nv, nd)) return MMD_ERR_NOMEM;
        if (sn < MAXD) stk[sn] = (int32_t)(nv.n - 1), lev[sn++] = l;
    }
    if (!nv.n) return diag_fail(err_off, msg, off, "ishikawa needs an effect line");
    size_t N = nv.n;
    IS s = {.A = a, .nd = nv.v, .fs = fs > 0 ? fs : 16, .bb = BB_EMPTY};
    s.lh = s.fs * 1.05;
    s.cosA = cos(82 * M_PI / 180), s.sinA = sin(82 * M_PI / 180);
    s.kids = ANEW(a, uint32_t, N + 1);
    s.ent = ANEW(a, IEnt, N + 1);
    s.yorder = ANEW(a, uint32_t, N + 1);
    if (!s.kids || !s.ent || !s.yorder) return MMD_ERR_NOMEM;
    for (size_t v = 1; v < N; v++) s.nd[s.nd[v].par].nc++;
    uint32_t acc = 0;
    for (size_t v = 0; v < N; v++) s.nd[v].c0 = acc, acc += s.nd[v].nc, s.nd[v].nc = 0;
    for (size_t v = 1; v < N; v++) { INode *p = &s.nd[s.nd[v].par]; s.kids[p->c0 + p->nc++] = (uint32_t)v; }

    Buf body = {};
    s.b = &body;
    buf_lit(&body, "<defs><marker id=\"ish-arrow\" viewBox=\"0 0 10 10\" refX=\"0\" refY=\"5\" markerWidth=\"6\" "
                   "markerHeight=\"6\" orient=\"auto\"><path d=\"M 10 0 L 0 5 L 10 10 Z\" class=\"iar\"/></marker></defs>");
    const INode *root = &s.nd[0];
    size_t up = 0, upmax = 0, dn = 0, dnmax = 0;
    for (uint32_t k = 0; k < root->nc; k++) {
        size_t d = count_desc(&s, s.kids[root->c0 + k]);
        if (k % 2 == 0) up += d, upmax = d > upmax ? d : upmax;
        else dn += d, dnmax = d > dnmax ? d : dnmax;
    }
    double ul = SPINE, dl = SPINE;
    if (up + dn) {
        ul = fmax(SPINE * 0.3, 2 * SPINE * (double)up / (double)(up + dn));
        dl = fmax(SPINE * 0.3, 2 * SPINE * (double)dn / (double)(up + dn));
    }
    ul = fmax(ul, (double)upmax * s.fs * 2), dl = fmax(dl, (double)dnmax * s.fs * 2);
    double sy = fmax(ul, SPINE), sx = root->nc ? -20 : 0;
    /* head */
    Str hl = dg_wrapc(a, root->text, (size_t)fmax(6, floor(110 / (s.fs * 0.6))));
    if (!hl.p && root->text.n) return MMD_ERR_NOMEM;
    BBox ht = dg_tbox(hl, 14, s.lh, 0, 0, true, "start", TB_MIDDLE);
    double tw = (ht.x1 - ht.x0) * 1.05, th = ht.y1 - ht.y0, hw = fmax(60, tw + 6), hh = fmax(40, th * 2 + 40);
    for (uint32_t p = 0; p < (root->nc + 1) / 2; p++) {
        buf_lit(&body, "<g class=\"pair\">");
        double left = sx;
        for (uint32_t q = 0; q < 2; q++) {
            uint32_t k = p * 2 + q;
            if (k >= root->nc) break;
            double l = branch(&s, s.kids[root->c0 + k], sx, sy, q ? 1 : -1, q ? dl : ul);
            if (l < left) left = l;
        }
        buf_lit(&body, "</g>");
        sx = left;
    }
    line(&s, sx, sy, 0, sy, "isp");
    /* the spine is drawn last here; mermaid has no marker on it */
    body.n -= sizeof("\" marker-start=\"url(#ish-arrow)\"/>") - 1;
    buf_lit(&body, "\"/>");
    buf_lit(&body, "<path class=\"ihd\" d=\"M 0 "); buf_num(&body, sy - hh / 2); buf_lit(&body, " L 0 ");
    buf_num(&body, sy + hh / 2); buf_lit(&body, " Q "); buf_num(&body, hw * 2.4); buf_ch(&body, ' ');
    buf_num(&body, sy); buf_lit(&body, " 0 "); buf_num(&body, sy - hh / 2); buf_lit(&body, " Z\"/>");
    bb_add(&s.bb, 0, sy - hh / 2, hw * 2.4, sy + hh / 2);
    dg_tspans(&body, nullptr, hl, 14, s.lh, hw / 2 + 3, sy, true, "middle", TB_MIDDLE, "iht");
    return DG_FINISH(out, &body, s.bb, 20, "ishikawa", CSS);
}
