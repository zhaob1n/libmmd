/* timeline: title, sections, "period : event : event", continuation lines ": event".
 * Periods in a row (coloured per section, or per period without sections), events stacked below. */
#include <math.h>
#include <string.h>
#include "dgraph.h"
#include "dcfg.h"
#include "diagrams.h"
#include "text.h"

typedef struct Period { Str name; uint32_t sec, e0, ne; } Period;

static constexpr char CSS[] =
    "#mermaid .box{stroke:#666;stroke-width:1px;}#mermaid .ev{fill:#fff;stroke:#999;stroke-width:1px;}"
    "#mermaid .ln{stroke:#666;stroke-width:2px;stroke-dasharray:5 5;}#mermaid .axis{stroke:#333;stroke-width:4px;}"
    "#mermaid .ttl{font-size:20px;font-weight:bold;}#mermaid .tx{font-size:14px;}"
    "#mermaid .tl4{font-size:4ex;font-weight:bold;}#mermaid .t16{font-size:16px;}#mermaid .nl{stroke:none;}"
    "#mermaid .dl{stroke:#000;stroke-width:2px;stroke-dasharray:5 5;}#mermaid .ax4{stroke:#000;stroke-width:4px;}";
static const char *const COL[] = {"#8686ff", "#ffff78", "#d7ff86", "#c286ff", "#ff86ff", "#ff86c2",
                                  "#ff8686", "#ffc286", "#c2ff86", "#86ffc2", "#86ffff", "#86c2ff"}; /* mermaid cScale */
#define TXG(out, k) do { if ((k) % countof(COL) == 0) buf_lit(out, "<g fill=\"#fff\">"); else buf_lit(out, "<g fill=\"#000\">"); } while (0)

static void push_events(Arena *a, Str s, void *evp, bool *oom) {
    typedef VEC(Str) EV;
    EV *ev = evp;
    while (s.n) {
        const char *c = memchr(s.p, ':', s.n);
        size_t k = c ? (size_t)(c - s.p) : s.n;
        Str e = str_trim((Str){.p = s.p, .n = k});
        s = c ? (Str){.p = c + 1, .n = s.n - k - 1} : (Str){};
        if (e.n && !VEC_PUSH(a, ev, e)) { *oom = true; return; }
    }
}

/* mermaid timeline wrap(): words joined until the line exceeds `width` at 16px; <br> forces a break */
static Str tl_wrap(Arena *a, Str s, double width) { return dg_wrap(a, s, 16, width); }
/* getVirtualNodeHeight: bbox.height + 16*1.1*0.5 + padding(20); a 16px line box is 17px, extra lines 1.1em */
static double tl_vh(Str w, double pad) {
    double tw;
    unsigned nl;
    label_measure(w, 16, &tw, &nl);
    if (!nl) nl = 1;
    return 17 + (nl - 1) * 17.6 + 8.8 + pad;
}
/* drawNode + defaultBkg: rounded-top path, bottom line, text block from padding/2 + 1em */
static void tl_node(Buf *out, double x, double y, double w, double h, Str t, size_t ci, double pad) {
    const char *fill = dcfg_pal("cScale", ci, COL[ci]);
    buf_lit(out, "<g transform=\"translate("); buf_num(out, x); buf_lit(out, ","); buf_num(out, y); buf_lit(out, ")\">");
    buf_lit(out, "<path class=\"box\" fill=\""); buf_str(out, (Str){.p = fill, .n = strlen(fill)});
    buf_lit(out, "\" d=\"M0 "); buf_num(out, h - 5); buf_lit(out, " v"); buf_num(out, -h + 10);
    buf_lit(out, " q0,-5,5,-5 h"); buf_num(out, w - 10); buf_lit(out, " q5,0,5,5 v"); buf_num(out, h - 5); buf_lit(out, " H0 Z\"/>");
    TXG(out, ci);
    double tw;
    unsigned nl;
    label_measure(t, 16, &tw, &nl);
    if (!nl) nl = 1;
    dg_text_lh(out, t, 17.6, w / 2, pad / 2 + 16 + (nl - 1) * 17.6 / 2, "middle", "t16");
    buf_lit(out, "</g></g>");
}

DIAG_FN(timeline_render) {
    (void)fs;
    LineIt it = {.s = src, .n = n, .i = off};
    Str line;
    size_t lo;
    if (!dg_next_line(&it, &line, &lo)) return diag_fail(err_off, msg, off, "empty timeline");
    Str hrest = line;
    (void)dg_word(&hrest);
    const bool TD = str_eq(str_trim(hrest), S("TD"));
    VEC(Str) secs = {};
    VEC(Str) ev = {};
    VEC(Period) per = {};
    bool oom = false;
    while (dg_next_line(&it, &line, &lo)) {
        Str rest = line, w = dg_word(&rest);
        rest = str_trim(rest);
        if (str_eq(w, S("title"))) { title = rest; continue; }
        if (str_starts(w, S("acc"))) continue;
        if (str_eq(w, S("section"))) { if (!VEC_PUSH(a, &secs, rest)) return CMM_ERR_NOMEM; continue; }
        if (line.p[0] == ':') {
            if (!per.n) return diag_fail(err_off, msg, lo, "event without a period");
            if (per.v[per.n - 1].e0 + per.v[per.n - 1].ne != ev.n) return diag_fail(err_off, msg, lo, "misplaced event");
            push_events(a, (Str){.p = line.p + 1, .n = line.n - 1}, &ev, &oom);
            if (oom) return CMM_ERR_NOMEM;
            per.v[per.n - 1].ne = (uint32_t)ev.n - per.v[per.n - 1].e0;
            continue;
        }
        const char *c = memchr(line.p, ':', line.n);
        Period p = {.name = str_trim((Str){.p = line.p, .n = c ? (size_t)(c - line.p) : line.n}),
                    .sec = secs.n ? (uint32_t)secs.n - 1 : 0, .e0 = (uint32_t)ev.n};
        if (c) push_events(a, (Str){.p = c + 1, .n = line.n - (size_t)(c + 1 - line.p)}, &ev, &oom);
        if (oom) return CMM_ERR_NOMEM;
        p.ne = (uint32_t)ev.n - p.e0;
        if (!VEC_PUSH(a, &per, p)) return CMM_ERR_NOMEM;
    }
    if (!per.n) return diag_fail(err_off, msg, off, "timeline has no periods");
    const bool MULTI = !dcfg_bool("timeline.disableMulticolor", false);
    /* ---- mermaid timelineRenderer (LR) ---- */
    Str *wev = ANEW(a, Str, ev.n + 1);
    double *eh = ANEW(a, double, ev.n + 1);
    if (!wev || !eh) return CMM_ERR_NOMEM;
    if (TD) { /* mermaid timelineRendererVertical */
        const double LMv = dcfg_num("timeline.leftMargin", 150), P = 5, NW = 200, EWv = 300;
        const double X0 = 50 + LMv, TX = X0 + NW + 2 * P + 20, SW = fmax(50, (NW + 2 * P + 20) + (EWv + 2 * P + 50) - 2 * P);
        Str *ws = ANEW(a, Str, secs.n + 1), *wp = ANEW(a, Str, per.n + 1);
        double *hp = ANEW(a, double, per.n + 1);
        if (!ws || !wp || !hp) return CMM_ERR_NOMEM;
        double mSH = 0, mTH = 0, mES = 0;
        for (size_t k = 0; k < secs.n; k++) { ws[k] = tl_wrap(a, secs.v[k], SW); mSH = fmax(mSH, tl_vh(ws[k], P)); }
        for (size_t i = 0; i < per.n; i++) {
            wp[i] = tl_wrap(a, per.v[i].name, NW);
            hp[i] = tl_vh(wp[i], P);
            mTH = fmax(mTH, hp[i]);
            double l = 0;
            for (uint32_t k = 0; k < per.v[i].ne; k++) {
                size_t e = per.v[i].e0 + k;
                wev[e] = tl_wrap(a, ev.v[e], EWv);
                eh[e] = tl_vh(wev[e], P);
                l += eh[e] + (k ? 10 : 0);
            }
            mES = fmax(mES, l);
        }
        const double TS = fmax(mTH, mES) + 30;
        /* pass 0: extents; pass 1: draw */
        double bot = 50, tx = 0;
        for (int pass = 0; pass < 2; pass++) {
            if (pass) {
                double x0 = TX - (NW + 2 * P + 20), x1 = TX + 50 + EWv + 2 * P;
                if (secs.n) x1 = fmax(x1, x0 + SW + 2 * P);
                tx = (x1 - x0) / 2 - LMv;
                BBox bb = {fmin(x0, title.n ? tx : x0), title.n ? -11 : 18, x1, bot + 28};
                if (title.n) bb.x1 = fmax(bb.x1, tx + text_width(title, 33.2) * 1.06);
                if (!buf_reserve(out, 4096 + (per.n + ev.n + secs.n) * 600)) return CMM_ERR_NOMEM;
                DG_HEAD(out, bb, 50, "timeline", CSS);
                /* the vertical renderer references url(#arrowhead), which is never defined: no arrowheads */
                buf_lit(out, "<line class=\"ax4\" x1=\""); buf_num(out, TX);
                buf_lit(out, "\" y1=\"18\" x2=\""); buf_num(out, TX); buf_lit(out, "\" y2=\""); buf_num(out, bot + 28); buf_lit(out, "\"/>");
            }
            double my = 50;
            for (size_t k = 0; k < (secs.n ? secs.n : 1); k++) {
                double ty = my;
                if (secs.n) {
                    if (pass) tl_node(out, TX - (NW + 2 * P + 20), my, SW + 2 * P, mSH, ws[k], MULTI ? k % countof(COL) : 0, P);
                    ty = my + mSH + 20;
                }
                size_t c = 0;
                for (size_t i = 0; i < per.n; i++) {
                    if (secs.n && per.v[i].sec != k) continue;
                    size_t ci = (secs.n ? (MULTI ? k : 0) : (MULTI ? i : 0)) % countof(COL);
                    if (pass) tl_node(out, TX - 20 - (NW + 2 * P), ty, NW + 2 * P, fmax(hp[i], mTH), wp[i], ci, P);
                    bot = fmax(bot, ty + fmax(hp[i], mTH));
                    double ey = ty;
                    for (uint32_t m = 0; m < per.v[i].ne; m++) {
                        size_t e = per.v[i].e0 + m;
                        if (pass) {
                            buf_lit(out, "<line class=\"dl\" x1=\""); buf_num(out, TX);
                            buf_lit(out, "\" y1=\""); buf_num(out, ey + eh[e] / 2); buf_lit(out, "\" x2=\""); buf_num(out, TX + 50);
                            buf_lit(out, "\" y2=\""); buf_num(out, ey + eh[e] / 2); buf_lit(out, "\"/>");
                            tl_node(out, TX + 50, ey, EWv + 2 * P, eh[e], wev[e], ci, P);
                        }
                        bot = fmax(bot, ey + eh[e]);
                        ey += eh[e] + 10;
                    }
                    ty += TS;
                    c++;
                }
                if (secs.n) my += mSH + 20 + TS * (double)(c ? c : 1) - (c ? 60 : 0);
            }
        }
        if (title.n) {
            buf_lit(out, "<text class=\"tl4\" x=\""); buf_num(out, tx); buf_lit(out, "\" y=\"20\">");
            buf_xml(out, title); buf_lit(out, "</text>");
        }
        buf_lit(out, "</svg>\n");
        return out->oom ? CMM_ERR_NOMEM : CMM_OK;
    }
    const double LM = dcfg_num("timeline.leftMargin", 150);
    Str *wper = ANEW(a, Str, per.n + 1), *wsec = ANEW(a, Str, secs.n + 1);
    double *ph = ANEW(a, double, per.n + 1), *sh = ANEW(a, double, secs.n + 1);
    size_t *cnt = ANEW(a, size_t, secs.n + 1);
    if (!wper || !wsec || !ph || !sh || !cnt) return CMM_ERR_NOMEM;
    double maxSH = 0, maxTH = 0, maxELL = 0;
    for (size_t k = 0; k < secs.n; k++) {
        wsec[k] = tl_wrap(a, secs.v[k], 150);
        sh[k] = tl_vh(wsec[k], 20);
        if (sh[k] + 20 > maxSH) maxSH = sh[k] + 20;
        cnt[k] = 0;
    }
    for (size_t i = 0; i < per.n; i++) {
        if (secs.n) cnt[per.v[i].sec]++;
        wper[i] = tl_wrap(a, per.v[i].name, 150);
        ph[i] = tl_vh(wper[i], 20);
        if (ph[i] + 20 > maxTH) maxTH = ph[i] + 20;
        double l = 0;
        for (uint32_t k = 0; k < per.v[i].ne; k++) {
            size_t e = per.v[i].e0 + k;
            wev[e] = tl_wrap(a, ev.v[e], 150);
            eh[e] = tl_vh(wev[e], 20);
            l += eh[e] + (k ? 10 : 0);
        }
        if (l > maxELL) maxELL = l;
    }
    /* content extents = svg.getBBox() before the title and axis are added */
    const bool hs = secs.n > 0;
    const double X0 = 50 + LM, ty = hs ? 50 + maxSH + 50 : 50;
    double cx1 = X0, cy1 = 50;
    if (hs) {
        double x = X0;
        for (size_t k = 0; k < secs.n; k++) {
            double slot = 200 * (double)(cnt[k] ? cnt[k] : 1);
            if (x + slot - 10 > cx1) cx1 = x + slot - 10;
            if (cnt[k] && ty + maxTH + 200 + maxELL > cy1) cy1 = ty + maxTH + 200 + maxELL;
            x += slot;
        }
    } else {
        cx1 = X0 + 200 * (double)per.n - 10;
        cy1 = ty + maxTH + 200 + maxELL;
    }
    const double bw = cx1 - X0, depthY = hs ? maxSH + maxTH + 150 : maxTH + 100;
    const double ax2 = bw + 3 * LM, tfs = 33.2, tx = bw / 2 - LM;
    BBox bb = {LM, 50, ax2, cy1};
    if (title.n) {
        if (20 - 31 < bb.y0) bb.y0 = 20 - 31; /* 4ex bold ascent */
        double tw = text_width(title, tfs) * 1.06;
        if (tx < bb.x0) bb.x0 = tx;
        if (tx + tw > bb.x1) bb.x1 = tx + tw;
    }
    if (!buf_reserve(out, 4096 + (per.n + ev.n + secs.n) * 500)) return CMM_ERR_NOMEM;
    DG_HEAD(out, bb, 50, "timeline", CSS);
    buf_lit(out, "<defs><marker id=\"tarrow\" refX=\"5\" refY=\"2\" markerWidth=\"6\" markerHeight=\"4\" orient=\"auto\">"
                 "<path d=\"M 0,0 V 4 L6,2 Z\"/></marker></defs>");
    double x = X0;
    for (size_t k = 0; k < (hs ? secs.n : 1); k++) {
        size_t c = hs ? cnt[k] : per.n;
        if (hs) {
            size_t ci = MULTI ? k % countof(COL) : 0;
            tl_node(out, x, 50, 200 * (double)(c ? c : 1) - 10, fmax(sh[k], maxSH), wsec[k], ci, 20);
        }
        size_t idx = 0;
        for (size_t i = 0; i < per.n; i++) {
            if (hs && per.v[i].sec != k) continue;
            double px = x + 200 * (double)idx++;
            size_t ci = (hs ? (MULTI ? k : 0) : (MULTI ? i : 0)) % countof(COL);
            buf_lit(out, "<line class=\"dl\" marker-end=\"url(#tarrow)\" x1=\""); buf_num(out, px + 95);
            buf_lit(out, "\" y1=\""); buf_num(out, ty + maxTH); buf_lit(out, "\" x2=\""); buf_num(out, px + 95);
            buf_lit(out, "\" y2=\""); buf_num(out, ty + maxTH + 200 + maxELL); buf_lit(out, "\"/>");
            tl_node(out, px, ty, 190, fmax(ph[i], maxTH), wper[i], ci, 20);
            double ey = ty + 200;
            for (uint32_t m = 0; m < per.v[i].ne; m++) {
                size_t e = per.v[i].e0 + m;
                double h = fmax(eh[e], 50);
                tl_node(out, px, ey, 190, h, wev[e], ci, 20);
                ey += 10 + h;
            }
        }
        x += 200 * (double)(c ? c : 1);
    }
    if (title.n) {
        buf_lit(out, "<text class=\"tl4\" x=\""); buf_num(out, tx); buf_lit(out, "\" y=\"20\">");
        buf_xml(out, title); buf_lit(out, "</text>");
    }
    buf_lit(out, "<line class=\"ax4\" marker-end=\"url(#tarrow)\" x1=\""); buf_num(out, LM);
    buf_lit(out, "\" y1=\""); buf_num(out, depthY); buf_lit(out, "\" x2=\""); buf_num(out, ax2);
    buf_lit(out, "\" y2=\""); buf_num(out, depthY); buf_lit(out, "\"/>");
    buf_lit(out, "</svg>\n");
    return out->oom ? CMM_ERR_NOMEM : CMM_OK;
}
