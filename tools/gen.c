/* gen: the random diagrams the benchmarks and tests run on. The generators were Python scripts; this
 * makes the same random.Random calls in the same order (an expression's operands left to right), so it
 * writes the same files.
 * usage: build/gen bench [DIR]    small, medium, large and xlarge.mmd: flowcharts to time (DIR defaults
 *                                 to bench/inputs)
 *        build/gen flow [DIR]     f000.mmd .. f399.mmd: small flowcharts with subgraphs (DIR defaults to
 *                                 tests/flow)
 *        build/gen stress OUTDIR [per_type=3] [seed=1] [scale=1]
 *                                 TYPE_I.mmd: larger diagrams of every type to stress layout quality;
 *                                 scale multiplies the upper bounds (those >= 6) of the size ranges */
#include "qa/qa.h"

static qa_rng R;
static qa_buf o; /* the diagram being generated */
#define L(...) qa_putf(&o, __VA_ARGS__)

static void save(char *path) {
    qa_write(path, o.n, o.p);
    free(path);
    o.n = 0;
}

/* ---- bench ---- */
static void bench_one(const char *dir, const char *name, int n, long fan, double label_rate, uint32_t seed, int subgraphs) {
    static const char *const shape[][2] = {
        {"[", "]"}, {"(", ")"}, {"{", "}"}, {"([", "])"}, {"[[", "]]"}, {"((", "))"}, {"{{", "}}"}, {"[(", ")]"},
    };
    static const char *const arrows[] = {"-->", "-->", "-.->", "==>", "---"};
    qa_seed(&R, seed);
    L("flowchart TD\n");
    for (int i = 0; i < n; i++) {
        const char *const *s = qa_choice(&R, shape);
        L("  n%d%sNode %d%s\n", i, s[0], i, s[1]);
    }
    if (subgraphs) {
        int per = n / (subgraphs * 3);
        for (int s = 0; s < subgraphs; s++) {
            L("  subgraph s%d [Group %d]\n", s, s);
            for (int k = 0; k < per; k++) L("    n%d\n", s * per + k);
            L("  end\n");
        }
    }
    for (int i = 1; i < n; i++)
        for (long e = qa_randint(&R, 1, fan); e > 0; e--) {
            long j = qa_randint(&R, i > 30 ? i - 30 : 0, i - 1);
            const char *arrow = qa_choice(&R, arrows);
            if (qa_random(&R) < label_rate) L("  n%ld %s|edge %ld-%d| n%d\n", j, arrow, j, i, i);
            else L("  n%ld %s n%d\n", j, arrow, i);
        }
    for (int k = 0; k < n / 50; k++) { /* a few back edges (cycles) */
        long a = qa_randint(&R, 0, n - 1), b = qa_randint(&R, 0, a);
        L("  n%ld --> n%ld\n", a, b);
    }
    save(qa_fmt("%s/%s.mmd", dir, name));
}

static void bench(const char *dir) {
    qa_mkdirs(dir);
    bench_one(dir, "small", 10, 2, 0.3, 1, 0);
    bench_one(dir, "medium", 100, 2, 0.2, 2, 3);
    bench_one(dir, "large", 1000, 2, 0.1, 3, 0);
    bench_one(dir, "xlarge", 3000, 2, 0.05, 4, 0);
}

/* ---- flow ---- */
static void flow_corpus(const char *dir) {
    static const char *const dirs[] = {"TD", "LR", "BT", "RL"};
    qa_mkdirs(dir);
    for (uint32_t s = 0; s < 400; s++) {
        qa_seed(&R, s);
        uint32_t n = (uint32_t)qa_randint(&R, 2, 40);
        L("flowchart %s\n", qa_choice(&R, dirs));
        for (uint32_t i = 0; i < n; i++) L("  n%u[%.*s n%u]\n", i, (int)qa_randint(&R, 1, 12), "xxxxxxxxxxxx", i);
        long ns = qa_randint(&R, 0, 4);
        bool used[40] = {};
        for (long k = 0; k < ns; k++) {
            uint32_t m = (uint32_t)qa_randint(&R, 1, n < 5 ? n : 5), pick[5];
            qa_sample(&R, n, m, pick);
            bool any = false;
            for (uint32_t j = 0; j < m; j++) {
                if (used[pick[j]]) continue;
                if (!any) L("  subgraph s%ld\n", k);
                any = used[pick[j]] = true;
                L("    n%u\n", pick[j]);
            }
            if (any) L("  end\n");
        }
        for (long e = 0, ne = qa_randint(&R, 1, 2 * n); e < ne; e++) {
            uint32_t a = qa_below(&R, n), b = qa_below(&R, ns && qa_random(&R) < .05 ? n + 1 : n); /* n: s0 */
            L("  n%u -->", a);
            if (qa_random(&R) < .3) L("|l%ld|", e);
            if (b < n) L(" n%u\n", b);
            else L(" s0\n");
        }
        save(qa_fmt("%s/f%03u.mmd", dir, s));
    }
}

/* ---- stress ---- */
static double scale;
static const char *const W[] = {"alpha", "beta", "gamma", "delta", "load config", "parse input", "validate",
    "retry later", "send mail", "x", "long label with words", "ok", "db", "cache miss", "user"};
static const char *w(void) { return qa_choice(&R, W); }
/* the randint the Python patched in: upper bounds >= 6 scaled */
static long ri(long a, long b) {
    if (b >= 6) {
        long u = (long)((double)b * scale);
        b = u > a ? u : a;
    }
    return qa_randint(&R, a, b);
}
static long rr(long n) { return qa_below(&R, (uint32_t)n); }
/* s title-cased if title, its spaces replaced by sp */
static void put_word(qa_buf b[static 1], const char *s, bool title, const char *sp) {
    for (bool start = true; *s; start = *s++ == ' ')
        if (*s == ' ') qa_puts(b, sp);
        else qa_putf(b, "%c", title && start ? *s - 'a' + 'A' : *s);
}

static void flow(void) {
    static const char *const dirs[] = {"TD", "LR", "BT", "RL"}, *const arrows[] = {"-->", "-.->", "==>", "---"};
    long n = ri(12, 30);
    L("flowchart %s\n", qa_choice(&R, dirs));
    for (long i = 0; i < n; i++)
        if (qa_random(&R) < .6) L("  n%ld[\"%s\"]\n", i, w());
        else L("  n%ld{\"%s\"}\n", i, w());
    for (long k = (long)((double)n * 1.4); k > 0; k--) {
        long a = rr(n), b = rr(n);
        L("  n%ld %s", a, qa_choice(&R, arrows));
        if (qa_random(&R) < .4) L("|%s|", w());
        L(" n%ld\n", b);
    }
    if (qa_random(&R) < .7) {
        long k = ri(2, 5);
        L("  subgraph S1 [group one]\n");
        for (long i = 0; i < k; i++) L("    n%ld\n", i);
        L("  end\n");
    }
}

static void cls(void) {
    static const char *const m1[] = {"1", "0..*", "many"}, *const rel[] = {"<|--", "*--", "o--", "-->", "..>"},
                             *const m2[] = {"1", "*"};
    long n = ri(6, 14);
    L("classDiagram\n");
    for (long i = 0; i < n; i++) {
        L("  class C%ld {\n    +", i);
        put_word(&o, w(), false, "_");
        L(" int\n    +m%ld() void\n  }\n", i);
    }
    for (long k = 0; k < n + 4; k++) {
        long a = rr(n);
        const char *x = qa_choice(&R, m1), *r = qa_choice(&R, rel), *y = qa_choice(&R, m2);
        long b = rr(n);
        L("  C%ld \"%s\" %s \"%s\" C%ld : %s\n", a, x, r, y, b, w());
    }
}

static void state(void) {
    long n = ri(8, 18);
    L("stateDiagram-v2\n  [*] --> s0\n");
    for (long k = 0; k < 2 * n; k++) {
        long a = rr(n), b = rr(n);
        L("  s%ld --> s%ld", a, b);
        if (qa_random(&R) < .5) L(" : %s", w());
        L("\n");
    }
    L("  s%ld --> [*]\n", n - 1);
}

static void er(void) {
    static const char *const rel[] = {"||--o{", "}|..|{", "|o--||"};
    long n = ri(5, 12);
    L("erDiagram\n");
    for (long k = 0; k < n + 3; k++) {
        long a = rr(n);
        const char *r = qa_choice(&R, rel);
        long b = rr(n);
        L("  E%ld %s E%ld : \"%s\"\n", a, r, b, w());
    }
    for (long i = 0; i < n; i++) {
        L("  E%ld {\n    int id PK\n    string ", i);
        put_word(&o, w(), false, "_");
        L("\n  }\n");
    }
}

static void seq(void) {
    static const char *const msg[] = {"->>", "-->>", "-x", "-)"};
    long n = ri(3, 7);
    L("sequenceDiagram\n");
    for (int k = 0; k < 25; k++) {
        double r = qa_random(&R);
        long a = rr(n), b = rr(n);
        if (r < .1) {
            const char *x = w();
            L("  Note over P%ld,P%ld: %s %s\n", a, b, x, w());
        } else if (r < .15) {
            L("  loop %s\n", w());
            L("    P%ld->>P%ld: %s\n  end\n", a, b, w());
        } else {
            const char *m = qa_choice(&R, msg);
            L("  P%ld%sP%ld: %s\n", a, m, b, w());
        }
    }
}

static void block(void) {
    L("block-beta\n  columns %ld\n", ri(2, 5));
    long n = ri(6, 16);
    for (long i = 0; i < n; i++) {
        L("  b%ld[\"%s\"]", i, w());
        if (qa_random(&R) < .2) L(":%ld", ri(1, 2));
        L("\n");
    }
    for (long k = 0; k < n / 2; k++) {
        bool labelled = qa_random(&R) < .5;
        long a = rr(n);
        const char *x = labelled ? w() : nullptr;
        long b = rr(n);
        if (labelled) L("  b%ld -- \"%s\" --> b%ld\n", a, x, b);
        else L("  b%ld --> b%ld\n", a, b);
    }
}

static void mind_rec(int d, int ind) {
    for (long k = ri(1, d < 2 ? 4 : 2); k > 0; k--) {
        L("%*s%s\n", ind, "", w());
        if (d < 3 && qa_random(&R) < .6) mind_rec(d + 1, ind + 2);
    }
}
static void mind(void) {
    L("mindmap\n  root((center))\n");
    mind_rec(0, 4);
}

/* branch names: -1 is main, i is bi */
static void put_branch(int b) {
    if (b < 0) L("main\n");
    else L("b%d\n", b);
}
static void gitg(void) {
    int br[26] = {-1}, cur = -1;
    uint32_t nb = 1;
    L("gitGraph\n");
    for (int i = 0; i < 25; i++) {
        double r = qa_random(&R);
        if (r < .15) {
            L("  branch b%d\n", i);
            br[nb++] = cur = i;
        } else if (r < .3) {
            cur = br[qa_below(&R, nb)];
            L("  checkout ");
            put_branch(cur);
        } else if (r < .4 && nb > 1) {
            int others[26];
            uint32_t no = 0;
            for (uint32_t k = 0; k < nb; k++)
                if (br[k] != cur) others[no++] = br[k];
            L("  merge ");
            put_branch(others[qa_below(&R, no)]);
        } else {
            L("  commit id: \"%s %d\"", w(), i);
            if (qa_random(&R) < .15) L(" tag: \"v%d\"", i);
            L("\n");
        }
    }
}

static void kanban(void) {
    L("kanban\n");
    for (long c = 0, nc = ri(3, 5); c < nc; c++) {
        L("  col%ld[%s]\n", c, w());
        for (long t = 0, nt = ri(1, 5); t < nt; t++) {
            const char *x = w(), *y = w();
            L("    t%ld_%ld[%s %s %s]", c, t, x, y, w());
            if (qa_random(&R) < .4) L("@{ assigned: \"%s\", priority: \"High\" }", w());
            L("\n");
        }
    }
}

static void timeline(void) {
    const char *x = w();
    L("timeline\n  title %s %s\n", x, w());
    for (long i = 0, n = ri(4, 9); i < n; i++) {
        L("  %ld : %s", 2000 + i, w());
        for (long k = ri(0, 3); k > 0; k--) L(" : %s", w());
        L("\n");
    }
}

static void pie(void) {
    L("pie title %s\n", w());
    for (long i = 0, n = ri(3, 10); i < n; i++) {
        const char *x = w();
        L("  \"%s %ld\" : %ld\n", x, i, ri(1, 50));
    }
}

static void gantt(void) {
    static const char *const kind[] = {"", "milestone, ", "crit, "};
    L("gantt\n  dateFormat YYYY-MM-DD\n  title %s\n", w());
    for (long i = 0, ns = ri(2, 4); i < ns; i++) {
        L("  section %s\n", w());
        for (long j = 0, nt = ri(2, 5); j < nt; j++) {
            const char *x = w(), *k = qa_choice(&R, kind);
            long month = ri(1, 6), day = ri(10, 28), days = ri(0, 20);
            L("  %s %ld%ld :%s2024-0%ld-%ld, %ldd\n", x, i, j, k, month, day, days);
        }
    }
}

static void venn(void) {
    L("venn-beta\n  title %s\n", w());
    for (long i = 0, n = ri(2, 3); i < n; i++) L("  set %c[\"%s\"]\n", (char)('A' + i), w());
    L("  union A,B[\"%s\"]\n", w());
}

static void em(void) {
    static const char *const kind[] = {"cmd", "evt", "rmo", "ui"};
    L("eventmodeling\n");
    for (long i = 0, n = ri(6, 14); i < n; i++) {
        L("tf %02ld %s ", i + 1, qa_choice(&R, kind));
        put_word(&o, w(), true, "");
        L("%ld\n", i);
    }
}

static void journey(void) {
    L("journey\n  title %s\n", w());
    for (int i = 0; i < 3; i++) {
        L("  section %s\n", w());
        for (long j = 0, n = ri(2, 4); j < n; j++) {
            const char *x = w(), *y = w();
            L("    %s %s: %ld: Me, ", x, y, ri(1, 5));
            put_word(&o, w(), true, " ");
            L("\n");
        }
    }
}

static void quadrant(void) {
    L("quadrantChart\n  title %s\n  x-axis Low --> High\n  y-axis Low --> High\n", w());
    for (long i = 0, n = ri(5, 15); i < n; i++) {
        const char *x = w();
        double p = qa_random(&R), q = qa_random(&R);
        L("  %s %ld: [%.2f, %.2f]\n", x, i, p, q);
    }
}

static void treemap(void) {
    L("treemap-beta\n");
    for (long i = 0, n = ri(2, 4); i < n; i++) {
        L("\"%s %ld\"\n", w(), i);
        for (long j = 0, nj = ri(1, 5); j < nj; j++) {
            const char *x = w();
            L("    \"%s %ld%ld\": %ld\n", x, i, j, ri(1, 100));
        }
    }
}

static void c4(void) {
    static const char *const kind[] = {"Person", "System", "System_Ext", "SystemDb"};
    long n = ri(5, 12);
    L("C4Context\n  title %s\n", w());
    for (long i = 0; i < n; i++) {
        const char *k = qa_choice(&R, kind), *x = w(), *y = w();
        L("  %s(e%ld, \"%s %ld\", \"%s %s\")\n", k, i, x, i, y, w());
    }
    if (qa_random(&R) < .6) {
        L("  Enterprise_Boundary(b0, \"%s\") {\n", w());
        for (long i = 0, nb = ri(1, 3); i < nb; i++) L("    System(eb%ld, \"%s\")\n", i, w());
        L("  }\n");
    }
    for (long k = 0; k < n; k++) {
        long a = rr(n), b = rr(n);
        L("  Rel(e%ld, e%ld, \"%s\")\n", a, b, w());
    }
}

static void wardley(void) {
    long n = ri(6, 14);
    L("wardley-beta\ntitle %s\n", w());
    char **names = qa_alloc((size_t)n * sizeof *names);
    for (long i = 0; i < n; i++) {
        qa_buf b = {};
        put_word(&b, w(), true, " ");
        qa_putf(&b, " %ld", i);
        names[i] = b.p;
    }
    for (long i = 0; i < n; i++) {
        double p = qa_uniform(&R, .05, .95), q = qa_uniform(&R, .05, .95);
        L("component %s [%.2f, %.2f]\n", names[i], p, q);
    }
    for (long k = 0; k < n; k++) {
        const char *a = names[rr(n)];
        L("%s -> %s\n", a, names[rr(n)]);
    }
}

static void sankey(void) {
    long n = ri(5, 12);
    L("sankey-beta\n");
    const char **names = qa_alloc((size_t)n * sizeof *names);
    for (long i = 0; i < n; i++) names[i] = w();
    for (long k = 0; k < 2 * n; k++) {
        long a = rr(n - 1), b = a + 1 + rr(n - a - 1);
        L("\"%s %ld\",\"%s %ld\",%.1f\n", names[a], a, names[b], b, qa_uniform(&R, 1, 100));
    }
}

static void requirement(void) {
    static const char *const rel[] = {"satisfies", "traces", "contains", "derives", "verifies"};
    long n = ri(3, 7);
    L("requirementDiagram\n");
    for (long i = 0; i < n; i++) {
        const char *x = w(), *y = w();
        L("  requirement r%ld {\n    id: %ld\n    text: %s %s %s\n    risk: high\n    verifymethod: test\n  }\n", i, i, x, y, w());
    }
    for (long i = 0, ne = ri(1, 3); i < ne; i++) L("  element el%ld {\n    type: %s\n  }\n", i, w());
    for (long k = 0; k < n + 2; k++) {
        long r = rr(n);
        bool el = qa_below(&R, 2); /* choice([f"r{r}", "el0"]) */
        const char *x = qa_choice(&R, rel);
        long b = rr(n);
        if (el) L("  el0 - %s -> r%ld\n", x, b);
        else L("  r%ld - %s -> r%ld\n", r, x, b);
    }
}

static void arch(void) {
    static const char *const kind[] = {"database", "server", "disk", "internet", "cloud"}, *const edge[] = {"--", "-->"};
    static const char side[] = {'L', 'R', 'T', 'B'};
    long n = ri(5, 10);
    L("architecture-beta\n  group g0(cloud)[%s]\n", w());
    for (long i = 0; i < n; i++) {
        const char *k = qa_choice(&R, kind);
        L("  service s%ld(%s)[%s]", i, k, w());
        if (qa_random(&R) < .4) L(" in g0");
        L("\n");
    }
    for (long k = 0; k < n; k++) {
        uint32_t ab[2];
        qa_sample(&R, (uint32_t)n, 2, ab);
        char x = qa_choice(&R, side);
        const char *e = qa_choice(&R, edge);
        L("  s%u:%c %s %c:s%u\n", ab[0], x, e, qa_choice(&R, side), ab[1]);
    }
}

static long number(const char *s) {
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s || *end) qa_die("not a whole number: %s", s);
    return v;
}

static void stress(int argc, char *argv[]) {
    static const struct { const char *name; void (*fn)(void); } types[] = {
        {"flow", flow}, {"cls", cls}, {"state", state}, {"er", er}, {"seq", seq}, {"block", block},
        {"mind", mind}, {"git", gitg}, {"kanban", kanban}, {"timeline", timeline}, {"pie", pie},
        {"gantt", gantt}, {"venn", venn}, {"em", em}, {"journey", journey}, {"quadrant", quadrant},
        {"treemap", treemap}, {"c4", c4}, {"wardley", wardley}, {"sankey", sankey}, {"req", requirement},
        {"arch", arch},
    };
    if (argc < 1) qa_die("usage: gen stress OUTDIR [per_type=3] [seed=1] [scale=1]");
    long per = argc > 1 ? number(argv[1]) : 3;
    qa_seed(&R, argc > 2 ? (uint32_t)number(argv[2]) : 1);
    scale = 1;
    if (argc > 3) {
        char *end;
        scale = strtod(argv[3], &end);
        if (end == argv[3] || *end) qa_die("not a number: %s", argv[3]);
    }
    char *dir = qa_from_cwd(argv[0]);
    qa_mkdirs(dir);
    for (size_t t = 0; t < countof(types); t++)
        for (long i = 0; i < per; i++) {
            types[t].fn();
            save(qa_fmt("%s/%s_%ld.mmd", dir, types[t].name, i));
        }
}

int main(int argc, char *argv[]) {
    qa_at_root(0, argv);
    const char *cmd = argc > 1 ? argv[1] : "";
    if (!strcmp(cmd, "bench") && argc <= 3) bench(argc > 2 ? qa_from_cwd(argv[2]) : "bench/inputs");
    else if (!strcmp(cmd, "flow") && argc <= 3) flow_corpus(argc > 2 ? qa_from_cwd(argv[2]) : "tests/flow");
    else if (!strcmp(cmd, "stress") && argc <= 6) stress(argc - 2, argv + 2);
    else qa_die("usage: gen bench [DIR] | gen flow [DIR] | gen stress OUTDIR [per_type=3] [seed=1] [scale=1]");
}
