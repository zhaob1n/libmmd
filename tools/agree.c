/* agree: how closely libmmd's layouts agree with mermaid.js's.
 *
 *   agree flow [--mmd BIN] [FILE...]   node order of flowcharts (default bench/mmdr_fixtures/flowchart*.mmd)
 *   agree state [--mmd BIN] [FILE...]  node order of state diagrams (default the .mmd files of
 *                                      tests/docs/stateDiagram, then tests/cases/<*state*>.mmd)
 *   agree size [--mmd BIN] [N]         the N (default 40) tests/docs examples whose aspect ratio differs most
 *
 * Two nodes agree on an axis when both renderers put them in the same order along it, with 1px slack.
 * flow matches nodes by id; it prints, per file, the agreeing pairs across the rank axis and along it,
 * and the node-centre error after aligning the centroids, then the totals. state matches states, notes
 * (NOTE:) and composites (C:) by label; a label used twice is left out. size ranks by
 * |log(aspect_libmmd / aspect_mermaid)| of the root viewBoxes; a libmmd failure counts as 99.
 * mermaid's SVGs come from mermaid.js with its classic look and dagre layout (qa_mermaid, QA_CLASSIC),
 * cached in $TMPDIR/mmd-UID/mermaid-classic. BIN is the renderer, default build/mmd. */
#include "qa/qa.h"

#include <ctype.h>
#include <math.h>

/* a Python dict of points: keys keep the place of their first insertion */
typedef struct pt { char *key; double x, y; bool dup; } pt;
typedef struct pts { pt *v; size_t n, cap; } pts;

static pt *find(const pts d[static 1], const char *key) {
    for (size_t i = 0; i < d->n; i++)
        if (!strcmp(d->v[i].key, key)) return &d->v[i];
    return nullptr;
}

/* d[key] = (x, y) */
static void set(pts d[static 1], char *key, double x, double y) {
    pt *p = find(d, key);
    if (!p) {
        qa_room(d->v, d->n, d->cap);
        p = &d->v[d->n++];
        *p = (pt){.key = key};
    }
    p->x = x, p->y = y;
}

/* the same, but a key seen twice is ambiguous: it is dropped */
static void put(pts d[static 1], char *key, double x, double y) {
    pt *p = find(d, key);
    if (p) p->dup = true;
    else set(d, key, x, y);
}

static int sg(double a, double b) { return (a > b + 1) - (a < b - 1); }

/* sum() of floats as CPython (3.12 on) adds them: Neumaier's compensated summation */
static double pysum(size_t n, const double x[static n]) {
    double s = 0, c = 0;
    for (size_t i = 0; i < n; i++) {
        double t = s + x[i];
        c += fabs(s) >= fabs(x[i]) ? (s - t) + x[i] : (x[i] - t) + s;
        s = t;
    }
    return c && isfinite(c) ? s + c : s;
}

static const char *mmd = "build/mmd";

static qa_res ours(const char *src) {
    return qa_run(&(qa_cmd){.argv = QA_ARGV(mmd, src), .timeout = 120});
}

static qa_render *mermaid(const qa_strs files[static 1]) {
    qa_render *m = qa_alloc(files->n * sizeof *m);
    if (!qa_mermaid(qa_scratch("mermaid-classic"), QA_CLASSIC, files->n, (const char *const *)files->v, m))
        qa_die("mermaid-cli is not installed");
    return m;
}

/* a span of [-0-9.e] (with e: also exponents), as strtod reads it; false when empty */
static bool num(const char **p, const char *set, double v[static 1]) {
    size_t n = strspn(*p, set);
    if (!n) return false;
    char *s = qa_fmt("%.*s", (int)n, *p);
    *v = strtod(s, nullptr);
    free(s);
    *p += n;
    return true;
}

/* ---- flow ---- */

/* <g class="node…" id="(my-svg-)flowchart-ID-N" … transform="translate(X, Y)"> */
static void flow_pos(const char *s, pts d[static 1]) {
    for (const char *p = s; (p = strstr(p, "<g class=\"node")); p++) {
        const char *q = p + 14;
        q += strcspn(q, "\"");
        if (strncmp(q, "\" id=\"", 6)) continue;
        q += 6;
        if (!strncmp(q, "my-svg-", 7)) q += 7;
        if (strncmp(q, "flowchart-", 10)) continue;
        q += 10;
        const char *e = strchr(q, '"'), *t = e;
        while (t > q && isdigit((unsigned char)t[-1])) t--;
        if (t == e || t - 1 <= q || t[-1] != '-') continue;
        const char *gt = strchr(e, '>'), *tr = strstr(e, "transform=\"translate(");
        if (!gt || !tr || tr > gt) continue;
        double x, y;
        tr += 21;
        if (!num(&tr, "-0123456789.e", &x) || *tr++ != ',') continue;
        if (*tr == ' ') tr++;
        if (!num(&tr, "-0123456789.e", &y) || *tr != ')') continue;
        set(d, qa_fmt("%.*s", (int)(t - 1 - q), q), x, y);
    }
}

/* ^\s*(flowchart|graph)\s+(LR|RL) on any line */
static bool left_right(const char *s) {
    for (const char *l = s; l; l = strchr(l, '\n'), l = l ? l + 1 : nullptr) {
        const char *p = l + strspn(l, " \t\n\r\f\v");
        if (!strncmp(p, "flowchart", 9)) p += 9;
        else if (!strncmp(p, "graph", 5)) p += 5;
        else continue;
        size_t n = strspn(p, " \t\n\r\f\v");
        if (n && (!strncmp(p + n, "LR", 2) || !strncmp(p + n, "RL", 2))) return true;
    }
    return false;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void flow(qa_strs files[static 1]) {
    if (!files->n) qa_glob(files, "bench/mmdr_fixtures/flowchart*.mmd");
    qa_render *m = mermaid(files);
    size_t tot[3] = {}, ne = 0, cap = 0;
    double *all = nullptr;
    for (size_t f = 0; f < files->n; f++) {
        const char *src = files->v[f], *base = strrchr(src, '/') ? strrchr(src, '/') + 1 : src;
        int name = (int)strlen(base) - 4;
        if (!m[f].svg) {
            fprintf(stderr, "%.*s: mermaid failed: %s", name, base, m[f].err);
            continue;
        }
        size_t len;
        pts ref = {}, me = {};
        flow_pos(qa_read(m[f].svg, &len), &ref);
        qa_res r = ours(src);
        flow_pos(r.out, &me);
        int X = left_right(qa_read(src, &len));
        size_t n = 0, ok = 0, okr = 0, t = 0;
        pt **k = qa_alloc((ref.n + 1) * sizeof *k), **o = qa_alloc((ref.n + 1) * sizeof *o);
        for (size_t i = 0; i < ref.n; i++)
            if ((o[n] = find(&me, ref.v[i].key))) k[n++] = &ref.v[i];
        for (size_t i = 0; i < n; i++)
            for (size_t j = i + 1; j < n; j++) {
                double mi[2] = {o[i]->x, o[i]->y}, mj[2] = {o[j]->x, o[j]->y};
                double ri[2] = {k[i]->x, k[i]->y}, rj[2] = {k[j]->x, k[j]->y};
                t++;
                ok += sg(mi[X], mj[X]) == sg(ri[X], rj[X]);
                okr += sg(mi[1 - X], mj[1 - X]) == sg(ri[1 - X], rj[1 - X]);
            }
        tot[0] += ok, tot[1] += okr, tot[2] += t;
        double err = 0, ex = 0, ey = 0;
        if (n) {
            double *v = qa_alloc(n * sizeof *v), *ee = qa_alloc(n * sizeof *ee);
            for (size_t i = 0; i < n; i++) v[i] = o[i]->x - k[i]->x;
            double ox = pysum(n, v) / (double)n;
            for (size_t i = 0; i < n; i++) v[i] = o[i]->y - k[i]->y;
            double oy = pysum(n, v) / (double)n;
            for (size_t i = 0; i < n; i++) {
                double dx = o[i]->x - ox - k[i]->x, dy = o[i]->y - oy - k[i]->y;
                ee[i] = pow(dx * dx + dy * dy, .5);
                qa_room(all, ne, cap);
                all[ne++] = ee[i];
            }
            err = pysum(n, ee) / (double)n;
            for (size_t i = 0; i < n; i++) v[i] = fabs(o[i]->x - ox - k[i]->x);
            ex = pysum(n, v) / (double)n;
            for (size_t i = 0; i < n; i++) v[i] = fabs(o[i]->y - oy - k[i]->y);
            ey = pysum(n, v) / (double)n;
        }
        printf("%-45.*s cross %zu/%zu  rankaxis %zu/%zu  err %.1f ex %.1f ey %.1f n %zu\n", name, base, ok, t,
               okr, t, err, ex, ey, n);
    }
    printf("TOTAL cross %.4f rankaxis %.4f pairs %zu\n", (double)tot[0] / (double)tot[2],
           (double)tot[1] / (double)tot[2], tot[2]);
    if (!ne) return;
    qsort(all, ne, sizeof *all, cmp_double);
    printf("RAW node-centre error (centroid-aligned, px): mean %.2f median %.2f p90 %.2f max %.2f n %zu\n",
           pysum(ne, all) / (double)ne, all[ne / 2], all[(size_t)((double)ne * .9)], all[ne - 1], ne);
}

/* ---- state ---- */

static char *text(const xml_node e[static 1]) {
    qa_buf b = {};
    qa_put(&b, 0, "");
    xml_text(e, " ", &b);
    return b.p;
}

static bool ends_with(const char *s, const char *end) {
    size_t n = strlen(s), k = strlen(end);
    return n >= k && !strcmp(s + n - k, end);
}

/* the label in my-svg-state-LABEL-N */
static char *state_id(const char *id) {
    if (!id || strncmp(id, "my-svg-state-", 13)) return nullptr;
    const char *q = id + 13, *e = q + strlen(q), *t = e;
    while (t > q && isdigit((unsigned char)t[-1])) t--;
    if (t == e || t - 1 <= q || t[-1] != '-') return nullptr;
    return qa_fmt("%.*s", (int)(t - 1 - q), q);
}

/* the first translate(X Y) of a transform */
static void translate(const char *tf, double o[static 2]) {
    for (const char *p = tf; p && (p = strstr(p, "translate(")); p++) {
        const char *q = p + 10;
        double x, y;
        q += strspn(q, " \t\n\r\f\v");
        if (!num(&q, "-0123456789.e", &x)) continue;
        size_t n = strspn(q, " ,");
        q += n;
        if (!n || !num(&q, "-0123456789.e", &y)) continue;
        o[0] += x, o[1] += y;
        return;
    }
}

static const xml_node *child(const xml_node e[static 1], const char *name, const char *cls) {
    for (const xml_node *c = e->child; c; c = c->next) {
        const char *v = xml_is(c, name) && cls ? xml_get(c, "class") : nullptr;
        if (xml_is(c, name) && (!cls || (v && !strcmp(v, cls)))) return c;
    }
    return nullptr;
}

static void mermaid_walk(const xml_node e[static 1], double ox, double oy, pts d[static 1]) {
    double o[2] = {ox, oy};
    translate(xml_get(e, "transform"), o);
    if (xml_is(e, "g") && xml_has_class(e, "node")) {
        char *id = state_id(xml_get(e, "id"));
        if (!id || ends_with(id, "_start") || ends_with(id, "_end")) return;
        if (xml_has_class(e, "statediagram-note")) {
            put(d, qa_fmt("NOTE:%s", text(e)), o[0], o[1]);
            return;
        }
        char *label = nullptr;
        for (const xml_node *n = e; n && !label; n = xml_next(e, n))
            if (n != e && xml_is(n, "p") && *(label = text(n)) == 0) label = nullptr;
        if (!label && *(label = text(e)) == 0) label = id;
        put(d, label, o[0], o[1]);
        return;
    }
    const char *data_id = xml_get(e, "data-id");
    if (xml_is(e, "g") && xml_has_class(e, "statediagram-cluster") && !xml_has_class(e, "statediagram-cluster-alt") &&
        data_id && *data_id) {
        const xml_node *r = nullptr;
        for (const xml_node *n = xml_next(e, e); n && !r; n = xml_next(e, n)) {
            const char *cls = xml_is(n, "rect") ? xml_get(n, "class") : nullptr;
            if (cls && !strcmp(cls, "outer")) r = n;
        }
        if (!r) r = child(e, "rect", nullptr);
        const xml_node *t = child(e, "g", "cluster-label");
        if (r)
            put(d, qa_fmt("C:%s", t ? text(t) : data_id),
                o[0] + strtod(xml_get(r, "x"), nullptr) + strtod(xml_get(r, "width"), nullptr) / 2,
                o[1] + strtod(xml_get(r, "y"), nullptr) + strtod(xml_get(r, "height"), nullptr) / 2);
    }
    for (const xml_node *c = e->child; c; c = c->next)
        if (c->name) mermaid_walk(c, o[0], o[1], d);
}

/* ` NAME="NUM"` with NUM of [-0-9.] */
static bool attr(const char **p, const char *name, double v[static 1]) {
    size_t n = strlen(name);
    if (strncmp(*p, name, n)) return false;
    *p += n;
    return num(p, "-0123456789.", v) && *(*p)++ == '"';
}

static bool rect(const char **p, double r[static 4]) {
    return attr(p, " x=\"", &r[0]) && attr(p, " y=\"", &r[1]) && attr(p, " width=\"", &r[2]) &&
           attr(p, " height=\"", &r[3]);
}

/* up to the next '>', which closes an empty element */
static bool tag_end(const char **p) {
    const char *gt = strchr(*p, '>');
    if (!gt || gt == *p || gt[-1] != '/') return false;
    *p = gt + 1;
    return true;
}

/* the words of an element's content with its tags made spaces, after a space when b has words */
static void words(qa_buf b[static 1], const char *s, const char *end) {
    bool space = b->n > 0;
    for (const char *p = s; p < end; p++) {
        const char *gt = *p == '<' && p + 1 < end && p[1] != '>' ? memchr(p + 1, '>', (size_t)(end - p - 1)) : nullptr;
        if (gt || isspace((unsigned char)*p)) {
            space = b->n > 0;
            if (gt) p = gt;
            continue;
        }
        if (space) qa_put(b, 1, " ");
        space = false;
        qa_put(b, 1, p);
    }
}

static void our_states(const char *svg, pts d[static 1]) {
    for (const char *p = svg; (p = strstr(p, "<rect class=\"")); p++) {
        const char *q = p + 13;
        bool note = !strncmp(q, "note\"", 5);
        if (!note && strncmp(q, "st\"", 3)) continue;
        q += note ? 5 : 3;
        double r[4];
        if (!rect(&q, r) || !tag_end(&q)) continue;
        qa_buf label = {};
        qa_put(&label, 0, "");
        size_t texts = 0;
        for (const char *c; !strncmp(q, "<text", 5); q = c + 7) {
            const char *o = strchr(q, '>');
            if (!o || !(c = strstr(o + 1, "</text>")) || memchr(o + 1, '\n', (size_t)(c - o - 1))) break;
            texts++;
            if (note || !label.n) words(&label, o + 1, c);
        }
        if (!texts) continue;
        if (label.n) put(d, note ? qa_fmt("NOTE:%s", label.p) : label.p, r[0] + r[2] / 2, r[1] + r[3] / 2);
        p = q - 1;
    }
    for (const char *p = svg; (p = strstr(p, "<rect class=\"comp\"")); p++) {
        const char *q = p + 18;
        double r[4];
        if (!rect(&q, r) || !tag_end(&q) || strncmp(q, "<path class=\"comp-t\"", 20)) continue;
        q += 20;
        if (!tag_end(&q) || strncmp(q, "<text", 5) || !(q = strchr(q, '>'))) continue;
        q++;
        size_t n = strcspn(q, "<");
        while (n && isspace((unsigned char)*q)) q++, n--;
        while (n && isspace((unsigned char)q[n - 1])) n--;
        put(d, qa_fmt("C:%.*s", (int)n, q), r[0] + r[2] / 2, r[1] + r[3] / 2);
        p = q + n - 1;
    }
}

static void state(qa_strs files[static 1]) {
    if (!files->n) {
        qa_glob(files, "tests/docs/stateDiagram/*.mmd");
        qa_glob(files, "tests/cases/*state*.mmd");
    }
    qa_render *m = mermaid(files);
    size_t tot[3] = {};
    for (size_t f = 0; f < files->n; f++) {
        const char *src = files->v[f];
        if (!m[f].svg) {
            printf("%s: mermaid-cli failed\n", src);
            continue;
        }
        size_t len;
        char *svg = qa_read(m[f].svg, &len), err[128];
        xml_doc *doc = xml_parse(len, svg, err);
        if (!doc) qa_die("%s: %s", m[f].svg, err);
        pts theirs = {}, mine = {};
        mermaid_walk(doc->root, 0, 0, &theirs);
        qa_res r = ours(src);
        our_states(r.out, &mine);
        pt **k = qa_alloc((theirs.n + 1) * sizeof *k), **o = qa_alloc((theirs.n + 1) * sizeof *o);
        size_t n = 0, nt = 0, agree[2] = {};
        for (size_t i = 0; i < theirs.n; i++) {
            if (theirs.v[i].dup) continue;
            nt++;
            if ((o[n] = find(&mine, theirs.v[i].key)) && !o[n]->dup) k[n++] = &theirs.v[i];
        }
        for (size_t i = 0; i < n; i++)
            for (size_t j = i + 1; j < n; j++) {
                agree[0] += sg(o[i]->x, o[j]->x) == sg(k[i]->x, k[j]->x);
                agree[1] += sg(o[i]->y, o[j]->y) == sg(k[i]->y, k[j]->y);
            }
        size_t pairs = n * (n - 1) / 2;
        tot[0] += agree[0], tot[1] += agree[1], tot[2] += pairs;
        printf("%-40s x %zu/%zu y %zu/%zu matched %zu/%zu\n", src, agree[0], pairs, agree[1], pairs, n, nt);
    }
    double all = (double)(tot[2] ? tot[2] : 1);
    printf("TOTAL x %.4f y %.4f pairs %zu\n", (double)tot[0] / all, (double)tot[1] / all, tot[2]);
}

/* ---- size ---- */

typedef struct row { double d; char *tag, *what; } row;

/* Python's descending sort of (d, tag, what) */
static int cmp_row(const void *a, const void *b) {
    const row *x = a, *y = b;
    if (x->d != y->d) return x->d < y->d ? 1 : -1;
    int c = strcmp(y->tag, x->tag);
    return c ? c : strcmp(y->what, x->what);
}

static void size(size_t top) {
    qa_strs files = {};
    qa_glob(&files, "tests/docs/*/*.mmd");
    qa_render *m = mermaid(&files);
    row *rows = qa_alloc(files.n * sizeof *rows);
    size_t n = 0;
    for (size_t f = 0; f < files.n; f++) {
        if (!m[f].svg) continue;
        const char *src = files.v[f] + 11;
        char *tag = qa_fmt("%.*s", (int)strlen(src) - 4, src);
        for (char *p = tag; *p; p++)
            if (*p == '/') *p = '_';
        qa_res r = ours(files.v[f]);
        if (r.code != 0) {
            rows[n++] = (row){99, tag, "ERR"};
            continue;
        }
        size_t len;
        double w, h, mw, mh;
        if (!qa_svg_size(r.out, &w, &h) || !qa_svg_size(qa_read(m[f].svg, &len), &mw, &mh)) continue;
        rows[n++] = (row){fabs(log((w / fmax(h, 1)) / (mw / mh))), tag,
                          qa_fmt("ours %.0fx%.0f mermaid %.0fx%.0f", w, h, mw, mh)};
        qa_res_free(&r);
    }
    qsort(rows, n, sizeof *rows, cmp_row);
    double *d = qa_alloc((n + 1) * sizeof *d);
    for (size_t i = 0; i < n; i++) {
        d[i] = rows[i].d;
        if (i < top) printf("%6.2f %-28s %s\n", rows[i].d, rows[i].tag, rows[i].what);
    }
    printf("n=%zu mean=%.3f\n", n, pysum(n, d) / (double)n);
}

static const char usage[] = "usage: agree flow|state [--mmd BIN] [FILE...]\n       agree size [--mmd BIN] [N]";

int main(int argc, char *argv[]) {
    qa_at_root(argc, argv);
    if (argc < 2) qa_die("%s", usage);
    qa_strs args = {};
    for (int i = 2; i < argc; i++)
        if (!strcmp(argv[i], "--mmd") && i + 1 < argc) mmd = argv[++i];
        else if (argv[i][0] == '-') qa_die("%s", usage);
        else qa_push(&args, argv[i]);
    if (!strcmp(argv[1], "flow")) flow(&args);
    else if (!strcmp(argv[1], "state")) state(&args);
    else if (!strcmp(argv[1], "size") && args.n <= 1) size(args.n ? strtoul(args.v[0], nullptr, 10) : 40);
    else qa_die("%s", usage);
    return 0;
}
