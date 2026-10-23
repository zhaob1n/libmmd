/* pie: optional showData, title (line or front matter), "label" : value slices; legend. */
#include <math.h>
#include <stdio.h>

#include "dgraph.h"
#include "dcfg.h"
#include "diagrams.h"
#include "text.h"

typedef struct Slice { Str label; double v; } Slice;

static constexpr char CSS[] =
    "#mermaid .slice{stroke:#000;stroke-width:2px;opacity:0.7;}"
    "#mermaid .outer{fill:none;stroke:#000;stroke-width:2px;}"
    "#mermaid .pct{font-size:17px;}#mermaid .ttl{font-size:25px;}";
/* mermaid default theme pie1..pie12 (hsl adjustments of primary/secondary/tertiary) */
static const char *const COLORS[] = {"#ececff", "#ffffde", "#b5ff20", "#b9b9ff", "#ffff45", "#d7ff86", "#ff86ff", "#20ffff", "#ff2020", "#ff20ff", "#20ff8f", "#ff5353"};

DIAG_FN(pie_render) {
    LineIt it = {.s = src, .n = n, .i = off};
    Str line;
    size_t lo;
    if (!dg_next_line(&it, &line, &lo)) return diag_fail(err_off, msg, off, "empty pie");
    Str rest = line;
    (void)dg_word(&rest);
    bool show = false;
    for (Str w = dg_word(&rest); w.n; w = dg_word(&rest)) {
        if (str_eq(w, S("showData"))) show = true;
        else if (str_eq(w, S("title"))) { title = str_trim(rest); break; }
    }
    VEC(Slice) sl = {};
    double total = 0;
    while (dg_next_line(&it, &line, &lo)) {
        rest = line;
        Str w = dg_word(&rest);
        if (str_eq(w, S("title"))) { title = str_trim(rest); continue; }
        if (str_eq(w, S("showData"))) { show = true; continue; }
        if (str_starts(w, S("accTitle")) || str_starts(w, S("accDescr"))) continue;
        Str label, tail;
        const char *q;
        if (line.p[0] == '"') {
            q = memchr(line.p + 1, '"', line.n - 1);
            if (!q) return diag_fail(err_off, msg, lo, "unterminated slice label");
            label = (Str){.p = line.p + 1, .n = (size_t)(q - line.p - 1)};
        } else { /* unquoted label: up to the last ':' */
            q = nullptr;
            for (size_t k = line.n; k-- > 0;) if (line.p[k] == ':') { q = line.p + k - 1; break; }
            if (!q) return diag_fail(err_off, msg, lo, "expected label : value");
            label = str_trim((Str){.p = line.p, .n = (size_t)(q + 1 - line.p)});
        }
        tail = str_trim((Str){.p = q + 1, .n = line.n - (size_t)(q + 1 - line.p)});
        double v;
        if (!tail.n || tail.p[0] != ':' || !dg_parse_num(str_trim((Str){.p = tail.p + 1, .n = tail.n - 1}), &v) || v < 0)
            return diag_fail(err_off, msg, (size_t)(q + 1 - src), "expected ': <non-negative number>'");
        if (!VEC_PUSH(a, &sl, ((Slice){label, v}))) return CMM_ERR_NOMEM;
        total += v;
    }
    if (!sl.n) return diag_fail(err_off, msg, off, "pie has no slices");
    constexpr double R = 185;
    const double TPOS = fmin(fmax(dcfg_num("pie.textPosition", 0.75), 0), 1);
    constexpr double CX = 225;
    constexpr double CY = 225;
    double lw = 0;
    for (size_t i = 0; i < sl.n; i++) {
        double w = text_width(sl.v[i].label, fs) + (show ? text_width(S(" [000000]"), fs) : 0);
        if (w > lw) lw = w;
    }
    /* percentage labels: mermaid puts them at TPOS * R. One that doesn't fit its slice there, or would sit
     * on an earlier label, goes outside the pie with a leader line (deviation from mermaid: tiny slices
     * otherwise stack unreadable "1%" labels on top of each other) */
    double *LX = ANEW(a, double, sl.n), *LY = ANEW(a, double, sl.n), *LW = ANEW(a, double, sl.n), *LM = ANEW(a, double, sl.n);
    signed char *LO = ANEW(a, signed char, sl.n); /* 0 inside, +1 outside right, -1 outside left */
    if (!LX || !LY || !LW || !LM || !LO) return CMM_ERR_NOMEM;
    char pct[32];
    bool any_out_r = false;
    double ext_l = 0, ext_r = 0;
    {
        double ang0 = 0;
        for (size_t i = 0; i < sl.n; i++) {
            double frac = total > 0 ? sl.v[i].v / total : 0, mid = ang0 + frac * M_PI;
            ang0 += frac * 2 * M_PI;
            LM[i] = mid, LO[i] = 0, LW[i] = 0;
            if (frac <= 0) continue;
            int len = snprintf(pct, sizeof pct, "%.0f%%", frac * 100);
            LW[i] = text_width((Str){.p = pct, .n = (size_t)len}, 17);
            LX[i] = CX + TPOS * R * sin(mid), LY[i] = CY - TPOS * R * cos(mid);
            bool fits = frac >= 0.5 || 2 * TPOS * R * sin(frac * M_PI) >= fmin(LW[i], 20) + 6;
            for (size_t k = 0; k < i && fits; k++)
                if (LW[k] > 0 && !LO[k] && fabs(LX[k] - LX[i]) * 2 < LW[k] + LW[i] + 2 && fabs(LY[k] - LY[i]) < 19) fits = false;
            if (fits || sl.n == 1) continue;
            LO[i] = sin(mid) >= 0 ? 1 : -1;
            LX[i] = CX + (R + 30) * sin(mid), LY[i] = CY - (R + 30) * cos(mid);
        }
        /* outside labels: keep 19px apart vertically on each side, in angle order */
        for (int side = -1; side <= 1; side += 2) {
            double last = -INFINITY;
            for (size_t k = 0; k < sl.n; k++) {
                size_t i = side > 0 ? k : sl.n - 1 - k; /* right side runs top-down clockwise, left side bottom-up: walk both top-down */
                if (LO[i] != side) continue;
                if (LY[i] < last + 19) LY[i] = last + 19;
                last = LY[i];
            }
            (void)last;
        }
        for (size_t i = 0; i < sl.n; i++) {
            if (!LO[i]) continue;
            if (LO[i] > 0) any_out_r = true, ext_r = fmax(ext_r, LX[i] + 4 + LW[i] - (CX + R));
            else ext_l = fmax(ext_l, (CX - R) - (LX[i] - 4 - LW[i]));
        }
    }
    const double LEGX = CX + R + 50 + (any_out_r ? fmax(0, ext_r - 40) : 0);
    BBox bb = {0, 0, LEGX + 22 + lw, 450};
    if (ext_l > CX - R) bb.x0 = (CX - R) - ext_l - 4;
    for (size_t i = 0; i < sl.n; i++) if (LO[i]) bb.y0 = fmin(bb.y0, LY[i] - 12), bb.y1 = fmax(bb.y1, LY[i] + 12);
    double ty = -14; /* title baseline: above any outside label near 12 o'clock */
    for (size_t i = 0; i < sl.n; i++) if (LO[i] && fabs(LX[i] - CX) < 120) ty = fmin(ty, LY[i] - 22);
    if (title.n) bb.y0 = fmin(bb.y0, ty - 26);
    bb.y0 = fmin(bb.y0, CY - sl.n * 22 / 2.0 - 4), bb.y1 = fmax(bb.y1, CY + sl.n * 22 / 2.0 + 4); /* tall legends */
    if (!buf_reserve(out, 2048 + sl.n * 600)) return CMM_ERR_NOMEM;
    DG_HEAD(out, bb, 8, "pie", CSS);
    if (title.n) dg_text(out, title, 25, CX, ty, "middle", "ttl");
    /* mermaid 11 keeps declaration order, clockwise from 12 o'clock */
    uint32_t *ord = ANEW(a, uint32_t, sl.n);
    if (!ord) return CMM_ERR_NOMEM;
    for (size_t i = 0; i < sl.n; i++) ord[i] = (uint32_t)i;
    double ang = 0;
    char fill[64];
    for (size_t j = 0; j < sl.n; j++) {
        size_t i = ord[j];
        double frac = total > 0 ? sl.v[i].v / total : 0, a1 = ang + frac * 2 * M_PI;
        const char *col = dcfg_pal("pie", i % countof(COLORS) + 1, COLORS[i % countof(COLORS)]);
        if (frac >= 1 - 1e-12) {
            buf_lit(out, "<circle class=\"slice\" cx=\""); buf_num(out, CX); buf_lit(out, "\" cy=\""); buf_num(out, CY);
            buf_lit(out, "\" r=\""); buf_num(out, R); buf_lit(out, "\" fill=\""); buf_put(out, strlen(col), col); buf_lit(out, "\"/>");
        } else if (frac > 0) {
            buf_lit(out, "<path class=\"slice\" fill=\""); buf_put(out, strlen(col), col); buf_lit(out, "\" d=\"M");
            dg_pt(out, CX, CY); buf_ch(out, 'L'); dg_pt(out, CX + R * sin(ang), CY - R * cos(ang));
            buf_lit(out, "A"); buf_num(out, R); buf_ch(out, ','); buf_num(out, R);
            if (frac > 0.5) buf_lit(out, " 0 1 1 "); else buf_lit(out, " 0 0 1 ");
            dg_pt(out, CX + R * sin(a1), CY - R * cos(a1)); buf_lit(out, "Z\"/>");
        }
        ang = a1;
    }
    for (size_t i = 0; i < sl.n; i++) {
        double frac = total > 0 ? sl.v[i].v / total : 0, mid = LM[i];
        if (frac <= 0) continue;
        int len = snprintf(fill, sizeof fill, "%.0f%%", frac * 100);
        if (!LO[i]) { dg_text(out, (Str){.p = fill, .n = (size_t)len}, 17, LX[i], LY[i], "middle", "pct"); continue; }
        buf_lit(out, "<path class=\"leader\" fill=\"none\" stroke=\"#666\" stroke-width=\"1\" d=\"M");
        dg_pt(out, CX + (R - 4) * sin(mid), CY - (R - 4) * cos(mid)); buf_ch(out, 'L');
        dg_pt(out, CX + (R + 18) * sin(mid), CY - (R + 18) * cos(mid)); buf_ch(out, 'L');
        dg_pt(out, LX[i], LY[i]); buf_lit(out, "\"/>");
        dg_text(out, (Str){.p = fill, .n = (size_t)len}, 17, LX[i] + 4 * LO[i], LY[i], LO[i] > 0 ? "start" : "end", "pct");
    }
    buf_lit(out, "<circle class=\"outer\" cx=\""); buf_num(out, CX); buf_lit(out, "\" cy=\""); buf_num(out, CY);
    buf_lit(out, "\" r=\""); buf_num(out, R + 1); buf_lit(out, "\"/>");
    double ly = CY - sl.n * 22 / 2.0;
    for (size_t i = 0; i < sl.n; i++, ly += 22) {
        const char *col = dcfg_pal("pie", i % countof(COLORS) + 1, COLORS[i % countof(COLORS)]);
        buf_lit(out, "<rect width=\"18\" height=\"18\" x=\""); buf_num(out, LEGX); buf_lit(out, "\" y=\"");
        buf_num(out, ly); buf_lit(out, "\" fill=\""); buf_put(out, strlen(col), col); buf_lit(out, "\" stroke=\"#000\"/>");
        Str lab = sl.v[i].label;
        if (show) {
            int len = snprintf(fill, sizeof fill, " [%g]", sl.v[i].v);
            char *t = arena_alloc(a, lab.n + (size_t)len, 1);
            if (!t) return CMM_ERR_NOMEM;
            memcpy(t, lab.p, lab.n);
            memcpy(t + lab.n, fill, (size_t)len);
            lab = (Str){.p = t, .n = lab.n + (size_t)len};
        }
        dg_text(out, lab, fs, LEGX + 22, ly + 9, "start", nullptr);
    }
    buf_lit(out, "</svg>\n");
    return out->oom ? CMM_ERR_NOMEM : CMM_OK;
}
