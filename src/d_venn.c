/* venn-beta: sets, unions (intersections), indented `text` nodes and `style` lines. Layout is a
 * compact take on venn.js (used by mermaid): circle areas from set sizes (default 10/k^2), pairwise
 * distances solved by bisection on the lens area, positions by stress descent, then the same
 * orient/scale-to-fit; labels go to the point of each region that is deepest inside it. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dgraph.h"
#include "dcfg.h"
#include "diagrams.h"
#include "text.h"

enum { MAXS = 16 };
typedef struct VArea { unsigned mask; Str label; double size; bool has_label; } VArea;
typedef struct VText { unsigned mask; Str id, label; } VText;
typedef struct VStyle { Str fill, fop, stroke, sw, color; } VStyle;
typedef struct VSty { Str key; unsigned mask; VStyle s; } VSty;

static const char *const PAL[8] = {"#5353ff", "#ffff45", "#b5ff20", "#ff53ff", "#53ffff", "#45ff45", "#ff5353", "#45ffff"};
static const char *const PALT[8] = {"#0000b9", "#abab00", "#598600", "#b900b9", "#00b9b9", "#00ab00", "#b90000", "#00abab"};

static constexpr char CSS[] = "#mermaid .vt{fill:#333;}#mermaid .vl{fill:#333;}#mermaid .vn{fill:#333;}";

static Str unq(Str s) {
    s = str_trim(s);
    if (s.n >= 2 && s.p[0] == '"' && s.p[s.n - 1] == '"') s.p++, s.n -= 2;
    return s;
}

static double lens(double r1, double r2, double d) { /* area of intersection of two circles */
    if (d >= r1 + r2) return 0;
    if (d <= fabs(r1 - r2)) { double r = fmin(r1, r2); return M_PI * r * r; }
    double a = r1 * r1 * acos((d * d + r1 * r1 - r2 * r2) / (2 * d * r1));
    double b = r2 * r2 * acos((d * d + r2 * r2 - r1 * r1) / (2 * d * r2));
    return a + b - 0.5 * sqrt(fmax(0, (-d + r1 + r2) * (d + r1 - r2) * (d - r1 + r2) * (d + r1 + r2)));
}

static double dist_for(double r1, double r2, double area) {
    if (area <= 0) return r1 + r2;
    if (area >= M_PI * fmin(r1, r2) * fmin(r1, r2) - 1e-12) return fabs(r1 - r2);
    double lo = fabs(r1 - r2), hi = r1 + r2;
    for (int i = 0; i < 60; i++) {
        double m = (lo + hi) / 2;
        if (lens(r1, r2, m) > area) lo = m; else hi = m;
    }
    return (lo + hi) / 2;
}

/* signed depth of p inside region `mask` (inside all members, outside every other set) */
static double depth(size_t n, const double X[static n], const double Y[static n], const double R[static n],
                    unsigned mask, double x, double y) {
    double m = 1e300;
    for (size_t i = 0; i < n; i++) {
        double d = hypot(x - X[i], y - Y[i]);
        m = fmin(m, mask >> i & 1 ? R[i] - d : d - R[i]);
    }
    return m;
}

static int set_idx(size_t ns, const Str *ids, Str id) {
    for (size_t i = 0; i < ns; i++) if (str_eq(ids[i], id)) return (int)i;
    return -1;
}

static bool safe_val(Str v) {
    if (!v.n || v.n > 40) return false;
    for (size_t i = 0; i < v.n; i++) {
        char c = v.p[i];
        if (!(c == '#' || c == '.' || c == ',' || c == '(' || c == ')' || c == '%' || c == ' ' || c == '-' ||
              (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return false;
    }
    return true;
}

static void put_s(Buf *b, Str s) { buf_put(b, s.n, s.p); }

/* darken a #rgb/#rrggbb colour by 30 lightness points (khroma darken), else nullptr */
static bool darken30(Str c, char out[static 8]) {
    unsigned v[3];
    if (c.n == 7 && c.p[0] == '#') {
        for (int i = 0; i < 3; i++) {
            char h[3] = {c.p[1 + 2 * i], c.p[2 + 2 * i], 0};
            char *e;
            v[i] = (unsigned)strtoul(h, &e, 16);
            if (*e) return false;
        }
    } else if (c.n == 4 && c.p[0] == '#') {
        for (int i = 0; i < 3; i++) {
            char h[2] = {c.p[1 + i], 0};
            char *e;
            v[i] = (unsigned)strtoul(h, &e, 16) * 17;
            if (*e) return false;
        }
    } else return false;
    double r = v[0] / 255.0, g = v[1] / 255.0, bl = v[2] / 255.0, mx = fmax(r, fmax(g, bl)), mn = fmin(r, fmin(g, bl));
    double l = (mx + mn) / 2, s = 0, h = 0, d = mx - mn;
    if (d > 0) {
        s = l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
        h = mx == r ? fmod((g - bl) / d + 6, 6) : mx == g ? (bl - r) / d + 2 : (r - g) / d + 4;
        h /= 6;
    }
    l = fmax(0, l - 0.3);
    double q = l < 0.5 ? l * (1 + s) : l + s - l * s, p = 2 * l - q, o[3];
    for (int i = 0; i < 3; i++) {
        double t = h + (1 - i) / 3.0;
        t -= floor(t);
        o[i] = t < 1 / 6.0 ? p + (q - p) * 6 * t : t < 0.5 ? q : t < 2 / 3.0 ? p + (q - p) * (2 / 3.0 - t) * 6 : p;
    }
    snprintf(out, 8, "#%02x%02x%02x", (unsigned)lround(o[0] * 255), (unsigned)lround(o[1] * 255), (unsigned)lround(o[2] * 255));
    return true;
}

static Str nl_next(Str t, size_t *i) { /* next '\n'-separated line from *i; *i past the end when done */
    size_t s = *i, e = s;
    while (e < t.n && t.p[e] != '\n') e++;
    *i = e + 1;
    return (Str){.p = t.p + s, .n = e - s};
}

DIAG_FN(venn_render) {
    (void)fs;
    LineIt it = {.s = src, .n = n, .i = off};
    Str ln;
    size_t lo;
    if (!dg_next_line(&it, &ln, &lo)) return diag_fail(err_off, msg, off, "empty venn");
    Str ids[MAXS];
    size_t ns = 0;
    VEC(VArea) ar = {};
    VEC(VText) tx = {};
    VEC(VSty) st = {};
    unsigned cur = 0;
    while (dg_next_line(&it, &ln, &lo)) {
        Str r = ln, w = dg_word(&r);
        if (str_eq(w, S("title"))) { title = unq(r); continue; }
        bool block;
        if (dcfg_is_acc(ln, &block)) continue;
        bool is_set = str_eq(w, S("set")), is_un = str_eq(w, S("union")), is_tx = str_eq(w, S("text"));
        if (str_eq(w, S("style"))) {
            r = str_trim(r);
            size_t k = 0;
            while (k < r.n && r.p[k] != ' ' && r.p[k] != '\t') k++;
            Str targ = {.p = r.p, .n = k}, rest = str_trim((Str){.p = r.p + k, .n = r.n - k});
            VSty s = {.key = targ};
            for (size_t a0 = 0; a0 < targ.n;) {
                size_t b0 = a0;
                while (b0 < targ.n && targ.p[b0] != ',') b0++;
                int si = set_idx(ns, ids, unq((Str){.p = targ.p + a0, .n = b0 - a0}));
                if (si >= 0) s.mask |= 1u << si;
                else s.mask |= 1u << 31;
                a0 = b0 + 1;
            }
            for (size_t a0 = 0; a0 < rest.n;) {
                size_t b0 = a0;
                int par = 0;
                while (b0 < rest.n && (rest.p[b0] != ',' || par)) par += rest.p[b0] == '(' ? 1 : rest.p[b0] == ')' ? -1 : 0, b0++;
                Str f = {.p = rest.p + a0, .n = b0 - a0};
                const char *c = memchr(f.p, ':', f.n);
                if (c) {
                    Str k2 = str_trim((Str){.p = f.p, .n = (size_t)(c - f.p)}), v = unq((Str){.p = c + 1, .n = f.n - (size_t)(c + 1 - f.p)});
                    if (safe_val(v)) {
                        if (str_eq(k2, S("fill"))) s.s.fill = v;
                        else if (str_eq(k2, S("fill-opacity"))) s.s.fop = v;
                        else if (str_eq(k2, S("stroke"))) s.s.stroke = v;
                        else if (str_eq(k2, S("stroke-width"))) s.s.sw = v;
                        else if (str_eq(k2, S("color"))) s.s.color = v;
                    }
                }
                a0 = b0 + 1;
            }
            if (!VEC_PUSH(a, &st, s)) return CMM_ERR_NOMEM;
            continue;
        }
        if (!is_set && !is_un && !is_tx) return diag_fail(err_off, msg, lo, "expected set, union, text or style");
        r = str_trim(r);
        /* identifier list, then optional ["label"] / [label], then optional :number */
        size_t k = 0;
        bool q = false;
        while (k < r.n && (q || (r.p[k] != '[' && r.p[k] != ':'))) q ^= r.p[k] == '"', k++;
        Str idl = str_trim((Str){.p = r.p, .n = k}), lab = {}, rest = {.p = r.p + k, .n = r.n - k};
        bool has_label = false;
        if (rest.n && rest.p[0] == '[') {
            const char *e = memchr(rest.p, ']', rest.n);
            if (!e) return diag_fail(err_off, msg, lo, "unclosed [");
            lab = unq((Str){.p = rest.p + 1, .n = (size_t)(e - rest.p - 1)}), has_label = true;
            rest = str_trim((Str){.p = e + 1, .n = rest.n - (size_t)(e + 1 - rest.p)});
        }
        double size = NAN;
        if (rest.n) {
            if (rest.p[0] != ':' || !dg_parse_num(str_trim((Str){.p = rest.p + 1, .n = rest.n - 1}), &size) || !(size >= 0) || size > 1e12)
                return diag_fail(err_off, msg, lo, "bad size");
        }
        if (is_tx) {
            if (!cur) return diag_fail(err_off, msg, lo, "text requires a set");
            Str id = unq(idl);
            if (!id.n) return diag_fail(err_off, msg, lo, "text needs an identifier");
            if (!VEC_PUSH(a, &tx, ((VText){cur, id, has_label ? lab : id}))) return CMM_ERR_NOMEM;
            continue;
        }
        unsigned mask = 0, cnt = 0;
        for (size_t a0 = 0; a0 < idl.n;) {
            size_t b0 = a0;
            while (b0 < idl.n && idl.p[b0] != ',') b0++;
            Str id = unq((Str){.p = idl.p + a0, .n = b0 - a0});
            a0 = b0 + 1;
            if (!id.n) return diag_fail(err_off, msg, lo, "empty identifier");
            int si = set_idx(ns, ids, id);
            if (is_set) {
                if (si < 0) {
                    if (ns >= MAXS) return diag_fail(err_off, msg, lo, "too many sets");
                    ids[ns] = id, si = (int)ns++;
                }
            } else if (si < 0) return diag_fail(err_off, msg, lo, "union references an unknown set");
            mask |= 1u << si, cnt++;
        }
        if (is_set ? cnt != 1 : cnt < 2) return diag_fail(err_off, msg, lo, is_set ? "set takes one identifier" : "union requires multiple identifiers");
        cnt = (unsigned)__builtin_popcount(mask);
        if (isnan(size)) size = 10.0 / (cnt * cnt);
        Str l = has_label ? lab : is_set ? ids[__builtin_ctz(mask)] : (Str){};
        bool okn;
        l = label_normalize(a, l, &okn);
        if (!okn) return CMM_ERR_NOMEM;
        size_t j = 0;
        while (j < ar.n && ar.v[j].mask != mask) j++;
        if (j == ar.n && ar.n >= 256) return diag_fail(err_off, msg, lo, "too many areas");
        if (j == ar.n) { if (!VEC_PUSH(a, &ar, ((VArea){mask, l, size, has_label || is_set}))) return CMM_ERR_NOMEM; }
        else ar.v[j] = (VArea){mask, l, size, has_label || is_set};
        cur = mask;
    }
    if (!ns) return diag_fail(err_off, msg, off, "venn needs at least one set");
    /* ensurePairwiseSubsets: synthetic pairs inside 3+-way unions */
    double ov[MAXS][MAXS], ssz[MAXS] = {};
    bool has_ov[MAXS][MAXS] = {};
    for (size_t i = 0; i < ar.n; i++) if (__builtin_popcount(ar.v[i].mask) == 1) ssz[__builtin_ctz(ar.v[i].mask)] = ar.v[i].size;
    for (size_t i = 0; i < ar.n; i++) {
        unsigned m = ar.v[i].mask;
        if (__builtin_popcount(m) == 2) {
            int x = __builtin_ctz(m), y = 31 - __builtin_clz(m);
            ov[x][y] = ov[y][x] = ar.v[i].size, has_ov[x][y] = has_ov[y][x] = true;
        }
    }
    for (size_t i = 0; i < ar.n; i++) {
        unsigned m = ar.v[i].mask;
        if (__builtin_popcount(m) < 3) continue;
        for (size_t x = 0; x < ns; x++) for (size_t y = x + 1; y < ns; y++)
            if ((m >> x & 1) && (m >> y & 1) && !has_ov[x][y]) ov[x][y] = ov[y][x] = fmin(ssz[x], ssz[y]) / 4, has_ov[x][y] = has_ov[y][x] = true;
    }
    double R[MAXS], X[MAXS], Y[MAXS], T[MAXS][MAXS];
    for (size_t i = 0; i < ns; i++) R[i] = sqrt(fmax(ssz[i], 1e-9) / M_PI);
    for (size_t i = 0; i < ns; i++) for (size_t j = 0; j < ns; j++)
        T[i][j] = i == j ? 0 : dist_for(R[i], R[j], has_ov[i][j] ? fmin(ov[i][j], M_PI * fmin(R[i], R[j]) * fmin(R[i], R[j])) : 0);
    double mr = 0;
    for (size_t i = 0; i < ns; i++) mr = fmax(mr, R[i]);
    for (size_t i = 0; i < ns; i++) X[i] = 2 * mr * cos(2 * M_PI * (double)i / (double)ns), Y[i] = 2 * mr * sin(2 * M_PI * (double)i / (double)ns);
    for (int itn = 0; itn < 3000 && ns > 1; itn++) {
        double gx[MAXS] = {}, gy[MAXS] = {}, step = 0.1 * (1 - itn / 3000.0) + 0.005;
        for (size_t i = 0; i < ns; i++) for (size_t j = 0; j < ns; j++) {
            if (i == j) continue;
            double dx = X[i] - X[j], dy = Y[i] - Y[j], d = hypot(dx, dy);
            if (d < 1e-9) dx = 1e-3 * ((double)i - (double)j), dy = 1e-3, d = hypot(dx, dy);
            double e = d - T[i][j];
            if (!has_ov[i][j] && e > 0) continue;
            gx[i] -= e * dx / d, gy[i] -= e * dy / d;
        }
        for (size_t i = 0; i < ns; i++) X[i] += step * gx[i], Y[i] += step * gy[i];
    }
    /* orient: first set left of second on a horizontal line, third set above */
    if (ns > 1) {
        double ang = atan2(Y[1] - Y[0], X[1] - X[0]), c = cos(-ang), s = sin(-ang), x0 = X[0], y0 = Y[0];
        for (size_t i = 0; i < ns; i++) {
            double dx = X[i] - x0, dy = Y[i] - y0;
            X[i] = dx * c - dy * s, Y[i] = dx * s + dy * c;
        }
        if (ns > 2 && Y[2] > 0) for (size_t i = 0; i < ns; i++) Y[i] = -Y[i];
    }
    double W = fmin(fmax(dcfg_num("venn.width", 800), 100), 10000), H = fmin(fmax(dcfg_num("venn.height", 450), 100), 10000);
    double pad = fmin(fmax(dcfg_num("venn.padding", 15), 0), 1000), sc = W / 1600, th = title.n ? 48 * sc : 0, AH = H - th;
    double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
    for (size_t i = 0; i < ns; i++) x0 = fmin(x0, X[i] - R[i]), x1 = fmax(x1, X[i] + R[i]), y0 = fmin(y0, Y[i] - R[i]), y1 = fmax(y1, Y[i] + R[i]);
    double k = fmin((W - 2 * pad) / (x1 - x0), (AH - 2 * pad) / (y1 - y0));
    k = fmax(k, 1e-9);
    double ox = (W - (x1 - x0) * k) / 2, oy = (AH - (y1 - y0) * k) / 2;
    for (size_t i = 0; i < ns; i++) X[i] = (X[i] - x0) * k + ox, Y[i] = (Y[i] - y0) * k + oy + th, R[i] *= k;
    /* label centres: deepest point of each region (grid search then refinement) */
    double *LX = ANEW(a, double, ar.n + 1), *LY = ANEW(a, double, ar.n + 1), *LD = ANEW(a, double, ar.n + 1);
    if (!LX || !LY || !LD) return CMM_ERR_NOMEM;
    for (size_t i = 0; i < ar.n; i++) {
        unsigned m = ar.v[i].mask;
        double bx = 0, by = 0, bd = -1e300;
        for (size_t j = 0; j < ns; j++) {
            if (!(m >> j & 1)) continue;
            for (int gy = 0; gy <= 40; gy++) for (int gx = 0; gx <= 40; gx++) {
                double px = X[j] - R[j] + 2 * R[j] * gx / 40, py = Y[j] - R[j] + 2 * R[j] * gy / 40, d = depth(ns, X, Y, R, m, px, py);
                if (d > bd) bd = d, bx = px, by = py;
            }
        }
        for (double stp = R[0] / 20; stp > 0.05; stp /= 2)
            for (int rep = 0; rep < 8; rep++) {
                bool moved = false;
                for (int dxi = -1; dxi <= 1; dxi++) for (int dyi = -1; dyi <= 1; dyi++) {
                    double d = depth(ns, X, Y, R, m, bx + dxi * stp, by + dyi * stp);
                    if (d > bd + 1e-12) bd = d, bx += dxi * stp, by += dyi * stp, moved = true;
                }
                if (!moved) break;
            }
        LX[i] = bx, LY[i] = by, LD[i] = bd;
    }
    /* deviate from mermaid: a label wider than its region wraps (and shrinks if one word is still too
       wide); labels whose boxes still collide re-wrap narrower, the wider first */
    Str *LB = ANEW(a, Str, ar.n + 1);
    double *LF = ANEW(a, double, ar.n + 1), *LW = ANEW(a, double, ar.n + 1);
    unsigned *LN = ANEW(a, unsigned, ar.n + 1);
    if (!LB || !LF || !LW || !LN) return CMM_ERR_NOMEM;
    for (size_t i = 0; i < ar.n; i++) LB[i] = ar.v[i].label, LF[i] = 48 * sc, LN[i] = 1, LW[i] = 0;
    for (int pass = 0; pass < 4; pass++) {
        bool again = false;
        for (size_t i = 0; i < ar.n; i++) {
            if (!ar.v[i].label.n || LD[i] <= -1e299) continue;
            double avail = pass ? LW[i] : fmax(2 * LD[i] * 1.9, 60 * sc);
            if (pass) {
                bool hit = false;
                for (size_t j = 0; j < ar.n && !hit; j++) {
                    if (j == i || !ar.v[j].label.n || LD[j] <= -1e299 || LW[j] > LW[i]) continue;
                    hit = fabs(LX[i] - LX[j]) < (LW[i] + LW[j]) / 2 && fabs(LY[i] - LY[j]) < 0.6 * (LN[i] * LF[i] + LN[j] * LF[j]);
                }
                if (!hit || LN[i] > 3) continue;
                avail = LW[i] * 0.6, again = true;
            }
            double fz = 48 * sc;
            Str lb = ar.v[i].label;
            if (text_width(lb, fz) > avail) {
                Str wl = dg_wrap(a, lb, fz, avail);
                if (wl.p) lb = wl;
            }
            unsigned nl = 0;
            double mw = 0;
            for (size_t k = 0; k <= lb.n; nl++) mw = fmax(mw, text_width(nl_next(lb, &k), fz));
            if (mw > avail && !pass) fz *= fmax(avail / mw, 0.5), mw *= fmax(avail / mw, 0.5);
            LB[i] = lb, LF[i] = fz, LN[i] = nl, LW[i] = mw;
        }
        if (pass && !again) break;
    }
    Buf b = {};
    BBox bb = {0, 0, W, H};
    if (title.n) {
        buf_lit(&b, "<text class=\"vt\" font-size=\"32px\" text-anchor=\"middle\" dominant-baseline=\"middle\" x=\"");
        buf_num(&b, W / 2); buf_lit(&b, "\" y=\""); buf_num(&b, fmax(32 * sc, 20)); /* 32px glyphs: keep the cap height inside the viewBox */
        buf_lit(&b, "\">"); buf_xml(&b, title); buf_lit(&b, "</text>");
    }
    const VStyle none = {};
    for (size_t pass = 0; pass < 2; pass++)
        for (size_t i = 0; i < ar.n; i++) {
            unsigned m = ar.v[i].mask;
            bool single = __builtin_popcount(m) == 1;
            if (single != (pass == 0)) continue;
            const VStyle *cs = &none;
            for (size_t s = 0; s < st.n; s++) if (st.v[s].mask == m) cs = &st.v[s].s;
            int si = __builtin_ctz(m);
            size_t ci = single ? (size_t)si : 0;
            char tcol[8];
            const char *pal = dcfg_pal("venn", ci % 8 + 1, PAL[ci % 8]);
            Str base = cs->fill.n ? cs->fill : (Str){.p = pal, .n = strlen(pal)};
            Str txt = cs->color;
            if (!txt.n && single) {
                if (!cs->fill.n && !strcmp(pal, PAL[ci % 8])) txt = (Str){.p = PALT[ci % 8], .n = 7};
                else if (darken30(base, tcol)) txt = (Str){.p = tcol, .n = 7};
            }
            buf_lit(&b, "<g class=\"venn-area "); if (single) buf_lit(&b, "venn-circle\">"); else buf_lit(&b, "venn-intersection\">");
            if (single) {
                buf_lit(&b, "<circle cx=\""); buf_num(&b, X[si]); buf_lit(&b, "\" cy=\""); buf_num(&b, Y[si]); buf_lit(&b, "\" r=\""); buf_num(&b, R[si]);
                buf_lit(&b, "\" style=\"fill:"); put_s(&b, base); buf_lit(&b, ";fill-opacity:"); if (cs->fop.n) put_s(&b, cs->fop); else buf_lit(&b, "0.1");
                buf_lit(&b, ";stroke:"); put_s(&b, cs->stroke.n ? cs->stroke : base); buf_lit(&b, ";stroke-width:");
                if (cs->sw.n) put_s(&b, cs->sw); else buf_num(&b, 5 * sc);
                buf_lit(&b, ";stroke-opacity:0.95\"/>");
            } else if (cs->fill.n) {
                int depthc = 0;
                for (size_t j = 0; j < ns; j++) if (m >> j & 1) {
                    buf_lit(&b, "<clipPath id=\"vc"); buf_num(&b, (double)i); buf_ch(&b, '_'); buf_num(&b, (double)j); buf_lit(&b, "\"><circle cx=\"");
                    buf_num(&b, X[j]); buf_lit(&b, "\" cy=\""); buf_num(&b, Y[j]); buf_lit(&b, "\" r=\""); buf_num(&b, R[j]); buf_lit(&b, "\"/></clipPath>");
                }
                for (size_t j = 0; j < ns; j++) if (m >> j & 1) {
                    buf_lit(&b, "<g clip-path=\"url(#vc"); buf_num(&b, (double)i); buf_ch(&b, '_'); buf_num(&b, (double)j); buf_lit(&b, ")\">"); depthc++;
                }
                buf_lit(&b, "<rect x=\"0\" y=\"0\" width=\""); buf_num(&b, W); buf_lit(&b, "\" height=\""); buf_num(&b, H);
                buf_lit(&b, "\" style=\"fill:"); put_s(&b, cs->fill); buf_lit(&b, "\"/>");
                while (depthc--) buf_lit(&b, "</g>");
            }
            if (ar.v[i].label.n && LD[i] > -1e299) {
                double fz = LF[i];
                Str lb = LB[i];
                unsigned nl = LN[i];
                buf_lit(&b, "<text class=\"vl\" text-anchor=\"middle\"");
                if (nl == 1) buf_lit(&b, " dy=\".35em\"");
                buf_lit(&b, " x=\""); buf_num(&b, LX[i]); buf_lit(&b, "\" y=\""); buf_num(&b, LY[i]);
                buf_lit(&b, "\" style=\"font-size:"); buf_num(&b, fz); buf_lit(&b, "px");
                if (txt.n) { buf_lit(&b, ";fill:"); put_s(&b, txt); }
                buf_ch(&b, '"');
                if (nl == 1) { buf_ch(&b, '>'); buf_xml(&b, lb); }
                else {
                    buf_ch(&b, '>');
                    unsigned li = 0;
                    for (size_t k = 0; k <= lb.n; li++) {
                        Str ln = nl_next(lb, &k);
                        buf_lit(&b, "<tspan x=\""); buf_num(&b, LX[i]); buf_lit(&b, "\" dy=\"");
                        buf_num(&b, li ? 1.1 : 0.35 - 0.55 * (nl - 1)); buf_lit(&b, "em\">"); buf_xml(&b, ln); buf_lit(&b, "</tspan>");
                    }
                }
                buf_lit(&b, "</text>");
            }
            buf_lit(&b, "</g>");
        }
    /* text nodes: grid inside the region's inscribed circle, below the area label */
    for (size_t i = 0; i < ar.n; i++) {
        unsigned m = ar.v[i].mask;
        size_t cnt = 0;
        for (size_t t = 0; t < tx.n; t++) cnt += tx.v[t].mask == m;
        if (!cnt) continue;
        double inr = fmax(0, LD[i]);
        if (inr == 0) { double mn = 1e300; for (size_t j = 0; j < ns; j++) if (m >> j & 1) mn = fmin(mn, R[j]); inr = mn * 0.6; }
        double iw = fmax(80 * sc, inr * 1.9), ih = fmax(60 * sc, inr * 1.9);
        double lo2 = (ar.v[i].label.n ? fmin(32 * sc, inr * 0.25) : 0) + (cnt <= 2 ? 30 * sc : 0);
        double sx = LX[i] - iw / 2, sy = LY[i] - ih / 2 + lo2;
        size_t cols = (size_t)ceil(sqrt((double)cnt)), rows = (cnt + cols - 1) / cols, idx = 0;
        double cw = iw / (double)cols, chh = ih / (double)rows;
        if (ar.v[i].label.n && LD[i] > -1e299) { /* keep the first row (wrapped, so maybe taller) below the area label */
            double need = 0;
            for (size_t t = 0, k = 0; t < tx.n && k < cols; t++) {
                if (tx.v[t].mask != m) continue;
                Str wl = dg_wrap(a, tx.v[t].label, 40 * sc, cw * 0.9);
                if (!wl.p) wl = tx.v[t].label;
                double nl = 1;
                for (size_t q = 0; q < wl.n; q++) nl += wl.p[q] == '\n';
                double cy = sy + chh * 0.5;
                need = fmax(need, LY[i] + 28 * sc + nl * 24 * sc - cy);
                k++;
            }
            sy += need;
        }
        buf_lit(&b, "<g class=\"venn-text-nodes\" font-size=\""); buf_num(&b, 40 * sc); buf_lit(&b, "px\">");
        for (size_t t = 0; t < tx.n; t++) {
            if (tx.v[t].mask != m) continue;
            double cx = sx + cw * ((double)(idx % cols) + 0.5), cy = sy + chh * ((double)(idx / cols) + 0.5);
            idx++;
            Str col = {};
            for (size_t s = 0; s < st.n; s++) if (str_eq(st.v[s].key, tx.v[t].id) && st.v[s].s.color.n) col = st.v[s].s.color;
            Str wl = dg_wrap(a, tx.v[t].label, 40 * sc, cw * 0.9);
            if (!wl.p) wl = tx.v[t].label;
            char cls[64] = "vn";
            if (col.n) {
                buf_lit(&b, "<g style=\"fill:"); put_s(&b, col); buf_lit(&b, "\">");
            }
            BBox tb = BB_EMPTY;
            dg_tspans(&b, &tb, wl, 40 * sc, 40 * sc * 1.2, cx, cy, true, "middle", TB_MIDDLE, col.n ? nullptr : cls);
            if (col.n) buf_lit(&b, "</g>");
            if (tb.x1 > tb.x0) bb_add(&bb, tb.x0, tb.y0, tb.x1, tb.y1);
        }
        buf_lit(&b, "</g>");
    }
    return DG_FINISH(out, &b, bb, 0, "venn", CSS);
}
