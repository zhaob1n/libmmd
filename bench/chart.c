/* bench-chart: regenerate docs/bench.svg and the README CLI table from bench/cli_times.csv.
 * usage: build/release/bench/chart
 * The chart is a libmmd xychart on a log axis: per input, the wall-clock ms of libmmd, mmdr and
 * mermaid-cli, each rival's bar labelled with its ratio to libmmd (a linear axis would leave libmmd's
 * bars under a pixel). The README table between the <!-- bench-table --> markers gives the same. */
#include "mmd.h"
#include "qa/qa.h"

enum { MAXROWS = 64 };
static const char *const TOOLS[] = {"libmmd", "mmdr", "mermaid-cli"};

typedef struct row { char *input, *source; double ms[3]; } row; /* libmmd, mmdr, mermaid-cli */

/* "27.9 ms (63×)": tool k's time, and a rival's ratio to libmmd */
static void cell(qa_buf b[static 1], const double m[static 3], size_t k) {
    qa_putf(b, m[k] < 10 ? "%.2f ms" : m[k] < 100 ? "%.1f ms" : "%.0f ms", m[k]);
    if (k) qa_putf(b, m[k] / m[0] >= 10 ? " (%.0f×)" : " (%.1f×)", m[k] / m[0]);
}

static size_t read_csv(row rows[static MAXROWS]) {
    size_t len, n = 0;
    char *s = qa_read("bench/cli_times.csv", &len);
    int col[5] = {-1, -1, -1, -1, -1}; /* input, source, libmmd, mmdr, mermaid-cli */
    static const char *const names[] = {"input", "source", "libmmd", "mmdr", "mermaid-cli"};
    bool header = true;
    for (char *line = s, *next; *line; line = next) {
        char *end = line + strcspn(line, "\n");
        next = end + (*end == '\n');
        if (*line == '#' || end == line) continue;
        *end = '\0';
        char *f[16];
        size_t nf = 0;
        for (char *p = line; nf < countof(f); p++) {
            f[nf++] = p;
            if (!(p = strchr(p, ','))) break;
            *p = '\0';
        }
        if (header) {
            for (size_t i = 0; i < nf; i++)
                for (size_t k = 0; k < countof(names); k++)
                    if (!strcmp(f[i], names[k])) col[k] = (int)i;
            for (size_t k = 0; k < countof(names); k++)
                if (col[k] < 0 || (size_t)col[k] >= nf) qa_die("bench/cli_times.csv: no %s column", names[k]);
            header = false;
            continue;
        }
        if (n == MAXROWS) qa_die("bench/cli_times.csv: more than %d rows", MAXROWS);
        for (size_t k = 0; k < countof(names); k++)
            if ((size_t)col[k] >= nf) qa_die("bench/cli_times.csv: short row %s", line);
        rows[n] = (row){.input = f[col[0]], .source = f[col[1]]};
        for (int k = 0; k < 3; k++) rows[n].ms[k] = strtod(f[col[2 + k]], nullptr);
        n++;
    }
    return n;
}

static void chart(size_t n, const row rows[static n]) {
    const char *cats[MAXROWS], *lp[3][MAXROWS];
    double v[3][MAXROWS];
    qa_buf lb[3][MAXROWS] = {};
    for (size_t i = 0; i < n; i++) {
        cats[i] = rows[i].input;
        for (size_t k = 0; k < countof(TOOLS); k++) {
            v[k][i] = rows[i].ms[k];
            cell(&lb[k][i], rows[i].ms, k);
            lp[k][i] = lb[k][i].p;
        }
    }
    mmd_writer *w = mmd_writer_new();
    char b[16];
    mmd_w_config(w, "xyChart.yAxis.scale", "log");
    mmd_w_config(w, "xyChart.width", "900");
    snprintf(b, sizeof b, "%d", 120 + 60 * (int)n);
    mmd_w_config(w, "xyChart.height", b);
    mmd_w_config(w, "themeVariables.fontFamily", "-apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, 'Helvetica Neue', Arial, sans-serif");
    mmd_xy_begin(w, "CLI time per diagram (lower is better)", true);
    mmd_xy_x_cats(w, nullptr, n, cats);
    mmd_xy_y_range(w, "wall-clock ms, log scale", 0, 0);
    for (size_t k = 0; k < countof(TOOLS); k++) mmd_xy_bar(w, TOOLS[k], n, v[k], lp[k]);
    mmd_buffer svg = {};
    mmd_error err = {};
    if (mmd_writer_render_svg(w, nullptr, &svg, &err) != MMD_OK)
        qa_die("docs/bench.svg: %u:%u: %s", err.line, err.col, err.message);
    qa_write("docs/bench.svg", svg.len, svg.data);
    mmd_buffer_free(&svg);
    mmd_writer_free(w);
    for (size_t i = 0; i < n; i++)
        for (size_t k = 0; k < countof(TOOLS); k++) free(lb[k][i].p);
}

static void table(size_t n, const row rows[static n]) {
    static const char open[] = "<!-- bench-table -->\n", close[] = "<!-- /bench-table -->";
    size_t len;
    char *s = qa_read("README.md", &len);
    char *a = strstr(s, open), *z = a ? strstr(a + strlen(open), close) : nullptr;
    if (!z) qa_die("README.md: no %s ... %s", open, close);
    qa_buf o = {};
    qa_put(&o, (size_t)(a - s) + strlen(open), s);
    qa_puts(&o, "| input | libmmd | mmdr 0.3.1 | mermaid-cli 12.0 |\n|---|---:|---:|---:|\n");
    for (size_t i = 0; i < n; i++) {
        qa_putf(&o, "| %s (%s)", rows[i].input, rows[i].source);
        for (size_t k = 0; k < countof(TOOLS); k++) {
            qa_puts(&o, " | ");
            cell(&o, rows[i].ms, k);
        }
        qa_puts(&o, " |\n");
    }
    qa_puts(&o, z);
    qa_write("README.md", o.n, o.p);
}

int main(int argc, char *argv[]) {
    qa_at_root(argc, argv);
    if (argc != 1) {
        fputs("usage: bench-chart\n", stderr);
        return 2;
    }
    row rows[MAXROWS];
    size_t n = read_csv(rows);
    chart(n, rows);
    table(n, rows);
}
