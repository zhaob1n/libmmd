/* xychart(-beta) [horizontal]: title, x-axis ["title"] [a, b, ..] | [title] min --> max,
 * y-axis ["title"] [min --> max], bar [..], line [..] (optional series title before the list).
 * Bars grow from zero (mermaid: from the bottom of the plot); an automatic range takes zero in, and
 * an explicit range without it starts the bars at its edge nearest zero.
 *
 * Config (%%{init}%% / front matter, dotted as xyChart.*):
 *   showDataLabel, showDataLabelOutsideBar — mermaid 11. Labels use each bar's own value
 *   (mermaid labels every bar from plots[0]), written as mermaid writes it (String(v)).
 *   Outside labels are 12px: mermaid shrinks them with the inside-fit heuristic, which makes
 *   end labels on a tall horizontal chart unreadably small. A bar below zero is labelled at its
 *   own end (mermaid draws every bar up from the axis), and an automatic range gains a step
 *   when the outside label would reach the axis labels; under an explicit range the label
 *   moves inside the bar instead.
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
    "#mermaid .tl,#mermaid .lt{font-size:14px;fill:#131300;}#mermaid .at{font-size:16px;fill:#131300;}"
    "#mermaid .ttl{font-size:20px;fill:#131300;}#mermaid .ln{fill:none;stroke-width:2px;}#mermaid .lg{stroke:#131300;stroke-width:.5px;}";
static constexpr char PAL[][8] = {"#ECECFF", "#8493A6", "#FFC3A0", "#DCDDE1", "#B8E994",
                                  "#D1A36F", "#C3CDE6", "#FFB6C1", "#496078", "#F8F3E3"};
static constexpr char LINEPAL[][8] = {"#8493A6", "#FF7F50", "#3c9a3c", "#7b52ab", "#c0392b"};
static constexpr double DL_FS = 12; /* outside data labels; inside size is fitted per series */
/* room an outside label needs past its bar's end: 10px gap and 4px to spare */
static double out_need(bool horiz, Str lab) { return 14 + (horiz ? text_width(lab, DL_FS) : DL_FS); }

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
/* the tick i·step, divided as d3 does below 1 so that 3·0.1 prints as 0.3 */
static double tick_at(double i, double step) { return step < 1 ? i / round(1 / step) : i * step; }
/* point i of nc spread over x-axis min --> max, rounded to the spacing's leading digit and three
 * more: steps of 1/3 read 0.3333, and float noise drops */
static double xr_at(double xmin, double xmax, size_t nc, size_t i) {
    if (nc < 2) return xmin;
    double sp = fabs(xmax - xmin) / (double)(nc - 1), v = xmin + (xmax - xmin) * (double)i / (double)(nc - 1);
    if (!(sp > 0)) return v;
    double p = pow(10, fmax(3 - floor(log10(sp)), 0));
    if (!(fabs(v) * p < 0x1p53)) return v; /* no digit left to round away (and p may be infinite) */
    return round(v * p) / p;
}
/* x-axis label i without categories: the point's value on a min --> max range, else i + 1 */
static Str x_num(bool xr, double xmin, double xmax, size_t nc, size_t i, char nb[static 32]) {
    return (Str){.p = nb, .n = fmt_js(xr ? xr_at(xmin, xmax, nc, i) : (double)(i + 1), nb)};
}

typedef struct R2 { double x, y, w, h; } R2;
/* overlap: every bar series uses the full band, centred on the tick */
static R2 bar_geom(bool horiz, bool overlap, size_t nbar, size_t bi, double band, double base, double p, double c) {
    double bwid = band * 0.9 / (overlap ? 1.0 : (double)nbar);
    double c0 = c - band * 0.45 + (overlap ? 0 : bwid * (double)bi);
    if (horiz) return (R2){fmin(base, p), c0, fabs(p - base), bwid};
    return (R2){c0, fmin(base, p), bwid, fabs(p - base)};
}
/* mermaid 11: shrink 1px at a time from its 0.7em guess until the text sits in the bar, with real
 * advances. Width is linear in the size up to a 1/64px snap, so skip to one step above the linear
 * bound and walk only the snap from there. */
static double fit_inside(bool horiz, double bw, double bh, Str lab) {
    if (!(bw > 0) || !(bh > 0) || !lab.n) return 0;
    double fs = horiz ? bh * 0.7 : bw / ((double)lab.n * 0.7);
    double em = text_width(lab, 2048) / 2048; /* exact advance per px of size */
    if (em > 0) {
        double cap = horiz ? (bw - 10) / em : fmin(bw / em, bh - 10);
        if (fs > cap + 1) fs -= ceil(fs - cap) - 1;
    }
    for (; fs >= 1; fs -= 1) {
        double tw = text_width(lab, fs);
        if (horiz ? tw <= bw - 10 : tw <= bw && 10 + fs <= bh) return floor(fs);
    }
    return 0;
}
typedef struct DLab { double x, y, fs; const char *anchor; TBase tb; size_t n; char t[32]; } DLab;
static Str dl_str(const DLab d[static 1]) { return (Str){.p = d->t, .n = d->n}; }
static BBox dl_box(const DLab d[static 1]) { return dg_tbox(dl_str(d), d->fs, d->fs, d->x, d->y, false, d->anchor, d->tb); }
static void data_label(Buf out[static 1], const DLab d[static 1], const char fill[static 1]) {
    buf_lit(out, "<g class=\"dl\" font-size=\"");
    buf_num(out, d->fs);
    buf_lit(out, "\" fill=\"");
    buf_cstr(out, fill);
    buf_lit(out, "\">");
    dg_tspans(out, nullptr, dl_str(d), d->fs, d->fs, d->x, d->y, false, d->anchor, d->tb, nullptr);
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
        if (ymin > 0) ymin = 0; /* bars start at zero, so an automatic range takes it in */
        if (ymax < 0) ymax = 0;
    }
    if (!(ymax > ymin)) ymax = ymin + 1;
    double step = nice((ymax - ymin) / 5);
    if (!yr) { ymin = floor(ymin / step) * step; ymax = ceil(ymax / step) * step; }
    /* categories: explicit, numeric range or 1..n */
    size_t nc = cats.n ? cats.n : np;
    char nb[32];
    const bool show_dl = dcfg_bool("xyChart.showDataLabel", false);
    const bool dl_out = show_dl && dcfg_bool("xyChart.showDataLabelOutsideBar", false);
    const bool overlap = dcfg_bool("xyChart.barOverlap", false);
    const double PW = fmin(fmax(dcfg_num("xyChart.width", 700), 100), 4000);
    const double PH = fmin(fmax(dcfg_num("xyChart.height", 500), 100), 4000) - 100;
    /* an outside label past a bar below zero needs room before the axis: widen an automatic range */
    if (dl_out && !yr && ymin < 0) {
        bool grow = true;
        for (int g = 0; g < 8 && grow; g++) {
            grow = false;
            for (size_t s = 0; s < ser.n && !grow; s++)
                for (uint32_t k = 0; !ser.v[s].line && k < ser.v[s].nv && k < nc; k++) {
                    double v = vals.v[ser.v[s].v0 + k];
                    if (v >= 0) continue;
                    double need = out_need(horiz, (Str){.p = nb, .n = fmt_js(v, nb)});
                    if ((v - ymin) / (ymax - ymin) * (horiz ? PW : PH) < need) { ymin -= step, grow = true; break; }
                }
        }
    }
    if (!(ymax - ymin > 0) || !isfinite(ymax - ymin) || (xr && !isfinite(xmax - xmin)))
        return diag_fail(err_off, msg, off, "axis range too large for a double");
    /* value ticks: the multiples of step inside the range, at most 1001 (counted with an int: past
     * 2^53 steps from zero, adding 1 to a double tick index changes nothing); the 1e-6 slack
     * keeps an end tick lost to float noise, not one that lies outside the range */
    double t0 = ceil(ymin / step - 1e-6), t1 = floor(ymax / step + 1e-6);
    if (tick_at(t0, step) < ymin - step * 1e-9) t0++;
    if (tick_at(t1, step) > ymax + step * 1e-9) t1--;
    const int nt = (int)fmin(t1 - t0, 1000);
    double tl = 0;
    for (int k = 0; k <= nt; k++) tl = fmax(tl, text_width((Str){.p = nb, .n = fmt_js(tick_at(t0 + k, step), nb)}, 14));
    /* vertical: value axis on the left; horizontal: the x labels on the left */
    double catw = 0;
    for (size_t i = 0; horiz && i < nc; i++)
        catw = fmax(catw, text_width(cats.n ? cats.v[i] : x_num(xr, xmin, xmax, nc, i, nb), 14));
    double leftw = horiz ? catw : tl;
    double ox = 20 + (horiz ? (xt.n ? 26 : 0) : (yt.n ? 26 : 0)) + leftw + 10, oy = title.n ? 50 : 15;
    /* outside labels sit above a rising bar's top: drop the plot so they clear the title */
    if (dl_out && !horiz && ymax > 0) oy += 24;
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
    double base = VPOS(fmin(fmax(0.0, ymin), ymax)); /* zero, or the range edge nearest it */
    /* data labels at each drawn bar's end, past it or inside it; inside labels share one fitted size
     * per series, and a series whose labels do not fit draws none */
    DLab *dl = nullptr;
    size_t ndl = 0;
    if (show_dl && nbar) {
        if (!(dl = ANEW(a, DLab, vals.n))) return MMD_ERR_NOMEM;
        size_t bi = 0;
        for (size_t s = 0; s < ser.n; s++) {
            if (ser.v[s].line) continue;
            size_t s0 = ndl;
            double fs = dl_out ? DL_FS : INFINITY;
            for (uint32_t k = 0; k < ser.v[s].nv && k < nc; k++) {
                double v = vals.v[ser.v[s].v0 + k], p = VPOS(v);
                if (overlap && v == 0) continue;
                R2 r = bar_geom(horiz, overlap, nbar, bi, band, base, p, CPOS(k));
                if (!(r.w > 0) || !(r.h > 0)) continue;
                DLab *d = &dl[ndl++];
                d->n = fmt_js(v, d->t);
                if (!dl_out) fs = fmin(fs, fit_inside(horiz, r.w, r.h, dl_str(d)));
                /* the label sits at the bar's far end, which is left of / below the baseline for a
                 * negative value: past it outside, short of it inside */
                bool up = horiz ? p > base : p < base, fwd = dl_out == up; /* text right of / above the end */
                /* below zero, an explicit range can end right past the bar, leaving the label no room
                 * before the axis labels (an automatic one grew for it): put it inside instead */
                if (dl_out && !up) {
                    double need = out_need(horiz, dl_str(d));
                    if ((horiz ? r.x - ox : oy + ph - r.y - r.h) < need && (horiz ? r.w : r.h) >= need) fwd = true;
                }
                if (horiz) {
                    double end = up ? r.x + r.w : r.x;
                    d->x = fwd ? end + 10 : end - 10, d->y = r.y + r.h / 2;
                    d->anchor = fwd ? "start" : "end", d->tb = TB_MIDDLE;
                } else {
                    double end = up ? r.y : r.y + r.h;
                    d->x = r.x + r.w / 2, d->y = fwd ? end - 10 : end + 10;
                    d->anchor = "middle", d->tb = fwd ? TB_ALPHA : TB_HANG;
                }
            }
            if (fs >= 1 && isfinite(fs))
                for (size_t i = s0; i < ndl; i++) dl[i].fs = fs;
            else ndl = s0;
            bi++;
        }
    }
    /* gutter between the plot and the legend, so an outside label past a full bar is not covered */
    double gutter = 0;
    for (size_t i = 0; i < ndl && dl_out && horiz; i++) gutter = fmax(gutter, dl_box(&dl[i]).x1 - (ox + pw));
    if (gutter > 0) gutter += 8;
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
    for (size_t i = 0; i < ndl; i++) {
        BBox t = dl_box(&dl[i]);
        bb_add(&bb, t.x0, t.y0, t.x1, t.y1);
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
    for (size_t i = 0; i < ndl; i++) data_label(out, &dl[i], dlfill);
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
    /* axes: x is the category axis (left in a horizontal chart), y the value axis; each carries its
     * own class so the xyChart.{x,y}Axis*Color variables reach it */
    const char *vx = horiz ? "ax x" : "ax y", *cx = horiz ? "ax y" : "ax x";
    dg_line(out, ox, oy, ox, oy + ph, vx);
    dg_line(out, ox, oy + ph, ox + pw, oy + ph, cx);
    for (int k = 0; k <= nt; k++) {
        double v = tick_at(t0 + k, step), p = VPOS(v);
        Str t = {.p = nb, .n = fmt_js(v, nb)};
        if (horiz) { dg_line(out, p, oy + ph, p, oy + ph + 5, "tk y"); dg_text(out, t, 14, p, oy + ph + 16, "middle", "tl y"); }
        else { dg_line(out, ox - 5, p, ox, p, "tk y"); dg_text(out, t, 14, ox - 8, p, "end", "tl y"); }
    }
    for (size_t i = 0; i < nc; i++) {
        Str t = !cats.n ? x_num(xr, xmin, xmax, nc, i, nb) : clab ? clab[i] : cats.v[i];
        double p = CPOS(i);
        if (horiz) { dg_line(out, ox - 5, p, ox, p, "tk x"); dg_text(out, t, 14, ox - 8, p, "end", "tl x"); }
        else {
            dg_line(out, p, oy + ph, p, oy + ph + 5, "tk x");
            unsigned ln = 1;
            for (size_t k = 0; k < t.n; k++) ln += t.p[k] == '\n';
            /* first line stays at +16, where a single-line label is centred */
            dg_text(out, t, 14, p, oy + ph + 16 + (ln - 1) * 10.5, "middle", "tl x");
        }
    }
    if (legw > 0) {
        double ly = oy + 10, lx = ox + pw + 20 + gutter;
        for (size_t s = 0; s < ser.n; s++) {
            if (!ser.v[s].name.n) continue;
            if (ser.v[s].line) { buf_lit(out, "<path class=\"ln\" stroke=\""); buf_cstr(out, ser.v[s].col);
                buf_lit(out, "\" d=\"M"); buf_pt(out, lx, ly); buf_ch(out, 'L'); buf_pt(out, lx + 16, ly); buf_lit(out, "\"/>"); }
            else dg_rect_fill(out, lx + 2, ly - 6, 12, 12, 0, "lg", ser.v[s].col);
            dg_text(out, ser.v[s].name, 14, lx + 22, ly, "start", "lt");
            ly += 22;
        }
    }
#undef VPOS
#undef CPOS
    Str bottom = horiz ? yt : xt, side = horiz ? xt : yt;
    if (bottom.n) dg_text(out, bottom, 16, ox + pw / 2, oy + ph + 42 + cat_extra, "middle", horiz ? "at y" : "at x");
    if (side.n) {
        buf_cstr(out, horiz ? "<text class=\"at x\"" : "<text class=\"at y\"");
        buf_lit(out, " text-anchor=\"middle\" dominant-baseline=\"central\" transform=\"translate(");
        buf_num(out, 30); buf_ch(out, ','); buf_num(out, oy + ph / 2); buf_lit(out, ") rotate(-90)\">");
        buf_xml(out, side);
        buf_lit(out, "</text>");
    }
    buf_lit(out, "</svg>\n");
    return out->oom ? MMD_ERR_NOMEM : MMD_OK;
}
