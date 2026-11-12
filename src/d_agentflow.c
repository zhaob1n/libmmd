/* agentflow-beta (mermaid 12): flowchart syntax with agent vocabulary. flow_parse reads it (`flow`,
 * `global`, `connector`, the shape aliases, `view: collapsed`) and the flowchart layout places it;
 * this file folds collapsed containers into nodes and draws mermaid's defaults for the type, the neo
 * look in redux-color: a fixed colour per node kind, a cycling slot per container, 120px label
 * boxes, orthogonal edges. With `look: classic` the flowchart renderer draws it instead.
 * mermaid lays it out with ELK; the layered layout stands in. */
#include <math.h>
#include <string.h>

#include "dcfg.h"
#include "dgraph.h"
#include "diagrams.h"
#include "rich.h"
#include "text.h"

struct AfInfo {
    bool neo;
    int32_t *sord; /* per subgraph: declaration ordinal (colour slot) */
    int32_t *coll; /* per node: ordinal of the collapsed container it stands for, or NONE */
};

enum { KIND_TOOL, KIND_TASK, KIND_DECISION, KIND_INPUT, KIND_REFDOC, KIND_CONNECTOR, KIND_ACTION, KIND_COUNT };

static bool has_class(Str classes, Str c) {
    for (size_t i = 0; i + c.n <= classes.n; i++)
        if (!memcmp(classes.p + i, c.p, c.n) && (i == 0 || classes.p[i - 1] == ' ') &&
            (i + c.n == classes.n || classes.p[i + c.n] == ' '))
            return true;
    return false;
}

static unsigned kind(const Graph g[static 1], size_t v) {
    if (has_class(g->nclass[v], S("af-kind-connector"))) return KIND_CONNECTOR;
    switch (g->nshape[v]) {
    case SH_SUBROUTINE: return KIND_TOOL;
    case SH_RHOMBUS: return KIND_DECISION;
    case SH_LEAN_R: return KIND_INPUT;
    case SH_DOC: return KIND_REFDOC;
    case SH_HEXAGON: return KIND_ACTION;
    default: return KIND_TASK;
    }
}
/* containers cycle the palette slots above the kinds */
static size_t cont_slot(int32_t ord) { return KIND_COUNT + (size_t)ord % (12 - KIND_COUNT); }

/* fold each outermost `view: collapsed` container into one node standing for it: its members and
 * nested containers go, edges crossing its boundary end at the node, edges inside it are dropped */
static mmd_status collapse(Graph g[static 1], AfInfo inf[static 1]) {
    size_t S = g->ns;
    int32_t *root = ANEW(g->arena, int32_t, S + 1), *sv = ANEW(g->arena, int32_t, S + 1);
    if (!root || !sv) return MMD_ERR_NOMEM;
    bool any = false;
    for (size_t s = 0; s < S; s++) {
        root[s] = NONE, sv[s] = NONE;
        for (int32_t a = (int32_t)s; a != NONE; a = g->sparent[a])
            if (g->sflags[a] & SF_COLLAPSED) root[s] = a;
        any |= root[s] != NONE;
    }
    if (!any) return MMD_OK;
    for (size_t s = 0; s < S; s++) {
        if (root[s] != (int32_t)s) continue;
        bool fresh;
        int32_t v = graph_intern(g, g->sid[s], &fresh);
        if (v == NONE) return MMD_ERR_NOMEM;
        g->nflags[v] = NF_DEFINED;
        g->nlabel[v] = g->stitle[s].n ? g->stitle[s] : g->sid[s];
        g->nshape[v] = SH_ROUND;
        g->nparent[v] = g->sparent[s];
        sv[s] = v;
    }
    size_t N = g->nn;
    int32_t *rep = ANEW(g->arena, int32_t, N + 1);
    inf->coll = ANEW(g->arena, int32_t, N + 1);
    if (!rep || !inf->coll) return MMD_ERR_NOMEM;
    for (size_t v = 0; v < N; v++) {
        rep[v] = (int32_t)v, inf->coll[v] = NONE;
        int32_t s = g->nparent[v];
        int32_t k = g->nflags[v] & NF_SUBREF ? graph_find_sg(g, g->nid[v]) : NONE;
        if (k != NONE && root[k] != NONE) rep[v] = sv[root[k]]; /* an edge to a container: to its collapsed node if inside one */
        if (s != NONE && root[s] != NONE && sv[s] != (int32_t)v) rep[v] = sv[root[s]];
        if (rep[v] != (int32_t)v) g->nflags[v] |= NF_HIDDEN, g->nparent[v] = NONE;
    }
    for (size_t s = 0; s < S; s++)
        if (sv[s] != NONE) inf->coll[sv[s]] = inf->sord[s];
    size_t k = 0;
    for (size_t e = 0; e < g->ne; e++) {
        int32_t a = rep[g->efrom[e]], b = rep[g->eto[e]];
        if (a == b && g->efrom[e] != g->eto[e]) continue;
        g->efrom[k] = a, g->eto[k] = b, g->elabel[k] = g->elabel[e], g->estroke[k] = g->estroke[e];
        g->estart[k] = g->estart[e], g->eend[k] = g->eend[e], g->elen[k] = g->elen[e], g->eid[k] = g->eid[e];
        g->eflags[k++] = g->eflags[e];
    }
    g->ne = k;
    int32_t *map = root; /* reuse: new index of each kept container */
    k = 0;
    for (size_t s = 0; s < S; s++) {
        if (root[s] != NONE) { map[s] = NONE; continue; }
        map[s] = (int32_t)k;
        g->sid[k] = g->sid[s], g->stitle[k] = g->stitle[s], g->sclass[k] = g->sclass[s], g->sstyle[k] = g->sstyle[s];
        g->sparent[k] = g->sparent[s], g->sdir[k] = g->sdir[s], g->sflags[k] = g->sflags[s], inf->sord[k] = inf->sord[s];
        k++;
    }
    g->ns = k;
    for (size_t s = 0; s < k; s++) if (g->sparent[s] != NONE) g->sparent[s] = map[g->sparent[s]];
    for (size_t v = 0; v < g->nn; v++) if (g->nparent[v] != NONE) g->nparent[v] = map[g->nparent[v]];
    return MMD_OK;
}

constexpr double AF_FS = 14;
constexpr double AF_LH = 21;
constexpr double AF_LABEL_W = 120;

/* mermaid's neo shapes around an AF_LABEL_W (wrapping) label box */
static void neo_size(Graph g[static 1], size_t v, bool coll) {
    Str lab = g->nlabel[v];
    if (lab.n && lab.p[0] == RICH_MD) lab.p++, lab.n--;
    Str w = dg_wrap(g->arena, lab, AF_FS, AF_LABEL_W);
    if (w.n || !lab.n) g->nlabel[v] = w;
    double tw;
    unsigned lines;
    label_measure(g->nlabel[v], AF_FS, &tw, &lines);
    double lw = coll ? tw : fmax(AF_LABEL_W, tw), lh = AF_LH * (lines ? lines : 1), W, H;
    if (coll) W = lw + 16, H = lh + 44;
    else switch (g->nshape[v]) {
    case SH_SUBROUTINE: W = lw + 44, H = lh + 12; break;
    case SH_LEAN_R: H = lh + 15, W = lw + 30 + H; break;
    case SH_RHOMBUS: W = H = lw + lh + 30; break;
    case SH_HEXAGON: W = lw + 84, H = lh + 70; break;
    case SH_DOC: W = (lw + 32) * 1.1, H = (lh + 24) * 1.5; break;
    default: W = lw + 30, H = lh + 30; break;
    }
    g->nw[v] = W, g->nh[v] = H;
    g->nflags[v] |= NF_FIXED;
}

mmd_status af_prepare(Graph *g, AfInfo *out[static 1]) {
    AfInfo *inf = ANEW(g->arena, AfInfo, 1);
    if (!inf) return MMD_ERR_NOMEM;
    *inf = (AfInfo){.neo = dcfg_neo()};
    inf->sord = ANEW(g->arena, int32_t, g->ns + 1);
    if (!inf->sord) return MMD_ERR_NOMEM;
    for (size_t s = 0; s < g->ns; s++) inf->sord[s] = (int32_t)s;
    mmd_status st = collapse(g, inf);
    if (st) return st;
    *out = inf;
    if (!inf->neo) return MMD_OK;
    g->font_size = AF_FS;
    for (size_t v = 0; v < g->nn; v++)
        if (!(g->nflags[v] & NF_HIDDEN)) neo_size(g, v, inf->coll && inf->coll[v] != NONE);
    return MMD_OK;
}

bool af_neo(const AfInfo *inf) { return inf && inf->neo; }

static constexpr char CSS[] =
    "#mermaid{font-family:\"Recursive Variable\",arial,sans-serif;font-size:14px;fill:#28253D;}"
    "#mermaid .shape{stroke-width:2px;filter:url(#neo-shadow);}"
    "#mermaid .flow{stroke-width:1px;filter:url(#neo-shadow);}"
    "#mermaid .title{fill:#000000;}"
    "#mermaid .link{fill:none;stroke:#000000;stroke-width:2px;}#mermaid .link.dotted{stroke-dasharray:2;}"
    "#mermaid .link.thick{stroke-width:3.5px;}"
    "#mermaid .sep{fill:none;stroke:#b3b3b3;stroke-dasharray:3,3;}#mermaid .dot{fill:#b3b3b3;}" DG_NEO_CSS;

static void colors(Buf b[static 1], NeoColor c) {
    buf_lit(b, " fill=\""); buf_cstr(b, c.fill); buf_lit(b, "\" stroke=\""); buf_cstr(b, c.stroke); buf_ch(b, '"');
}

static void shape(Buf b[static 1], const Graph g[static 1], size_t v, NeoColor c) {
    double x = g->nx[v], y = g->ny[v], w = g->nw[v], h = g->nh[v], hw = w / 2, hh = h / 2;
    switch (g->nshape[v]) {
    case SH_SUBROUTINE: case SH_ROUND: {
        bool sub = g->nshape[v] == SH_SUBROUTINE;
        buf_lit(b, "<rect class=\"shape\" x=\""); buf_num(b, x - hw); buf_lit(b, "\" y=\""); buf_num(b, y - hh);
        buf_lit(b, "\" width=\""); buf_num(b, w); buf_lit(b, "\" height=\""); buf_num(b, h);
        buf_cstr(b, sub ? "\"" : "\" rx=\"12\"");
        break;
    }
    case SH_DOC:
        buf_lit(b, "<path class=\"shape\" d=\""); flow_doc_d(b, x, y, w, h); buf_ch(b, '"');
        break;
    default: {
        double P[12];
        size_t n;
        if (g->nshape[v] == SH_RHOMBUS) {
            double q[] = {x, y - hh, x + hw, y, x, y + hh, x - hw, y}; memcpy(P, q, sizeof q); n = 4;
        } else if (g->nshape[v] == SH_LEAN_R) {
            double q[] = {x - hw + hh, y - hh, x + hw, y - hh, x + hw - hh, y + hh, x - hw, y + hh}; memcpy(P, q, sizeof q); n = 4;
        } else {
            double m = h * 2 / 7;
            double q[] = {x - hw + m, y - hh, x + hw - m, y - hh, x + hw, y, x + hw - m, y + hh, x - hw + m, y + hh, x - hw, y};
            memcpy(P, q, sizeof q); n = 6;
        }
        buf_lit(b, "<polygon class=\"shape\" points=\"");
        for (size_t i = 0; i < n; i++) { if (i) buf_ch(b, ' '); buf_pt(b, P[2 * i], P[2 * i + 1]); }
        buf_ch(b, '"');
    }
    }
    colors(b, c);
    flow_style_attr(b, g, g->nclass[v], g->nstyle[v], false, true);
    buf_lit(b, "/>");
    if (g->nshape[v] == SH_SUBROUTINE)
        for (int s = -1; s <= 1; s += 2) {
            buf_lit(b, "<path d=\"M"); buf_pt(b, x + s * (hw - 8), y - hh); buf_ch(b, 'V'); buf_num(b, y + hh);
            buf_ch(b, '"'); colors(b, c); buf_lit(b, " stroke-width=\"2\"/>");
        }
}

mmd_status af_svg(const Graph *g, const AfInfo *inf, Buf *b) {
    BBox bb = dg_bbox(g);
    if (g->title.n) bb.y0 -= 30;
    if (!buf_reserve(b, 4096 + g->nn * 600 + g->ne * 400 + g->ns * 300)) return MMD_ERR_NOMEM;
    DG_HEAD(b, bb, 8, "agentflow", CSS);
    dg_neo_defs(b);
    if (g->title.n) dg_text(b, g->title, 18, (bb.x0 + bb.x1) / 2, bb.y0 + 12, "middle", "title");
    for (size_t s = 0; s < g->ns; s++) {
        if (g->sw[s] <= 0) continue;
        buf_lit(b, "<rect class=\"flow\" x=\""); buf_num(b, g->sx[s]); buf_lit(b, "\" y=\""); buf_num(b, g->sy[s]);
        buf_lit(b, "\" width=\""); buf_num(b, g->sw[s]); buf_lit(b, "\" height=\""); buf_num(b, g->sh[s]);
        buf_lit(b, "\" rx=\"10\""); colors(b, dg_redux(cont_slot(inf->sord[s])));
        flow_style_attr(b, g, g->sclass[s], g->sstyle[s], false, false);
        buf_lit(b, "/>");
        dg_text(b, g->stitle[s].n ? g->stitle[s] : g->sid[s], AF_FS, g->sx[s] + g->sw[s] / 2, g->sy[s] + AF_LH / 2, "middle", "title");
    }
    const double *ports = dg_ports(g);
    for (size_t e = 0; e < g->ne; e++) {
        double mid[2];
        size_t mark = b->n;
        buf_lit(b, "<path class=\"link");
        if (g->estroke[e] == ST_DOTTED) buf_lit(b, " dotted");
        else if (g->estroke[e] == ST_THICK) buf_lit(b, " thick");
        else if (g->estroke[e] == ST_INVISIBLE) { b->n = mark; continue; }
        buf_lit(b, "\" d=\"");
        if (!dg_ortho_d(b, g, e, ports, mid)) {
            if (!g->eptn[e] && g->efrom[e] != g->eto[e]) { b->n = mark; continue; }
            dg_edge_d(b, g, e);
            mid[0] = g->elx[e], mid[1] = g->ely[e];
        }
        buf_ch(b, '"');
        for (int end = 0; end < 2; end++) {
            const char *m = dg_neo_marker(end ? g->eend[e] : g->estart[e], end);
            if (!m) continue;
            buf_cstr(b, end ? " marker-end=\"url(#" : " marker-start=\"url(#"); buf_cstr(b, m); buf_lit(b, ")\"");
        }
        buf_lit(b, "/>");
        if (g->elabel[e].n && g->elw[e] > 0) {
            dg_rect(b, mid[0] - g->elw[e] / 2, mid[1] - g->elh[e] / 2, g->elw[e], g->elh[e], 0, "elbl");
            dg_text(b, g->elabel[e], AF_FS, mid[0], mid[1], "middle", nullptr);
        }
    }
    for (size_t v = 0; v < g->nn; v++) {
        if (g->nflags[v] & NF_HIDDEN) continue;
        int32_t co = inf->coll ? inf->coll[v] : NONE;
        double x = g->nx[v], y = g->ny[v], hw = g->nw[v] / 2, hh = g->nh[v] / 2;
        if (co != NONE) { /* collapsed container: title, a dashed rule, three dots */
            buf_lit(b, "<rect class=\"flow\" x=\""); buf_num(b, x - hw); buf_lit(b, "\" y=\""); buf_num(b, y - hh);
            buf_lit(b, "\" width=\""); buf_num(b, 2 * hw); buf_lit(b, "\" height=\""); buf_num(b, 2 * hh);
            buf_lit(b, "\" rx=\"10\""); colors(b, dg_redux(cont_slot(co))); buf_lit(b, "/>");
            double ty = y - hh + 8 + AF_LH / 2;
            dg_text(b, g->nlabel[v], AF_FS, x, ty, "middle", nullptr);
            dg_line(b, x - hw + 8, ty + AF_LH / 2 + 3, x + hw - 8, ty + AF_LH / 2 + 3, "sep");
            for (int k = -1; k <= 1; k++) {
                buf_lit(b, "<circle class=\"dot\" cx=\""); buf_num(b, x + 10 * k); buf_lit(b, "\" cy=\"");
                buf_num(b, ty + AF_LH / 2 + 13); buf_lit(b, "\" r=\"2.5\"/>");
            }
            continue;
        }
        shape(b, g, v, dg_redux(kind(g, v)));
        double cy = g->nshape[v] == SH_DOC ? y - g->nh[v] / 12 : y;
        bool wrap = flow_text_style_open(b, g, g->nclass[v], g->nstyle[v]);
        dg_text(b, g->nlabel[v], AF_FS, x, cy, "middle", nullptr);
        if (wrap) buf_lit(b, "</g>");
    }
    buf_lit(b, "</svg>\n");
    return b->oom ? MMD_ERR_NOMEM : MMD_OK;
}
