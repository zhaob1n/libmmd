/* treemap(-beta): indentation tree of `"Section"` and `"Leaf": value` lines (quotes optional),
 * `:::class` suffixes and `classDef name fill:..,stroke:..,color:..`. Parent values are the sum of
 * their children (an explicit parent value is used only when it has no children). Layout is the
 * a port of d3's treemap() (squarify, phi ratio) with mermaid 11.17's paddings, header band and label fitting. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "dgraph.h"
#include "dcfg.h"
#include "diagrams.h"
#include "text.h"

typedef struct TNode {
    Str label, cls;
    double v, x, y, w, h;
    uint32_t parent, first, next, depth, sec;
} TNode;
typedef struct TCls { Str name, fill, stroke, color; } TCls;

static constexpr char CSS[] =
    "#mermaid .sh{font-size:12px;font-weight:bold;}#mermaid .lf{fill-opacity:.3;stroke-width:3px;}"
    "#mermaid .sv{font-size:10px;font-style:italic;}"
    "#mermaid .sec{fill-opacity:.6;stroke-width:2px;stroke-opacity:.4;}#mermaid .ttl{font-size:16px;fill:#333;}";
/* mermaid 11 default cScale (hsl(h,100%,~76%)): sections fill .6 opacity, leaves .3 with a 3px same-colour border */
static const char *const SEC[] = {"#8686ff", "#ffff78", "#d7ff86", "#86c2ff", "#86ffff", "#86ffc2", "#ff86ff", "#ff8686"};
static const char *const SECD[] = {"#3939ff", "#ffff2c", "#b5ff20", "#399cff", "#39ffff", "#39ff9c", "#ff39ff", "#ff3939"};
static constexpr unsigned char TMI[] = {0, 1, 2, 11, 10, 9, 4, 6}; /* SEC[k] == cScale[TMI[k]] */
static const char *const SECT[] = {"#fff", "#000", "#000", "#000", "#000", "#000", "#000", "#000"};

/* ---- d3-hierarchy 3 treemap() port: squarify (ratio phi), paddings, .round(true) ---- */
static void dice(TNode *V, const uint32_t *ch, size_t n, double sum, double x0, double y0, double x1, double y1) {
    double k = sum ? (x1 - x0) / sum : 0;
    for (size_t i = 0; i < n; i++) { TNode *t = &V[ch[i]]; t->y = y0, t->h = y1, t->x = x0, t->w = x0 += t->v * k; }
}
static void slice(TNode *V, const uint32_t *ch, size_t n, double sum, double x0, double y0, double x1, double y1) {
    double k = sum ? (y1 - y0) / sum : 0;
    for (size_t i = 0; i < n; i++) { TNode *t = &V[ch[i]]; t->x = x0, t->w = x1, t->y = y0, t->h = y0 += t->v * k; }
}
/* x,y,w,h hold x0,y0,x1,y1 during layout */
static void squarify(TNode *V, const uint32_t *ch, size_t n, double value, double x0, double y0, double x1, double y1) {
    const double ratio = (1 + sqrt(5.0)) / 2;
    size_t i0 = 0, i1 = 0;
    while (i0 < n) {
        double dx = x1 - x0, dy = y1 - y0, sum;
        do sum = V[ch[i1++]].v; while (!sum && i1 < n);
        double mn = sum, mx = sum, alpha = fmax(dy / dx, dx / dy) / (value * ratio), beta = sum * sum * alpha;
        double minr = fmax(mx / beta, beta / mn);
        for (; i1 < n; ++i1) {
            double nv = V[ch[i1]].v;
            sum += nv;
            if (nv < mn) mn = nv;
            if (nv > mx) mx = nv;
            beta = sum * sum * alpha;
            double nr = fmax(mx / beta, beta / mn);
            if (nr > minr) { sum -= nv; break; }
            minr = nr;
        }
        if (dx < dy) { double yy = value ? y0 + dy * sum / value : y1; dice(V, ch + i0, i1 - i0, sum, x0, y0, x1, yy); if (value) y0 = yy; }
        else { double xx = value ? x0 + dx * sum / value : x1; slice(V, ch + i0, i1 - i0, sum, x0, y0, xx, y1); if (value) x0 = xx; }
        value -= sum, i0 = i1;
    }
}
static int by_val_desc(const uint32_t *a, const uint32_t *b, const TNode *V) {
    double x = V[*a].v, y = V[*b].v;
    return (x < y) - (x > y);
}
static void sort_kids(TNode *V, uint32_t p, uint32_t *tmp) { /* hierarchy.sort: stable, value descending */
    size_t k = 0;
    for (uint32_t c = V[p].first; c != UINT32_MAX; c = V[c].next) tmp[k++] = c;
    for (size_t i = 1; i < k; i++) {
        uint32_t t = tmp[i];
        size_t j = i;
        while (j && by_val_desc(&tmp[j - 1], &t, V) > 0) tmp[j] = tmp[j - 1], j--;
        tmp[j] = t;
    }
    if (!k) return;
    V[p].first = tmp[0];
    for (size_t i = 0; i + 1 < k; i++) V[tmp[i]].next = tmp[i + 1];
    V[tmp[k - 1]].next = UINT32_MAX;
}
constexpr double HDR = 25;  /* SECTION_HEADER_HEIGHT */
constexpr double SPAD = 10; /* SECTION_INNER_PADDING */
/* positionNode: p = paddingStack[depth] (half the parent's paddingInner) */
static void position(TNode *V, uint32_t *tmp, uint32_t id, double p, double inner) {
    TNode *t = &V[id];
    double x0 = t->x + p, y0 = t->y + p, x1 = t->w - p, y1 = t->h - p;
    if (x1 < x0) x0 = x1 = (x0 + x1) / 2;
    if (y1 < y0) y0 = y1 = (y0 + y1) / 2;
    t->x = x0, t->y = y0, t->w = x1, t->h = y1;
    if (t->first == UINT32_MAX) return;
    double q = inner / 2;
    x0 += SPAD - q, y0 += HDR + SPAD - q, x1 -= SPAD - q, y1 -= SPAD - q;
    if (x1 < x0) x0 = x1 = (x0 + x1) / 2;
    if (y1 < y0) y0 = y1 = (y0 + y1) / 2;
    size_t k = 0;
    for (uint32_t c = t->first; c != UINT32_MAX; c = V[c].next) tmp[k++] = c;
    squarify(V, tmp, k, t->v, x0, y0, x1, y1);
    for (uint32_t c = t->first; c != UINT32_MAX; c = V[c].next) position(V, tmp + k, c, q, inner);
}


static size_t ord_idx(Str *dom, size_t *n, Str name, bool root) {
    for (size_t i = 0; i < *n; i++) if (dom[i].p ? (!root && str_eq(dom[i], name)) : root) return i;
    dom[*n] = root ? (Str){} : name;
    if (!root && !name.p) dom[*n] = (Str){.p = "", .n = 0};
    return (*n)++;
}
static const TCls *find_cls(const TCls *c, size_t n, Str name) {
    if (!name.n) return nullptr;
    const TCls *r = nullptr;
    for (size_t k = 0; k < n; k++) if (str_eq(c[k].name, name)) r = &c[k];
    return r;
}
/* d3 ordinal ranges: cScale/cScalePeer start with "transparent", cScaleLabel does not */
static void put_c(Buf *out, const char *pfx, size_t i) {
    bool lab = !strcmp(pfx, "cScaleLabel");
    if (!lab) {
        i %= 13;
        if (i == 0) { buf_lit(out, "transparent"); return; }
        i--;
    } else i %= 12;
    const char *const *tab = lab ? SECT : !strcmp(pfx, "cScale") ? SEC : SECD;
    const char *def = tab[i % countof(SEC)];
    for (size_t k = 0; k < countof(TMI); k++) if (TMI[k] == i) def = tab[k];
    if (lab) def = i == 0 || i == 3 ? "#fff" : "#000"; /* default theme: cScaleLabel0/3 = invert(labelTextColor) */
    const char *c = dcfg_pal(pfx, i, def);
    buf_put(out, strlen(c), c);
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
        return MMD_ERR_NOMEM;
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
            if (!VEC_PUSH(a, &cls, c)) return MMD_ERR_NOMEM;
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
        if (!VEC_PUSH(a, &v, t) || !VEC_PUSH(a, &ind, in)) return MMD_ERR_NOMEM;
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
    /* mermaid 11.17 treemap renderer: size nodeWidth*10 x nodeHeight*10, title band 30 */
    double inner = dcfg_num("treemap.padding", 10);
    double W = dcfg_num("treemap.nodeWidth", 100) * SPAD, H = dcfg_num("treemap.nodeHeight", 40) * SPAD;
    double top = title.n ? 30 : 0;
    uint32_t *scratch = ANEW(a, uint32_t, v.n + 1), *bfs = ANEW(a, uint32_t, v.n), *leaves = ANEW(a, uint32_t, v.n);
    if (!scratch || !bfs || !leaves) return MMD_ERR_NOMEM;
    for (size_t i = 0; i < v.n; i++) sort_kids(V, (uint32_t)i, scratch);
    V[0].x = 0, V[0].y = 0, V[0].w = W, V[0].h = H;
    position(V, scratch, 0, 0, inner);
    for (size_t i = 0; i < v.n; i++) { /* .round(true); then x,y,w,h back to origin + size, title offset */
        TNode *t = &V[i];
        double x0 = round(t->x), y0 = round(t->y), x1 = round(t->w), y1 = round(t->h);
        t->x = x0, t->y = y0 + top, t->w = x1 - x0, t->h = y1 - y0;
    }
    /* descendants() (breadth-first) and leaves() (pre-order) in sorted child order */
    size_t nb = 0, nlv = 0, head = 0;
    bfs[nb++] = 0;
    while (head < nb) for (uint32_t c = V[bfs[head++]].first; c != UINT32_MAX; c = V[c].next) bfs[nb++] = c;
    {
        size_t sp = 0;
        scratch[sp++] = 0;
        while (sp) {
            uint32_t id = scratch[--sp];
            if (V[id].first == UINT32_MAX) { if (id) leaves[nlv++] = id; continue; }
            size_t s0 = sp;
            for (uint32_t c = V[id].first; c != UINT32_MAX; c = V[c].next) scratch[sp++] = c;
            for (size_t i = s0, j = sp - 1; i < j; i++, j--) { uint32_t t = scratch[i]; scratch[i] = scratch[j], scratch[j] = t; }
        }
    }
    /* d3 scaleOrdinal: domain grows in first-use order; fill/peer see the root ("") first, labels do not */
    Str *dom_f = ANEW(a, Str, v.n + 1), *dom_l = ANEW(a, Str, v.n + 1);
    if (!dom_f || !dom_l) return MMD_ERR_NOMEM;
    size_t nf = 0, nlab = 0;
    BBox bb = {INFINITY, INFINITY, -INFINITY, -INFINITY};
    if (title.n) bb_add(&bb, W / 2 - text_width(title, 16) / 2, 15 - 8, W / 2 + text_width(title, 16) / 2, 15 + 8);
    for (size_t k = 0; k < nb; k++) if (V[bfs[k]].first != UINT32_MAX) {
        ord_idx(dom_f, &nf, V[bfs[k]].label, bfs[k] == 0);
        const TNode *t = &V[bfs[k]];
        if (bfs[k]) bb_add(&bb, t->x, t->y, t->x + t->w, t->y + t->h);
    }
    for (size_t k = 0; k < nlv; k++) { const TNode *t = &V[leaves[k]]; bb_add(&bb, t->x, t->y, t->x + t->w, t->y + t->h); }
    if (!(bb.x1 > bb.x0)) bb = (BBox){0, 0, W, H + top};
    if (!buf_reserve(out, 2048 + v.n * 600)) return MMD_ERR_NOMEM;
    DG_HEAD(out, bb, dcfg_num("treemap.diagramPadding", 8), "treemap", CSS);
    bool showv = !str_eq(dcfg_get("treemap.showValues"), S("false"));
    char nb_[40];
    size_t nl;
    for (size_t k = 1; k < nb; k++) { /* sections */
        const TNode *t = &V[bfs[k]];
        if (t->first == UINT32_MAX) continue;
        const TCls *uc = find_cls(cls.v, cls.n, t->cls);
        size_t fi = ord_idx(dom_f, &nf, t->label, false), li = ord_idx(dom_l, &nlab, t->label, false);
        buf_lit(out, "<rect class=\"sec");
        if (t->cls.n) { buf_ch(out, ' '); buf_xml(out, t->cls); }
        buf_lit(out, "\" x=\""); buf_num(out, t->x); buf_lit(out, "\" y=\""); buf_num(out, t->y);
        buf_lit(out, "\" width=\""); buf_num(out, t->w); buf_lit(out, "\" height=\""); buf_num(out, t->h);
        buf_lit(out, "\" fill=\"");
        if (uc && uc->fill.n) buf_xml(out, uc->fill); else put_c(out, "cScale", fi);
        buf_lit(out, "\" stroke=\"");
        if (uc && uc->stroke.n) buf_xml(out, uc->stroke); else put_c(out, "cScalePeer", fi);
        buf_lit(out, "\"/>");
        /* header label: x+6, clipped/ellipsised to the space left of the value */
        double room = fmax(15, showv && t->v ? t->w - 10 - 30 - 10 - 6 : t->w - 12);
        bool sv = showv && t->v; /* narrow header: the value keeps what's left after real room for the label */
        if (sv) {
            fmt_val(t->v, sizeof nb_, nb_, &nl);
            double vw = text_width((Str){.p = nb_, .n = nl}, 10);
            if (vw + 16 > t->w) sv = false;
            else room = fmin(room, t->w - 6 - 10 - vw - 6);
        }
        Str lab = t->label;
        char *ell = nullptr;
        if (text_width(lab, 12) > room) {
            ell = ANEW(a, char, lab.n + 4);
            if (!ell) return MMD_ERR_NOMEM;
            size_t m = lab.n;
            while (m > 0) {
                m--;
                while (m && ((unsigned char)lab.p[m] & 0xC0) == 0x80) m--;
                memcpy(ell, lab.p, m), memcpy(ell + m, "...", 3);
                if (!m || text_width((Str){.p = ell, .n = m + 3}, 12) <= room) break;
            }
            lab = (Str){.p = ell, .n = m + 3};
            if (!m || text_width(lab, 12) > room) lab = (Str){}; /* "..." alone says nothing */
        }
        buf_lit(out, "<g fill=\""); put_c(out, "cScaleLabel", li); buf_lit(out, "\">");
        if (lab.n) dg_text(out, lab, 12, t->x + 6, t->y + HDR / 2, "start", "sh");
        if (sv) {
            dg_text(out, (Str){.p = nb_, .n = nl}, 10, t->x + t->w - 10, t->y + HDR / 2, "end", "sv");
        }
        buf_lit(out, "</g>");
    }
    bool cx = nlv > 20;
    const double bl = cx ? 16 : 38, bv = cx ? 14 : 28, ml = cx ? 4 : 8, mv = cx ? 4 : 6, lp = cx ? 2 : 4, thr = cx ? 8 : 10, sp = cx ? 1 : 2;
    for (size_t k = 0; k < nlv; k++) {
        const TNode *t = &V[leaves[k]];
        const TCls *uc = find_cls(cls.v, cls.n, t->cls);
        size_t fi = ord_idx(dom_f, &nf, V[t->parent].label, t->parent == 0);
        buf_lit(out, "<rect class=\"lf");
        if (t->cls.n) { buf_ch(out, ' '); buf_xml(out, t->cls); }
        buf_lit(out, "\" x=\""); buf_num(out, t->x); buf_lit(out, "\" y=\""); buf_num(out, t->y);
        buf_lit(out, "\" width=\""); buf_num(out, t->w); buf_lit(out, "\" height=\""); buf_num(out, t->h);
        buf_lit(out, "\" fill=\"");
        if (uc && uc->fill.n) buf_xml(out, uc->fill); else put_c(out, "cScale", fi);
        buf_lit(out, "\" stroke=\"");
        if (uc && uc->stroke.n) buf_xml(out, uc->stroke); else put_c(out, "cScale", fi);
        buf_lit(out, "\"/>");
        size_t li = ord_idx(dom_l, &nlab, t->label, false);
        double aw = t->w - 2 * lp, ah = t->h - 2 * lp;
        if (aw < thr || ah < thr) continue;
        double f = bl;
        while (text_width(t->label, f) > aw && f > ml) f--;
        double pv = fmax(mv, fmin(bv, round(f * 0.6)));
        while (f + sp + pv > ah && f > ml) {
            f--;
            pv = fmax(mv, fmin(bv, round(f * 0.6)));
        }
        if (cx ? (f < ml || ah < ml) : (text_width(t->label, f) > aw || f < ml || ah < f)) continue;
        buf_lit(out, "<g fill=\"");
        if (uc && uc->color.n) buf_xml(out, uc->color); else put_c(out, "cScaleLabel", li);
        buf_lit(out, "\">");
        char cl[64];
        snprintf(cl, sizeof cl, "ln\" font-size=\"%.0f", f);
        dg_text(out, t->label, f, t->x + t->w / 2, t->y + t->h / 2, "middle", cl);
        if (showv && t->v) {
            fmt_val(t->v, sizeof nb_, nb_, &nl);
            Str vs = {.p = nb_, .n = nl};
            double vt = t->h / 2 + f / 2 + sp; /* dominant-baseline: hanging */
            if (!(text_width(vs, pv) > aw || vt + pv > t->h - 4)) {
                snprintf(cl, sizeof cl, "lv\" font-size=\"%.0f", pv);
                dg_text(out, vs, pv, t->x + t->w / 2, t->y + vt + pv / 2, "middle", cl);
            }
        }
        buf_lit(out, "</g>");
    }
    if (title.n) dg_text(out, title, 16, W / 2, 15, "middle", "ttl");
    buf_lit(out, "</svg>\n");
    return out->oom ? MMD_ERR_NOMEM : MMD_OK;
}
