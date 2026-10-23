/* block(-beta): author-controlled grid. `columns N`, nodes with flowchart-style shapes
 * (id, id["l"], id("l"), id(("l")), id(["l"]), id[("l")], id{"l"}, id{{"l"}}, id[["l"]], id>"l"], id[/"l"/]),
 * `id:span`, `space[:n]`, nested `block[:id["label"]][:span] ... end`, edges `a --> b`, `a -- "t" --> b`,
 * `a --- b`. Layout: bottom-up natural size (uniform cell per group), top-down stretch into cells. */
#include <math.h>
#include <stdio.h>

#include "dgraph.h"
#include "diagrams.h"
#include "text.h"

enum { BS_RECT, BS_ROUND, BS_STADIUM, BS_CIRCLE, BS_CYL, BS_DIAMOND, BS_HEX, BS_SUB, BS_ASYM, BS_PARA, BS_PARA_L, BS_TRAP, BS_TRAPI,
       BS_AR_R, BS_AR_L, BS_AR_U, BS_AR_D, BS_AR_X, BS_AR_Y }; /* id<[".."]>(dir) block arrows */
typedef struct Blk {
    Str id, label;
    uint32_t parent, first, last, next;
    int32_t cols;
    uint32_t span;
    uint8_t shape;
    bool group, space;
    double nr; /* circle: natural radius tw/2 + padding/2 (mermaid ignores the cell size) */
    double x, y, w, h, lh;
} Blk;
typedef struct BEdge { uint32_t a, b; Str label; bool arrow, arrow_s; } BEdge;
typedef VEC(Blk) BlkVec;

static constexpr double GAP = 8; /* mermaid block.padding */
static constexpr double PAD = 8;
static constexpr char CSS[] =
    "#mermaid .bn{fill:#ECECFF;stroke:#9370DB;stroke-width:1px;}#mermaid .bg{fill:#f4f4ff;stroke:#9370DB;stroke-width:1px;stroke-dasharray:4,2;}"
    "#mermaid .be{fill:none;stroke:#333;stroke-width:2px;}#mermaid .bl{fill:#e8e8e8;opacity:.85;}#mermaid .bt{font-size:16px;}";
static constexpr char DEFS[] =
    "<defs><marker id=\"blk-ae\" viewBox=\"0 0 10 10\" refX=\"9\" refY=\"5\" markerWidth=\"10\" markerHeight=\"10\" markerUnits=\"userSpaceOnUse\" orient=\"auto\">"
    "<path d=\"M0,0 L10,5 L0,10 z\" fill=\"#333\"/></marker>"
    "<marker id=\"blk-as\" viewBox=\"0 0 10 10\" refX=\"1\" refY=\"5\" markerWidth=\"10\" markerHeight=\"10\" markerUnits=\"userSpaceOnUse\" orient=\"auto\">"
    "<path d=\"M10,0 L0,5 L10,10 z\" fill=\"#333\"/></marker></defs>";

static bool isid(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || (unsigned char)c >= 0x80; }

static uint32_t add_blk(Arena *a, BlkVec *v, uint32_t parent, Blk b) {
    b.parent = parent, b.first = b.last = b.next = UINT32_MAX;
    if (!VEC_PUSH(a, v, b)) return UINT32_MAX;
    uint32_t i = (uint32_t)(v->n - 1);
    if (parent != UINT32_MAX) {
        Blk *p = &v->v[parent];
        if (p->last == UINT32_MAX) p->first = i; else v->v[p->last].next = i;
        p->last = i;
    }
    return i;
}

typedef struct P { const char *s; size_t i, n; } P;
static void sp(P *p) { while (p->i < p->n && is_sp(p->s[p->i])) p->i++; }

/* id + optional shape/label + optional :span. returns false on syntax error */
static bool node_tok(P *p, Str *id, Str *label, uint8_t *shape, uint32_t *span, bool *has_label) {
    size_t st = p->i;
    while (p->i < p->n && isid(p->s[p->i])) p->i++;
    if (p->i == st) return false;
    *id = (Str){.p = p->s + st, .n = p->i - st};
    *has_label = false, *shape = BS_RECT, *span = 1;
    if (p->i + 1 < p->n && p->s[p->i] == '<' && p->s[p->i + 1] == '[') { /* block arrow */
        p->i += 2;
        size_t ls = p->i;
        if (p->i < p->n && p->s[p->i] == '"') {
            ls = ++p->i;
            while (p->i < p->n && p->s[p->i] != '"') p->i++;
            if (p->i >= p->n) return false;
            *label = (Str){.p = p->s + ls, .n = p->i - ls};
            p->i++;
        } else {
            while (p->i < p->n && p->s[p->i] != ']') p->i++;
            *label = str_trim((Str){.p = p->s + ls, .n = p->i - ls});
        }
        if (p->i + 1 >= p->n || p->s[p->i] != ']' || p->s[p->i + 1] != '>') return false;
        p->i += 2;
        *has_label = true, *shape = BS_AR_R;
        if (p->i < p->n && p->s[p->i] == '(') {
            const char *e = memchr(p->s + p->i, ')', p->n - p->i);
            if (!e) return false;
            Str d = str_trim((Str){.p = p->s + p->i + 1, .n = (size_t)(e - p->s) - p->i - 1});
            *shape = str_eq(d, S("left")) ? BS_AR_L : str_eq(d, S("up")) ? BS_AR_U : str_eq(d, S("down")) ? BS_AR_D
                   : str_eq(d, S("x")) ? BS_AR_X : str_eq(d, S("y")) ? BS_AR_Y : BS_AR_R;
            p->i = (size_t)(e - p->s) + 1;
        }
        if (label->n == 18 && !memcmp(label->p, "&nbsp;&nbsp;&nbsp;", 18)) *label = S(" ");
    } else
    if (p->i < p->n && strchr("([{>", p->s[p->i])) {
        char op[4] = {};
        size_t k = 0;
        while (p->i < p->n && k < 3 && strchr("([{>/\\", p->s[p->i])) op[k++] = p->s[p->i++];
        size_t ls;
        if (p->i < p->n && p->s[p->i] == '"') {
            ls = ++p->i;
            while (p->i < p->n && p->s[p->i] != '"') p->i++;
            if (p->i >= p->n) return false;
            *label = (Str){.p = p->s + ls, .n = p->i - ls};
            p->i++;
        } else {
            ls = p->i;
            while (p->i < p->n && !strchr(")]}/\\", p->s[p->i])) p->i++;
            *label = str_trim((Str){.p = p->s + ls, .n = p->i - ls});
        }
        size_t cl = 0;
        while (p->i < p->n && cl < 3 && strchr(")]}/\\", p->s[p->i])) p->i++, cl++;
        if (!cl) return false;
        *has_label = true;
        *shape = !strcmp(op, "((") || !strcmp(op, "(((") ? BS_CIRCLE : !strcmp(op, "(") ? BS_ROUND : !strcmp(op, "([") ? BS_STADIUM
               : !strcmp(op, "[(") ? BS_CYL : !strcmp(op, "{") ? BS_DIAMOND : !strcmp(op, "{{") ? BS_HEX : !strcmp(op, "[[") ? BS_SUB
               : op[0] == '>' ? BS_ASYM : BS_RECT;
        if (!strcmp(op, "[/") || !strcmp(op, "[\\")) { /* [/ /] lean_right, [\ \] lean_left, [/ \] trapezoid, [\ /] inv_trapezoid */
            bool o = op[1] == '/', c = p->s[p->i - cl] == '/';
            *shape = o ? (c ? BS_PARA : BS_TRAP) : (c ? BS_TRAPI : BS_PARA_L);
        }
    }
    if (p->i < p->n && p->s[p->i] == ':') {
        p->i++;
        size_t ds = p->i;
        unsigned v = 0;
        while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9' && v < 1000) v = v * 10 + (unsigned)(p->s[p->i++] - '0');
        if (p->i == ds || !v) return false;
        *span = v > 64 ? 64 : v;
    }
    return true;
}

static uint32_t find(const BlkVec *v, Str id) {
    for (size_t i = 0; i < v->n; i++) if (!v->v[i].space && !v->v[i].group && str_eq(v->v[i].id, id)) return (uint32_t)i;
    for (size_t i = 0; i < v->n; i++) if (v->v[i].group && str_eq(v->v[i].id, id)) return (uint32_t)i;
    return UINT32_MAX;
}

static uint32_t group_cols(const Blk *B, const Blk *g) {
    if (g->cols > 0) return (uint32_t)g->cols;
    uint32_t t = 0;
    for (uint32_t c = g->first; c != UINT32_MAX; c = B[c].next) t += B[c].span;
    return t ? t : 1;
}

/* rows needed and per-child (row, col) via flow placement */
static uint32_t flow(Blk *B, const Blk *g, uint32_t cols, uint32_t *rc, uint32_t *cc) {
    uint32_t r = 0, c = 0;
    for (uint32_t k = g->first; k != UINT32_MAX; k = B[k].next) {
        uint32_t s = B[k].span < cols ? B[k].span : cols;
        if (c + s > cols) r++, c = 0;
        rc[k] = r, cc[k] = c;
        c += s;
    }
    return g->first == UINT32_MAX ? 0 : r + 1;
}

static bool in_list(Str ids, Str id) {
    while (ids.n) {
        const char *c = memchr(ids.p, ',', ids.n);
        Str one = str_trim((Str){.p = ids.p, .n = c ? (size_t)(c - ids.p) : ids.n});
        if (str_eq(one, id)) return true;
        ids = c ? (Str){.p = c + 1, .n = ids.n - (size_t)(c + 1 - ids.p)} : (Str){};
    }
    return false;
}
/* css for node id from `class ids cls` + `classDef cls css` and `style id css` (style wins: it comes last) */
static Str node_css(size_t n, const Str L[static n], Str id) {
    Str cls = {}, sty = {}, out = {};
    for (size_t i = 0; i < n; i++) {
        Str r = L[i], w = dg_word(&r), ids = dg_word(&r);
        if (str_eq(w, S("class")) && in_list(ids, id)) cls = str_trim(r);
        if (str_eq(w, S("style")) && in_list(ids, id)) sty = str_trim(r);
    }
    for (size_t i = 0; cls.n && i < n; i++) {
        Str r = L[i], w = dg_word(&r), nm = dg_word(&r);
        if (str_eq(w, S("classDef")) && in_list(nm, cls)) out = str_trim(r);
    }
    return sty.n ? sty : out;
}
/* insert style="css" after every class="bn" written since offset `from` */
static bool style_shape(Buf *b, size_t from, Str css) {
    static constexpr char K[] = "class=\"bn\"";
    for (size_t i = from; i + sizeof K - 1 <= b->n; i++) {
        if (memcmp(b->p + i, K, sizeof K - 1)) continue;
        size_t at = i + sizeof K - 1, add = css.n + 9;
        if (!buf_reserve(b, add)) return false;
        memmove(b->p + at + add, b->p + at, b->n - at);
        memcpy(b->p + at, " style=\"", 8);
        size_t k = at + 8;
        for (size_t j = 0; j < css.n; j++) {
            char ch = css.p[j] == ',' ? ';' : css.p[j];
            b->p[k++] = ch == '"' || ch == '<' || ch == '&' ? ' ' : ch;
        }
        b->p[k] = '"';
        b->n += add;
        i = at + add;
    }
    return true;
}

/* mermaid lean/trapezoid shapes: slant h/2 inside a cell of width label + padding + h; TL TR BR BL */
static void para_pts(uint8_t shape, double x, double y, double w, double h, double Q[static 8]) {
    double o = (h - 32) / 2; /* overhang: polygon width tw + 8 + h vs cell tw + 40 */
    x -= o, w += 2 * o;
    double e = fmin(h / 2, w / 3), tl = x, tr = x + w, br = x + w, bl = x;
    switch (shape) {
    case BS_PARA: tl += e, bl = x, br -= e; break;
    case BS_PARA_L: tr -= e, bl += e; break;
    case BS_TRAP: tl += e, tr -= e; break;
    default: bl += e, br -= e; break;
    }
    const double q[8] = {tl, y, tr, y, br, y + h, bl, y + h};
    memcpy(Q, q, sizeof q);
}

/* outline polygon of b (arcs approximated); returns vertex count */
static size_t outline(const Blk *b, double P[static 80]) {
    double x = b->x, y = b->y, w = b->w, h = b->h, cx = x + w / 2, cy = y + h / 2;
    size_t k = 0;
    #define V(px, py) (P[2 * k] = (px), P[2 * k + 1] = (py), k++)
    switch (b->shape) {
    case BS_CIRCLE: {
        double r = b->nr > 0 ? fmin(b->nr, fmin(w, h) / 2) : fmin(w, h) / 2;
        for (int i = 0; i < 32; i++) V(cx + r * cos(i * M_PI / 16), cy + r * sin(i * M_PI / 16));
        break;
    }
    case BS_STADIUM: case BS_ROUND: {
        double r = b->shape == BS_STADIUM ? fmin(h / 2, w / 2) : fmin(8, fmin(w, h) / 2);
        double C[4][2] = {{x + w - r, y + r}, {x + w - r, y + h - r}, {x + r, y + h - r}, {x + r, y + r}};
        for (int c = 0; c < 4; c++)
            for (int i = 0; i <= 4; i++) {
                double t = (c * 4 + i - 4) * M_PI / 8; /* quarter arcs: -90..0, 0..90, ... */
                V(C[c][0] + r * cos(t), C[c][1] + r * sin(t));
            }
        break;
    }
    case BS_DIAMOND: V(cx, y); V(x + w, cy); V(cx, y + h); V(x, cy); break;
    case BS_HEX: { double e = fmin(h / 2, w / 4); V(x + e, y); V(x + w - e, y); V(x + w, cy); V(x + w - e, y + h); V(x + e, y + h); V(x, cy); break; }
    case BS_ASYM: { double e = h / 3; V(x, y); V(x + w, y); V(x + w, y + h); V(x, y + h); V(x + e, cy); break; }
    case BS_PARA: case BS_PARA_L: case BS_TRAP: case BS_TRAPI: {
        double Q[8];
        para_pts(b->shape, x, y, w, h, Q);
        for (int i = 0; i < 4; i++) V(Q[2 * i], Q[2 * i + 1]);
        break;
    }
    default: V(x, y); V(x + w, y); V(x + w, y + h); V(x, y + h);
    }
    #undef V
    return k;
}

static void clip(const Blk *b, double ox, double oy, double *x, double *y) {
    /* first crossing of the ray centre->(ox,oy) with b's outline */
    double P[80], cx = b->x + b->w / 2, cy = b->y + b->h / 2, dx = ox - cx, dy = oy - cy, best = 1;
    size_t k = outline(b, P);
    for (size_t i = 0; i < k; i++) {
        double ax = P[2 * i], ay = P[2 * i + 1], bx = P[2 * ((i + 1) % k)], by = P[2 * ((i + 1) % k) + 1];
        double ex = bx - ax, ey = by - ay, den = dx * ey - dy * ex;
        if (fabs(den) < 1e-12) continue;
        double t = ((ax - cx) * ey - (ay - cy) * ex) / den, u = ((ax - cx) * dy - (ay - cy) * dx) / den;
        if (t > 0 && t < best && u >= -1e-9 && u <= 1 + 1e-9) best = t;
    }
    *x = cx + dx * best, *y = cy + dy * best;
}

DIAG_FN(block_render) {
    LineIt it = {.s = src, .n = n, .i = off};
    Str line;
    size_t lo;
    if (!dg_next_line(&it, &line, &lo)) return diag_fail(err_off, msg, off, "empty block diagram");
    BlkVec bv = {};
    VEC(BEdge) ev = {};
    VEC(Str) sv = {}; /* classDef / class / style lines, resolved when drawing */
    if (add_blk(a, &bv, UINT32_MAX, (Blk){.cols = -1, .span = 1, .group = true}) == UINT32_MAX) return CMM_ERR_NOMEM;
    uint32_t cur = 0;
    unsigned depth = 0;
    while (dg_next_line(&it, &line, &lo)) {
        if (str_starts(line, S("classDef ")) || str_starts(line, S("class ")) || str_starts(line, S("style "))) {
            if (!VEC_PUSH(a, &sv, line)) return CMM_ERR_NOMEM;
            continue;
        }
        P p = {.s = line.p, .i = 0, .n = line.n};
        uint32_t prev = UINT32_MAX;
        while (sp(&p), p.i < p.n) {
            Str rest = {.p = p.s + p.i, .n = p.n - p.i};
            size_t eo = lo + (size_t)(rest.p - line.p);
            bool wend = p.i + 3 == p.n || (p.i + 3 < p.n && !isid(p.s[p.i + 3]));
            if (str_starts(rest, S("end")) && wend) {
                if (!depth) return diag_fail(err_off, msg, eo, "'end' without block");
                depth--, cur = bv.v[cur].parent, p.i += 3, prev = UINT32_MAX;
                continue;
            }
            if (str_starts(rest, S("columns")) && (rest.n == 7 || is_sp(rest.p[7]))) {
                p.i += 7, sp(&p);
                Str w = {.p = p.s + p.i, .n = 0};
                while (p.i < p.n && !is_sp(p.s[p.i])) p.i++, w.n++;
                double v;
                if (str_eq(w, S("auto"))) bv.v[cur].cols = -1;
                else if (!dg_parse_num(w, &v) || v < 1 || v > 64 || v != floor(v)) return diag_fail(err_off, msg, eo, "columns must be 1..64 or auto");
                else bv.v[cur].cols = (int32_t)v;
                continue;
            }
            if (str_starts(rest, S("block")) && (rest.n == 5 || is_sp(rest.p[5]) || rest.p[5] == ':')) {
                p.i += 5;
                Blk g = {.cols = -1, .span = 1, .group = true};
                if (p.i < p.n && p.s[p.i] == ':') {
                    p.i++;
                    bool hl;
                    if (!node_tok(&p, &g.id, &g.label, &g.shape, &g.span, &hl)) return diag_fail(err_off, msg, eo, "bad block header");
                }
                if (depth >= 32) return diag_fail(err_off, msg, eo, "blocks nested too deeply");
                uint32_t gi = add_blk(a, &bv, cur, g);
                if (gi == UINT32_MAX) return CMM_ERR_NOMEM;
                cur = gi, depth++, prev = UINT32_MAX;
                continue;
            }
            if (str_starts(rest, S("space")) && (rest.n == 5 || is_sp(rest.p[5]) || rest.p[5] == ':')) {
                p.i += 5;
                uint32_t s = 1;
                if (p.i < p.n && p.s[p.i] == ':') {
                    p.i++;
                    unsigned v = 0;
                    size_t ds = p.i;
                    while (p.i < p.n && p.s[p.i] >= '0' && p.s[p.i] <= '9' && v < 1000) v = v * 10 + (unsigned)(p.s[p.i++] - '0');
                    if (p.i == ds || !v) return diag_fail(err_off, msg, eo, "bad space width");
                    s = v > 64 ? 64 : v;
                }
                if (add_blk(a, &bv, cur, (Blk){.span = s, .space = true}) == UINT32_MAX) return CMM_ERR_NOMEM;
                prev = UINT32_MAX;
                continue;
            }
            /* edge continuation */
            if (prev != UINT32_MAX && p.i + 1 < p.n && strchr("-=.<", p.s[p.i])) {
                size_t as = p.i;
                while (p.i < p.n && strchr("-=.<>ox", p.s[p.i]) && !(p.s[p.i] == 'o' || p.s[p.i] == 'x' ? isid(p.i + 1 < p.n ? p.s[p.i + 1] : ' ') : false)) p.i++;
                Str ar = {.p = p.s + as, .n = p.i - as}, lab = {};
                sp(&p);
                if (ar.n == 2 && p.i < p.n && p.s[p.i] == '"') { /* -- "label" --> */
                    size_t ls = ++p.i;
                    while (p.i < p.n && p.s[p.i] != '"') p.i++;
                    if (p.i >= p.n) return diag_fail(err_off, msg, eo, "unterminated edge label");
                    lab = (Str){.p = p.s + ls, .n = p.i - ls};
                    p.i++, sp(&p), as = p.i;
                    while (p.i < p.n && strchr("-=.>", p.s[p.i])) p.i++;
                    ar = (Str){.p = p.s + as, .n = p.i - as};
                    sp(&p);
                }
                if (ar.n < 2) return diag_fail(err_off, msg, eo, "bad edge");
                if (p.i < p.n && p.s[p.i] == '|') { /* -->|label| */
                    size_t ls = ++p.i;
                    while (p.i < p.n && p.s[p.i] != '|') p.i++;
                    if (p.i >= p.n) return diag_fail(err_off, msg, eo, "unterminated edge label");
                    lab = str_trim((Str){.p = p.s + ls, .n = p.i - ls});
                    p.i++, sp(&p);
                }
                Str id, label = {};
                uint8_t shape;
                uint32_t span;
                bool hl;
                size_t ts = p.i;
                if (!node_tok(&p, &id, &label, &shape, &span, &hl)) return diag_fail(err_off, msg, lo + ts, "expected edge target");
                uint32_t t = find(&bv, id);
                if (t == UINT32_MAX && (t = add_blk(a, &bv, cur, (Blk){.id = id, .label = hl ? label : id, .shape = shape, .span = span})) == UINT32_MAX) return CMM_ERR_NOMEM;
                else if (hl) bv.v[t].label = label, bv.v[t].shape = shape;
                if (!VEC_PUSH(a, &ev, ((BEdge){.a = prev, .b = t, .label = lab, .arrow = ar.p[ar.n - 1] == '>', .arrow_s = ar.p[0] == '<'}))) return CMM_ERR_NOMEM;
                prev = t;
                continue;
            }
            Str id, label = {};
            uint8_t shape;
            uint32_t span;
            bool hl;
            if (!node_tok(&p, &id, &label, &shape, &span, &hl)) return diag_fail(err_off, msg, eo, "unexpected token in block diagram");
            uint32_t t = find(&bv, id);
            if (t == UINT32_MAX) {
                if ((t = add_blk(a, &bv, cur, (Blk){.id = id, .label = hl ? label : id, .shape = shape, .span = span})) == UINT32_MAX) return CMM_ERR_NOMEM;
            } else if (hl) bv.v[t].label = label, bv.v[t].shape = shape;
            prev = t;
        }
    }
    if (depth) return diag_fail(err_off, msg, n, "missing 'end'");
    Blk *B = bv.v;
    size_t nb = bv.n;
    uint32_t *rc = ANEW(a, uint32_t, nb), *cc = ANEW(a, uint32_t, nb), *rows = ANEW(a, uint32_t, nb);
    double **gx = ANEW(a, double *, nb); /* per group: extra gap after column k (labelled side-by-side edges) */
    if (!rc || !cc || !rows || !gx) return CMM_ERR_NOMEM;
    memset(gx, 0, nb * sizeof *gx); /* arena memory is not zeroed */
    /* bottom-up natural sizes (children always have larger indices) */
    for (size_t i = nb; i-- > 0;) {
        Blk *b = &B[i];
        if (b->space) { b->w = 10, b->h = 10; continue; }
        if (!b->group) {
            double tw;
            unsigned nl;
            label_measure(b->label, fs, &tw, &nl);
            /* mermaid's natural shape sizes (block.padding = 8, label line = 1.5em) */
            double lh = nl * fs * 1.5;
            b->w = tw + 32, b->h = lh + 16;
            switch (b->shape) {
            case BS_ROUND: b->w = tw + 16; break;
            case BS_PARA: case BS_PARA_L: case BS_TRAP: case BS_TRAPI: b->w = tw + 40; break; /* mermaid layout width; the polygon (tw + 8 + h) overhangs it */
            case BS_STADIUM: case BS_ASYM: b->w = tw + 16, b->h = lh + 8; break;
            case BS_SUB: case BS_HEX: b->w = tw + 24, b->h = lh + 8; break;
            case BS_CYL: {
                double w = tw + 8, ry = w / 2 / (2.5 + w / 50);
                b->w = w, b->h = lh + 8 + 3 * ry;
                break;
            }
            default: break;
            }
            if (b->shape == BS_CIRCLE) b->w = b->h = tw + 8, b->nr = (tw + 8) / 2; /* mermaid: r = tw/2 + padding/2 */
            if (b->shape == BS_DIAMOND) b->w = b->h = tw + lh + 16; /* mermaid: square, side = w + h */
            continue;
        }
        uint32_t cols = group_cols(B, b);
        double uw = 0, uh = 0;
        for (uint32_t c = b->first; c != UINT32_MAX; c = B[c].next) {
            uint32_t s = B[c].span < cols ? B[c].span : cols;
            uw = fmax(uw, (B[c].w - (s - 1) * GAP) / s), uh = fmax(uh, B[c].h);
        }
        rows[i] = flow(B, b, cols, rc, cc);
        b->lh = (i && b->label.n) ? fs * 1.4 : 0;
        double lw = b->label.n ? text_width(b->label, fs) + 2 * PAD : 0;
        double S = 0; /* deviate from mermaid: room for a label on an edge between neighbouring blocks in a row */
        for (size_t e = 0; e < ev.n; e++) {
            uint32_t L = ev.v[e].a, R = ev.v[e].b;
            if (L == R && L < nb && B[L].parent == i) { /* self loop + label live in the gap right of the block */
                uint32_t k = cc[L] + (B[L].span < cols ? B[L].span : cols) - 1;
                double need = 30 + (ev.v[e].label.n ? text_width(ev.v[e].label, fs * 0.875) + 8 : 0) - GAP;
                if (k >= cols || need <= 0) continue;
                if (!gx[i]) { if (!(gx[i] = ANEW(a, double, cols))) return CMM_ERR_NOMEM; memset(gx[i], 0, cols * sizeof *gx[i]); }
                gx[i][k] = fmax(gx[i][k], need);
                continue;
            }
            if (!ev.v[e].label.n || L >= nb || R >= nb || B[L].parent != i || B[R].parent != i || rc[L] != rc[R]) continue;
            if (cc[L] > cc[R]) { uint32_t t = L; L = R; R = t; }
            uint32_t sL = B[L].span < cols ? B[L].span : cols;
            if (cc[R] != cc[L] + sL || !cc[R]) continue;
            double need = text_width(ev.v[e].label, fs) + 16 - GAP;
            if (need <= 0) continue;
            if (!gx[i]) { if (!(gx[i] = ANEW(a, double, cols))) return CMM_ERR_NOMEM; memset(gx[i], 0, cols * sizeof *gx[i]); }
            gx[i][cc[R] - 1] = fmax(gx[i][cc[R] - 1], need);
        }
        if (gx[i]) for (uint32_t k = 0; k < cols; k++) S += gx[i][k];
        b->w = fmax(cols * uw + (cols - 1) * GAP + S + 2 * PAD, lw);
        b->h = rows[i] ? rows[i] * uh + (rows[i] - 1) * GAP + 2 * PAD + b->lh : 2 * PAD + fmax(b->lh, 20);
    }
    B[0].x = B[0].y = 0;
    for (size_t i = 0; i < nb; i++) { /* top-down stretch */
        Blk *b = &B[i];
        if (!b->group || b->first == UINT32_MAX) continue;
        uint32_t cols = group_cols(B, b);
        double S = 0;
        if (gx[i]) for (uint32_t k = 0; k < cols; k++) S += gx[i][k];
        double cw = (b->w - 2 * PAD - (cols - 1) * GAP - S) / cols, ch = (b->h - 2 * PAD - b->lh - (rows[i] - 1) * GAP) / rows[i];
        for (uint32_t c = b->first; c != UINT32_MAX; c = B[c].next) {
            uint32_t s = B[c].span < cols ? B[c].span : cols;
            double pre = 0, in = 0;
            if (gx[i]) {
                for (uint32_t k = 0; k < cc[c] && k < cols; k++) pre += gx[i][k];
                for (uint32_t k = cc[c]; k + 1 < cc[c] + s && k < cols; k++) in += gx[i][k];
            }
            B[c].x = b->x + PAD + cc[c] * (cw + GAP) + pre;
            B[c].y = b->y + PAD + b->lh + rc[c] * (ch + GAP);
            B[c].w = s * cw + (s - 1) * GAP + in, B[c].h = ch;
        }
    }
    double top = title.n ? 40 : 0;
    /* mermaid: viewBox = drawn content (the root group is not drawn) + 5 */
    BBox bb = {B[0].x + PAD, B[0].y + PAD - top, B[0].x + B[0].w - PAD, B[0].y + B[0].h - PAD};
    if (nb > 1) { /* vertical extent follows the drawn shapes (circles keep their natural size) */
        double y0 = INFINITY, y1 = -INFINITY;
        for (size_t i = 1; i < nb; i++) {
            const Blk *b = &B[i];
            if (b->space) continue;
            double r = b->shape == BS_CIRCLE && b->nr > 0 ? fmin(b->nr, fmin(b->w, b->h) / 2) : -1;
            double t = r < 0 ? b->y : b->y + b->h / 2 - r, u = r < 0 ? b->y + b->h : b->y + b->h / 2 + r;
            y0 = fmin(y0, t), y1 = fmax(y1, u);
            if (b->shape >= BS_PARA && b->shape <= BS_TRAPI) { /* polygon overhang (para_pts) */
                double o = (b->h - 32) / 2;
                bb.x0 = fmin(bb.x0, b->x - o), bb.x1 = fmax(bb.x1, b->x + b->w + o);
            }
        }
        if (y0 < y1) bb.y0 = y0 - top, bb.y1 = y1;
    }
    if (!buf_reserve(out, 2048 + nb * 400 + ev.n * 300)) return CMM_ERR_NOMEM;
    DG_HEAD(out, bb, 5, "block", CSS);
    buf_lit(out, DEFS);
    if (title.n) dg_text(out, title, 18, B[0].w / 2, -top / 2, "middle", "bt");
    for (size_t i = 1; i < nb; i++) {
        const Blk *b = &B[i];
        if (b->space) continue;
        double x = b->x, y = b->y, w = b->w, h = b->h, cx = x + w / 2, cy = y + h / 2;
        if (b->group) {
            dg_rect(out, x, y, w, h, 4, "bg");
            if (b->label.n) dg_text(out, b->label, fs, cx, y + PAD + b->lh / 2, "middle", "bt");
            continue;
        }
        buf_lit(out, "<g class=\"node\" id=\"blk-"); buf_xml(out, b->id); buf_lit(out, "\">");
        size_t shape0 = out->n;
        switch (b->shape) {
        case BS_ROUND: dg_rect(out, x, y, w, h, 8, "bn"); break;
        case BS_STADIUM: dg_rect(out, x, y, w, h, h / 2, "bn"); break;
        case BS_CIRCLE: {
            double r = b->nr > 0 ? fmin(b->nr, fmin(w, h) / 2) : fmin(w, h) / 2;
            buf_lit(out, "<circle class=\"bn\" cx=\""); buf_num(out, cx); buf_lit(out, "\" cy=\""); buf_num(out, cy);
            buf_lit(out, "\" r=\""); buf_num(out, r); buf_lit(out, "\"/>");
            break;
        }
        case BS_AR_R: case BS_AR_L: case BS_AR_U: case BS_AR_D: case BS_AR_X: case BS_AR_Y: {
            bool vert = b->shape == BS_AR_U || b->shape == BS_AR_D || b->shape == BS_AR_Y;
            bool two = b->shape == BS_AR_X || b->shape == BS_AR_Y, neg = b->shape == BS_AR_L || b->shape == BS_AR_U;
            /* unit arrow along +u in (u,v) space: u in [0,L], v in [-T/2,T/2]; head length = T/2 */
            /* natural size (mermaid does not stretch block arrows to the cell): label + head, centred */
            double nl = text_width(b->label, fs) + 40, nt = 40;
            double L = fmin(vert ? h : w, nl), T = fmin(vert ? w : h, nt);
            if (vert) x = cx - T / 2, y = cy - L / 2; else x = cx - L / 2, y = cy - T / 2;
            double hd = fmin(T / 2, L / 2), q = T / 4;
            double U[14], Vv[14];
            size_t k = 0;
            #define AP(u_, v_) (U[k] = (u_), Vv[k] = (v_), k++)
            if (two) { AP(0, 0); AP(hd, -T / 2); AP(hd, -q); } else { AP(0, -q); }
            AP(L - hd, -q); AP(L - hd, -T / 2); AP(L, 0); AP(L - hd, T / 2); AP(L - hd, q);
            if (two) { AP(hd, q); AP(hd, T / 2); } else AP(0, q);
            #undef AP
            buf_lit(out, "<polygon class=\"bn\" points=\"");
            for (size_t j = 0; j < k; j++) {
                double u = neg ? L - U[j] : U[j];
                if (j) buf_ch(out, ' ');
                if (vert) dg_pt(out, cx + Vv[j], y + u); else dg_pt(out, x + u, cy + Vv[j]);
            }
            buf_lit(out, "\"/>");
            break;
        }
        case BS_DIAMOND: case BS_HEX: case BS_ASYM: case BS_PARA: case BS_PARA_L: case BS_TRAP: case BS_TRAPI: {
            double P_[12];
            size_t k;
            if (b->shape == BS_DIAMOND) { double q[] = {cx, y, x + w, cy, cx, y + h, x, cy}; k = 4; memcpy(P_, q, sizeof q); }
            else if (b->shape == BS_HEX) { double e = fmin(h / 2, w / 4), q[] = {x + e, y, x + w - e, y, x + w, cy, x + w - e, y + h, x + e, y + h, x, cy}; k = 6; memcpy(P_, q, sizeof q); }
            else if (b->shape == BS_ASYM) { double e = h / 3, q[] = {x, y, x + w, y, x + w, y + h, x, y + h, x + e, cy}; k = 5; memcpy(P_, q, sizeof q); }
            else { para_pts(b->shape, x, y, w, h, P_); k = 4; }
            buf_lit(out, "<polygon class=\"bn\" points=\"");
            for (size_t j = 0; j < k; j++) { if (j) buf_ch(out, ' '); dg_pt(out, P_[2 * j], P_[2 * j + 1]); }
            buf_lit(out, "\"/>");
            break;
        }
        case BS_CYL: {
            double ry = w / 2 / (2.5 + w / 50); /* mermaid cylinder */
            buf_lit(out, "<path class=\"bn\" d=\"M"); dg_pt(out, x, y + ry);
            buf_lit(out, "a"); buf_num(out, w / 2); buf_ch(out, ','); buf_num(out, ry); buf_lit(out, " 0 0 0 "); buf_num(out, w); buf_lit(out, ",0");
            buf_lit(out, "a"); buf_num(out, w / 2); buf_ch(out, ','); buf_num(out, ry); buf_lit(out, " 0 0 0 "); buf_num(out, -w); buf_lit(out, ",0");
            buf_lit(out, "v"); buf_num(out, h - 2 * ry);
            buf_lit(out, "a"); buf_num(out, w / 2); buf_ch(out, ','); buf_num(out, ry); buf_lit(out, " 0 0 0 "); buf_num(out, w); buf_lit(out, ",0");
            buf_lit(out, "v"); buf_num(out, -(h - 2 * ry)); buf_lit(out, "\"/>");
            break;
        }
        case BS_SUB:
            dg_rect(out, x, y, w, h, 0, "bn");
            dg_line(out, x + 8, y, x + 8, y + h, "bn");
            dg_line(out, x + w - 8, y, x + w - 8, y + h, "bn");
            break;
        default: dg_rect(out, x, y, w, h, 0, "bn");
        }
        Str css = sv.n ? node_css(sv.n, sv.v, b->id) : (Str){};
        if (css.n && !style_shape(out, shape0, css)) return CMM_ERR_NOMEM;
        dg_text(out, b->label, fs, cx, cy, "middle", "bt");
        buf_lit(out, "</g>");
    }
    for (size_t e = 0; e < ev.n; e++) {
        const Blk *s = &B[ev.v[e].a], *t = &B[ev.v[e].b];
        double x0, y0, x1, y1;
        if (s == t) { /* self edge: a loop on the right side, label right of it (room reserved in gx) */
            double X = s->x + s->w, Y = s->y + s->h / 2, q = fmin(10, s->h / 4);
            buf_lit(out, "<path class=\"be\" fill=\"none\" d=\"M"); dg_pt(out, X, Y - q); buf_ch(out, 'C'); dg_pt(out, X + 28, Y - q - 14);
            buf_ch(out, ' '); dg_pt(out, X + 28, Y + q + 14); buf_ch(out, ' '); dg_pt(out, X, Y + q); buf_ch(out, '"');
            if (ev.v[e].arrow) buf_lit(out, " marker-end=\"url(#blk-ae)\"");
            buf_lit(out, "/>");
            if (ev.v[e].label.n) {
                double tw = text_width(ev.v[e].label, fs * 0.875);
                dg_rect(out, X + 26, Y - fs * 0.6, tw + 4, fs * 1.2, 0, "bl");
                dg_text(out, ev.v[e].label, fs * 0.875, X + 28 + tw / 2, Y, "middle", "bt\" font-size=\"14");
            }
            continue;
        }
        clip(s, t->x + t->w / 2, t->y + t->h / 2, &x0, &y0);
        clip(t, s->x + s->w / 2, s->y + s->h / 2, &x1, &y1);
        buf_lit(out, "<path class=\"be\" d=\"M"); dg_pt(out, x0, y0); buf_ch(out, 'L'); dg_pt(out, x1, y1); buf_ch(out, '"');
        if (ev.v[e].arrow) buf_lit(out, " marker-end=\"url(#blk-ae)\"");
        if (ev.v[e].arrow_s) buf_lit(out, " marker-start=\"url(#blk-as)\"");
        buf_lit(out, "/>");
        if (ev.v[e].label.n) {
            double tw = text_width(ev.v[e].label, fs * 0.875), mx = (x0 + x1) / 2, my = (y0 + y1) / 2;
            /* the midpoint can land on a block crossed by the straight edge: slide the label along it to a free spot */
            static constexpr double TS[] = {0.5, 0.4, 0.6, 0.3, 0.7, 0.2, 0.8, 0.12, 0.88};
            double best = INFINITY;
            for (size_t k = 0; k < countof(TS) && best > 0; k++) { /* least overlap with other blocks; first clear wins */
                double cx = x0 + (x1 - x0) * TS[k], cy = y0 + (y1 - y0) * TS[k], ov = 0;
                for (size_t j = 1; j < nb; j++) {
                    const Blk *o = &B[j];
                    if (o->group || o->w <= 0 || o == s || o == t) continue;
                    double ox = fmin(cx + tw / 2 + 2, o->x + o->w) - fmax(cx - tw / 2 - 2, o->x);
                    double oy = fmin(cy + fs * 0.6, o->y + o->h) - fmax(cy - fs * 0.6, o->y);
                    if (ox > 0 && oy > 0) ov += ox * oy;
                }
                if (ov < best - 1e-9) best = ov, mx = cx, my = cy;
            }
            dg_rect(out, mx - tw / 2 - 2, my - fs * 0.6, tw + 4, fs * 1.2, 0, "bl");
            dg_text(out, ev.v[e].label, fs * 0.875, mx, my, "middle", "bt\" font-size=\"14");
        }
    }
    buf_lit(out, "</svg>\n");
    return out->oom ? CMM_ERR_NOMEM : CMM_OK;
}
