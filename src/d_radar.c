/* radar(-beta): axis a["A"], b; curve id["Label"]{1, 2, 3} or {a: 1, b: 2}; options max, min,
 * ticks, graticule circle|polygon, showLegend. Geometry follows mermaid's defaults (600x600 plot,
 * radius 250, 5 ticks, curve tension 0.17, label factor 1.05). Missing values count as min. */
#include <math.h>
#include <stdio.h>

#include "dgraph.h"
#include "dcfg.h"
#include "diagrams.h"
#include "text.h"

typedef struct Axis { Str id, label; } Axis;
/* mermaid 11 default cScale (radarCurve-N) */
static const char *const COL[] = {"#8686ff", "#ffff78", "#d7ff86", "#c286ff", "#ff86ff", "#ff86c2", "#ff8686", "#ffc286", "#c2ff86", "#86ffc2", "#86ffff", "#86c2ff"};
typedef struct Curve { Str id, label; uint32_t v0; } Curve;

static constexpr char CSS[] =
    "#mermaid .gr{fill:#DEDEDE;fill-opacity:.3;stroke:#DEDEDE;stroke-width:1px;}"
    "#mermaid .ax{stroke:#333;stroke-width:2px;}#mermaid .al{font-size:12px;fill:#333;}"
    "#mermaid .cv{fill-opacity:.5;stroke-width:2px;}#mermaid .lg{font-size:12px;fill:#333;}"
    "#mermaid .lb{fill-opacity:.5;}#mermaid .ttl{font-size:16px;fill:#333;font-weight:bold;}";

/* id or id["label"] at the front of s */
static void id_label(Str *s, Str *id, Str *label) {
    *s = str_trim(*s);
    size_t i = 0;
    while (i < s->n && s->p[i] != '[' && s->p[i] != ',' && s->p[i] != '{' && s->p[i] != ' ') i++;
    *id = (Str){.p = s->p, .n = i}, *label = *id;
    Str r = str_trim((Str){.p = s->p + i, .n = s->n - i});
    if (r.n && r.p[0] == '[') {
        const char *e = memchr(r.p, ']', r.n);
        if (e) {
            Str l = str_trim((Str){.p = r.p + 1, .n = (size_t)(e - r.p) - 1});
            if (l.n >= 2 && l.p[0] == '"' && l.p[l.n - 1] == '"') l = (Str){.p = l.p + 1, .n = l.n - 2};
            *label = l;
            r = (Str){.p = e + 1, .n = r.n - (size_t)(e + 1 - r.p)};
        }
    }
    *s = r;
}

DIAG_FN(radar_render) {
    (void)fs;
    LineIt it = {.s = src, .n = n, .i = off};
    Str line;
    size_t lo;
    if (!dg_next_line(&it, &line, &lo)) return diag_fail(err_off, msg, off, "empty radar");
    VEC(Axis) ax = {};
    VEC(Curve) cv = {};
    VEC(double) vals = {};
    double vmax = NAN, vmin = 0;
    int ticks = 5;
    bool poly = false, legend = true;
    VEC(Str) raw = {}; /* curve bodies, resolved once all axes are known */
    while (dg_next_line(&it, &line, &lo)) {
        Str rest = line, w = dg_word(&rest);
        rest = str_trim(rest);
        if (str_eq(w, S("title"))) { title = rest; continue; }
        if (str_starts(w, S("acc"))) continue;
        double d;
        if (str_eq(w, S("max"))) { if (!dg_parse_num(rest, &vmax)) return diag_fail(err_off, msg, lo, "bad max"); continue; }
        if (str_eq(w, S("min"))) { if (!dg_parse_num(rest, &vmin)) return diag_fail(err_off, msg, lo, "bad min"); continue; }
        if (str_eq(w, S("ticks"))) {
            if (!dg_parse_num(rest, &d) || d < 1 || d > 50) return diag_fail(err_off, msg, lo, "bad ticks");
            ticks = (int)d;
            continue;
        }
        if (str_eq(w, S("graticule"))) { poly = str_eq(rest, S("polygon")); continue; }
        if (str_eq(w, S("showLegend"))) { legend = !str_eq(rest, S("false")); continue; }
        if (str_eq(w, S("axis"))) {
            while (rest.n) {
                Axis x;
                id_label(&rest, &x.id, &x.label);
                if (!x.id.n) return diag_fail(err_off, msg, (size_t)(rest.p - src), "expected axis name");
                if (!VEC_PUSH(a, &ax, x)) return CMM_ERR_NOMEM;
                rest = str_trim(rest);
                if (rest.n && rest.p[0] == ',') rest = (Str){.p = rest.p + 1, .n = rest.n - 1};
                else if (rest.n) return diag_fail(err_off, msg, (size_t)(rest.p - src), "expected ','");
            }
            continue;
        }
        if (str_eq(w, S("curve"))) {
            Curve c = {};
            id_label(&rest, &c.id, &c.label);
            /* the label may be a bare multi-word name: everything up to '{' */
            const char *br = memchr(line.p, '{', line.n);
            if (!br || line.p[line.n - 1] != '}') return diag_fail(err_off, msg, lo, "expected {values}");
            if (c.label.p == c.id.p) c.id = c.label = str_trim((Str){.p = w.p + w.n, .n = (size_t)(br - w.p) - w.n});
            if (!c.id.n) return diag_fail(err_off, msg, lo, "curve needs a name");
            if (!VEC_PUSH(a, &cv, c) || !VEC_PUSH(a, &raw, ((Str){.p = br + 1, .n = (size_t)(line.p + line.n - 1 - br - 1)})))
                return CMM_ERR_NOMEM;
            continue;
        }
        return diag_fail(err_off, msg, lo, "unknown radar statement");
    }
    size_t na = ax.n;
    if (na < 1) return diag_fail(err_off, msg, off, "radar needs at least one axis");
    double dmax = -INFINITY;
    for (size_t c = 0; c < cv.n; c++) {
        cv.v[c].v0 = (uint32_t)vals.n;
        for (size_t k = 0; k < na; k++) if (!VEC_PUSH(a, &vals, vmin)) return CMM_ERR_NOMEM;
        Str b = raw.v[c];
        for (size_t k = 0; b.n || k == 0; k++) {
            const char *cm = memchr(b.p, ',', b.n);
            size_t L = cm ? (size_t)(cm - b.p) : b.n;
            Str e = str_trim((Str){.p = b.p, .n = L});
            b = cm ? (Str){.p = cm + 1, .n = b.n - L - 1} : (Str){};
            if (!e.n && !cm && k == 0) break;
            const char *col = memchr(e.p, ':', e.n);
            size_t slot = k;
            if (col) {
                Str key = str_trim((Str){.p = e.p, .n = (size_t)(col - e.p)});
                e = str_trim((Str){.p = col + 1, .n = e.n - (size_t)(col + 1 - e.p)});
                slot = SIZE_MAX;
                for (size_t q = 0; q < na; q++) if (str_eq(ax.v[q].id, key)) slot = q;
            }
            double d;
            if (!dg_parse_num(e, &d) || !isfinite(d)) d = 0; /* mermaid/mmdr: non-numeric -> 0 (centre) */
            if (slot < na) vals.v[cv.v[c].v0 + slot] = d;
            if (d > dmax) dmax = d;
        }
    }
    if (isnan(vmax)) vmax = dmax > vmin ? dmax : vmin + 1;
    if (vmax <= vmin) vmax = vmin + 1;
    constexpr double R = 250;
    constexpr double CX = 350;
    double cy = (title.n ? 40 : 10) + 300, lgw = 0;
    if (legend) for (size_t c = 0; c < cv.n; c++) lgw = fmax(lgw, text_width(cv.v[c].label, 12) + 30);
    BBox bb = {0, 0, 700 + lgw, cy + 310};
    if (!buf_reserve(out, 2048 + na * 300 + cv.n * (na * 60 + 300))) return CMM_ERR_NOMEM;
    DG_HEAD(out, bb, 4, "radar", CSS);
#define ANG(k) (2 * M_PI * (double)(k) / (double)na - M_PI / 2)
    for (int t = ticks; t >= 1; t--) {
        double r = R * t / ticks;
        if (!poly) {
            buf_lit(out, "<circle class=\"gr\" cx=\""); buf_num(out, CX); buf_lit(out, "\" cy=\""); buf_num(out, cy);
            buf_lit(out, "\" r=\""); buf_num(out, r); buf_lit(out, "\"/>");
        } else {
            buf_lit(out, "<path class=\"gr\" d=\"");
            for (size_t k = 0; k < na; k++) { buf_ch(out, k ? 'L' : 'M'); dg_pt(out, CX + r * cos(ANG(k)), cy + r * sin(ANG(k))); }
            buf_lit(out, "Z\"/>");
        }
    }
    for (size_t k = 0; k < na; k++) {
        double c = cos(ANG(k)), s = sin(ANG(k));
        dg_line(out, CX, cy, CX + R * c, cy + R * s, "ax");
        const char *anc = fabs(c) < 0.1 ? "middle" : c > 0 ? "start" : "end";
        dg_text(out, ax.v[k].label, 12, CX + R * 1.05 * c + (fabs(c) < 0.1 ? 0 : c > 0 ? 4 : -4), cy + R * 1.05 * s + s * 6, anc, "al");
    }
    for (size_t c = 0; c < cv.n; c++) {
        const char *col = dcfg_pal("cScale", c % countof(COL), COL[c % countof(COL)]);
        double P[2 * 64];
        size_t m = na < 64 ? na : 64;
        for (size_t k = 0; k < m; k++) {
            double v = vals.v[cv.v[c].v0 + k], r = R * fmin(fmax((v - vmin) / (vmax - vmin), 0), 1);
            P[2 * k] = CX + r * cos(ANG(k)), P[2 * k + 1] = cy + r * sin(ANG(k));
        }
        buf_lit(out, "<path class=\"cv\" fill=\""); buf_put(out, strlen(col), col); buf_lit(out, "\" stroke=\"");
        buf_put(out, strlen(col), col); buf_lit(out, "\" d=\"M");
        dg_pt(out, P[0], P[1]);
        for (size_t k = 0; k < m; k++) { /* closed cardinal spline, tension 0.17 */
            size_t i0 = (k + m - 1) % m, i1 = k, i2 = (k + 1) % m, i3 = (k + 2) % m;
            constexpr double T = (1 - 0.17) / 6; /* d3 curveCardinalClosed.tension(0.17) */
            buf_ch(out, 'C');
            dg_pt(out, P[2 * i1] + (P[2 * i2] - P[2 * i0]) * T, P[2 * i1 + 1] + (P[2 * i2 + 1] - P[2 * i0 + 1]) * T); buf_ch(out, ' ');
            dg_pt(out, P[2 * i2] - (P[2 * i3] - P[2 * i1]) * T, P[2 * i2 + 1] - (P[2 * i3 + 1] - P[2 * i1 + 1]) * T); buf_ch(out, ' ');
            dg_pt(out, P[2 * i2], P[2 * i2 + 1]);
        }
        buf_lit(out, "Z\"/>");
    }
#undef ANG
    if (legend)
        for (size_t c = 0; c < cv.n; c++) {
            double y = cy - R + 20 * (double)c;
            dg_rect_fill(out, CX + R + 50, y - 6, 12, 12, 0, "lb", dcfg_pal("cScale", c % countof(COL), COL[c % countof(COL)]));
            dg_text(out, cv.v[c].label, 12, CX + R + 68, y, "start", "lg");
        }
    if (title.n) dg_text(out, title, 16, CX, 22, "middle", "ttl");
    buf_lit(out, "</svg>\n");
    return out->oom ? CMM_ERR_NOMEM : CMM_OK;
}
