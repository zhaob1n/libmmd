/* sbs: pictures of libmmd's rendering beside mermaid.js's, for looking at differences.
 *
 *   sbs [--gallery] [--ours] [--mmd BIN | --base REV] [-o OUT] INPUT...
 *
 * INPUT is a .mmd file or the name of a tests/docs directory (`block`: the .mmd files in tests/docs/block).
 *   default    OUT/<tag>.png: libmmd left, mermaid right, a red gap between. OUT defaults to
 *              $TMPDIR/mmd-UID/sbs; tag is the path below tests/docs/ or tests/cases/ (else the
 *              basename) without .mmd, '/' made '_'. Where mermaid refuses the source, libmmd alone.
 *   --ours     libmmd's rendering alone.
 *   --gallery  the README gallery: OUT defaults to docs/gallery, the file is named after the input's
 *              basename; each side scaled down to fit 700x640, captioned, a thin rule between. The PNG
 *              is quantised to 128 colours with pngquant when that is on PATH, else kept as Chrome
 *              wrote it.
 *   -o OUT     the output directory; with a single input, a name ending in .png is the file itself.
 *   --mmd BIN  the renderer (default build/release/mmd).
 *   --base REV the renderer of that revision, built in a git worktree: what a change looked like
 *              before, e.g.  sbs --base HEAD -o x_before.png f.mmd; sbs -o x_after.png f.mmd
 * Prints each PNG written, or `ERR file: reason` and goes on; exits 1 when any input failed.
 *
 * Nothing is rasterised here: both SVGs go into a small HTML page sized from their root viewBoxes, and
 * headless Chrome screenshots it. mermaid's SVGs come from mermaid.js with its classic look and dagre
 * layout (qa_mermaid, QA_CLASSIC), cached in $TMPDIR/mmd-UID/mermaid-classic. A diagram whose file
 * name contains `dark` is drawn on #333, as mermaid's dark themes leave the background transparent. */
#include "qa/qa.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <unistd.h>

enum mode { SBS, OURS, GALLERY };

typedef struct job {
    char *src, *tag, *png;
    qa_render m;
    char *line; /* what to print */
    bool failed;
} job;

static struct {
    enum mode mode;
    const char *mmd, *work, *css, *pngquant;
    job *jobs;
} g = {.mmd = "build/release/mmd"};

/* REV's renderer, built in a worktree that stays (TMPDIR/mmd-UID/base): what a change looked like before */
static const char *base_mmd(const char *rev) {
    char *wt = qa_scratch("base"), *git = qa_fmt("%s/.git", wt);
    qa_res r = qa_exec("git", "rev-parse", "--verify", "-q", rev);
    if (r.code != 0) qa_die("--base %s: no such revision", rev);
    char *hash = qa_fmt("%.*s", (int)strcspn(r.out, "\n"), r.out);
    r = access(git, F_OK) == 0 ? qa_exec("git", "-C", wt, "switch", "-q", "--detach", hash)
                               : qa_exec("git", "worktree", "add", "-q", "--detach", wt, hash);
    /* the default goal is `all` in both layouts; only the binary's path under it changed */
    if (r.code == 0) r = qa_exec("make", "-s", "-C", wt);
    if (r.code != 0) qa_die("--base %s:\n%s", rev, r.err);
    char *rel = qa_fmt("%s/build/release/mmd", wt);
    return access(rel, F_OK) == 0 ? rel : qa_fmt("%s/build/mmd", wt);
}

static void put_url(qa_buf b[static 1], const char *path) {
    qa_puts(b, "file://");
    for (const unsigned char *s = (const unsigned char *)path; *s; s++)
        if (strchr("/._-~", *s) || (*s < 128 && isalnum(*s))) qa_put(b, 1, (const char *)s);
        else qa_putf(b, "%%%02X", *s);
}

/* scaled down to fit 700 x 640, never up */
static void fit(unsigned size[static 2], double w, double h) {
    double s = fmin(fmin(700 / w, 640 / h), 1);
    size[0] = s < 1 ? (unsigned)fmax(1, floor(w * s)) : (unsigned)ceil(w);
    size[1] = s < 1 ? (unsigned)fmax(1, floor(h * s)) : (unsigned)ceil(h);
}

/* why: its first line */
static void fail(job j[static 1], const char *why) {
    j->line = qa_fmt("ERR %s: %.*s", j->src, (int)strcspn(why, "\r\n"), why);
    j->failed = true;
}

static void put_box(qa_buf b[static 1], unsigned x, unsigned y, unsigned w, unsigned h, const char *style) {
    qa_putf(b, "<div style=\"left:%upx;top:%upx;width:%upx;height:%upx;%s\">", x, y, w, h, style);
}

static void put_caption(qa_buf b[static 1], unsigned x, const char *color, const char *s) {
    qa_putf(b, "<div class=\"caption\" style=\"left:%upx;top:8px;color:%s\">%s</div>\n", x, color, s);
}

/* libmmd's SVG as an image. mermaid's goes into the page itself, as mermaid-cli shows it: its labels
 * are HTML in <foreignObject>, set in the web fonts and Font Awesome of mermaid-cli's stylesheets
 * (g.css), which an SVG document (<img>, <object>, <iframe>) cannot see. */
static void put_sides(qa_buf b[static 1], const char *ours, const unsigned a[static 2], unsigned ax,
                      const char *mermaid, const unsigned m[static 2], unsigned mx, unsigned y, bool dark) {
    const char *bg = dark ? "background:#333" : "";
    put_box(b, ax, y, a[0], a[1], bg);
    qa_puts(b, "<img src=\"");
    put_url(b, ours);
    qa_puts(b, "\"></div>\n");
    if (!mermaid) return;
    put_box(b, mx, y, m[0], m[1], bg);
    qa_puts(b, mermaid);
    qa_puts(b, "</div>\n");
}

static void one(size_t i, void *) {
    job *j = &g.jobs[i];
    qa_res r = qa_run(&(qa_cmd){.argv = QA_ARGV(g.mmd, j->src), .timeout = 120});
    if (r.code != 0) {
        fail(j, r.err_len && !strchr("\r\n", *r.err) ? r.err : qa_fmt("%s exited with %d", g.mmd, r.code));
        return;
    }
    double ow, oh, mw, mh;
    if (!qa_svg_size(r.out, &ow, &oh)) {
        fail(j, "no viewBox on libmmd's <svg>");
        return;
    }
    char *ours = qa_fmt("%s/%s.%d.svg", g.work, j->tag, (int)getpid()), *mermaid = nullptr;
    qa_write(ours, r.out_len, r.out);
    qa_res_free(&r);
    if (g.mode != OURS && j->m.svg) {
        size_t n;
        mermaid = qa_read(j->m.svg, &n);
        if (!qa_svg_size(mermaid, &mw, &mh)) free(mermaid), mermaid = nullptr;
    }
    const char *base = strrchr(j->src, '/');
    bool dark = strstr(base ? base : j->src, "dark");

    qa_buf b = {};
    qa_puts(&b, "<!doctype html>\n<meta charset=\"utf-8\">\n");
    qa_puts(&b, g.css);
    qa_puts(&b, "<style>body{margin:0;background:#fff}body>div{position:absolute}img,svg{display:block;"
                "width:100%!important;height:100%!important;max-width:none!important;max-height:none!important}"
                ".caption{font:bold 18px sans-serif}</style>\n");
    unsigned a[2] = {(unsigned)ceil(ow), (unsigned)ceil(oh)}, m[2] = {}, w, h;
    if (mermaid) m[0] = (unsigned)ceil(mw), m[1] = (unsigned)ceil(mh);
    if (g.mode == GALLERY) {
        fit(a, ow, oh);
        if (mermaid) fit(m, mw, mh);
        w = a[0] + (mermaid ? m[0] + 30 : 20);
        h = (a[1] > m[1] ? a[1] : m[1]) + 40;
        put_caption(&b, 10, "#1f6feb", "libmmd");
        put_sides(&b, ours, a, 10, mermaid, m, a[0] + 20, 36, dark);
        if (mermaid) {
            put_caption(&b, a[0] + 20, "#cf222e", "mermaid-cli");
            put_box(&b, a[0] + 15, 36, 1, h - 40, "background:#ccc");
            qa_puts(&b, "</div>\n");
        }
    } else {
        w = a[0] + (mermaid ? 20 + m[0] : 0);
        h = a[1] > m[1] ? a[1] : m[1];
        put_sides(&b, ours, a, 0, mermaid, m, a[0] + 20, 0, dark);
        if (mermaid) {
            put_box(&b, a[0], 0, 20, h, "background:red");
            qa_puts(&b, "</div>\n");
        }
    }
    char *page = qa_fmt("%s/%s.%d.html", g.work, j->tag, (int)getpid());
    qa_write(page, b.n, b.p);
    qa_screenshot(page, w, h, j->png);
    unlink(page);
    unlink(ours);
    if (g.mode == GALLERY && g.pngquant) {
        r = qa_exec(g.pngquant, "--force", "--ext", ".png", "128", j->png);
        if (r.code != 0) qa_die("pngquant %s failed:\n%s", j->png, r.err);
    }
    j->line = j->png;
}

static const char usage[] = "usage: sbs [--gallery] [--ours] [--mmd BIN | --base REV] [-o OUT] INPUT...";

static bool ends_with(const char *s, const char *end) {
    size_t n = strlen(s), k = strlen(end);
    return n >= k && !strcmp(s + n - k, end);
}

int main(int argc, char *argv[]) {
    qa_at_root(argc, argv);
    const char *out = nullptr;
    qa_strs src = {};
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--gallery")) g.mode = GALLERY;
        else if (!strcmp(a, "--ours")) g.mode = OURS;
        else if (!strcmp(a, "--mmd") && i + 1 < argc) g.mmd = argv[++i];
        else if (!strcmp(a, "--base") && i + 1 < argc) g.mmd = base_mmd(argv[++i]);
        else if (!strcmp(a, "-o") && i + 1 < argc) out = qa_from_cwd(argv[++i]);
        else if (a[0] == '-') qa_die("%s", usage);
        else if (ends_with(a, ".mmd")) qa_push(&src, qa_fmt("%s", a));
        else {
            size_t n = src.n;
            char *pat = qa_fmt("tests/docs/%s/*.mmd", a);
            qa_glob(&src, pat);
            if (src.n == n) qa_die("%s: neither a .mmd file nor a tests/docs directory", a);
            free(pat);
        }
    }
    if (!src.n) qa_die("%s", usage);
    bool file = out && ends_with(out, ".png");
    if (file && src.n > 1) qa_die("-o %s takes a single input", out);
    if (!out) out = g.mode == GALLERY ? "docs/gallery" : qa_scratch("sbs");
    if (!file) qa_mkdirs(out);
    g.work = qa_scratch("sbs/work");
    if (g.mode == GALLERY) g.pngquant = qa_which("pngquant");

    char root[PATH_MAX], full[PATH_MAX];
    if (!getcwd(root, sizeof root)) qa_die("getcwd: %m");
    size_t rn = strlen(root);
    g.jobs = qa_alloc(src.n * sizeof *g.jobs);
    for (size_t i = 0; i < src.n; i++) {
        job *j = &g.jobs[i];
        /* inside the tree, the path from its root: tags and mermaid's cache do not depend on how it was named */
        if (!realpath(src.v[i], full)) qa_die("%s: %m", src.v[i]);
        j->src = !strncmp(full, root, rn) && full[rn] == '/' ? qa_fmt("%s", full + rn + 1) : src.v[i];
        const char *t = strrchr(j->src, '/') ? strrchr(j->src, '/') + 1 : j->src;
        if (g.mode != GALLERY && (!strncmp(j->src, "tests/docs/", 11) || !strncmp(j->src, "tests/cases/", 12)))
            t = strchr(j->src + 6, '/') + 1;
        j->tag = qa_fmt("%.*s", (int)(strlen(t) - 4), t);
        for (char *p = j->tag; *p; p++)
            if (*p == '/') *p = '_';
        j->png = file ? qa_fmt("%s", out) : qa_fmt("%s/%s.png", out, j->tag);
    }
    g.css = "";
    if (g.mode != OURS) {
        char *dir = qa_scratch("mermaid-classic");
        qa_render *m = qa_alloc(src.n * sizeof *m);
        const char **files = qa_alloc(src.n * sizeof *files);
        for (size_t i = 0; i < src.n; i++) files[i] = g.jobs[i].src;
        if (!qa_mermaid(dir, QA_CLASSIC, src.n, files, m)) qa_die("mermaid-cli is not installed");
        for (size_t i = 0; i < src.n; i++) g.jobs[i].m = m[i];
        qa_buf css = {};
        qa_puts(&css, "<link rel=\"stylesheet\" href=\"");
        put_url(&css, qa_fmt("%s/mmdc.css", dir));
        qa_puts(&css, "\">\n");
        g.css = css.p;
    }
    qa_parallel(src.n, one, nullptr);
    int status = 0;
    for (size_t i = 0; i < src.n; i++) {
        puts(g.jobs[i].line);
        status |= g.jobs[i].failed;
    }
    return status;
}
