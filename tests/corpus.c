/* The CLI over the whole corpus, every file rendered once:
 *   tests/cases        the SVG equals tests/golden/NAME.svg; err* must fail and name line:col
 *   tests/docs         mermaid's own examples: what mermaid renders (tests/docs/mermaid_status.txt) renders
 *   tests/flow, bench/inputs, bench/mmdr_fixtures     render
 * and for all of them: no crash, no sanitizer report; the SVG is well-formed XML; every coordinate
 * it draws lies inside its viewBox; where there is no golden the output hashes to its line in
 * tests/sums, so no change of output goes unseen; and each CSS class selector that a <style> or the
 * themeVariables table can write matches an element in some output of its diagram type (a rule that
 * matches nothing is dead weight, or a theme variable without effect).
 * usage: test-corpus [mmd binary]        UPDATE_GOLDEN=1 rewrites tests/golden/ and tests/sums */
#include <math.h>
#include <unistd.h>

#include "dcfg.h"
#include "qa/qa.h"

static const char *bin = "build/release/mmd";

typedef enum expect : uint8_t { RENDERS, FAILS, EITHER } expect;
typedef struct file {
    char *path;
    bool is_case; /* compared with a golden, not with tests/sums */
    expect expect;
    char *fail;   /* why it failed; nullptr: passed */
    char *svg;    /* stdout */
    size_t len;
    const char *type;    /* class of the root element: the diagram type */
    qa_strs classes, css; /* class names on the elements below it, and the text of each <style> */
} file;

/* ---- bounds: shapes, path points with their control points, text anchors ---- */
constexpr double TOL = 1.0; /* user units */

typedef struct vec { double *v; size_t n, cap; } vec;
static void push(vec a[static 1], double x) {
    qa_room(a->v, a->n, a->cap);
    a->v[a->n++] = x;
}
static void push2(vec a[static 1], double x, double y) { push(a, x); push(a, y); }

static bool digit(char c) { return (unsigned)(c - '0') < 10; }

/* the number starting at s, as SVG writes them; returns its end, nullptr when there is none */
static const char *num_at(const char *s, const char *end, double v[static 1]) {
    const char *p = s;
    if (p < end && (*p == '-' || *p == '+')) p++;
    const char *d = p;
    while (p < end && digit(*p)) p++;
    if (p > d) {
        if (p < end && *p == '.')
            for (p++; p < end && digit(*p);) p++;
    } else if (p + 1 < end && *p == '.' && digit(p[1])) {
        for (p++; p < end && digit(*p);) p++;
    } else return nullptr;
    if (p < end && (*p == 'e' || *p == 'E')) {
        const char *q = p + 1;
        if (q < end && (*q == '-' || *q == '+')) q++;
        if (q < end && digit(*q)) {
            while (q < end && digit(*q)) q++;
            p = q;
        }
    }
    char buf[64];
    size_t n = (size_t)(p - s) < sizeof buf ? (size_t)(p - s) : sizeof buf - 1;
    memcpy(buf, s, n);
    buf[n] = 0;
    *v = strtod(buf, nullptr);
    return p;
}

static void nums(const char *s, const char *end, vec out[static 1]) {
    double v;
    for (const char *e; s < end; s = e ? e : s + 1)
        if ((e = num_at(s, end, &v))) push(out, v);
}

static double attr(const xml_node e[static 1], const char *name) {
    const char *v = xml_get(e, name);
    return v ? strtod(v, nullptr) : 0;
}

typedef struct mat { double a, b, c, d, e, f; } mat;
static mat mul(mat A, mat B) {
    return (mat){A.a * B.a + A.c * B.b, A.b * B.a + A.d * B.b, A.a * B.c + A.c * B.d,
                 A.b * B.c + A.d * B.d, A.a * B.e + A.c * B.f + A.e, A.b * B.e + A.d * B.f + A.f};
}

/* chains of translate, scale, matrix and rotate */
static mat transform(const char *t) {
    mat M = {1, 0, 0, 1, 0, 0};
    vec a = {};
    for (const char *p = t, *close; p && (p = strchr(p, '(')) && (close = strchr(p, ')')); p = close + 1) {
        const char *e = p, *b;
        while (e > t && e[-1] == ' ') e--;
        for (b = e; b > t && (unsigned)((b[-1] | 32) - 'a') < 26;) b--;
        a.n = 0;
        nums(p + 1, close, &a);
        size_t len = (size_t)(e - b);
#define IS(lit) (len == sizeof(lit) - 1 && memcmp(b, lit, len) == 0)
        if (IS("translate") && a.n >= 1) M = mul(M, (mat){1, 0, 0, 1, a.v[0], a.n > 1 ? a.v[1] : 0});
        else if (IS("scale") && a.n >= 1) M = mul(M, (mat){a.v[0], 0, 0, a.n > 1 ? a.v[1] : a.v[0], 0, 0});
        else if (IS("matrix") && a.n >= 6) M = mul(M, (mat){a.v[0], a.v[1], a.v[2], a.v[3], a.v[4], a.v[5]});
        else if (IS("rotate") && a.n >= 1) {
            double r = a.v[0] * M_PI / 180, c = cos(r), s = sin(r);
            double cx = a.n > 2 ? a.v[1] : 0, cy = a.n > 2 ? a.v[2] : 0;
            M = mul(M, (mat){c, s, -s, c, cx - c * cx + s * cy, cy - s * cx - c * cy});
        }
#undef IS
    }
    free(a.v);
    return M;
}

static double angle(double ux, double uy, double vx, double vy) { return atan2(ux * vy - uy * vx, ux * vx + uy * vy); }

/* 33 points along an elliptical arc (SVG 1.1, F.6.5) */
static void arc(vec out[static 1], double x1, double y1, double rx, double ry, double phi, bool large, bool sweep, double x2, double y2) {
    rx = fabs(rx);
    ry = fabs(ry);
    if (rx < 1e-9 || ry < 1e-9) return push2(out, x2, y2);
    double c = cos(phi * M_PI / 180), s = sin(phi * M_PI / 180);
    double dx = (x1 - x2) / 2, dy = (y1 - y2) / 2;
    double x1p = c * dx + s * dy, y1p = -s * dx + c * dy;
    double lam = x1p * x1p / (rx * rx) + y1p * y1p / (ry * ry);
    if (lam > 1) {
        rx *= sqrt(lam);
        ry *= sqrt(lam);
    }
    double den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
    double co = sqrt(fmax(0, (rx * rx * ry * ry - den) / (den != 0 ? den : 1e-12))) * (large == sweep ? -1 : 1);
    double cxp = co * rx * y1p / ry, cyp = -co * ry * x1p / rx;
    double cx = c * cxp - s * cyp + (x1 + x2) / 2, cy = s * cxp + c * cyp + (y1 + y2) / 2;
    double t1 = angle(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry);
    double dt = angle((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry);
    if (!sweep && dt > 0) dt -= 2 * M_PI;
    else if (sweep && dt < 0) dt += 2 * M_PI;
    for (int k = 0; k <= 32; k++) {
        double t = t1 + dt * k / 32, ex = rx * cos(t), ey = ry * sin(t);
        push2(out, c * ex - s * ey + cx, s * ex + c * ey + cy);
    }
}

static void path_points(const char *d, vec out[static 1]) {
    typedef struct tok { char cmd; double v; } tok; /* cmd 0: a number */
    tok *toks = nullptr;
    size_t nt = 0, cap = 0;
    for (const char *end = d + strlen(d), *e; d < end; d = e ? e : d + 1) {
        tok t = {};
        e = strchr("MmLlHhVvCcSsQqTtAaZz", *d) ? (t.cmd = *d, d + 1) : num_at(d, end, &t.v);
        if (!e) continue;
        qa_room(toks, nt, cap);
        toks[nt++] = t;
    }
    double x = 0, y = 0, sx = 0, sy = 0;
    char cmd = 0;
    for (size_t i = 0; i < nt;) {
        if (toks[i].cmd) cmd = toks[i++].cmd;
        if (cmd == 'Z' || cmd == 'z') {
            x = sx;
            y = sy;
            cmd = 0;
            continue;
        }
        if (!cmd) {
            i++;
            continue;
        }
        char U = cmd & ~32;
        bool rel = cmd & 32;
        size_t n = U == 'H' || U == 'V' ? 1 : U == 'C' ? 6 : U == 'S' || U == 'Q' ? 4 : U == 'A' ? 7 : 2, k = 0;
        double a[7];
        while (k < n && i + k < nt && !toks[i + k].cmd) {
            a[k] = toks[i + k].v;
            k++;
        }
        i += k;
        if (k < n) { /* a command cut short: the rest of the path up to the next one is skipped */
            cmd = 0;
            continue;
        }
        double ox = rel ? x : 0, oy = rel ? y : 0;
        if (U == 'H') x = a[0] + ox;
        else if (U == 'V') y = a[0] + oy;
        else if (U == 'A') {
            arc(out, x, y, a[0], a[1], a[2], a[3] != 0, a[4] != 0, a[5] + ox, a[6] + oy);
            x = a[5] + ox;
            y = a[6] + oy;
        } else {
            for (k = 0; k < n; k += 2) push2(out, x = a[k] + ox, y = a[k + 1] + oy);
            out->n -= 2; /* the end point follows */
        }
        if (U == 'M') {
            sx = x;
            sy = y;
            cmd = rel ? 'l' : 'L';
        }
        push2(out, x, y);
    }
    free(toks);
}

typedef struct bounds {
    double x0, y0, x1, y1;
    vec pts;
    size_t bad;
    char first[160];
} bounds;

static void walk(bounds b[static 1], const xml_node e[static 1], mat M) {
    static const char *const skip[] = {"defs", "marker", "style", "title", "desc", "clipPath", "mask",
                                       "pattern", "symbol", "linearGradient", "radialGradient", "filter"};
    for (size_t i = 0; i < countof(skip); i++)
        if (xml_is(e, skip[i])) return;
    const char *v = xml_get(e, "transform");
    if (v) M = mul(M, transform(v));
    vec *p = &b->pts;
    p->n = 0;
    if (xml_is(e, "rect")) {
        double x = attr(e, "x"), y = attr(e, "y");
        push2(p, x, y);
        push2(p, x + attr(e, "width"), y + attr(e, "height"));
    } else if (xml_is(e, "circle") || xml_is(e, "ellipse")) {
        double cx = attr(e, "cx"), cy = attr(e, "cy");
        double rx = attr(e, e->name[0] == 'c' ? "r" : "rx"), ry = attr(e, e->name[0] == 'c' ? "r" : "ry");
        push2(p, cx - rx, cy - ry);
        push2(p, cx + rx, cy + ry);
    } else if (xml_is(e, "line")) {
        push2(p, attr(e, "x1"), attr(e, "y1"));
        push2(p, attr(e, "x2"), attr(e, "y2"));
    } else if ((xml_is(e, "polygon") || xml_is(e, "polyline")) && (v = xml_get(e, "points"))) {
        nums(v, v + strlen(v), p);
        p->n &= ~(size_t)1;
    } else if (xml_is(e, "path") && (v = xml_get(e, "d"))) {
        path_points(v, p);
    } else if (xml_is(e, "text") || xml_is(e, "tspan")) { /* one anchor; a list of x positions is per glyph */
        const char *x = xml_get(e, "x"), *y = xml_get(e, "y");
        vec xs = {}, ys = {};
        if (x && y) {
            nums(x, x + strlen(x), &xs);
            nums(y, y + strlen(y), &ys);
        }
        if (xs.n == 1 && ys.n) push2(p, xs.v[0], ys.v[0]);
        free(xs.v);
        free(ys.v);
    }
    for (size_t i = 0; i < p->n; i += 2) {
        double x = M.a * p->v[i] + M.c * p->v[i + 1] + M.e, y = M.b * p->v[i] + M.d * p->v[i + 1] + M.f;
        if (x >= b->x0 && x <= b->x1 && y >= b->y0 && y <= b->y1) continue;
        if (!b->bad++) snprintf(b->first, sizeof b->first, "%s (%.2f,%.2f)", e->name, x, y);
        return;
    }
    if (xml_is(e, "foreignObject")) return;
    for (const xml_node *c = e->child; c; c = c->next)
        if (c->name) walk(b, c, M);
}

/* nullptr, or what is drawn outside the viewBox */
static char *out_of_bounds(const xml_node root[static 1]) {
    const char *vb = xml_get(root, "viewBox");
    vec v = {};
    if (vb) nums(vb, vb + strlen(vb), &v);
    char *why = nullptr;
    if (v.n == 4) {
        bounds b = {.x0 = v.v[0] - TOL, .y0 = v.v[1] - TOL, .x1 = v.v[0] + v.v[2] + TOL, .y1 = v.v[1] + v.v[3] + TOL};
        walk(&b, root, (mat){1, 0, 0, 1, 0, 0});
        if (b.bad) why = qa_fmt("%zu shape(s) outside viewBox %s; first: %s", b.bad, vb, b.first);
        free(b.pts.v);
    }
    free(v.v);
    return why;
}

/* ---- one file ---- */
static bool has(const qa_strs s[static 1], const char *x) {
    for (size_t i = 0; i < s->n; i++)
        if (strcmp(s->v[i], x) == 0) return true;
    return false;
}
/* x unless the set has it; the set owns what it keeps, and what it keeps stays where it is */
static void add(qa_strs s[static 1], const char *x) {
    if (!has(s, x)) qa_push(s, qa_fmt("%s", x));
}

/* ":LINE:COL:" */
static bool line_col(const char *s) {
    for (; (s = strchr(s, ':')); s++) {
        const char *p = s + 1;
        for (int k = 0; k < 2; k++) {
            if (!digit(*p)) goto next;
            while (digit(*p)) p++;
            if (*p++ != ':') goto next;
        }
        return true;
    next:;
    }
    return false;
}

static bool sanitizer(const char *err) {
    return strstr(err, "Sanitizer") || strstr(err, "runtime error:");
}

static void inspect(file f[static 1]) {
    char err[128];
    xml_doc *d = xml_parse(f->len, f->svg, err);
    if (!d) {
        f->fail = qa_fmt("not well-formed XML: %s", err);
        return;
    }
    f->fail = out_of_bounds(d->root);
    f->type = xml_get(d->root, "class");
    if (f->type) f->type = qa_fmt("%s", f->type);
    for (const xml_node *n = xml_next(d->root, d->root); f->type && n; n = xml_next(d->root, n)) {
        if (xml_is(n, "style")) {
            qa_buf css = {};
            xml_text(n, "", &css);
            if (css.n) add(&f->css, css.p);
            free(css.p);
        }
        const char *cls = n->name ? xml_get(n, "class") : nullptr;
        for (size_t k; cls && *cls; cls += k) {
            cls += strspn(cls, " ");
            if ((k = strcspn(cls, " "))) {
                char *one = qa_fmt("%.*s", (int)k, cls);
                add(&f->classes, one);
                free(one);
            }
        }
    }
    xml_free(d);
}

static void render(size_t i, void *ctx) {
    file *f = &((file *)ctx)[i];
    qa_res r = qa_run(&(qa_cmd){.argv = QA_ARGV(bin, f->path), .timeout = 300});
    f->svg = r.out;
    f->len = r.out_len;
    if (r.code < 0 || r.code > 1 || sanitizer(r.err)) f->fail = qa_fmt("crashed (status %d): %.400s", r.code, r.err);
    else if (r.code == 0 && f->expect == FAILS) f->fail = qa_fmt("rendered; an error was expected");
    else if (r.code == 1 && f->expect == FAILS && !line_col(r.err)) f->fail = qa_fmt("error without line:col: %s", r.err);
    else if (r.code == 1 && f->expect == RENDERS) f->fail = qa_fmt("%.*s", (int)strcspn(r.err, "\n"), r.err);
    else if (r.code == 0) inspect(f);
    free(r.err);
}

/* ---- CSS ---- */
typedef struct type { const char *name; qa_strs classes, css; } type;
static type *types;
static size_t ntypes, types_cap;
static qa_strs dead;

static type *type_of(const char *name, bool create) {
    for (size_t i = 0; i < ntypes; i++)
        if (strcmp(types[i].name, name) == 0) return &types[i];
    if (!create) return nullptr;
    qa_room(types, ntypes, types_cap);
    types[ntypes] = (type){.name = name};
    return &types[ntypes++];
}

static bool word(char c) { return (unsigned char)c >= 0x80 || c == '_' || c == '-' || digit(c) || (unsigned)((c | 32) - 'a') < 26; }

/* every "#mermaid ..." selector of every rule of css needs each of its classes on some element of the
 * type; "*" stands for all types */
static void audit(const char *css, const char *type_name, const char *where) {
    for (const char *p = css, *open; *(open = p + strcspn(p, "{}")); p = open + 1) {
        if (*open == '}') continue;
        for (const char *s = p, *e; s < open; s = e + 1) {
            if (!(e = memchr(s, ',', (size_t)(open - s)))) e = open;
            s += strspn(s, " \t\n\r");
            if (strncmp(s, "#mermaid", 8) != 0) continue;
            int len = (int)(e - s);
            while (len && strchr(" \t\n\r", s[len - 1])) len--;
            qa_buf miss = {};
            for (const char *c = s; (c = memchr(c, '.', (size_t)(s + len - c))); c++) {
                size_t k = 1;
                while (c + k < s + len && word(c[k])) k++;
                if (k == 1) continue;
                char *cls = qa_fmt("%.*s", (int)k - 1, c + 1);
                bool found = false;
                for (size_t i = 0; i < ntypes; i++)
                    found |= (*type_name == '*' || strcmp(types[i].name, type_name) == 0) && has(&types[i].classes, cls);
                if (!found) qa_putf(&miss, "%s.%s", miss.n ? ", " : "", cls);
                free(cls);
            }
            if (miss.n) {
                char *why = qa_fmt("%-7s %-18s %-50.*s no element has %s", where, type_name, len, s, miss.p);
                add(&dead, why);
                free(why);
            }
            free(miss.p);
        }
        if (!(open = strchr(open, '}'))) break;
    }
}

static int by_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static size_t audit_css(size_t n, const file files[static n]) {
    size_t nclasses = 0;
    for (size_t i = 0; i < n; i++) {
        if (!files[i].type) continue;
        type *t = type_of(files[i].type, true);
        for (size_t k = 0; k < files[i].classes.n; k++) add(&t->classes, files[i].classes.v[k]);
        for (size_t k = 0; k < files[i].css.n; k++) add(&t->css, files[i].css.v[k]);
    }
    for (size_t i = 0; i < DCFG_NTVS; i++) {
        const DThemeRule *r = &DCFG_TVS[i];
        if (*r->kind != '*' && !type_of(r->kind, false)) {
            char *why = qa_fmt("%-7s %-18s %s: no output of this type", "theme", r->kind, r->var);
            add(&dead, why);
            free(why);
            continue;
        }
        char *tpl = qa_fmt("%s", r->tpl);
        for (char *p = tpl; (p = strchr(p, '$'));) *p = 'x';
        audit(tpl, r->kind, "theme");
        free(tpl);
    }
    for (size_t i = 0; i < ntypes; i++) {
        nclasses += types[i].classes.n;
        for (size_t k = 0; k < types[i].css.n; k++) audit(types[i].css.v[k], types[i].name, "style");
    }
    if (dead.n) qsort(dead.v, dead.n, sizeof *dead.v, by_str);
    for (size_t i = 0; i < dead.n; i++) printf("DEAD %s\n", dead.v[i]);
    return nclasses;
}

/* ---- goldens and sums ---- */
static bool update;

static void fail(file f[static 1], char *why) {
    if (f->fail) free(why);
    else f->fail = why;
}

static void golden(file f[static 1]) {
    const char *base = strrchr(f->path, '/') + 1;
    char *name = qa_fmt("%.*s", (int)(strlen(base) - 4), base), *path = qa_fmt("tests/golden/%s.svg", name);
    size_t len = 0;
    char *want = qa_try_read(path, &len);
    bool same = want && len == f->len && memcmp(want, f->svg, len) == 0;
    if (update && !same) qa_write(path, f->len, f->svg);
    else if (!want) fail(f, qa_fmt("no %s", path));
    else if (!same) {
        char *got = qa_fmt("tests/out/%s.svg", name);
        qa_mkdirs("tests/out");
        qa_write(got, f->len, f->svg);
        fail(f, qa_fmt("differs from %s: see %s", path, got));
    }
    free(want);
}

static int by_path(const void *a, const void *b) { return strcmp((*(file *const *)a)->path, (*(file *const *)b)->path); }

/* tests/sums: "HASH PATH" for every file that has no golden, sorted by path */
static size_t sums(size_t n, file files[static n]) {
    file **v = qa_alloc(n * sizeof *v);
    size_t nv = 0, len, stale = 0;
    for (size_t i = 0; i < n; i++)
        if (!files[i].is_case) v[nv++] = &files[i];
    qsort(v, nv, sizeof *v, by_path);
    qa_buf now = {};
    for (size_t i = 0; i < nv; i++) qa_putf(&now, "%016llx %s\n", (unsigned long long)qa_hash(v[i]->len, v[i]->svg), v[i]->path);
    if (update) {
        qa_write("tests/sums", now.n, now.p);
        return 0;
    }
    char *old = qa_try_read("tests/sums", &len);
    if (!old) qa_die("tests/sums: %m (UPDATE_GOLDEN=1 writes it)");
    /* both sorted by path: merge */
    const char *a = old, *b = now.p;
    for (size_t i = 0; *a || i < nv;) {
        size_t la = strcspn(a, "\n"), lb = strcspn(b, "\n");
        int c = !*a ? 1 : i == nv ? -1 : strncmp(a + 17, v[i]->path, la > 17 ? la - 17 : 0);
        if (c == 0 && la - 17 != strlen(v[i]->path)) c = la - 17 < strlen(v[i]->path) ? -1 : 1;
        if (c < 0) {
            if (!stale++) printf("FAIL tests/sums: %.*s is no longer in the corpus\n", (int)(la > 17 ? la - 17 : 0), a + 17);
            a += la + (a[la] == '\n');
            continue;
        }
        if (c > 0) fail(v[i], qa_fmt("not in tests/sums"));
        else if (memcmp(a, b, 16) != 0) fail(v[i], qa_fmt("output changed (tests/sums)"));
        if (c == 0) a += la + (a[la] == '\n');
        b += lb + 1;
        i++;
    }
    return stale;
}

int main(int argc, char *argv[]) {
    qa_at_root(argc, argv);
    if (argc > 1) bin = argv[1];
    update = getenv("UPDATE_GOLDEN") != nullptr;
    setenv("MMD_TODAY", "2014-01-08", 1); /* inside the corpus' gantt charts: the today marker is drawn */

    qa_strs paths = {};
    qa_glob(&paths, "tests/cases/*.mmd");
    size_t ncases = paths.n, len;
    qa_glob(&paths, "tests/docs/*/*.mmd");
    size_t ndocs = paths.n - ncases, docs_ok = 0, docs_want = 0;
    qa_glob(&paths, "tests/flow/*.mmd");
    qa_glob(&paths, "bench/inputs/*.mmd");
    qa_glob(&paths, "bench/mmdr_fixtures/*.mmd");
    char *status = qa_read("tests/docs/mermaid_status.txt", &len);
    file *files = qa_alloc(paths.n * sizeof *files);
    for (size_t i = 0; i < paths.n; i++) {
        file *f = &files[i];
        *f = (file){.path = paths.v[i], .is_case = i < ncases};
        if (f->is_case) f->expect = strncmp(f->path, "tests/cases/err", 15) == 0 ? FAILS : RENDERS;
        else if (i < ncases + ndocs) {
            char *line = qa_fmt("%s ok\n", f->path);
            f->expect = strstr(status, line) ? RENDERS : EITHER;
            docs_want += f->expect == RENDERS;
            free(line);
        }
    }
    qa_parallel(paths.n, render, files);

    size_t ngolden = 0, failed = 0;
    for (size_t i = 0; i < ncases; i++) {
        if (files[i].expect != RENDERS || files[i].fail) continue;
        golden(&files[i]);
        ngolden++;
    }
    for (size_t i = ncases; i < ncases + ndocs; i++) docs_ok += files[i].expect == RENDERS && !files[i].fail;
    qa_strs goldens = {};
    qa_glob(&goldens, "tests/golden/*.svg");
    for (size_t i = 0; i < goldens.n; i++) { /* a golden whose case is gone */
        char *src = qa_fmt("tests/cases/%.*s.mmd", (int)(strlen(goldens.v[i]) - 17), goldens.v[i] + 13);
        if (access(src, F_OK) != 0) failed += printf("FAIL %s: no %s\n", goldens.v[i], src) > 0;
        free(src);
    }
    failed += sums(paths.n, files);
    size_t nclasses = audit_css(paths.n, files);

    /* the CLI's other paths: standard input, -o */
    static constexpr char piped[] = "graph LR; a-->b\n";
    char err[128], *out = qa_fmt("%s/o.svg", qa_scratch("corpus"));
    qa_res r = qa_run(&(qa_cmd){.argv = QA_ARGV(bin), .in = piped, .in_len = sizeof piped - 1, .timeout = 60});
    xml_doc *d = r.code == 0 ? xml_parse(r.out_len, r.out, err) : nullptr;
    if (!d) failed += printf("FAIL stdin: %s\n", r.code ? r.err : err) > 0;
    r = qa_run(&(qa_cmd){.argv = QA_ARGV(bin, files[0].path, "-o", out), .timeout = 60});
    char *written = r.code == 0 ? qa_try_read(out, &len) : nullptr;
    if (!written || len != files[0].len || memcmp(written, files[0].svg, len) != 0)
        failed += printf("FAIL -o: %s is not what %s prints\n", out, files[0].path) > 0;

    size_t shown = 0;
    for (size_t i = 0; i < paths.n; i++) {
        if (!files[i].fail) continue;
        if (shown++ < 40) printf("FAIL %s: %s\n", files[i].path, files[i].fail);
    }
    if (shown > 40) printf("... and %zu more\n", shown - 40);
    failed += shown + dead.n;
    printf("corpus: %zu files, %zu goldens, %zu/%zu docs examples that mermaid renders, %zu CSS classes; %zu failed\n",
           paths.n, ngolden, docs_ok, docs_want, nclasses, failed);
    return failed != 0;
}
