#include "dgraph.h"
#include "rich.h"
#include "dcfg.h"

#include <math.h>
#include <stdlib.h>

#include "text.h"

void dg_init(Graph g[static 1], Arena A[static 1], Dir dir, double fs) {
    *g = (Graph){.arena = A, .dir = dir, .node_sep = 50, .rank_sep = 50, .font_size = fs};
}

int32_t dg_node(Graph g[static 1], Str id, bool *fresh) {
    bool f;
    int32_t v = graph_intern(g, id, &f);
    if (f) g->nflags[v] = NF_DEFINED;
    if (fresh) *fresh = f;
    return v;
}

int32_t dg_edge(Graph g[static 1], int32_t a, int32_t b, Str label) {
    if (!graph_edge_room(g)) return NONE;
    size_t e = g->ne++;
    g->efrom[e] = a, g->eto[e] = b;
    g->elabel[e] = label;
    g->estroke[e] = ST_NORMAL;
    g->estart[e] = AH_NONE;
    g->eend[e] = AH_ARROW;
    g->elen[e] = 1;
    g->eid[e] = (Str){};
    g->eflags[e] = 0;
    return (int32_t)e;
}

int32_t dg_cluster(Graph g[static 1], Str id, Str title, int32_t parent) {
    if (!graph_sg_room(g)) return NONE;
    size_t s = g->ns++;
    g->sid[s] = id, g->stitle[s] = title;
    g->sclass[s] = g->sstyle[s] = (Str){};
    g->sparent[s] = parent;
    g->sdir[s] = g->dir;
    g->sflags[s] = 0;
    return (int32_t)s;
}

BBox dg_bbox(const Graph g[static 1]) {
    BBox bb = BB_EMPTY;
    for (size_t v = 0; v < g->nn; v++) {
        if (g->nflags[v] & NF_HIDDEN) continue;
        bb_add(&bb, g->nx[v] - g->nw[v] / 2, g->ny[v] - g->nh[v] / 2, g->nx[v] + g->nw[v] / 2, g->ny[v] + g->nh[v] / 2);
    }
    for (size_t e = 0; e < g->ne; e++) { /* only points an edge still references (self-loop helpers leave stale ones) */
        const double *P = g->pts.v + g->eptoff[e];
        for (uint32_t i = 0; i < g->eptn[e]; i++) bb_add(&bb, P[2 * i], P[2 * i + 1], P[2 * i], P[2 * i + 1]);
    }
    for (size_t e = 0; e < g->ne; e++) {
        if (g->efrom[e] == g->eto[e] && g->efrom[e] != NONE && !(g->eflags && (g->eflags[e] & EF_LOOPPTS))) {
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

void dg_arrowhead(Buf b[restrict static 1], const char cls[restrict static 1], const double tip[restrict static 2],
                  const double from[restrict static 2], double half) {
    double dx = tip[0] - from[0], dy = tip[1] - from[1], L = sqrt(dx * dx + dy * dy);
    if (L < 1e-9) return;
    dx /= L, dy /= L;
    buf_lit(b, "<path class=\""); buf_cstr(b, cls); buf_lit(b, "\" d=\"M"); buf_pt(b, tip[0], tip[1]); buf_ch(b, 'L');
    buf_pt(b, tip[0] - dx * 10 - dy * half, tip[1] - dy * 10 + dx * half); buf_ch(b, 'L');
    buf_pt(b, tip[0] - dx * 10 + dy * half, tip[1] - dy * 10 - dx * half); buf_lit(b, "z\"/>");
}

void dg_basis(Buf b[restrict static 1], size_t n, const double P[restrict static 2 * n]) {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    unsigned st = 0;
    for (size_t i = 0; i < n; i++) {
        double x = P[2 * i], y = P[2 * i + 1];
        switch (st) {
        case 0: st = 1; buf_ch(b, 'M'); buf_pt(b, x, y); break;
        case 1: st = 2; break;
        case 2:
            st = 3;
            buf_ch(b, 'L'); buf_pt(b, (5 * x0 + x1) / 6, (5 * y0 + y1) / 6);
            [[fallthrough]];
        default:
            buf_ch(b, 'C'); buf_pt(b, (2 * x0 + x1) / 3, (2 * y0 + y1) / 3);
            buf_ch(b, ' '); buf_pt(b, (x0 + 2 * x1) / 3, (y0 + 2 * y1) / 3);
            buf_ch(b, ' '); buf_pt(b, (x0 + 4 * x1 + x) / 6, (y0 + 4 * y1 + y) / 6);
            break;
        }
        x0 = x1, x1 = x, y0 = y1, y1 = y;
    }
    switch (st) {
    case 3:
        buf_ch(b, 'C'); buf_pt(b, (2 * x0 + x1) / 3, (2 * y0 + y1) / 3);
        buf_ch(b, ' '); buf_pt(b, (x0 + 2 * x1) / 3, (y0 + 2 * y1) / 3);
        buf_ch(b, ' '); buf_pt(b, (x0 + 5 * x1) / 6, (y0 + 5 * y1) / 6);
        [[fallthrough]];
    case 2: buf_ch(b, 'L'); buf_pt(b, x1, y1); break;
    default: break;
    }
}

void dg_edge_d(Buf b[restrict static 1], const Graph g[restrict static 1], size_t e) {
    if (g->efrom[e] == g->eto[e] && g->efrom[e] != NONE && !(g->eflags && (g->eflags[e] & EF_LOOPPTS) && g->eptn[e] >= 2)) {
        int32_t v = g->efrom[e];
        double x = g->nx[v] + g->nw[v] / 2, y = g->ny[v], q = g->nh[v] / 4;
        buf_ch(b, 'M'); buf_pt(b, x, y - q);
        buf_ch(b, 'C'); buf_pt(b, x + 40, y - q - 20); buf_ch(b, ' '); buf_pt(b, x + 40, y + q + 20);
        buf_ch(b, ' '); buf_pt(b, x, y + q);
        return;
    }
    if (g->eptn[e] >= 2) dg_basis(b, g->eptn[e], g->pts.v + g->eptoff[e]);
}

void dg_text(Buf b[restrict static 1], Str s, double fs, double x, double cy, const char anchor[restrict static 1],
             const char *restrict cls) {
    dg_text_lh(b, s, fs * 1.5, x, cy, anchor, cls);
}

void dg_text_lh(Buf b[restrict static 1], Str s, double lh, double x, double cy, const char anchor[restrict static 1],
                const char *restrict cls) {
    unsigned k = 0;
    size_t pos = 0;
    Str ln;
    if (!s.n) return;
    while (label_next_line(s, &pos, &ln)) k++;
    double y = cy - (k - 1) * lh / 2;
    pos = 0;
    while (label_next_line(s, &pos, &ln)) {
        buf_lit(b, "<text");
        if (cls) { buf_lit(b, " class=\""); buf_cstr(b, cls); buf_ch(b, '"'); }
        buf_lit(b, " x=\""); buf_num(b, x); buf_lit(b, "\" y=\""); buf_num(b, y);
        buf_lit(b, "\" text-anchor=\""); buf_cstr(b, anchor);
        buf_lit(b, "\" dominant-baseline=\"central\">");
        if (rich_needed(ln)) { bool bo = false, it = false; rich_emit_line(b, ln, &bo, &it); }
        else buf_xml(b, ln);
        buf_lit(b, "</text>");
        y += lh;
    }
}

void dg_head(Buf b[restrict static 1], BBox bb, double m, const char kind[restrict static 1], size_t ncss,
             const char css[restrict static ncss]) {
    double x = bb.x0 - m, y = bb.y0 - m, w = bb.x1 - bb.x0 + 2 * m, h = bb.y1 - bb.y0 + 2 * m;
    size_t kl = strlen(kind);
    buf_lit(b, "<svg xmlns=\"http://www.w3.org/2000/svg\" id=\"mermaid\" class=\""); buf_put(b, kl, kind);
    buf_lit(b, "\" width=\""); buf_num(b, w); buf_lit(b, "\" height=\""); buf_num(b, h);
    buf_lit(b, "\" viewBox=\""); buf_num(b, x); buf_ch(b, ' '); buf_num(b, y); buf_ch(b, ' ');
    buf_num(b, w); buf_ch(b, ' '); buf_num(b, h);
    buf_lit(b, "\" role=\"graphics-document document\" aria-roledescription=\""); buf_put(b, kl, kind);
    dcfg_svg_aria(b);
    buf_lit(b, "<style>#mermaid{font-family:\"trebuchet ms\",verdana,arial,sans-serif;font-size:16px;fill:#333;}");
    buf_put(b, ncss, css);
    dcfg_theme_css(b, kind);
    buf_lit(b, "</style><rect x=\""); buf_num(b, x); buf_lit(b, "\" y=\""); buf_num(b, y);
    buf_lit(b, "\" width=\""); buf_num(b, w); buf_lit(b, "\" height=\""); buf_num(b, h); buf_lit(b, "\" fill=\"");
    /* mermaid's xychart paints its own background, xyChart.backgroundColor (the theme's by default) */
    Str bg = dcfg_tv(strcmp(kind, "xychart") ? "background" : "xyChart.backgroundColor");
    if (bg.n) buf_str(b, bg); else buf_lit(b, "white");
    buf_lit(b, "\"/>");
}

void dg_rect(Buf b[restrict static 1], double x, double y, double w, double h, double rx, const char *restrict cls) {
    buf_lit(b, "<rect");
    if (cls) { buf_lit(b, " class=\""); buf_cstr(b, cls); buf_ch(b, '"'); }
    buf_lit(b, " x=\""); buf_num(b, x); buf_lit(b, "\" y=\""); buf_num(b, y);
    buf_lit(b, "\" width=\""); buf_num(b, w); buf_lit(b, "\" height=\""); buf_num(b, h);
    if (rx > 0) { buf_lit(b, "\" rx=\""); buf_num(b, rx); }
    buf_lit(b, "\"/>");
}

void dg_line(Buf b[restrict static 1], double x1, double y1, double x2, double y2, const char *restrict cls) {
    buf_lit(b, "<line");
    if (cls) { buf_lit(b, " class=\""); buf_cstr(b, cls); buf_ch(b, '"'); }
    buf_lit(b, " x1=\""); buf_num(b, x1); buf_lit(b, "\" y1=\""); buf_num(b, y1);
    buf_lit(b, "\" x2=\""); buf_num(b, x2); buf_lit(b, "\" y2=\""); buf_num(b, y2); buf_lit(b, "\"/>");
}

bool dg_next_line(LineIt it[static 1], Str line[static 1], size_t off[static 1]) {
    while (it->i < it->n) {
        const char *nl = memchr(it->s + it->i, '\n', it->n - it->i);
        size_t e = nl ? (size_t)(nl - it->s) : it->n;
        Str raw = {.p = it->s + it->i, .n = e - it->i};
        it->lstart = it->i;
        it->i = nl ? e + 1 : e;
        Str t = str_trim(raw);
        if (str_starts(t, S("%%"))) continue;
        for (size_t k = 1; !it->keep_tail && k + 2 <= t.n; k++) /* trailing ` %% comment`: mermaid lexers drop it */
            if (t.p[k] == '%' && t.p[k + 1] == '%' && is_sp(t.p[k - 1]) && (k + 2 >= t.n || t.p[k + 2] != '{')) {
                t = str_trim((Str){.p = t.p, .n = k});
                break;
            }
        while (!it->keep_tail && t.n && t.p[t.n - 1] == ';' && !ent_tail(t)) t.n--;
        if (!t.n) continue;
        bool blk;
        if (dcfg_is_acc(t, &blk)) { /* handled by dcfg (emitted as <title>/<desc>) */
            if (blk && !memchr(t.p, '}', t.n)) {
                const char *cb = memchr(it->s + it->i, '}', it->n - it->i);
                const char *n2 = cb ? memchr(cb, '\n', it->n - (size_t)(cb - it->s)) : nullptr;
                it->i = n2 ? (size_t)(n2 - it->s) + 1 : it->n;
            }
            continue;
        }
        *line = t;
        *off = (size_t)(t.p - it->s);
        return true;
    }
    return false;
}

unsigned dg_indent(const LineIt it[static 1], size_t off) {
    size_t k = off;
    while (k > 0 && it->s[k - 1] != '\n') k--;
    unsigned c = 0;
    for (; k < off; k++) c += it->s[k] == '\t' ? 4 : 1;
    return c;
}

Str dg_word(Str s[static 1]) {
    *s = str_trim(*s);
    size_t k = 0;
    while (k < s->n && !is_sp(s->p[k])) k++;
    Str w = {.p = s->p, .n = k};
    *s = str_trim((Str){.p = s->p + k, .n = s->n - k});
    return w;
}

static size_t skip_digits(Str s, size_t i) {
    while (i < s.n && (unsigned)(s.p[i] - '0') < 10u) i++;
    return i;
}

/* JS Number() on mermaid's number tokens: [+-]digits[.digits][e[+-]digits], at least one digit.
 * The grammar is checked here; strtod rounds correctly (renders run in the C locale). */
bool dg_parse_num(Str s, double out[static 1]) {
    s = str_trim(s);
    size_t i = s.n && (s.p[0] == '-' || s.p[0] == '+'), m = i;
    i = skip_digits(s, i);
    size_t nd = i - m;
    if (i < s.n && s.p[i] == '.') m = ++i, i = skip_digits(s, i), nd += i - m;
    if (!nd) return false;
    if (i < s.n && (s.p[i] | 32) == 'e') {
        size_t j = i + 1 + (i + 1 < s.n && (s.p[i + 1] == '-' || s.p[i + 1] == '+'));
        if (skip_digits(s, j) > j) i = skip_digits(s, j);
    }
    if (i != s.n) return false;
    char tmp[64];
    AUTOFREE char *big = s.n < sizeof tmp ? nullptr : malloc(s.n + 1);
    char *t = big ? big : tmp;
    if (s.n >= sizeof tmp && !big) return false;
    memcpy(t, s.p, s.n);
    t[s.n] = 0;
    double v = strtod(t, nullptr);
    if (!isfinite(v)) return false;
    *out = v;
    return true;
}

void dg_text_open(Buf b[restrict static 1], double x, double cy, const char anchor[restrict static 1], const char *restrict cls) {
    buf_lit(b, "<text");
    if (cls) { buf_lit(b, " class=\""); buf_cstr(b, cls); buf_ch(b, '"'); }
    buf_lit(b, " x=\""); buf_num(b, x); buf_lit(b, "\" y=\""); buf_num(b, cy);
    buf_lit(b, "\" text-anchor=\""); buf_cstr(b, anchor);
    buf_lit(b, "\" dominant-baseline=\"central\">");
}
void dg_rect_fill(Buf b[restrict static 1], double x, double y, double w, double h, double rx, const char *restrict cls,
                  const char *restrict fill) {
    buf_lit(b, "<rect");
    if (cls) { buf_lit(b, " class=\""); buf_cstr(b, cls); buf_ch(b, '"'); }
    buf_lit(b, " x=\""); buf_num(b, x); buf_lit(b, "\" y=\""); buf_num(b, y);
    buf_lit(b, "\" width=\""); buf_num(b, w < 0 ? 0 : w); buf_lit(b, "\" height=\""); buf_num(b, h < 0 ? 0 : h);
    if (rx > 0) { buf_lit(b, "\" rx=\""); buf_num(b, rx); }
    if (fill) { buf_lit(b, "\" fill=\""); buf_cstr(b, fill); }
    buf_lit(b, "\"/>");
}
const char *dg_palette(size_t i) {
    static const char *const P[] = {"#ECECFF", "#ffffde", "#f5ffe0", "#b8b8ff", "#ffffab", "#e4ffb8",
                                    "#ffb8ff", "#b8ffff", "#ffe0e0", "#ff85ff", "#85ffff", "#ffb8b8"};
    return P[i % countof(P)];
}
Str dg_wrap(Arena a[static 1], Str raw, double fs, double maxw) { return dg_wrapf(a, raw, fs, maxw, text_width); }

Str dg_wrapf(Arena a[static 1], Str raw, double fs, double maxw, double (*meas)(Str, double)) {
    bool ok = true;
    Str s = label_normalize(a, raw, &ok);
    if (!ok) return (Str){};
    double w;
    unsigned nl;
    label_measure_f(s, fs, meas, &w, &nl);
    if (w <= maxw) return s;
    char *o = arena_alloc(a, s.n + 1, 1);
    if (!o) return (Str){};
    memcpy(o, s.p, s.n);
    size_t ls = 0, lastsp = SIZE_MAX;
    for (size_t i = 0; i < s.n; i++) {
        if (o[i] == '\n') { ls = i + 1, lastsp = SIZE_MAX; continue; }
        if (o[i] == ' ') {
            if (lastsp != SIZE_MAX && meas((Str){.p = o + ls, .n = i - ls}, fs) > maxw) {
                o[lastsp] = '\n', ls = lastsp + 1;
            }
            lastsp = i;
        }
    }
    if (lastsp != SIZE_MAX && lastsp >= ls && meas((Str){.p = o + ls, .n = s.n - ls}, fs) > maxw) o[lastsp] = '\n';
    return (Str){.p = o, .n = s.n};
}

/* ---- free-form renderer helpers ---- */
static unsigned nlines(Str s) {
    unsigned k = 0;
    size_t pos = 0;
    Str ln;
    while (label_next_line(s, &pos, &ln)) k++;
    return k;
}

BBox dg_tbox(Str s, double fs, double lh, double x, double y, bool centre, const char anchor[static 1], TBase base) {
    unsigned k = nlines(s);
    double w = 0;
    size_t pos = 0;
    Str ln;
    while (label_next_line(s, &pos, &ln)) w = fmax(w, text_width(ln, fs));
    if (centre) y -= (k - 1) * lh / 2;
    double top = base == TB_MIDDLE ? y - 0.55 * fs : base == TB_HANG ? y - 0.1 * fs : y - 0.92 * fs;
    double x0 = anchor[0] == 'm' ? x - w / 2 : anchor[0] == 'e' ? x - w : x;
    return (BBox){x0, top, x0 + w, top + (k - 1) * lh + 1.15 * fs};
}

unsigned dg_tspans(Buf b[restrict static 1], BBox *restrict bb, Str s, double fs, double lh, double x, double y, bool centre,
                   const char anchor[restrict static 1], TBase base, const char *restrict cls) {
    unsigned k = nlines(s);
    if (bb) {
        BBox t = dg_tbox(s, fs, lh, x, y, centre, anchor, base);
        bb_add(bb, t.x0, t.y0, t.x1, t.y1);
    }
    if (centre) y -= (k - 1) * lh / 2;
    buf_lit(b, "<text");
    if (cls) { buf_lit(b, " class=\""); buf_cstr(b, cls); buf_ch(b, '"'); }
    buf_lit(b, " x=\""); buf_num(b, x); buf_lit(b, "\" y=\""); buf_num(b, y);
    buf_lit(b, "\" text-anchor=\""); buf_cstr(b, anchor); buf_ch(b, '"');
    if (base == TB_MIDDLE) buf_lit(b, " dominant-baseline=\"central\"");
    else if (base == TB_HANG) buf_lit(b, " dominant-baseline=\"hanging\"");
    buf_ch(b, '>');
    size_t pos = 0;
    Str ln;
    for (unsigned i = 0; label_next_line(s, &pos, &ln); i++) {
        buf_lit(b, "<tspan x=\""); buf_num(b, x); buf_lit(b, "\" dy=\""); buf_num(b, i ? lh : 0); buf_lit(b, "\">");
        buf_xml(b, ln);
        buf_lit(b, "</tspan>");
    }
    buf_lit(b, "</text>");
    return k;
}

Str dg_wrapc(Arena a[static 1], Str s, size_t maxc) {
    if (s.n <= maxc) return s;
    char *o = arena_alloc(a, s.n + 1, 1);
    if (!o) return (Str){};
    size_t n = 0, cur = 0;
    for (size_t i = 0; i < s.n;) {
        while (i < s.n && (s.p[i] == ' ' || s.p[i] == '\t' || s.p[i] == '\n')) i++;
        size_t j = i;
        while (j < s.n && s.p[j] != ' ' && s.p[j] != '\t' && s.p[j] != '\n') j++;
        if (j == i) break;
        if (n && cur + 1 + (j - i) <= maxc) o[n++] = ' ', cur++;
        else if (n) o[n++] = '\n', cur = 0;
        memcpy(o + n, s.p + i, j - i);
        n += j - i, cur += j - i;
        i = j;
    }
    return (Str){.p = o, .n = n};
}

mmd_status dg_finish(Buf out[restrict static 1], Buf body[restrict static 1], BBox bb, double m, const char kind[restrict static 1],
                     size_t ncss, const char css[restrict static ncss]) {
    if (bb.x0 > bb.x1) bb = (BBox){0, 0, 100, 100};
    if (!body->oom && buf_reserve(out, body->n + ncss + 1024)) {
        dg_head(out, bb, m, kind, ncss, css);
        buf_put(out, body->n, body->p ? body->p : "");
        buf_lit(out, "</svg>\n");
    }
    bool oom = body->oom || out->oom;
    free(body->p);
    *body = (Buf){};
    return oom ? MMD_ERR_NOMEM : MMD_OK;
}

void dg_css_put(Buf b[static 1], Str css) {
    for (size_t k = 0; k < css.n; k++) {
        char ch = css.p[k] == ',' ? ';' : css.p[k];
        if (ch != '"' && ch != '<' && ch != '&') buf_ch(b, ch);
    }
}

/* ---- mermaid 12 neo look ---- */
NeoColor dg_redux(size_t i) {
    static const NeoColor P[] = { /* redux-color borderColorArray, with the fills its slots pair them with */
        {"#E879F9", "#FDF4FF"}, {"#2DD4BF", "#F0FDFA"}, {"#FB923C", "#FFF7ED"}, {"#22D3EE", "#ECFEFF"},
        {"#4ADE80", "#F0FDF4"}, {"#A78BFA", "#F5F3FF"}, {"#F87171", "#FEF2F2"}, {"#FACC15", "#FEFCE8"},
        {"#818CF8", "#EEF2FF"}, {"#A3E635", "#F7FEE7"}, {"#38BDF8", "#F0F9FF"}, {"#FB7185", "#FFF1F2"},
    };
    return P[i % countof(P)];
}

void dg_neo_defs(Buf b[static 1]) {
    buf_lit(b, "<defs><filter id=\"neo-shadow\" height=\"130%\" width=\"130%\"><feDropShadow dx=\"4\" dy=\"4\" "
               "stdDeviation=\"0\" flood-opacity=\"0.06\" flood-color=\"#000000\"/></filter>");
    static const struct { const char *id, *vb, *ref, *size, *shape; } M[] = {
        {"neo-point-end", "0 0 11.5 14", "refX=\"11.5\" refY=\"7\"", "10.5\" markerHeight=\"14", "<path d=\"M0,0L11.5,7L0,14z\" class=\"mk\"/>"},
        {"neo-point-start", "0 0 11.5 14", "refX=\"0\" refY=\"7\"", "10.5\" markerHeight=\"14", "<path d=\"M11.5,0L0,7L11.5,14z\" class=\"mk\"/>"},
        {"neo-circle-end", "0 0 10 10", "refX=\"10\" refY=\"5\"", "11\" markerHeight=\"11", "<circle cx=\"5\" cy=\"5\" r=\"5\" class=\"mk\"/>"},
        {"neo-circle-start", "0 0 10 10", "refX=\"0\" refY=\"5\"", "11\" markerHeight=\"11", "<circle cx=\"5\" cy=\"5\" r=\"5\" class=\"mk\"/>"},
        {"neo-cross-end", "0 0 11 11", "refX=\"12\" refY=\"5.5\"", "11\" markerHeight=\"11", "<path d=\"M1,1L10,10M10,1L1,10\" class=\"mkx\"/>"},
        {"neo-cross-start", "0 0 11 11", "refX=\"-1\" refY=\"5.5\"", "11\" markerHeight=\"11", "<path d=\"M1,1L10,10M10,1L1,10\" class=\"mkx\"/>"},
        {"neo-ext-end", "0 0 20 16", "refX=\"19\" refY=\"8\"", "20\" markerHeight=\"16", "<path d=\"M1,1L19,8L1,15z\" class=\"mke\"/>"},
    };
    for (size_t i = 0; i < countof(M); i++) {
        buf_lit(b, "<marker id=\""); buf_cstr(b, M[i].id); buf_lit(b, "\" viewBox=\""); buf_cstr(b, M[i].vb);
        buf_lit(b, "\" "); buf_cstr(b, M[i].ref);
        buf_lit(b, " markerUnits=\"userSpaceOnUse\" markerWidth=\""); buf_cstr(b, M[i].size);
        buf_lit(b, "\" orient=\"auto\">"); buf_cstr(b, M[i].shape); buf_lit(b, "</marker>");
    }
    buf_lit(b, "</defs>");
}

const char *dg_neo_marker(Arrow a, bool end) {
    switch (a) {
    case AH_ARROW: return end ? "neo-point-end" : "neo-point-start";
    case AH_CIRCLE: return end ? "neo-circle-end" : "neo-circle-start";
    case AH_CROSS: return end ? "neo-cross-end" : "neo-cross-start";
    default: return nullptr;
    }
}

/* the side of v's box facing (x,y): its centre, or the layout's own end point p for a cluster */
typedef struct { double x, y, w, h; } Box; /* centre and size */

/* a node's box; an edge end naming a subgraph (NF_SUBREF) uses the cluster's */
static Box nbox(const Graph g[static 1], int32_t v) {
    int32_t s = g->nflags[v] & NF_SUBREF ? graph_find_sg(g, g->nid[v]) : NONE;
    if (s != NONE) return (Box){g->sx[s] + g->sw[s] / 2, g->sy[s] + g->sh[s] / 2, g->sw[s], g->sh[s]};
    return (Box){g->nx[v], g->ny[v], g->nw[v], g->nh[v]};
}

/* the point of box n's face towards (x, y), on the rank axis (vert) or across it, off its centre by
 * `off`; a diamond's face is its corner, so an offset slides down the slanted side */
static void face(Box n, bool diamond, bool vert, double x, double y, double off, double out[static 2]) {
    out[0] = n.x, out[1] = n.y;
    double sg = vert ? (y > n.y ? 1 : -1) : (x > n.x ? 1 : -1);
    double in = diamond ? fabs(off) * (vert ? n.h / n.w : n.w / n.h) : 0;
    if (vert) out[1] += sg * (n.h / 2 - in), out[0] += off;
    else out[0] += sg * (n.w / 2 - in), out[1] += off;
}

/* both ends' boxes and whether the edge runs along the rank axis */
static bool ends(const Graph g[static 1], size_t e, Box *A, Box *B) {
    *A = nbox(g, g->efrom[e]), *B = nbox(g, g->eto[e]);
    bool vert = g->dir == DIR_TB || g->dir == DIR_BT;
    /* an edge across the rank axis (same rank, or a cluster laid out the other way) bends the other way */
    if (vert) return fabs(B->y - A->y) >= (A->h + B->h) / 2;
    return fabs(B->x - A->x) < (A->w + B->w) / 2;
}

typedef struct { int32_t v; int8_t side; double at; uint32_t slot; } PortEnd; /* slot = 2e + (to end) */

static int port_cmp(const void *x, const void *y) {
    const PortEnd *a = x, *b = y;
    if (a->v != b->v) return a->v < b->v ? -1 : 1;
    if (a->side != b->side) return a->side < b->side ? -1 : 1;
    if (a->at != b->at) return a->at < b->at ? -1 : 1;
    return a->slot < b->slot ? -1 : a->slot > b->slot;
}

double *dg_ports(const Graph g[static 1]) {
    size_t E = g->ne, m = 0;
    double *off = ANEW(g->arena, double, 2 * E + 1);
    PortEnd *pe = ANEW(g->arena, PortEnd, 2 * E + 1);
    if (!off || !pe) return nullptr;
    for (size_t e = 0; e < E; e++) {
        off[2 * e] = off[2 * e + 1] = 0;
        int32_t a = g->efrom[e], t = g->eto[e];
        if (a == NONE || t == NONE || a == t || g->estroke[e] == ST_INVISIBLE) continue;
        Box A, B;
        bool vert = ends(g, e, &A, &B);
        /* side: which face (sign on the bend axis); at: where the far end lies across it */
        pe[m++] = (PortEnd){a, (int8_t)(vert ? (B.y > A.y) : (B.x > A.x)), vert ? B.x : B.y, (uint32_t)(2 * e)};
        pe[m++] = (PortEnd){t, (int8_t)(vert ? (A.y > B.y) : (A.x > B.x)), vert ? A.x : A.y, (uint32_t)(2 * e + 1)};
        pe[m - 2].side += vert ? 0 : 2, pe[m - 1].side += vert ? 0 : 2;
    }
    qsort(pe, m, sizeof *pe, port_cmp);
    for (size_t i = 0, j; i < m; i = j) {
        for (j = i + 1; j < m && pe[j].v == pe[i].v && pe[j].side == pe[i].side; j++) {}
        size_t k = j - i;
        if (k < 2) continue;
        Box n = nbox(g, pe[i].v);
        double len = pe[i].side < 2 ? n.w : n.h, step = fmin(20, len / (double)(k + 1));
        for (size_t q = i; q < j; q++) off[pe[q].slot] = ((double)(q - i) - (double)(k - 1) / 2) * step;
    }
    return off;
}

/* does the 3-segment route S -> mid -> E (bending at the main-axis midpoint) avoid every other node? */
static bool z_clear(const Graph g[static 1], int32_t a, int32_t t, bool vert, const double S[static 2], const double E[static 2]) {
    int k = vert ? 1 : 0, o = !k;
    double m = (S[k] + E[k]) / 2, seg[3][4] = {{S[k], m, S[o], S[o]}, {m, m, S[o], E[o]}, {m, E[k], E[o], E[o]}};
    for (size_t v = 0; v < g->nn; v++) {
        if ((int32_t)v == a || (int32_t)v == t || (g->nflags[v] & (NF_HIDDEN | NF_SUBREF | NF_PROXY))) continue;
        double c[2] = {g->nx[v], g->ny[v]}, hd[2] = {g->nw[v] / 2, g->nh[v] / 2};
        for (size_t i = 0; i < 3; i++) /* segment i: [seg0, seg1] on the main axis, [seg2, seg3] across */
            if (fmax(seg[i][0], seg[i][1]) > c[k] - hd[k] && fmin(seg[i][0], seg[i][1]) < c[k] + hd[k] &&
                fmax(seg[i][2], seg[i][3]) > c[o] - hd[o] && fmin(seg[i][2], seg[i][3]) < c[o] + hd[o]) return false;
    }
    return true;
}

bool dg_ortho_d(Buf b[restrict static 1], const Graph g[restrict static 1], size_t e, const double *ports,
                double mid[restrict static 2]) {
    uint32_t n = g->eptn[e];
    int32_t a = g->efrom[e], t = g->eto[e];
    if (n < 2 || a == t) return false;
    const double *P = g->pts.v + g->eptoff[e];
    Box A, B;
    bool vert = ends(g, e, &A, &B);
    double S[2], E[2];
    bool da = !(g->nflags[a] & NF_SUBREF) && g->nshape[a] == SH_RHOMBUS, dt = !(g->nflags[t] & NF_SUBREF) && g->nshape[t] == SH_RHOMBUS;
    face(A, da, vert, B.x, B.y, ports ? ports[2 * e] : 0, S);
    face(B, dt, vert, A.x, A.y, ports ? ports[2 * e + 1] : 0, E);
    double *Q = ANEW(g->arena, double, 6 * n + 8);
    if (!Q) return false;
    size_t m = 0;
    Q[m++] = S[0], Q[m++] = S[1];
    int k = vert ? 1 : 0; /* main coordinate */
    double lo = fmin(S[k], E[k]) + 1, hi = fmax(S[k], E[k]) - 1;
    bool direct = z_clear(g, a, t, vert, S, E);
    for (uint32_t i = 0; i <= n; i++) { /* the layout's inner bends (long edges) unless a Z clears, then E */
        const double *T = i < n ? P + 2 * i : E;
        if (i < n && (direct || i == 0 || i == n - 1 || T[k] <= lo || T[k] >= hi)) continue;
        double cx = Q[m - 2], cy = Q[m - 1];
        if (fabs(vert ? T[0] - cx : T[1] - cy) > 0.5) {
            double mm = ((vert ? cy : cx) + T[k]) / 2;
            if (vert) { Q[m++] = cx, Q[m++] = mm, Q[m++] = T[0], Q[m++] = mm; }
            else { Q[m++] = mm, Q[m++] = cy, Q[m++] = mm, Q[m++] = T[1]; }
        }
        Q[m++] = T[0], Q[m++] = T[1];
    }
    size_t np = m / 2;
    double len = 0;
    for (size_t i = 1; i < np; i++) len += hypot(Q[2 * i] - Q[2 * i - 2], Q[2 * i + 1] - Q[2 * i - 1]);
    double half = len / 2;
    mid[0] = Q[0], mid[1] = Q[1];
    for (size_t i = 1; i < np; i++) {
        double l = hypot(Q[2 * i] - Q[2 * i - 2], Q[2 * i + 1] - Q[2 * i - 1]);
        if (l >= half && l > 0) {
            mid[0] = Q[2 * i - 2] + (Q[2 * i] - Q[2 * i - 2]) * half / l;
            mid[1] = Q[2 * i - 1] + (Q[2 * i + 1] - Q[2 * i - 1]) * half / l;
            break;
        }
        half -= l;
    }
    buf_ch(b, 'M'); buf_pt(b, Q[0], Q[1]);
    for (size_t i = 1; i + 1 < np; i++) { /* rounded corners (mermaid neo: radius 5) */
        const double *A = Q + 2 * i - 2, *C = Q + 2 * i, *D = Q + 2 * i + 2;
        double l1 = hypot(C[0] - A[0], C[1] - A[1]), l2 = hypot(D[0] - C[0], D[1] - C[1]);
        double r = fmin(5, fmin(l1, l2) / 2);
        if (r < 0.01) { buf_ch(b, 'L'); buf_pt(b, C[0], C[1]); continue; }
        buf_ch(b, 'L'); buf_pt(b, C[0] + (A[0] - C[0]) * r / l1, C[1] + (A[1] - C[1]) * r / l1);
        buf_ch(b, 'Q'); buf_pt(b, C[0], C[1]); buf_ch(b, ' ');
        buf_pt(b, C[0] + (D[0] - C[0]) * r / l2, C[1] + (D[1] - C[1]) * r / l2);
    }
    buf_ch(b, 'L'); buf_pt(b, Q[2 * np - 2], Q[2 * np - 1]);
    return true;
}
