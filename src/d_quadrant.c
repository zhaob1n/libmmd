/* quadrantChart: title, x-axis/y-axis "low --> high", quadrant-1..4 labels, points
 * "Name: [x, y]" with optional "radius: r, color: #hex, stroke-color: .., stroke-width: Npx"
 * and ":::class" (classDef supports the same keys). */
#include <math.h>
#include "dgraph.h"
#include "dcfg.h"
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
    constexpr double X0 = 40;
    constexpr double Y0 = 50;
    const double SZ = fmin(fmax(dcfg_num("quadrantChart.chartWidth", 500), 100), 4000) - 80;
    const double SH = fmin(fmax(dcfg_num("quadrantChart.chartHeight", 500), 100), 4000) - 80;
    const double SW = SZ;
    bool yax = yl.n || yh.n, xax = xl.n || xh.n;
    double ox = X0 + (yax ? 30 : 0), oy = Y0 + (title.n ? 20 : 0);
    BBox bb = {0, 0, ox + SW + 30, oy + SH + (xax ? 40 : 10)};
    if (!buf_reserve(out, 4096 + pts.n * 400)) return CMM_ERR_NOMEM;
    DG_HEAD(out, bb, 4, "quadrantChart", CSS);
    if (title.n) dg_text(out, title, 20, ox + SW / 2, 30, "middle", "ttl");
    double h = SW / 2, v = SH / 2;
    /* quadrant-1 top-right, 2 top-left, 3 bottom-left, 4 bottom-right */
    const double qx[4] = {ox + h, ox, ox, ox + h}, qy[4] = {oy, oy, oy + v, oy + v};
    bool anypts = pts.n > 0;
    double *ob = ANEW(a, double, 4 * (pts.n * 2 + 8)); /* obstacle rects x0 y0 x1 y1 for point labels */
    if (!ob) return CMM_ERR_NOMEM;
    size_t no = 0;
    for (int i = 0; i < 4; i++) {
        char c[3] = {'q', (char)('1' + i), 0};
        dg_rect(out, qx[i], qy[i], h, v, 0, c);
        /* labels sit at the top of each quadrant when points exist, centred otherwise */
        if (q[i].n) {
            Str wq = dg_wrap(a, q[i], 16, h - 10);
            double lw, ly = anypts ? qy[i] + 20 : qy[i] + v / 2;
            unsigned ln;
            label_measure(wq, 16, &lw, &ln);
            dg_text(out, wq, 16, qx[i] + h / 2, ly, "middle", "ql");
            double *o = ob + 4 * no++;
            o[0] = qx[i] + h / 2 - lw / 2, o[1] = ly - 11 * ln, o[2] = qx[i] + h / 2 + lw / 2, o[3] = ly + 11 * ln;
        }
    }
    dg_line(out, ox + h, oy, ox + h, oy + SH, "in");
    dg_line(out, ox, oy + v, ox + SW, oy + v, "in");
    dg_rect(out, ox, oy, SW, SH, 0, "bd");
    if (xax) {
        if (xh.n) {
            dg_text(out, xl, 16, ox + h / 2, oy + SH + 20, "middle", "al");
            dg_text(out, xh, 16, ox + h + h / 2, oy + SH + 20, "middle", "al");
        } else dg_text(out, xl, 16, ox + h, oy + SH + 20, "middle", "al");
    }
    if (yax) {
        double yc[2] = {oy + v + v / 2, oy + v / 2};
        Str ys[2] = {yl, yh};
        for (int i = 0; i < (yh.n ? 2 : 1); i++) {
            double cy = yh.n ? yc[i] : oy + v, cx = ox - 18;
            buf_lit(out, "<text class=\"al\" text-anchor=\"middle\" dominant-baseline=\"central\" transform=\"translate(");
            buf_num(out, cx); buf_ch(out, ','); buf_num(out, cy); buf_lit(out, ") rotate(-90)\">");
            buf_xml(out, ys[i]);
            buf_lit(out, "</text>");
            double tw = text_width(ys[i], 16), *o = ob + 4 * no++; /* rotated: an obstacle for point labels */
            o[0] = cx - 10, o[1] = cy - tw / 2, o[2] = cx + 10, o[3] = cy + tw / 2;
        }
    }
    const size_t nl0 = no; /* boxes from here on are labels: text-on-text costs more */
    for (size_t i = 0; i < pts.n; i++) {
        const QPt *p = &pts.v[i];
        double cx = ox + p->x * SW, cy = oy + (1 - p->y) * SH;
        buf_lit(out, "<circle cx=\""); buf_num(out, cx); buf_lit(out, "\" cy=\""); buf_num(out, cy);
        buf_lit(out, "\" r=\""); buf_num(out, p->st.r); buf_lit(out, "\" fill=\"");
        if (safe_color(p->st.fill)) buf_str(out, p->st.fill); else buf_lit(out, "#0000eb");
        if (p->st.sw > 0) {
            buf_lit(out, "\" stroke=\"");
            if (safe_color(p->st.stroke)) buf_str(out, p->st.stroke); else buf_lit(out, "#000");
            buf_lit(out, "\" stroke-width=\""); buf_num(out, p->st.sw);
        }
        buf_lit(out, "\"/>");
        double *o = ob + 4 * no++;
        o[0] = cx - p->st.r, o[1] = cy - p->st.r, o[2] = cx + p->st.r, o[3] = cy + p->st.r;
    }
    double axy = oy + SH + 20; /* x-axis labels */
    if (xax) { double *o = ob + 4 * no++; o[0] = ox, o[1] = axy - 10, o[2] = ox + SW, o[3] = axy + 10; }
    /* deviate from mermaid: point labels try below/above/right/left and take the spot that collides least
       with quadrant titles, axis labels, points and earlier labels */
    for (size_t i = 0; i < pts.n; i++) {
        const QPt *p = &pts.v[i];
        double cx = ox + p->x * SW, cy = oy + (1 - p->y) * SH, r = p->st.r + p->st.sw / 2, w = text_width(p->name, 12);
        /* rings 0..13 (half-steps) of below/above/right/left/diagonals; farther rings get a leader line */
        double DX[112], DY[112];
        for (int k = 0; k < 14; k++) {
            double dy = r + 10 + 9 * k, dx = r + 4 + w / 2 + 6 * k, ex = w / 2 + r;
            const double X[8] = {0, 0, dx, -dx, ex, -ex, ex, -ex}, Y[8] = {dy, -dy, 0, 0, dy, dy, -dy, -dy};
            for (int c = 0; c < 8; c++) DX[8 * k + c] = X[c], DY[8 * k + c] = Y[c];
        }
        double best = HUGE_VAL, bx = cx, by = cy + r + 10;
        int bk = 0;
        for (int c = 0; c < 112; c++) {
            if (c >= 4 && c < 8 && best == 0) break; /* unchanged first choice when the plain spots are free */
            double lx = cx + DX[c], ly = cy + DY[c], x0 = lx - w / 2 - 2, x1 = lx + w / 2 + 2, y0 = ly - 8, y1 = ly + 8;
            double sc = (c % 8) * 2.0 + (c / 8) * 20.0 + (c % 8 >= 4 ? 10 : 0);
            if (c < 4) sc = c * 2.0;
            if (x0 < 0 || x1 > ox + SW + 30 || y0 < oy - 30 || y1 > oy + SH + 40) sc += 1e4;
            for (size_t k = 0; k < no; k++) {
                const double *o = ob + 4 * k;
                double ix = fmin(x1, o[2]) - fmax(x0, o[0]), iy = fmin(y1, o[3]) - fmax(y0, o[1]);
                if (ix > 0 && iy > 0) sc += ix * iy * (k >= nl0 ? 12 : 4);
            }
            if (sc < best) best = sc, bx = lx, by = ly, bk = c / 8;
        }
        double lt = hypot(bx - cx, by - cy);
        if (bk && lt > r + w / 2 + 14) { /* leader from the point's rim toward the label */
            double vx = bx - cx, vy = by - cy, l = hypot(vx, vy);
            buf_lit(out, "<path class=\"leader\" stroke=\"#888\" stroke-width=\"1\" d=\"M");
            dg_pt(out, cx + vx / l * (r + 1), cy + vy / l * (r + 1)); buf_ch(out, 'L');
            /* stop at the label box edge so the leader stays short */
            double tx = fabs(vx) > 1e-9 ? (w / 2 + 3) / fabs(vx) : HUGE_VAL, ty = fabs(vy) > 1e-9 ? 9 / fabs(vy) : HUGE_VAL;
            double t = fmax(0, 1 - fmin(tx, ty));
            dg_pt(out, cx + vx * t, cy + vy * t); buf_lit(out, "\"/>");
        }
        dg_text(out, p->name, 12, bx, by, "middle", "pl");
        double *o = ob + 4 * no++;
        o[0] = bx - w / 2 - 2, o[1] = by - 8, o[2] = bx + w / 2 + 2, o[3] = by + 8;
    }
    buf_lit(out, "</svg>\n");
    return out->oom ? CMM_ERR_NOMEM : CMM_OK;
}
