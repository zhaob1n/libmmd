#include <math.h>
#include <stdlib.h>

#include "graph.h"
#include "text.h"

static constexpr char CSS[] =
    "#mermaid{font-family:\"trebuchet ms\",verdana,arial,sans-serif;font-size:16px;fill:#333;}"
    "#mermaid .node rect,#mermaid .node circle,#mermaid .node ellipse,#mermaid .node polygon,#mermaid .node path"
    "{fill:#ECECFF;stroke:#9370DB;stroke-width:1px;}"
    "#mermaid .flowchart-link{stroke:#333333;fill:none;}"
    "#mermaid .edgeLabel rect{fill:rgba(232,232,232,0.8);stroke:none;}"
    "#mermaid .cluster rect{fill:#ffffde;stroke:#aaaa33;stroke-width:1px;}"
    "#mermaid .marker{fill:#333333;stroke:#333333;}"
    "#mermaid text{fill:#333;}"
    "#mermaid .title{font-size:18px;}";

static constexpr char DEFS[] =
    "<defs>"
    "<marker id=\"mm-arrow-end\" class=\"marker\" viewBox=\"0 0 10 10\" refX=\"9\" refY=\"5\" "
    "markerUnits=\"userSpaceOnUse\" markerWidth=\"8\" markerHeight=\"8\" orient=\"auto\">"
    "<path d=\"M 0 0 L 10 5 L 0 10 z\" class=\"marker\"/></marker>"
    "<marker id=\"mm-arrow-start\" class=\"marker\" viewBox=\"0 0 10 10\" refX=\"1\" refY=\"5\" "
    "markerUnits=\"userSpaceOnUse\" markerWidth=\"8\" markerHeight=\"8\" orient=\"auto\">"
    "<path d=\"M 10 0 L 0 5 L 10 10 z\" class=\"marker\"/></marker>"
    "<marker id=\"mm-circle-end\" class=\"marker\" viewBox=\"0 0 10 10\" refX=\"9\" refY=\"5\" "
    "markerUnits=\"userSpaceOnUse\" markerWidth=\"10\" markerHeight=\"10\" orient=\"auto\">"
    "<circle cx=\"5\" cy=\"5\" r=\"4\" class=\"marker\"/></marker>"
    "<marker id=\"mm-circle-start\" class=\"marker\" viewBox=\"0 0 10 10\" refX=\"1\" refY=\"5\" "
    "markerUnits=\"userSpaceOnUse\" markerWidth=\"10\" markerHeight=\"10\" orient=\"auto\">"
    "<circle cx=\"5\" cy=\"5\" r=\"4\" class=\"marker\"/></marker>"
    "<marker id=\"mm-cross-end\" class=\"marker\" viewBox=\"0 0 11 11\" refX=\"10\" refY=\"5.5\" "
    "markerUnits=\"userSpaceOnUse\" markerWidth=\"11\" markerHeight=\"11\" orient=\"auto\">"
    "<path d=\"M 1 1 L 10 10 M 10 1 L 1 10\" class=\"marker\" style=\"stroke-width:2px\"/></marker>"
    "<marker id=\"mm-cross-start\" class=\"marker\" viewBox=\"0 0 11 11\" refX=\"1\" refY=\"5.5\" "
    "markerUnits=\"userSpaceOnUse\" markerWidth=\"11\" markerHeight=\"11\" orient=\"auto\">"
    "<path d=\"M 1 1 L 10 10 M 10 1 L 1 10\" class=\"marker\" style=\"stroke-width:2px\"/></marker>"
    "</defs>";

/* emit css list "a:b,c:d" as "a:b;c:d" ; text=true picks color->fill only */
static void css_out(Buf *b, Str css, bool text) {
    size_t i = 0;
    while (i < css.n) {
        size_t st = i;
        int depth = 0;
        while (i < css.n && !(css.p[i] == ',' && depth == 0) && css.p[i] != ';') {
            if (css.p[i] == '(') depth++;
            else if (css.p[i] == ')') depth--;
            i++;
        }
        Str decl = str_trim((Str){.p = css.p + st, .n = i - st});
        i++;
        if (!decl.n) continue;
        bool is_color = str_starts(decl, S("color"));
        if (text) {
            if (!is_color) continue;
            const char *c = memchr(decl.p, ':', decl.n);
            if (!c) continue;
            buf_lit(b, "fill:");
            buf_xml(b, str_trim((Str){.p = c + 1, .n = decl.n - (size_t)(c + 1 - decl.p)}));
            buf_ch(b, ';');
        } else {
            if (is_color) continue;
            buf_xml(b, decl);
            buf_ch(b, ';');
        }
    }
}

static Str find_classdef(const Graph *g, Str name) {
    for (size_t i = 0; i < g->classdefs.n; i++)
        if (str_eq(g->classdefs.v[i].name, name)) return g->classdefs.v[i].css;
    return (Str){};
}

static void style_attr(Buf *b, const Graph *g, Str classes, Str style, bool text, bool node) {
    size_t mark = b->n;
    buf_lit(b, " style=\"");
    size_t body = b->n;
    if (node) css_out(b, find_classdef(g, S("default")), text);
    Str cl = classes;
    while (cl.n) {
        const char *sp = memchr(cl.p, ' ', cl.n);
        size_t len = sp ? (size_t)(sp - cl.p) : cl.n;
        css_out(b, find_classdef(g, (Str){.p = cl.p, .n = len}), text);
        cl.p += len; cl.n -= len;
        if (cl.n) cl.p++, cl.n--;
    }
    css_out(b, style, text);
    if (b->n == body) { b->n = mark; return; }
    buf_ch(b, '"');
}

static void text_lines(Buf *b, Str lab, double fs, double cx, double cy, const char *cls) {
    unsigned k = 0;
    size_t pos = 0;
    Str ln;
    while (label_next_line(lab, &pos, &ln)) k++;
    if (!k || !lab.n) return;
    double lh = fs * 1.5, y = cy - (k - 1) * lh / 2;
    buf_lit(b, "<text");
    if (cls) { buf_lit(b, " class=\""); buf_put(b, strlen(cls), cls); buf_ch(b, '"'); }
    buf_lit(b, " text-anchor=\"middle\" dominant-baseline=\"central\"");
    if (k == 1) {
        buf_lit(b, " x=\""); buf_num(b, cx); buf_lit(b, "\" y=\""); buf_num(b, y); buf_lit(b, "\">");
        buf_xml(b, lab);
        buf_lit(b, "</text>");
        return;
    }
    buf_ch(b, '>');
    pos = 0;
    while (label_next_line(lab, &pos, &ln)) {
        buf_lit(b, "<tspan x=\""); buf_num(b, cx); buf_lit(b, "\" y=\""); buf_num(b, y); buf_lit(b, "\">");
        buf_xml(b, ln);
        buf_lit(b, "</tspan>");
        y += lh;
    }
    buf_lit(b, "</text>");
}

static void pt(Buf *b, double x, double y) { buf_num(b, x); buf_ch(b, ','); buf_num(b, y); }
static void poly(Buf *b, size_t n, const double xy[static 2 * n]) {
    buf_lit(b, "<polygon points=\"");
    for (size_t i = 0; i < n; i++) { if (i) buf_ch(b, ' '); pt(b, xy[2 * i], xy[2 * i + 1]); }
    buf_ch(b, '"');
}

static void node_shape(Buf *b, const Graph *g, size_t v) {
    double w = g->nw[v], h = g->nh[v], hw = w / 2, hh = h / 2;
    switch (g->nshape[v]) {
    case SH_RECT: case SH_ROUND: case SH_STADIUM: case SH_SUBROUTINE: {
        double rx = g->nshape[v] == SH_ROUND ? 5 : g->nshape[v] == SH_STADIUM ? hh : 0;
        buf_lit(b, "<rect x=\""); buf_num(b, -hw); buf_lit(b, "\" y=\""); buf_num(b, -hh);
        buf_lit(b, "\" width=\""); buf_num(b, w); buf_lit(b, "\" height=\""); buf_num(b, h); buf_ch(b, '"');
        if (rx > 0) { buf_lit(b, " rx=\""); buf_num(b, rx); buf_lit(b, "\" ry=\""); buf_num(b, rx); buf_ch(b, '"'); }
        break;
    }
    case SH_CYLINDER: {
        double rx = hw, ry = rx / (2.5 + w / 50), body = h - 2 * ry;
        buf_lit(b, "<path d=\"M"); pt(b, -hw, -hh + ry);
        buf_lit(b, "a"); pt(b, rx, ry); buf_lit(b, " 0 0 0 "); pt(b, w, 0);
        buf_lit(b, "a"); pt(b, rx, ry); buf_lit(b, " 0 0 0 "); pt(b, -w, 0);
        buf_lit(b, "l0,"); buf_num(b, body);
        buf_lit(b, "a"); pt(b, rx, ry); buf_lit(b, " 0 0 0 "); pt(b, w, 0);
        buf_lit(b, "l0,"); buf_num(b, -body); buf_ch(b, '"');
        break;
    }
    case SH_CIRCLE: case SH_DCIRCLE:
        buf_lit(b, "<circle r=\""); buf_num(b, hw); buf_ch(b, '"');
        break;
    case SH_ASYM: { double p[] = {-hw, -hh, hw, -hh, hw, hh, -hw, hh, -hw + h / 4, 0}; poly(b, 5, p); break; }
    case SH_RHOMBUS: { double p[] = {0, -hh, hw, 0, 0, hh, -hw, 0}; poly(b, 4, p); break; }
    case SH_HEXAGON: { double f = h / 4; double p[] = {-hw + f, -hh, hw - f, -hh, hw, 0, hw - f, hh, -hw + f, hh, -hw, 0}; poly(b, 6, p); break; }
    case SH_LEAN_R: { double d = h / 3; double p[] = {-hw + d, -hh, hw, -hh, hw - d, hh, -hw, hh}; poly(b, 4, p); break; }
    case SH_LEAN_L: { double d = h / 3; double p[] = {-hw, -hh, hw - d, -hh, hw, hh, -hw + d, hh}; poly(b, 4, p); break; }
    case SH_TRAP: { double d = h / 3; double p[] = {-hw + d, -hh, hw - d, -hh, hw, hh, -hw, hh}; poly(b, 4, p); break; }
    case SH_TRAP_ALT: { double d = h / 3; double p[] = {-hw, -hh, hw, -hh, hw - d, hh, -hw + d, hh}; poly(b, 4, p); break; }
    default: break;
    }
    style_attr(b, g, g->nclass[v], g->nstyle[v], false, true);
    buf_lit(b, "/>");
    if (g->nshape[v] == SH_DCIRCLE) {
        buf_lit(b, "<circle r=\""); buf_num(b, hw - 5); buf_ch(b, '"');
        style_attr(b, g, g->nclass[v], g->nstyle[v], false, true);
        buf_lit(b, "/>");
    } else if (g->nshape[v] == SH_SUBROUTINE) {
        for (int s = -1; s <= 1; s += 2) {
            buf_lit(b, "<path d=\"M"); pt(b, s * (hw - 8), -hh); buf_lit(b, "V"); buf_num(b, hh); buf_ch(b, '"');
            style_attr(b, g, g->nclass[v], g->nstyle[v], false, true);
            buf_lit(b, "/>");
        }
    }
}

/* d3 curveBasis */
static void basis_path(Buf *b, size_t n, const double P[static 2 * n]) {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    unsigned st = 0;
    for (size_t i = 0; i < n; i++) {
        double x = P[2 * i], y = P[2 * i + 1];
        switch (st) {
        case 0: st = 1; buf_ch(b, 'M'); pt(b, x, y); break;
        case 1: st = 2; break;
        case 2:
            st = 3;
            buf_ch(b, 'L'); pt(b, (5 * x0 + x1) / 6, (5 * y0 + y1) / 6);
            [[fallthrough]];
        default:
            buf_ch(b, 'C'); pt(b, (2 * x0 + x1) / 3, (2 * y0 + y1) / 3);
            buf_ch(b, ' '); pt(b, (x0 + 2 * x1) / 3, (y0 + 2 * y1) / 3);
            buf_ch(b, ' '); pt(b, (x0 + 4 * x1 + x) / 6, (y0 + 4 * y1 + y) / 6);
            break;
        }
        x0 = x1, x1 = x, y0 = y1, y1 = y;
    }
    switch (st) {
    case 3:
        buf_ch(b, 'C'); pt(b, (2 * x0 + x1) / 3, (2 * y0 + y1) / 3);
        buf_ch(b, ' '); pt(b, (x0 + 2 * x1) / 3, (y0 + 2 * y1) / 3);
        buf_ch(b, ' '); pt(b, (x0 + 5 * x1) / 6, (y0 + 5 * y1) / 6);
        [[fallthrough]];
    case 2: buf_ch(b, 'L'); pt(b, x1, y1); break;
    default: break;
    }
}

static const char *marker_name(Arrow a, bool end) {
    switch (a) {
    case AH_ARROW: return end ? "mm-arrow-end" : "mm-arrow-start";
    case AH_CIRCLE: return end ? "mm-circle-end" : "mm-circle-start";
    case AH_CROSS: return end ? "mm-cross-end" : "mm-cross-start";
    default: return nullptr;
    }
}

typedef struct BBox { double x0, y0, x1, y1; } BBox;
static inline void bb_add(BBox *bb, double x0, double y0, double x1, double y1) {
    if (x0 < bb->x0) bb->x0 = x0;
    if (y0 < bb->y0) bb->y0 = y0;
    if (x1 > bb->x1) bb->x1 = x1;
    if (y1 > bb->y1) bb->y1 = y1;
}

cmm_status flow_svg(const Graph *g, Buf *b) {
    double fs = g->font_size;
    BBox bb = {INFINITY, INFINITY, -INFINITY, -INFINITY};
    for (size_t v = 0; v < g->nn; v++) {
        if (g->nflags[v] & NF_HIDDEN) continue;
        bb_add(&bb, g->nx[v] - g->nw[v] / 2, g->ny[v] - g->nh[v] / 2, g->nx[v] + g->nw[v] / 2, g->ny[v] + g->nh[v] / 2);
    }
    for (size_t s = 0; s < g->ns; s++)
        if (g->sw[s] > 0) bb_add(&bb, g->sx[s], g->sy[s], g->sx[s] + g->sw[s], g->sy[s] + g->sh[s]);
    for (size_t e = 0; e < g->ne; e++) {
        const double *P = g->pts.v + g->eptoff[e];
        for (uint32_t i = 0; i < g->eptn[e]; i++) bb_add(&bb, P[2 * i], P[2 * i + 1], P[2 * i], P[2 * i + 1]);
        if (g->elw[e] > 0 && (g->eptn[e] || g->efrom[e] == g->eto[e]))
            bb_add(&bb, g->elx[e] - g->elw[e] / 2, g->ely[e] - g->elh[e] / 2, g->elx[e] + g->elw[e] / 2, g->ely[e] + g->elh[e] / 2);
        if (g->efrom[e] == g->eto[e] && !(g->nflags[g->efrom[e]] & NF_HIDDEN)) {
            int32_t v = g->efrom[e];
            bb_add(&bb, g->nx[v], g->ny[v], g->nx[v] + g->nw[v] / 2 + 40, g->ny[v]);
        }
    }
    if (bb.x0 == INFINITY) bb = (BBox){0, 0, 0, 0};
    double title_h = g->title.n ? 30 : 0;
    double m = 8, vx = bb.x0 - m, vy = bb.y0 - m - title_h, vw = bb.x1 - bb.x0 + 2 * m, vh = bb.y1 - bb.y0 + 2 * m + title_h;

    buf_lit(b, "<svg xmlns=\"http://www.w3.org/2000/svg\" id=\"mermaid\" class=\"flowchart\" width=\"");
    buf_num(b, vw); buf_lit(b, "\" height=\""); buf_num(b, vh);
    buf_lit(b, "\" viewBox=\""); buf_num(b, vx); buf_ch(b, ' '); buf_num(b, vy); buf_ch(b, ' ');
    buf_num(b, vw); buf_ch(b, ' '); buf_num(b, vh);
    buf_lit(b, "\" role=\"graphics-document document\" aria-roledescription=\"flowchart-v2\"><style>");
    buf_lit(b, CSS);
    buf_lit(b, "</style>");
    buf_lit(b, DEFS);
    buf_lit(b, "<rect x=\""); buf_num(b, vx); buf_lit(b, "\" y=\""); buf_num(b, vy);
    buf_lit(b, "\" width=\""); buf_num(b, vw); buf_lit(b, "\" height=\""); buf_num(b, vh);
    buf_lit(b, "\" fill=\"white\"/>");
    if (g->title.n) text_lines(b, g->title, 18, (bb.x0 + bb.x1) / 2, bb.y0 - title_h / 2, "title");
    buf_lit(b, "<g class=\"root\"><g class=\"clusters\">");
    /* parents before children */
    for (size_t s = 0; s < g->ns; s++) {
        if (g->sw[s] <= 0) continue;
        buf_lit(b, "<g class=\"cluster"); if (g->sclass[s].n) { buf_ch(b, ' '); buf_xml(b, g->sclass[s]); }
        buf_lit(b, "\" id=\""); buf_xml(b, g->sid[s]); buf_lit(b, "\"><rect x=\"");
        buf_num(b, g->sx[s]); buf_lit(b, "\" y=\""); buf_num(b, g->sy[s]);
        buf_lit(b, "\" width=\""); buf_num(b, g->sw[s]); buf_lit(b, "\" height=\""); buf_num(b, g->sh[s]); buf_ch(b, '"');
        style_attr(b, g, g->sclass[s], g->sstyle[s], false, false);
        buf_lit(b, "/>");
        text_lines(b, g->stitle[s], fs, g->sx[s] + g->sw[s] / 2, g->sy[s] + 15 + 12 - 6, "cluster-label");
        buf_lit(b, "</g>");
    }
    buf_lit(b, "</g><g class=\"edgePaths\">");
    Str lsdef = {};
    for (size_t i = 0; i < g->linkstyles.n; i++) if (g->linkstyles.v[i].idx < 0) lsdef = g->linkstyles.v[i].css;
    for (size_t e = 0; e < g->ne; e++) {
        if (g->estroke[e] == ST_INVISIBLE) continue;
        bool self = g->efrom[e] == g->eto[e];
        if (!g->eptn[e] && !self) continue;
        buf_lit(b, "<path class=\"flowchart-link\" d=\"");
        if (self) {
            int32_t v = g->efrom[e];
            double x = g->nx[v] + g->nw[v] / 2, y = g->ny[v], q = g->nh[v] / 4;
            buf_ch(b, 'M'); pt(b, x, y - q);
            buf_ch(b, 'C'); pt(b, x + 40, y - q - 20); buf_ch(b, ' '); pt(b, x + 40, y + q + 20);
            buf_ch(b, ' '); pt(b, x, y + q);
        } else basis_path(b, g->eptn[e], g->pts.v + g->eptoff[e]);
        buf_lit(b, "\" style=\"");
        switch (g->estroke[e]) {
        case ST_THICK: buf_lit(b, "stroke-width:3.5px;"); break;
        case ST_DOTTED: buf_lit(b, "stroke-width:2px;stroke-dasharray:3;"); break;
        default: buf_lit(b, "stroke-width:2px;"); break;
        }
        css_out(b, lsdef, false);
        for (size_t i = 0; i < g->linkstyles.n; i++)
            if (g->linkstyles.v[i].idx == (int32_t)e) css_out(b, g->linkstyles.v[i].css, false);
        buf_ch(b, '"');
        const char *ms = marker_name(g->estart[e], false), *me = marker_name(g->eend[e], true);
        if (ms) { buf_lit(b, " marker-start=\"url(#"); buf_put(b, strlen(ms), ms); buf_lit(b, ")\""); }
        if (me) { buf_lit(b, " marker-end=\"url(#"); buf_put(b, strlen(me), me); buf_lit(b, ")\""); }
        buf_lit(b, "/>");
    }
    buf_lit(b, "</g><g class=\"edgeLabels\">");
    for (size_t e = 0; e < g->ne; e++) {
        if (!g->elabel[e].n || g->estroke[e] == ST_INVISIBLE) continue;
        if (!g->eptn[e] && g->efrom[e] != g->eto[e]) continue;
        double x = g->elx[e], y = g->ely[e], w = g->elw[e], h = g->elh[e];
        buf_lit(b, "<g class=\"edgeLabel\"><rect class=\"background\" x=\""); buf_num(b, x - w / 2);
        buf_lit(b, "\" y=\""); buf_num(b, y - h / 2); buf_lit(b, "\" width=\""); buf_num(b, w);
        buf_lit(b, "\" height=\""); buf_num(b, h); buf_lit(b, "\"/>");
        text_lines(b, g->elabel[e], fs, x, y, nullptr);
        buf_lit(b, "</g>");
    }
    buf_lit(b, "</g><g class=\"nodes\">");
    for (size_t v = 0; v < g->nn; v++) {
        if (g->nflags[v] & NF_HIDDEN) continue;
        buf_lit(b, "<g class=\"node default");
        if (g->nclass[v].n) { buf_ch(b, ' '); buf_xml(b, g->nclass[v]); }
        buf_lit(b, "\" id=\"flowchart-"); buf_xml(b, g->nid[v]); buf_ch(b, '-'); buf_int(b, (int64_t)v);
        buf_lit(b, "\" transform=\"translate("); pt(b, g->nx[v], g->ny[v]); buf_lit(b, ")\">");
        node_shape(b, g, v);
        size_t mark = b->n;
        text_lines(b, g->nlabel[v], fs, 0, 0, "nodeLabel");
        /* inject text colour if class/style specifies color: */
        if (b->n > mark) {
            Buf tmp = {};
            style_attr(&tmp, g, g->nclass[v], g->nstyle[v], true, true);
            if (tmp.oom) { free(tmp.p); return CMM_ERR_NOMEM; }
            if (tmp.n) {
                /* insert after "<text" */
                if (!buf_reserve(b, tmp.n)) { free(tmp.p); return CMM_ERR_NOMEM; }
                memmove(b->p + mark + 5 + tmp.n, b->p + mark + 5, b->n - mark - 5);
                memcpy(b->p + mark + 5, tmp.p, tmp.n);
                b->n += tmp.n;
            }
            free(tmp.p);
        }
        buf_lit(b, "</g>");
    }
    buf_lit(b, "</g></g></svg>\n");
    return b->oom ? CMM_ERR_NOMEM : CMM_OK;
}
