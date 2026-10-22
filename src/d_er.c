/* erDiagram: entities with attribute tables (type, name, PK/FK/UK keys, comments), aliases,
 * all crow's-foot cardinalities (symbolic and word forms), identifying (--) and
 * non-identifying (..) relationships, labels, direction. Layout: flow_layout. */
#include <math.h>

#include "dgraph.h"
#include "diagrams.h"
#include "text.h"

typedef enum : uint8_t { CD_ONE, CD_ZERO_ONE, CD_ONE_MANY, CD_ZERO_MANY } Card;
typedef struct Attr { int32_t ent; Str type, name, keys, comment; } Attr;
typedef struct ERel { Card ca, cb; bool dashed; } ERel;
typedef struct EInfo { uint32_t a0, an; double cw[4]; } EInfo;

typedef struct EP {
    Arena *A;
    Graph g;
    VEC(EInfo) ei;
    VEC(Attr) at;
    VEC(ERel) rel;
    size_t *err_off;
    char *msg;
} EP;

[[nodiscard]] static int32_t ent(EP *p, Str ref) {
    ref = str_trim(ref);
    Str label = {};
    const char *b = ref.n ? memchr(ref.p, '[', ref.n) : nullptr;
    if (b && ref.p[ref.n - 1] == ']') {
        label = str_trim((Str){.p = b + 1, .n = ref.n - (size_t)(b + 1 - ref.p) - 1});
        if (label.n >= 2 && label.p[0] == '"' && label.p[label.n - 1] == '"') label.p++, label.n -= 2;
        ref = str_trim((Str){.p = ref.p, .n = (size_t)(b - ref.p)});
    }
    if (ref.n >= 2 && ref.p[0] == '"' && ref.p[ref.n - 1] == '"') ref.p++, ref.n -= 2;
    if (!ref.n) return NONE - 1;
    bool fresh;
    int32_t v = dg_node(&p->g, ref, &fresh);
    if (v == NONE) return NONE;
    if (fresh && !VEC_PUSH(p->A, &p->ei, (EInfo){})) return NONE;
    if (label.n) p->g.nlabel[v] = label;
    return v;
}

typedef struct CardTok { const char *t; Card c; } CardTok;
static const CardTok LCARD[] = {
    {"|o", CD_ZERO_ONE}, {"||", CD_ONE}, {"}o", CD_ZERO_MANY}, {"}|", CD_ONE_MANY},
};
static const CardTok RCARD[] = {
    {"o|", CD_ZERO_ONE}, {"||", CD_ONE}, {"o{", CD_ZERO_MANY}, {"|{", CD_ONE_MANY},
};
static const CardTok WCARD[] = { /* word forms, longest first */
    {"one or zero", CD_ZERO_ONE}, {"zero or one", CD_ZERO_ONE}, {"one or more", CD_ONE_MANY},
    {"one or many", CD_ONE_MANY}, {"zero or more", CD_ZERO_MANY}, {"zero or many", CD_ZERO_MANY},
    {"many(1)", CD_ONE_MANY}, {"many(0)", CD_ZERO_MANY}, {"only one", CD_ONE}, {"1+", CD_ONE_MANY},
    {"0+", CD_ZERO_MANY}, {"1", CD_ONE},
};

/* symbolic: A <lc>(--|..)<rc> B ; word form: A <card> (to|optionally to) <card> B */
static bool parse_rel(Str l, Str *a, Str *b, ERel *r, Str *label) {
    const char *c = nullptr;
    bool inq = false;
    for (size_t k = 0; k < l.n; k++) {
        if (l.p[k] == '"') inq = !inq;
        if (!inq && l.p[k] == ':') { c = l.p + k; break; }
    }
    if (!c) return false;
    Str lhs = str_trim((Str){.p = l.p, .n = (size_t)(c - l.p)});
    *label = str_trim((Str){.p = c + 1, .n = l.n - (size_t)(c + 1 - l.p)});
    if (label->n >= 2 && label->p[0] == '"' && label->p[label->n - 1] == '"') label->p++, label->n -= 2;
    for (size_t k = 2; k + 4 <= lhs.n; k++) {
        bool dash = lhs.p[k] == '-' && lhs.p[k + 1] == '-', dot = lhs.p[k] == '.' && lhs.p[k + 1] == '.';
        if (!dash && !dot) continue;
        size_t li = countof(LCARD), ri = countof(RCARD);
        for (size_t t = 0; t < countof(LCARD); t++) if (memcmp(lhs.p + k - 2, LCARD[t].t, 2) == 0) { li = t; break; }
        for (size_t t = 0; t < countof(RCARD); t++) if (memcmp(lhs.p + k + 2, RCARD[t].t, 2) == 0) { ri = t; break; }
        if (li == countof(LCARD) || ri == countof(RCARD)) continue;
        r->ca = LCARD[li].c, r->cb = RCARD[ri].c, r->dashed = dot;
        *a = (Str){.p = lhs.p, .n = k - 2};
        *b = (Str){.p = lhs.p + k + 4, .n = lhs.n - k - 4};
        return true;
    }
    /* word form */
    for (size_t k = 0; k + 4 <= lhs.n; k++) {
        bool opt = false;
        size_t tl = 0;
        if (k + 15 <= lhs.n && memcmp(lhs.p + k, " optionally to ", 15) == 0) opt = true, tl = 15;
        else if (memcmp(lhs.p + k, " to ", 4) == 0) tl = 4;
        else continue;
        Str L = str_trim((Str){.p = lhs.p, .n = k}), R = str_trim((Str){.p = lhs.p + k + tl, .n = lhs.n - k - tl});
        size_t li = SIZE_MAX, ri = SIZE_MAX;
        for (size_t t = 0; t < countof(WCARD) && li == SIZE_MAX; t++) {
            size_t wl = strlen(WCARD[t].t);
            if (L.n > wl && memcmp(L.p + L.n - wl, WCARD[t].t, wl) == 0 && is_sp(L.p[L.n - wl - 1])) {
                li = t;
                L.n -= wl;
            }
        }
        for (size_t t = 0; t < countof(WCARD) && ri == SIZE_MAX; t++) {
            size_t wl = strlen(WCARD[t].t);
            if (R.n > wl && memcmp(R.p, WCARD[t].t, wl) == 0 && is_sp(R.p[wl])) {
                ri = t;
                R.p += wl, R.n -= wl;
            }
        }
        if (li == SIZE_MAX || ri == SIZE_MAX) continue;
        r->ca = WCARD[li].c, r->cb = WCARD[ri].c, r->dashed = opt;
        *a = L, *b = R;
        return true;
    }
    return false;
}

static cmm_status parse(EP *p, LineIt *it) {
    Graph *g = &p->g;
    Str line;
    size_t off;
    int32_t body = NONE;
    while (dg_next_line(it, &line, &off)) {
        if (body != NONE) {
            if (str_eq(line, S("}"))) { body = NONE; continue; }
            Str r = line, comment = {};
            if (r.n && r.p[r.n - 1] == '"') {
                for (size_t k = r.n - 1; k-- > 0;)
                    if (r.p[k] == '"') { comment = (Str){.p = r.p + k + 1, .n = r.n - k - 2}; r = str_trim((Str){.p = r.p, .n = k}); break; }
            }
            Str type = dg_word(&r), name = dg_word(&r), keys = str_trim(r);
            if (!type.n || !name.n) return diag_fail(p->err_off, p->msg, off, "attribute needs a type and a name");
            Attr a = {.ent = body, .type = type, .name = name, .keys = keys, .comment = comment};
            bool ok;
            a.type = label_normalize(p->A, type, &ok); /* keep raw ~generic~ markers readable */
            if (!ok || !VEC_PUSH(p->A, &p->at, a)) return CMM_ERR_NOMEM;
            continue;
        }
        Str rest = line, w = dg_word(&rest);
        if (str_eq(w, S("direction"))) {
            Str d = dg_word(&rest);
            g->dir = str_eq(d, S("LR")) ? DIR_LR : str_eq(d, S("RL")) ? DIR_RL : str_eq(d, S("BT")) ? DIR_BT : DIR_TB;
            continue;
        }
        if (str_starts(w, S("accTitle")) || str_starts(w, S("accDescr")) || str_eq(w, S("classDef")) ||
            str_eq(w, S("class")) || str_eq(w, S("style")))
            continue;
        Str a, b, label;
        ERel r;
        if (parse_rel(line, &a, &b, &r, &label)) {
            int32_t va = ent(p, a), vb = ent(p, b);
            if (va == NONE || vb == NONE) return CMM_ERR_NOMEM;
            if (va < 0 || vb < 0) return diag_fail(p->err_off, p->msg, off, "relationship needs two entities");
            if (dg_edge(g, va, vb, label) == NONE || !VEC_PUSH(p->A, &p->rel, r)) return CMM_ERR_NOMEM;
            g->eend[g->ne - 1] = AH_NONE;
            continue;
        }
        Str ref = line;
        bool open = ref.n && ref.p[ref.n - 1] == '{';
        bool close_inline = false;
        if (open) ref = str_trim((Str){.p = ref.p, .n = ref.n - 1});
        else if (ref.n && ref.p[ref.n - 1] == '}') { /* ENTITY { } on one line */
            const char *ob = memchr(ref.p, '{', ref.n);
            if (ob) { ref.n = (size_t)(ob - ref.p); close_inline = true; }
        }
        for (size_t k = 0; k < ref.n; k++)
            if (ref.p[k] == ':' || ref.p[k] == '{' || ref.p[k] == '}' || ref.p[k] == '|')
                return diag_fail(p->err_off, p->msg, off + k, "unrecognised statement in ER diagram");
        int32_t v = ent(p, ref);
        if (v == NONE) return CMM_ERR_NOMEM;
        if (v < 0) return diag_fail(p->err_off, p->msg, off, "expected entity name");
        if (open && !close_inline) body = v;
    }
    if (body != NONE) return diag_fail(p->err_off, p->msg, it->n, "unterminated entity (missing '}')");
    return CMM_OK;
}

static constexpr char CSS[] =
    "#mermaid .ent{fill:#ECECFF;stroke:#9370DB;stroke-width:1px;}"
    "#mermaid .row0{fill:#ffffff;stroke:#9370DB;stroke-width:0.5px;}"
    "#mermaid .row1{fill:#f6f6ff;stroke:#9370DB;stroke-width:0.5px;}"
    "#mermaid .rel{fill:none;stroke:#333;stroke-width:1px;}#mermaid .rel.dash{stroke-dasharray:8,8;}"
    "#mermaid .mk{fill:none;stroke:#333;stroke-width:1px;}#mermaid .mkc{fill:#fff;stroke:#333;stroke-width:1px;}"
    "#mermaid .lbl{fill:#fff;opacity:0.85;}#mermaid .name{font-weight:bold;}#mermaid .attr{font-size:14px;}";

constexpr double ROW_PAD = 8;

/* crow's foot at P (entity boundary) pointing outward along unit (ux,uy) */
static void card_mark(Buf *b, double px, double py, double ux, double uy, Card c) {
    double nx = -uy, ny = ux;
    #define AT(d, s) px + ux * (d) + nx * (s), py + uy * (d) + ny * (s)
    buf_lit(b, "<path class=\"mk\" d=\"");
    bool many = c == CD_ONE_MANY || c == CD_ZERO_MANY;
    if (many) { buf_ch(b, 'M'); dg_pt(b, AT(0, -9)); buf_ch(b, 'L'); dg_pt(b, AT(16, 0)); buf_ch(b, 'L'); dg_pt(b, AT(0, 9)); }
    double bar = many ? 20 : 8;
    if (c != CD_ZERO_MANY) { buf_ch(b, 'M'); dg_pt(b, AT(bar, -8)); buf_ch(b, 'L'); dg_pt(b, AT(bar, 8)); }
    if (c == CD_ONE) { buf_ch(b, 'M'); dg_pt(b, AT(14, -8)); buf_ch(b, 'L'); dg_pt(b, AT(14, 8)); }
    buf_lit(b, "\"/>");
    if (c == CD_ZERO_ONE || c == CD_ZERO_MANY) {
        double d = c == CD_ZERO_ONE ? 20 : 24;
        buf_lit(b, "<circle class=\"mkc\" cx=\""); buf_num(b, px + ux * d); buf_lit(b, "\" cy=\""); buf_num(b, py + uy * d);
        buf_lit(b, "\" r=\"5\"/>");
    }
    #undef AT
}

DIAG_FN(er_render) {
    EP p = {.A = a, .err_off = err_off, .msg = msg};
    dg_init(&p.g, a, DIR_TB, fs);
    LineIt it = {.s = src, .n = n, .i = off};
    Str hdr;
    size_t ho;
    if (!dg_next_line(&it, &hdr, &ho)) return diag_fail(err_off, msg, off, "empty ER diagram");
    cmm_status st = parse(&p, &it);
    if (st) return st;
    Graph *g = &p.g;
    if (!g->nn) return diag_fail(err_off, msg, off, "ER diagram has no entities");
    size_t N = g->nn, M = p.at.n;
    Attr *as = ANEW(a, Attr, M + 1);
    if (!as) return CMM_ERR_NOMEM;
    for (size_t i = 0; i < M; i++) p.ei.v[p.at.v[i].ent].an++;
    uint32_t acc = 0;
    for (size_t v = 0; v < N; v++) { p.ei.v[v].a0 = acc; acc += p.ei.v[v].an; p.ei.v[v].an = 0; }
    for (size_t i = 0; i < M; i++) { EInfo *e = &p.ei.v[p.at.v[i].ent]; as[e->a0 + e->an++] = p.at.v[i]; }
    double afs = 14, rh = afs * 1.5 + ROW_PAD, hh = fs * 1.5 + 2 * ROW_PAD;
    for (size_t v = 0; v < N; v++) {
        EInfo *e = &p.ei.v[v];
        for (uint32_t i = 0; i < e->an; i++) {
            const Attr *t = &as[e->a0 + i];
            Str cols[4] = {t->type, t->name, t->keys, t->comment};
            for (unsigned k = 0; k < 4; k++) {
                double w = cols[k].n ? text_width(cols[k], afs) * 1.06 + 2 * ROW_PAD + 4 : 0;
                if (w > e->cw[k]) e->cw[k] = w;
            }
        }
        double tw = e->cw[0] + e->cw[1] + e->cw[2] + e->cw[3], nw = text_width(g->nlabel[v], fs) + 30;
        if (nw > tw) { double extra = nw - tw; e->cw[e->an ? 1 : 0] += extra; tw = nw; }
        if (tw < 100) { e->cw[0] += 100 - tw; tw = 100; }
        dg_size(g, (int32_t)v, tw, e->an ? hh + e->an * rh : 75);
    }
    if ((st = flow_layout(g))) return st;
    BBox bb = dg_bbox(g);
    if (title.n) bb.y0 -= 30;
    if (!buf_reserve(out, 2048 + N * 400 + g->ne * 400 + M * 300)) return CMM_ERR_NOMEM;
    DG_HEAD(out, bb, 12, "erDiagram", CSS);
    if (title.n) dg_text(out, title, 18, (bb.x0 + bb.x1) / 2, bb.y0 + 12, "middle", "name");
    for (size_t e = 0; e < g->ne; e++) {
        const ERel *r = &p.rel.v[e];
        if (!g->eptn[e] && g->efrom[e] != g->eto[e]) continue;
        buf_lit(out, "<path class=\"rel");
        if (r->dashed) buf_lit(out, " dash");
        buf_lit(out, "\" d=\"");
        dg_edge_d(out, g, e);
        buf_lit(out, "\"/>");
        uint32_t np = g->eptn[e];
        if (np >= 2) {
            const double *P = g->pts.v + g->eptoff[e];
            for (unsigned end = 0; end < 2; end++) {
                const double *A0 = end ? P + 2 * (np - 1) : P, *A1 = end ? P + 2 * (np - 2) : P + 2;
                double dx = A1[0] - A0[0], dy = A1[1] - A0[1], L = sqrt(dx * dx + dy * dy);
                if (L > 1e-9) card_mark(out, A0[0], A0[1], dx / L, dy / L, end ? r->cb : r->ca);
            }
        }
        if (g->elabel[e].n) {
            dg_rect(out, g->elx[e] - g->elw[e] / 2, g->ely[e] - g->elh[e] / 2, g->elw[e], g->elh[e], 0, "lbl");
            dg_text(out, g->elabel[e], fs, g->elx[e], g->ely[e], "middle", nullptr);
        }
    }
    for (size_t v = 0; v < N; v++) {
        const EInfo *e = &p.ei.v[v];
        double w = g->nw[v], h = g->nh[v], x0 = g->nx[v] - w / 2, y0 = g->ny[v] - h / 2;
        dg_rect(out, x0, y0, w, h, 0, "ent");
        if (!e->an) { dg_text(out, g->nlabel[v], fs, g->nx[v], g->ny[v], "middle", "name"); continue; }
        dg_text(out, g->nlabel[v], fs, g->nx[v], y0 + hh / 2, "middle", "name");
        double y = y0 + hh;
        for (uint32_t i = 0; i < e->an; i++) {
            const Attr *t = &as[e->a0 + i];
            Str cols[4] = {t->type, t->name, t->keys, t->comment};
            double x = x0;
            for (unsigned k = 0; k < 4; k++) {
                if (e->cw[k] <= 0) continue;
                dg_rect(out, x, y, e->cw[k], rh, 0, i & 1 ? "row1" : "row0");
                if (cols[k].n) dg_text(out, cols[k], afs, x + ROW_PAD, y + rh / 2, "start", "attr");
                x += e->cw[k];
            }
            y += rh;
        }
    }
    buf_lit(out, "</svg>\n");
    return out->oom ? CMM_ERR_NOMEM : CMM_OK;
}
