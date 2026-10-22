/* journey: title, sections, tasks "name : score : actor, actor". Mermaid-like layout: section
 * headers, task boxes, a face per task placed by score, actor legend with coloured dots. */
#include <stdio.h>

#include "dgraph.h"
#include "dcfg.h"
#include "diagrams.h"
#include "text.h"

typedef struct JTask { Str name; double score; uint32_t sec; uint32_t a0, na; } JTask;

/* byte-wise string order, as JS Array.prototype.sort() does for ASCII */
static bool jr_less(Str x, Str y) {
    size_t n = x.n < y.n ? x.n : y.n;
    int c = n ? memcmp(x.p, y.p, n) : 0;
    return c < 0 || (c == 0 && x.n < y.n);
}

static constexpr char CSS[] =
    "#mermaid .face{fill:#FFF8DC;stroke:#999;stroke-width:1px;}#mermaid .feat{fill:none;stroke:#666;stroke-width:1.5px;}"
    "#mermaid .eye{fill:#666;}#mermaid .tl{stroke:#666;stroke-width:1px;stroke-dasharray:4 4;}"
    "#mermaid .tbox{stroke:#666;stroke-width:1px;}#mermaid .axis{stroke:#000;stroke-width:4px;}"
    "#mermaid .ttl{font-size:20px;font-weight:bold;}#mermaid .tx{font-size:14px;}"
    "#mermaid .adot{stroke:#000;stroke-width:1px;}";
static const char *const SECCOL[] = {"#ECECFF", "#ffffde", "#f5ffe0", "#e0f0ff", "#ffe0f0", "#e0ffe8"};
static const char *const ACTCOL[] = {"#8FBC8F", "#7CFC00", "#00FFFF", "#20B2AA", "#B0E0E6", "#FFFFE0", "#FFB6C1", "#87CEFA"};

static void face(Buf *b, double cx, double cy, double score) {
    buf_lit(b, "<circle class=\"face\" cx=\""); buf_num(b, cx); buf_lit(b, "\" cy=\""); buf_num(b, cy);
    buf_lit(b, "\" r=\"15\"/><circle class=\"eye\" r=\"1.5\" cx=\""); buf_num(b, cx - 5); buf_lit(b, "\" cy=\"");
    buf_num(b, cy - 5); buf_lit(b, "\"/><circle class=\"eye\" r=\"1.5\" cx=\""); buf_num(b, cx + 5);
    buf_lit(b, "\" cy=\""); buf_num(b, cy - 5); buf_lit(b, "\"/>");
    if (score > 3) { /* smile */
        buf_lit(b, "<path class=\"feat\" d=\"M"); dg_pt(b, cx - 7, cy + 2); buf_lit(b, "Q"); dg_pt(b, cx, cy + 10);
        buf_ch(b, ' '); dg_pt(b, cx + 7, cy + 2); buf_lit(b, "\"/>");
    } else if (score < 3) { /* frown */
        buf_lit(b, "<path class=\"feat\" d=\"M"); dg_pt(b, cx - 7, cy + 8); buf_lit(b, "Q"); dg_pt(b, cx, cy + 1);
        buf_ch(b, ' '); dg_pt(b, cx + 7, cy + 8); buf_lit(b, "\"/>");
    } else dg_line(b, cx - 6, cy + 6, cx + 6, cy + 6, "feat");
}

DIAG_FN(journey_render) {
    (void)fs;
    LineIt it = {.s = src, .n = n, .i = off};
    Str line;
    size_t lo;
    if (!dg_next_line(&it, &line, &lo)) return diag_fail(err_off, msg, off, "empty journey");
    VEC(Str) secs = {};
    VEC(Str) actors = {};
    VEC(uint32_t) refs = {}; /* actor indices per task */
    VEC(JTask) tk = {};
    while (dg_next_line(&it, &line, &lo)) {
        Str rest = line, w = dg_word(&rest);
        rest = str_trim(rest);
        if (str_eq(w, S("title"))) { title = rest; continue; }
        if (str_starts(w, S("acc"))) continue;
        if (str_eq(w, S("section"))) { if (!VEC_PUSH(a, &secs, rest)) return CMM_ERR_NOMEM; continue; }
        const char *c1 = memchr(line.p, ':', line.n);
        if (!c1) return diag_fail(err_off, msg, lo, "expected 'task : score : actors'");
        Str tail = {.p = c1 + 1, .n = line.n - (size_t)(c1 + 1 - line.p)};
        const char *c2 = memchr(tail.p, ':', tail.n);
        Str sc = c2 ? (Str){.p = tail.p, .n = (size_t)(c2 - tail.p)} : tail;
        JTask t = {.name = str_trim((Str){.p = line.p, .n = (size_t)(c1 - line.p)}), .sec = secs.n ? (uint32_t)secs.n - 1 : 0,
                   .a0 = (uint32_t)refs.n};
        if (!dg_parse_num(sc, &t.score)) return diag_fail(err_off, msg, (size_t)(sc.p - src), "expected a numeric score");
        if (c2) {
            Str al = {.p = c2 + 1, .n = tail.n - (size_t)(c2 + 1 - tail.p)};
            while (al.n) {
                const char *cm = memchr(al.p, ',', al.n);
                size_t k = cm ? (size_t)(cm - al.p) : al.n;
                Str nm = str_trim((Str){.p = al.p, .n = k});
                al = cm ? (Str){.p = cm + 1, .n = al.n - k - 1} : (Str){};
                if (!nm.n) continue;
                size_t ai = 0;
                while (ai < actors.n && !str_eq(actors.v[ai], nm)) ai++;
                if (ai == actors.n && !VEC_PUSH(a, &actors, nm)) return CMM_ERR_NOMEM;
                if (!VEC_PUSH(a, &refs, (uint32_t)ai)) return CMM_ERR_NOMEM;
            }
        }
        t.na = (uint32_t)refs.n - t.a0;
        if (!VEC_PUSH(a, &tk, t)) return CMM_ERR_NOMEM;
    }
    if (!tk.n) return diag_fail(err_off, msg, off, "journey has no tasks");
    { /* mermaid lists (and colours) actors in sorted order: insertion sort, then remap task refs */
        uint32_t *rank = ANEW(a, uint32_t, actors.n + 1), *inv = ANEW(a, uint32_t, actors.n + 1);
        if (!rank || !inv) return CMM_ERR_NOMEM;
        for (size_t i = 0; i < actors.n; i++) rank[i] = (uint32_t)i;
        for (size_t i = 1; i < actors.n; i++)
            for (size_t j = i; j > 0 && jr_less(actors.v[j], actors.v[j - 1]); j--) {
                Str t = actors.v[j]; actors.v[j] = actors.v[j - 1]; actors.v[j - 1] = t;
                uint32_t r = rank[j]; rank[j] = rank[j - 1]; rank[j - 1] = r;
            }
        for (size_t i = 0; i < actors.n; i++) inv[rank[i]] = (uint32_t)i;
        for (size_t i = 0; i < refs.n; i++) refs.v[i] = inv[refs.v[i]];
    }
    double lw = 0;
    for (size_t i = 0; i < actors.n; i++) {
        double w = text_width(actors.v[i], 14);
        if (w > lw) lw = w;
    }
    constexpr double TW = 150;
    constexpr double TM = 50;
    constexpr double SY = 60;
    constexpr double TY = 120;
    constexpr double BOX = 50;
    constexpr double FACE0 = 200;
    constexpr double AX = 340;
    double left = actors.n ? lw + 60 : 20, W = left + tk.n * (TW + TM);
    BBox bb = {0, 0, W, AX + 30};
    if (!buf_reserve(out, 4096 + tk.n * 1500)) return CMM_ERR_NOMEM;
    DG_HEAD(out, bb, 4, "journey", CSS);
    if (title.n) dg_text(out, title, 20, left, 25, "start", "ttl");
    for (size_t i = 0; i < actors.n; i++) {
        double y = 60 + i * 24;
        buf_lit(out, "<circle class=\"adot\" r=\"7\" cx=\"20\" cy=\""); buf_num(out, y);
        buf_lit(out, "\" fill=\""); buf_put(out, strlen(ACTCOL[i % countof(ACTCOL)]), ACTCOL[i % countof(ACTCOL)]);
        buf_lit(out, "\"/>");
        dg_text(out, actors.v[i], 14, 35, y, "start", "tx");
    }
    for (size_t i = 0; i < tk.n;) { /* section headers span their tasks */
        size_t j = i;
        while (j < tk.n && tk.v[j].sec == tk.v[i].sec) j++;
        double x = left + i * (TW + TM), w = (j - i) * (TW + TM) - TM;
        const char *col = dcfg_pal("fillType", tk.v[i].sec % countof(SECCOL), SECCOL[tk.v[i].sec % countof(SECCOL)]);
        if (secs.n) {
            dg_rect_fill(out, x, SY, w, BOX, 3, "tbox", col);
            dg_text(out, dg_wrap(a, secs.v[tk.v[i].sec], 14, w - 10), 14, x + w / 2, SY + BOX / 2, "middle", "tx");
        }
        i = j;
    }
    for (size_t i = 0; i < tk.n; i++) {
        const JTask *t = &tk.v[i];
        double x = left + i * (TW + TM), cx = x + TW / 2;
        double s = t->score < 0 ? 0 : t->score > 5 ? 5 : t->score, fy = FACE0 + (5 - s) * 25;
        dg_line(out, cx, TY + BOX, cx, AX, "tl");
        dg_rect_fill(out, x, TY, TW, BOX, 3, "tbox", dcfg_pal("fillType", t->sec % countof(SECCOL), SECCOL[t->sec % countof(SECCOL)]));
        Str nm = dg_wrap(a, t->name, 14, TW - 10);
        dg_text(out, nm, 14, cx, TY + BOX / 2, "middle", "tx");
        face(out, cx, fy, s);
        for (uint32_t k = 0; k < t->na; k++) {
            uint32_t ai = refs.v[t->a0 + k];
            buf_lit(out, "<circle class=\"adot\" r=\"7\" cx=\""); buf_num(out, x + 10 + k * 16);
            buf_lit(out, "\" cy=\""); buf_num(out, TY); buf_lit(out, "\" fill=\"");
            buf_put(out, strlen(ACTCOL[ai % countof(ACTCOL)]), ACTCOL[ai % countof(ACTCOL)]);
            buf_lit(out, "\"/>");
        }
    }
    buf_lit(out, "<defs><marker id=\"jarrow\" markerWidth=\"6\" markerHeight=\"6\" refX=\"5\" refY=\"3\" orient=\"auto\">"
                 "<path d=\"M0,0L6,3L0,6Z\"/></marker></defs>");
    buf_lit(out, "<line class=\"axis\" marker-end=\"url(#jarrow)\" x1=\""); buf_num(out, left);
    buf_lit(out, "\" y1=\""); buf_num(out, AX); buf_lit(out, "\" x2=\""); buf_num(out, W - 10);
    buf_lit(out, "\" y2=\""); buf_num(out, AX); buf_lit(out, "\"/>");
    buf_lit(out, "</svg>\n");
    return out->oom ? CMM_ERR_NOMEM : CMM_OK;
}
