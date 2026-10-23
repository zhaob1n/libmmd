/* C4Context / C4Container / C4Component / C4Dynamic / C4Deployment.
 * Elements: Person, System, SystemDb, SystemQueue, Container*, Component* (+ _Ext variants);
 * boundaries: Boundary, Enterprise_Boundary, System_Boundary, Container_Boundary, Deployment_Node,
 * Node, Node_L, Node_R with `{ ... }`; relations Rel, BiRel, Rel_U/D/L/R/Up/Down/Left/Right/Back,
 * RelIndex. UpdateElementStyle($bgColor/$fontColor/$borderColor), UpdateRelStyle($textColor/$lineColor/
 * $offsetX/$offsetY) and UpdateLayoutConfig($c4ShapeInRow/$c4BoundaryInRow) are honoured; tags are ignored.
 * Layout follows mermaid: in each boundary, shapes in declaration order fill rows of c4ShapeInRow, then
 * child boundaries fill rows of c4BoundaryInRow below them. Relations are straight clipped lines. */
#include <math.h>
#include <stdio.h>

#include "dgraph.h"
#include "diagrams.h"
#include "text.h"

enum { K_PERSON, K_SYSTEM, K_CONTAINER, K_COMPONENT };
static const char *const C4_TY[4] = {"Person", "Software System", "Container", "Component"};
enum { F_PLAIN, F_DB, F_QUEUE };
typedef struct CInfo { Str stereo, techn, descr; uint8_t kind, form; bool ext; } CInfo;
typedef struct CRel { Str techn; bool bi; } CRel;
typedef struct EStyle { Str alias, bg, font, border; } EStyle;
typedef struct RStyle { Str from, to, text, line; double ox, oy; } RStyle;
typedef struct Item { int32_t idx; bool box; } Item;

static constexpr char CSS[] =
    "#mermaid .ct{fill:#fff;}#mermaid .ct.ext{fill:#fff;}#mermaid .cn{font-weight:bold;font-size:14px;}#mermaid .cs{font-size:10.5px;}"
    "#mermaid .cd{font-size:11.48px;}#mermaid .cb{fill:none;stroke:#444;stroke-width:1px;stroke-dasharray:7,7;}"
    "#mermaid .cbt{font-weight:bold;font-size:14px;fill:#444;}#mermaid .cbs{font-size:12px;fill:#444;}"
    "#mermaid .cr{fill:none;stroke:#444;stroke-width:1px;}#mermaid .crh{fill:#444;}#mermaid .crl{font-size:13px;fill:#444;}"
    "#mermaid .crt{font-size:12px;font-style:italic;fill:#444;}#mermaid .crx{fill:inherit;}#mermaid .crc .cr{stroke:inherit;}#mermaid .crc .crh{fill:inherit;}";
static const char *const FILL[2][4] = {{"#08427B", "#1168BD", "#438DD5", "#85BBF0"}, {"#686868", "#999999", "#B3B3B3", "#E6E6E6"}};
static const char *const STROKE[2][4] = {{"#073B6F", "#3C7FC0", "#3C7FC0", "#78A8D8"}, {"#8A8A8A", "#8A8A8A", "#A6A6A6", "#D0D0D0"}};

static Str unq(Str s) {
    s = str_trim(s);
    if (s.n >= 2 && s.p[0] == '"' && s.p[s.n - 1] == '"') s = (Str){.p = s.p + 1, .n = s.n - 2};
    return s;
}

/* split `( a, "b, c", d )`; positional only, $named skipped */
static size_t args(Str in, size_t max, Str out[static max]) {
    size_t k = 0, st = 0;
    bool q = false;
    for (size_t i = 0; i <= in.n; i++) {
        if (i < in.n && in.p[i] == '"') q = !q;
        if (i == in.n || (in.p[i] == ',' && !q)) {
            Str a = str_trim((Str){.p = in.p + st, .n = i - st});
            if (!(a.n && a.p[0] == '$') && k < max) out[k++] = unq(a);
            st = i + 1;
        }
    }
    return k;
}

/* value of `$key=...` among the args, unquoted; empty if absent */
static Str named(Str in, const char *key) {
    size_t kl = strlen(key), st = 0;
    bool q = false;
    for (size_t i = 0; i <= in.n; i++) {
        if (i < in.n && in.p[i] == '"') q = !q;
        if (i == in.n || (in.p[i] == ',' && !q)) {
            Str a = str_trim((Str){.p = in.p + st, .n = i - st});
            if (a.n > kl + 1 && a.p[0] == '$' && !memcmp(a.p + 1, key, kl)) {
                Str r = str_trim((Str){.p = a.p + 1 + kl, .n = a.n - 1 - kl});
                if (r.n && r.p[0] == '=') return unq((Str){.p = r.p + 1, .n = r.n - 1});
            }
            st = i + 1;
        }
    }
    return (Str){};
}

/* style values are arbitrary user strings (mermaid passes them through); they are only ever written as
 * presentation-attribute values via put_attr, which escapes everything that could end the attribute. */
static bool color_ok(Str c) { return c.n > 0 && c.n <= 256; }

static void put_attr(Buf *b, Str s) {
    for (size_t i = 0; i < s.n; i++) {
        unsigned char ch = (unsigned char)s.p[i];
        switch (ch) {
        case '&': buf_lit(b, "&amp;"); break;
        case '<': buf_lit(b, "&lt;"); break;
        case '>': buf_lit(b, "&gt;"); break;
        case '"': buf_lit(b, "&quot;"); break;
        case '\'': buf_lit(b, "&#39;"); break;
        default: if (ch >= 0x20 && ch != 0x7f) buf_ch(b, (char)ch); /* drop control bytes (invalid in XML) */
        }
    }
}


/* first crossing of the segment centre->(ox,oy) with the w*h box around (cx,cy) */
static void rclip(double cx, double cy, double w, double h, double ox, double oy, double *x, double *y) {
    double dx = ox - cx, dy = oy - cy;
    double tx = dx != 0 ? (w / 2) / fabs(dx) : INFINITY, ty = dy != 0 ? (h / 2) / fabs(dy) : INFINITY;
    double t = fmin(fmin(tx, ty), 1);
    *x = cx + dx * t, *y = cy + dy * t;
}

typedef struct Lay {
    const Graph *g;
    const Item *items;
    size_t nitems;
    unsigned shape_row, box_row;
    double maxx, maxy; /* globalBoundaryMax */
    double *nx, *ny, *sx, *sy, *sw, *sh;
} Lay;

/* mermaid c4Renderer port: Bounds.insert / drawInsideBoundary (positions only) */
constexpr double C4_SHAPE_MARGIN = 50;  /* conf.c4ShapeMargin */
constexpr double C4_DMX = 50;           /* conf.diagramMarginX */
constexpr double C4_DMY = 10;           /* conf.diagramMarginY */
constexpr double C4_SCREEN_W = 800;     /* screen.availWidth under mermaid-cli's headless browser */
constexpr double C4_LBL_H = 22;         /* boundary label line (16px bold) */
constexpr double C4_TYPE_H = 19;        /* boundary [type] line (14px) */

typedef struct Bnd {
    double x0, x1, y0, y1;     /* data */
    double nx0, nx1, ny0, ny1; /* nextData */
    double lim;
    unsigned cnt;
} Bnd;

static void bnd_set(Bnd *b, double x, double y) { b->x0 = b->x1 = b->nx0 = b->nx1 = x, b->y0 = b->y1 = b->ny0 = b->ny1 = y; }

static void bnd_insert(Bnd *b, unsigned shape_row, double w, double h, double *ox, double *oy) {
    constexpr double m = C4_SHAPE_MARGIN;
    b->cnt++;
    double sx = b->nx0 == b->nx1 ? b->nx1 + m : b->nx1 + 2 * m, ex = sx + w, sy = b->ny0 + 2 * m, ey = sy + h;
    if (sx >= b->lim || ex >= b->lim || b->cnt > shape_row) {
        sx = b->nx0 + m;
        sy = b->ny1 + 2 * m;
        b->nx1 = ex = sx + w;
        b->ny0 = b->ny1;
        b->ny1 = ey = sy + h;
        b->cnt = 1;
    }
    *ox = sx, *oy = sy;
    b->x0 = fmin(b->x0, sx), b->y0 = fmin(b->y0, sy), b->x1 = fmax(b->x1, ex), b->y1 = fmax(b->y1, ey);
    b->nx0 = fmin(b->nx0, sx), b->ny0 = fmin(b->ny0, sy), b->nx1 = fmax(b->nx1, ex), b->ny1 = fmax(b->ny1, ey);
}

constexpr int32_t C4_GLOBAL = -2; /* mermaid's implicit "global" boundary (not drawn) */

/* mermaid lineBreakRegex /<br\s*\/?>/gi: length of the next segment of t from *i, advancing past the break */
static Str br_next(Str t, size_t *i) {
    size_t s0 = *i;
    for (size_t k = s0; k < t.n; k++) {
        if (t.p[k] != '<' || k + 3 > t.n || (t.p[k + 1] | 32) != 'b' || (t.p[k + 2] | 32) != 'r') continue;
        size_t j = k + 3;
        while (j < t.n && (t.p[j] == ' ' || t.p[j] == '\t')) j++;
        if (j < t.n && t.p[j] == '/') j++;
        if (j < t.n && t.p[j] == '>') { *i = j + 1; return (Str){.p = t.p + s0, .n = k - s0}; }
    }
    *i = t.n + 1;
    return (Str){.p = t.p + s0, .n = t.n - s0};
}
static unsigned br_lines(Str t) {
    unsigned n = 0;
    for (size_t i = 0; i <= t.n;) { (void)br_next(t, &i); n++; }
    return n;
}

/* drawInsideBoundary(parentBounds, boundaries whose parent is `parent`) */
static void lay(Lay *L, Bnd *P, int32_t parent, unsigned depth) {
    const Graph *g = L->g;
    size_t nb = 0;
    if (parent == C4_GLOBAL) nb = 1;
    else for (size_t i = 0; i < L->nitems; i++) nb += L->items[i].box && g->sparent[L->items[i].idx] == parent;
    if (!nb) return;
    Bnd cur = {.lim = P->lim / (double)(L->box_row < nb ? L->box_row : nb)};
    size_t bi = 0;
    for (size_t k = 0; k < (parent == C4_GLOBAL ? 1 : L->nitems); k++) {
        int32_t s;
        if (parent == C4_GLOBAL) s = NONE;
        else if (!L->items[k].box || g->sparent[L->items[k].idx] != parent) continue;
        else s = L->items[k].idx;
        double Y = 8 + C4_LBL_H + 5 + C4_TYPE_H; /* the implicit global boundary is typed */
        if (s != NONE) { /* wrap is off for boundaries: one text line per <br> segment */
            Str t = g->stitle[s];
            const char *nl = memchr(t.p, '\n', t.n);
            Str t1 = nl ? (Str){.p = t.p, .n = (size_t)(nl - t.p)} : t;
            Y = 8 + C4_LBL_H * br_lines(t1);
            if (nl) Y += 5 + C4_TYPE_H * br_lines((Str){.p = nl + 1, .n = t.n - (size_t)(nl + 1 - t.p)});
        }
        if (bi == 0 || bi % L->box_row == 0) bnd_set(&cur, P->x0 + C4_DMX, P->y1 + C4_DMY + Y);
        else bnd_set(&cur, cur.x1 != cur.x0 ? cur.x1 + C4_DMX : cur.x0, cur.y0);
        bi++;
        bool any = false;
        for (size_t i = 0; i < L->nitems; i++) {
            Item it = L->items[i];
            if (it.box || g->nparent[it.idx] != s) continue;
            double x, y;
            bnd_insert(&cur, L->shape_row, g->nw[it.idx], g->nh[it.idx], &x, &y);
            L->nx[it.idx] = x + g->nw[it.idx] / 2, L->ny[it.idx] = y + g->nh[it.idx] / 2;
            any = true;
        }
        if (any) cur.x1 += C4_SHAPE_MARGIN, cur.y1 += C4_SHAPE_MARGIN; /* bumpLastMargin */
        if (depth < 64) lay(L, &cur, s, depth + 1);
        if (s != NONE) L->sx[s] = cur.x0, L->sy[s] = cur.y0, L->sw[s] = cur.x1 - cur.x0, L->sh[s] = cur.y1 - cur.y0;
        P->y1 = fmax(cur.y1 + C4_SHAPE_MARGIN, P->y1);
        P->x1 = fmax(cur.x1 + C4_SHAPE_MARGIN, P->x1);
        L->maxx = fmax(L->maxx, P->x1), L->maxy = fmax(L->maxy, P->y1);
    }
}

static bool is(Str s, const char *lit) { return s.n == strlen(lit) && !memcmp(s.p, lit, s.n); }

static void arrowhead(Buf *b, const double *A, const double *B) { /* tip at A, coming from B */
    double dx = A[0] - B[0], dy = A[1] - B[1], L = sqrt(dx * dx + dy * dy);
    if (L < 1e-9) return;
    dx /= L, dy /= L;
    buf_lit(b, "<path class=\"crh\" d=\"M"); dg_pt(b, A[0], A[1]); buf_ch(b, 'L');
    dg_pt(b, A[0] - dx * 10 - dy * 4, A[1] - dy * 10 + dx * 4); buf_ch(b, 'L');
    dg_pt(b, A[0] - dx * 10 + dy * 4, A[1] - dy * 10 - dx * 4); buf_lit(b, "z\"/>");
}

constexpr double C4_NF = 14;     /* c4 element name */
constexpr double C4_TF = 10.5;   /* [type] 0.75em */
constexpr double C4_DF = 11.48;  /* description 0.82em */
constexpr double C4_PAD = 20;    /* conf.c4ShapePadding */
constexpr double C4_GAP = 3;     /* c4LabelHelper SECTION_GAP */
/* svg text bbox height of an n-line block: rows 1.1em apart plus the glyph box of one row. Chromium's
 * glyph box is the rounded ascent plus the rounded descent of the font ("Open Sans": 1.069em / 0.293em),
 * measured: 14px bold 19, 10.5px 14, 11.48px 15 */
static double sec_h(unsigned n, double f) { return (n ? n - 1 : 0) * 1.1 * f + round(1.069 * f) + round(0.293 * f); }
/* mermaid cylinder end radius (ry = rx / (2.5 + w/50)) and person head radius */
static double cyl_r(double w) { return w / 2 / (2.5 + w / 50); }
static double head_r(double w) { return fmin(fmax(w * 0.23, 16), 56); }

DIAG_FN(c4_render) {
    Graph g;
    dg_init(&g, a, DIR_TB, fs);
    LineIt it = {.s = src, .n = n, .i = off};
    Str line;
    size_t lo;
    if (!dg_next_line(&it, &line, &lo)) return diag_fail(err_off, msg, off, "empty C4 diagram");
    VEC(CInfo) ci = {};
    VEC(CRel) cr = {};
    VEC(EStyle) es = {};
    VEC(RStyle) rs = {};
    VEC(Item) items = {};
    unsigned shape_row = 4, box_row = 2;
    int32_t stack[32];
    unsigned depth = 0;
    Str ttl = title;
    while (dg_next_line(&it, &line, &lo)) {
        if (str_eq(line, S("}"))) {
            if (!depth) return diag_fail(err_off, msg, lo, "unmatched '}'");
            depth--;
            continue;
        }
        if (str_starts(line, S("title "))) { ttl = str_trim((Str){.p = line.p + 6, .n = line.n - 6}); continue; }
        const char *lp = memchr(line.p, '(', line.n);
        if (!lp) return diag_fail(err_off, msg, lo, "expected C4 macro call");
        Str mac = str_trim((Str){.p = line.p, .n = (size_t)(lp - line.p)});
        const char *rp = nullptr;
        bool q = false;
        for (const char *c = lp + 1; c < line.p + line.n; c++) {
            if (*c == '"') q = !q;
            else if (*c == ')' && !q) rp = c;
        }
        if (!rp) return diag_fail(err_off, msg, lo, "missing ')'");
        Str tail = str_trim((Str){.p = rp + 1, .n = (size_t)(line.p + line.n - rp - 1)});
        bool open = str_eq(tail, S("{"));
        if (tail.n && !open) return diag_fail(err_off, msg, (size_t)(tail.p - src), "unexpected text after macro");
        Str A[8];
        size_t na = args((Str){.p = lp + 1, .n = (size_t)(rp - lp - 1)}, 8, A);
        Str raw = {.p = lp + 1, .n = (size_t)(rp - lp - 1)};
        if (is(mac, "UpdateLayoutConfig")) {
            double v;
            Str a1 = named(raw, "c4ShapeInRow"), a2 = named(raw, "c4BoundaryInRow");
            if (a1.n && dg_parse_num(a1, &v) && v >= 1 && v <= 64) shape_row = (unsigned)v;
            if (a2.n && dg_parse_num(a2, &v) && v >= 1 && v <= 64) box_row = (unsigned)v;
            continue;
        }
        if (is(mac, "UpdateElementStyle")) {
            if (na && !VEC_PUSH(a, &es, ((EStyle){A[0], named(raw, "bgColor"), named(raw, "fontColor"), named(raw, "borderColor")}))) return CMM_ERR_NOMEM;
            continue;
        }
        if (is(mac, "UpdateRelStyle")) {
            double ox = 0, oy = 0;
            Str sx_ = named(raw, "offsetX"), sy_ = named(raw, "offsetY");
            if (sx_.n && (!dg_parse_num(sx_, &ox) || fabs(ox) > 1e4)) ox = 0;
            if (sy_.n && (!dg_parse_num(sy_, &oy) || fabs(oy) > 1e4)) oy = 0;
            if (na >= 2 && !VEC_PUSH(a, &rs, ((RStyle){A[0], A[1], named(raw, "textColor"), named(raw, "lineColor"), ox, oy}))) return CMM_ERR_NOMEM;
            continue;
        }
        if (str_starts(mac, S("Update")) || is(mac, "AddElementTag") || is(mac, "AddRelTag") || is(mac, "AddBoundaryTag")) continue;
        int32_t parent = depth ? stack[depth - 1] : NONE;
        if (str_starts(mac, S("Rel")) || is(mac, "BiRel")) {
            size_t b0 = is(mac, "RelIndex") ? 1 : 0;
            if (na < b0 + 3) return diag_fail(err_off, msg, lo, "Rel needs from, to, label");
            if (!A[b0].n || !A[b0 + 1].n) return diag_fail(err_off, msg, lo, "empty Rel endpoint");
            int32_t va = dg_node(&g, A[b0], nullptr), vb = va == NONE ? NONE : dg_node(&g, A[b0 + 1], nullptr);
            if (vb == NONE) return CMM_ERR_NOMEM;
            if (is(mac, "Rel_Back")) { int32_t t = va; va = vb; vb = t; }
            if (dg_edge(&g, va, vb, A[b0 + 2]) == NONE) return CMM_ERR_NOMEM;
            g.eend[g.ne - 1] = AH_NONE;
            if (!VEC_PUSH(a, &cr, ((CRel){.techn = na > b0 + 3 ? A[b0 + 3] : (Str){}, .bi = is(mac, "BiRel")}))) return CMM_ERR_NOMEM;
            continue;
        }
        if (open) {
            if (!na || !A[0].n) return diag_fail(err_off, msg, lo, "boundary needs an alias");
            if (depth >= countof(stack)) return diag_fail(err_off, msg, lo, "boundaries nested too deeply");
            Str type = na > 2 ? A[2] : is(mac, "Enterprise_Boundary") ? S("Enterprise") : is(mac, "System_Boundary") ? S("System")
                     : is(mac, "Container_Boundary") ? S("Container") : str_starts(mac, S("Node")) || is(mac, "Deployment_Node") ? S("") : S("");
            Str t2 = na > 1 ? A[1] : A[0];
            if (type.n) {
                char *b = ANEW(a, char, t2.n + type.n + 4);
                if (!b) return CMM_ERR_NOMEM;
                memcpy(b, t2.p, t2.n), memcpy(b + t2.n, "\n[", 2), memcpy(b + t2.n + 2, type.p, type.n), b[t2.n + 2 + type.n] = ']';
                t2 = (Str){.p = b, .n = t2.n + type.n + 3};
            }
            int32_t c = dg_cluster(&g, A[0], t2, parent);
            if (c == NONE) return CMM_ERR_NOMEM;
            stack[depth++] = c;
            if (!VEC_PUSH(a, &items, ((Item){c, true}))) return CMM_ERR_NOMEM;
            continue;
        }
        CInfo info = {};
        Str m = mac;
        if (str_starts(m, S("Person"))) info.kind = K_PERSON, info.stereo = S("person");
        else if (str_starts(m, S("System"))) info.kind = K_SYSTEM, info.stereo = S("system");
        else if (str_starts(m, S("Container"))) info.kind = K_CONTAINER, info.stereo = S("container");
        else if (str_starts(m, S("Component"))) info.kind = K_COMPONENT, info.stereo = S("component");
        else return diag_fail(err_off, msg, lo, "unknown C4 macro");
        info.ext = m.n > 4 && !memcmp(m.p + m.n - 4, "_Ext", 4);
        Str base = info.ext ? (Str){.p = m.p, .n = m.n - 4} : m;
        if (base.n > 2 && !memcmp(base.p + base.n - 2, "Db", 2)) info.form = F_DB;
        if (base.n > 5 && !memcmp(base.p + base.n - 5, "Queue", 5)) info.form = F_QUEUE;
        static const char *const OK[] = {"Person", "System", "SystemDb", "SystemQueue", "Container", "ContainerDb", "ContainerQueue",
                                         "Component", "ComponentDb", "ComponentQueue"};
        bool ok = false;
        for (size_t k = 0; k < countof(OK); k++) ok |= is(base, OK[k]);
        if (!ok) return diag_fail(err_off, msg, lo, "unknown C4 element");
        if (info.ext) {
            char *s = ANEW(a, char, info.stereo.n + 9);
            if (!s) return CMM_ERR_NOMEM;
            memcpy(s, "external_", 9), memcpy(s + 9, info.stereo.p, info.stereo.n);
            info.stereo = (Str){.p = s, .n = info.stereo.n + 9};
        }
        if (!na || !A[0].n) return diag_fail(err_off, msg, lo, "element needs an alias");
        bool techd = info.kind >= K_CONTAINER;
        if (techd) { info.techn = na > 2 ? A[2] : (Str){}; info.descr = na > 3 ? A[3] : (Str){}; }
        else info.descr = na > 2 ? A[2] : (Str){};
        bool fresh;
        int32_t v = dg_node(&g, A[0], &fresh);
        if (v == NONE) return CMM_ERR_NOMEM;
        g.nlabel[v] = na > 1 ? A[1] : A[0];
        g.nparent[v] = parent;
        if (!VEC_PUSH(a, &items, ((Item){v, false}))) return CMM_ERR_NOMEM;
        while (ci.n < g.nn) if (!VEC_PUSH(a, &ci, (CInfo){.kind = K_SYSTEM, .stereo = S("system")})) return CMM_ERR_NOMEM;
        ci.v[v] = info;
    }
    if (depth) return diag_fail(err_off, msg, n, "missing '}'");
    if (!g.nn) return diag_fail(err_off, msg, off, "C4 diagram has no elements");
    while (ci.n < g.nn) if (!VEC_PUSH(a, &ci, (CInfo){.kind = K_SYSTEM, .stereo = S("system")})) return CMM_ERR_NOMEM;
    /* mermaid 11.17 c4LabelHelper: sections name (14px bold) / [type] (0.75em) / descr (0.82em) wrapped to
     * width - 2*padding (padding 20), stacked 3px apart; shapes pad the stack by 20 on each side. */
    Str *dw = ANEW(a, Str, g.nn), *dn = ANEW(a, Str, g.nn), *dt = ANEW(a, Str, g.nn);
    double *lab = ANEW(a, double, g.nn);
    if (!dw || !dn || !dt || !lab) return CMM_ERR_NOMEM;
    for (size_t v = 0; v < g.nn; v++) {
        CInfo *c = &ci.v[v];
        double w = 216; /* mermaid: node.width = conf.width; the [type] line is not used for sizing */
        /* cylinder: node.width loses one padding before c4LabelHelper subtracts two */
        double ww = w - 2 * C4_PAD - (c->form == F_DB ? C4_PAD : c->form == F_QUEUE ? C4_PAD / 2 : 0);
        dn[v] = dg_wrapf(a, g.nlabel[v], C4_NF, ww, text_width_os);
        dw[v] = c->descr.n ? dg_wrapf(a, c->descr, C4_DF, ww, text_width_os) : (Str){};
        if (!dn[v].n && g.nlabel[v].n) return CMM_ERR_NOMEM;
        double tw;
        unsigned nl = 0;
        label_measure_f(dn[v], C4_NF, text_width_os, &tw, &nl);
        w = fmax(w, tw + w - ww);
        { /* mermaid 11 type line: [Person], [Software System], [Container: techn]; wrapped like the rest */
            size_t kl = strlen(C4_TY[c->kind]), n = kl + 3 + (c->techn.n ? c->techn.n + 2 : 0);
            char *t = ANEW(a, char, n);
            if (!t) return CMM_ERR_NOMEM;
            size_t k = 0;
            t[k++] = '[';
            memcpy(t + k, C4_TY[c->kind], kl), k += kl;
            if (c->techn.n) t[k++] = ':', t[k++] = ' ', memcpy(t + k, c->techn.p, c->techn.n), k += c->techn.n;
            t[k++] = ']';
            dt[v] = dg_wrapf(a, (Str){.p = t, .n = k}, C4_TF, ww, text_width_os);
            if (!dt[v].n) return CMM_ERR_NOMEM;
        }
        double twid;
        unsigned tl;
        label_measure_f(dt[v], C4_TF, text_width_os, &twid, &tl);
        w = fmax(w, twid + w - ww);
        double h = sec_h(nl, C4_NF) + C4_GAP + sec_h(tl, C4_TF);
        if (dw[v].n) {
            double dwid;
            unsigned dl;
            label_measure_f(dw[v], C4_DF, text_width_os, &dwid, &dl);
            h += C4_GAP + sec_h(dl, C4_DF);
            w = fmax(w, dwid + w - ww);
        }
        lab[v] = h;
        h += 2 * C4_PAD;
        if (c->form == F_QUEUE) { /* mermaid tiltedCylinder: label + padding/2; body width + rx, and the
                                      * caps (rx each) count towards the layout width */
            h = lab[v] + C4_PAD / 2;
            w += 3 * (h / 2 / (2.5 + h / 50));
        }
        else if (c->form == F_DB) h = lab[v] + C4_PAD + 3 * cyl_r(w); /* mermaid cylinder: label + padding + ry, plus both caps */
        else if (c->kind == K_PERSON) h += 2 * head_r(w) - 0.27 * head_r(w);
        dg_size(&g, (int32_t)v, w, h);
    }
    for (size_t e = 0; e < g.ne; e++) {
        if (cr.v[e].techn.n) {
            char *b = ANEW(a, char, g.elabel[e].n + cr.v[e].techn.n + 3);
            if (!b) return CMM_ERR_NOMEM;
            memcpy(b, g.elabel[e].p, g.elabel[e].n), memcpy(b + g.elabel[e].n, "\n[", 2);
            memcpy(b + g.elabel[e].n + 2, cr.v[e].techn.p, cr.v[e].techn.n), b[g.elabel[e].n + 2 + cr.v[e].techn.n] = ']';
            g.elabel[e] = (Str){.p = b, .n = g.elabel[e].n + cr.v[e].techn.n + 3};
        }
    }
    /* nodes only referenced by Rel (never declared) go to the root in first-use order */
    {
        bool *seen = ANEW(a, bool, g.nn);
        if (!seen) return CMM_ERR_NOMEM;
        for (size_t v = 0; v < g.nn; v++) seen[v] = false; /* arena memory is not zeroed */
        size_t k = 0;
        for (size_t i = 0; i < items.n; i++) { /* keep the first declaration of each element */
            Item it = items.v[i];
            if (!it.box && seen[it.idx]) continue;
            if (!it.box) seen[it.idx] = true;
            items.v[k++] = it;
        }
        items.n = k;
        for (size_t v = 0; v < g.nn; v++)
            if (!seen[v] && !VEC_PUSH(a, &items, ((Item){(int32_t)v, false}))) return CMM_ERR_NOMEM;
    }
    /* layout output arrays (flow_layout normally allocates these) */
    g.nx = ANEW(a, double, g.nn), g.ny = ANEW(a, double, g.nn);
    g.sx = ANEW(a, double, g.ns + 1), g.sy = ANEW(a, double, g.ns + 1), g.sw = ANEW(a, double, g.ns + 1), g.sh = ANEW(a, double, g.ns + 1);
    g.elx = ANEW(a, double, g.ne + 1), g.ely = ANEW(a, double, g.ne + 1);
    if (!g.nx || !g.ny || !g.sx || !g.sy || !g.sw || !g.sh || !g.elx || !g.ely) return CMM_ERR_NOMEM;
    Lay L = {.g = &g, .items = items.v, .nitems = items.n, .shape_row = shape_row, .box_row = box_row,
             .nx = g.nx, .ny = g.ny, .sx = g.sx, .sy = g.sy, .sw = g.sw, .sh = g.sh};
    Bnd screen = {.lim = C4_SCREEN_W};
    bnd_set(&screen, C4_DMX, C4_DMY);
    L.maxx = C4_DMX, L.maxy = C4_DMY;
    lay(&L, &screen, C4_GLOBAL, 0);
    /* mermaid: viewBox = (startx - marginX, -(marginY + 60 if titled), box + 2 margins); screen start (50, 10) */
    BBox bb = {0, -C4_DMY - (ttl.n ? 60 : 0), L.maxx + C4_DMX, L.maxy};
    /* resolve style overrides */
    const EStyle **nst = ANEW(a, const EStyle *, g.nn);
    const RStyle **rst = ANEW(a, const RStyle *, g.ne + 1);
    if (!nst || !rst) return CMM_ERR_NOMEM;
    for (size_t v = 0; v < g.nn; v++) nst[v] = nullptr;
    for (size_t e = 0; e <= g.ne; e++) rst[e] = nullptr;
    for (size_t i = 0; i < es.n; i++) { int32_t v = dg_find(&g, es.v[i].alias); if (v != NONE) nst[v] = &es.v[i]; }
    for (size_t i = 0; i < rs.n; i++) {
        int32_t f = dg_find(&g, rs.v[i].from), t = dg_find(&g, rs.v[i].to);
        for (size_t e = 0; e < g.ne; e++)
            if (f != NONE && ((g.efrom[e] == f && g.eto[e] == t) || (g.efrom[e] == t && g.eto[e] == f))) rst[e] = &rs.v[i];
        }
    double *lbw = ANEW(a, double, g.ne + 1), *lbh = ANEW(a, double, g.ne + 1);
    if (!lbw || !lbh) return CMM_ERR_NOMEM;
    for (size_t e = 0; e < g.ne; e++) {
        int32_t f = g.efrom[e], t = g.eto[e];
        double x0, y0, x1, y1;
        rclip(g.nx[f], g.ny[f], g.nw[f], g.nh[f], g.nx[t], g.ny[t], &x0, &y0);
        rclip(g.nx[t], g.ny[t], g.nw[t], g.nh[t], g.nx[f], g.ny[f], &x1, &y1);
        bb_add(&bb, x0, y0, x0, y0), bb_add(&bb, x1, y1, x1, y1);
        double lw;
        unsigned ln;
        label_measure(g.elabel[e], 13, &lw, &ln);
        /* label placement: slide along the edge / step off it to dodge element boxes and earlier labels */
        {
            const char *nl2 = memchr(g.elabel[e].p, '\n', g.elabel[e].n);
            double lh = nl2 ? 34 : 18, best = HUGE_VAL, bx = (x0 + x1) / 2, by = (y0 + y1) / 2;
            double len = hypot(x1 - x0, y1 - y0), nx = len > 0 ? -(y1 - y0) / len : 0, ny = len > 0 ? (x1 - x0) / len : 1;
            static constexpr double TS[] = {.5, .42, .58, .34, .66, .26, .74};
            static constexpr double OS[] = {0, -1, 1};
            for (size_t oi = 0; oi < countof(OS); oi++)
                for (size_t ti = 0; ti < countof(TS); ti++) {
                    double off = OS[oi] * (fabs(nx) > fabs(ny) ? lw / 2 + 6 : lh / 2 + 4);
                    double cx = x0 + (x1 - x0) * TS[ti] + nx * off, cy = y0 + (y1 - y0) * TS[ti] + ny * off;
                    double ax0 = cx - lw / 2 - 3, ax1 = cx + lw / 2 + 3, ay0 = cy - lh / 2, ay1 = cy + lh / 2, sc = 0;
                    for (size_t v = 0; v < g.nn; v++) {
                        double ox = fmin(ax1, g.nx[v] + g.nw[v] / 2) - fmax(ax0, g.nx[v] - g.nw[v] / 2);
                        double oy = fmin(ay1, g.ny[v] + g.nh[v] / 2) - fmax(ay0, g.ny[v] - g.nh[v] / 2);
                        if (ox > 0 && oy > 0) sc += ox * oy;
                    }
                    for (size_t q = 0; q < e; q++) {
                        double qw = lbw[q] / 2 + 3, qh = lbh[q] / 2;
                        double ox = fmin(ax1, g.elx[q] + qw) - fmax(ax0, g.elx[q] - qw);
                        double oy = fmin(ay1, g.ely[q] + qh) - fmax(ay0, g.ely[q] - qh);
                        if (ox > 0 && oy > 0) sc += 2 * ox * oy;
                    }
                    sc += (fabs(TS[ti] - .5) * 40 + fabs(OS[oi]) * 60); /* prefer the middle, on the line */
                    if (sc < best) best = sc, bx = cx, by = cy;
                }
            lbw[e] = lw, lbh[e] = lh;
            g.elx[e] = bx + (rst[e] ? rst[e]->ox : 0), g.ely[e] = by + (rst[e] ? rst[e]->oy : 0);
        }
        (void)lw; /* mermaid does not grow the viewBox for relation labels */
    }
    if (!buf_reserve(out, 2048 + g.nn * 900 + g.ne * 500 + g.ns * 300)) return CMM_ERR_NOMEM;
    DG_HEAD(out, bb, 0, "c4", CSS);
    /* mermaid: plain 16px text, start-anchored at (boxWidth/2 - 4*marginX, starty + marginY), baseline */
    if (ttl.n) dg_text(out, ttl, 16, (L.maxx - C4_DMX) / 2 - 4 * C4_DMX, 20 - 0.35 * 16, "start", "cn");
    for (size_t s = 0; s < g.ns; s++) {
        dg_rect(out, g.sx[s], g.sy[s], g.sw[s], g.sh[s], 2.5, "cb");
        Str t = g.stitle[s];
        const char *nl = memchr(t.p, '\n', t.n);
        Str t1 = nl ? (Str){.p = t.p, .n = (size_t)(nl - t.p)} : t;
        dg_text(out, t1, 14, g.sx[s] + g.sw[s] / 2, g.sy[s] + 14, "middle", "cbt"); /* mermaid centres boundary titles */
        if (nl) {
            Str t2 = {.p = nl + 1, .n = t.n - (size_t)(nl + 1 - t.p)};
            unsigned k = 0;
            for (size_t i = 0; i <= t2.n; k++) dg_text(out, br_next(t2, &i), 12, g.sx[s] + g.sw[s] / 2, g.sy[s] + 30 + C4_TYPE_H * k, "middle", "cbs");
        }
    }
    for (size_t e = 0; e < g.ne; e++) {
        int32_t f = g.efrom[e], t = g.eto[e];
        double P[4];
        rclip(g.nx[f], g.ny[f], g.nw[f], g.nh[f], g.nx[t], g.ny[t], &P[0], &P[1]);
        rclip(g.nx[t], g.ny[t], g.nw[t], g.nh[t], g.nx[f], g.ny[f], &P[2], &P[3]);
        bool lc = rst[e] && color_ok(rst[e]->line), tc = rst[e] && color_ok(rst[e]->text);
        if (lc) { buf_lit(out, "<g class=\"crc\" stroke=\""); put_attr(out, rst[e]->line); buf_lit(out, "\" fill=\""); put_attr(out, rst[e]->line); buf_lit(out, "\">"); }
        buf_lit(out, "<path class=\"cr\"");
        buf_lit(out, " d=\"M"); dg_pt(out, P[0], P[1]); buf_ch(out, 'L'); dg_pt(out, P[2], P[3]); buf_lit(out, "\"/>");
        arrowhead(out, P + 2, P);
        if (cr.v[e].bi) arrowhead(out, P, P + 2);
        if (lc) buf_lit(out, "</g>");
        if (tc) { buf_lit(out, "<g fill=\""); put_attr(out, rst[e]->text); buf_lit(out, "\">"); }
        Str l = g.elabel[e];
        const char *nl = memchr(l.p, '\n', l.n);
        Str l1 = nl ? (Str){.p = l.p, .n = (size_t)(nl - l.p)} : l;
        double y = g.ely[e] - (nl ? 8 : 0);
        dg_text(out, l1, 13, g.elx[e], y, "middle", tc ? "crl crx" : "crl");
        if (nl) dg_text(out, (Str){.p = nl + 1, .n = l.n - (size_t)(nl + 1 - l.p)}, 12, g.elx[e], y + 16, "middle", tc ? "crt crx" : "crt");
        if (tc) buf_lit(out, "</g>");
    }
    for (size_t v = 0; v < g.nn; v++) {
        const CInfo *c = &ci.v[v];
        double w = g.nw[v], h = g.nh[v], x0 = g.nx[v] - w / 2, y0 = g.ny[v] - h / 2, cx = g.nx[v];
        const char *fill = FILL[c->ext][c->kind], *stroke = STROKE[c->ext][c->kind];
        bool dark = !(c->kind == K_COMPONENT);
        const EStyle *u = nst[v];
        Str fill_s = u && color_ok(u->bg) ? u->bg : (Str){.p = fill, .n = strlen(fill)};
        Str stroke_s = u && color_ok(u->border) ? u->border : (Str){.p = stroke, .n = strlen(stroke)};
        buf_lit(out, "<g class=\"c4-element\" fill=\"");
        if (u && color_ok(u->font)) put_attr(out, u->font);
        else buf_put(out, strlen(dark ? "#fff" : "#000"), dark ? "#fff" : "#000");
        buf_lit(out, "\">");
        double lt = g.ny[v] - lab[v] / 2; /* label stack top */
        if (c->form == F_DB) {
            double ry = cyl_r(w);
            buf_lit(out, "<path fill=\""); put_attr(out, fill_s); buf_lit(out, "\" stroke=\""); put_attr(out, stroke_s);
            buf_lit(out, "\" d=\"M"); dg_pt(out, x0, y0 + ry);
            buf_lit(out, "a"); buf_num(out, w / 2); buf_ch(out, ','); buf_num(out, ry); buf_lit(out, " 0 0 1 "); buf_num(out, w); buf_lit(out, ",0");
            buf_lit(out, "v"); buf_num(out, h - 2 * ry);
            buf_lit(out, "a"); buf_num(out, w / 2); buf_ch(out, ','); buf_num(out, ry); buf_lit(out, " 0 0 1 "); buf_num(out, -w); buf_lit(out, ",0");
            buf_lit(out, "z M"); dg_pt(out, x0, y0 + ry);
            buf_lit(out, "a"); buf_num(out, w / 2); buf_ch(out, ','); buf_num(out, ry); buf_lit(out, " 0 0 0 "); buf_num(out, w); buf_lit(out, ",0\"/>");
            lt = y0 + 1.5 * ry + (h - 1.5 * ry - lab[v]) / 2;
        } else if (c->form == F_QUEUE) {
            double rx = h / 2 / (2.5 + h / 50);
            buf_lit(out, "<path fill=\""); put_attr(out, fill_s); buf_lit(out, "\" stroke=\""); put_attr(out, stroke_s);
            buf_lit(out, "\" d=\"M"); dg_pt(out, x0 + rx, y0);
            buf_lit(out, "h"); buf_num(out, w - 2 * rx);
            buf_lit(out, "a"); buf_num(out, rx); buf_ch(out, ','); buf_num(out, h / 2); buf_lit(out, " 0 0 1 0,"); buf_num(out, h);
            buf_lit(out, "h"); buf_num(out, -(w - 2 * rx));
            buf_lit(out, "a"); buf_num(out, rx); buf_ch(out, ','); buf_num(out, h / 2); buf_lit(out, " 0 0 1 0,"); buf_num(out, -h);
            buf_lit(out, "z M"); dg_pt(out, x0 + w - rx, y0);
            buf_lit(out, "a"); buf_num(out, rx); buf_ch(out, ','); buf_num(out, h / 2); buf_lit(out, " 0 0 0 0,"); buf_num(out, h); buf_lit(out, "\"/>");
        } else if (c->kind == K_PERSON) { /* mermaid 11: rounded body with a large head circle above it */
            double r = head_r(w), by = y0 + 2 * r - 0.27 * r, bh = h - (by - y0);
            buf_lit(out, "<rect fill=\""); put_attr(out, fill_s); buf_lit(out, "\" stroke=\""); put_attr(out, stroke_s);
            buf_lit(out, "\" rx=\""); buf_num(out, fmin(w * 0.177, bh * 0.45)); buf_lit(out, "\" x=\""); buf_num(out, x0); buf_lit(out, "\" y=\""); buf_num(out, by);
            buf_lit(out, "\" width=\""); buf_num(out, w); buf_lit(out, "\" height=\""); buf_num(out, bh); buf_lit(out, "\"/>");
            buf_lit(out, "<circle fill=\""); put_attr(out, fill_s); buf_lit(out, "\" stroke=\""); put_attr(out, stroke_s);
            buf_lit(out, "\" cx=\""); buf_num(out, cx); buf_lit(out, "\" cy=\""); buf_num(out, y0 + r); buf_lit(out, "\" r=\""); buf_num(out, r); buf_lit(out, "\"/>");
            lt = by + (bh - lab[v]) / 2;
        } else {
            buf_lit(out, "<rect rx=\"2.5\" fill=\""); put_attr(out, fill_s); buf_lit(out, "\" stroke=\""); put_attr(out, stroke_s);
            buf_lit(out, "\" x=\""); buf_num(out, x0); buf_lit(out, "\" y=\""); buf_num(out, y0);
            buf_lit(out, "\" width=\""); buf_num(out, w); buf_lit(out, "\" height=\""); buf_num(out, h); buf_lit(out, "\"/>");
        }
        unsigned nl;
        double tw;
        label_measure_f(dn[v], C4_NF, text_width_os, &tw, &nl);
        double y = lt;
        dg_text_lh(out, dn[v], 1.1 * C4_NF, cx, y + sec_h(nl, C4_NF) / 2, "middle", "cn");
        y += sec_h(nl, C4_NF) + C4_GAP;
        {
            double twid;
            unsigned tl;
            label_measure_f(dt[v], C4_TF, text_width_os, &twid, &tl);
            dg_text_lh(out, dt[v], 1.1 * C4_TF, cx, y + sec_h(tl, C4_TF) / 2, "middle", "cs");
            y += sec_h(tl, C4_TF) + C4_GAP;
        }
        if (dw[v].n) {
            double dwid;
            unsigned dl;
            label_measure_f(dw[v], C4_DF, text_width_os, &dwid, &dl);
            dg_text_lh(out, dw[v], 1.1 * C4_DF, cx, y + sec_h(dl, C4_DF) / 2, "middle", "cd");
        }
        buf_lit(out, "</g>");
    }
    buf_lit(out, "</svg>\n");
    return out->oom ? CMM_ERR_NOMEM : CMM_OK;
}
