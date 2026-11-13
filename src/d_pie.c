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
static constexpr char COLORS[][8] = {"#ececff", "#ffffde", "#b5ff20", "#b9b9ff", "#ffff45", "#d7ff86", "#ff86ff", "#20ffff", "#ff2020", "#ff20ff", "#20ff8f", "#ff5353"};

/* showData's " [v]" after a legend label, v as mermaid writes it (String(v)) */
static Str shown_value(double v, char out[static 40]) {
    memcpy(out, " [", 2);
    size_t n = 2 + fmt_js(v, out + 2);
    out[n++] = ']';
    return (Str){.p = out, .n = n};
}

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
        if (!VEC_PUSH(a, &sl, ((Slice){label, v}))) return MMD_ERR_NOMEM;
        total += v;
    }
    if (!sl.n) return diag_fail(err_off, msg, off, "pie has no slices");
    constexpr double R = 185;
    const double TPOS = fmin(fmax(dcfg_num("pie.textPosition", 0.75), 0), 1);
    constexpr double CX = 225;
    constexpr double CY = 225;
    double lw = 0;
    char val[40];
    for (size_t i = 0; i < sl.n; i++)
        lw = fmax(lw, text_width(sl.v[i].label, fs) + (show ? text_width(shown_value(sl.v[i].v, val), fs) : 0));
    /* percentage labels: mermaid puts them at TPOS * R. One that doesn't fit its slice there, or would sit
     * on an earlier label, goes outside the pie with a leader line (deviation from mermaid: tiny slices
     * otherwise stack unreadable "1%" labels on top of each other) */
    double *LX = ANEW(a, double, sl.n), *LY = ANEW(a, double, sl.n), *LW = ANEW(a, double, sl.n), *LM = ANEW(a, double, sl.n);
    signed char *LO = ANEW(a, signed char, sl.n); /* 0 inside, +1 outside right, -1 outside left */
    if (!LX || !LY || !LW || !LM || !LO) return MMD_ERR_NOMEM;
    char pct[32];
    bool any_out_r = false;
    double ext_l = 0, ext_r = 0;
    {
        double ang0 = 0;
        for (size_t i = 0; i < sl.n; i++) {
            double frac = total > 0 ? sl.v[i].v / total : 0, mid = ang0 + frac * M_PI, pc = round(frac * 100);
            ang0 += frac * 2 * M_PI;
            LM[i] = mid, LO[i] = 0, LW[i] = 0;
            if (pc < 1) continue; /* a "0%" says nothing the slice and the legend do not */
            int len = snprintf(pct, sizeof pct, "%.0f%%", pc); /* toFixed(0) */
            LW[i] = text_width((Str){.p = pct, .n = (size_t)len}, 17);
            LX[i] = CX + TPOS * R * sin(mid), LY[i] = CY - TPOS * R * cos(mid);
            bool fits = frac >= 0.5 || 2 * TPOS * R * sin(frac * M_PI) >= fmin(LW[i], 20) + 6;
            for (size_t k = 0; k < i && fits; k++)
                if (LW[k] > 0 && !LO[k] && fabs(LX[k] - LX[i]) * 2 < LW[k] + LW[i] + 2 && fabs(LY[k] - LY[i]) < 19) fits = false;
            if (fits || sl.n == 1) continue;
            LO[i] = sin(mid) >= 0 ? 1 : -1;
            LX[i] = CX + (R + 30) * sin(mid), LY[i] = CY - (R + 30) * cos(mid);
        }
        /* outside labels, each side top-down in angle order: 19px apart and as near their slices as
         * that allows (runs of touching labels centred on the heights they want), then back on the
         * label circle at their new height, so a run never slides into the pie; a side whose labels
         * reach past the circle's height lines them all up beside it instead, where no leader crosses
         * another label */
        typedef struct { double s; size_t n, first; } Run; /* s: the sum of want - 19 * position */
        Run *run = ANEW(a, Run, sl.n);
        size_t *ord = ANEW(a, size_t, sl.n);
        if (!run || !ord) return MMD_ERR_NOMEM;
        for (int side = -1; side <= 1; side += 2) {
            size_t nr = 0, m = 0;
            for (size_t k = 0; k < sl.n; k++) {
                size_t i = side > 0 ? k : sl.n - 1 - k; /* right side runs top-down clockwise, left side bottom-up */
                if (LO[i] != side) continue;
                run[nr++] = (Run){.s = LY[i], .n = 1, .first = m};
                ord[m++] = i;
                while (nr > 1) { /* a run overlapping the one above joins it */
                    Run *p = &run[nr - 2], *c = &run[nr - 1];
                    if (p->s / (double)p->n + 19.0 * (double)p->n <= c->s / (double)c->n) break;
                    p->s += c->s - 19.0 * (double)p->n * (double)c->n, p->n += c->n, nr--;
                }
            }
            const double rr = R + 30;
            bool column = false;
            for (size_t r = 0; r < nr; r++)
                for (size_t k = 0; k < run[r].n; k++) {
                    size_t i = ord[run[r].first + k];
                    LY[i] = run[r].s / (double)run[r].n + 19.0 * (double)k;
                    column |= fabs(LY[i] - CY) >= rr;
                }
            for (size_t k = 0; k < m; k++) {
                size_t i = ord[k];
                double dy = LY[i] - CY;
                LX[i] = CX + side * (column ? rr : sqrt(rr * rr - dy * dy));
            }
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
    /* title baseline: above every outside label whose text or leader shares the title's columns */
    double ty = -14, thw = title.n ? text_width(title, 25) / 2 + 4 : 0;
    for (size_t i = 0; i < sl.n; i++) {
        if (!LO[i] || LY[i] >= CY) continue;
        double rim = CX + (R + 18) * sin(LM[i]), tip = LX[i] + LO[i] * (4 + LW[i]);
        if (fmin(rim, tip) < CX + thw && fmax(rim, tip) > CX - thw) ty = fmin(ty, LY[i] - 22);
    }
    if (title.n) bb.y0 = fmin(bb.y0, ty - 26);
    bb.y0 = fmin(bb.y0, CY - (double)(sl.n * 22) / 2.0 - 4), bb.y1 = fmax(bb.y1, CY + (double)(sl.n * 22) / 2.0 + 4); /* tall legends */
    if (!buf_reserve(out, 2048 + sl.n * 600)) return MMD_ERR_NOMEM;
    DG_HEAD(out, bb, 8, "pie", CSS);
    if (title.n) dg_text(out, title, 25, CX, ty, "middle", "ttl");
    /* declaration order, clockwise from 12 o'clock */
    double ang = 0;
    char fill[64];
    for (size_t i = 0; i < sl.n; i++) {
        double frac = total > 0 ? sl.v[i].v / total : 0, a1 = ang + frac * 2 * M_PI;
        const char *col = dcfg_pal("pie", i % countof(COLORS) + 1, COLORS[i % countof(COLORS)]);
        if (frac >= 1 - 1e-12) {
            buf_lit(out, "<circle class=\"slice\" cx=\""); buf_num(out, CX); buf_lit(out, "\" cy=\""); buf_num(out, CY);
            buf_lit(out, "\" r=\""); buf_num(out, R); buf_lit(out, "\" fill=\""); buf_cstr(out, col); buf_lit(out, "\"/>");
        } else if (frac > 0) {
            buf_lit(out, "<path class=\"slice\" fill=\""); buf_cstr(out, col); buf_lit(out, "\" d=\"M");
            buf_pt(out, CX, CY); buf_ch(out, 'L'); buf_pt(out, CX + R * sin(ang), CY - R * cos(ang));
            buf_lit(out, "A"); buf_num(out, R); buf_ch(out, ','); buf_num(out, R);
            if (frac > 0.5) buf_lit(out, " 0 1 1 "); else buf_lit(out, " 0 0 1 ");
            buf_pt(out, CX + R * sin(a1), CY - R * cos(a1)); buf_lit(out, "Z\"/>");
        }
        ang = a1;
    }
    for (size_t i = 0; i < sl.n; i++) {
        double mid = LM[i];
        if (!(LW[i] > 0)) continue;
        int len = snprintf(fill, sizeof fill, "%.0f%%", round(sl.v[i].v / total * 100));
        if (!LO[i]) { dg_text(out, (Str){.p = fill, .n = (size_t)len}, 17, LX[i], LY[i], "middle", "pct"); continue; }
        buf_lit(out, "<path class=\"leader\" fill=\"none\" stroke=\"#666\" stroke-width=\"1\" d=\"M");
        buf_pt(out, CX + (R - 4) * sin(mid), CY - (R - 4) * cos(mid)); buf_ch(out, 'L');
        buf_pt(out, CX + (R + 18) * sin(mid), CY - (R + 18) * cos(mid)); buf_ch(out, 'L');
        buf_pt(out, LX[i], LY[i]); buf_lit(out, "\"/>");
        dg_text(out, (Str){.p = fill, .n = (size_t)len}, 17, LX[i] + 4 * LO[i], LY[i], LO[i] > 0 ? "start" : "end", "pct");
    }
    buf_lit(out, "<circle class=\"outer\" cx=\""); buf_num(out, CX); buf_lit(out, "\" cy=\""); buf_num(out, CY);
    buf_lit(out, "\" r=\""); buf_num(out, R + 1); buf_lit(out, "\"/>");
    double ly = CY - (double)(sl.n * 22) / 2.0;
    for (size_t i = 0; i < sl.n; i++, ly += 22) {
        const char *col = dcfg_pal("pie", i % countof(COLORS) + 1, COLORS[i % countof(COLORS)]);
        buf_lit(out, "<rect width=\"18\" height=\"18\" x=\""); buf_num(out, LEGX); buf_lit(out, "\" y=\"");
        buf_num(out, ly); buf_lit(out, "\" fill=\""); buf_cstr(out, col); buf_lit(out, "\" stroke=\"#000\"/>");
        Str lab = sl.v[i].label;
        if (show) {
            Str v = shown_value(sl.v[i].v, val);
            char *t = arena_alloc(a, lab.n + v.n, 1);
            if (!t) return MMD_ERR_NOMEM;
            memcpy(t, lab.p, lab.n);
            memcpy(t + lab.n, v.p, v.n);
            lab = (Str){.p = t, .n = lab.n + v.n};
        }
        dg_text(out, lab, fs, LEGX + 22, ly + 9, "start", nullptr);
    }
    buf_lit(out, "</svg>\n");
    return out->oom ? MMD_ERR_NOMEM : MMD_OK;
}
