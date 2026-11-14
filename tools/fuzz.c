/* fuzz: mutation fuzzer. Feeds mutated corpus files to a (sanitizer) build of the CLI on stdin and
 * reports what crashed it.
 * usage: build/fuzz [BIN=build-asan/mmd] [N=1000] [SEED=1]
 * FUZZ_GLOB picks the seed corpus, relative to the repository root (default: the .mmd files of
 * tests/cases), taken in sorted order. A crash is an exit status other than 0 and 1 (death by a signal
 * included) or a sanitizer report on stderr. Each crashing input is saved under $TMPDIR and printed as
 *   CRASH path status first-300-bytes-of-stderr
 * with the stderr bytes escaped (\n, \t, \r, \\, \xHH) to keep one line per crash.
 * The last line is "N cases, K crashes"; the exit status is 1 when K > 0.
 * The mutations are those of the Python fuzzer this replaces (random.Random), seed for seed. */
#include "qa/qa.h"

#include <unistd.h>

typedef struct tok { const char *s; size_t n; } tok;
#define T(s) {s, sizeof s - 1}
static const tok toks[] = {
    T("-->"), T("---"), T("-.-"), T("==>"), T("["), T("]"), T("("), T(")"), T("{"), T("}"), T("|"), T("\""),
    T("subgraph x"), T("end"), T("\n"), T("&"), T(":::"), T("%%"), T("<br>"), T("@{"), T("->>"),
    T("Note over A"), T("loop"), T("#"), T(";"), T("\344\275\240"), T("\377"), T("[*]"), T("state "),
    T("<<fork>>"), T("--"), T("note left of "), T("end note"), T("class "), T("<|--"), T("*--"), T("o--"),
    T("..>"), T("()--"), T("~T~"), T("namespace N {"), T("||--o{"), T("}|..|{"), T(" to "), T("PK, FK"),
    T("\"x\" : "), T("showData"), T("create participant "), T("destroy "), T("box Aqua G"), T("par_over"),
    T("@{ \"type\": \"queue\" }"), T("@{ \"type\": \"database\" }"), T("\"type\": \"boundary\""),
    T("collections"), T("wardley-beta"), T("anchor "), T("component "), T("pipeline P {"), T("evolve "),
    T("(inertia)"), T("+<>"), T("+'l'>"), T("evolution Genesis -> Custom"), T("annotation 1,[0.1, 0.2] "),
    T("accelerator "), T("dateFormat "), T("after a1"), T("until "), T("crit, "), T("milestone, "), T(", 3d"),
    T("excludes weekends"), T("axisFormat %b"), T("section S"), T(": 5: "), T(": 0.5"), T("[0.3, 0.9]"),
    T("radius: 9"), T("x-axis "), T("y-axis "), T(" --> "), T("bar ["), T("line ["), T("horizontal"),
    T("quadrant-1 "), T("commit "), T("id: \""), T("branch b"), T("checkout "), T("merge "),
    T("cherry-pick "), T("tag: \""), T("type: REVERSE"), T("order: "), T("(("), T("))"), T("{{"),
    T("::icon(x)"), T("  "), T("\t"), T("packet-beta\n"), T("0-15: \"F\""), T("+8: "), T("axis a, b"),
    T("curve c{1,2"), T("max 10"), T("treemap-beta\n"), T("\"Leaf\": 5"), T("@{ priority: "), T("ticket: "),
    T("sankey-beta\n"), T("A,B,10\n"), T("\"q,\"\"x\","), T("columns 3"), T("space:2"), T("block:g[\"G\"]:2"),
    T("a:2"), T("id[(\"db\")]"), T("-- \"t\" -->"), T("requirement r {"), T("id: 1"), T("risk: high"),
    T(" - satisfies -> "), T(" <- copies - "), T("element e {"), T("Person(p, \"P\")"),
    T("System_Boundary(b, \"B\") {"), T("Rel(a, b, \"u\", \"t\")"), T("ContainerDb(d, \"D\", \"t\", \"x\")"),
    T("$tags="), T("service s(server)[S]"), T("group g(cloud)[G]"), T(" in g"), T("junction j"),
    T(":R -- L:"), T(":B --> T:"), T("-[lbl]-"), T("{group}"),
    T("UpdateLayoutConfig($c4ShapeInRow=\"1\", $c4BoundaryInRow=\"0\")"),
    T("UpdateElementStyle(p, $bgColor=\"red\", $fontColor="),
    T("UpdateRelStyle(a, b, $offsetX=\"-1e9\", $lineColor=\""), T("Rel_Back("), T("(logos:github)"),
    T("(logos:aws)"), T("(fa:"), T("::icon(fa fa-book)"), T("::icon(fas fa-"), T("::icon(fa-solid fa-heart)"),
    T("$bgColor=\"<&\001>\""), T("$borderColor=\"url(#a) x='y'\""), T("$textColor=\""), T("state X {\n"),
    T("\n--\n"), T("accTitle: t\n"), T("accDescr: d\n"), T("accDescr {\nx\n}\n"),
    T("%%{init: {\"themeVariables\": {\"primaryColor\": \"#f00\"}}}%%\n"),
    T("---\nconfig:\n  themeVariables:\n    background: red\n---\n"), T("zenuml\n"), T("A.m(x) {\n"),
    T("}\n"), T("} else if (c) {\n"), T("try {\n"), T("} catch (e) {\n"), T("return r\n"), T("@Database D\n"),
    T("x = new B()\n"), T("A->B: hi\n"), T("// c\n"), T("---\nconfig:\n  theme: dark\n---\n"),
    T("%%{init: {\"theme\":\"forest\"}}%%\n"),
    T("%%{init: {\"theme\":\"neutral\",\"themeVariables\":{\"pie1\":\"#f00\",\"cScale3\":\"red\",\"git2\":\"url(x)\"}}}%%\n"),
    T("%%{init: {\"timeline\":{\"disableMulticolor\":true},\"sankey\":{\"showValues\":false},\"quadrantChart\":{\"chartHeight\":-5},\"gantt\":{\"axisFormat\":\"%Y\",\"tickInterval\":\"2day\"}}}%%\n"),
    T("@Starter(A)\n"), T("r = A.m(x) {\n"), T("o = new O(p)\n"), T("B-->A: r\n"), T("while (c) {\n"),
    T("} finally {\n"), T("par {\n"), T("opt {\n"), T("A.m() // c\n"), T("title T\n"), T("@Actor \"q x\"\n"),
    T("A.A.m()\n"), T("subgraph t1\n"), T("subgraph \"x y\"\n"), T("->>()"), T("()->>"), T(" %% c"),
    T("tickInterval 1decade\n"), T("#  theme: forest\n"), T("<[\"a\"]>(right)"), T("align row "), T(":::c"),
    T("540 \"PaLM\""), T("ishikawa-beta\n"), T("cynefin-beta\n"), T("complex\n"), T("confusion\n"),
    T("clear --> chaotic : \"x\"\n"), T("    \"item\"\n"), T("venn-beta\n"), T("set A[\"x\"]:3\n"),
    T("union A,B[\"ab\"]\n"), T("    text T1[\"t\"]\n"), T("style A,B fill:#f00\n"), T("treeView-beta\n"),
    T("\342\224\234\342\224\200\342\224\200 "), T("\342\224\202   "), T(" :::highlight"),
    T(" icon(logos:react)"), T(" ## d"), T("    dir/\n"), T("swimlane-beta LR\n"), T("swimlane-beta TB\n"),
    T("  subgraph Lane\n"), T("  end\n"), T("eventmodeling\n"), T("tf 01 ui A\n"), T("rf 02 evt N.B\n"),
    T(" ->> 01"), T(" { x: 1 }"), T(" [[D]]"), T("data D {\n  a: 1\n}\n"), T("railroad-beta"),
    T("railroad-ebnf-beta"), T("railroad-abnf-beta"), T("railroad-peg-beta"), T("::="), T("<-"), T("(*"),
    T("*)"), T("oneOrMore("), T("zeroOrMore("), T("terminal("), T("nonterminal("), T("sequence("),
    T("choice("), T("optional("), T("1*"), T("%x41"),
};

typedef struct fuzz_case {
    char *in;
    size_t len;
    qa_res res;
    bool crash;
} fuzz_case;
typedef struct fuzz { const char *bin; fuzz_case *c; } fuzz;

/* replaces del bytes at p by add */
static void splice(qa_buf s[static 1], size_t p, size_t del, size_t n, const char add[n]) {
    if (n > del) s->p = qa_realloc(s->p, s->n + n - del);
    memmove(s->p + p + n, s->p + p + del, s->n - p - del);
    memcpy(s->p + p, add, n);
    s->n += n - del;
}

static void run(size_t i, void *ctx) {
    fuzz *f = ctx;
    fuzz_case *c = &f->c[i];
    c->res = qa_run(&(qa_cmd){.argv = QA_ARGV(f->bin), .in = c->in, .in_len = c->len});
    free(c->res.out);
    c->crash = (c->res.code != 0 && c->res.code != 1) || memmem(c->res.err, c->res.err_len, "ERROR: AddressSanitizer", 23) ||
               memmem(c->res.err, c->res.err_len, "runtime error", 13);
}

static unsigned long number(const char *s) {
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    if (end == s || *end) qa_die("not a whole number: %s", s);
    return v;
}

int main(int argc, char *argv[]) {
    qa_at_root(argc, argv);
    if (argc > 4) qa_die("usage: fuzz [BIN] [N] [SEED]");
    fuzz f = {.bin = argc > 1 ? argv[1] : "build-asan/mmd"};
    size_t n = argc > 2 ? number(argv[2]) : 1000;
    unsigned long seed = argc > 3 ? number(argv[3]) : 1;
    const char *pattern = getenv("FUZZ_GLOB");
    qa_strs files = {};
    qa_glob(&files, pattern ? pattern : "tests/cases/*.mmd");
    if (!files.n) qa_die("empty seed corpus");
    qa_buf *seeds = qa_alloc(files.n * sizeof *seeds);
    for (size_t i = 0; i < files.n; i++) seeds[i].p = qa_read(files.v[i], &seeds[i].n);

    /* one random stream: every case before any runs */
    qa_rng r;
    qa_seed(&r, (uint32_t)seed);
    f.c = qa_alloc(n * sizeof *f.c);
    for (size_t i = 0; i < n; i++) {
        const qa_buf *from = &seeds[qa_below(&r, (uint32_t)files.n)];
        qa_buf s = {.p = qa_alloc(from->n + 1), .n = from->n};
        memcpy(s.p, from->p, from->n);
        for (long k = qa_randint(&r, 1, 8); k > 0; k--) {
            double op = qa_random(&r);
            size_t p = (size_t)qa_randint(&r, 0, (long)s.n);
            if (op < 0.4) {
                const tok *t = &qa_choice(&r, toks);
                splice(&s, p, 0, t->n, t->s);
            } else if (op < 0.7) {
                size_t d = (size_t)qa_randint(&r, 1, 10);
                splice(&s, p, d < s.n - p ? d : s.n - p, 0, "");
            } else {
                size_t q = (size_t)qa_randint(&r, 0, (long)s.n), m = (size_t)qa_randint(&r, 1, 40);
                char copy[40];
                if (m > s.n - q) m = s.n - q;
                memcpy(copy, s.p + q, m);
                splice(&s, p, 0, m, copy);
            }
        }
        f.c[i].in = s.p, f.c[i].len = s.n;
    }
    qa_parallel(n, run, &f);

    size_t bad = 0;
    const char *tmp = getenv("TMPDIR");
    for (size_t i = 0; i < n; i++) {
        fuzz_case *c = &f.c[i];
        if (!c->crash) continue;
        char *path = qa_fmt("%s/mmd-crash%lu_%zu.XXXXXX.mmd", tmp && *tmp ? tmp : "/tmp", seed, ++bad);
        int fd = mkstemps(path, 4);
        if (fd < 0 || write(fd, c->in, c->len) != (ssize_t)c->len || close(fd) != 0) qa_die("%s: %m", path);
        printf("CRASH %s %d ", path, c->res.code);
        for (size_t k = 0; k < c->res.err_len && k < 300; k++) {
            unsigned char ch = (unsigned char)c->res.err[k];
            if (ch == '\n') fputs("\\n", stdout);
            else if (ch == '\t') fputs("\\t", stdout);
            else if (ch == '\r') fputs("\\r", stdout);
            else if (ch == '\\') fputs("\\\\", stdout);
            else if (ch < 0x20 || ch >= 0x7f) printf("\\x%02x", ch);
            else putchar(ch);
        }
        putchar('\n');
    }
    printf("%zu cases, %zu crashes\n", n, bad);
    return bad > 0;
}
