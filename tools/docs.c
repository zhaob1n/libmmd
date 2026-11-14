/* docs: the mermaid documentation examples in tests/docs.
 * usage: build/docs extract <mermaid-repo>/packages/mermaid/src/docs/syntax
 *          copy the ```mermaid / ```mermaid-example blocks of DIR's .md files into tests/docs/<doc>/NN.mmd
 *          (the flowchart doc and flowchart/graph blocks are skipped)
 *        build/docs status
 *          render every tests/docs/x/y.mmd with mermaid.js and rewrite tests/docs/mermaid_status.txt as
 *          sorted lines "path ok|fail"; exits 1 when mermaid-cli is not installed */
#include <ctype.h>

#include "qa/qa.h"

/* the line starting at p, if it is a fence opening a mermaid block: the start of the body */
static const char *opening(const char *p) {
    if (strncmp(p, "```mermaid", 10)) return nullptr;
    p += 10;
    if (!strncmp(p, "-example", 8)) p += 8;
    p += strspn(p, " \t");
    return *p == '\n' ? p + 1 : nullptr;
}

/* the first line that is not blank, a %% comment or ---, is a flowchart */
static bool flowchart(const char *s, const char *end) {
    for (const char *l = s; l < end;) {
        const char *e = memchr(l, '\n', (size_t)(end - l));
        if (!e) e = end;
        const char *a = l, *z = e;
        while (a < z && isspace((unsigned char)*a)) a++;
        while (z > a && isspace((unsigned char)z[-1])) z--;
        l = e + 1;
        if (a == z || (z - a >= 2 && !strncmp(a, "%%", 2)) || (z - a == 3 && !strncmp(a, "---", 3))) continue;
        for (const char *const *w = (const char *const[]){"flowchart", "graph", nullptr}; *w; w++) {
            size_t n = strlen(*w);
            if ((size_t)(z - a) >= n && !strncmp(a, *w, n) && (a + n == z || !(isalnum((unsigned char)a[n]) || a[n] == '_')))
                return true;
        }
        return false;
    }
    return false;
}

static void extract(const char *src) {
    qa_strs mds = {};
    qa_glob(&mds, qa_fmt("%s/*.md", src));
    size_t total = 0;
    for (size_t i = 0; i < mds.n; i++) {
        const char *base = strrchr(mds.v[i], '/') + 1;
        char *doc = qa_fmt("%.*s", (int)(strlen(base) - 3), base);
        if (!strcmp(doc, "flowchart")) continue;
        size_t len, k = 0;
        char *s = qa_read(mds.v[i], &len);
        /* like re.finditer(r'^```mermaid(?:-example)?[ \t]*\n(.*?)^```', re.S | re.M) */
        for (const char *l = s; l < s + len;) {
            const char *body = opening(l), *close = nullptr;
            if (body)
                for (const char *c = body; c < s + len; c = strchr(c, '\n') ? strchr(c, '\n') + 1 : s + len)
                    if (!strncmp(c, "```", 3)) { close = c; break; }
            if (!close) {
                const char *nl = strchr(l, '\n');
                l = nl ? nl + 1 : s + len;
                continue;
            }
            const char *nl = strchr(close, '\n');
            l = nl ? nl + 1 : s + len;
            if (flowchart(body, close)) continue;
            char *dir = qa_fmt("tests/docs/%s", doc), *out = qa_fmt("%s/%02zu.mmd", dir, ++k);
            qa_mkdirs(dir);
            qa_write(out, (size_t)(close - body), body);
            total++;
        }
    }
    printf("%zu examples\n", total);
}

static int status(void) {
    qa_strs files = {};
    qa_glob(&files, "tests/docs/*/*.mmd");
    char *tmp = qa_tmpdir("docs-status"); /* not a cache: a new mermaid must be seen */
    qa_render *r = qa_alloc(files.n * sizeof *r);
    bool have = qa_mermaid(tmp, nullptr, files.n, (const char *const *)files.v, r);
    if (!have) fputs("mermaid-cli not found: put mmdc on PATH or set MMDC\n", stderr);
    qa_buf o = {};
    size_t ok = 0;
    for (size_t i = 0; have && i < files.n; i++) {
        bool good = false;
        if (r[i].svg) {
            size_t len;
            char *svg = qa_read(r[i].svg, &len);
            good = !strstr(svg, "Syntax error");
        }
        ok += good;
        qa_putf(&o, "%s %s\n", files.v[i], good ? "ok" : "fail");
    }
    qa_rmtree(tmp);
    if (!have) return 1;
    qa_write("tests/docs/mermaid_status.txt", o.n, o.p);
    printf("%zu/%zu ok with mermaid.js\n", ok, files.n);
    return 0;
}

int main(int argc, char *argv[]) {
    qa_at_root(argc, argv);
    if (argc == 3 && !strcmp(argv[1], "extract")) extract(argv[2]);
    else if (argc == 2 && !strcmp(argv[1], "status")) return status();
    else {
        fputs("usage: docs extract <mermaid>/packages/mermaid/src/docs/syntax | docs status\n", stderr);
        return 2;
    }
}
