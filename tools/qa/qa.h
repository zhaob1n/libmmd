/* What the test, benchmark and developer programs share. They are tools, not library code: running
 * out of memory, or a harness that cannot do its job, ends the program (qa_die). */
#ifndef MMD_QA_H
#define MMD_QA_H
#include <stdbool.h>
#include <stdcountof.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

[[noreturn, gnu::format(printf, 1, 2)]] void qa_die(const char *fmt, ...); /* "prog: message", exit 2 */
[[nodiscard, gnu::returns_nonnull]] void *qa_alloc(size_t size);            /* zeroed */
[[nodiscard, gnu::returns_nonnull]] void *qa_realloc(void *p, size_t size);
[[nodiscard, gnu::format(printf, 1, 2)]] char *qa_fmt(const char *fmt, ...); /* malloc'd */
/* room for one more element in the array v of n elements and capacity cap */
#define qa_room(v, n, cap) \
    ((n) < (cap) ? (void)0 : (void)((v) = qa_realloc((v), ((cap) = (cap) ? 2 * (cap) : 16) * sizeof *(v))))

/* ---- bytes ---- */
typedef struct qa_buf { char *p; size_t n, cap; } qa_buf; /* p[n] is NUL once anything was put */
void qa_put(qa_buf b[static 1], size_t n, const char s[n]);
static inline void qa_puts(qa_buf b[static 1], const char *s) { qa_put(b, strlen(s), s); }
[[gnu::format(printf, 2, 3)]] void qa_putf(qa_buf b[static 1], const char *fmt, ...);
/* s with the five XML specials as entities */
void qa_put_xml(qa_buf b[static 1], const char *s);
/* FNV-1a: tells two outputs apart, nothing more */
uint64_t qa_hash(size_t n, const char s[n]);

typedef struct qa_strs { char **v; size_t n, cap; } qa_strs; /* v[n] is nullptr once anything was pushed */
void qa_push(qa_strs s[static 1], char *x);

/* ---- files ---- */
[[nodiscard]] char *qa_read(const char *path, size_t len[static 1]);     /* malloc'd, NUL after len */
[[nodiscard]] char *qa_try_read(const char *path, size_t len[static 1]); /* nullptr when it cannot be opened */
void qa_write(const char *path, size_t n, const char data[n]);
void qa_mkdirs(const char *path);
/* appends what the pattern matches, sorted bytewise; the strings are malloc'd */
void qa_glob(qa_strs out[static 1], const char *pattern);
/* $TMPDIR/mmd-UID/name, created: caches and output worth a look survive the run. malloc'd. */
[[nodiscard]] char *qa_scratch(const char *name);
/* a fresh directory under qa_scratch(name), for the caller to qa_rmtree(). malloc'd. */
[[nodiscard]] char *qa_tmpdir(const char *name);
void qa_rmtree(const char *path);
/* Makes arguments that name existing files absolute, then changes to the repository root, so a
 * program runs the same from any directory. */
void qa_at_root(int argc, char *argv[]);
/* A path the user gave for something to create, as they meant it: relative to where the program was
 * started. malloc'd. */
[[nodiscard]] char *qa_from_cwd(const char *path);

/* ---- children ---- */
typedef struct qa_cmd {
    const char *const *argv; /* nullptr-terminated; argv[0] is looked up on PATH unless it has a '/' */
    const char *in;          /* stdin, in_len bytes; nullptr: /dev/null */
    size_t in_len;
    unsigned timeout;        /* seconds, 0 = none; SIGALRM ends the child when they are over */
} qa_cmd;
typedef struct qa_res {
    int code;                /* exit status, or minus the signal that ended it */
    char *out, *err;         /* malloc'd, NUL after the length */
    size_t out_len, err_len;
} qa_res;
/* A program that cannot be executed is a broken harness, not a result: qa_run dies. */
qa_res qa_run(const qa_cmd c[static 1]);
void qa_res_free(qa_res r[static 1]);
#define QA_ARGV(...) ((const char *const[]){__VA_ARGS__, nullptr})
#define qa_exec(...) qa_run(&(qa_cmd){.argv = QA_ARGV(__VA_ARGS__)})
/* where PATH has the program; nullptr when it does not. malloc'd. */
[[nodiscard]] char *qa_which(const char *name);

/* fn(i, ctx) for every i in [0, n), on one thread per core ($QA_JOBS overrides) */
void qa_parallel(size_t n, void (*fn)(size_t i, void *ctx), void *ctx);

/* ---- Python's random.Random ----
 * Bit for bit, so generated inputs and fuzz seeds mean what they meant when the generators were
 * Python scripts. Each function is the method of that name. */
typedef struct qa_rng { uint32_t mt[624]; unsigned i; } qa_rng;
void qa_seed(qa_rng r[static 1], uint32_t seed);
double qa_random(qa_rng r[static 1]);
uint32_t qa_below(qa_rng r[static 1], uint32_t n); /* randrange(n); n > 0 */
static inline long qa_randint(qa_rng r[static 1], long a, long b) { return a + (long)qa_below(r, (uint32_t)(b - a + 1)); }
static inline double qa_uniform(qa_rng r[static 1], double a, double b) { return a + (b - a) * qa_random(r); }
/* choice() of an actual array */
#define qa_choice(r, arr) ((arr)[qa_below((r), (uint32_t)countof(arr))])
/* sample(range(n), k): k distinct indices; k <= n */
void qa_sample(qa_rng r[static 1], uint32_t n, uint32_t k, uint32_t out[static k]);

/* ---- XML (xml.c) ---- */
typedef struct xml_attr { const char *name, *value; } xml_attr; /* value with its references decoded */
typedef struct xml_node xml_node;
struct xml_node {
    const char *name;                /* an element's name as written, prefix included; nullptr for text */
    const char *text;                /* character data and CDATA, decoded; nullptr for an element */
    xml_attr *attrs;
    size_t nattrs;
    xml_node *parent, *child, *next; /* first child, next sibling */
};
typedef struct xml_doc { xml_node *root; struct xml_block *blocks; } xml_doc;
/* Parses a document that is well-formed by XML 1.0 without a DTD: one root, matching tags, quoted and
 * unique attributes, only the five predefined entities and character references, legal characters in
 * valid UTF-8. Anything else returns nullptr and says why, with line and column, in err. */
[[nodiscard]] xml_doc *xml_parse(size_t n, const char src[static n], char err[static 128]);
void xml_free(xml_doc *d);
const char *xml_get(const xml_node e[static 1], const char *name); /* attribute value; nullptr when absent */
bool xml_is(const xml_node *e, const char *name);                  /* an element with this name */
bool xml_has_class(const xml_node e[static 1], const char *cls);  /* cls is one of the words of class="" */
/* The node after n in document order, inside top's subtree; nullptr after the last. Start with top. */
xml_node *xml_next(const xml_node top[static 1], const xml_node n[static 1]);
/* the same, without entering n's children */
xml_node *xml_skip(const xml_node top[static 1], const xml_node n[static 1]);
/* appends the text below e: each run trimmed, runs joined by sep */
void xml_text(const xml_node e[static 1], const char *sep, qa_buf out[static 1]);

/* ---- mermaid.js and Chrome ----
 * mermaid-cli, its puppeteer config and Chrome come from $MMDC, $MMDC_PUPPETEER and $CHROME, else
 * from PATH (tools/mermaid_render.mjs). */
/* Renders files[i] with mermaid.js itself, mermaid.initialize() given `config` (JSON; nullptr for
 * none). Results stay in dir and are reused while newer than their source. The SVGs name mermaid-cli's
 * fonts without carrying them: dir/mmdc.css imports the stylesheets a page needs to show them as
 * mermaid-cli does. False when mermaid-cli is not installed. */
typedef struct qa_render {
    char *svg; /* path of the SVG, serialised as XML; nullptr where mermaid failed */
    char *err; /* then: why */
} qa_render;
[[nodiscard]] bool qa_mermaid(const char *dir, const char *config, size_t n, const char *const files[static n], qa_render out[static n]);
/* The config under which mermaid draws what libmmd draws. mermaid 12 picks another look and, for
 * some types, another layout engine by default; a source that names its own still wins. */
#define QA_CLASSIC "{\"look\":\"classic\",\"layout\":\"dagre\",\"theme\":\"default\"}"
const char *qa_chrome(void);
/* width and height of the root <svg>'s viewBox; false when it has none */
bool qa_svg_size(const char *svg, double w[static 1], double h[static 1]);
/* Headless Chrome on a local page: a w x h screenshot, or the DOM once its scripts ran (malloc'd). */
void qa_screenshot(const char *page, unsigned w, unsigned h, const char *png);
[[nodiscard]] char *qa_dump_dom(const char *page);

#endif
