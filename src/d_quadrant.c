/* quadrantChart: title, x-axis/y-axis "low --> high", quadrant-1..4 labels, points
 * "Name: [x, y]" with optional "radius: r, color: #hex, stroke-color: .., stroke-width: Npx"
 * and ":::class" (classDef supports the same keys). */
#include "dgraph.h"
#include "diagrams.h"
#include "text.h"

typedef struct QStyle { double r, sw; Str fill, stroke; } QStyle;
typedef struct QPt { Str name; double x, y; QStyle st; } QPt;
typedef struct QCls { Str name; QStyle st; } QCls;

static constexpr char CSS[] =
    "#mermaid .q1{fill:#ECECFF;}#mermaid .q2{fill:#f1f1ff;}#mermaid .q3{fill:#f6f6ff;}#mermaid .q4{fill:#fbfbff;}"
    "#mermaid .bd{stroke:#c7c7f1;stroke-width:2px;fill:none;}#mermaid .in{stroke:#c7c7f1;stroke-width:1px;}"
    "#mermaid .ql{font-size:16px;fill:#131300;}#mermaid .al{font-size:16px;fill:#131300;}"
    "#mermaid .pl{font-size:12px;fill:#131300;}#mermaid .ttl{font-size:20px;}";

static void parse_style(Str s, QStyle *st) {
    while (s.n) {
        const char *c = memchr(s.p, ',', s.n);
        size_t k = c ? (size_t)(c - s.p) : s.n;
        Str kv = str_trim((Str){.p = s.p, .n = k});
        s = c ? (Str){.p = c + 1, .n = s.n - k - 1} : (Str){};
        const char *col = memchr(kv.p, ':', kv.n);
        if (!col) continue;
        Str key = str_trim((Str){.p = kv.p, .n = (size_t)(col - kv.p)});
        Str val = str_trim((Str){.p = col + 1, .n = kv.n - (size_t)(col + 1 - kv.p)});
        if (val.n > 2 && !memcmp(val.p + val.n - 2, "px", 2)) val.n -= 2;
        double v;
        if (str_eq(key, S("radius")) && dg_parse_num(val, &v) && v >= 0 && v < 1000) st->r = v;
        else if (str_eq(key, S("stroke-width")) && dg_parse_num(val, &v) && v >= 0 && v < 100) st->sw = v;
        else if (str_eq(key, S("color"))) st->fill = val;
        else if (str_eq(key, S("stroke-color"))) st->stroke = val;
    }
}
static bool safe_color(Str c) {
    for (size_t i = 0; i < c.n; i++) {
        char ch = c.p[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '#' ||
              ch == '(' || ch == ')' || ch == ',' || ch == '.' || ch == ' ' || ch == '%'))
            return false;
    }
    return c.n > 0;
}
/* split "a --> b" */
static void axis_pair(Str s, Str *lo, Str *hi) {
    for (size_t i = 0; i + 3 <= s.n; i++)
        if (!memcmp(s.p + i, "-->", 3)) {
            *lo = str_trim((Str){.p = s.p, .n = i});
            *hi = str_trim((Str){.p = s.p + i + 3, .n = s.n - i - 3});
            return;
        }
    *lo = str_trim(s), *hi = (Str){};
}
static Str unq(Str s) {
    s = str_trim(s);
    if (s.n >= 2 && s.p[0] == '"' && s.p[s.n - 1] == '"') s = (Str){.p = s.p + 1, .n = s.n - 2};
    return s;
}

DIAG_FN(quadrant_render) {
    (void)fs;
    LineIt it = {.s = src, .n = n, .i = off};
    Str line;
    size_t lo;
    if (!dg_next_line(&it, &line, &lo)) return diag_fail(err_off, msg, off, "empty quadrantChart");
    Str xl = {}, xh = {}, yl = {}, yh = {}, q[4] = {};
    VEC(QPt) pts = {};
    VEC(QCls) cls = {};
    VEC(Str) pcls = {};
    while (dg_next_line(&it, &line, &lo)) {
        Str rest = line, w = dg_word(&rest);
        rest = str_trim(rest);
        if (str_eq(w, S("title"))) { title = rest; continue; }
        if (str_starts(w, S("acc"))) continue;
        if (str_eq(w, S("x-axis"))) { axis_pair(rest, &xl, &xh); continue; }
        if (str_eq(w, S("y-axis"))) { axis_pair(rest, &yl, &yh); continue; }
        if (w.n == 10 && str_starts(w, S("quadrant-")) && w.p[9] >= '1' && w.p[9] <= '4') { q[w.p[9] - '1'] = rest; continue; }
        if (str_eq(w, S("classDef"))) {
            Str nm = dg_word(&rest);
            QCls c = {.name = nm};
            parse_style(rest, &c.st);
            if (!VEC_PUSH(a, &cls, c)) return CMM_ERR_NOMEM;
            continue;
        }
        const char *br = memchr(line.p, '[', line.n);
        const char *ke = br ? memchr(br, ']', line.n - (size_t)(br - line.p)) : nullptr;
        const char *col = memchr(line.p, ':', line.n);
        if (!br || !ke || !col || col > br) return diag_fail(err_off, msg, lo, "expected 'Name: [x, y]'");
        QPt p = {.name = (Str){.p = line.p, .n = (size_t)(col - line.p)}};
        Str cn = {};
        for (size_t i = 0; i + 3 <= p.name.n; i++)
            if (!memcmp(p.name.p + i, ":::", 3)) { cn = str_trim((Str){.p = p.name.p + i + 3, .n = p.name.n - i - 3}); p.name.n = i; break; }
        /* "Name:::cls: [..]" — the first ':' may belong to ':::' */
        if (col + 2 < line.p + line.n && col[1] == ':' && col[2] == ':') {
            const char *c2 = memchr(col + 3, ':', (size_t)(br - col - 3));
            if (!c2) return diag_fail(err_off, msg, lo, "expected ':' before point");
            cn = str_trim((Str){.p = col + 3, .n = (size_t)(c2 - col - 3)});
        }
        p.name = unq(p.name);
        Str in = {.p = br + 1, .n = (size_t)(ke - br - 1)};
        const char *cm = memchr(in.p, ',', in.n);
        if (!cm || !dg_parse_num((Str){.p = in.p, .n = (size_t)(cm - in.p)}, &p.x) ||
            !dg_parse_num((Str){.p = cm + 1, .n = in.n - (size_t)(cm + 1 - in.p)}, &p.y) || p.x < 0 || p.x > 1 ||
            p.y < 0 || p.y > 1)
            return diag_fail(err_off, msg, (size_t)(br - src), "point coordinates must be in [0, 1]");
        Str tail = str_trim((Str){.p = ke + 1, .n = line.n - (size_t)(ke + 1 - line.p)});
        if (!VEC_PUSH(a, &pcls, cn)) return CMM_ERR_NOMEM;
        p.st.r = -1, p.st.sw = -1;
        parse_style(tail, &p.st);
        if (!VEC_PUSH(a, &pts, p)) return CMM_ERR_NOMEM;
    }
    for (size_t i = 0; i < pts.n; i++) { /* class defaults under inline style */
        QStyle *s = &pts.v[i].st;
        for (size_t k = 0; pcls.v[i].n && k < cls.n; k++)
            if (str_eq(cls.v[k].name, pcls.v[i])) {
                if (s->r < 0) s->r = cls.v[k].st.r;
                if (s->sw < 0) s->sw = cls.v[k].st.sw;
                if (!s->fill.n) s->fill = cls.v[k].st.fill;
                if (!s->stroke.n) s->stroke = cls.v[k].st.stroke;
            }
        if (s->r <= 0) s->r = 5;
        if (s->sw < 0) s->sw = 0;
    }
    constexpr double X0 = 40, Y0 = 50, SZ = 420;
    bool yax = yl.n || yh.n, xax = xl.n || xh.n;
    double ox = X0 + (yax ? 30 : 0), oy = Y0 + (title.n ? 20 : 0);
    BBox bb = {0, 0, ox + SZ + 30, oy + SZ + (xax ? 40 : 10)};
    if (!buf_reserve(out, 4096 + pts.n * 400)) return CMM_ERR_NOMEM;
    DG_HEAD(out, bb, 4, "quadrantChart", CSS);
    if (title.n) dg_text(out, title, 20, ox + SZ / 2, 30, "middle", "ttl");
    double h = SZ / 2;
    /* quadrant-1 top-right, 2 top-left, 3 bottom-left, 4 bottom-right */
    const double qx[4] = {ox + h, ox, ox, ox + h}, qy[4] = {oy, oy, oy + h, oy + h};
    bool anypts = pts.n > 0;
    for (int i = 0; i < 4; i++) {
        char c[3] = {'q', (char)('1' + i), 0};
        dg_rect(out, qx[i], qy[i], h, h, 0, c);
        /* labels sit at the top of each quadrant when points exist, centred otherwise */
        if (q[i].n) dg_text(out, dg_wrap(a, q[i], 16, h - 10), 16, qx[i] + h / 2, anypts ? qy[i] + 20 : qy[i] + h / 2, "middle", "ql");
    }
    dg_line(out, ox + h, oy, ox + h, oy + SZ, "in");
    dg_line(out, ox, oy + h, ox + SZ, oy + h, "in");
    dg_rect(out, ox, oy, SZ, SZ, 0, "bd");
    if (xax) {
        if (xh.n) {
            dg_text(out, xl, 16, ox + h / 2, oy + SZ + 20, "middle", "al");
            dg_text(out, xh, 16, ox + h + h / 2, oy + SZ + 20, "middle", "al");
        } else dg_text(out, xl, 16, ox + h, oy + SZ + 20, "middle", "al");
    }
    if (yax) {
        double yc[2] = {oy + h + h / 2, oy + h / 2};
        Str ys[2] = {yl, yh};
        for (int i = 0; i < (yh.n ? 2 : 1); i++) {
            double cy = yh.n ? yc[i] : oy + h, cx = ox - 18;
            buf_lit(out, "<text class=\"al\" text-anchor=\"middle\" dominant-baseline=\"central\" transform=\"translate(");
            buf_num(out, cx); buf_ch(out, ','); buf_num(out, cy); buf_lit(out, ") rotate(-90)\">");
            buf_xml(out, ys[i]);
            buf_lit(out, "</text>");
        }
    }
    for (size_t i = 0; i < pts.n; i++) {
        const QPt *p = &pts.v[i];
        double cx = ox + p->x * SZ, cy = oy + (1 - p->y) * SZ;
        buf_lit(out, "<circle cx=\""); buf_num(out, cx); buf_lit(out, "\" cy=\""); buf_num(out, cy);
        buf_lit(out, "\" r=\""); buf_num(out, p->st.r); buf_lit(out, "\" fill=\"");
        if (safe_color(p->st.fill)) buf_str(out, p->st.fill); else buf_lit(out, "#0000eb");
        if (p->st.sw > 0) {
            buf_lit(out, "\" stroke=\"");
            if (safe_color(p->st.stroke)) buf_str(out, p->st.stroke); else buf_lit(out, "#000");
            buf_lit(out, "\" stroke-width=\""); buf_num(out, p->st.sw);
        }
        buf_lit(out, "\"/>");
        dg_text(out, p->name, 12, cx, cy + p->st.r + 10, "middle", "pl");
    }
    buf_lit(out, "</svg>\n");
    return out->oom ? CMM_ERR_NOMEM : CMM_OK;
}
