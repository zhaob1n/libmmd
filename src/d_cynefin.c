/* cynefin-beta: the five Cynefin domains (complex/complicated/clear/chaotic quadrants plus the
 * confusion ellipse) with item badges and `a --> b : "label"` transitions. Port of mermaid's
 * cynefinRenderer: seeded wavy boundaries (mulberry32, seed = hash of the svg id "my-svg" as in
 * mermaid-cli unless cynefin.seed is set), the clear/chaotic cliff, fixed 800x600 layout. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "dgraph.h"
#include "dcfg.h"
#include "diagrams.h"
#include "text.h"

enum { CX, CD, CH, CL, CF, ND }; /* complex complicated chaotic clear confusion (draw order) */
static const char *const NAME[ND] = {"complex", "complicated", "chaotic", "clear", "confusion"};
static const char *const CAP[ND] = {"Complex", "Complicated", "Chaotic", "Clear", "Confusion"};
static const char *const MODEL[ND] = {"Probe \u2192 Sense \u2192 Respond", "Sense \u2192 Analyse \u2192 Respond",
                                      "Act \u2192 Sense \u2192 Respond", "Sense \u2192 Categorise \u2192 Respond", ""};
static const char *const PRACT[ND] = {"Emergent Practices", "Good Practices", "Novel Practices", "Best Practices", "Disorder"};
static const char *const BG[ND] = {"#E8F5E9", "#E3F2FD", "#FBE9E7", "#FFF8E1", "#F3E5F5"};
static const char *const BGKEY[ND] = {"complexBg", "complicatedBg", "chaoticBg", "clearBg", "confusionBg"};

typedef struct CItem { unsigned dom; Str label; } CItem;
typedef struct CTr { unsigned a, b; Str label; } CTr;

static constexpr char CSS[] =
    "#mermaid .cdl{font-size:16px;font-weight:bold;fill:#131300;}#mermaid .csub{font-size:11px;fill:#333;font-style:italic;}"
    "#mermaid .cit{fill-opacity:0.95;stroke:#333333;stroke-width:1;}#mermaid .citt{font-size:12px;fill:#333;}"
    "#mermaid .cio{fill-opacity:0.6;stroke:#333333;stroke-width:1;stroke-dasharray:3 2;}"
    "#mermaid .cbd{fill:none;stroke:#333333;stroke-width:2;stroke-dasharray:6 3;}#mermaid .ccl{fill:none;stroke:#8B0000;stroke-width:4;}"
    "#mermaid .ccf{stroke:#333333;stroke-width:1.5;stroke-dasharray:4 2;}#mermaid .car{stroke:#333333;stroke-width:2;fill:none;}"
    "#mermaid .cah{fill:#333333;stroke:none;}#mermaid .cal{font-size:11px;fill:#333;}"
    "#mermaid .cttl{font-size:18px;font-weight:bold;fill:#131300;}";

static double rnd(int64_t seed) { /* mermaid seededRandom (mulberry32 step) */
    uint32_t t = (uint32_t)seed + 0x6d2b79f5u;
    t = (t ^ (t >> 15)) * (t | 1u);
    t ^= t + (t ^ (t >> 7)) * (t | 61u);
    return (double)(t ^ (t >> 14)) / 4294967296.0;
}

static int32_t hash_id(const char *s) {
    uint32_t h = 0;
    for (; *s; s++) h = (h << 5) - h + (uint32_t)(unsigned char)*s;
    return (int32_t)h;
}

static void wavy(Buf *b, double W, double H, int64_t seed, double amp, bool vert) {
    double P[8][2];
    for (int i = 0; i <= 7; i++) {
        double j = rnd(seed + i * (vert ? 17 : 23)) * amp * 2 - amp;
        P[i][0] = vert ? W / 2 + j : i * W / 7;
        P[i][1] = vert ? i * H / 7 : H / 2 + j;
    }
    buf_lit(b, "<path class=\"cbd\" d=\"M"); buf_pt(b, P[0][0], P[0][1]);
    for (int i = 0; i < 7; i++) {
        double off = amp * 1.5 * (i % 2 ? -1 : 1) * rnd(seed + i * (vert ? 31 : 37) + (vert ? 7 : 11));
        double m = vert ? (P[i][1] + P[i + 1][1]) / 2 : (P[i][0] + P[i + 1][0]) / 2;
        buf_lit(b, " C");
        if (vert) { buf_pt(b, P[i][0] + off, m); buf_ch(b, ' '); buf_pt(b, P[i + 1][0] - off, m); }
        else { buf_pt(b, m, P[i][1] + off); buf_ch(b, ' '); buf_pt(b, m, P[i + 1][1] - off); }
        buf_ch(b, ' '); buf_pt(b, P[i + 1][0], P[i + 1][1]);
    }
    buf_lit(b, "\"/>");
}

static void text(Buf *b, const char *s, double x, double y, const char *cls, const char *base) {
    buf_lit(b, "<text class=\""); buf_put(b, strlen(cls), cls); buf_lit(b, "\" x=\""); buf_num(b, x);
    buf_lit(b, "\" y=\""); buf_num(b, y); buf_lit(b, "\" text-anchor=\"middle\" dominant-baseline=\"");
    buf_put(b, strlen(base), base); buf_lit(b, "\">");
    buf_xml(b, (Str){.p = s, .n = strlen(s)});
    buf_lit(b, "</text>");
}

static int dom_of(Str w) {
    for (int d = 0; d < ND; d++) if (str_eq(w, (Str){.p = NAME[d], .n = strlen(NAME[d])})) return d;
    return -1;
}

static Str unq(Str s) {
    s = str_trim(s);
    if (s.n >= 2 && (s.p[0] == '"' || s.p[0] == '\'') && s.p[s.n - 1] == s.p[0]) s.p++, s.n -= 2;
    return s;
}

DIAG_FN(cynefin_render) {
    (void)fs;
    LineIt it = {.s = src, .n = n, .i = off};
    Str ln;
    size_t lo;
    if (!dg_next_line(&it, &ln, &lo)) return diag_fail(err_off, msg, off, "empty cynefin");
    VEC(CItem) items = {};
    VEC(CTr) trs = {};
    int cur = -1;
    while (dg_next_line(&it, &ln, &lo)) {
        Str r = ln, w = dg_word(&r);
        if (str_eq(w, S("title"))) { title = str_trim(r); continue; }
        if (str_starts(ln, S("acc"))) continue;
        const char *arrow = nullptr;
        for (size_t k = 0; k + 3 <= ln.n; k++) if (!memcmp(ln.p + k, "-->", 3)) { arrow = ln.p + k; break; }
        if (arrow && ln.p[0] != '"') {
            Str sa = str_trim((Str){.p = ln.p, .n = (size_t)(arrow - ln.p)});
            Str rest = {.p = arrow + 3, .n = ln.n - (size_t)(arrow + 3 - ln.p)};
            const char *c = memchr(rest.p, ':', rest.n);
            Str sb = str_trim((Str){.p = rest.p, .n = c ? (size_t)(c - rest.p) : rest.n});
            int da = dom_of(sa), db = dom_of(sb);
            if (da < 0 || db < 0) return diag_fail(err_off, msg, lo, "transition needs two domain names");
            Str lab = c ? unq((Str){.p = c + 1, .n = rest.n - (size_t)(c + 1 - rest.p)}) : (Str){};
            if (da != db && !VEC_PUSH(a, &trs, ((CTr){(unsigned)da, (unsigned)db, lab}))) return CMM_ERR_NOMEM;
            continue;
        }
        int d = !r.n ? dom_of(w) : -1;
        if (d >= 0) { cur = d; continue; }
        if (cur < 0) return diag_fail(err_off, msg, lo, "item outside a domain block");
        bool ok;
        Str lab = label_normalize(a, unq(ln), &ok);
        if (!ok) return CMM_ERR_NOMEM;
        if (!VEC_PUSH(a, &items, ((CItem){(unsigned)cur, lab}))) return CMM_ERR_NOMEM;
    }
    double W = dcfg_num("cynefin.width", 800), H = dcfg_num("cynefin.height", 600), P = dcfg_num("cynefin.padding", 40);
    W = fmin(fmax(W, 100), 10000), H = fmin(fmax(H, 100), 10000), P = fmin(fmax(P, 0), 1000);
    bool desc = dcfg_bool("cynefin.showDomainDescriptions", true);
    double amp = fmin(fmax(dcfg_num("cynefin.boundaryAmplitude", 8), 0), 200);
    double cfg_seed = dcfg_num("cynefin.seed", 0);
    int64_t seed = cfg_seed != 0 && isfinite(cfg_seed) && fabs(cfg_seed) < 1e15 ? (int64_t)cfg_seed : hash_id("my-svg");
    const char *bg[ND];
    for (int d = 0; d < ND; d++) {
        char key[48];
        snprintf(key, sizeof key, "themeVariables.cynefin.%s", BGKEY[d]);
        Str v = dcfg_get(key);
        bg[d] = BG[d];
        if (v.n && v.n < 40 && v.p[0] == '#') {
            char *c = arena_alloc(a, v.n + 1, 1);
            if (!c) return CMM_ERR_NOMEM;
            memcpy(c, v.p, v.n), c[v.n] = 0, bg[d] = c;
        }
    }
    double hw = W / 2, hh = H / 2;
    const double L[ND][4] = {{hw / 2, hh / 2, 0, 0}, {hw + hw / 2, hh / 2, hw, 0}, {hw / 2, hh + hh / 2, 0, hh},
                             {hw + hw / 2, hh + hh / 2, hw, hh}, {hw, hh, 0, 0}};
    Buf b = {};
    buf_lit(&b, "<defs><marker id=\"cyn-arrow\" viewBox=\"0 0 10 10\" refX=\"9\" refY=\"5\" markerWidth=\"6\" markerHeight=\"6\" "
                "orient=\"auto-start-reverse\"><path d=\"M 0 0 L 10 5 L 0 10 z\" class=\"cah\"/></marker></defs><g transform=\"translate(");
    buf_num(&b, P); buf_ch(&b, ','); buf_num(&b, P); buf_lit(&b, ")\">");
    for (int d = 0; d < CF; d++) {
        buf_lit(&b, "<rect x=\""); buf_num(&b, L[d][2]); buf_lit(&b, "\" y=\""); buf_num(&b, L[d][3]);
        buf_lit(&b, "\" width=\""); buf_num(&b, hw); buf_lit(&b, "\" height=\""); buf_num(&b, hh);
        buf_lit(&b, "\" fill=\""); buf_put(&b, strlen(bg[d]), bg[d]); buf_lit(&b, "\" fill-opacity=\"0.4\" stroke=\"none\"/>");
    }
    wavy(&b, W, H, seed, amp, true);
    wavy(&b, W, H, seed + 100, amp, false);
    double cx = W / 2, A = W * 0.03, top = H * 0.5, sp = H - top;
    buf_lit(&b, "<path class=\"ccl\" d=\"M"); buf_pt(&b, cx, top);
    buf_lit(&b, " C"); buf_pt(&b, cx + A, top + sp * 0.2); buf_ch(&b, ' '); buf_pt(&b, cx - A * 1.5, top + sp * 0.55);
    buf_ch(&b, ' '); buf_pt(&b, cx + A * 0.5, top + sp * 0.75);
    buf_lit(&b, " C"); buf_pt(&b, cx - A, top + sp * 0.85); buf_ch(&b, ' '); buf_pt(&b, cx + A * 0.3, top + sp * 0.95);
    buf_ch(&b, ' '); buf_pt(&b, cx, H); buf_lit(&b, "\"/>");
    double rx = W * 0.15, ry = H * 0.15;
    buf_lit(&b, "<path class=\"ccf\" d=\"M"); buf_pt(&b, cx - rx, hh); buf_lit(&b, " A"); buf_pt(&b, rx, ry);
    buf_lit(&b, " 0 1,1 "); buf_pt(&b, cx + rx, hh); buf_lit(&b, " A"); buf_pt(&b, rx, ry); buf_lit(&b, " 0 1,1 ");
    buf_pt(&b, cx - rx, hh); buf_lit(&b, " Z\" fill=\""); buf_put(&b, strlen(bg[CF]), bg[CF]); buf_lit(&b, "\" fill-opacity=\"0.5\"/>");
    for (int d = 0; d < CF; d++) text(&b, CAP[d], L[d][0], desc ? L[d][1] - 30 : L[d][1], "cdl", "middle");
    text(&b, CAP[CF], W / 2, desc ? H / 2 - 10 : H / 2, "cdl", "middle");
    if (desc) {
        for (int d = 0; d < CF; d++) {
            text(&b, MODEL[d], L[d][0], L[d][1] - 10, "csub", "middle");
            text(&b, PRACT[d], L[d][0], L[d][1] + 5, "csub", "middle");
        }
        text(&b, PRACT[CF], W / 2, H / 2 + 8, "csub", "middle");
    }
    for (int d = 0; d < ND; d++) {
        size_t cnt = 0, shown = 0;
        for (size_t i = 0; i < items.n; i++) cnt += items.v[i].dom == (unsigned)d;
        double y0 = L[d][1] + (d == CF ? (desc ? 22 : 14) : (desc ? 25 : 15));
        for (size_t i = 0; i <= items.n; i++) {
            bool over = i == items.n;
            if (over ? !(d == CF && cnt > 3) : items.v[i].dom != (unsigned)d || (d == CF && shown >= 3)) continue;
            char ob[32];
            Str lab = over ? (Str){.p = ob, .n = (size_t)snprintf(ob, sizeof ob, "+%zu more", cnt - 3)} : items.v[i].label;
            double bw = text_width(lab, 12) + 20, x = L[d][0] - bw / 2, y = y0 + (double)shown * 30;
            buf_lit(&b, "<rect class=\""); if (over) buf_lit(&b, "cio"); else buf_lit(&b, "cit");
            buf_lit(&b, "\" x=\""); buf_num(&b, x); buf_lit(&b, "\" y=\""); buf_num(&b, y);
            buf_lit(&b, "\" width=\""); buf_num(&b, bw); buf_lit(&b, "\" height=\"26\" rx=\"4\" ry=\"4\" fill=\"");
            buf_put(&b, strlen(bg[d]), bg[d]); buf_lit(&b, "\"/>");
            dg_text(&b, lab, 12, x + bw / 2, y + 13, "middle", "citt");
            shown++;
        }
    }
    for (size_t i = 0; i < trs.n; i++) {
        const CTr *t = &trs.v[i];
        double x1 = L[t->a][0], y1 = L[t->a][1], x2 = L[t->b][0], y2 = L[t->b][1];
        double dx = x2 - x1, dy = y2 - y1, len = sqrt(dx * dx + dy * dy);
        double cpx = (x1 + x2) / 2 - dy / len * len * 0.15, cpy = (y1 + y2) / 2 + dx / len * len * 0.15;
        buf_lit(&b, "<path class=\"car\" d=\"M"); buf_pt(&b, x1, y1); buf_lit(&b, " Q"); buf_pt(&b, cpx, cpy);
        buf_ch(&b, ' '); buf_pt(&b, x2, y2); buf_lit(&b, "\" marker-end=\"url(#cyn-arrow)\"/>");
        if (t->label.n) {
            buf_lit(&b, "<text class=\"cal\" x=\""); buf_num(&b, cpx); buf_lit(&b, "\" y=\""); buf_num(&b, cpy - 6);
            buf_lit(&b, "\" text-anchor=\"middle\">"); buf_xml(&b, t->label); buf_lit(&b, "</text>");
        }
    }
    if (title.n) {
        buf_lit(&b, "<text class=\"cttl\" x=\""); buf_num(&b, W / 2); buf_lit(&b, "\" y=\""); buf_num(&b, -P / 2);
        buf_lit(&b, "\" text-anchor=\"middle\" dominant-baseline=\"middle\">"); buf_xml(&b, title); buf_lit(&b, "</text>");
    }
    buf_lit(&b, "</g>");
    return DG_FINISH(out, &b, ((BBox){0, 0, W + 2 * P, H + 2 * P}), 0, "cynefin", CSS);
}
