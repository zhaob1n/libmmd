/* xychart(-beta) [horizontal]: title, x-axis ["title"] [a, b, ..] | [title] min --> max,
 * y-axis ["title"] [min --> max], bar [..], line [..] (optional series title before the list).
 *
 * Config (%%{init}%% / front matter, dotted as xyChart.*):
 *   showDataLabel, showDataLabelOutsideBar — mermaid 11. Labels use each bar's own value
 *   (mermaid labels every bar from plots[0]) and the axis formatter, not Number.toString().
 *   Outside labels are 12px: mermaid shrinks them with the inside-fit heuristic, which makes
 *   end labels on a tall horizontal chart unreadably small.
 *   barOverlap — libmmd only. Bar series share the full band instead of sitting side by side,
 *   and a 0 draws neither a bar nor a label, so one series per group (0 elsewhere) colours
 *   bars by group. mermaid.js ignores the key and draws grouped bars; the text still parses. */
#include <math.h>
#include <stdio.h>

#include "dgraph.h"
#include "dcfg.h"
#include "diagrams.h"
#include "text.h"

typedef struct Series { bool line; uint32_t v0, nv; Str name; const char *col; } Series;

static constexpr char CSS[] =
    "#mermaid .ax{stroke:#131300;stroke-width:2px;}#mermaid .tk{stroke:#131300;stroke-width:2px;}"
    "#mermaid .tl{font-size:14px;fill:#131300;}#mermaid .at{font-size:16px;fill:#131300;}"
    "#mermaid .ttl{font-size:20px;fill:#131300;}#mermaid .ln{fill:none;stroke-width:2px;}#mermaid .lg{stroke:#131300;stroke-width:.5px;}";
static constexpr char PAL[][8] = {"#ECECFF", "#8493A6", "#FFC3A0", "#DCDDE1", "#B8E994",
                                  "#D1A36F", "#C3CDE6", "#FFB6C1", "#496078", "#F8F3E3"};
static constexpr char LINEPAL[][8] = {"#8493A6", "#FF7F50", "#3c9a3c", "#7b52ab", "#c0392b"};
static constexpr double DL_FS = 12; /* outside data labels; inside size is fitted per series */

/* optional leading quoted or bare title, then rest */
static Str take_title(Str s[static 1]) {
    Str t = str_trim(*s);
    if (t.n && t.p[0] == '"') {
        const char *q = memchr(t.p + 1, '"', t.n - 1);
        if (q) {
            *s = (Str){.p = q + 1, .n = t.n - (size_t)(q + 1 - t.p)};
            return (Str){.p = t.p + 1, .n = (size_t)(q - t.p - 1)};
        }
    }
    if (t.n && t.p[0] == '[') { *s = t; return (Str){}; }
    /* bare title: up to '[' or a number range */
    size_t i = 0;
    while (i < t.n && t.p[i] != '[') i++;
    Str head = str_trim((Str){.p = t.p, .n = i});
    /* "title min --> max": title is the words before the first numeric word */
    Str h = head, w;
    const char *cut = head.p + head.n;
    while ((w = dg_word(&h)).n) {
        double d;
        if (dg_parse_num(w, &d)) { cut = w.p; break; }
    }
    *s = (Str){.p = cut, .n = t.n - (size_t)(cut - t.p)};
    return str_trim((Str){.p = head.p, .n = (size_t)(cut - head.p)});
}
[[nodiscard]] static bool range(Str s, double lo[static 1], double hi[static 1]) {
    s = str_trim(s);
    for (size_t i = 0; i + 3 <= s.n; i++)
        if (!memcmp(s.p + i, "-->", 3))
            return dg_parse_num((Str){.p = s.p, .n = i}, lo) && dg_parse_num((Str){.p = s.p + i + 3, .n = s.n - i - 3}, hi);
    return false;
}
/* "[a, "b, c", d]" -> items */
typedef VEC(Str) StrVec;

[[nodiscard]] static bool list(Arena a[static 1], Str s, StrVec v[static 1], bool oom[static 1]) {
    s = str_trim(s);
    if (s.n < 2 || s.p[0] != '[' || s.p[s.n - 1] != ']') return false;
    s = (Str){.p = s.p + 1, .n = s.n - 2};
    size_t i = 0;
    while (i < s.n) {
        while (i < s.n && (s.p[i] == ' ' || s.p[i] == '\t')) i++;
        size_t st = i;
        if (i < s.n && s.p[i] == '"') {
            const char *q = memchr(s.p + i + 1, '"', s.n - i - 1);
            if (!q) return false;
            i = (size_t)(q - s.p) + 1;
        }
        while (i < s.n && s.p[i] != ',') i++;
        Str it = dg_unquote((Str){.p = s.p + st, .n = i - st}, false);
        if (it.n || i < s.n) if (!VEC_PUSH(a, v, it)) { *oom = true; return false; }
        i++;
    }
    return true;
}
static double nice(double x) {
    double e = pow(10, floor(log10(x))), f = x / e;
    return (f <= 1 ? 1 : f <= 2 ? 2 : f <= 5 ? 5 : 10) * e;
}
static size_t fmt_num(double v, size_t cap, char b[static cap]) {
    int r = snprintf(b, cap, "%.6g", fabs(v) < 1e-9 ? 0.0 : v);
    return r < 0 ? 0 : (size_t)r < cap ? (size_t)r : cap - 1;
}

typedef struct R2 { double x, y, w, h; } R2;
/* overlap: every bar series uses the full band, centred on the tick */
static R2 bar_geom(bool horiz, bool overlap, size_t nbar, size_t bi, double band, double base, double p, double c) {
    double bwid = band * 0.9 / (overlap ? 1.0 : (double)nbar);
    double c0 = c - band * 0.45 + (overlap ? 0 : bwid * (double)bi);
    if (horiz) return (R2){fmin(base, p), c0, fabs(p - base), bwid};
    return (R2){c0, fmin(base, p), bwid, fabs(p - base)};
}
/* mermaid 11: shrink until the text sits in the bar. Fit uses real advances, not its 0.7em guess. */
static double fit_inside(bool horiz, double bw, double bh, Str lab) {
    if (!(bw > 0) || !(bh > 0) || !lab.n) return 0;
    double fs = horiz ? bh * 0.7 : bw / ((double)lab.n * 0.7);
    while (fs >= 1) {
        double tw = text_width(lab, fs);
        if (horiz ? tw <= bw - 10 : tw <= bw && 10 + fs <= bh) return floor(fs);
        fs -= 1;
    }
    return 0;
}
static void data_label(Buf out[static 1], Str lab, double fs, double x, double y, const char anchor[static 1], TBase base,
                       const char fill[static 1]) {
    buf_lit(out, "<g class=\"dl\" font-size=\"");
    buf_num(out, fs);
    buf_lit(out, "\" fill=\"");
    buf_cstr(out, fill);
    buf_lit(out, "\">");
    dg_tspans(out, nullptr, lab, fs, fs, x, y, false, anchor, base, nullptr);
    buf_lit(out, "</g>");
}

DIAG_FN(xychart_render) {
    LineIt it = {.s = src, .n = n, .i = off};
    Str line;
    size_t lo;
    if (!dg_next_line(&it, &line, &lo)) return diag_fail(err_off, msg, off, "empty xychart");
    bool horiz = false;
    {
        Str r = line;
        (void)dg_word(&r);
        if (str_eq(dg_word(&r), S("horizontal"))) horiz = true;
    }
    Str xt = {}, yt = {};
    StrVec cats = {};
    VEC(double) vals = {};
    VEC(Str) labs = {};
    VEC(Series) ser = {};
    double xmin = 0, xmax = 0, ymin = 0, ymax = 0;
    bool xr = false, yr = false, oom = false;
    while (dg_next_line(&it, &line, &lo)) {
        Str rest = line, w = dg_word(&rest);
        if (str_eq(w, S("title"))) { title = dg_unquote(rest, false); continue; }
        if (str_starts(w, S("acc"))) continue;
        if (str_eq(w, S("x-axis")) || str_eq(w, S("y-axis"))) {
            bool isx = w.p[0] == 'x';
            Str t = take_title(&rest);
            rest = str_trim(rest);
            if (isx) xt = t; else yt = t;
            if (!rest.n) continue;
            if (isx && rest.p[0] == '[') {
                cats.n = 0;
                if (!list(a, rest, &cats, &oom)) return oom ? MMD_ERR_NOMEM : diag_fail(err_off, msg, lo, "bad category list");
            } else if (range(rest, isx ? &xmin : &ymin, isx ? &xmax : &ymax)) {
                if (isx) xr = true; else yr = true;
            } else return diag_fail(err_off, msg, (size_t)(rest.p - src), "expected [categories] or min --> max");
            continue;
        }
        if (str_eq(w, S("bar")) || str_eq(w, S("line"))) {
            Series s = {.line = w.p[0] == 'l', .v0 = (uint32_t)vals.n};
            s.name = take_title(&rest);
            StrVec items = {};
            if (!list(a, rest, &items, &oom)) return oom ? MMD_ERR_NOMEM : diag_fail(err_off, msg, lo, "expected [numbers]");
            for (size_t i = 0; i < items.n; i++) {
                double d;
                Str itm = items.v[i], lab = {};
                for (size_t q = 0; q < itm.n; q++) /* `540 "PaLM"`: per-point label */
                    if (itm.p[q] == '"' && q + 1 < itm.n && itm.p[itm.n - 1] == '"') {
                        lab = (Str){.p = itm.p + q + 1, .n = itm.n - q - 2};
                        items.v[i] = str_trim((Str){.p = itm.p, .n = q});
                        break;
                    }
                if (!VEC_PUSH(a, &labs, lab)) return MMD_ERR_NOMEM;
                if (!dg_parse_num(items.v[i], &d)) return diag_fail(err_off, msg, (size_t)(items.v[i].p - src), "not a number");
                if (!VEC_PUSH(a, &vals, d)) return MMD_ERR_NOMEM;
            }
            s.nv = (uint32_t)items.n;
            if (!VEC_PUSH(a, &ser, s)) return MMD_ERR_NOMEM;
            continue;
        }
        return diag_fail(err_off, msg, lo, "unknown xychart statement");
    }
    if (!ser.n) return diag_fail(err_off, msg, off, "xychart has no data");
    size_t np = 0;
    for (size_t i = 0; i < ser.n; i++) if (ser.v[i].nv > np) np = ser.v[i].nv;
    if (!np) return diag_fail(err_off, msg, off, "xychart has no data");
    if (!yr) {
        ymin = 1e300, ymax = -1e300;
        for (size_t i = 0; i < vals.n; i++) { if (vals.v[i] < ymin) ymin = vals.v[i]; if (vals.v[i] > ymax) ymax = vals.v[i]; }
        if (ymin > 0) ymin = 0;
    }
    if (!(ymax > ymin)) ymax = ymin + 1;
    double step = nice((ymax - ymin) / 5);
    if (!yr) { ymin = floor(ymin / step) * step; ymax = ceil(ymax / step) * step; }
    /* categories: explicit, numeric range or 1..n */
    size_t nc = cats.n ? cats.n : np;
    char nb[48];
    const bool show_dl = dcfg_bool("xyChart.showDataLabel", false);
    const bool dl_out = show_dl && dcfg_bool("xyChart.showDataLabelOutsideBar", false);
    const bool overlap = dcfg_bool("xyChart.barOverlap", false);
    const double PW = fmin(fmax(dcfg_num("xyChart.width", 700), 100), 4000);
    const double PH = fmin(fmax(dcfg_num("xyChart.height", 500), 100), 4000) - 100;
    double tl = 0;
    for (double v = ymin; v <= ymax + step * 1e-6; v += step) {
        double w = text_width((Str){.p = nb, .n = fmt_num(v, sizeof nb, nb)}, 14);
        if (w > tl) tl = w;
        if ((ymax - ymin) / step > 1000) break;
    }
    /* vertical: value axis on the left; horizontal: categories on the left */
    double catw = 0;
    for (size_t i = 0; i < cats.n; i++) { double w = text_width(cats.v[i], 14); if (w > catw) catw = w; }
    double leftw = horiz ? (cats.n ? catw : tl) : tl;
    double ox = 20 + (horiz ? (xt.n ? 26 : 0) : (yt.n ? 26 : 0)) + leftw + 10, oy = title.n ? 50 : 15;
    /* outside labels sit above the bar top: drop the plot so they clear the title */
    if (dl_out && !horiz) oy += 24;
    double pw = PW, ph = horiz ? PH + 50 : PH;
    double vlen = horiz ? pw : ph, clen = horiz ? ph : pw, band = clen / (double)nc;
#define VPOS(v) (horiz ? ox + ((v) - ymin) / (ymax - ymin) * vlen : oy + ph - ((v) - ymin) / (ymax - ymin) * vlen)
#define CPOS(i) (horiz ? oy + band * ((double)(i) + 0.5) : ox + band * ((double)(i) + 0.5))
    /* vertical category labels wider than their band wrap at spaces; a chart whose labels all fit
     * keeps today's single line and the same box */
    Str *clab = nullptr;
    double cat_extra = 0;
    if (!horiz && cats.n) {
        bool wide = false;
        for (size_t i = 0; i < cats.n; i++)
            if (text_width(cats.v[i], 14) > band) { wide = true; break; }
        if (wide) {
            if (!(clab = ANEW(a, Str, cats.n))) return MMD_ERR_NOMEM;
            unsigned maxl = 1;
            bool any = false;
            for (size_t i = 0; i < cats.n; i++) {
                clab[i] = cats.v[i];
                if (text_width(cats.v[i], 14) <= band) continue;
                Str wr = dg_wrap(a, cats.v[i], 14, band);
                if (!wr.p && cats.v[i].n) return MMD_ERR_NOMEM;
                unsigned ln = 1;
                for (size_t k = 0; k < wr.n; k++) ln += wr.p[k] == '\n';
                if (ln > 1) {
                    clab[i] = wr;
                    any = true;
                    if (ln > maxl) maxl = ln;
                }
            }
            if (!any) clab = nullptr;
            else cat_extra = (maxl - 1) * 21.0;
        }
    }
    size_t nbar = 0;
    for (size_t s = 0; s < ser.n; s++) nbar += !ser.v[s].line;
    double base = VPOS(ymin < 0 && ymax > 0 ? 0.0 : ymin);
    /* gutter between the plot and the legend, so an outside label past a full bar is not covered */
    double gutter = 0;
    if (dl_out && horiz && nbar) {
        size_t bi = 0;
        for (size_t s = 0; s < ser.n; s++) {
            if (ser.v[s].line) continue;
            for (uint32_t k = 0; k < ser.v[s].nv && k < nc; k++) {
                double v = vals.v[ser.v[s].v0 + k];
                if (overlap && v == 0) continue;
                R2 r = bar_geom(true, overlap, nbar, bi, band, base, VPOS(v), CPOS(k));
                if (!(r.w > 0) || !(r.h > 0)) continue;
                char b[48];
                double tw = text_width((Str){.p = b, .n = fmt_num(v, sizeof b, b)}, DL_FS);
                gutter = fmax(gutter, r.x + r.w + 10 + tw - (ox + pw));
            }
            bi++;
        }
        if (gutter < 0) gutter = 0;
        else gutter += 8;
    }
    char dlfill[64];
    memcpy(dlfill, "#131300", 8);
    if (show_dl) {
        Str dlc = dcfg_tv("xyChart.dataLabelColor");
        if (dlc.n && dlc.n < sizeof dlfill) memcpy(dlfill, dlc.p, dlc.n), dlfill[dlc.n] = 0;
    }
    /* legend for named series, right of the plot */
    double legw = 0;
    for (size_t s = 0; s < ser.n; s++) if (ser.v[s].name.n) legw = fmax(legw, text_width(ser.v[s].name, 14) + 40);
    BBox bb = {0, 0, ox + pw + 20 + gutter + legw, oy + ph + 60 + cat_extra};
    if (show_dl && nbar) {
        size_t bi = 0;
        for (size_t s = 0; s < ser.n; s++) {
            if (ser.v[s].line) continue;
            double fs = dl_out ? DL_FS : 0;
            if (!dl_out) {
                fs = 1e300;
                for (uint32_t k = 0; k < ser.v[s].nv && k < nc; k++) {
                    double v = vals.v[ser.v[s].v0 + k];
                    if (overlap && v == 0) continue;
                    R2 r = bar_geom(horiz, overlap, nbar, bi, band, base, VPOS(v), CPOS(k));
                    if (!(r.w > 0) || !(r.h > 0)) continue;
                    char b[48];
                    Str lab = {.p = b, .n = fmt_num(v, sizeof b, b)};
                    fs = fmin(fs, fit_inside(horiz, r.w, r.h, lab));
                }
                if (!(fs < 1e300) || fs < 1) fs = 0;
            }
            if (fs >= 1)
                for (uint32_t k = 0; k < ser.v[s].nv && k < nc; k++) {
                    double v = vals.v[ser.v[s].v0 + k];
                    if (overlap && v == 0) continue;
                    R2 r = bar_geom(horiz, overlap, nbar, bi, band, base, VPOS(v), CPOS(k));
                    if (!(r.w > 0) || !(r.h > 0)) continue;
                    char b[48];
                    Str lab = {.p = b, .n = fmt_num(v, sizeof b, b)};
                    double x, y;
                    const char *anchor;
                    TBase tb;
                    if (horiz) {
                        y = r.y + r.h / 2;
                        x = dl_out ? r.x + r.w + 10 : r.x + r.w - 10;
                        anchor = dl_out ? "start" : "end";
                        tb = TB_MIDDLE;
                    } else {
                        x = r.x + r.w / 2;
                        y = dl_out ? r.y - 10 : r.y + 10;
                        anchor = "middle";
                        tb = dl_out ? TB_ALPHA : TB_HANG;
                    }
                    BBox t = dg_tbox(lab, fs, fs, x, y, false, anchor, tb);
                    bb_add(&bb, t.x0, t.y0, t.x1, t.y1);
                }
            bi++;
        }
    }
    if (!buf_reserve(out, 4096 + vals.n * 200 + nc * 200)) return MMD_ERR_NOMEM;
    DG_HEAD(out, bb, 4, "xychart", CSS);
    if (title.n) dg_text(out, title, 20, ox + pw / 2, 25, "middle", "ttl");
    /* bars first (side by side when several bar series, unless barOverlap), then lines */
    size_t bi = 0, li = 0;
    for (size_t s = 0; s < ser.n; s++) {
        if (ser.v[s].line) continue;
        const char *col = ser.v[s].col = dcfg_list("xyChart.plotColorPalette", s, PAL[bi % countof(PAL)]);
        for (uint32_t k = 0; k < ser.v[s].nv && k < nc; k++) {
            double v = vals.v[ser.v[s].v0 + k];
            if (overlap && v == 0) continue;
            R2 r = bar_geom(horiz, overlap, nbar, bi, band, base, VPOS(v), CPOS(k));
            dg_rect_fill(out, r.x, r.y, r.w, r.h, 0, nullptr, col);
        }
        bi++;
    }
    if (show_dl && nbar) {
        bi = 0;
        for (size_t s = 0; s < ser.n; s++) {
            if (ser.v[s].line) continue;
            double fs = dl_out ? DL_FS : 1e300;
            if (!dl_out)
                for (uint32_t k = 0; k < ser.v[s].nv && k < nc; k++) {
                    double v = vals.v[ser.v[s].v0 + k];
                    if (overlap && v == 0) continue;
                    R2 r = bar_geom(horiz, overlap, nbar, bi, band, base, VPOS(v), CPOS(k));
                    if (!(r.w > 0) || !(r.h > 0)) continue;
                    char b[48];
                    fs = fmin(fs, fit_inside(horiz, r.w, r.h, (Str){.p = b, .n = fmt_num(v, sizeof b, b)}));
                }
            if (fs >= 1 && fs < 1e300)
                for (uint32_t k = 0; k < ser.v[s].nv && k < nc; k++) {
                    double v = vals.v[ser.v[s].v0 + k];
                    if (overlap && v == 0) continue;
                    R2 r = bar_geom(horiz, overlap, nbar, bi, band, base, VPOS(v), CPOS(k));
                    if (!(r.w > 0) || !(r.h > 0)) continue;
                    char b[48];
                    Str lab = {.p = b, .n = fmt_num(v, sizeof b, b)};
                    if (horiz) data_label(out, lab, fs, dl_out ? r.x + r.w + 10 : r.x + r.w - 10, r.y + r.h / 2,
                                          dl_out ? "start" : "end", TB_MIDDLE, dlfill);
                    else data_label(out, lab, fs, r.x + r.w / 2, dl_out ? r.y - 10 : r.y + 10, "middle",
                                    dl_out ? TB_ALPHA : TB_HANG, dlfill);
                }
            bi++;
        }
    }
    for (size_t s = 0; s < ser.n; s++) {
        if (!ser.v[s].line) continue;
        const char *col = ser.v[s].col = dcfg_list("xyChart.plotColorPalette", s, LINEPAL[li++ % countof(LINEPAL)]);
        buf_lit(out, "<path class=\"ln\" stroke=\""); buf_cstr(out, col); buf_lit(out, "\" d=\"");
        for (uint32_t k = 0; k < ser.v[s].nv && k < nc; k++) {
            double v = vals.v[ser.v[s].v0 + k];
            buf_ch(out, k ? 'L' : 'M');
            if (horiz) buf_pt(out, VPOS(v), CPOS(k)); else buf_pt(out, CPOS(k), VPOS(v));
        }
        buf_lit(out, "\"/>");
    }
    for (size_t s = 0; s < ser.n; s++) /* per-point labels, above the point / bar end */
        for (uint32_t k = 0; k < ser.v[s].nv && k < nc; k++) {
            Str l = labs.v[ser.v[s].v0 + k];
            if (!l.n) continue;
            double p = VPOS(vals.v[ser.v[s].v0 + k]), c = CPOS(k);
            buf_lit(out, "<g font-size=\"12\" fill=\""); buf_cstr(out, ser.v[s].col); buf_lit(out, "\">");
            if (horiz) dg_text(out, l, 12, p + 6, c, "start", nullptr);
            else dg_text(out, l, 12, c, p - 10, "middle", nullptr);
            buf_lit(out, "</g>");
        }
    /* axes */
    dg_line(out, ox, oy, ox, oy + ph, "ax");
    dg_line(out, ox, oy + ph, ox + pw, oy + ph, "ax");
    for (int k = 0; k <= 1000; k++) {
        double v = ymin + step * k;
        if (v > ymax + step * 1e-6) break;
        Str t = {.p = nb, .n = fmt_num(v, sizeof nb, nb)};
        double p = VPOS(v);
        if (horiz) { dg_line(out, p, oy + ph, p, oy + ph + 5, "tk"); dg_text(out, t, 14, p, oy + ph + 16, "middle", "tl"); }
        else { dg_line(out, ox - 5, p, ox, p, "tk"); dg_text(out, t, 14, ox - 8, p, "end", "tl"); }
    }
    for (size_t i = 0; i < nc; i++) {
        Str t;
        if (cats.n) t = clab ? clab[i] : cats.v[i];
        else if (xr) t = (Str){.p = nb, .n = fmt_num(xmin + (nc > 1 ? (xmax - xmin) * (double)i / (double)(nc - 1) : 0), sizeof nb, nb)};
        else t = (Str){.p = nb, .n = fmt_num((double)(i + 1), sizeof nb, nb)};
        double p = CPOS(i);
        if (horiz) { dg_line(out, ox - 5, p, ox, p, "tk"); dg_text(out, t, 14, ox - 8, p, "end", "tl"); }
        else {
            dg_line(out, p, oy + ph, p, oy + ph + 5, "tk");
            unsigned ln = 1;
            for (size_t k = 0; k < t.n; k++) ln += t.p[k] == '\n';
            /* first line stays at +16, where a single-line label is centred */
            dg_text(out, t, 14, p, oy + ph + 16 + (ln - 1) * 10.5, "middle", "tl");
        }
    }
    if (legw > 0) {
        double ly = oy + 10, lx = ox + pw + 20 + gutter;
        for (size_t s = 0; s < ser.n; s++) {
            if (!ser.v[s].name.n) continue;
            if (ser.v[s].line) { buf_lit(out, "<path class=\"ln\" stroke=\""); buf_cstr(out, ser.v[s].col);
                buf_lit(out, "\" d=\"M"); buf_pt(out, lx, ly); buf_ch(out, 'L'); buf_pt(out, lx + 16, ly); buf_lit(out, "\"/>"); }
            else dg_rect_fill(out, lx + 2, ly - 6, 12, 12, 0, "lg", ser.v[s].col);
            dg_text(out, ser.v[s].name, 14, lx + 22, ly, "start", "tl");
            ly += 22;
        }
    }
#undef VPOS
#undef CPOS
    Str bottom = horiz ? yt : xt, side = horiz ? xt : yt;
    if (bottom.n) dg_text(out, bottom, 16, ox + pw / 2, oy + ph + 42 + cat_extra, "middle", "at");
    if (side.n) {
        buf_lit(out, "<text class=\"at\" text-anchor=\"middle\" dominant-baseline=\"central\" transform=\"translate(");
        buf_num(out, 30); buf_ch(out, ','); buf_num(out, oy + ph / 2); buf_lit(out, ") rotate(-90)\">");
        buf_xml(out, side);
        buf_lit(out, "</text>");
    }
    buf_lit(out, "</svg>\n");
    return out->oom ? MMD_ERR_NOMEM : MMD_OK;
}
