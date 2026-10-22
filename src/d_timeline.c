/* timeline: title, sections, "period : event : event", continuation lines ": event".
 * Periods in a row (coloured per section, or per period without sections), events stacked below. */
#include "dgraph.h"
#include "diagrams.h"
#include "text.h"

typedef struct Period { Str name; uint32_t sec, e0, ne; } Period;

static constexpr char CSS[] =
    "#mermaid .box{stroke:#666;stroke-width:1px;}#mermaid .ev{fill:#fff;stroke:#999;stroke-width:1px;}"
    "#mermaid .ln{stroke:#666;stroke-width:2px;stroke-dasharray:5 5;}#mermaid .axis{stroke:#333;stroke-width:4px;}"
    "#mermaid .ttl{font-size:20px;font-weight:bold;}#mermaid .tx{font-size:14px;}";
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

DIAG_FN(timeline_render) {
    (void)fs;
    LineIt it = {.s = src, .n = n, .i = off};
    Str line;
    size_t lo;
    if (!dg_next_line(&it, &line, &lo)) return diag_fail(err_off, msg, off, "empty timeline");
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
    constexpr double PW = 150;
    constexpr double GAP = 20;
    constexpr double PH = 60;
    constexpr double SH = 50;
    constexpr double EG = 10;
    constexpr double TOPY = 50;
    double py = TOPY + (secs.n ? SH + 20 : 0);
    Str *wev = ANEW(a, Str, ev.n + 1);
    double *eh = ANEW(a, double, ev.n + 1);
    if (!wev || !eh) return CMM_ERR_NOMEM;
    double maxbot = py + PH;
    for (size_t i = 0; i < per.n; i++) {
        double y = py + PH + 40;
        for (uint32_t k = 0; k < per.v[i].ne; k++) {
            size_t e = per.v[i].e0 + k;
            wev[e] = dg_wrap(a, ev.v[e], 14, PW - 16);
            double w;
            unsigned nl;
            label_measure(wev[e], 14, &w, &nl);
            eh[e] = nl * 21 + 18;
            y += eh[e] + EG;
        }
        if (y > maxbot) maxbot = y;
    }
    double W = per.n * (PW + GAP) + GAP, axy = py + PH + 20;
    BBox bb = {0, 0, W, maxbot + 10};
    if (!buf_reserve(out, 4096 + (per.n + ev.n) * 400)) return CMM_ERR_NOMEM;
    DG_HEAD(out, bb, 4, "timeline", CSS);
    if (title.n) dg_text(out, title, 20, W / 2, 22, "middle", "ttl");
    if (secs.n)
        for (size_t i = 0; i < per.n;) {
            size_t j = i;
            while (j < per.n && per.v[j].sec == per.v[i].sec) j++;
            double x = GAP + i * (PW + GAP), w = (j - i) * (PW + GAP) - GAP;
            dg_rect_fill(out, x, TOPY, w, SH, 6, "box", COL[per.v[i].sec % countof(COL)]);
            TXG(out, per.v[i].sec);
            dg_text(out, dg_wrap(a, secs.v[per.v[i].sec], 14, w - 10), 14, x + w / 2, TOPY + SH / 2, "middle", "tx");
            buf_lit(out, "</g>");
            i = j;
        }
    buf_lit(out, "<defs><marker id=\"tarrow\" markerWidth=\"6\" markerHeight=\"6\" refX=\"5\" refY=\"3\" orient=\"auto\">"
                 "<path d=\"M0,0L6,3L0,6Z\"/></marker></defs>");
    for (size_t i = 0; i < per.n; i++) {
        double x = GAP + i * (PW + GAP), cx = x + PW / 2;
        const char *col = COL[(secs.n ? per.v[i].sec : i) % countof(COL)];
        double y = py + PH + 40, last = y;
        for (uint32_t k = 0; k < per.v[i].ne; k++) last = y, y += eh[per.v[i].e0 + k] + EG;
        if (per.v[i].ne) dg_line(out, cx, py + PH, cx, last, "ln");
        size_t ci = secs.n ? per.v[i].sec : i;
        TXG(out, ci);
        dg_rect_fill(out, x, py, PW, PH, 6, "box", col);
        dg_text(out, dg_wrap(a, per.v[i].name, 14, PW - 12), 14, cx, py + PH / 2, "middle", "tx");
        y = py + PH + 40;
        for (uint32_t k = 0; k < per.v[i].ne; k++) {
            size_t e = per.v[i].e0 + k;
            dg_rect_fill(out, x + 4, y, PW - 8, eh[e], 6, "box", col);
            dg_text(out, wev[e], 14, cx, y + eh[e] / 2, "middle", "tx");
            y += eh[e] + EG;
        }
        buf_lit(out, "</g>");
    }
    buf_lit(out, "<line class=\"axis\" marker-end=\"url(#tarrow)\" x1=\"4\" y1=\""); buf_num(out, axy);
    buf_lit(out, "\" x2=\""); buf_num(out, W - 4); buf_lit(out, "\" y2=\""); buf_num(out, axy); buf_lit(out, "\"/>");
    buf_lit(out, "</svg>\n");
    return out->oom ? CMM_ERR_NOMEM : CMM_OK;
}
