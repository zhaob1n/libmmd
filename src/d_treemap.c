/* treemap(-beta): indentation tree of `"Section"` and `"Leaf": value` lines (quotes optional),
 * `:::class` suffixes and `classDef name fill:..,stroke:..,color:..`. Parent values are the sum of
 * their children (an explicit parent value is used only when it has no children). Layout is the
 * squarified algorithm (Bruls et al.) inside each section, with a header band for section names. */
#include <math.h>
#include <stdio.h>

#include "dgraph.h"
#include "diagrams.h"
#include "text.h"

typedef struct TNode {
    Str label, cls;
    double v, x, y, w, h;
    uint32_t parent, first, next, depth, sec;
} TNode;
typedef struct TCls { Str name, fill, stroke, color; } TCls;

static constexpr char CSS[] =
    "#mermaid .sh{font-size:12px;font-weight:bold;fill:#333;}#mermaid .lf{fill-opacity:.3;stroke-width:3px;}"
    "#mermaid .sv{font-size:10px;font-style:italic;fill:#333;}"
    "#mermaid .sec{fill-opacity:.6;stroke-width:2px;stroke-opacity:.4;}#mermaid .ttl{font-size:18px;fill:#333;}";
/* mermaid 11 default cScale (hsl(h,100%,~76%)): sections fill .6 opacity, leaves .3 with a 3px same-colour border */
static const char *const SEC[] = {"#8686ff", "#ffff78", "#d7ff86", "#86c2ff", "#86ffff", "#86ffc2", "#ff86ff", "#ff8686"};
static const char *const SECD[] = {"#3939ff", "#ffff2c", "#b5ff20", "#399cff", "#39ffff", "#39ff9c", "#ff39ff", "#ff3939"};
static const char *const SECT[] = {"#fff", "#000", "#000", "#000", "#000", "#000", "#000", "#000"};

static double worst(double s, double mn, double mx, double side) {
    double s2 = s * s, w2 = side * side;
    return fmax(w2 * mx / s2, s2 / (w2 * mn));
}
/* squarify children of p into rect (x,y,w,h); kids sorted by value descending in idx[0..k) */
static void squarify(TNode *V, uint32_t *idx, size_t k, double x, double y, double w, double h) {
    double total = 0;
    for (size_t i = 0; i < k; i++) total += V[idx[i]].v;
    if (total <= 0 || w <= 0 || h <= 0) {
        for (size_t i = 0; i < k; i++) V[idx[i]].x = x, V[idx[i]].y = y, V[idx[i]].w = V[idx[i]].h = 0;
        return;
    }
    double scale = w * h / total;
    size_t i = 0;
    while (i < k) {
        double side = fmin(w, h), s = 0, mn = INFINITY, mx = 0, best = INFINITY;
        size_t j = i;
        for (; j < k; j++) {
            double a = V[idx[j]].v * scale;
            double s2 = s + a, mn2 = fmin(mn, a), mx2 = fmax(mx, a), wr = worst(s2, fmax(mn2, 1e-9), mx2, side);
            if (j > i && wr > best) break;
            s = s2, mn = mn2, mx = mx2, best = wr;
        }
        /* lay row i..j along the short side */
        double thick = s / side, off = 0;
        for (size_t q = i; q < j; q++) {
            double len = V[idx[q]].v * scale / fmax(thick, 1e-9);
            TNode *t = &V[idx[q]];
            if (w >= h) t->x = x, t->y = y + off, t->w = thick, t->h = len;
            else t->x = x + off, t->y = y, t->w = len, t->h = thick;
            off += len;
        }
        if (w >= h) x += thick, w -= thick; else y += thick, h -= thick;
        i = j;
    }
}
static int by_val_desc(const uint32_t *a, const uint32_t *b, const TNode *V) {
    double x = V[*a].v, y = V[*b].v;
    return (x < y) - (x > y);
}
static void sort_idx(TNode *V, uint32_t *idx, size_t k) { /* insertion sort: stable, tiny lists */
    for (size_t i = 1; i < k; i++) {
        uint32_t t = idx[i];
        size_t j = i;
        while (j && by_val_desc(&idx[j - 1], &t, V) > 0) idx[j] = idx[j - 1], j--;
        idx[j] = t;
    }
}
constexpr double HDR = 25;
constexpr double PAD = 6;
static void layout(TNode *V, uint32_t *scratch, uint32_t p) {
    size_t k = 0;
    for (uint32_t c = V[p].first; c != UINT32_MAX; c = V[c].next) scratch[k++] = c;
    if (!k) return;
    sort_idx(V, scratch, k);
    double x = V[p].x + PAD, y = V[p].y + (p ? HDR : PAD), w = V[p].w - 2 * PAD, h = V[p].h - (p ? HDR : PAD) - PAD;
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    squarify(V, scratch, k, x, y, w, h);
    for (uint32_t c = V[p].first; c != UINT32_MAX; c = V[c].next) layout(V, scratch + k, c);
}

static Str unq(Str s) {
    s = str_trim(s);
    if (s.n >= 2 && (s.p[0] == '"' || s.p[0] == '\'') && s.p[s.n - 1] == s.p[0]) s = (Str){.p = s.p + 1, .n = s.n - 2};
    return s;
}
static void fmt_val(double v, size_t cap, char b[static cap], size_t *len) {
    int k = fabs(v - round(v)) < 1e-9 ? snprintf(b, cap, "%.0f", v) : snprintf(b, cap, "%.2f", v);
    *len = k < 0 ? 0 : (size_t)k < cap ? (size_t)k : cap - 1;
}

DIAG_FN(treemap_render) {
    (void)fs;
    LineIt it = {.s = src, .n = n, .i = off};
    Str line;
    size_t lo;
    if (!dg_next_line(&it, &line, &lo)) return diag_fail(err_off, msg, off, "empty treemap");
    VEC(TNode) v = {};
    VEC(uint32_t) ind = {};
    VEC(TCls) cls = {};
    /* virtual root holds all top-level nodes */
    if (!VEC_PUSH(a, &v, ((TNode){.parent = UINT32_MAX, .first = UINT32_MAX, .next = UINT32_MAX})) || !VEC_PUSH(a, &ind, 0))
        return CMM_ERR_NOMEM;
    while (dg_next_line(&it, &line, &lo)) {
        Str rest = line, w = dg_word(&rest);
        if (str_eq(w, S("title"))) { title = str_trim(rest); continue; }
        if (str_starts(w, S("acc"))) continue;
        if (str_eq(w, S("classDef"))) {
            TCls c = {.name = dg_word(&rest)};
            for (Str r = str_trim(rest); r.n;) {
                const char *cm = memchr(r.p, ',', r.n);
                size_t L = cm ? (size_t)(cm - r.p) : r.n;
                Str kv = str_trim((Str){.p = r.p, .n = L});
                r = cm ? (Str){.p = cm + 1, .n = r.n - L - 1} : (Str){};
                if (kv.n && kv.p[kv.n - 1] == ';') kv.n--;
                const char *col = memchr(kv.p, ':', kv.n);
                if (!col) continue;
                Str key = str_trim((Str){.p = kv.p, .n = (size_t)(col - kv.p)}), val = str_trim((Str){.p = col + 1, .n = kv.n - (size_t)(col + 1 - kv.p)});
                if (str_eq(key, S("fill"))) c.fill = val;
                else if (str_eq(key, S("stroke"))) c.stroke = val;
                else if (str_eq(key, S("color"))) c.color = val;
            }
            if (!VEC_PUSH(a, &cls, c)) return CMM_ERR_NOMEM;
            continue;
        }
        unsigned in = dg_indent(&it, lo) + 1;
        TNode t = {.first = UINT32_MAX, .next = UINT32_MAX, .v = NAN};
        Str s = line;
        for (size_t i = 0; i + 3 <= s.n; i++)
            if (!memcmp(s.p + i, ":::", 3)) { t.cls = str_trim((Str){.p = s.p + i + 3, .n = s.n - i - 3}); s = str_trim((Str){.p = s.p, .n = i}); break; }
        /* label: quoted string, or up to the last ':' */
        size_t ce = SIZE_MAX;
        if (s.n && (s.p[0] == '"' || s.p[0] == '\'')) {
            const char *q = memchr(s.p + 1, s.p[0], s.n - 1);
            if (!q) return diag_fail(err_off, msg, lo, "unterminated string");
            t.label = (Str){.p = s.p + 1, .n = (size_t)(q - s.p) - 1};
            Str r = str_trim((Str){.p = q + 1, .n = s.n - (size_t)(q + 1 - s.p)});
            if (r.n && r.p[0] == ':') ce = (size_t)(r.p - s.p);
            else if (r.n) return diag_fail(err_off, msg, (size_t)(r.p - src), "expected ':' value");
        } else {
            for (size_t i = s.n; i-- > 0;) if (s.p[i] == ':') { ce = i; break; }
            t.label = unq((Str){.p = s.p, .n = ce == SIZE_MAX ? s.n : ce});
        }
        if (ce != SIZE_MAX) {
            Str vs = str_trim((Str){.p = s.p + ce + 1, .n = s.n - ce - 1});
            if (!dg_parse_num(vs, &t.v) || !(t.v >= 0)) return diag_fail(err_off, msg, (size_t)(vs.p - src), "expected a non-negative value");
        }
        if (!t.label.n) return diag_fail(err_off, msg, lo, "empty treemap label");
        uint32_t p = (uint32_t)v.n - 1;
        while (p && ind.v[p] >= in) p = v.v[p].parent;
        t.parent = p, t.depth = v.v[p].depth + 1;
        if (!VEC_PUSH(a, &v, t) || !VEC_PUSH(a, &ind, in)) return CMM_ERR_NOMEM;
        uint32_t me = (uint32_t)v.n - 1, *pp = &v.v[p].first;
        while (*pp != UINT32_MAX) pp = &v.v[*pp].next;
        *pp = me;
    }
    if (v.n < 2) return diag_fail(err_off, msg, off, "treemap has no nodes");
    TNode *V = v.v;
    for (size_t i = v.n; i-- > 0;) { /* sums bottom-up (children follow parents) */
        if (V[i].first != UINT32_MAX) {
            double s = 0;
            for (uint32_t c = V[i].first; c != UINT32_MAX; c = V[c].next) s += V[c].v;
            V[i].v = s;
        } else if (isnan(V[i].v)) V[i].v = 0;
    }
    uint32_t sc = 0;
    for (size_t i = 1; i < v.n; i++) V[i].sec = V[i].depth == 1 ? sc++ : V[V[i].parent].sec;
    double top = title.n ? 40 : 0;
    constexpr double W = 960;
    constexpr double H = 500;
    V[0].x = 0, V[0].y = top - PAD, V[0].w = W, V[0].h = H + PAD; /* root: no header */
    uint32_t *scratch = ANEW(a, uint32_t, v.n + 1);
    if (!scratch) return CMM_ERR_NOMEM;
    layout(V, scratch, 0);
    BBox bb = {0, 0, W, top + H};
    if (!buf_reserve(out, 2048 + v.n * 500)) return CMM_ERR_NOMEM;
    DG_HEAD(out, bb, 4, "treemap", CSS);
    char nb[40];
    size_t nl;
    for (size_t i = 1; i < v.n; i++) {
        const TNode *t = &V[i];
        if (t->w < 1 || t->h < 1) continue;
        const char *fill = SEC[t->sec % countof(SEC)], *st = SECD[t->sec % countof(SECD)];
        const TCls *uc = nullptr;
        for (size_t k = 0; k < cls.n && t->cls.n; k++) if (str_eq(cls.v[k].name, t->cls)) uc = &cls.v[k];
        bool leaf = t->first == UINT32_MAX;
        if (leaf) buf_lit(out, "<rect class=\"lf");
        else buf_lit(out, "<rect class=\"sec");
        if (t->cls.n) { buf_ch(out, ' '); buf_xml(out, t->cls); }
        buf_lit(out, "\" x=\""); buf_num(out, t->x); buf_lit(out, "\" y=\""); buf_num(out, t->y);
        buf_lit(out, "\" width=\""); buf_num(out, t->w); buf_lit(out, "\" height=\""); buf_num(out, t->h);
        buf_lit(out, "\" fill=\"");
        if (uc && uc->fill.n) buf_xml(out, uc->fill); else buf_put(out, strlen(fill), fill);
        buf_lit(out, "\" stroke=\"");
        if (uc && uc->stroke.n) buf_xml(out, uc->stroke); else if (leaf) buf_put(out, strlen(fill), fill); else buf_put(out, strlen(st), st);
        buf_lit(out, "\"/>");
        fmt_val(t->v, sizeof nb, nb, &nl);
        if (!leaf) {
            if (t->w > 30) {
                dg_text(out, t->label, 12, t->x + 6, t->y + HDR / 2, "start", "sh");
                if (t->w > text_width(t->label, 12) + text_width((Str){.p = nb, .n = nl}, 10) + 24)
                    dg_text(out, (Str){.p = nb, .n = nl}, 10, t->x + t->w - 6, t->y + HDR / 2, "end", "sv");
            }
            continue;
        }
        /* mermaid fits leaf labels up to 38px; value at ~0.6x */
        double fsz = fmin(38, fmax(8, fmin((t->w - 12) / fmax(1, text_width(t->label, 1)), t->h / 2.2)));
        double vsz = fmax(8, fmin(23, fsz * 0.6));
        const char *tc = SECT[t->sec % countof(SECT)];
        if (t->w > 20 && t->h > 20) {
            buf_lit(out, "<g fill=\""); buf_put(out, strlen(tc), tc); buf_lit(out, "\">");
            char cl[64];
            snprintf(cl, sizeof cl, "ln\" font-size=\"%.1f", fsz);
            if (uc && uc->color.n) { dg_text_open(out, t->x + t->w / 2, t->y + t->h / 2 - fsz * 0.45, "middle", cl);
                buf_lit(out, "<tspan fill=\""); buf_xml(out, uc->color); buf_lit(out, "\">"); buf_xml(out, t->label); buf_lit(out, "</tspan></text>"); }
            else dg_text(out, t->label, fsz, t->x + t->w / 2, t->y + t->h / 2 - fsz * 0.45, "middle", cl);
            char vc[64];
            snprintf(vc, sizeof vc, "lv\" font-size=\"%.1f", vsz);
            dg_text(out, (Str){.p = nb, .n = nl}, vsz, t->x + t->w / 2, t->y + t->h / 2 + fsz * 0.25 + vsz * 0.6, "middle", vc);
            buf_lit(out, "</g>");
        }
    }
    if (title.n) dg_text(out, title, 18, W / 2, 20, "middle", "ttl");
    buf_lit(out, "</svg>\n");
    return out->oom ? CMM_ERR_NOMEM : CMM_OK;
}
