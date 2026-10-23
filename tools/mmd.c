/* mmd: render Mermaid (file or stdin) to SVG. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mmd.h"

[[noreturn]] static void usage(int code) {
    fputs("usage: mmd [-i input.mmd|-] [-o output.svg|-] [-t default|dark|forest|neutral] [--no-avoid]\n", code ? stderr : stdout);
    exit(code);
}

[[nodiscard]] static char *slurp(FILE *f, size_t *len) {
    size_t cap = 1 << 16, n = 0;
    char *p = malloc(cap);
    if (!p) return nullptr;
    for (;;) {
        if (cap - n < 4096) {
            char *np = realloc(p, cap * 2);
            if (!np) goto fail;
            p = np;
            cap *= 2;
        }
        size_t r = fread(p + n, 1, cap - n, f);
        n += r;
        if (r == 0) {
            if (ferror(f)) goto fail;
            break;
        }
    }
    *len = n;
    return p;
fail:
    free(p);
    return nullptr;
}

int main(int argc, char **argv) {
    const char *in = "-", *outp = "-";
    mmd_options opt = {};
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-i") && i + 1 < argc) in = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) outp = argv[++i];
        else if ((!strcmp(argv[i], "-t") || !strcmp(argv[i], "--theme")) && i + 1 < argc) {
            static const char *const names[] = {"default", "dark", "forest", "neutral"};
            const char *t = argv[++i];
            opt.theme = MMD_THEME_AUTO;
            for (size_t k = 0; k < sizeof names / sizeof names[0]; k++)
                if (!strcmp(t, names[k])) opt.theme = (mmd_theme)(MMD_THEME_DEFAULT + k);
            if (opt.theme == MMD_THEME_AUTO) usage(2);
        } else if (!strcmp(argv[i], "--avoid")) opt.flags |= MMD_F_ROUTE_AVOID;
        else if (!strcmp(argv[i], "--no-avoid")) opt.flags |= MMD_F_NO_AVOID;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) usage(0);
        else if (argv[i][0] != '-' || !argv[i][1]) in = argv[i];
        else usage(2);
    }
    int rc = 1;
    FILE *f = strcmp(in, "-") ? fopen(in, "rb") : stdin;
    char *src = nullptr;
    mmd_buffer svg = {};
    if (!f) { fprintf(stderr, "mmd: %s: %s\n", in, strerror(errno)); return 1; }
    size_t n = 0;
    src = slurp(f, &n);
    if (f != stdin) fclose(f);
    if (!src) { fprintf(stderr, "mmd: read failed\n"); goto out; }
    mmd_error err;
    mmd_status st = mmd_render_svg(n, src, &opt, &svg, &err);
    if (st != MMD_OK) {
        if (err.line) fprintf(stderr, "mmd: %s:%u:%u: %s\n", in, err.line, err.col, err.message);
        else fprintf(stderr, "mmd: %s: %s\n", in, err.message);
        goto out;
    }
    FILE *o = strcmp(outp, "-") ? fopen(outp, "wb") : stdout;
    if (!o) { fprintf(stderr, "mmd: %s: %s\n", outp, strerror(errno)); goto out; }
    bool ok = fwrite(svg.data, 1, svg.len, o) == svg.len;
    if (o != stdout) ok = (fclose(o) == 0) && ok;
    else ok = (fflush(o) == 0) && ok;
    if (!ok) { fprintf(stderr, "mmd: write failed\n"); goto out; }
    rc = 0;
out:
    mmd_buffer_free(&svg);
    free(src);
    return rc;
}
