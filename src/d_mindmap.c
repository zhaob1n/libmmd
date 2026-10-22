/* mindmap: indentation tree. Shapes: [square] (rounded) ((circle)) ))bang(( )cloud( {{hexagon}},
 * bare text = default (underlined). `::icon(fa fa-x)` adds an icon badge to the previous node and
 * `:::a b` adds CSS classes (both as their own line or as a suffix).
 * Layout: radial (the shape mermaid's default cose-bilkent converges to): each root child gets a wedge
 * proportional to its leaf count, rings are pushed out until siblings fit. `layout: tidy-tree` in the
 * front matter/init selects the two-sided tidy tree. */
#include <stdio.h>
#include <math.h>

#include "dgraph.h"
#include "icons.h"
#include "diagrams.h"
#include "text.h"

enum : uint8_t { M_DEF, M_SQ, M_RND, M_CIR, M_BANG, M_CLOUD, M_HEX };
typedef struct MNode {
    Str label, icon, cls;
    uint32_t parent, first, next, depth, sec;
    uint8_t shape;
    int8_t side;
    double w, h, x, y, sub; /* sub: subtree height */
} MNode;

static constexpr char CSS[] = "#mermaid .mt{font-size:16px;}#mermaid .me{fill:none;}"
                              "#mermaid .ic{fill:#fff;stroke:#333;stroke-width:1px;}#mermaid .ict{font-size:11px;font-weight:bold;fill:#333;}";
static const char *const SEC[] = {"#ffb8ff", "#b8ffff", "#ffe0e0", "#e4ffb8", "#ffffab", "#b8b8ff",
                                  "#ff85ff", "#85ffff", "#ffb8b8", "#f5ffe0", "#ffffde"};
static const char *const SECS[] = {"#c060c0", "#40b0b0", "#d08080", "#80b040", "#c0c040", "#6060d0",
                                   "#d040d0", "#30c0c0", "#d06060", "#90b060", "#c0c070"};

static bool parse_node(Str s, Str *label, uint8_t *shape) {
    static const struct { const char *o, *c; uint8_t sh; } SH[] = {
        {"((", "))", M_CIR}, {"))", "((", M_BANG}, {"{{", "}}", M_HEX}, {"[", "]", M_SQ},
        {"(", ")", M_RND}, {")", "(", M_CLOUD}};
    for (size_t i = 0; i < s.n; i++)
        for (size_t k = 0; k < countof(SH); k++) {
            size_t ol = strlen(SH[k].o), cl = strlen(SH[k].c);
            if (s.n - i >= ol + cl && !memcmp(s.p + i, SH[k].o, ol) && s.n >= cl &&
                !memcmp(s.p + s.n - cl, SH[k].c, cl)) {
                Str in = {.p = s.p + i + ol, .n = s.n - i - ol - cl};
                if (in.n >= 2 && in.p[0] == '"' && in.p[in.n - 1] == '"') in = (Str){.p = in.p + 1, .n = in.n - 2};
                *label = in, *shape = SH[k].sh;
                return true;
            }
        }
    *label = s, *shape = M_DEF;
    return true;
}

static void measure_sub(MNode *v, uint32_t i) {
    double s = 0;
    for (uint32_t c = v[i].first; c != UINT32_MAX; c = v[c].next) {
        measure_sub(v, c);
        s += v[c].sub;
    }
    v[i].sub = fmax(v[i].h + 16, s);
}
static void place(MNode *v, uint32_t i, double x, double ytop, int dir) {
    v[i].x = x, v[i].y = ytop + v[i].sub / 2;
    double kids = 0;
    for (uint32_t c = v[i].first; c != UINT32_MAX; c = v[c].next) kids += v[c].sub;
    double y = ytop + (v[i].sub - kids) / 2;
    for (uint32_t c = v[i].first; c != UINT32_MAX; c = v[c].next) {
        double nx = x + dir * (v[i].w / 2 + 50 + v[c].w / 2);
        place(v, c, nx, y, dir);
        y += v[c].sub;
    }
}

DIAG_FN(mindmap_render) {
    (void)fs, (void)title;
    LineIt it = {.s = src, .n = n, .i = off};
    Str line;
    size_t lo;
    if (!dg_next_line(&it, &line, &lo)) return diag_fail(err_off, msg, off, "empty mindmap");
    VEC(MNode) v = {};
    VEC(uint32_t) ind = {}; /* indentation per node */
    while (dg_next_line(&it, &line, &lo)) {
        if (str_starts(line, S("::icon("))) {
            if (v.n && line.p[line.n - 1] == ')') v.v[v.n - 1].icon = str_trim((Str){.p = line.p + 7, .n = line.n - 8});
            continue;
        }
        if (str_starts(line, S(":::"))) {
            if (v.n) v.v[v.n - 1].cls = str_trim((Str){.p = line.p + 3, .n = line.n - 3});
            continue;
        }
        unsigned in = dg_indent(&it, lo);
        MNode m = {.parent = UINT32_MAX, .first = UINT32_MAX, .next = UINT32_MAX};
        for (size_t i = 0; i + 3 <= line.n; i++)
            if (!memcmp(line.p + i, ":::", 3)) {
                m.cls = str_trim((Str){.p = line.p + i + 3, .n = line.n - i - 3});
                line = str_trim((Str){.p = line.p, .n = i});
                break;
            }
        for (size_t i = 0; i + 7 <= line.n; i++) /* inline "text ::icon(x)" */
            if (!memcmp(line.p + i, "::icon(", 7)) {
                const char *e = memchr(line.p + i, ')', line.n - i);
                if (e) m.icon = str_trim((Str){.p = line.p + i + 7, .n = (size_t)(e - line.p) - i - 7});
                line = str_trim((Str){.p = line.p, .n = i});
                break;
            }
        if (!parse_node(line, &m.label, &m.shape)) return diag_fail(err_off, msg, lo, "bad node");
        if (v.n) {
            if (in <= ind.v[0]) return diag_fail(err_off, msg, lo, "mindmap can only have one root");
            uint32_t p = (uint32_t)v.n - 1;
            while (ind.v[p] >= in) p = v.v[p].parent;
            m.parent = p;
            m.depth = v.v[p].depth + 1;
        }
        if (!VEC_PUSH(a, &v, m) || !VEC_PUSH(a, &ind, (uint32_t)in)) return CMM_ERR_NOMEM;
        uint32_t me = (uint32_t)v.n - 1;
        if (m.parent != UINT32_MAX) { /* append as last child */
            uint32_t *pp = &v.v[m.parent].first;
            while (*pp != UINT32_MAX) pp = &v.v[*pp].next;
            *pp = me;
        }
    }
    if (!v.n) return diag_fail(err_off, msg, off, "mindmap has no nodes");
    MNode *V = v.v;
    for (size_t i = 0; i < v.n; i++) {
        Str lab = dg_wrap(a, V[i].label, 16, 200);
        if (!lab.p && V[i].label.n) return CMM_ERR_NOMEM;
        V[i].label = lab;
        double w;
        unsigned nl;
        label_measure(lab, 16, &w, &nl);
        double pad = V[i].shape == M_DEF ? 8 : 20;
        V[i].w = w + 2 * pad, V[i].h = nl * 24 + (V[i].shape == M_DEF ? 8 : 20);
        if (V[i].shape == M_CIR) V[i].w = V[i].h = fmax(V[i].w, V[i].h);
        if (V[i].shape == M_HEX) V[i].w += 20;
        if (V[i].icon.n) V[i].w += 28, V[i].h = fmax(V[i].h, 32);
        if (i) V[i].sec = V[i].depth == 1 ? 0 : V[V[i].parent].sec;
    }
    /* sections = index among root's children; alternate sides */
    uint32_t k = 0;
    double hr = 0, hl = 0;
    for (uint32_t c = V[0].first; c != UINT32_MAX; c = V[c].next, k++) {
        measure_sub(V, c);
        /* balance: put on the lighter side */
        V[c].side = hr <= hl ? 1 : -1;
        if (V[c].side > 0) hr += V[c].sub; else hl += V[c].sub;
    }
    k = 0;
    for (size_t i = 1; i < v.n; i++) if (V[i].depth == 1) V[i].sec = k++;
    for (size_t i = 1; i < v.n; i++) if (V[i].depth > 1) V[i].sec = V[V[i].parent].sec;
    V[0].x = 0, V[0].y = 0;
    bool tidy = false;
    for (size_t i = 0; i + 9 <= off && !tidy; i++) tidy = !memcmp(src + i, "tidy-tree", 9);
    if (!tidy) {
        double *leaves = ANEW(a, double, v.n), *ang = ANEW(a, double, v.n), *wedge = ANEW(a, double, v.n);
        if (!leaves || !ang || !wedge) return CMM_ERR_NOMEM;
        for (size_t i = v.n; i-- > 0;) { /* children come after parents */
            if (V[i].first == UINT32_MAX) leaves[i] = 1;
            if (i) leaves[V[i].parent] += leaves[i];
        }
        ang[0] = 0, wedge[0] = 2 * M_PI;
        for (size_t i = 0; i < v.n; i++) { /* split each wedge among children, start angle at -90deg + offset */
            double start = ang[i] - wedge[i] / 2;
            if (!i) start = -M_PI / 2 - (V[0].first != UINT32_MAX ? M_PI / (double)leaves[0] : 0);
            for (uint32_t c = V[i].first; c != UINT32_MAX; c = V[c].next) {
                wedge[c] = wedge[i] * leaves[c] / fmax(1, leaves[i]);
                ang[c] = start + wedge[c] / 2;
                start += wedge[c];
            }
        }
        /* per-node radius: just past the parent, but far enough out that its chord fits its wedge */
        double *rad = ANEW(a, double, v.n);
        if (!rad) return CMM_ERR_NOMEM;
        rad[0] = 0;
        for (size_t i = 1; i < v.n; i++) { /* parents precede children */
            uint32_t p = V[i].parent;
            double si = hypot(V[i].w, V[i].h) / 2, sp = hypot(V[p].w, V[p].h) / 2;
            double r = rad[p] + sp + si + 30;
            double need = (2 * si + 12) / (2 * sin(fmin(wedge[i], M_PI) / 2));
            if (p && need > r) { /* only push out when the sibling-wedge is actually crowded */
                uint32_t nsib = 0;
                for (uint32_t c = V[p].first; c != UINT32_MAX; c = V[c].next) nsib++;
                if (nsib > 1 || leaves[i] > 1) r = need;
            } else if (!p && need > r) r = need;
            rad[i] = r;
        }
        for (size_t i = 1; i < v.n; i++) V[i].x = rad[i] * cos(ang[i]), V[i].y = rad[i] * sin(ang[i]);
    }
    for (int side = -1; side <= 1 && tidy; side += 2) {
        double tot = side > 0 ? hr : hl, y = -tot / 2;
        for (uint32_t c = V[0].first; c != UINT32_MAX; c = V[c].next) {
            if (V[c].side != side) continue;
            place(V, c, side * (V[0].w / 2 + 70 + V[c].w / 2), y, side);
            y += V[c].sub;
        }
    }
    BBox bb = BB_EMPTY;
    for (size_t i = 0; i < v.n; i++) bb_add(&bb, V[i].x - V[i].w / 2, V[i].y - V[i].h / 2, V[i].x + V[i].w / 2, V[i].y + V[i].h / 2);
    if (!buf_reserve(out, 2048 + v.n * 600)) return CMM_ERR_NOMEM;
    DG_HEAD(out, bb, 10, "mindmap", CSS);
    for (size_t i = 1; i < v.n; i++) { /* edges: cubic from parent side to child side, thinner with depth */
        const MNode *p = &V[V[i].parent], *c = &V[i];
        if (!tidy) {
            const char *col = SECS[c->sec % countof(SECS)];
            buf_lit(out, "<path class=\"me\" stroke=\""); buf_put(out, strlen(col), col);
            buf_lit(out, "\" stroke-width=\""); buf_num(out, fmax(2, 17 - 3.0 * c->depth)); buf_lit(out, "\" d=\"M");
            dg_pt(out, p->x, p->y); buf_ch(out, 'L'); dg_pt(out, c->x, c->y); buf_lit(out, "\"/>");
            continue;
        }
        double dir = c->x > p->x ? 1 : -1;
        double x0 = V[i].parent == 0 ? p->x : p->x + dir * p->w / 2, y0 = p->y;
        double x1 = c->x - dir * c->w / 2, y1 = c->y + (c->shape == M_DEF ? c->h / 2 : 0);
        const char *col = SECS[c->sec % countof(SECS)];
        buf_lit(out, "<path class=\"me\" stroke=\""); buf_put(out, strlen(col), col);
        buf_lit(out, "\" stroke-width=\""); buf_num(out, fmax(2, 17 - 3.0 * c->depth)); buf_lit(out, "\" d=\"M");
        dg_pt(out, x0, y0); buf_ch(out, 'C'); dg_pt(out, (x0 + x1) / 2, y0); buf_ch(out, ' ');
        dg_pt(out, (x0 + x1) / 2, y1); buf_ch(out, ' '); dg_pt(out, x1, y1); buf_lit(out, "\"/>");
    }
    for (size_t i = 0; i < v.n; i++) {
        const MNode *m = &V[i];
        const char *fill = i ? SEC[m->sec % countof(SEC)] : "#9370DB", *st = i ? SECS[m->sec % countof(SECS)] : "#6a4fb0";
        double x = m->x - m->w / 2, y = m->y - m->h / 2, w = m->w, h = m->h;
        buf_lit(out, "<g class=\"mindmap-node section-");
        char sn[16];
        buf_put(out, (size_t)snprintf(sn, sizeof sn, "%d", i ? (int)m->sec : -1), sn);
        if (m->cls.n) { buf_ch(out, ' '); buf_xml(out, m->cls); }
        buf_lit(out, "\" fill=\""); buf_put(out, strlen(fill), fill); buf_lit(out, "\" stroke=\""); buf_put(out, strlen(st), st);
        buf_lit(out, "\">");
        switch (m->shape) {
        case M_DEF:
            dg_rect_fill(out, x, y, w, h, 5, nullptr, nullptr);
            buf_lit(out, "<line stroke-width=\"3\" x1=\""); buf_num(out, x); buf_lit(out, "\" y1=\""); buf_num(out, y + h);
            buf_lit(out, "\" x2=\""); buf_num(out, x + w); buf_lit(out, "\" y2=\""); buf_num(out, y + h); buf_lit(out, "\"/>");
            break;
        case M_SQ: dg_rect_fill(out, x, y, w, h, 0, nullptr, nullptr); break;
        case M_RND: dg_rect_fill(out, x, y, w, h, 12, nullptr, nullptr); break;
        case M_CIR:
            buf_lit(out, "<circle cx=\""); buf_num(out, m->x); buf_lit(out, "\" cy=\""); buf_num(out, m->y);
            buf_lit(out, "\" r=\""); buf_num(out, w / 2); buf_lit(out, "\"/>");
            break;
        case M_HEX:
            buf_lit(out, "<path d=\"M"); dg_pt(out, x, m->y); buf_ch(out, 'L'); dg_pt(out, x + 12, y); buf_ch(out, 'L');
            dg_pt(out, x + w - 12, y); buf_ch(out, 'L'); dg_pt(out, x + w, m->y); buf_ch(out, 'L');
            dg_pt(out, x + w - 12, y + h); buf_ch(out, 'L'); dg_pt(out, x + 12, y + h); buf_lit(out, "Z\"/>");
            break;
        case M_BANG: case M_CLOUD: { /* scalloped outline: bumps (cloud) or spikes (bang) */
            int nx = (int)fmax(3, w / 30), ny = (int)fmax(2, h / 30);
            bool bang = m->shape == M_BANG;
            buf_lit(out, "<path d=\"M"); dg_pt(out, x, y);
            const double cxs[4][4] = {{x, y, x + w, y}, {x + w, y, x + w, y + h}, {x + w, y + h, x, y + h}, {x, y + h, x, y}};
            for (int s = 0; s < 4; s++) {
                int cnt = s % 2 ? ny : nx;
                double ax = cxs[s][0], ay = cxs[s][1], bx = cxs[s][2], by = cxs[s][3];
                double nxv = (by - ay), nyv = -(bx - ax), L = hypot(nxv, nyv);
                nxv /= -L, nyv /= -L;
                for (int j = 0; j < cnt; j++) {
                    double t0 = (double)j / cnt, t1 = (double)(j + 1) / cnt, tm = (t0 + t1) / 2, d = bang ? 10 : 8;
                    buf_ch(out, 'Q');
                    dg_pt(out, ax + (bx - ax) * tm + nxv * d * (bang ? 1 : 1.6), ay + (by - ay) * tm + nyv * d * (bang ? 1 : 1.6));
                    buf_ch(out, ' ');
                    dg_pt(out, ax + (bx - ax) * t1, ay + (by - ay) * t1);
                }
            }
            buf_lit(out, "Z\"/>");
            break;
        }
        }
        buf_lit(out, "</g>");
        double tx = m->x;
        if (m->icon.n) { /* badge with the icon's initial (fa-book -> B), label shifted right */
            Str nm = m->icon;
            for (size_t k = nm.n; k-- > 0;) if (nm.p[k] == ' ' || nm.p[k] == '-' || nm.p[k] == ':') { nm = (Str){.p = nm.p + k + 1, .n = nm.n - k - 1}; break; }
            double cx = x + 18;
            buf_lit(out, "<g class=\"icon\"><title>"); buf_xml(out, m->icon); buf_lit(out, "</title>");
            if (icon_draw(out, m->icon, cx - 11, m->y - 11, 22, i ? "mt" : "mt\" fill=\"#fff")) {
                buf_lit(out, "</g>");
                tx += 14;
                goto label;
            }
            buf_lit(out, "<title>"); buf_xml(out, m->icon); buf_lit(out, "</title><circle class=\"ic\" cx=\"");
            buf_num(out, cx); buf_lit(out, "\" cy=\""); buf_num(out, m->y); buf_lit(out, "\" r=\"10\"/>");
            char g[2] = {nm.n ? (char)(nm.p[0] >= 'a' && nm.p[0] <= 'z' ? nm.p[0] - 32 : nm.p[0]) : '?', 0};
            dg_text(out, (Str){.p = g, .n = 1}, 11, cx, m->y, "middle", "ict");
            buf_lit(out, "</g>");
            tx += 14;
        }
    label:
        dg_text(out, m->label, 16, tx, m->y, "middle", i ? "mt" : "mt\" fill=\"#fff");
    }
    buf_lit(out, "</svg>\n");
    return out->oom ? CMM_ERR_NOMEM : CMM_OK;
}
