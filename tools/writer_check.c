/* writer_check: cross-checks the writer against mermaid.js. build/test-writer --dump writes a corpus of
 * every typed helper and escape context fed with hostile labels and ids; each file must parse in
 * mermaid (rendered with qa_mermaid, as build/docs status does) and its SVG must show every intended
 * label (whitespace ignored). See docs/dev/writer.md.
 *
 *   build/writer_check [-v] [--seeds=N] [test-writer binary]   (default build/test-writer)
 *
 * --seeds=N checks N corpora (each ~400 diagrams, ~12 s) instead of the default one; -v shows three
 * sources per failing slot instead of one, and the text mermaid drew. Exit status 1 when a diagram is
 * rejected or misses a label; 0, skipped, when mermaid-cli is not installed (see tools/qa/qa.h). */
#define _GNU_SOURCE
#include "qa/qa.h"

#include <ctype.h>
#include <locale.h>
#include <stdlib.h>
#include <wctype.h>

/* mermaid limits the writer can't escape around (docs/dev/writer.md): these slots are not checked */
static const char *const SKIP[] = {
    "zenuml",             /* the zenuml plugin prints entities literally */
    "C4 ",                /* mermaid prints entities literally */
    "architecture ",      /* mermaid prints entities literally, and lexes only a few characters */
    "wardley component",  /* the lexer takes no entity and few characters */
    "eventmodeling name", /* ASCII names starting with a letter only */
    "usecase id",         /* ASCII ids only */
    "gitGraph branch",    /* ASCII names only (mmd_git_* check that) */
    "xychart series",     /* mermaid leaves out some series names ("::x::", after a category it escapes) */
};
/* ... and labels mermaid turns into markup whatever the escape: such a diagram only has to parse (the
 * markup can move the other labels too: xychart drops its series names after a "<br>" category) */
static const char *const DRAWN[] = {"<br>", "fa:fa-"};
/* markdown (and state's <br> handling): a decoded '\\' escapes what follows */
static const char *const MD[] = {"class note", "mindmap", "ER", "state"};

static bool starts(const char *s, const char *prefix) { return !strncmp(s, prefix, strlen(prefix)); }

/* ---- the text as Python saw it: code points, its \s and \w, html.unescape() ---- */
static uint32_t next_cp(const char **p) {
    const unsigned char *s = (const unsigned char *)*p;
    uint32_t c = s[0];
    size_t n = c < 0xc0 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    if (n > 1) c &= 0x3fu >> (n - 1);
    size_t i = 1;
    for (; i < n && (s[i] & 0xc0) == 0x80; i++) c = c << 6 | (s[i] & 0x3fu);
    *p += i;
    return c;
}

static void put_cp(qa_buf b[static 1], uint32_t c) {
    char u[4];
    size_t n = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
    if (n == 1) u[0] = (char)c;
    else {
        for (size_t i = n - 1; i; i--, c >>= 6) u[i] = (char)(0x80 | (c & 0x3f));
        u[0] = (char)((0xf00u >> n) | c);
    }
    qa_put(b, n, u);
}

/* Python's str.isspace(), which re's \s matches */
static bool py_space(uint32_t c) {
    return (c >= 9 && c <= 13) || (c >= 0x1c && c <= 0x20) || c == 0x85 || c == 0xa0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000;
}

/* s without whitespace. qa_mermaid's XML has U+00A0 where mermaid's own (HTML) serialisation, which
 * the checks were written against, has &nbsp;: keep_nbsp keeps it for the text still to be decoded. */
static char *drop_space(const char *s, bool keep_nbsp) {
    qa_buf b = {};
    qa_put(&b, 0, "");
    for (const char *p = s; *p;) {
        const char *at = p;
        uint32_t c = next_cp(&p);
        if (!py_space(c) || (keep_nbsp && c == 0xa0)) qa_put(&b, (size_t)(p - at), at);
    }
    return b.p;
}

/* s without the characters in del */
static char *drop(const char *s, const char *del) {
    qa_buf b = {};
    qa_put(&b, 0, "");
    for (; *s; s++)
        if (!strchr(del, *s)) qa_put(&b, 1, s);
    return b.p;
}

/* s with each `from` replaced by `to` */
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

/* What html.unescape() makes of a character reference: the numeric ones in full (Python's mapping of
 * the windows-1252 range, U+FFFD, the code points it drops), and of the named ones those the text can
 * hold: the &amp; &lt; &gt; of the serialisation, and below that what mermaid leaves of an escape it
 * did not decode, which is all numeric: &quot; &apos; &nbsp; for good measure, none of the
 * semicolon-less legacy forms (their letters do not follow a '&' in the corpus). */
static void put_charref(qa_buf b[static 1], uint32_t num) {
    static const uint16_t cp1252[32] = {
        0x20ac, 0x81,   0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160,
        0x2039, 0x0152, 0x8d,   0x017d, 0x8f,   0x90,   0x2018, 0x2019, 0x201c, 0x201d, 0x2022,
        0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x9d,   0x017e, 0x0178,
    };
    if (num == 0 || (num >= 0xd800 && num <= 0xdfff) || num > 0x10ffff) put_cp(b, 0xfffd);
    else if (num >= 0x80 && num <= 0x9f) put_cp(b, cp1252[num - 0x80]);
    else if (num == 0xd) put_cp(b, num);
    else if ((num >= 1 && num <= 8) || num == 0xb || (num >= 0xe && num <= 0x1f) || num == 0x7f ||
             (num >= 0xfdd0 && num <= 0xfdef) || (num & 0xfffe) == 0xfffe) return;
    else put_cp(b, num);
}

static char *unescape(const char *s) {
    static const struct { const char *name, *text; } NAMED[] = {
        {"amp;", "&"}, {"lt;", "<"}, {"gt;", ">"}, {"quot;", "\""}, {"apos;", "'"}, {"nbsp;", "\u00a0"},
    };
    qa_buf b = {};
    qa_put(&b, 0, "");
    while (*s) {
        size_t n = strcspn(s, "&");
        qa_put(&b, n, s);
        if (!*(s += n)) break;
        const char *p = s + 1;
        if (*p == '#') {
            bool hex = p[1] == 'x' || p[1] == 'X';
            const char *d = p + 1 + hex, *e = d;
            uint32_t num = 0;
            for (; hex ? isxdigit((unsigned char)*e) : isdigit((unsigned char)*e); e++) {
                uint32_t v = isdigit((unsigned char)*e) ? (uint32_t)(*e - '0') : (uint32_t)(tolower(*e) - 'a' + 10);
                num = num > 0x10ffff ? num : num * (hex ? 16 : 10) + v;
            }
            if (e > d) {
                put_charref(&b, num);
                s = e + (*e == ';');
                continue;
            }
        } else {
            size_t i = 0;
            while (i < countof(NAMED) && !starts(p, NAMED[i].name)) i++;
            if (i < countof(NAMED)) {
                qa_puts(&b, NAMED[i].text);
                s = p + strlen(NAMED[i].name);
                continue;
            }
        }
        qa_put(&b, 1, s++);
    }
    return b.p;
}

/* re's \w on str: alphanumerics in Unicode's sense, and '_' */
static bool py_word(uint32_t c) { return c == '_' || iswalnum((wint_t)c); }

/* re.sub(r'<style[^>]*>.*?</style>', '', svg, flags=re.S), then re.sub(r'<[^>]+>', '', ...) */
static char *strip_tags(const char *svg) {
    qa_buf b = {};
    qa_put(&b, 0, "");
    for (const char *s = svg; *s;) {
        const char *gt = s[0] == '<' ? strchr(s + 1, '>') : nullptr, *end;
        if (gt && starts(s, "<style") && (end = strstr(gt, "</style>"))) s = end + strlen("</style>");
        else if (gt && gt > s + 1) s = gt + 1;
        else qa_put(&b, 1, s++);
    }
    return b.p;
}

/* mermaid hyphenates long sequence and data labels, at times inside an escape it then leaves
 * undecoded; it also shows '<' '>' as &lt; &gt; in gitGraph and xychart */
static char *lenient(const char *raw) {
    const char *s = replace(replace(drop(raw, "-"), "&amp;gt;", ">"), "&amp;lt;", "<");
    /* re.sub(r'\ufb02\u00b0\u00b0(\d+)(?:\u00b6\u00df|;)', chr(digits), ...): mermaid's placeholder */
    static const char ph[] = "\ufb02\u00b0\u00b0", end[] = "\u00b6\u00df";
    qa_buf b = {};
    qa_put(&b, 0, "");
    for (const char *f; (f = strstr(s, ph)); s = f) {
        qa_put(&b, (size_t)(f - s), s);
        f += strlen(ph);
        const char *d = f;
        uint32_t num = 0;
        for (; isdigit((unsigned char)*f); f++) num = num > 0x10ffff ? num : num * 10 + (uint32_t)(*f - '0');
        if (f > d && num <= 0x10ffff && (*f == ';' || starts(f, end))) {
            put_cp(&b, num);
            f += *f == ';' ? 1 : strlen(end);
        } else qa_puts(&b, ph), f = d;
    }
    qa_puts(&b, s);
    return drop_space(unescape(unescape(replace(b.p, end, ""))), false);
}

/* re.search('[\x00-\x08\x0b-\x1f\ufb02\u00b0\u00b6]|#\\w*-|-;', raw): an escape mermaid hyphenated
 * ("&#5-8;", its placeholder "\ufb02\u00b0\u00b058\u00b6\u00df") */
static bool garbled(const char *raw) {
    if (strstr(raw, "-;")) return true;
    for (const char *p = raw; *p;) {
        uint32_t c = next_cp(&p);
        if ((c < 0x20 && c != '\t' && c != '\n') || c == 0xfb02 || c == 0xb0 || c == 0xb6) return true;
        if (c != '#') continue;
        const char *q = p;
        while (*q && (c = next_cp(&q), py_word(c))) {}
        if (c == '-') return true;
    }
    return false;
}

/* Python's repr() of a str; printable is str.isprintable() for the characters these texts hold:
 * controls, the space separators but ' ', soft hyphen, formatting characters and private use not */
static void put_repr(qa_buf b[static 1], const char *s) {
    char q = strchr(s, '\'') && !strchr(s, '"') ? '"' : '\'';
    qa_put(b, 1, &q);
    for (const char *p = s; *p;) {
        const char *at = p;
        uint32_t c = next_cp(&p);
        if (c == '\\' || c == (uint32_t)q) qa_putf(b, "\\%c", (char)c);
        else if (c == '\t') qa_puts(b, "\\t");
        else if (c == '\n') qa_puts(b, "\\n");
        else if (c == '\r') qa_puts(b, "\\r");
        else if (c < 0x20 || (c >= 0x7f && c <= 0xa0) || c == 0xad) qa_putf(b, "\\x%02x", c);
        else if (c == 0x1680 || (c >= 0x2000 && c <= 0x200f) || (c >= 0x2028 && c <= 0x202f) ||
                 (c >= 0x205f && c <= 0x206f) || c == 0x3000 || (c >= 0xd800 && c <= 0xf8ff) || c == 0xfeff ||
                 (c >= 0xfff9 && c <= 0xfffb))
            qa_putf(b, "\\u%04x", c);
        else qa_put(b, (size_t)(p - at), at);
    }
    qa_put(b, 1, &q);
}

static void put_list(qa_buf b[static 1], const qa_strs l[static 1]) {
    qa_puts(b, "[");
    for (size_t i = 0; i < l->n; i++) {
        if (i) qa_puts(b, ", ");
        put_repr(b, l->v[i]);
    }
    qa_puts(b, "]");
}

/* ---- the check ---- */
typedef struct bad { const char *kind, *name, *src, *why; size_t seq; } bad;

static int by_slot(const void *x, const void *y) {
    const bad *a = x, *b = y;
    int c = strcmp(b->kind, a->kind); /* REJECTED first */
    if (!c) c = strcmp(a->name, b->name);
    return c ? c : (a->seq > b->seq) - (a->seq < b->seq);
}

int main(int argc, char *argv[]) {
    qa_at_root(argc, argv);
    setlocale(LC_CTYPE, "C.UTF-8"); /* iswalnum() */
    bool verbose = false;
    const char *exe = nullptr;
    long seeds = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) verbose = true;
        else if (starts(argv[i], "--seeds=") && !seeds) {
            char *end;
            seeds = strtol(argv[i] + 8, &end, 10);
            if (*end || seeds < 1) qa_die("bad %s", argv[i]);
        } else if (argv[i][0] != '-' && !exe) exe = argv[i];
    }
    if (!exe) exe = "build/test-writer";
    if (!seeds) seeds = 1;

    char *tmp = qa_tmpdir("wcheck"); /* left behind for a look when the harness dies */
    for (long k = 0; k < seeds; k++) {
        char *d = qa_fmt("%s/%ld", tmp, k), *seed = qa_fmt("%ld", k);
        qa_mkdirs(d);
        qa_res r = qa_exec(exe, "--dump", d, seed);
        if (r.code) qa_die("test-writer --dump failed\n%s", r.out + (r.out_len > 2000 ? r.out_len - 2000 : 0));
        qa_res_free(&r);
    }
    qa_strs files = {};
    qa_glob(&files, qa_fmt("%s/*/*.mmd", tmp));
    qa_render *res = qa_alloc((files.n + 1) * sizeof *res);
    if (!qa_mermaid(qa_fmt("%s/svg", tmp), nullptr, files.n, (const char *const *)files.v, res)) {
        puts("writer_check: mermaid-cli not found, skipped");
        qa_rmtree(tmp);
        return 0;
    }

    bad *bads = nullptr;
    size_t nbad = 0, cap = 0, skipped = 0, wrapped = 0;
    for (size_t i = 0; i < files.n; i++) {
        size_t len;
        char *path = qa_fmt("%.*s.want", (int)(strlen(files.v[i]) - 4), files.v[i]);
        char *text = qa_read(path, &len);
        qa_strs want = {}; /* the lines after "# NAME" */
        for (char *nl; (nl = strchr(text, '\n')); text = nl + 1) {
            *nl = 0;
            qa_push(&want, text);
        }
        if (*text) qa_push(&want, text);
        if (!want.n || !starts(want.v[0], "# ")) qa_die("%s: no slot name", path);
        const char *name = want.v[0] + 2;
        want.v++, want.n--;
        bool drawn = false;
        for (size_t j = 0; j < want.n; j++)
            for (size_t m = 0; m < countof(DRAWN); m++) drawn |= strstr(want.v[j], DRAWN[m]) != nullptr;
        if (drawn) want.n = 0;
        if (!strcmp(name, "xychart helpers")) { /* as 'xychart series' */
            size_t n = 0;
            for (size_t j = 0; j < want.n; j++) {
                size_t l = strlen(want.v[j]);
                if (l < 3 || strcmp(want.v[j] + l - 3, "Bar")) want.v[n++] = want.v[j];
            }
            want.n = n;
        }
        bool md = false, md_id = !strcmp(name, "state id") || !strcmp(name, "mindmap id"), skip = false;
        for (size_t m = 0; m < countof(MD); m++) md |= starts(name, MD[m]);
        for (size_t m = 0; m < countof(SKIP); m++) skip |= starts(name, SKIP[m]);
        if (skip) {
            skipped++;
            continue;
        }
        char *src = qa_read(files.v[i], &len);
        if (!res[i].svg) {
            qa_room(bads, nbad, cap);
            bads[nbad] = (bad){"REJECTED", name, src, strndup(res[i].err, strcspn(res[i].err, "\n")), nbad};
            nbad++;
            continue;
        }
        char *raw = drop_space(strip_tags(qa_read(res[i].svg, &len)), true);
        char *got = drop_space(unescape(raw), false), *loose = nullptr, *got_ = md_id ? drop(got, "_*") : nullptr;
        qa_strs lost = {};
        for (size_t j = 0; j < want.n; j++) {
            const char *w = want.v[j];
            if (strstr(got, w)) continue;
            if (!loose) loose = lenient(raw);
            if (strstr(loose, drop(w, "-")) || (md && strstr(got, drop(w, "\\"))) ||
                (md_id && strstr(got_, drop(w, "_*"))))
                continue;
            qa_push(&lost, want.v[j]);
        }
        if (lost.n && garbled(raw)) {
            wrapped++;
            continue;
        }
        if (lost.n) {
            qa_buf why = {};
            put_list(&why, &lost);
            if (verbose) {
                const char *cut = got;
                for (size_t k = 0; k < 300 && *cut; k++) next_cp(&cut);
                qa_puts(&why, " in ");
                put_repr(&why, strndup(got, (size_t)(cut - got)));
            }
            qa_room(bads, nbad, cap);
            bads[nbad] = (bad){"MISSING", name, src, why.p, nbad};
            nbad++;
        }
    }
    if (nbad) qsort(bads, nbad, sizeof *bads, by_slot);
    for (size_t i = 0, j; i < nbad; i = j) {
        for (j = i; j < nbad && !strcmp(bads[j].kind, bads[i].kind) && !strcmp(bads[j].name, bads[i].name); j++) {}
        printf("%s %s: %zu\n", bads[i].kind, bads[i].name, j - i);
        for (size_t k = i; k < j && k < i + (verbose ? 3 : 1); k++) {
            qa_buf src = {};
            qa_puts(&src, "");
            for (const char *s = bads[k].src; *s; s++)
                if (*s == '\n') qa_puts(&src, "\n    ");
                else qa_put(&src, 1, s);
            while (src.n && isspace((unsigned char)src.p[src.n - 1])) src.p[--src.n] = 0;
            printf("    %s \n    %s\n", bads[k].why, src.p);
        }
    }
    printf("writer vs mermaid.js: %zu diagrams (%zu in mermaid-limited slots skipped), %zu rejected or misrendered",
           files.n - skipped, skipped, nbad);
    if (wrapped) printf(", %zu garbled by mermaid's hyphenation", wrapped);
    putchar('\n');
    qa_rmtree(tmp);
    return nbad != 0;
}
