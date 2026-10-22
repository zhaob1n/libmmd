/* requirementDiagram: requirement-type blocks (requirement, functionalRequirement, interfaceRequirement,
 * performanceRequirement, physicalRequirement, designConstraint) with id/text/risk/verifymethod,
 * element blocks with type/docref, relations `a - rel -> b` / `b <- rel - a`
 * (contains, copies, derives, satisfies, verifies, refines, traces). Layered layout via flow_layout. */
#include <math.h>

#include "dgraph.h"
#include "diagrams.h"
#include "text.h"

typedef struct RInfo { Str kind, f[4]; bool elem; } RInfo; /* f: id,text,risk,verify | type,docref */
typedef struct RRel { Str type; } RRel;

static const struct { const char *kw, *disp; } KINDS[] = {
    {"requirement", "Requirement"}, {"functionalRequirement", "Functional Requirement"},
    {"interfaceRequirement", "Interface Requirement"}, {"performanceRequirement", "Performance Requirement"},
    {"physicalRequirement", "Physical Requirement"}, {"designConstraint", "Design Constraint"}, {"element", "Element"}};
static const char *const RELS[] = {"contains", "copies", "derives", "satisfies", "verifies", "refines", "traces"};

static constexpr char CSS[] =
    "#mermaid .rq{fill:#ECECFF;stroke:#9370DB;stroke-width:1px;}#mermaid .rqd{stroke:#9370DB;stroke-width:1px;}"
    "#mermaid .rr{fill:none;stroke:#333;stroke-width:1px;stroke-dasharray:10,7;}#mermaid .rr.c{stroke-dasharray:none;}"
    "#mermaid .rh{fill:#333;}#mermaid .rl{fill:#fff;opacity:.9;}#mermaid .rn{font-weight:bold;}#mermaid .rt{font-size:14px;}";

static Str unq(Str s) {
    s = str_trim(s);
    if (s.n >= 2 && s.p[0] == '"' && s.p[s.n - 1] == '"') s = (Str){.p = s.p + 1, .n = s.n - 2};
    return s;
}
static Str cap(Arena *a, Str s) { /* Medium / Test: capitalise like mermaid */
    if (!s.n || !(s.p[0] >= 'a' && s.p[0] <= 'z')) return s;
    char *o = ANEW(a, char, s.n);
    if (!o) return s;
    memcpy(o, s.p, s.n);
    o[0] = (char)(o[0] - 32);
    return (Str){.p = o, .n = s.n};
}

static void arrowhead(Buf *b, const double *P, uint32_t np) {
    if (np < 2) return;
    const double *A = P + 2 * (np - 1), *B = P + 2 * (np - 2);
    double dx = A[0] - B[0], dy = A[1] - B[1], L = sqrt(dx * dx + dy * dy);
    if (L < 1e-9) return;
    dx /= L, dy /= L;
    buf_lit(b, "<path class=\"rh\" d=\"M"); dg_pt(b, A[0], A[1]); buf_ch(b, 'L');
    dg_pt(b, A[0] - dx * 10 - dy * 5, A[1] - dy * 10 + dx * 5); buf_ch(b, 'L');
    dg_pt(b, A[0] - dx * 10 + dy * 5, A[1] - dy * 10 - dx * 5); buf_lit(b, "z\"/>");
}

DIAG_FN(requirement_render) {
    Graph g;
    dg_init(&g, a, DIR_TB, fs);
    LineIt it = {.s = src, .n = n, .i = off};
    Str line;
    size_t lo;
    if (!dg_next_line(&it, &line, &lo)) return diag_fail(err_off, msg, off, "empty requirement diagram");
    VEC(RInfo) ri = {};
    VEC(RRel) rr = {};
    int32_t body = NONE;
    while (dg_next_line(&it, &line, &lo)) {
        if (body != NONE) {
            if (str_eq(line, S("}"))) { body = NONE; continue; }
            const char *c = memchr(line.p, ':', line.n);
            if (!c) return diag_fail(err_off, msg, lo, "expected 'key: value'");
            Str k = str_trim((Str){.p = line.p, .n = (size_t)(c - line.p)}), v = unq((Str){.p = c + 1, .n = line.n - (size_t)(c + 1 - line.p)});
            RInfo *r = &ri.v[body];
            int slot = -1;
            if (r->elem) slot = str_ieq(k, S("type")) ? 0 : str_ieq(k, S("docref")) ? 1 : -1;
            else slot = str_ieq(k, S("id")) ? 0 : str_ieq(k, S("text")) ? 1 : str_ieq(k, S("risk")) ? 2 : str_ieq(k, S("verifymethod")) ? 3 : -1;
            if (slot < 0) return diag_fail(err_off, msg, lo, "unknown requirement field");
            r->f[slot] = slot >= 2 ? cap(a, v) : v;
            continue;
        }
        if (str_starts(line, S("direction "))) {
            Str d = str_trim((Str){.p = line.p + 10, .n = line.n - 10});
            g.dir = str_eq(d, S("LR")) ? DIR_LR : str_eq(d, S("RL")) ? DIR_RL : str_eq(d, S("BT")) ? DIR_BT : DIR_TB;
            continue;
        }
        if (str_starts(line, S("style ")) || str_starts(line, S("classDef ")) || str_starts(line, S("class "))) continue;
        Str l = line, w = dg_word(&l);
        size_t kind = countof(KINDS);
        for (size_t k = 0; k < countof(KINDS); k++)
            if (w.n == strlen(KINDS[k].kw) && !memcmp(w.p, KINDS[k].kw, w.n)) kind = k;
        l = str_trim(l);
        if (kind < countof(KINDS) && l.n && l.p[l.n - 1] == '{') {
            Str name = unq((Str){.p = l.p, .n = l.n - 1});
            if (!name.n) return diag_fail(err_off, msg, lo, "expected requirement name");
            bool fresh;
            int32_t v = dg_node(&g, name, &fresh);
            if (v == NONE) return CMM_ERR_NOMEM;
            while (ri.n <= (size_t)v) if (!VEC_PUSH(a, &ri, (RInfo){})) return CMM_ERR_NOMEM;
            ri.v[v].kind = (Str){.p = KINDS[kind].disp, .n = strlen(KINDS[kind].disp)};
            ri.v[v].elem = kind == countof(KINDS) - 1;
            body = v;
            continue;
        }
        /* relation */
        const char *s = line.p, *e = line.p + line.n;
        const char *d1 = nullptr, *d2 = nullptr;
        bool rev = false;
        for (const char *q = s; q + 1 < e; q++) {
            if (!d1 && q[0] == '<' && q[1] == '-') { d1 = q, rev = true; q++; continue; }
            if (!d1 && q[0] == '-' && q > s && is_sp(q[-1])) { d1 = q; continue; }
            if (d1 && !d2 && q[0] == '-' && (rev || q[1] == '>')) { d2 = q; break; }
        }
        if (!d1 || !d2) return diag_fail(err_off, msg, lo, "expected requirement, element or relation");
        Str A = unq((Str){.p = s, .n = (size_t)(d1 - s)});
        Str rel = str_trim((Str){.p = d1 + (rev ? 2 : 1), .n = (size_t)(d2 - d1 - (rev ? 2 : 1))});
        Str B = unq((Str){.p = d2 + (rev ? 1 : 2), .n = (size_t)(e - d2 - (rev ? 1 : 2))});
        bool okr = false;
        for (size_t k = 0; k < countof(RELS); k++) okr |= rel.n == strlen(RELS[k]) && !memcmp(rel.p, RELS[k], rel.n);
        if (!okr) return diag_fail(err_off, msg, lo, "unknown relationship type");
        if (!A.n || !B.n) return diag_fail(err_off, msg, lo, "relation needs two ends");
        if (rev) { Str t = A; A = B; B = t; }
        int32_t va = dg_node(&g, A, nullptr), vb = va == NONE ? NONE : dg_node(&g, B, nullptr);
        if (vb == NONE) return CMM_ERR_NOMEM;
        while (ri.n < g.nn) if (!VEC_PUSH(a, &ri, (RInfo){})) return CMM_ERR_NOMEM;
        char *lab = ANEW(a, char, rel.n + 4);
        if (!lab) return CMM_ERR_NOMEM;
        memcpy(lab, "<<", 2), memcpy(lab + 2, rel.p, rel.n), memcpy(lab + 2 + rel.n, ">>", 2);
        if (dg_edge(&g, va, vb, (Str){.p = lab, .n = rel.n + 4}) == NONE) return CMM_ERR_NOMEM;
        g.eend[g.ne - 1] = AH_NONE;
        if (!VEC_PUSH(a, &rr, ((RRel){.type = rel}))) return CMM_ERR_NOMEM;
    }
    if (body != NONE) return diag_fail(err_off, msg, n, "unterminated block (missing '}')");
    if (!g.nn) return diag_fail(err_off, msg, off, "requirement diagram is empty");
    double rfs = 14, lh = rfs * 1.5;
    static const char *const RLAB[] = {"ID: ", "Text: ", "Risk: ", "Verification: "};
    static const char *const ELAB[] = {"Type: ", "Doc Ref: "};
    for (size_t v = 0; v < g.nn; v++) {
        RInfo *r = &ri.v[v];
        if (!r->kind.n) r->kind = S("Element"), r->elem = true; /* referenced only */
        double w = fmax(text_width(r->kind, rfs) + 30, text_width(g.nlabel[v], fs) + 10);
        unsigned rows = 0;
        for (unsigned k = 0; k < (r->elem ? 2u : 4u); k++) {
            if (!r->f[k].n) continue;
            const char *pfx = r->elem ? ELAB[k] : RLAB[k];
            w = fmax(w, text_width((Str){.p = pfx, .n = strlen(pfx)}, rfs) + text_width(r->f[k], rfs));
            rows++;
        }
        dg_size(&g, (int32_t)v, fmax(w + 30, 140), 2 * lh + fs * 0.5 + 16 + rows * lh + (rows ? 10 : 0));
    }
    cmm_status st = flow_layout(&g);
    if (st) return st;
    BBox bb = dg_bbox(&g);
    if (title.n) bb.y0 -= 30;
    if (!buf_reserve(out, 2048 + g.nn * 700 + g.ne * 400)) return CMM_ERR_NOMEM;
    DG_HEAD(out, bb, 12, "requirementDiagram", CSS);
    if (title.n) dg_text(out, title, 18, (bb.x0 + bb.x1) / 2, bb.y0 + 12, "middle", "rn");
    for (size_t e = 0; e < g.ne; e++) {
        if (!g.eptn[e]) continue;
        bool cont = str_eq(rr.v[e].type, S("contains"));
        buf_lit(out, "<path class=\"rr");
        if (cont) buf_lit(out, " c");
        buf_lit(out, "\" d=\"");
        dg_edge_d(out, &g, e);
        buf_lit(out, "\"/>");
        const double *P = g.pts.v + g.eptoff[e];
        if (cont) { /* circle-plus at the container (source) end */
            double dx = P[2] - P[0], dy = P[3] - P[1], L = sqrt(dx * dx + dy * dy);
            if (L > 1e-9) {
                double cx = P[0] + dx / L * 8, cy = P[1] + dy / L * 8;
                buf_lit(out, "<circle class=\"rq\" cx=\""); buf_num(out, cx); buf_lit(out, "\" cy=\""); buf_num(out, cy); buf_lit(out, "\" r=\"8\"/>");
                dg_line(out, cx - 8, cy, cx + 8, cy, "rqd");
                dg_line(out, cx, cy - 8, cx, cy + 8, "rqd");
            }
        } else arrowhead(out, P, g.eptn[e]);
        dg_rect(out, g.elx[e] - g.elw[e] / 2, g.ely[e] - g.elh[e] / 2, g.elw[e], g.elh[e], 0, "rl");
        { /* mermaid draws relation labels on a light grey plate */
            double lw, lh;
            unsigned ln;
            label_measure(g.elabel[e], fs, &lw, &ln);
            lh = ln * fs * 1.3;
            buf_lit(out, "<rect fill=\"#e8e8e8\" fill-opacity=\".8\" x=\""); buf_num(out, g.elx[e] - lw / 2 - 2); buf_lit(out, "\" y=\""); buf_num(out, g.ely[e] - lh / 2);
            buf_lit(out, "\" width=\""); buf_num(out, lw + 4); buf_lit(out, "\" height=\""); buf_num(out, lh); buf_lit(out, "\"/>");
        }
        dg_text(out, g.elabel[e], fs, g.elx[e], g.ely[e], "middle", nullptr);
    }
    for (size_t v = 0; v < g.nn; v++) {
        const RInfo *r = &ri.v[v];
        double w = g.nw[v], h = g.nh[v], x0 = g.nx[v] - w / 2, y0 = g.ny[v] - h / 2, y = y0 + 8 + lh / 2;
        buf_lit(out, "<g class=\"req\">");
        dg_rect(out, x0, y0, w, h, 0, "rq");
        buf_lit(out, "<text class=\"rt\" text-anchor=\"middle\" dominant-baseline=\"central\" x=\""); buf_num(out, g.nx[v]);
        buf_lit(out, "\" y=\""); buf_num(out, y); buf_lit(out, "\">&lt;&lt;"); buf_xml(out, r->kind); buf_lit(out, "&gt;&gt;</text>");
        y += lh;
        dg_text(out, g.nlabel[v], fs, g.nx[v], y, "middle", "rn");
        y += lh / 2 + fs * 0.25 + 4;
        bool any = false;
        for (unsigned k = 0; k < (r->elem ? 2u : 4u); k++) any |= r->f[k].n > 0;
        if (any) dg_line(out, x0, y, x0 + w, y, "rqd");
        y += 5 + lh / 2;
        for (unsigned k = 0; k < (r->elem ? 2u : 4u); k++) {
            if (!r->f[k].n) continue;
            const char *pfx = r->elem ? ELAB[k] : RLAB[k];
            dg_text_open(out, x0 + 12, y, "start", "rt");
            buf_put(out, strlen(pfx), pfx);
            buf_xml(out, r->f[k]);
            buf_lit(out, "</text>");
            y += lh;
        }
        buf_lit(out, "</g>");
    }
    buf_lit(out, "</svg>\n");
    return out->oom ? CMM_ERR_NOMEM : CMM_OK;
}
