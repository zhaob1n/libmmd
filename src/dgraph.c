#include "dgraph.h"

#include <math.h>

#include "text.h"

void dg_init(Graph *g, Arena *A, Dir dir, double fs) {
    *g = (Graph){.arena = A, .dir = dir, .node_sep = 50, .rank_sep = 50, .font_size = fs};
}

static inline uint64_t hstr(Str s) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < s.n; i++) h = (h ^ (unsigned char)s.p[i]) * 0x100000001b3ull;
    return h ^ (h >> 31);
}

#define GROW(A, arr, oldc, newc)                                                                  \
    do {                                                                                          \
        typeof(arr) na_ = arena_alloc_array((A), (newc), sizeof *(arr), alignof(typeof(*(arr)))); \
        if (!na_) return false;                                                                   \
        if ((oldc)) memcpy(na_, (arr), (oldc) * sizeof *(arr));                                   \
        (arr) = na_;                                                                              \
    } while (0)

[[nodiscard]] static bool node_grow(Graph *g) {
    size_t oc = g->ncap, nc = oc ? oc * 2 : 32;
    Arena *A = g->arena;
    GROW(A, g->nid, oc, nc); GROW(A, g->nlabel, oc, nc); GROW(A, g->nclass, oc, nc);
    GROW(A, g->nstyle, oc, nc); GROW(A, g->nshape, oc, nc); GROW(A, g->nparent, oc, nc);
    GROW(A, g->nflags, oc, nc); GROW(A, g->nw, oc, nc); GROW(A, g->nh, oc, nc);
    g->ncap = nc;
    return true;
}
[[nodiscard]] static bool edge_grow(Graph *g) {
    size_t oc = g->ecap, nc = oc ? oc * 2 : 32;
    Arena *A = g->arena;
    GROW(A, g->efrom, oc, nc); GROW(A, g->eto, oc, nc); GROW(A, g->elabel, oc, nc);
    GROW(A, g->estroke, oc, nc); GROW(A, g->estart, oc, nc); GROW(A, g->eend, oc, nc);
    GROW(A, g->elen, oc, nc);
    g->ecap = nc;
    return true;
}
[[nodiscard]] static bool sg_grow(Graph *g) {
    size_t oc = g->scap, nc = oc ? oc * 2 : 16;
    Arena *A = g->arena;
    GROW(A, g->sid, oc, nc); GROW(A, g->stitle, oc, nc); GROW(A, g->sclass, oc, nc);
    GROW(A, g->sstyle, oc, nc); GROW(A, g->sparent, oc, nc); GROW(A, g->sdir, oc, nc);
    g->scap = nc;
    return true;
}
[[nodiscard]] static bool rehash(Graph *g) {
    size_t nc = g->hcap ? g->hcap * 2 : 64;
    uint32_t *t = ANEW(g->arena, uint32_t, nc);
    if (!t) return false;
    memset(t, 0, nc * sizeof *t);
    for (size_t v = 0; v < g->nn; v++) {
        size_t h = hstr(g->nid[v]) & (nc - 1);
        while (t[h]) h = (h + 1) & (nc - 1);
        t[h] = (uint32_t)v + 1;
    }
    g->htab = t;
    g->hcap = nc;
    return true;
}

int32_t dg_find(const Graph *g, Str id) {
    if (!g->hcap) return NONE;
    for (size_t h = hstr(id) & (g->hcap - 1);; h = (h + 1) & (g->hcap - 1)) {
        uint32_t e = g->htab[h];
        if (!e) return NONE;
        if (str_eq(g->nid[e - 1], id)) return (int32_t)e - 1;
    }
}

int32_t dg_node(Graph *g, Str id, bool *fresh) {
    if (fresh) *fresh = false;
    int32_t f = dg_find(g, id);
    if (f != NONE) return f;
    if (g->nn >= INT32_MAX / 2) return NONE;
    if (g->nn == g->ncap && !node_grow(g)) return NONE;
    if ((g->nn + 1) * 2 > g->hcap && !rehash(g)) return NONE;
    size_t v = g->nn++;
    g->nid[v] = id;
    g->nlabel[v] = id;
    g->nclass[v] = g->nstyle[v] = (Str){};
    g->nshape[v] = SH_RECT;
    g->nparent[v] = NONE;
    g->nflags[v] = NF_DEFINED;
    g->nw[v] = g->nh[v] = 0;
    size_t h = hstr(id) & (g->hcap - 1);
    while (g->htab[h]) h = (h + 1) & (g->hcap - 1);
    g->htab[h] = (uint32_t)v + 1;
    if (fresh) *fresh = true;
    return (int32_t)v;
}

int32_t dg_edge(Graph *g, int32_t a, int32_t b, Str label) {
    if (g->ne >= INT32_MAX / 2) return NONE;
    if (g->ne == g->ecap && !edge_grow(g)) return NONE;
    size_t e = g->ne++;
    g->efrom[e] = a, g->eto[e] = b;
    g->elabel[e] = label;
    g->estroke[e] = ST_NORMAL;
    g->estart[e] = AH_NONE;
    g->eend[e] = AH_ARROW;
    g->elen[e] = 1;
    return (int32_t)e;
}

int32_t dg_cluster(Graph *g, Str id, Str title, int32_t parent) {
    if (g->ns >= 1u << 20) return NONE;
    if (g->ns == g->scap && !sg_grow(g)) return NONE;
    size_t s = g->ns++;
    g->sid[s] = id, g->stitle[s] = title;
    g->sclass[s] = g->sstyle[s] = (Str){};
    g->sparent[s] = parent;
    g->sdir[s] = g->dir;
    return (int32_t)s;
}

BBox dg_bbox(const Graph *g) {
    BBox bb = BB_EMPTY;
    for (size_t v = 0; v < g->nn; v++) {
        if (g->nflags[v] & NF_HIDDEN) continue;
        bb_add(&bb, g->nx[v] - g->nw[v] / 2, g->ny[v] - g->nh[v] / 2, g->nx[v] + g->nw[v] / 2, g->ny[v] + g->nh[v] / 2);
    }
    for (size_t i = 0; i + 1 < g->pts.n; i += 2) bb_add(&bb, g->pts.v[i], g->pts.v[i + 1], g->pts.v[i], g->pts.v[i + 1]);
    for (size_t e = 0; e < g->ne; e++) {
        if (g->efrom[e] == g->eto[e] && g->efrom[e] != NONE) {
            int32_t v = g->efrom[e];
            bb_add(&bb, g->nx[v], g->ny[v], g->nx[v] + g->nw[v] / 2 + 50 + g->elw[e], g->ny[v]);
        }
        if (g->elw[e] > 0)
            bb_add(&bb, g->elx[e] - g->elw[e] / 2, g->ely[e] - g->elh[e] / 2, g->elx[e] + g->elw[e] / 2, g->ely[e] + g->elh[e] / 2);
    }
    for (size_t s = 0; s < g->ns; s++)
        if (g->sw[s] > 0) bb_add(&bb, g->sx[s], g->sy[s], g->sx[s] + g->sw[s], g->sy[s] + g->sh[s]);
    if (bb.x0 > bb.x1) bb = (BBox){0, 0, 100, 100};
    return bb;
}

void dg_pt(Buf *b, double x, double y) { buf_num(b, x); buf_ch(b, ','); buf_num(b, y); }

void dg_basis(Buf *b, size_t n, const double P[static 2 * n]) {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    unsigned st = 0;
    for (size_t i = 0; i < n; i++) {
        double x = P[2 * i], y = P[2 * i + 1];
        switch (st) {
        case 0: st = 1; buf_ch(b, 'M'); dg_pt(b, x, y); break;
        case 1: st = 2; break;
        case 2:
            st = 3;
            buf_ch(b, 'L'); dg_pt(b, (5 * x0 + x1) / 6, (5 * y0 + y1) / 6);
            [[fallthrough]];
        default:
            buf_ch(b, 'C'); dg_pt(b, (2 * x0 + x1) / 3, (2 * y0 + y1) / 3);
            buf_ch(b, ' '); dg_pt(b, (x0 + 2 * x1) / 3, (y0 + 2 * y1) / 3);
            buf_ch(b, ' '); dg_pt(b, (x0 + 4 * x1 + x) / 6, (y0 + 4 * y1 + y) / 6);
            break;
        }
        x0 = x1, x1 = x, y0 = y1, y1 = y;
    }
    switch (st) {
    case 3:
        buf_ch(b, 'C'); dg_pt(b, (2 * x0 + x1) / 3, (2 * y0 + y1) / 3);
        buf_ch(b, ' '); dg_pt(b, (x0 + 2 * x1) / 3, (y0 + 2 * y1) / 3);
        buf_ch(b, ' '); dg_pt(b, (x0 + 5 * x1) / 6, (y0 + 5 * y1) / 6);
        [[fallthrough]];
    case 2: buf_ch(b, 'L'); dg_pt(b, x1, y1); break;
    default: break;
    }
}

void dg_edge_d(Buf *b, const Graph *g, size_t e) {
    if (g->efrom[e] == g->eto[e] && g->efrom[e] != NONE) {
        int32_t v = g->efrom[e];
        double x = g->nx[v] + g->nw[v] / 2, y = g->ny[v], q = g->nh[v] / 4;
        buf_ch(b, 'M'); dg_pt(b, x, y - q);
        buf_ch(b, 'C'); dg_pt(b, x + 40, y - q - 20); buf_ch(b, ' '); dg_pt(b, x + 40, y + q + 20);
        buf_ch(b, ' '); dg_pt(b, x, y + q);
        return;
    }
    if (g->eptn[e] >= 2) dg_basis(b, g->eptn[e], g->pts.v + g->eptoff[e]);
}

void dg_text(Buf *b, Str s, double fs, double x, double cy, const char *anchor, const char *cls) {
    unsigned k = 0;
    size_t pos = 0;
    Str ln;
    if (!s.n) return;
    while (label_next_line(s, &pos, &ln)) k++;
    double lh = fs * 1.5, y = cy - (k - 1) * lh / 2;
    pos = 0;
    while (label_next_line(s, &pos, &ln)) {
        buf_lit(b, "<text");
        if (cls) { buf_lit(b, " class=\""); buf_put(b, strlen(cls), cls); buf_ch(b, '"'); }
        buf_lit(b, " x=\""); buf_num(b, x); buf_lit(b, "\" y=\""); buf_num(b, y);
        buf_lit(b, "\" text-anchor=\""); buf_put(b, strlen(anchor), anchor);
        buf_lit(b, "\" dominant-baseline=\"central\">");
        buf_xml(b, ln);
        buf_lit(b, "</text>");
        y += lh;
    }
}

void dg_head(Buf *b, BBox bb, double m, const char *kind, size_t ncss, const char css[static ncss]) {
    double x = bb.x0 - m, y = bb.y0 - m, w = bb.x1 - bb.x0 + 2 * m, h = bb.y1 - bb.y0 + 2 * m;
    size_t kl = strlen(kind);
    buf_lit(b, "<svg xmlns=\"http://www.w3.org/2000/svg\" id=\"mermaid\" class=\""); buf_put(b, kl, kind);
    buf_lit(b, "\" width=\""); buf_num(b, w); buf_lit(b, "\" height=\""); buf_num(b, h);
    buf_lit(b, "\" viewBox=\""); buf_num(b, x); buf_ch(b, ' '); buf_num(b, y); buf_ch(b, ' ');
    buf_num(b, w); buf_ch(b, ' '); buf_num(b, h);
    buf_lit(b, "\" role=\"graphics-document document\" aria-roledescription=\""); buf_put(b, kl, kind);
    buf_lit(b, "\"><style>#mermaid{font-family:\"trebuchet ms\",verdana,arial,sans-serif;font-size:16px;fill:#333;}");
    buf_put(b, ncss, css);
    buf_lit(b, "</style><rect x=\""); buf_num(b, x); buf_lit(b, "\" y=\""); buf_num(b, y);
    buf_lit(b, "\" width=\""); buf_num(b, w); buf_lit(b, "\" height=\""); buf_num(b, h); buf_lit(b, "\" fill=\"white\"/>");
}

void dg_rect(Buf *b, double x, double y, double w, double h, double rx, const char *cls) {
    buf_lit(b, "<rect");
    if (cls) { buf_lit(b, " class=\""); buf_put(b, strlen(cls), cls); buf_ch(b, '"'); }
    buf_lit(b, " x=\""); buf_num(b, x); buf_lit(b, "\" y=\""); buf_num(b, y);
    buf_lit(b, "\" width=\""); buf_num(b, w); buf_lit(b, "\" height=\""); buf_num(b, h);
    if (rx > 0) { buf_lit(b, "\" rx=\""); buf_num(b, rx); }
    buf_lit(b, "\"/>");
}

void dg_line(Buf *b, double x1, double y1, double x2, double y2, const char *cls) {
    buf_lit(b, "<line");
    if (cls) { buf_lit(b, " class=\""); buf_put(b, strlen(cls), cls); buf_ch(b, '"'); }
    buf_lit(b, " x1=\""); buf_num(b, x1); buf_lit(b, "\" y1=\""); buf_num(b, y1);
    buf_lit(b, "\" x2=\""); buf_num(b, x2); buf_lit(b, "\" y2=\""); buf_num(b, y2); buf_lit(b, "\"/>");
}

bool dg_next_line(LineIt *it, Str *line, size_t *off) {
    while (it->i < it->n) {
        const char *nl = memchr(it->s + it->i, '\n', it->n - it->i);
        size_t e = nl ? (size_t)(nl - it->s) : it->n;
        Str raw = {.p = it->s + it->i, .n = e - it->i};
        it->lstart = it->i;
        it->i = nl ? e + 1 : e;
        Str t = str_trim(raw);
        while (t.n && t.p[t.n - 1] == ';') t.n--;
        if (!t.n || str_starts(t, S("%%"))) continue;
        *line = t;
        *off = (size_t)(t.p - it->s);
        return true;
    }
    return false;
}

unsigned dg_indent(const LineIt *it, size_t off) {
    size_t k = off;
    while (k > 0 && it->s[k - 1] != '\n') k--;
    unsigned c = 0;
    for (; k < off; k++) c += it->s[k] == '\t' ? 4 : 1;
    return c;
}

Str dg_word(Str *s) {
    *s = str_trim(*s);
    size_t k = 0;
    while (k < s->n && !is_sp(s->p[k])) k++;
    Str w = {.p = s->p, .n = k};
    *s = str_trim((Str){.p = s->p + k, .n = s->n - k});
    return w;
}

bool dg_parse_num(Str s, double *out) {
    s = str_trim(s);
    size_t i = 0;
    bool neg = false, any = false;
    if (i < s.n && (s.p[i] == '-' || s.p[i] == '+')) neg = s.p[i++] == '-';
    double v = 0;
    while (i < s.n && (unsigned)(s.p[i] - '0') < 10u) { v = v * 10 + (s.p[i++] - '0'); any = true; }
    if (i < s.n && s.p[i] == '.') {
        double f = 0.1;
        for (i++; i < s.n && (unsigned)(s.p[i] - '0') < 10u; i++, f /= 10) { v += (s.p[i] - '0') * f; any = true; }
    }
    if (i < s.n && (s.p[i] == 'e' || s.p[i] == 'E')) {
        size_t j = i + 1;
        bool en = false;
        if (j < s.n && (s.p[j] == '-' || s.p[j] == '+')) en = s.p[j++] == '-';
        int ex = 0;
        bool ed = false;
        while (j < s.n && (unsigned)(s.p[j] - '0') < 10u && ex < 400) { ex = ex * 10 + (s.p[j++] - '0'); ed = true; }
        if (ed) { v *= pow(10, en ? -ex : ex); i = j; }
    }
    if (!any || i != s.n || !isfinite(v)) return false;
    *out = neg ? -v : v;
    return true;
}
