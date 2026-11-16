/* bench-compare: compare mmd and mmdr on the same inputs.
 * usage: build/release/bench/compare FILE.mmd...
 * Columns: process wall-clock median (both CLIs, SVG to a file), plus in-library time (mmd: the
 * bench-render median; mmdr: its own --timing total_us).
 * Environment: MMD (build/release/mmd), BENCH (build/release/bench/render), MMDR (mmdr on PATH), TIMEOUT (seconds,
 * 120), MMDR_ARGS (extra mmdr arguments, split on whitespace). */
#include <math.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#include "qa/qa.h"

static double timeout;

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* median seconds of up to n runs (NAN on failure, err then says why), else the last run's stderr */
static double wall(const char *const argv[], int n, char **err) {
    double ts[n];
    int k = 0;
    while (k < n) {
        double t = now();
        qa_res r = qa_run(&(qa_cmd){.argv = argv, .timeout = (unsigned)ceil(timeout)});
        ts[k++] = now() - t;
        if (r.code == -SIGALRM) {
            *err = qa_fmt("timeout>%.0fs", timeout);
            return NAN;
        }
        if (r.code) {
            *err = qa_fmt("ERR %.80s", r.err);
            return NAN;
        }
        free(*err);
        *err = r.err;
        free(r.out);
        if (ts[k - 1] > 5) break;
    }
    qsort(ts, (size_t)k, sizeof *ts, cmp_double);
    return k % 2 ? ts[k / 2] : (ts[k / 2 - 1] + ts[k / 2]) / 2;
}

static char *ms(double x) { return isnan(x) ? qa_fmt("        -") : qa_fmt("%9.2f", x * 1000); }
/* library times are often a few microseconds: in us (ms with 2 decimals rounded them to 0.00) */
static char *us(double x) { return isnan(x) ? qa_fmt("        -") : qa_fmt("%9.1f", x * 1e6); }

static const char *env(const char *name, const char *dflt) {
    const char *v = getenv(name);
    return v && *v ? v : dflt;
}

int main(int argc, char *argv[]) {
    qa_at_root(argc, argv);
    const char *mmd = env("MMD", "build/release/mmd"), *bench = env("BENCH", "build/release/bench/render");
    const char *mmdr = env("MMDR", nullptr);
    if (!mmdr && !(mmdr = qa_which("mmdr"))) {
        fputs("mmdr not found: put it on PATH or set MMDR\n", stderr);
        return 1;
    }
    timeout = strtod(env("TIMEOUT", "120"), nullptr);
    qa_strs extra = {};
    for (char *s = qa_fmt("%s", env("MMDR_ARGS", "")), *t; (t = strtok(s, " \t\n")); s = nullptr) qa_push(&extra, t);

    char *tdir = qa_tmpdir("compare");
    char *mmd_svg = qa_fmt("%s/mmd.svg", tdir), *mmdr_svg = qa_fmt("%s/mmdr.svg", tdir);

    printf("%-44s %10s %10s %11s %11s %7s %7s\n", "input", "mmd lib us", "mmd cli ms", "mmdr lib us", "mmdr cli ms", "lib x", "cli x");
    fflush(stdout);
    for (int i = 1; i < argc; i++) {
        const char *f = argv[i];
        int n = 15;
        qa_res b = qa_exec(bench, f, "30");
        double clib = NAN;
        for (char *m = b.out; (m = strstr(m, "median=")); m++) {
            char *e;
            double v = strtod(m + 7, &e);
            if (e > m + 7 && e[0] == 'u' && e[1] == 's' && m[7] != '-' && m[7] != '+') {
                clib = v / 1e6;
                break;
            }
        }
        qa_res_free(&b);
        char *err = nullptr;
        double c = wall(QA_ARGV(mmd, f, "-o", mmd_svg), n, &err);
        free(err);
        err = nullptr;
        const char *dv[extra.n + 8];
        size_t k = 0;
        dv[k++] = mmdr;
        for (size_t j = 0; j < extra.n; j++) dv[k++] = extra.v[j];
        static const char *const tail[] = {"--timing", "-i", nullptr, "-o", nullptr};
        for (size_t j = 0; j < countof(tail); j++) dv[k++] = j == 2 ? f : j == 4 ? mmdr_svg : tail[j];
        dv[k] = nullptr;
        double d = wall(dv, n, &err), dlib = NAN;
        if (!isnan(d)) {
            /* the last line of stderr is JSON with total_us */
            size_t len = strlen(err);
            while (len && strchr(" \t\r\n", err[len - 1])) err[--len] = '\0';
            char *last = strrchr(err, '\n'), *t;
            last = last ? last + 1 : err;
            if ((t = strstr(last, "\"total_us\""))) {
                t += 10;
                t += strspn(t, " \t");
                char *e;
                if (*t == ':' && (dlib = strtod(t + 1, &e), e > t + 1)) dlib /= 1e6;
                else dlib = NAN;
            }
        }
        const char *base = strrchr(f, '/');
        const char *note = isnan(d) ? qa_fmt("  mmdr: %s", err) : "";
        const char *lx = clib && dlib && !isnan(clib) && !isnan(dlib) ? qa_fmt("%6.0fx", dlib / clib) : "     -";
        const char *sp = c && d && !isnan(c) && !isnan(d) ? qa_fmt("%6.1fx", d / c) : "     -";
        base = base ? base + 1 : f;
        printf("%-44.44s %10s %10s %11s %11s %7s %7s%s\n", base, us(clib), ms(c), us(dlib), ms(d), lx, sp, note);
        fflush(stdout);
    }
    qa_rmtree(tdir);
}
