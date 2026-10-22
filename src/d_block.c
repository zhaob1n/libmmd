/* block(-beta): author-controlled grid. `columns N`, nodes with flowchart-style shapes
 * (id, id["l"], id("l"), id(("l")), id(["l"]), id[("l")], id{"l"}, id{{"l"}}, id[["l"]], id>"l"], id[/"l"/]),
 * `id:span`, `space[:n]`, nested `block[:id["label"]][:span] ... end`, edges `a --> b`, `a -- "t" --> b`,
 * `a --- b`. Layout: bottom-up natural size (uniform cell per group), top-down stretch into cells. */
#include <math.h>
#include <stdio.h>

#include "dgraph.h"
#include "diagrams.h"
#include "text.h"

enum { BS_RECT, BS_ROUND, BS_STADIUM, BS_CIRCLE, BS_CYL, BS_DIAMOND, BS_HEX, BS_SUB, BS_ASYM, BS_PARA };
typedef struct Blk {
    Str id, label;
    uint32_t parent, first, last, next;
    int32_t cols;
    uint32_t span;
    uint8_t shape;
    bool group, space;
    double x, y, w, h, lh;
} Blk;
typedef struct BEdge { uint32_t a, b; Str label; bool arrow, arrow_s; } BEdge;
typedef VEC(Blk) BlkVec;

static constexpr double GAP = 24;
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
               : op[0] == '>' ? BS_ASYM : (!strcmp(op, "[/") || !strcmp(op, "[\\")) ? BS_PARA : BS_RECT;
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

static void clip(const Blk *b, double ox, double oy, double *x, double *y) {
    /* move (x,y)=centre of b towards (ox,oy) until it leaves b's rect */
    double cx = b->x + b->w / 2, cy = b->y + b->h / 2, dx = ox - cx, dy = oy - cy;
    double tx = dx != 0 ? (b->w / 2) / fabs(dx) : INFINITY, ty = dy != 0 ? (b->h / 2) / fabs(dy) : INFINITY;
    double t = fmin(fmin(tx, ty), 1);
    *x = cx + dx * t, *y = cy + dy * t;
}

DIAG_FN(block_render) {
    LineIt it = {.s = src, .n = n, .i = off};
    Str line;
    size_t lo;
    if (!dg_next_line(&it, &line, &lo)) return diag_fail(err_off, msg, off, "empty block diagram");
    BlkVec bv = {};
    VEC(BEdge) ev = {};
    if (add_blk(a, &bv, UINT32_MAX, (Blk){.cols = -1, .span = 1, .group = true}) == UINT32_MAX) return CMM_ERR_NOMEM;
    uint32_t cur = 0;
    unsigned depth = 0;
    while (dg_next_line(&it, &line, &lo)) {
        if (str_starts(line, S("classDef ")) || str_starts(line, S("class ")) || str_starts(line, S("style "))) continue;
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
    if (!rc || !cc || !rows) return CMM_ERR_NOMEM;
    /* bottom-up natural sizes (children always have larger indices) */
    for (size_t i = nb; i-- > 0;) {
        Blk *b = &B[i];
        if (b->space) { b->w = 10, b->h = 10; continue; }
        if (!b->group) {
            double tw;
            unsigned nl;
            label_measure(b->label, fs, &tw, &nl);
            b->w = fmax(tw + 32, 50), b->h = fmax(nl * fs * 1.5 + 16, 40);
            if (b->shape == BS_CIRCLE) b->w = b->h = fmax(b->w, b->h);
            if (b->shape == BS_DIAMOND) b->w *= 1.5, b->h *= 1.5;
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
        b->w = fmax(cols * uw + (cols - 1) * GAP + 2 * PAD, lw);
        b->h = rows[i] ? rows[i] * uh + (rows[i] - 1) * GAP + 2 * PAD + b->lh : 2 * PAD + fmax(b->lh, 20);
    }
    B[0].x = B[0].y = 0;
    for (size_t i = 0; i < nb; i++) { /* top-down stretch */
        Blk *b = &B[i];
        if (!b->group || b->first == UINT32_MAX) continue;
        uint32_t cols = group_cols(B, b);
        double cw = (b->w - 2 * PAD - (cols - 1) * GAP) / cols, ch = (b->h - 2 * PAD - b->lh - (rows[i] - 1) * GAP) / rows[i];
        for (uint32_t c = b->first; c != UINT32_MAX; c = B[c].next) {
            uint32_t s = B[c].span < cols ? B[c].span : cols;
            B[c].x = b->x + PAD + cc[c] * (cw + GAP);
            B[c].y = b->y + PAD + b->lh + rc[c] * (ch + GAP);
            B[c].w = s * cw + (s - 1) * GAP, B[c].h = ch;
        }
    }
    double top = title.n ? 40 : 0;
    BBox bb = {B[0].x, B[0].y - top, B[0].x + B[0].w, B[0].y + B[0].h};
    if (!buf_reserve(out, 2048 + nb * 400 + ev.n * 300)) return CMM_ERR_NOMEM;
    DG_HEAD(out, bb, 8, "block", CSS);
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
        switch (b->shape) {
        case BS_ROUND: dg_rect(out, x, y, w, h, 8, "bn"); break;
        case BS_STADIUM: dg_rect(out, x, y, w, h, h / 2, "bn"); break;
        case BS_CIRCLE: {
            double r = fmin(w, h) / 2;
            buf_lit(out, "<circle class=\"bn\" cx=\""); buf_num(out, cx); buf_lit(out, "\" cy=\""); buf_num(out, cy);
            buf_lit(out, "\" r=\""); buf_num(out, r); buf_lit(out, "\"/>");
            break;
        }
        case BS_DIAMOND: case BS_HEX: case BS_ASYM: case BS_PARA: {
            double P_[12];
            size_t k;
            if (b->shape == BS_DIAMOND) { double q[] = {cx, y, x + w, cy, cx, y + h, x, cy}; k = 4; memcpy(P_, q, sizeof q); }
            else if (b->shape == BS_HEX) { double e = fmin(h / 2, w / 4), q[] = {x + e, y, x + w - e, y, x + w, cy, x + w - e, y + h, x + e, y + h, x, cy}; k = 6; memcpy(P_, q, sizeof q); }
            else if (b->shape == BS_ASYM) { double e = h / 3, q[] = {x, y, x + w, y, x + w, y + h, x, y + h, x + e, cy}; k = 5; memcpy(P_, q, sizeof q); }
            else { double e = h / 3, q[] = {x + e, y, x + w, y, x + w - e, y + h, x, y + h}; k = 4; memcpy(P_, q, sizeof q); }
            buf_lit(out, "<polygon class=\"bn\" points=\"");
            for (size_t j = 0; j < k; j++) { if (j) buf_ch(out, ' '); dg_pt(out, P_[2 * j], P_[2 * j + 1]); }
            buf_lit(out, "\"/>");
            break;
        }
        case BS_CYL: {
            double ry = fmin(8, h / 6);
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
        dg_text(out, b->label, fs, cx, cy, "middle", "bt");
        buf_lit(out, "</g>");
    }
    for (size_t e = 0; e < ev.n; e++) {
        const Blk *s = &B[ev.v[e].a], *t = &B[ev.v[e].b];
        double x0, y0, x1, y1;
        clip(s, t->x + t->w / 2, t->y + t->h / 2, &x0, &y0);
        clip(t, s->x + s->w / 2, s->y + s->h / 2, &x1, &y1);
        buf_lit(out, "<path class=\"be\" d=\"M"); dg_pt(out, x0, y0); buf_ch(out, 'L'); dg_pt(out, x1, y1); buf_ch(out, '"');
        if (ev.v[e].arrow) buf_lit(out, " marker-end=\"url(#blk-ae)\"");
        if (ev.v[e].arrow_s) buf_lit(out, " marker-start=\"url(#blk-as)\"");
        buf_lit(out, "/>");
        if (ev.v[e].label.n) {
            double tw = text_width(ev.v[e].label, fs * 0.875), mx = (x0 + x1) / 2, my = (y0 + y1) / 2;
            dg_rect(out, mx - tw / 2 - 2, my - fs * 0.6, tw + 4, fs * 1.2, 0, "bl");
            dg_text(out, ev.v[e].label, fs * 0.875, mx, my, "middle", "bt\" font-size=\"14");
        }
    }
    buf_lit(out, "</svg>\n");
    return out->oom ? CMM_ERR_NOMEM : CMM_OK;
}
