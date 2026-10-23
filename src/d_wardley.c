/* wardley-beta: port of mermaid's Wardley map renderer (axes, stages, components, anchors,
 * sourcing overlays, inertia, links with flow arrows, evolve trends, pipelines, notes,
 * annotations, (de)accelerators). Coordinates are [visibility, evolution] in 0-1 or 0-100. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dgraph.h"
#include "dcfg.h"
#include "diagrams.h"
#include "text.h"

typedef enum : uint8_t { WK_ANCHOR, WK_COMP, WK_PIPEC } WKind;
typedef enum : uint8_t { SS_NONE, SS_BUILD, SS_BUY, SS_OUT, SS_MARKET } Strat;
typedef struct WNode {
    Str id, label;
    double x, y; /* percent */
    double px, py;
    double lox, loy;
    bool has_lo, inertia, pparent;
    WKind k;
    Strat ss;
} WNode;
typedef struct WLink { size_t s, t; bool dashed; uint8_t flow; /* 1 fwd 2 back 3 both */ Str label; } WLink;
typedef struct WPipe { size_t parent; size_t first, n; } WPipe; /* pipeline components: node range */
typedef struct WPt { Str s; double x, y; unsigned num; } WPt;

typedef struct W {
    Arena *a;
    VEC(WNode) nodes;
    VEC(WLink) links;
    VEC(WPipe) pipes;
    VEC(WPt) notes, acc, dec, ann, trends; /* trends: num = node index, x = target */
    VEC(Str) stages;
    VEC(double) bounds;
    double width, height;
    bool has_box;
    double boxx, boxy;
} W;

static Str unq(Str s) {
    s = str_trim(s);
    if (s.n >= 2 && s.p[0] == '"' && s.p[s.n - 1] == '"') s.p++, s.n -= 2;
    return s;
}
static bool pct(double v, double *o) {
    double r = v <= 1 ? v * 100 : v;
    if (!(r >= 0 && r <= 100)) return false;
    *o = r;
    return true;
}
/* "[a, b]" at the start of s (after spaces); advances s */
static int brack(Str *s, double *a, double *b) {
    Str t = str_trim(*s);
    if (!t.n || t.p[0] != '[') return 0;
    size_t e = 1;
    while (e < t.n && t.p[e] != ']') e++;
    if (e >= t.n) return -1;
    Str in = {.p = t.p + 1, .n = e - 1};
    size_t c = 0;
    while (c < in.n && in.p[c] != ',') c++;
    int got;
    if (c < in.n) {
        if (!dg_parse_num(str_trim((Str){.p = in.p, .n = c}), a) || !dg_parse_num(str_trim((Str){.p = in.p + c + 1, .n = in.n - c - 1}), b)) return -1;
        got = 2;
    } else {
        if (!dg_parse_num(str_trim(in), a)) return -1;
        got = 1;
    }
    *s = (Str){.p = t.p + e + 1, .n = t.n - e - 1};
    return got;
}
/* find the first '[' outside quotes; name = text before it */
static size_t find_br(Str s) {
    bool q = false;
    for (size_t i = 0; i < s.n; i++) {
        if (s.p[i] == '"') q = !q;
        else if (s.p[i] == '[' && !q) return i;
    }
    return s.n;
}
static ptrdiff_t node_find(const W *w, Str name) {
    for (size_t i = 0; i < w->nodes.n; i++) if (str_eq(w->nodes.v[i].id, name)) return (ptrdiff_t)i;
    for (size_t i = 0; i < w->nodes.n; i++) if (str_eq(w->nodes.v[i].label, name)) return (ptrdiff_t)i;
    return -1;
}

/* component tail: "label [x, y]" and "(build|buy|outsource|market|inertia)" decorators */
static const char *decorators(Str r, WNode *nd) {
    for (;;) {
        r = str_trim(r);
        if (!r.n) return nullptr;
        if (str_starts(r, S("label"))) {
            r.p += 5, r.n -= 5;
            double a, b;
            if (brack(&r, &a, &b) != 2) return "expected label [x, y]";
            nd->lox = a, nd->loy = b, nd->has_lo = true;
            continue;
        }
        if (r.p[0] == '(') {
            size_t e = 1;
            while (e < r.n && r.p[e] != ')') e++;
            if (e >= r.n) return "unclosed '('";
            Str d = str_trim((Str){.p = r.p + 1, .n = e - 1});
            if (str_eq(d, S("inertia"))) nd->inertia = true;
            else if (str_eq(d, S("build"))) nd->ss = SS_BUILD;
            else if (str_eq(d, S("buy"))) nd->ss = SS_BUY;
            else if (str_eq(d, S("outsource"))) nd->ss = SS_OUT;
            else if (str_eq(d, S("market"))) nd->ss = SS_MARKET;
            else return "unknown decorator";
            r.p += e + 1, r.n -= e + 1;
            continue;
        }
        return "unexpected text after component";
    }
}

static const char *add_node(W *w, WNode nd) {
    ptrdiff_t e = -1;
    for (size_t i = 0; i < w->nodes.n; i++) if (str_eq(w->nodes.v[i].id, nd.id)) e = (ptrdiff_t)i;
    if (e >= 0) { w->nodes.v[e] = nd; return nullptr; }
    return VEC_PUSH(w->a, &w->nodes, nd) ? nullptr : "out of memory";
}

/* link operators, longest first */
static const char *const OPS[] = {"+<>", "-.->", "+<", "+>", "->", "+'"};

static const char *parse_link(W *w, Str ln, bool *matched, Str *from_out, Str *to_out, WLink *lk) {
    *matched = false;
    bool q = false;
    for (size_t i = 0; i < ln.n; i++) {
        if (ln.p[i] == '"') { q = !q; continue; }
        if (q) continue;
        for (size_t k = 0; k < countof(OPS); k++) {
            size_t ol = strlen(OPS[k]);
            if (ln.n - i < ol || memcmp(ln.p + i, OPS[k], ol)) continue;
            *matched = true;
            *lk = (WLink){};
            size_t j = i + ol;
            if (k == 5) { /* +'label'> or +'label'<> or +'label'< */
                size_t e = j;
                while (e < ln.n && ln.p[e] != '\'') e++;
                if (e >= ln.n) return "unterminated flow label";
                lk->label = (Str){.p = ln.p + j, .n = e - j};
                j = e + 1;
                if (j + 1 < ln.n && ln.p[j] == '<' && ln.p[j + 1] == '>') lk->flow = 3, j += 2;
                else if (j < ln.n && ln.p[j] == '<') lk->flow = 2, j++;
                else if (j < ln.n && ln.p[j] == '>') lk->flow = 1, j++;
                else return "expected '>' after flow label";
            } else {
                lk->flow = k == 0 ? 3 : k == 2 ? 2 : k == 3 ? 1 : 0;
                lk->dashed = k == 1;
            }
            Str to = {.p = ln.p + j, .n = ln.n - j};
            size_t sc = 0;
            while (sc < to.n && to.p[sc] != ';') sc++;
            if (sc < to.n) {
                if (!lk->label.n) lk->label = str_trim((Str){.p = to.p + sc + 1, .n = to.n - sc - 1});
                to.n = sc;
            }
            *from_out = unq((Str){.p = ln.p, .n = i}), *to_out = unq(to);
            if (!from_out->n || !to_out->n) return "link needs two components";
            (void)w;
            return nullptr;
        }
    }
    return nullptr;
}

/* ---- drawing helpers ---- */
static void bs(Buf *b, const char *s) { buf_put(b, strlen(s), s); }
static void attr(Buf *b, const char *k, double v) {
    buf_ch(b, ' '); buf_put(b, strlen(k), k); buf_lit(b, "=\""); buf_num(b, v); buf_ch(b, '"');
}
static void line(Buf *b, double x1, double y1, double x2, double y2, const char *extra) {
    buf_lit(b, "<line"); attr(b, "x1", x1); attr(b, "y1", y1); attr(b, "x2", x2); attr(b, "y2", y2);
    buf_put(b, strlen(extra), extra); buf_lit(b, "/>");
}
static void circle(Buf *b, double cx, double cy, double r, const char *extra) {
    buf_lit(b, "<circle"); attr(b, "cx", cx); attr(b, "cy", cy); attr(b, "r", r);
    buf_put(b, strlen(extra), extra); buf_lit(b, "/>");
}
static void text(Buf *b, double x, double y, const char *extra, Str s) {
    buf_lit(b, "<text"); attr(b, "x", x); attr(b, "y", y); buf_put(b, strlen(extra), extra); buf_ch(b, '>');
    buf_xml(b, s); buf_lit(b, "</text>");
}

constexpr double PAD = 48;
constexpr double NR = 6;
constexpr double NLO = 8;
/* hits of segment (x0,y0)-(x1,y1) with box b (x0 y0 x1 y1), sampled */
/* how much of segment (x0,y0)-(x1,y1) lies inside box b (Liang-Barsky); 0 when it misses */
static double seg_box_len(double x0, double y0, double x1, double y1, const double b[static 4]) {
    double t0 = 0, t1 = 1, dx = x1 - x0, dy = y1 - y0;
    const double P[4] = {-dx, dx, -dy, dy}, Q[4] = {x0 - b[0], b[2] - x0, y0 - b[1], b[3] - y0};
    for (int k = 0; k < 4; k++) {
        if (fabs(P[k]) < 1e-12) { if (Q[k] < 0) return 0; continue; }
        double r = Q[k] / P[k];
        if (P[k] < 0) t0 = fmax(t0, r); else t1 = fmin(t1, r);
        if (t0 >= t1) return 0;
    }
    return (t1 - t0) * sqrt(dx * dx + dy * dy);
}
/* label cost of a segment crossing a box: a flat charge plus its length inside */
static double seg_box_hits(double x0, double y0, double x1, double y1, const double b[static 4]) {
    double l = seg_box_len(x0, y0, x1, y1, b);
    return l > 0 ? 3 + l / 3 : 0;
}

#define TXT "#131300"
#define STK "#333333"

DIAG_FN(wardley_render) {
    (void)fs;
    LineIt it = {.s = src, .n = n, .i = off};
    Str ln;
    size_t lo;
    if (!dg_next_line(&it, &ln, &lo)) return diag_fail(err_off, msg, off, "empty wardley map");
    W w = {.a = a, .width = 900, .height = 600};
    Str ttl = title;
    ptrdiff_t pipe = -1; /* index into w.pipes while inside pipeline { } */
#define FAIL(m) return (strcmp((m), "out of memory") ? diag_fail(err_off, msg, lo, (m)) : CMM_ERR_NOMEM)
    while (dg_next_line(&it, &ln, &lo)) {
        /* strip trailing %% comments */
        for (size_t i = 0; i + 1 < ln.n; i++) if (ln.p[i] == '%' && ln.p[i + 1] == '%') { ln.n = i; break; }
        ln = str_trim(ln);
        if (!ln.n) continue;
        bool block;
        if (dcfg_is_acc(ln, &block)) continue;
        Str r = ln, kw = dg_word(&r);
        if (pipe >= 0) {
            if (str_eq(ln, S("}"))) { pipe = -1; continue; }
            if (!str_eq(kw, S("component"))) FAIL("expected component or '}' in pipeline");
            size_t bi = find_br(r);
            Str name = unq((Str){.p = r.p, .n = bi});
            Str rest = {.p = r.p + bi, .n = r.n - bi};
            double e, dummy;
            if (!name.n || brack(&rest, &e, &dummy) != 1) FAIL("expected pipeline component \"name\" [evolution]");
            WNode nd = {.k = WK_PIPEC, .label = name, .y = w.nodes.v[w.pipes.v[pipe].parent].y};
            if (!pct(e, &nd.x)) FAIL("evolution must be between 0-1 or 0-100");
            const char *err = decorators(rest, &nd);
            if (err) FAIL(err);
            Str pid = w.nodes.v[w.pipes.v[pipe].parent].id;
            char *idb = arena_alloc_array(a, pid.n + name.n + 1, 1, 1);
            if (!idb) return CMM_ERR_NOMEM;
            memcpy(idb, pid.p, pid.n), idb[pid.n] = '_', memcpy(idb + pid.n + 1, name.p, name.n);
            nd.id = (Str){.p = idb, .n = pid.n + 1 + name.n};
            if ((err = add_node(&w, nd))) FAIL(err);
            w.pipes.v[pipe].n++;
            continue;
        }
        if (str_eq(kw, S("title"))) { ttl = str_trim(r); continue; }
        if (str_eq(kw, S("size"))) {
            double sw, sh;
            if (brack(&r, &sw, &sh) != 2 || !(sw >= 50 && sw <= 20000 && sh >= 50 && sh <= 20000)) FAIL("expected size [width, height]");
            w.width = sw, w.height = sh;
            continue;
        }
        if (str_eq(kw, S("evolution"))) {
            w.stages.n = w.bounds.n = 0;
            Str s = r;
            while (s.n) {
                size_t e = 0;
                while (e < s.n && !(e + 1 < s.n && s.p[e] == '-' && s.p[e + 1] == '>')) e++;
                Str st = str_trim((Str){.p = s.p, .n = e});
                for (size_t k = 0; k < st.n; k++) if (st.p[k] == '@') {
                    double bv;
                    if (!dg_parse_num(str_trim((Str){.p = st.p + k + 1, .n = st.n - k - 1}), &bv)) FAIL("bad stage boundary");
                    if (!VEC_PUSH(a, &w.bounds, bv)) return CMM_ERR_NOMEM;
                    st = str_trim((Str){.p = st.p, .n = k});
                    break;
                }
                /* normalise "A / B" spacing */
                for (size_t k = 0; k < st.n; k++) if (st.p[k] == '/') {
                    Str l = str_trim((Str){.p = st.p, .n = k}), rr = str_trim((Str){.p = st.p + k + 1, .n = st.n - k - 1});
                    char *o = arena_alloc_array(a, l.n + rr.n + 3, 1, 1);
                    if (!o) return CMM_ERR_NOMEM;
                    memcpy(o, l.p, l.n), memcpy(o + l.n, " / ", 3), memcpy(o + l.n + 3, rr.p, rr.n);
                    st = (Str){.p = o, .n = l.n + 3 + rr.n};
                    break;
                }
                if (!st.n) FAIL("empty evolution stage");
                if (!VEC_PUSH(a, &w.stages, st)) return CMM_ERR_NOMEM;
                if (e >= s.n) break;
                s = (Str){.p = s.p + e + 2, .n = s.n - e - 2};
            }
            continue;
        }
        if (str_eq(kw, S("anchor")) || str_eq(kw, S("component"))) {
            size_t bi = find_br(r);
            Str name = unq((Str){.p = r.p, .n = bi});
            Str rest = {.p = r.p + bi, .n = r.n - bi};
            double v, e;
            if (!name.n || brack(&rest, &v, &e) != 2) FAIL("expected name [visibility, evolution]");
            WNode nd = {.k = kw.p[0] == 'a' ? WK_ANCHOR : WK_COMP, .id = name, .label = name};
            if (!pct(e, &nd.x) || !pct(v, &nd.y)) FAIL("coordinates must be between 0-1 or 0-100");
            const char *err = decorators(rest, &nd);
            if (err) FAIL(err);
            if ((err = add_node(&w, nd))) FAIL(err);
            continue;
        }
        if (str_eq(kw, S("pipeline"))) {
            Str nm = str_trim(r);
            if (!nm.n || nm.p[nm.n - 1] != '{') FAIL("expected pipeline Name {");
            nm = unq((Str){.p = nm.p, .n = nm.n - 1});
            ptrdiff_t pi = node_find(&w, nm);
            if (pi < 0) FAIL("pipeline must reference an existing component");
            w.nodes.v[pi].pparent = true;
            if (!VEC_PUSH(a, &w.pipes, ((WPipe){.parent = (size_t)pi, .first = w.nodes.n}))) return CMM_ERR_NOMEM;
            pipe = (ptrdiff_t)w.pipes.n - 1;
            continue;
        }
        if (str_eq(kw, S("evolve"))) {
            Str t = str_trim(r);
            size_t sp = t.n;
            while (sp > 0 && t.p[sp - 1] != ' ' && t.p[sp - 1] != '\t') sp--;
            double tv, tp;
            if (!sp || !dg_parse_num((Str){.p = t.p + sp, .n = t.n - sp}, &tv) || !pct(tv, &tp)) FAIL("expected evolve Name target");
            ptrdiff_t ni = node_find(&w, unq((Str){.p = t.p, .n = sp}));
            if (ni >= 0) {
                bool dup = false;
                for (size_t k = 0; k < w.trends.n; k++) if (w.trends.v[k].num == (unsigned)ni) w.trends.v[k].x = tp, dup = true;
                if (!dup && !VEC_PUSH(a, &w.trends, ((WPt){.num = (unsigned)ni, .x = tp, .y = w.nodes.v[ni].y}))) return CMM_ERR_NOMEM;
            }
            continue;
        }
        if (str_eq(kw, S("note")) || str_eq(kw, S("accelerator")) || str_eq(kw, S("deaccelerator"))) {
            size_t bi = find_br(r);
            Str name = unq((Str){.p = r.p, .n = bi});
            Str rest = {.p = r.p + bi, .n = r.n - bi};
            double v, e;
            WPt p = {.s = name};
            if (brack(&rest, &v, &e) != 2 || !pct(e, &p.x) || !pct(v, &p.y)) FAIL("expected \"text\" [visibility, evolution]");
            if (!VEC_PUSH(a, kw.p[0] == 'n' ? &w.notes : kw.p[0] == 'a' ? &w.acc : &w.dec, p)) return CMM_ERR_NOMEM;
            continue;
        }
        if (str_eq(kw, S("annotations"))) {
            double v, e;
            if (brack(&r, &v, &e) != 2 || !pct(e, &w.boxx) || !pct(v, &w.boxy)) FAIL("expected annotations [x, y]");
            w.has_box = true;
            continue;
        }
        if (str_eq(kw, S("annotation"))) {
            Str t = str_trim(r);
            size_t c = 0;
            while (c < t.n && t.p[c] != ',') c++;
            double num;
            if (c >= t.n || !dg_parse_num(str_trim((Str){.p = t.p, .n = c}), &num) || !(num >= 0 && num < 1e6)) FAIL("expected annotation N,[x, y] \"text\"");
            Str rest = {.p = t.p + c + 1, .n = t.n - c - 1};
            double v, e;
            WPt p = {.num = (unsigned)num};
            if (brack(&rest, &v, &e) != 2 || !pct(e, &p.x) || !pct(v, &p.y)) FAIL("expected annotation N,[x, y] \"text\"");
            p.s = unq(rest);
            if (!VEC_PUSH(a, &w.ann, p)) return CMM_ERR_NOMEM;
            continue;
        }
        bool m;
        Str fa, ta;
        WLink lk;
        const char *err = parse_link(&w, ln, &m, &fa, &ta, &lk);
        if (err) FAIL(err);
        if (!m) FAIL("unrecognised wardley statement");
        ptrdiff_t si = node_find(&w, fa), ti = node_find(&w, ta);
        if (si < 0 || ti < 0) continue; /* mermaid drops links to unknown nodes */
        lk.s = (size_t)si, lk.t = (size_t)ti;
        if (!VEC_PUSH(a, &w.links, lk)) return CMM_ERR_NOMEM;
    }
#undef FAIL
    if (pipe >= 0) return diag_fail(err_off, msg, n, "unclosed pipeline");

    double W_ = w.width, H = w.height, cw = W_ - 2 * PAD, ch = H - 2 * PAD, sq = NR * 1.6;
#define PX(v) (PAD + (v) / 100 * cw)
#define PY(v) (H - PAD - (v) / 100 * ch)
    for (size_t i = 0; i < w.nodes.n; i++) w.nodes.v[i].px = PX(w.nodes.v[i].x), w.nodes.v[i].py = PY(w.nodes.v[i].y);

    Buf b = {};
    buf_lit(&b, "<g class=\"wardley-map\"><rect class=\"wardley-background\""); attr(&b, "width", W_); attr(&b, "height", H);
    buf_lit(&b, " fill=\"white\"/>");
    if (ttl.n) text(&b, W_ / 2, PAD / 2, " class=\"wardley-title\" fill=\"" TXT "\" font-size=\"12.6\" font-weight=\"bold\" text-anchor=\"middle\" dominant-baseline=\"middle\"", ttl);
    buf_lit(&b, "<g class=\"wardley-axes\">");
    line(&b, PAD, H - PAD, W_ - PAD, H - PAD, " stroke=\"" STK "\" stroke-width=\"1\"");
    line(&b, PAD, PAD, PAD, H - PAD, " stroke=\"" STK "\" stroke-width=\"1\"");
    text(&b, PAD + cw / 2, H - PAD / 4, " class=\"wardley-axis-label\" fill=\"" TXT "\" font-size=\"12\" font-weight=\"bold\" text-anchor=\"middle\"", S("Evolution"));
    {
        char tr[96];
        snprintf(tr, sizeof tr, " transform=\"rotate(-90 %g %g)\"", PAD / 3, PAD + ch / 2);
        buf_lit(&b, "<text class=\"wardley-axis-label\""); attr(&b, "x", PAD / 3); attr(&b, "y", PAD + ch / 2);
        buf_lit(&b, " fill=\"" TXT "\" font-size=\"12\" font-weight=\"bold\" text-anchor=\"middle\"");
        buf_put(&b, strlen(tr), tr); buf_lit(&b, ">Visibility</text>");
    }
    buf_lit(&b, "</g><g class=\"wardley-stages\">");
    static const Str DEF[] = {S("Genesis"), S("Custom Built"), S("Product"), S("Commodity")};
    size_t ns = w.stages.n ? w.stages.n : 4;
    bool useb = w.stages.n && w.bounds.n == w.stages.n;
    double prev = 0;
    for (size_t i = 0; i < ns; i++) {
        double st = useb ? prev : (double)i / (double)ns, en = useb ? w.bounds.v[i] : (double)(i + 1) / (double)ns;
        prev = en;
        double sx = PAD + st * cw, ex = PAD + en * cw;
        if (i > 0) line(&b, sx, PAD, sx, H - PAD, " stroke=\"#000\" stroke-width=\"1\" stroke-dasharray=\"5 5\" opacity=\"0.8\"");
        text(&b, (sx + ex) / 2, H - PAD / 1.5, " class=\"wardley-stage-label\" fill=\"" TXT "\" font-size=\"10\" text-anchor=\"middle\"", w.stages.n ? w.stages.v[i] : DEF[i]);
    }
    buf_lit(&b, "</g>");
    /* pipelines */
    if (w.pipes.n) {
        Buf pl = {};
        buf_lit(&b, "<g class=\"wardley-pipelines\">");
        buf_lit(&pl, "<g class=\"wardley-pipeline-links\">");
        for (size_t p = 0; p < w.pipes.n; p++) {
            WPipe *pp = &w.pipes.v[p];
            if (!pp->n) continue;
            /* sort component indices by x (insertion sort, small) */
            size_t *ord = ANEW(a, size_t, pp->n);
            if (!ord) { free(b.p), free(pl.p); return CMM_ERR_NOMEM; }
            for (size_t k = 0; k < pp->n; k++) {
                size_t v = pp->first + k, j = k;
                while (j > 0 && w.nodes.v[ord[j - 1]].x > w.nodes.v[v].x) ord[j] = ord[j - 1], j--;
                ord[j] = v;
            }
            for (size_t k = 0; k + 1 < pp->n; k++)
                line(&pl, w.nodes.v[ord[k]].px, w.nodes.v[ord[k]].py, w.nodes.v[ord[k + 1]].px, w.nodes.v[ord[k + 1]].py,
                     " class=\"wardley-pipeline-evolution-link\" stroke=\"" STK "\" stroke-width=\"1\" stroke-dasharray=\"4 4\"");
            double mn = INFINITY, mx = -INFINITY, y = 0;
            for (size_t k = 0; k < pp->n; k++) mn = fmin(mn, w.nodes.v[pp->first + k].px), mx = fmax(mx, w.nodes.v[pp->first + k].px), y = w.nodes.v[pp->first + k].py;
            double hh = NR * 4, top = y - hh / 2;
            w.nodes.v[pp->parent].px = (mn + mx) / 2, w.nodes.v[pp->parent].py = top - sq / 6;
            buf_lit(&b, "<rect class=\"wardley-pipeline-box\""); attr(&b, "x", mn - 15); attr(&b, "y", top);
            attr(&b, "width", mx - mn + 30); attr(&b, "height", hh);
            buf_lit(&b, " fill=\"none\" stroke=\"" STK "\" stroke-width=\"1.5\" rx=\"4\" ry=\"4\"/>");
        }
        buf_lit(&b, "</g>");
        buf_lit(&pl, "</g>");
        buf_put(&b, pl.n, pl.p);
        free(pl.p);
    }
    /* component labels without a user offset: the default up-right spot, or the first of a few
     * alternatives that keeps clear of links, trends, other nodes, earlier labels and the canvas */
    double *lbo = ANEW(a, double, 6 * w.nodes.n + 1); /* ox, oy, box x0 y0 x1 y1 */
    if (!lbo) { free(b.p); return CMM_ERR_NOMEM; }
    for (size_t i = 0; i < w.nodes.n; i++) {
        WNode *nd = &w.nodes.v[i];
        double *L = lbo + 6 * i, r = NLO + (nd->ss ? 10 : 0), tw = text_width(nd->label, 10);
        L[0] = r, L[1] = -r, L[2] = L[3] = L[4] = L[5] = 0;
        if (nd->k == WK_ANCHOR || nd->has_lo) continue;
        double cand[12 + 64][2] = {{r, -r}, {-r - tw, -r}, {r, r + 9}, {-r - tw, r + 9}, {r + 4, 3}, {-r - 4 - tw, 3},
            {-tw / 2, -r - 3}, {-tw / 2, r + 12}, {2 * r, -2 * r}, {-2 * r - tw, -2 * r}, {2 * r, 2 * r + 9}, {-2 * r - tw, 2 * r + 9}};
        for (int k = 0; k < 64; k++) { /* farther rings: label side faces the node; rings 3-4 get a leader */
            double th = (k % 16) * M_PI / 8, R = (const double[]){3, 4.5, 6.5, 9}[k / 16] * r, ct = cos(th);
            cand[12 + k][0] = R * ct - (ct < -0.3 ? tw : ct > 0.3 ? 0 : tw / 2), cand[12 + k][1] = R * sin(th) + 4;
        }
        double bc = INFINITY;
        for (size_t c = 0; c < countof(cand); c++) {
            double bx[4] = {nd->px + cand[c][0], nd->py + cand[c][1] - 9, nd->px + cand[c][0] + tw, nd->py + cand[c][1] + 2};
            double cost = c < 12 ? c * 0.5 : 6 + (c >= 28) * 3 + (c >= 44) * 9 + (c >= 60) * 6;
            if (bx[0] < 2 || bx[2] > W_ - 2 || bx[1] < 2 || bx[3] > H - 2) cost += 40;
            for (size_t l = 0; l < w.links.n; l++) {
                const WNode *s = &w.nodes.v[w.links.v[l].s], *t = &w.nodes.v[w.links.v[l].t];
                cost += 4 * seg_box_hits(s->px, s->py, t->px, t->py, bx);
            }
            for (size_t k = 0; k < w.trends.n; k++) {
                const WNode *o = &w.nodes.v[w.trends.v[k].num];
                cost += 4 * seg_box_hits(o->px, o->py, PX(w.trends.v[k].x), PY(w.trends.v[k].y), bx);
            }
            for (size_t j = 0; j < w.nodes.n; j++) {
                double rr = (w.nodes.v[j].ss ? 2 * NR : NR) + 1, qx = w.nodes.v[j].px, qy = w.nodes.v[j].py;
                if (qx + rr > bx[0] && qx - rr < bx[2] && qy + rr > bx[1] && qy - rr < bx[3]) cost += 30;
            }
            for (size_t k = 0; k < w.notes.n; k++) {
                double nx = PX(w.notes.v[k].x), ny = PY(w.notes.v[k].y), nw = text_width(w.notes.v[k].s, 11) * 1.05;
                if (nx < bx[2] && bx[0] < nx + nw && ny - 11 < bx[3] && bx[1] < ny + 3) cost += 30;
            }
            for (size_t j = 0; j < i; j++) {
                const double *M = lbo + 6 * j;
                if (M[4] > M[2] && M[2] < bx[2] && bx[0] < M[4] && M[3] < bx[3] && bx[1] < M[5]) cost += 30;
            }
            if (cost < bc) bc = cost, L[0] = cand[c][0], L[1] = cand[c][1], memcpy(L + 2, bx, sizeof bx);
        }
    }
    /* links */
    double *LL = ANEW(a, double, 4 * w.links.n + 4); /* placed link-label boxes */
    size_t nll = 0;
    if (!LL) { free(b.p); return CMM_ERR_NOMEM; }
    buf_lit(&b, "<g class=\"wardley-links\">");
    for (size_t pass = 0; pass < 2; pass++)
        for (size_t l = 0; l < w.links.n; l++) {
            WLink *lk = &w.links.v[l];
            WNode *s = &w.nodes.v[lk->s], *t = &w.nodes.v[lk->t];
            bool skip = false;
            for (size_t p = 0; p < w.pipes.n; p++)
                if (w.pipes.v[p].parent == lk->t && lk->s >= w.pipes.v[p].first && lk->s < w.pipes.v[p].first + w.pipes.v[p].n) skip = true;
            if (skip) continue;
            double dx = t->px - s->px, dy = t->py - s->py, d = sqrt(dx * dx + dy * dy);
            if (d < 1e-9) d = 1e-9; /* mermaid divides by zero here (NaN); keep finite */
            if (pass == 0) {
                double rs = s->pparent ? sq / sqrt(2) : NR, rt = t->pparent ? sq / sqrt(2) : NR;
                bs(&b, lk->dashed ? "<line class=\"wardley-link wardley-link--dashed\"" : "<line class=\"wardley-link\"");
                attr(&b, "x1", s->px + dx / d * rs); attr(&b, "y1", s->py + dy / d * rs);
                attr(&b, "x2", t->px - dx / d * rt); attr(&b, "y2", t->py - dy / d * rt);
                buf_lit(&b, " stroke=\"" STK "\" stroke-width=\"1\"");
                if (lk->dashed) buf_lit(&b, " stroke-dasharray=\"6 6\"");
                if (lk->flow & 1) buf_lit(&b, " marker-end=\"url(#mermaid-link-arrow-end)\"");
                if (lk->flow & 2) buf_lit(&b, " marker-start=\"url(#mermaid-link-arrow-start)\"");
                buf_lit(&b, "/>");
            } else if (lk->label.n) {
                /* default: midpoint, 8px to the left of the direction; else the cheapest spot along the
                 * link (either side) clear of other links, nodes, component labels and earlier link labels */
                double ux = dx / d, uy = dy / d, hw = text_width(lk->label, 10) / 2 + 2, hh = 7;
                double lx = (s->px + t->px) / 2 + uy * 8, ly = (s->py + t->py) / 2 - ux * 8, bc = INFINITY;
                static const double TT[] = {0.5, 0.4, 0.6, 0.3, 0.7, 0.22, 0.78};
                for (size_t ti = 0; ti < countof(TT); ti++)
                    for (int sd = 0; sd < 4; sd++) {
                        double off = (sd & 1 ? -1 : 1) * (sd < 2 ? 8 : 15), mx = s->px + dx * TT[ti], my = s->py + dy * TT[ti];
                        double cx = mx + uy * off, cy = my - ux * off, cost = ti * 0.6 + sd * 0.4;
                        if (d * fmin(TT[ti], 1 - TT[ti]) < hw + NR) cost += 20; /* label would reach a node */
                        const double box[4] = {-hw, -hh, hw, hh};
                        for (size_t m = 0; m < w.links.n; m++) { /* other links, in the label's frame */
                            if (m == l) continue;
                            const WNode *p0 = &w.nodes.v[w.links.v[m].s], *p1 = &w.nodes.v[w.links.v[m].t];
                            double ax = p0->px - cx, ay = p0->py - cy, bx_ = p1->px - cx, by = p1->py - cy;
                            cost += 4 * seg_box_hits(ax * ux + ay * uy, -ax * uy + ay * ux, bx_ * ux + by * uy, -bx_ * uy + by * ux, box);
                        }
                        double ab[4] = {cx - fabs(ux) * hw - fabs(uy) * hh, cy - fabs(uy) * hw - fabs(ux) * hh, cx + fabs(ux) * hw + fabs(uy) * hh, cy + fabs(uy) * hw + fabs(ux) * hh};
                        for (size_t j = 0; j < w.nodes.n; j++) {
                            double qx = w.nodes.v[j].px - cx, qy = w.nodes.v[j].py - cy, rr = NR + 2;
                            if (fabs(qx * ux + qy * uy) < hw + rr && fabs(-qx * uy + qy * ux) < hh + rr) cost += 30;
                            const double *M = lbo + 6 * j;
                            if (M[4] > M[2] && M[2] < ab[2] && ab[0] < M[4] && M[3] < ab[3] && ab[1] < M[5]) cost += 15;
                        }
                        for (size_t j = 0; j < nll; j++)
                            if (LL[4 * j] < ab[2] && ab[0] < LL[4 * j + 2] && LL[4 * j + 1] < ab[3] && ab[1] < LL[4 * j + 3]) cost += 15;
                        if (ab[0] < 2 || ab[1] < 2 || ab[2] > W_ - 2 || ab[3] > H - 2) cost += 40;
                        if (cost < bc - 1e-9) {
                            bc = cost, lx = cx, ly = cy;
                            memcpy(LL + 4 * nll, ab, sizeof ab);
                        }
                    }
                nll++;
                double ang = atan2(dy, dx) * 180 / M_PI;
                if (ang > 90 || ang < -90) ang += 180;
                char tr[128];
                snprintf(tr, sizeof tr, " transform=\"rotate(%.4g %.2f %.2f)\"", ang, lx, ly);
                buf_lit(&b, "<text class=\"wardley-link-label\""); attr(&b, "x", lx); attr(&b, "y", ly);
                buf_lit(&b, " fill=\"" TXT "\" font-size=\"10\" text-anchor=\"middle\" dominant-baseline=\"middle\"");
                buf_put(&b, strlen(tr), tr); buf_ch(&b, '>'); buf_xml(&b, lk->label); buf_lit(&b, "</text>");
            }
        }
    buf_lit(&b, "</g><g class=\"wardley-trends\">");
    for (size_t k = 0; k < w.trends.n; k++) {
        WNode *o = &w.nodes.v[w.trends.v[k].num];
        double tx = PX(w.trends.v[k].x), ty = PY(w.trends.v[k].y), dx = tx - o->px, dy = ty - o->py, d = sqrt(dx * dx + dy * dy), sb = NR + 2;
        double x2 = d > sb ? tx - dx / d * sb : tx, y2 = d > sb ? ty - dy / d * sb : ty;
        line(&b, o->px, o->py, x2, y2, " class=\"wardley-trend\" stroke=\"#dc3545\" stroke-width=\"1\" stroke-dasharray=\"4 4\" marker-end=\"url(#mermaid-arrow)\"");
    }
    buf_lit(&b, "</g><g class=\"wardley-nodes\">");
    const double tr = NR * 1.2, c6 = cos(M_PI / 6), s6 = sin(M_PI / 6);
    for (size_t i = 0; i < w.nodes.n; i++) {
        WNode *nd = &w.nodes.v[i];
        double x = nd->px, y = nd->py;
        bs(&b, nd->k == WK_ANCHOR ? "<g class=\"wardley-node wardley-node--anchor\">"
                    : nd->k == WK_COMP ? "<g class=\"wardley-node wardley-node--component\">"
                                       : "<g class=\"wardley-node wardley-node--pipeline-component\">");
        static const char *const OV[] = {"", " class=\"wardley-build-overlay\" fill=\"#eee\" stroke=\"#000\" stroke-width=\"1\"",
            " class=\"wardley-buy-overlay\" fill=\"#ccc\" stroke=\"" STK "\" stroke-width=\"1\"",
            " class=\"wardley-outsource-overlay\" fill=\"#666\" stroke=\"" STK "\" stroke-width=\"1\"",
            " class=\"wardley-market-overlay\" fill=\"white\" stroke=\"" STK "\" stroke-width=\"1\""};
        if (nd->ss) circle(&b, x, y, NR * 2, OV[nd->ss]);
        if (!nd->pparent && nd->ss != SS_MARKET && nd->k != WK_ANCHOR) circle(&b, x, y, NR, " fill=\"white\" stroke=\"" STK "\" stroke-width=\"1\"");
        if (nd->ss == SS_MARKET) {
            const char *ml = " class=\"wardley-market-line\" stroke=\"" STK "\" stroke-width=\"1\"";
            line(&b, x, y - tr, x - tr * c6, y + tr * s6, ml);
            line(&b, x - tr * c6, y + tr * s6, x + tr * c6, y + tr * s6, ml);
            line(&b, x + tr * c6, y + tr * s6, x, y - tr, ml);
            const char *md = " class=\"wardley-market-dot\" fill=\"white\" stroke=\"" STK "\" stroke-width=\"2\"";
            circle(&b, x, y - tr, NR * 0.7, md);
            circle(&b, x - tr * c6, y + tr * s6, NR * 0.7, md);
            circle(&b, x + tr * c6, y + tr * s6, NR * 0.7, md);
        }
        if (nd->pparent) {
            buf_lit(&b, "<rect"); attr(&b, "x", x - sq / 2); attr(&b, "y", y - sq / 2); attr(&b, "width", sq); attr(&b, "height", sq);
            buf_lit(&b, " fill=\"white\" stroke=\"" STK "\" stroke-width=\"1\"/>");
        }
        if (nd->inertia) {
            double o = (nd->pparent ? sq / 2 : NR) + 15 + (nd->ss ? NR + 10 : 0), lh = nd->pparent ? sq : NR * 2;
            line(&b, x + o, y - lh / 2, x + o, y + lh / 2, " class=\"wardley-inertia\" stroke=\"" STK "\" stroke-width=\"6\"");
        }
        if (nd->k == WK_ANCHOR)
            text(&b, x + (nd->has_lo ? nd->lox : 0), y + (nd->has_lo ? nd->loy : -3),
                 " class=\"wardley-node-label\" fill=\"#000\" font-size=\"10\" font-weight=\"bold\" text-anchor=\"middle\" dominant-baseline=\"middle\"", nd->label);
        else {
            double ox = nd->has_lo ? nd->lox : lbo[6 * i], oy = nd->has_lo ? nd->loy : lbo[6 * i + 1];
            if (!nd->has_lo && lbo[6 * i + 4] > lbo[6 * i + 2]) { /* far placement: leader to the nearest box point */
                const double *B = lbo + 6 * i + 2;
                double qx = fmin(fmax(x, B[0]), B[2]), qy = fmin(fmax(y, B[1]), B[3]), vx = qx - x, vy = qy - y, l = hypot(vx, vy);
                double rr = NR + (nd->ss ? NR : 0) + 1;
                if (l > 3.2 * NLO + rr) line(&b, x + vx / l * rr, y + vy / l * rr, qx - vx / l * 2, qy - vy / l * 2, " class=\"wardley-leader\" stroke=\"#999\" stroke-width=\"0.75\"");
            }
            text(&b, x + ox, y + oy, " class=\"wardley-node-label\" fill=\"" TXT "\" font-size=\"10\" font-weight=\"normal\" text-anchor=\"start\" dominant-baseline=\"auto\"", nd->label);
        }
        buf_lit(&b, "</g>");
    }
    buf_lit(&b, "</g>");
    if (w.ann.n) {
        buf_lit(&b, "<g class=\"wardley-annotations\">");
        for (size_t k = 0; k < w.ann.n; k++) {
            char nb[16];
            int nl = snprintf(nb, sizeof nb, "%u", w.ann.v[k].num);
            buf_lit(&b, "<g class=\"wardley-annotation\">");
            circle(&b, PX(w.ann.v[k].x), PY(w.ann.v[k].y), 10, " fill=\"white\" stroke=\"" STK "\" stroke-width=\"1.5\"");
            text(&b, PX(w.ann.v[k].x), PY(w.ann.v[k].y), " text-anchor=\"middle\" dominant-baseline=\"central\" font-size=\"10\" fill=\"" TXT "\" font-weight=\"bold\"", (Str){.p = nb, .n = (size_t)nl});
            buf_lit(&b, "</g>");
        }
        if (w.has_box) {
            /* sorted (stable) by number, only those with text */
            size_t *ord = ANEW(a, size_t, w.ann.n), m = 0;
            if (!ord) { free(b.p); return CMM_ERR_NOMEM; }
            for (size_t k = 0; k < w.ann.n; k++) {
                if (!w.ann.v[k].s.n) continue;
                size_t j = m++;
                while (j > 0 && w.ann.v[ord[j - 1]].num > w.ann.v[k].num) ord[j] = ord[j - 1], j--;
                ord[j] = k;
            }
            if (m) {
                double maxw = 0;
                Str *lines = ANEW(a, Str, m);
                if (!lines) { free(b.p); return CMM_ERR_NOMEM; }
                for (size_t k = 0; k < m; k++) {
                    WPt *p = &w.ann.v[ord[k]];
                    char *o = arena_alloc_array(a, p->s.n + 16, 1, 1);
                    if (!o) { free(b.p); return CMM_ERR_NOMEM; }
                    int pl = snprintf(o, 16, "%u. ", p->num);
                    memcpy(o + pl, p->s.p, p->s.n);
                    lines[k] = (Str){.p = o, .n = (size_t)pl + p->s.n};
                    maxw = fmax(maxw, text_width(lines[k], 11));
                }
                double bw = maxw + 20 + 105, bh = (double)m * 16 + 20 + 13.0 / 2;
                double bx = fmax(PAD, fmin(PX(w.boxx), W_ - PAD - bw)), by = fmax(PAD, fmin(PY(w.boxy), H - PAD - bh));
                buf_lit(&b, "<g class=\"wardley-annotations-box\"><rect"); attr(&b, "x", bx); attr(&b, "y", by);
                attr(&b, "width", bw); attr(&b, "height", bh);
                buf_lit(&b, " fill=\"white\" stroke=\"" STK "\" stroke-width=\"1.5\" rx=\"4\" ry=\"4\"/>");
                for (size_t k = 0; k < m; k++)
                    text(&b, bx + 10, by + 10 + (double)(k + 1) * 16, " font-size=\"11\" fill=\"" TXT "\" text-anchor=\"start\" dominant-baseline=\"middle\"", lines[k]);
                buf_lit(&b, "</g>");
            }
        }
        buf_lit(&b, "</g>");
    }
    if (w.notes.n) {
        buf_lit(&b, "<g class=\"wardley-notes\">");
        for (size_t k = 0; k < w.notes.n; k++)
            text(&b, fmax(2, fmin(PX(w.notes.v[k].x), W_ - 4 - text_width(w.notes.v[k].s, 11) * 1.05)), PY(w.notes.v[k].y), " text-anchor=\"start\" font-size=\"11\" fill=\"" TXT "\" font-weight=\"bold\"", w.notes.v[k].s);
        buf_lit(&b, "</g>");
    }
    for (int dir = 0; dir < 2; dir++) {
        const WPt *v = dir ? w.dec.v : w.acc.v;
        size_t cnt = dir ? w.dec.n : w.acc.n;
        if (!cnt) continue;
        bs(&b, dir ? "<g class=\"wardley-deaccelerators\">" : "<g class=\"wardley-accelerators\">");
        for (size_t k = 0; k < cnt; k++) {
            double x = PX(v[k].x), y = PY(v[k].y);
            double P[7][2] = {{0, -15}, {40, -15}, {40, -23}, {60, 0}, {40, 23}, {40, 15}, {0, 15}};
            buf_lit(&b, "<path d=\"");
            for (int q = 0; q < 7; q++) {
                bs(&b, q ? " L " : "M ");
                buf_num(&b, x + (dir ? 60 - P[q][0] : P[q][0])); buf_ch(&b, ' '); buf_num(&b, y + P[q][1]);
            }
            buf_lit(&b, " Z\" fill=\"white\" stroke=\"" STK "\" stroke-width=\"1\"/>");
            text(&b, x + 30, y + 30, " text-anchor=\"middle\" font-size=\"10\" fill=\"" TXT "\" font-weight=\"bold\"", v[k].s);
        }
        buf_lit(&b, "</g>");
    }
    buf_lit(&b, "</g><defs>"
        "<marker id=\"mermaid-arrow\" viewBox=\"0 0 10 10\" refX=\"9\" refY=\"5\" markerWidth=\"6\" markerHeight=\"6\" orient=\"auto-start-reverse\"><path d=\"M 0 0 L 10 5 L 0 10 z\" fill=\"#dc3545\" stroke=\"none\"/></marker>"
        "<marker id=\"mermaid-link-arrow-end\" viewBox=\"0 0 10 10\" refX=\"9\" refY=\"5\" markerWidth=\"5\" markerHeight=\"5\" orient=\"auto\"><path d=\"M 0 0 L 10 5 L 0 10 z\" fill=\"" STK "\" stroke=\"none\"/></marker>"
        "<marker id=\"mermaid-link-arrow-start\" viewBox=\"0 0 10 10\" refX=\"1\" refY=\"5\" markerWidth=\"5\" markerHeight=\"5\" orient=\"auto\"><path d=\"M 10 0 L 0 5 L 10 10 z\" fill=\"" STK "\" stroke=\"none\"/></marker></defs>");
#undef PX
#undef PY
    BBox vb = {0, 0, W_, H};
    return DG_FINISH(out, &b, vb, 0, "wardley", "#mermaid{font-family:\"trebuchet ms\",verdana,arial,sans-serif;}");
}
