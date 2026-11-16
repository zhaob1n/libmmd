/* quality_scan: renders .mmd files with mmd, loads the SVGs in headless Chrome and reports geometric
 * defects found by tools/quality_scan.js (ll label/label overlap, le label/edge overlap, ct cluster
 * title hit by an edge, ov text overflowing its node, cl text clipped by the viewport, en edge through
 * a node), one line per file: FILE {KIND: (COUNT, [first 3 samples]), ...}.
 *
 *   build/release/tools/quality_scan [-a] [FILE...]   (default: the .mmd in tests/docs/NAME, tests/cases, bench/mmdr_fixtures)
 *
 * -a also shows the reviewed exceptions in KNOWN, otherwise reported as "FILE known: KINDS".
 * env BIN=path/to/mmd (default build/release/mmd), CHROME as tools/qa/qa.h says. */
#include "qa/qa.h"

#include <unistd.h>

/* flags that are intended (reviewed); reported as "known" and not counted */
static const struct { const char *file, *kinds[3]; } KNOWN[] = {
    /* notes/label offsets at user-given coords; the note text runs to the user-sized canvas edge (as mermaid) */
    {"tests/cases/wardley_19.mmd", {"le", "cl"}},
    /* 300x200 edge case: note and annotation pinned to nearby user coords */
    {"tests/cases/wardley_edge.mmd", {"ll"}},
    /* degenerate set A:0 has no area; its label sits on the union label by construction */
    {"tests/cases/venn_edge.mmd", {"ll"}},
};
enum { BATCH = 40 }; /* SVGs per page */

static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static char *replace(const char *s, const char *from, const char *to) {
    qa_buf b = {};
    qa_put(&b, 0, "");
    for (const char *f; (f = strstr(s, from)); s = f + strlen(from)) {
        qa_put(&b, (size_t)(f - s), s);
        qa_puts(&b, to);
    }
    qa_puts(&b, s);
    return b.p;
}

typedef struct job { char **files, **svg; const char *bin; } job;

/* mmd's SVG of files[i] without its XML declaration; nullptr when mmd failed */
static void render(size_t i, void *ctx) {
    job *j = ctx;
    qa_res r = qa_exec(j->bin, j->files[i], "-o", "-");
    if (r.code) return;
    for (char *x; (x = strstr(r.out, "<?xml")) && strchr(x, '>');) {
        char *gt = strchr(x, '>') + 1;
        memmove(x, gt, strlen(gt) + 1);
    }
    j->svg[i] = r.out;
}

/* the root's id: re.search(r'<svg[^>]*\sid="([^"]+)"', s) */
static char *svg_id(const char *s) {
    for (const char *t = s; (t = strstr(t, "<svg")); t += 4) {
        const char *end = t + strcspn(t, ">");
        for (const char *a = t; (a = strstr(a + 1, " id=\"")) && a < end;) {
            size_t k = strcspn(a + 5, "\">");
            if (k && a[5 + k] == '"') return strndup(a + 5, k);
        }
    }
    return nullptr;
}

/* Python's repr() of a str, as far as short labels go: C0 and C1 controls, U+00A0 and soft hyphens
 * escaped, other non-ASCII as is */
static void put_repr(qa_buf b[static 1], const char *s) {
    char q = strchr(s, '\'') && !strchr(s, '"') ? '"' : '\'';
    qa_put(b, 1, &q);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '\\' || *p == q) qa_putf(b, "\\%c", *p);
        else if (*p == '\t') qa_puts(b, "\\t");
        else if (*p == '\n') qa_puts(b, "\\n");
        else if (*p == '\r') qa_puts(b, "\\r");
        else if (*p < 0x20 || *p == 0x7f) qa_putf(b, "\\x%02x", *p);
        else if (*p == 0xc2 && (p[1] <= 0xa0 || p[1] == 0xad)) qa_putf(b, "\\x%02x", *++p);
        else qa_put(b, 1, (const char *)p);
    }
    qa_put(b, 1, &q);
}

/* splits a record of quality_scan.js in place at its tabs, undoing \\ \t \n \r */
static size_t split(char *line, char *field[static 9]) {
    size_t n = 0;
    field[n++] = line;
    char *o = line;
    for (char *s = line; *s; s++) {
        if (*s == '\t') {
            *o++ = 0;
            if (n < 9) field[n++] = o;
        } else if (*s == '\\' && s[1]) {
            s++;
            *o++ = *s == 't' ? '\t' : *s == 'n' ? '\n' : *s == 'r' ? '\r' : *s;
        } else *o++ = *s;
    }
    *o = 0;
    return n;
}

static bool known(const char *file, const char *kind) {
    for (size_t i = 0; i < countof(KNOWN); i++)
        for (size_t k = 0; !strcmp(KNOWN[i].file, file) && k < countof(KNOWN[i].kinds) && KNOWN[i].kinds[k]; k++)
            if (!strcmp(KNOWN[i].kinds[k], kind)) return true;
    return false;
}

/* the scan of files[from, to): the records of quality_scan.js, each line ending in '\n' */
static char *scan(const job j[static 1], size_t from, size_t to, const char *js, const char *page) {
    qa_buf h = {};
    qa_puts(&h, "<html><body style=\"margin:0\">");
    size_t parts = 0;
    for (size_t i = from; i < to; i++) {
        if (!j->svg[i]) continue;
        char *s = j->svg[i], *sid = svg_id(s);
        if (sid) { /* the page holds many SVGs: make the root id (and its #id style selectors) unique */
            char *u = qa_fmt("%s-q%zu", sid, parts);
            s = replace(s, qa_fmt("id=\"%s\"", sid), qa_fmt("id=\"%s\"", u));
            s = replace(s, qa_fmt("#%s", sid), qa_fmt("#%s", u));
        }
        qa_puts(&h, "<div data-f=\"");
        qa_put_xml(&h, j->files[i]);
        qa_puts(&h, "\" style=\"position:relative\">");
        qa_puts(&h, s);
        qa_puts(&h, "</div>");
        parts++;
    }
    qa_puts(&h, "<pre id=\"out\"></pre><script>");
    qa_puts(&h, js);
    qa_puts(&h, "</script></body></html>");
    qa_write(page, h.n, h.p);
    free(h.p);

    char *dom = qa_dump_dom(page), *s = strstr(dom, "<pre id=\"out\">"), *e = s ? strstr(s, "</pre>") : nullptr;
    if (!e) qa_die("%s: no <pre id=\"out\"> in Chrome's DOM", page);
    *e = 0;
    /* the text as Chrome serialises it: & < > and U+00A0 escaped */
    s = replace(s + strlen("<pre id=\"out\">"), "&lt;", "<");
    s = replace(replace(replace(s, "&gt;", ">"), "&nbsp;", "\u00a0"), "&amp;", "&");
    size_t n = strlen(s);
    if (n < 4 || strcmp(s + n - 4, "end\n") || (n > 4 && s[n - 5] != '\n'))
        qa_die("%s: tools/quality_scan.js did not finish", page);
    s[n - 4] = 0;
    return s;
}

int main(int argc, char *argv[]) {
    qa_at_root(argc, argv);
    char root[4096];
    if (!getcwd(root, sizeof root)) qa_die("getcwd: %m");
    const char *bin = getenv("BIN");
    bool all = false;
    qa_strs files = {};
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-a")) all = true;
        else qa_push(&files, argv[i]);
    if (!files.n) {
        qa_glob(&files, "tests/docs/*/*.mmd");
        qa_glob(&files, "tests/cases/*.mmd");
        qa_glob(&files, "bench/mmdr_fixtures/*.mmd");
        if (files.n) qsort(files.v, files.n, sizeof *files.v, by_name);
    }
    size_t len;
    char *js = qa_read("tools/quality_scan.js", &len), *dir = qa_tmpdir("scan"), *page = qa_fmt("%s/p.html", dir);
    job j = {.files = files.v, .svg = qa_alloc((files.n + 1) * sizeof *j.svg), .bin = bin && *bin ? bin : "build/release/mmd"};
    qa_parallel(files.n, render, &j);

    for (size_t i = 0; i < files.n; i += BATCH) {
        /* a record per file and kind, the kinds of a file together */
        char *rec = scan(&j, i, i + BATCH < files.n ? i + BATCH : files.n, js, page);
        while (*rec) {
            size_t flen = strcspn(rec, "\t");
            char *file = strndup(rec, flen), *rel = file;
            if (!strncmp(file, root, strlen(root)) && file[strlen(root)] == '/') rel += strlen(root) + 1;
            qa_buf line = {};
            qa_strs hid = {};
            while (!strncmp(rec, file, flen) && rec[flen] == '\t') {
                char *field[9], *next = strchr(rec, '\n') + 1;
                next[-1] = 0;
                size_t n = split(rec, field);
                rec = next;
                if (n < 5) qa_die("bad record from tools/quality_scan.js: %s", field[0]);
                if (!all && known(rel, field[1])) {
                    qa_push(&hid, field[1]);
                    continue;
                }
                bool px = !strcmp(field[1], "ov") || !strcmp(field[1], "cl");
                qa_putf(&line, "%s'%s': (%s, [", line.n ? ", " : "", field[1], field[2]);
                for (size_t k = 3; k + 1 < n; k += 2) {
                    qa_puts(&line, k > 3 ? ", [" : "[");
                    put_repr(&line, field[k]);
                    qa_puts(&line, ", ");
                    if (px) qa_puts(&line, field[k + 1]);
                    else put_repr(&line, field[k + 1]);
                    qa_puts(&line, "]");
                }
                qa_puts(&line, "])");
            }
            if (hid.n) {
                qsort(hid.v, hid.n, sizeof *hid.v, by_name);
                printf("%s known:", rel);
                for (size_t k = 0; k < hid.n; k++) printf("%c%s", k ? ',' : ' ', hid.v[k]);
                putchar('\n');
            }
            if (line.n) printf("%s {%s}\n", rel, line.p);
        }
    }
    qa_rmtree(dir);
    return 0;
}
