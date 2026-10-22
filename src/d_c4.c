/* C4Context / C4Container / C4Component / C4Dynamic / C4Deployment.
 * Elements: Person, System, SystemDb, SystemQueue, Container*, Component* (+ _Ext variants);
 * boundaries: Boundary, Enterprise_Boundary, System_Boundary, Container_Boundary, Deployment_Node,
 * Node, Node_L, Node_R with `{ ... }`; relations Rel, BiRel, Rel_U/D/L/R/Up/Down/Left/Right/Back,
 * RelIndex. Named `$k=v` args and Update*Style/UpdateLayoutConfig are accepted and ignored.
 * Layout uses flow_layout with boundaries as clusters. */
#include <math.h>

#include "dgraph.h"
#include "diagrams.h"
#include "text.h"

enum { K_PERSON, K_SYSTEM, K_CONTAINER, K_COMPONENT };
enum { F_PLAIN, F_DB, F_QUEUE };
typedef struct CInfo { Str stereo, techn, descr; uint8_t kind, form; bool ext; } CInfo;
typedef struct CRel { Str techn; bool bi; } CRel;

static constexpr char CSS[] =
    "#mermaid .ct{fill:#fff;}#mermaid .ct.ext{fill:#fff;}#mermaid .cn{font-weight:bold;}#mermaid .cs{font-size:12px;font-style:italic;}"
    "#mermaid .cd{font-size:14px;}#mermaid .cb{fill:none;stroke:#444;stroke-width:1px;stroke-dasharray:7,7;}"
    "#mermaid .cbt{font-weight:bold;font-size:14px;fill:#444;}#mermaid .cbs{font-size:12px;fill:#444;}"
    "#mermaid .cr{fill:none;stroke:#444;stroke-width:1px;}#mermaid .crh{fill:#444;}#mermaid .crl{font-size:13px;fill:#444;}"
    "#mermaid .crt{font-size:12px;font-style:italic;fill:#444;}";
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

static bool is(Str s, const char *lit) { return s.n == strlen(lit) && !memcmp(s.p, lit, s.n); }

static void arrowhead(Buf *b, const double *A, const double *B) { /* tip at A, coming from B */
    double dx = A[0] - B[0], dy = A[1] - B[1], L = sqrt(dx * dx + dy * dy);
    if (L < 1e-9) return;
    dx /= L, dy /= L;
    buf_lit(b, "<path class=\"crh\" d=\"M"); dg_pt(b, A[0], A[1]); buf_ch(b, 'L');
    dg_pt(b, A[0] - dx * 10 - dy * 4, A[1] - dy * 10 + dx * 4); buf_ch(b, 'L');
    dg_pt(b, A[0] - dx * 10 + dy * 4, A[1] - dy * 10 - dx * 4); buf_lit(b, "z\"/>");
}

DIAG_FN(c4_render) {
    Graph g;
    dg_init(&g, a, DIR_TB, fs);
    LineIt it = {.s = src, .n = n, .i = off};
    Str line;
    size_t lo;
    if (!dg_next_line(&it, &line, &lo)) return diag_fail(err_off, msg, off, "empty C4 diagram");
    VEC(CInfo) ci = {};
    VEC(CRel) cr = {};
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
        while (ci.n < g.nn) if (!VEC_PUSH(a, &ci, (CInfo){.kind = K_SYSTEM, .stereo = S("system")})) return CMM_ERR_NOMEM;
        ci.v[v] = info;
    }
    if (depth) return diag_fail(err_off, msg, n, "missing '}'");
    if (!g.nn) return diag_fail(err_off, msg, off, "C4 diagram has no elements");
    while (ci.n < g.nn) if (!VEC_PUSH(a, &ci, (CInfo){.kind = K_SYSTEM, .stereo = S("system")})) return CMM_ERR_NOMEM;
    Str *dw = ANEW(a, Str, g.nn);
    if (!dw) return CMM_ERR_NOMEM;
    for (size_t v = 0; v < g.nn; v++) {
        CInfo *c = &ci.v[v];
        dw[v] = c->descr.n ? dg_wrap(a, c->descr, 14, 190) : (Str){};
        double w, tw;
        unsigned nl = 0;
        label_measure(g.nlabel[v], fs, &tw, &nl);
        w = fmax(216, tw + 30);
        double h = 20 + 18 + nl * fs * 1.3 + (c->techn.n ? 18 : 0) + 8;
        if (dw[v].n) {
            double dwid;
            unsigned dl;
            label_measure(dw[v], 14, &dwid, &dl);
            h += dl * 18 + 6;
            w = fmax(w, dwid + 30);
        }
        if (c->kind == K_PERSON) h += 48;
        if (c->form == F_DB) h += 14;
        dg_size(&g, (int32_t)v, w, fmax(h, 60 + (c->kind == K_PERSON ? 48 : 0)));
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
    cmm_status st = flow_layout(&g);
    if (st) return st;
    BBox bb = dg_bbox(&g);
    if (ttl.n) bb.y0 -= 34;
    if (!buf_reserve(out, 2048 + g.nn * 900 + g.ne * 500 + g.ns * 300)) return CMM_ERR_NOMEM;
    DG_HEAD(out, bb, 12, "c4", CSS);
    if (ttl.n) dg_text(out, ttl, 18, (bb.x0 + bb.x1) / 2, bb.y0 + 14, "middle", "cn");
    for (size_t s = 0; s < g.ns; s++) {
        dg_rect(out, g.sx[s], g.sy[s], g.sw[s], g.sh[s], 2.5, "cb");
        Str t = g.stitle[s];
        const char *nl = memchr(t.p, '\n', t.n);
        Str t1 = nl ? (Str){.p = t.p, .n = (size_t)(nl - t.p)} : t;
        dg_text(out, t1, 14, g.sx[s] + 10, g.sy[s] + 14, "start", "cbt");
        if (nl) dg_text(out, (Str){.p = nl + 1, .n = t.n - (size_t)(nl + 1 - t.p)}, 12, g.sx[s] + 10, g.sy[s] + 30, "start", "cbs");
    }
    for (size_t e = 0; e < g.ne; e++) {
        uint32_t np = g.eptn[e];
        if (np < 2) continue;
        buf_lit(out, "<path class=\"cr\" d=\""); dg_edge_d(out, &g, e); buf_lit(out, "\"/>");
        const double *P = g.pts.v + g.eptoff[e];
        arrowhead(out, P + 2 * (np - 1), P + 2 * (np - 2));
        if (cr.v[e].bi) arrowhead(out, P, P + 2);
        Str l = g.elabel[e];
        const char *nl = memchr(l.p, '\n', l.n);
        Str l1 = nl ? (Str){.p = l.p, .n = (size_t)(nl - l.p)} : l;
        double y = g.ely[e] - (nl ? 8 : 0);
        dg_text(out, l1, 13, g.elx[e], y, "middle", "crl");
        if (nl) dg_text(out, (Str){.p = nl + 1, .n = l.n - (size_t)(nl + 1 - l.p)}, 12, g.elx[e], y + 16, "middle", "crt");
    }
    for (size_t v = 0; v < g.nn; v++) {
        const CInfo *c = &ci.v[v];
        double w = g.nw[v], h = g.nh[v], x0 = g.nx[v] - w / 2, y0 = g.ny[v] - h / 2, cx = g.nx[v];
        const char *fill = FILL[c->ext][c->kind], *stroke = STROKE[c->ext][c->kind];
        bool dark = !(c->kind == K_COMPONENT);
        buf_lit(out, "<g class=\"c4-element\" fill=\""); buf_put(out, strlen(dark ? "#fff" : "#000"), dark ? "#fff" : "#000"); buf_lit(out, "\">");
        if (c->form == F_DB) {
            double ry = 10;
            buf_lit(out, "<path fill=\""); buf_put(out, strlen(fill), fill); buf_lit(out, "\" stroke=\""); buf_put(out, strlen(stroke), stroke);
            buf_lit(out, "\" d=\"M"); dg_pt(out, x0, y0 + ry);
            buf_lit(out, "a"); buf_num(out, w / 2); buf_ch(out, ','); buf_num(out, ry); buf_lit(out, " 0 0 1 "); buf_num(out, w); buf_lit(out, ",0");
            buf_lit(out, "v"); buf_num(out, h - 2 * ry);
            buf_lit(out, "a"); buf_num(out, w / 2); buf_ch(out, ','); buf_num(out, ry); buf_lit(out, " 0 0 1 "); buf_num(out, -w); buf_lit(out, ",0");
            buf_lit(out, "z M"); dg_pt(out, x0, y0 + ry);
            buf_lit(out, "a"); buf_num(out, w / 2); buf_ch(out, ','); buf_num(out, ry); buf_lit(out, " 0 0 0 "); buf_num(out, w); buf_lit(out, ",0\"/>");
            y0 += 2 * ry - 4;
        } else if (c->form == F_QUEUE) {
            double rx = 10;
            buf_lit(out, "<path fill=\""); buf_put(out, strlen(fill), fill); buf_lit(out, "\" stroke=\""); buf_put(out, strlen(stroke), stroke);
            buf_lit(out, "\" d=\"M"); dg_pt(out, x0 + rx, y0);
            buf_lit(out, "h"); buf_num(out, w - 2 * rx);
            buf_lit(out, "a"); buf_num(out, rx); buf_ch(out, ','); buf_num(out, h / 2); buf_lit(out, " 0 0 1 0,"); buf_num(out, h);
            buf_lit(out, "h"); buf_num(out, -(w - 2 * rx));
            buf_lit(out, "a"); buf_num(out, rx); buf_ch(out, ','); buf_num(out, h / 2); buf_lit(out, " 0 0 1 0,"); buf_num(out, -h);
            buf_lit(out, "z M"); dg_pt(out, x0 + w - rx, y0);
            buf_lit(out, "a"); buf_num(out, rx); buf_ch(out, ','); buf_num(out, h / 2); buf_lit(out, " 0 0 0 0,"); buf_num(out, h); buf_lit(out, "\"/>");
        } else {
            buf_lit(out, "<rect rx=\"2.5\" fill=\""); buf_put(out, strlen(fill), fill); buf_lit(out, "\" stroke=\""); buf_put(out, strlen(stroke), stroke);
            buf_lit(out, "\" x=\""); buf_num(out, x0); buf_lit(out, "\" y=\""); buf_num(out, y0);
            buf_lit(out, "\" width=\""); buf_num(out, w); buf_lit(out, "\" height=\""); buf_num(out, h); buf_lit(out, "\"/>");
        }
        double y = y0 + 16;
        if (c->kind == K_PERSON) { /* head + shoulders glyph */
            buf_lit(out, "<circle fill=\"#fff\" fill-opacity=\".9\" cx=\""); buf_num(out, cx); buf_lit(out, "\" cy=\""); buf_num(out, y + 10);
            buf_lit(out, "\" r=\"10\"/><path fill=\"#fff\" fill-opacity=\".9\" d=\"M"); dg_pt(out, cx - 18, y + 42);
            buf_lit(out, "q0,-20 18,-20 q18,0 18,20 z\"/>");
            y += 50;
        }
        buf_lit(out, "<text class=\"cs\" text-anchor=\"middle\" dominant-baseline=\"central\" x=\""); buf_num(out, cx);
        buf_lit(out, "\" y=\""); buf_num(out, y); buf_lit(out, "\">&lt;&lt;"); buf_xml(out, c->stereo); buf_lit(out, "&gt;&gt;</text>");
        double tw;
        unsigned nl;
        label_measure(g.nlabel[v], fs, &tw, &nl);
        y += 10 + nl * fs * 1.3 / 2;
        dg_text(out, g.nlabel[v], fs, cx, y, "middle", "cn");
        y += nl * fs * 1.3 / 2 + 4;
        if (c->techn.n) {
            dg_text_open(out, cx, y + 8, "middle", "cs");
            buf_ch(out, '['); buf_xml(out, c->techn); buf_lit(out, "]</text>");
            y += 18;
        }
        if (dw[v].n) {
            double dwid;
            unsigned dl;
            label_measure(dw[v], 14, &dwid, &dl);
            dg_text(out, dw[v], 14, cx, y + 6 + dl * 9, "middle", "cd");
        }
        buf_lit(out, "</g>");
    }
    buf_lit(out, "</svg>\n");
    return out->oom ? CMM_ERR_NOMEM : CMM_OK;
}
