/* architecture(-beta): `group id(icon)[Title] (in parent)`, `service id(icon)[Label] (in group)`,
 * `junction id (in group)`, edges `a:R -- L:b`, `a:B --> T:b`, `<-->`, `-[label]-`, `a{group}:R`.
 * Layout: services sit on an integer grid derived from port constraints (BFS from each
 * component's first node, offset = sign(portA - portB)), collisions probe further along the
 * constraint; components are placed side by side. Groups are padded bounding boxes; edges are
 * orthogonal between port anchors. Built-in icons: cloud, database, disk, internet, server. */
#include <math.h>

#include "dgraph.h"
#include "diagrams.h"
#include "text.h"

typedef struct ANode { Str id, icon, label; int32_t group, gx, gy; bool junction, placed; } ANode;
typedef struct AGroup { Str id, icon, label; int32_t parent; double x0, y0, x1, y1; bool any; } AGroup;
typedef struct AEdge { uint32_t a, b; char pa, pb; bool arr_a, arr_b; Str label; } AEdge;

static constexpr double PITCH_X = 200;
static constexpr double PITCH_Y = 170;
static constexpr double IS = 80; /* icon size */
static constexpr char CSS[] =
    "#mermaid .ai{fill:#087ebf;}#mermaid .ag{fill:none;stroke:#cccccc;stroke-width:2px;stroke-dasharray:8;}"
    "#mermaid .ae{fill:none;stroke:#333;stroke-width:3px;}#mermaid .ah{fill:#333;}#mermaid .aj{fill:#333;}"
    "#mermaid .agl{font-size:16px;}#mermaid .aw{fill:none;stroke:#fff;stroke-width:3px;stroke-linecap:round;stroke-linejoin:round;}"
    "#mermaid .awf{fill:#fff;}#mermaid .ael{font-size:14px;}";

static bool idc(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || (unsigned char)c >= 0x80; }
static Str take_id(Str *s) {
    size_t i = 0;
    while (i < s->n && idc(s->p[i]) && !(s->p[i] == '-' && i + 1 < s->n && s->p[i + 1] == '-')) i++;
    Str r = {.p = s->p, .n = i};
    s->p += i, s->n -= i;
    return r;
}
static bool encl(Str *s, char o, char c, Str *out) {
    if (!s->n || s->p[0] != o) return true;
    const char *e = memchr(s->p, c, s->n);
    if (!e) return false;
    *out = (Str){.p = s->p + 1, .n = (size_t)(e - s->p - 1)};
    s->n -= (size_t)(e + 1 - s->p), s->p = e + 1;
    return true;
}
static int pdx(char p) { return p == 'R' ? 1 : p == 'L' ? -1 : 0; }
static int pdy(char p) { return p == 'B' ? 1 : p == 'T' ? -1 : 0; }
static int sgn(int v) { return (v > 0) - (v < 0); }

static void glyph(Buf *b, Str icon, double x, double y, double s) { /* x,y top-left of s×s tile */
    double u = s / 80;
    #define P(px, py) dg_pt(b, x + (px) * u, y + (py) * u)
    if (str_eq(icon, S("database"))) {
        buf_lit(b, "<path class=\"aw\" d=\"M"); P(20, 22); buf_lit(b, "a"); buf_num(b, 20 * u); buf_ch(b, ','); buf_num(b, 7 * u);
        buf_lit(b, " 0 0 0 "); buf_num(b, 40 * u); buf_lit(b, ",0a"); buf_num(b, 20 * u); buf_ch(b, ','); buf_num(b, 7 * u); buf_lit(b, " 0 0 0 ");
        buf_num(b, -40 * u); buf_lit(b, ",0v"); buf_num(b, 36 * u);
        for (int k = 0; k < 3; k++) {
            if (k) { buf_ch(b, 'M'); P(20, 22 + 12 * k + (k == 2 ? 12 : 0)); }
            if (k == 2) break;
        }
        buf_lit(b, "a"); buf_num(b, 20 * u); buf_ch(b, ','); buf_num(b, 7 * u); buf_lit(b, " 0 0 0 "); buf_num(b, 40 * u); buf_lit(b, ",0v");
        buf_num(b, -36 * u); buf_ch(b, 'M'); P(20, 40); buf_lit(b, "a"); buf_num(b, 20 * u); buf_ch(b, ','); buf_num(b, 7 * u);
        buf_lit(b, " 0 0 0 "); buf_num(b, 40 * u); buf_lit(b, ",0\"/>");
    } else if (str_eq(icon, S("disk"))) {
        buf_lit(b, "<rect class=\"aw\" x=\""); buf_num(b, x + 18 * u); buf_lit(b, "\" y=\""); buf_num(b, y + 18 * u);
        buf_lit(b, "\" width=\""); buf_num(b, 44 * u); buf_lit(b, "\" height=\""); buf_num(b, 44 * u); buf_lit(b, "\" rx=\""); buf_num(b, 4 * u); buf_lit(b, "\"/>");
        buf_lit(b, "<circle class=\"aw\" cx=\""); buf_num(b, x + 40 * u); buf_lit(b, "\" cy=\""); buf_num(b, y + 40 * u);
        buf_lit(b, "\" r=\""); buf_num(b, 13 * u); buf_lit(b, "\"/><circle class=\"awf\" cx=\""); buf_num(b, x + 40 * u);
        buf_lit(b, "\" cy=\""); buf_num(b, y + 40 * u); buf_lit(b, "\" r=\""); buf_num(b, 3 * u); buf_lit(b, "\"/>");
    } else if (str_eq(icon, S("server"))) {
        for (int k = 0; k < 3; k++) {
            buf_lit(b, "<rect class=\"aw\" x=\""); buf_num(b, x + 18 * u); buf_lit(b, "\" y=\""); buf_num(b, y + (17 + 16 * k) * u);
            buf_lit(b, "\" width=\""); buf_num(b, 44 * u); buf_lit(b, "\" height=\""); buf_num(b, 12 * u); buf_lit(b, "\" rx=\""); buf_num(b, 2 * u); buf_lit(b, "\"/>");
            buf_lit(b, "<circle class=\"awf\" cx=\""); buf_num(b, x + 26 * u); buf_lit(b, "\" cy=\""); buf_num(b, y + (23 + 16 * k) * u);
            buf_lit(b, "\" r=\""); buf_num(b, 2 * u); buf_lit(b, "\"/>");
        }
    } else if (str_eq(icon, S("internet"))) {
        buf_lit(b, "<circle class=\"aw\" cx=\""); buf_num(b, x + 40 * u); buf_lit(b, "\" cy=\""); buf_num(b, y + 40 * u);
        buf_lit(b, "\" r=\""); buf_num(b, 22 * u); buf_lit(b, "\"/><ellipse class=\"aw\" cx=\""); buf_num(b, x + 40 * u);
        buf_lit(b, "\" cy=\""); buf_num(b, y + 40 * u); buf_lit(b, "\" rx=\""); buf_num(b, 9 * u); buf_lit(b, "\" ry=\""); buf_num(b, 22 * u);
        buf_lit(b, "\"/><path class=\"aw\" d=\"M"); P(18, 40); buf_ch(b, 'H'); buf_num(b, x + 62 * u); buf_ch(b, 'M'); P(22, 29);
        buf_ch(b, 'H'); buf_num(b, x + 58 * u); buf_ch(b, 'M'); P(22, 51); buf_ch(b, 'H'); buf_num(b, x + 58 * u); buf_lit(b, "\"/>");
    } else if (str_eq(icon, S("cloud"))) {
        buf_lit(b, "<path class=\"aw\" d=\"M"); P(26, 54); buf_lit(b, "a"); buf_num(b, 10 * u); buf_ch(b, ','); buf_num(b, 10 * u);
        buf_lit(b, " 0 0 1 "); buf_num(b, 2 * u); buf_ch(b, ','); buf_num(b, -19.8 * u); buf_lit(b, "a"); buf_num(b, 13 * u); buf_ch(b, ',');
        buf_num(b, 13 * u); buf_lit(b, " 0 0 1 "); buf_num(b, 24 * u); buf_ch(b, ','); buf_num(b, -3 * u); buf_lit(b, "a"); buf_num(b, 10 * u);
        buf_ch(b, ','); buf_num(b, 10 * u); buf_lit(b, " 0 0 1 "); buf_num(b, 2 * u); buf_ch(b, ','); buf_num(b, 22.8 * u); buf_lit(b, "z\"/>");
    } else { /* unknown icon: question mark */
        buf_lit(b, "<text class=\"awf\" text-anchor=\"middle\" dominant-baseline=\"central\" font-size=\""); buf_num(b, 40 * u);
        buf_lit(b, "\" x=\""); buf_num(b, x + 40 * u); buf_lit(b, "\" y=\""); buf_num(b, y + 40 * u); buf_lit(b, "\">?</text>");
    }
    #undef P
}

DIAG_FN(architecture_render) {
    LineIt it = {.s = src, .n = n, .i = off};
    Str line;
    size_t lo;
    if (!dg_next_line(&it, &line, &lo)) return diag_fail(err_off, msg, off, "empty architecture diagram");
    VEC(ANode) nv = {};
    VEC(AGroup) gv = {};
    VEC(AEdge) ev = {};
    while (dg_next_line(&it, &line, &lo)) {
        Str l = line, w = dg_word(&l);
        l = str_trim(l);
        bool isg = str_eq(w, S("group")), iss = str_eq(w, S("service")), isj = str_eq(w, S("junction"));
        if (isg || iss || isj) {
            Str id = take_id(&l), icon = {}, label = {};
            if (!id.n) return diag_fail(err_off, msg, lo, "expected id");
            if (!isj && (!encl(&l, '(', ')', &icon) || !encl(&l, '[', ']', &label))) return diag_fail(err_off, msg, lo, "unterminated icon or label");
            l = str_trim(l);
            int32_t par = NONE;
            if (l.n) {
                Str kw = dg_word(&l);
                l = str_trim(l);
                if (!str_eq(kw, S("in")) || !l.n) return diag_fail(err_off, msg, lo, "expected 'in <group>'");
                for (size_t k = 0; k < gv.n; k++) if (str_eq(gv.v[k].id, l)) par = (int32_t)k;
                if (par == NONE) return diag_fail(err_off, msg, (size_t)(l.p - src), "unknown group");
            }
            if (isg) {
                for (size_t k = 0; k < gv.n; k++) if (str_eq(gv.v[k].id, id)) return diag_fail(err_off, msg, lo, "duplicate group id");
                if (!VEC_PUSH(a, &gv, ((AGroup){.id = id, .icon = icon, .label = label, .parent = par}))) return CMM_ERR_NOMEM;
            } else {
                for (size_t k = 0; k < nv.n; k++) if (str_eq(nv.v[k].id, id)) return diag_fail(err_off, msg, lo, "duplicate service id");
                if (!VEC_PUSH(a, &nv, ((ANode){.id = id, .icon = icon, .label = label, .group = par, .junction = isj}))) return CMM_ERR_NOMEM;
            }
            continue;
        }
        /* edge: id[{group}]:P <?--[label]--? >? P:id[{group}] */
        Str s = line, ia = take_id(&s), dummy;
        if (!ia.n || !encl(&s, '{', '}', &dummy) || s.n < 2 || s.p[0] != ':') return diag_fail(err_off, msg, lo, "expected service, group, junction or edge");
        char pa = s.p[1];
        s.p += 2, s.n -= 2;
        s = str_trim(s);
        bool arr_a = false, arr_b = false;
        Str lab = {};
        if (s.n && s.p[0] == '<') arr_a = true, s.p++, s.n--;
        size_t dsh = 0;
        while (dsh < s.n && s.p[dsh] == '-') dsh++;
        if (!dsh) return diag_fail(err_off, msg, lo, "expected '--'");
        s.p += dsh, s.n -= dsh;
        if (s.n && s.p[0] == '[') {
            if (!encl(&s, '[', ']', &lab)) return diag_fail(err_off, msg, lo, "unterminated edge label");
            dsh = 0;
            while (dsh < s.n && s.p[dsh] == '-') dsh++;
            s.p += dsh, s.n -= dsh;
        } else if (dsh < 2) return diag_fail(err_off, msg, lo, "expected '--'");
        if (s.n && s.p[0] == '>') arr_b = true, s.p++, s.n--;
        s = str_trim(s);
        if (s.n < 3 || s.p[1] != ':') return diag_fail(err_off, msg, lo, "expected 'P:target'");
        char pb = s.p[0];
        s.p += 2, s.n -= 2;
        Str ib = take_id(&s);
        if (!ib.n || !encl(&s, '{', '}', &dummy) || str_trim(s).n) return diag_fail(err_off, msg, lo, "bad edge target");
        if (!strchr("LRTB", pa) || !strchr("LRTB", pb) || !pa || !pb) return diag_fail(err_off, msg, lo, "port must be L, R, T or B");
        uint32_t A = UINT32_MAX, B = UINT32_MAX;
        for (size_t k = 0; k < nv.n; k++) { if (str_eq(nv.v[k].id, ia)) A = (uint32_t)k; if (str_eq(nv.v[k].id, ib)) B = (uint32_t)k; }
        if (A == UINT32_MAX || B == UINT32_MAX) return diag_fail(err_off, msg, lo, "edge references unknown service");
        if (!VEC_PUSH(a, &ev, ((AEdge){.a = A, .b = B, .pa = pa, .pb = pb, .arr_a = arr_a, .arr_b = arr_b, .label = lab}))) return CMM_ERR_NOMEM;
    }
    if (!nv.n) return diag_fail(err_off, msg, off, "architecture diagram has no services");
    ANode *N = nv.v;
    size_t nn = nv.n;
    uint32_t *q = ANEW(a, uint32_t, nn);
    if (!q) return CMM_ERR_NOMEM;
    int32_t xoff = 0;
    for (size_t root = 0; root < nn; root++) {
        if (N[root].placed) continue;
        N[root].gx = N[root].gy = 0, N[root].placed = true;
        size_t h = 0, t = 0, c0 = 0;
        q[t++] = (uint32_t)root;
        while (h < t) {
            uint32_t u = q[h++];
            for (size_t e = 0; e < ev.n; e++) {
                const AEdge *E = &ev.v[e];
                uint32_t v;
                int dx, dy;
                if (E->a == u && !N[E->b].placed) v = E->b, dx = sgn(pdx(E->pa) - pdx(E->pb)), dy = sgn(pdy(E->pa) - pdy(E->pb));
                else if (E->b == u && !N[E->a].placed) v = E->a, dx = sgn(pdx(E->pb) - pdx(E->pa)), dy = sgn(pdy(E->pb) - pdy(E->pa));
                else continue;
                if (!dx && !dy) dx = E->a == u ? pdx(E->pa) : pdx(E->pb), dy = E->a == u ? pdy(E->pa) : pdy(E->pb);
                int32_t x = N[u].gx + dx, y = N[u].gy + dy;
                for (bool hit = true; hit;) { /* probe along the constraint until free */
                    hit = false;
                    for (size_t k = c0; k < t; k++) if (N[q[k]].gx == x && N[q[k]].gy == y) { hit = true; break; }
                    if (hit) x += dx, y += dy;
                }
                N[v].gx = x, N[v].gy = y, N[v].placed = true;
                q[t++] = v;
            }
        }
        int32_t mx = INT32_MAX, Mx = INT32_MIN;
        for (size_t k = c0; k < t; k++) mx = N[q[k]].gx < mx ? N[q[k]].gx : mx, Mx = N[q[k]].gx > Mx ? N[q[k]].gx : Mx;
        for (size_t k = c0; k < t; k++) N[q[k]].gx += xoff - mx;
        xoff += Mx - mx + 1;
    }
    int32_t my = INT32_MAX;
    for (size_t i = 0; i < nn; i++) if (N[i].gy < my) my = N[i].gy;
    double *cx = ANEW(a, double, nn), *cy = ANEW(a, double, nn);
    if (!cx || !cy) return CMM_ERR_NOMEM;
    for (size_t i = 0; i < nn; i++) cx[i] = (N[i].gx) * PITCH_X, cy[i] = (N[i].gy - my) * PITCH_Y;
    /* groups: bbox of members, then parents absorb children (children have larger indices) */
    for (size_t i = 0; i < nn; i++) {
        for (int32_t g = N[i].group; g != NONE; g = gv.v[g].parent) {
            AGroup *G = &gv.v[g];
            double hw = N[i].junction ? 5 : fmax(IS / 2, text_width(N[i].label, fs) / 2), bot = N[i].junction ? 5 : IS / 2 + 26;
            double x0 = cx[i] - hw - 20, x1 = cx[i] + hw + 20, y0 = cy[i] - IS / 2 - 20, y1 = cy[i] + bot + 15;
            if (!G->any) G->x0 = x0, G->x1 = x1, G->y0 = y0, G->y1 = y1, G->any = true;
            else G->x0 = fmin(G->x0, x0), G->x1 = fmax(G->x1, x1), G->y0 = fmin(G->y0, y0), G->y1 = fmax(G->y1, y1);
        }
    }
    for (size_t g = gv.n; g-- > 0;) {
        AGroup *G = &gv.v[g];
        if (!G->any) G->x0 = G->y0 = 0, G->x1 = G->y1 = 60, G->any = true;
        if (G->parent != NONE) {
            AGroup *P = &gv.v[G->parent];
            double pad = 15;
            if (!P->any) P->x0 = G->x0 - pad, P->y0 = G->y0 - pad - 30, P->x1 = G->x1 + pad, P->y1 = G->y1 + pad, P->any = true;
            else P->x0 = fmin(P->x0, G->x0 - pad), P->y0 = fmin(P->y0, G->y0 - pad - 30), P->x1 = fmax(P->x1, G->x1 + pad), P->y1 = fmax(P->y1, G->y1 + pad);
        }
    }
    for (size_t g = 0; g < gv.n; g++) gv.v[g].y0 -= 30; /* title band */
    BBox bb = {INFINITY, INFINITY, -INFINITY, -INFINITY};
    for (size_t i = 0; i < nn; i++) {
        double hw = fmax(IS / 2, text_width(N[i].label, fs) / 2 + 4);
        bb_add(&bb, cx[i] - hw, cy[i] - IS / 2, cx[i] + hw, cy[i] + IS / 2 + 30);
    }
    for (size_t g = 0; g < gv.n; g++) bb_add(&bb, gv.v[g].x0, gv.v[g].y0, gv.v[g].x1, gv.v[g].y1);
    if (title.n) bb.y0 -= 34;
    if (!buf_reserve(out, 4096 + nn * 1500 + ev.n * 300 + gv.n * 1200)) return CMM_ERR_NOMEM;
    DG_HEAD(out, bb, 16, "architecture", CSS);
    if (title.n) dg_text(out, title, 18, (bb.x0 + bb.x1) / 2, bb.y0 + 14, "middle", "agl");
    for (size_t g = 0; g < gv.n; g++) {
        const AGroup *G = &gv.v[g];
        dg_rect(out, G->x0, G->y0, G->x1 - G->x0, G->y1 - G->y0, 0, "ag");
        double tx = G->x0 + 6;
        if (G->icon.n) {
            dg_rect(out, tx, G->y0 + 3, 24, 24, 3, "ai");
            glyph(out, G->icon, tx, G->y0 + 3, 24);
            tx += 30;
        }
        dg_text(out, G->label.n ? G->label : G->id, fs, tx, G->y0 + 15, "start", "agl");
    }
    for (size_t e = 0; e < ev.n; e++) {
        const AEdge *E = &ev.v[e];
        double ra = N[E->a].junction ? 0 : IS / 2, rb = N[E->b].junction ? 0 : IS / 2;
        double x0 = cx[E->a] + pdx(E->pa) * ra, y0 = cy[E->a] + pdy(E->pa) * ra;
        double x1 = cx[E->b] + pdx(E->pb) * rb, y1 = cy[E->b] + pdy(E->pb) * rb;
        bool ha = pdx(E->pa) != 0, hb = pdx(E->pb) != 0;
        double P[8];
        size_t np = 0;
        P[np++] = x0, P[np++] = y0;
        if (ha && hb && fabs(y0 - y1) > 0.5) { double mx = (x0 + x1) / 2; P[np++] = mx, P[np++] = y0, P[np++] = mx, P[np++] = y1; }
        else if (!ha && !hb && fabs(x0 - x1) > 0.5) { double m = (y0 + y1) / 2; P[np++] = x0, P[np++] = m, P[np++] = x1, P[np++] = m; }
        else if (ha != hb) { if (ha) P[np++] = x1, P[np++] = y0; else P[np++] = x0, P[np++] = y1; }
        P[np++] = x1, P[np++] = y1;
        size_t k = np / 2;
        buf_lit(out, "<path class=\"ae\" d=\"M");
        for (size_t i = 0; i < k; i++) { if (i) buf_ch(out, 'L'); dg_pt(out, P[2 * i], P[2 * i + 1]); }
        buf_lit(out, "\"/>");
        for (int end = 0; end < 2; end++) {
            if (!(end ? E->arr_b : E->arr_a)) continue;
            const double *T = end ? P + 2 * (k - 1) : P, *F = end ? P + 2 * (k - 2) : P + 2;
            double dx = T[0] - F[0], dy = T[1] - F[1], L = sqrt(dx * dx + dy * dy);
            if (L < 1e-9) continue;
            dx /= L, dy /= L;
            buf_lit(out, "<path class=\"ah\" d=\"M"); dg_pt(out, T[0], T[1]); buf_ch(out, 'L');
            dg_pt(out, T[0] - dx * 12 - dy * 6, T[1] - dy * 12 + dx * 6); buf_ch(out, 'L');
            dg_pt(out, T[0] - dx * 12 + dy * 6, T[1] - dy * 12 - dx * 6); buf_lit(out, "z\"/>");
        }
        if (E->label.n) {
            size_t mid = (k - 1) / 2;
            double lx = (P[2 * mid] + P[2 * mid + 2]) / 2, ly = (P[2 * mid + 1] + P[2 * mid + 3]) / 2;
            double tw = text_width(E->label, 14);
            dg_rect_fill(out, lx - tw / 2 - 3, ly - 10, tw + 6, 20, 0, "ael", "#fff");
            dg_text(out, E->label, 14, lx, ly, "middle", "ael");
        }
    }
    for (size_t i = 0; i < nn; i++) {
        if (N[i].junction) {
            buf_lit(out, "<circle class=\"aj\" cx=\""); buf_num(out, cx[i]); buf_lit(out, "\" cy=\""); buf_num(out, cy[i]); buf_lit(out, "\" r=\"4\"/>");
            continue;
        }
        buf_lit(out, "<g class=\"architecture-service\" id=\"service-"); buf_xml(out, N[i].id); buf_lit(out, "\">");
        dg_rect(out, cx[i] - IS / 2, cy[i] - IS / 2, IS, IS, 6, "ai");
        glyph(out, N[i].icon, cx[i] - IS / 2, cy[i] - IS / 2, IS);
        Str lab = N[i].label.n ? N[i].label : N[i].id;
        dg_text(out, lab, fs, cx[i], cy[i] + IS / 2 + 14, "middle", nullptr);
        buf_lit(out, "</g>");
    }
    buf_lit(out, "</svg>\n");
    return out->oom ? CMM_ERR_NOMEM : CMM_OK;
}
