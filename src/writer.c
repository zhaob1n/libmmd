/* Mermaid source writer: a Buf with a sticky error. Labels are escaped with
 * mermaid entities (#35; ...), which every diagram type decodes on output;
 * docs/dev/writer.md has the per-diagram audit behind the escape sets. */
#include "mmd.h"

#include <math.h>
#include <stdarg.h>
#include <strings.h>

#include "base.h"

struct mmd_writer {
    Buf b;
    mmd_status st;
    size_t err_at; /* text offset where the rejected value would have gone */
    char msg[120];
    unsigned nopen; /* blocks the helpers opened: open[] says which, mmd_end() closes them */
    char open[32];
    int level; /* depth of the last mindmap / treeView node, -1 before the first */
};

mmd_writer *mmd_writer_new(void) { return calloc(1, sizeof(mmd_writer)); }

void mmd_writer_free(mmd_writer *w) {
    if (!w) return;
    free(w->b.p);
    free(w);
}

void mmd_writer_reset(mmd_writer *w) {
    if (!w) return;
    w->b.n = 0;
    w->b.oom = false;
    w->st = MMD_OK;
    w->nopen = 0;
    w->level = -1;
    if (w->b.p) w->b.p[0] = 0;
}

/* every entry point bails out once something failed, and keeps the text NUL-terminated */
static bool live(const mmd_writer *w) { return w && !w->st && !w->b.oom; }
static void term(mmd_writer w[static 1]) {
    if (w->b.p) w->b.p[w->b.n] = 0; /* buf_reserve always leaves a spare byte */
}

/* at: text offset of the rejected value */
[[gnu::format(printf, 3, 4)]] static void fail(mmd_writer *w, size_t at, const char *fmt, ...) {
    w->st = MMD_ERR_SYNTAX;
    w->err_at = w->b.n = at; /* drop what the rejected value had begun */
    term(w);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(w->msg, sizeof w->msg, fmt, ap);
    va_end(ap);
}

mmd_status mmd_writer_text(const mmd_writer *w, const char **s, size_t *len, mmd_error *err) {
    mmd_status st = !w || w->b.oom ? MMD_ERR_NOMEM : w->st;
    *s = nullptr;
    *len = 0;
    if (err) *err = (mmd_error){.code = st};
    if (st) {
        if (err && st == MMD_ERR_SYNTAX) {
            uint32_t line = 1, col = 1;
            for (size_t i = 0; i < w->err_at; i++) {
                if (w->b.p[i] == '\n') line++, col = 1;
                else col++;
            }
            err->line = line;
            err->col = col;
        }
        if (err) snprintf(err->message, sizeof err->message, "%s", st == MMD_ERR_SYNTAX ? w->msg : mmd_status_str(st));
        return st;
    }
    if (w->nopen) { /* the text would not parse */
        if (err) {
            *err = (mmd_error){.code = MMD_ERR_SYNTAX};
            snprintf(err->message, sizeof err->message, "%u block(s) still open: close them with mmd_end()", w->nopen);
        }
        return MMD_ERR_SYNTAX;
    }
    *s = w->b.p ? w->b.p : "";
    *len = w->b.n;
    return MMD_OK;
}

mmd_status mmd_writer_render_svg(const mmd_writer *w, const mmd_options *opt, mmd_buffer *out, mmd_error *err) {
    const char *s;
    size_t n;
    mmd_status st = mmd_writer_text(w, &s, &n, err);
    if (st) {
        *out = (mmd_buffer){};
        return st;
    }
    return mmd_render_svg(n, s, opt, out, err);
}

/* ---- escape contexts ---- */

enum : uint8_t { CQ = 1, CT = 2, CC = 4 }; /* quoted "…", text to end of line, CSV field */
/* extra escapes the typed helpers ask for, beside MMD_W_BR (writer.md has why) */
enum : unsigned {
    W_LEAD = 1u << 8,  /* the first non-blank character: it would start a keyword */
    W_TREE = 1u << 9,  /* treeView names: '(' (icon()), a trailing '/', box drawing (└── ├── │ draw the tree) */
    W_DIR = 1u << 10,  /* "direction LR" anywhere on a state / ER line sets the direction */
};
/* always escaped: they end or split a label somewhere, or mermaid.js drops them ('#' starts a sequence
 * or journey comment, '%' a timeline one, '\\' an escape in quoted strings, '*' markdown emphasis,
 * gitGraph shows a quoted '=' as "&equals;"; audit in docs/dev/writer.md) */
static constexpr uint8_t ESC[128] = {
    ['"'] = CQ | CT, [':'] = CQ | CT, ['['] = CQ | CT, [']'] = CQ | CT, ['{'] = CQ | CT, ['}'] = CQ | CT,
    ['|'] = CQ | CT, ['<'] = CQ | CT, ['#'] = CQ | CT, ['%'] = CQ | CT, ['\\'] = CQ | CT, ['*'] = CQ | CT, ['='] = CQ,
    ['`'] = CQ, [';'] = CT, [','] = CT, ['~'] = CT,
};

static inline bool u8_cont(unsigned char c) { return (c & 0xC0u) == 0x80u; }
/* length of the valid UTF-8 sequence at s[0], 0 if invalid; s is NUL-terminated, and each byte is
 * checked before the next is read, so this never reads past the terminator */
static size_t utf8_len(const unsigned char *s) {
    unsigned c = s[0];
    if (c >= 0xC2 && c <= 0xDF) return u8_cont(s[1]) ? 2 : 0;
    if (c >= 0xE0 && c <= 0xEF) {
        unsigned lo = c == 0xE0 ? 0xA0 : 0x80, hi = c == 0xED ? 0x9F : 0xBF;
        return s[1] >= lo && s[1] <= hi && u8_cont(s[2]) ? 3 : 0;
    }
    if (c >= 0xF0 && c <= 0xF4) {
        unsigned lo = c == 0xF0 ? 0x90 : 0x80, hi = c == 0xF4 ? 0x8F : 0xBF;
        return s[1] >= lo && s[1] <= hi && u8_cont(s[2]) && u8_cont(s[3]) ? 4 : 0;
    }
    return 0;
}

/* a letter, digit or non-ASCII byte: markdown's '_' inside one is not emphasis */
static inline bool md_word(char c) {
    unsigned char u = (unsigned char)c;
    return u >= 0x80 || (unsigned)((u | 32) - 'a') < 26u || (unsigned)(u - '0') < 10u;
}

/* byte at s[i] must become an entity in context ctx */
static bool must_esc(const char *s, size_t i, unsigned ctx, unsigned flags) {
    unsigned char c = (unsigned char)s[i];
    if (c >= 0x80) return false;
    if (ESC[c] & ctx) return true;
    if (flags & W_TREE && (c == '(' || (c == '/' && !s[i + 1]))) return true;
    if (flags & W_DIR && (c | 32) == 'd' && !strncasecmp(s + i, "direction", 9) && (s[i + 9] == ' ' || s[i + 9] == '\t'))
        return true;
    switch (c) {
    case '_': /* mindmap, ER and state text is markdown: _x_ at a word edge is emphasis */
        return ctx != CC && (!i || !md_word(s[i - 1]) || !md_word(s[i + 1]));
    case '-': return ctx == CT && (s[i + 1] == '-' || s[i + 1] == '>' || s[i + 1] == '.'); /* arrow */
    case '/': return ctx == CT && s[i + 1] == '/';                                    /* zenuml comment */
    }
    return false;
}

/* code point of the valid k-byte sequence at s */
static uint32_t utf8_cp(const unsigned char *s, size_t k) {
    uint32_t cp = k == 1 ? s[0] : s[0] & (0x7Fu >> k);
    for (size_t j = 1; j < k; j++) cp = cp << 6 | (s[j] & 0x3Fu);
    return cp;
}

/* false (and the error set) when s can't be written in ctx */
static bool put_esc(mmd_writer w[static 1], size_t at, const char *fn, const char *s, unsigned ctx, unsigned flags) {
    size_t run = 0, i = 0, lead = flags & W_LEAD ? strspn(s, " \t") : SIZE_MAX;
    for (; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        size_t k = c >= 0x80 ? utf8_len((const unsigned char *)s + i) : 1;
        if (!k) return fail(w, at, "%s: invalid UTF-8 at byte %zu", fn, i), false;
        uint32_t cp = utf8_cp((const unsigned char *)s + i, k);
        if (ctx == CC) { /* mermaid's sankey CSV: printable ASCII, no entity, no %%{ directive (even quoted);
                          * it shows < > as &lt; &gt; or drops "<x" */
            if (c >= 0x80 || c == '\t' || c == '<' || c == '>' || (c == '%' && !strncmp(s + i, "%%{", 3)) ||
                (c == '#' && ent_end(strnlen(s + i, 16), s + i, 0)))
                return fail(w, at, "%s: mermaid's sankey cannot show '%.*s' (byte %zu)", fn, (int)k, s + i, i), false;
        } else if ((i == lead && c > ' ') || (flags & W_TREE && cp >= 0x2500 && cp <= 0x257F)) {
            buf_put(&w->b, i - run, s + run);
            buf_ch(&w->b, '#');
            buf_int(&w->b, cp);
            buf_ch(&w->b, ';');
            run = i + k;
            i += k - 1;
            continue;
        }
        if (c >= 0x80) {
            i += k - 1;
            continue;
        }
        if (c < 0x20 && c != '\t') {
            if (c != '\n') return fail(w, at, "%s: control character 0x%02x at byte %zu", fn, c, i), false;
            if (!(flags & MMD_W_BR)) return fail(w, at, "%s: line break at byte %zu (MMD_W_BR where the diagram draws them)", fn, i), false;
            buf_put(&w->b, i - run, s + run);
            buf_lit(&w->b, "<br>");
            run = i + 1;
            continue;
        }
        if (ctx == CC && c == '"') { /* RFC 4180: "" inside the quoted field */
            buf_put(&w->b, i + 1 - run, s + run);
            run = i;
            continue;
        }
        if (!must_esc(s, i, ctx, flags)) continue;
        buf_put(&w->b, i - run, s + run);
        buf_ch(&w->b, '#');
        buf_int(&w->b, c);
        buf_ch(&w->b, ';');
        run = i + 1;
    }
    buf_put(&w->b, i - run, s + run);
    return true;
}

void mmd_w_line(mmd_writer *w, unsigned depth) {
    if (!live(w)) return;
    if (w->b.n && w->b.p[w->b.n - 1] != '\n') buf_ch(&w->b, '\n');
    for (unsigned d = 0; d < depth; d++) buf_lit(&w->b, "    ");
    term(w);
}

void mmd_w_raw(mmd_writer *w, const char *s) {
    if (!live(w)) return;
    buf_cstr(&w->b, s);
    term(w);
}

void mmd_w_str(mmd_writer *w, const char *s, unsigned flags) {
    if (!live(w)) return;
    size_t at = w->b.n;
    buf_ch(&w->b, '"');
    if (!put_esc(w, at, "mmd_w_str", s, CQ, flags)) return;
    buf_ch(&w->b, '"');
    term(w);
}

static bool line_kw(const mmd_writer w[static 1], const char *s);

void mmd_w_text(mmd_writer *w, const char *s, unsigned flags) {
    if (!live(w)) return;
    if (flags & MMD_W_LINESTART && line_kw(w, s)) flags |= W_LEAD;
    if (put_esc(w, w->b.n, "mmd_w_text", s, CT, flags & ~(unsigned)MMD_W_LINESTART)) term(w);
}

void mmd_w_id(mmd_writer *w, const char *s) {
    if (!live(w)) return;
    size_t n = strlen(s);
    if (!n) return fail(w, w->b.n, "mmd_w_id: empty identifier");
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 0x80) {
            size_t k = utf8_len((const unsigned char *)s + i);
            if (!k) return fail(w, w->b.n, "mmd_w_id: invalid UTF-8 at byte %zu", i);
            if (k == 4) return fail(w, w->b.n, "mmd_w_id: mermaid's ids stop at U+FFFF (byte %zu)", i); /* emoji */
            i += k - 1;
        } else if (!(c == '_' || (unsigned)(c - '0') < 10u || (unsigned)((c | 32) - 'a') < 26u))
            return fail(w, w->b.n, "mmd_w_id: '%c' (0x%02x) cannot appear in an identifier", c >= 0x20 && c < 0x7F ? c : '?', c);
    }
    buf_put(&w->b, n, s);
    term(w);
}

void mmd_w_csv(mmd_writer *w, const char *s) {
    if (!live(w)) return;
    size_t n = strlen(s);
    if (memchr(s, '\n', n)) return fail(w, w->b.n, "mmd_w_csv: sankey fields cannot hold a line break");
    bool q = !n || strpbrk(s, ",\"") || is_sp(s[0]) || is_sp(s[n - 1]) || s[0] == '%';
    size_t at = w->b.n;
    if (q) buf_ch(&w->b, '"');
    if (!put_esc(w, at, "mmd_w_csv", s, CC, 0)) return;
    if (q) buf_ch(&w->b, '"');
    term(w);
}

/* ---- numbers ---- */

/* u (< 2^63) with a '.' before its last d digits, zero-padded to one digit before the point */
static void put_fixed(Buf b[static 1], bool neg, uint64_t u, unsigned d) {
    if (neg) buf_ch(b, '-');
    size_t n0 = b->n;
    buf_int(b, (int64_t)u);
    if (!d || !buf_reserve(b, d + 2)) return;
    char *p = b->p + n0;
    size_t k = b->n - n0, lead = k <= d ? d + 1 - k : 0, t = k + lead;
    memmove(p + lead, p, k);
    memset(p, '0', lead);
    memmove(p + t - d + 1, p + t - d, d);
    p[t - d] = '.';
    b->n = n0 + t + 1;
}

/* Shortest digits g·10^q that read back as a, for 1e-4 <= a < 2^53 (bench and test data), by exact
 * integer arithmetic: a = m·2^-s; scaled by 10^k to 17 or 18 digits, the decimals that round to a
 * form the integer interval [lo, hi] (ends included when m is even: round-half-even). The multiple
 * of the largest power of ten inside it, nearest a, is the answer. */
static bool shortest(double a, uint64_t g[static 1], int q[static 1]) {
    static constexpr uint64_t P10[] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000,
                                       1000000000, 10000000000, 100000000000, 1000000000000};
    if (!(a >= 1e-4 && a < 0x1p53)) return false;
    uint64_t bits;
    memcpy(&bits, &a, sizeof bits);
    int ex = (int)(bits >> 52);
    uint64_t m = (bits & ((1ull << 52) - 1)) | 1ull << 52;
    unsigned sh = (unsigned)(1076 - ex);
    int e10 = ((ex - 1023) * 78913) >> 18; /* floor(log10 a) or one less */
    unsigned k = (unsigned)(16 - e10);       /* 1..21: products stay below 2^126 */
    unsigned __int128 P = (unsigned __int128)P10[k > 12 ? 12 : k] * P10[k > 12 ? k - 12 : 0];
    unsigned __int128 xl = (2 * m - 1) * P, xm = 2 * m * P, xh = (2 * m + 1) * P; /* ×2^-sh */
    if (m == 1ull << 52 && ex > 1) /* the gap below a power of two is half as wide */
        xl = (4 * m - 1) * P, xm *= 2, xh *= 2, sh++;
    bool incl = !(m & 1);
    unsigned __int128 lo = xl >> sh, hi = xh >> sh;
    if (lo << sh != xl || !incl) lo++;
    if (hi << sh == xh && !incl) hi--;
    uint64_t L = (uint64_t)lo, H = (uint64_t)hi, p10 = 1;
    unsigned t = 0;
    while ((L + 9) / 10 <= H / 10) L = (L + 9) / 10, H /= 10, p10 *= 10, t++;
    uint64_t x = (uint64_t)(xm >> sh), c = x / p10 + (x % p10 * 2 >= p10); /* nearest multiple */
    *g = c < L ? L : c > H ? H : c;
    *q = (int)t - (int)k;
    return true;
}

/* shortest round-trip decimal, positional notation: mermaid's number tokens have no exponent */
static void put_num(Buf b[static 1], double v) {
    if (v == 0) { buf_ch(b, '0'); return; } /* -0 too */
    double a = fabs(v);
    uint64_t g;
    int q;
    if (shortest(a, &g, &q)) {
        if (q < 0) return put_fixed(b, v < 0, g, (unsigned)-q);
        if (v < 0) buf_ch(b, '-');
        buf_int(b, (int64_t)g);
        while (q--) buf_ch(b, '0');
        return;
    }
    /* elsewhere: the shortest %.*e that reads back (round-tripping is monotonic in the precision),
     * expanded to positional digits */
    char t[40];
    int lo = 0, hi = 16;
    locale_t host = c_locale_enter();
    while (lo < hi) {
        int m = (lo + hi) / 2;
        snprintf(t, sizeof t, "%.*e", m, a);
        if (strtod(t, nullptr) == a) hi = m; else lo = m + 1;
    }
    snprintf(t, sizeof t, "%.*e", lo, a);
    uselocale(host);
    char dig[20];
    size_t nd = 0;
    char *e = strchr(t, 'e');
    for (char *q = t; q < e; q++) if (*q != '.') dig[nd++] = *q;
    long ex = strtol(e + 1, nullptr, 10); /* value = 0.dig × 10^(ex + 1) */
    if (v < 0) buf_ch(b, '-');
    if (ex < 0) {
        buf_lit(b, "0.");
        for (long z = -1; z > ex; z--) buf_ch(b, '0');
        buf_put(b, nd, dig);
    } else if ((size_t)ex + 1 >= nd) {
        buf_put(b, nd, dig);
        for (size_t z = nd; z < (size_t)ex + 1; z++) buf_ch(b, '0');
    } else {
        buf_put(b, (size_t)ex + 1, dig);
        buf_ch(b, '.');
        buf_put(b, nd - (size_t)ex - 1, dig + ex + 1);
    }
}

void mmd_w_num(mmd_writer *w, double v) {
    if (!live(w)) return;
    if (!isfinite(v)) return fail(w, w->b.n, "mmd_w_num: %s is not a finite number", isnan(v) ? "NaN" : v < 0 ? "-Infinity" : "Infinity");
    put_num(&w->b, v);
    term(w);
}
/* dotted key -> nested JSON object keys */
static bool cfg_key_ok(const char *key) {
    if (!key || !key[0] || key[0] == '.') return false;
    for (const char *p = key; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '.') {
            if (p[1] == '.' || !p[1]) return false;
            continue;
        }
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_')) return false;
    }
    return true;
}
static bool cfg_preamble(const mmd_writer *w) {
    size_t i = 0;
    while (i < w->b.n) {
        size_t e = i;
        while (e < w->b.n && w->b.p[e] != '\n') e++;
        size_t s = i;
        while (s < e && (w->b.p[s] == ' ' || w->b.p[s] == '\t')) s++;
        if (s < e && !(e - s >= 3 && !memcmp(w->b.p + s, "%%{", 3))) return false;
        i = e < w->b.n ? e + 1 : e;
    }
    return true;
}
static const char *digits(const char *p) {
    while (*p >= '0' && *p <= '9') p++;
    return p;
}
/* true, false and JSON numbers go bare: mermaid.js reads the string "false" as true */
static bool cfg_bare(const char *v) {
    if (!strcmp(v, "true") || !strcmp(v, "false")) return true;
    const char *p = v + (*v == '-'), *q = digits(p);
    if (q == p || (*p == '0' && q - p > 1)) return false;
    if (*q == '.') {
        p = q + 1, q = digits(p);
        if (q == p) return false;
    }
    if (*q == 'e' || *q == 'E') {
        p = q + 1 + (q[1] == '+' || q[1] == '-'), q = digits(p);
        if (q == p) return false;
    }
    return !*q;
}
void mmd_w_config(mmd_writer *w, const char *key, const char *value) {
    if (!live(w)) return;
    if (!key || !value) return fail(w, w->b.n, "mmd_w_config: key and value are required");
    if (!cfg_key_ok(key)) return fail(w, w->b.n, "mmd_w_config: '%s' is not a dotted config key", key);
    for (const char *p = value; *p; p++)
        if (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20)
            return fail(w, w->b.n, "mmd_w_config: value cannot contain quotes, backslashes or controls");
    if (!cfg_preamble(w)) return fail(w, w->b.n, "mmd_w_config: config goes before the diagram");
    buf_lit(&w->b, "%%{init: {");
    int depth = 0;
    for (const char *p = key; *p;) {
        const char *dot = strchr(p, '.');
        size_t n = dot ? (size_t)(dot - p) : strlen(p);
        buf_ch(&w->b, '"');
        buf_put(&w->b, n, p);
        buf_lit(&w->b, "\":");
        if (!dot) break;
        buf_ch(&w->b, '{');
        depth++;
        p = dot + 1;
    }
    bool bare = cfg_bare(value);
    if (!bare) buf_ch(&w->b, '"');
    buf_cstr(&w->b, value);
    if (!bare) buf_ch(&w->b, '"');
    while (depth--) buf_ch(&w->b, '}');
    buf_lit(&w->b, "}}%%\n");
    term(w);
}


/* ---- typed helpers ---- */

/* every *_begin: a new diagram, nothing open */
static void begin(mmd_writer w[static 1], const char header[static 1]) {
    w->nopen = 0;
    w->level = -1;
    mmd_w_line(w, 0);
    mmd_w_raw(w, header);
}

/* a statement line, indented one level per open block */
static void stmt(mmd_writer w[static 1], const char kw[static 1]) {
    mmd_w_line(w, 1 + w->nopen);
    mmd_w_raw(w, kw);
}
static void eol(mmd_writer w[static 1]) { mmd_w_raw(w, "\n"); }

void mmd_xy_begin(mmd_writer *w, const char *title, bool horizontal) {
    if (!live(w)) return;
    begin(w, horizontal ? "xychart horizontal" : "xychart");
    if (title) {
        stmt(w, "title ");
        mmd_w_str(w, title, 0);
    }
    eol(w);
}

static void axis(mmd_writer w[static 1], const char kw[static 1], const char *title) {
    stmt(w, kw);
    if (title) {
        mmd_w_raw(w, " ");
        mmd_w_str(w, title, 0);
    }
}

void mmd_xy_x_cats(mmd_writer *w, const char *title, size_t n, const char *const *cats) {
    if (!live(w)) return;
    if (!n) return fail(w, w->b.n, "mmd_xy_x_cats: mermaid needs at least one category");
    axis(w, "x-axis", title);
    mmd_w_raw(w, " [");
    for (size_t i = 0; i < n; i++) {
        if (i) mmd_w_raw(w, ", ");
        mmd_w_str(w, cats[i], 0);
    }
    mmd_w_raw(w, "]");
    eol(w);
}

static void range(mmd_writer w[static 1], const char kw[static 1], const char *title, double lo, double hi) {
    axis(w, kw, title);
    if (lo != hi) {
        mmd_w_raw(w, " ");
        mmd_w_num(w, lo);
        mmd_w_raw(w, " --> ");
        mmd_w_num(w, hi);
    }
    eol(w);
}
void mmd_xy_x_range(mmd_writer *w, const char *title, double lo, double hi) {
    if (live(w)) range(w, "x-axis", title, lo, hi);
}
void mmd_xy_y_range(mmd_writer *w, const char *title, double lo, double hi) {
    if (live(w)) range(w, "y-axis", title, lo, hi);
}

static void series(mmd_writer w[static 1], const char kw[static 1], const char *name, size_t n, const double *v) {
    if (!live(w)) return;
    if (!n) return fail(w, w->b.n, "mmd_xy_%s: mermaid needs at least one value", kw);
    axis(w, kw, name);
    mmd_w_raw(w, " [");
    for (size_t i = 0; i < n; i++) {
        if (i) mmd_w_raw(w, ", ");
        mmd_w_num(w, v[i]);
    }
    mmd_w_raw(w, "]");
    eol(w);
}
void mmd_xy_bar(mmd_writer *w, const char *name, size_t n, const double *v) { series(w, "bar", name, n, v); }
void mmd_xy_line(mmd_writer *w, const char *name, size_t n, const double *v) { series(w, "line", name, n, v); }

void mmd_pie_begin(mmd_writer *w, const char *title, bool show_data) {
    if (!live(w)) return;
    begin(w, show_data ? "pie showData" : "pie");
    if (title) {
        stmt(w, "title ");
        mmd_w_text(w, title, 0);
    }
    eol(w);
}

void mmd_pie_slice(mmd_writer *w, const char *label, double v) {
    if (!live(w)) return;
    if (v < 0) {
        AUTO_BUF Buf t = {};
        put_num(&t, v);
        buf_ch(&t, 0);
        return fail(w, w->b.n, "mmd_pie_slice: negative value %s", t.oom ? "" : t.p);
    }
    mmd_w_line(w, 1);
    mmd_w_str(w, label, 0);
    mmd_w_raw(w, " : ");
    mmd_w_num(w, v);
    eol(w);
}

void mmd_flow_begin(mmd_writer *w, const char *dir) {
    if (!live(w)) return;
    static const char *const DIRS[] = {"TB", "TD", "BT", "LR", "RL"};
    size_t k = 0;
    while (k < countof(DIRS) && strcmp(DIRS[k], dir)) k++;
    if (k == countof(DIRS)) return fail(w, w->b.n, "mmd_flow_begin: direction must be TB, TD, BT, LR or RL");
    begin(w, "flowchart ");
    mmd_w_raw(w, dir);
    eol(w);
}

/* a flowchart statement keyword can't name a node (mermaid has the same rule for "end") */
static void flow_id(mmd_writer w[static 1], const char id[static 1]) {
    static const char *const KW[] = {"subgraph", "end", "direction", "classDef", "class",
                                     "style", "linkStyle", "click", "accTitle", "accDescr"};
    for (size_t k = 0; k < countof(KW); k++)
        if (!strcmp(KW[k], id)) return fail(w, w->b.n, "flowchart: '%s' is a keyword, not a node id", id);
    mmd_w_id(w, id);
}

void mmd_flow_node(mmd_writer *w, const char *id, const char *label, mmd_shape shape) {
    static const char DELIM[][2][4] = {
        [MMD_SHAPE_RECT] = {"[", "]"},        [MMD_SHAPE_ROUND] = {"(", ")"},
        [MMD_SHAPE_STADIUM] = {"([", "])"},   [MMD_SHAPE_SUBROUTINE] = {"[[", "]]"},
        [MMD_SHAPE_CYLINDER] = {"[(", ")]"},  [MMD_SHAPE_CIRCLE] = {"((", "))"},
        [MMD_SHAPE_DCIRCLE] = {"(((", ")))"}, [MMD_SHAPE_ASYM] = {">", "]"},
        [MMD_SHAPE_RHOMBUS] = {"{", "}"},     [MMD_SHAPE_HEXAGON] = {"{{", "}}"},
        [MMD_SHAPE_LEAN_R] = {"[/", "/]"},    [MMD_SHAPE_LEAN_L] = {"[\\", "\\]"},
        [MMD_SHAPE_TRAP] = {"[/", "\\]"},     [MMD_SHAPE_TRAP_ALT] = {"[\\", "/]"},
    };
    if (!live(w)) return;
    if (shape >= countof(DELIM)) return fail(w, w->b.n, "mmd_flow_node: unknown shape %u", (unsigned)shape);
    mmd_w_line(w, 1);
    flow_id(w, id);
    if (label || shape != MMD_SHAPE_RECT) {
        mmd_w_raw(w, DELIM[shape][0]);
        mmd_w_str(w, label ? label : id, MMD_W_BR);
        mmd_w_raw(w, DELIM[shape][1]);
    }
    eol(w);
}

void mmd_flow_edge(mmd_writer *w, const char *from, const char *to, const char *label) {
    if (!live(w)) return;
    mmd_w_line(w, 1);
    flow_id(w, from);
    mmd_w_raw(w, " -->");
    if (label) {
        mmd_w_raw(w, "|");
        mmd_w_str(w, label, MMD_W_BR);
        mmd_w_raw(w, "|");
    }
    mmd_w_raw(w, " ");
    flow_id(w, to);
    eol(w);
}

/* ---- blocks, keywords and checks shared by the helpers below ---- */

/* kind: what mmd_end() writes: '}' for a brace block, else "end" (and for sequence blocks, the
 * opener: 'a'lt, 'p'ar, 'c'ritical take else / and / option) */
static void open_block(mmd_writer w[static 1], char kind) {
    if (w->nopen == sizeof w->open) return fail(w, w->b.n, "blocks nested deeper than %zu", sizeof w->open);
    w->open[w->nopen++] = kind;
}

void mmd_end(mmd_writer *w) {
    if (!live(w)) return;
    if (!w->nopen) return fail(w, w->b.n, "mmd_end: no block is open");
    char k = w->open[--w->nopen];
    stmt(w, k == '}' ? "}" : "end");
    eol(w);
}

/* s, after leading blanks, starts with one of kw (ASCII case-insensitively); whole: and the keyword
 * is the whole of s, else any prefix counts (mermaid's jison lexers take the first rule that matches,
 * so "endpoint" lexes as "end" + "point") */
static bool has_kw(const char *s, size_t n, const char *const kw[static n], bool whole) {
    while (*s == ' ' || *s == '\t') s++;
    for (size_t k = 0; k < n; k++) {
        size_t m = strlen(kw[k]);
        if (!strncasecmp(s, kw[k], m) && (!whole || !s[m])) return true;
    }
    return false;
}
#define HAS_KW(s, kw, whole) has_kw((s), countof(kw), (kw), (whole))

static bool is_word(unsigned char c) { return c == '_' || (unsigned)(c - '0') < 10u || (unsigned)((c | 32) - 'a') < 26u; }

/* mmd_w_id, but with the first character as an entity when lead (a keyword would lex there) */
static void id_lead(mmd_writer w[static 1], const char *id, bool lead) {
    if (!lead) return mmd_w_id(w, id);
    size_t at = w->b.n;
    mmd_w_id(w, id); /* validates */
    if (!live(w)) return;
    w->b.n = at;
    put_esc(w, at, "mmd_w_id", id, CT, W_LEAD);
    term(w);
}

/* a nonempty T value (a keyword followed by nothing is not a statement) */
static bool need(mmd_writer w[static 1], const char *fn, const char *what, const char *s) {
    const char *p = s ? s : "";
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) fail(w, w->b.n, "%s: empty %s", fn, what);
    return *p;
}

/* ---- sequenceDiagram ---- */

static const char *const SEQ_KW[] = {"participant", "actor", "box", "create", "destroy", "loop", "rect", "opt",
                                     "alt", "else", "par", "and", "critical", "option", "break", "end", "over",
                                     "activate", "deactivate", "autonumber", "off", "note", "sequenceDiagram"};

static void seq_id(mmd_writer w[static 1], const char *id) {
    id_lead(w, id, HAS_KW(id, SEQ_KW, false) || (unsigned)(id[0] - '0') < 10u); /* a leading number lexes as NUM */
}

void mmd_seq_begin(mmd_writer *w, const char *title, bool autonumber) {
    if (!live(w)) return;
    begin(w, "sequenceDiagram");
    if (autonumber) stmt(w, "autonumber");
    if (title && need(w, "mmd_seq_begin", "title", title)) {
        stmt(w, "title ");
        mmd_w_text(w, title, 0);
    }
    eol(w);
}

void mmd_seq_participant(mmd_writer *w, const char *id, const char *alias, bool actor) {
    if (!live(w)) return;
    stmt(w, actor ? "actor " : "participant ");
    seq_id(w, id);
    if (alias && need(w, "mmd_seq_participant", "alias", alias)) {
        mmd_w_raw(w, " as ");
        mmd_w_text(w, alias, MMD_W_BR);
    }
    eol(w);
}

void mmd_seq_message(mmd_writer *w, const char *from, const char *to, mmd_arrow arrow, const char *text) {
    static const char *const ARROW[] = {
        [MMD_ARROW_SOLID] = "->>", [MMD_ARROW_DOTTED] = "-->>", [MMD_ARROW_LINE] = "->",
        [MMD_ARROW_DOTTED_LINE] = "-->", [MMD_ARROW_CROSS] = "-x", [MMD_ARROW_DOTTED_CROSS] = "--x",
        [MMD_ARROW_ASYNC] = "-)", [MMD_ARROW_DOTTED_ASYNC] = "--)",
    };
    if (!live(w)) return;
    if (arrow >= countof(ARROW)) return fail(w, w->b.n, "mmd_seq_message: unknown arrow %u", (unsigned)arrow);
    stmt(w, "");
    seq_id(w, from);
    mmd_w_raw(w, ARROW[arrow]);
    seq_id(w, to);
    mmd_w_raw(w, ": ");
    mmd_w_text(w, text ? text : "", MMD_W_BR);
    eol(w);
}

void mmd_seq_note(mmd_writer *w, mmd_side side, const char *a, const char *b, const char *text) {
    if (!live(w)) return;
    if (side > MMD_OVER) return fail(w, w->b.n, "mmd_seq_note: unknown side %u", (unsigned)side);
    if (b && side != MMD_OVER) return fail(w, w->b.n, "mmd_seq_note: only a note over spans two participants");
    stmt(w, side == MMD_LEFT_OF ? "Note left of " : side == MMD_RIGHT_OF ? "Note right of " : "Note over ");
    seq_id(w, a);
    if (b) {
        mmd_w_raw(w, ",");
        seq_id(w, b);
    }
    mmd_w_raw(w, ": ");
    mmd_w_text(w, text ? text : "", MMD_W_BR);
    eol(w);
}

void mmd_seq_activate(mmd_writer *w, const char *id, bool on) {
    if (!live(w)) return;
    stmt(w, on ? "activate " : "deactivate ");
    seq_id(w, id);
    eol(w);
}

void mmd_seq_block(mmd_writer *w, const char *kw, const char *label) {
    static const struct { char kw[9], kind, in; } K[] = {
        {"loop", 'e', 0}, {"opt", 'e', 0}, {"break", 'e', 0}, {"alt", 'a', 0}, {"par", 'p', 0},
        {"critical", 'c', 0}, {"else", 0, 'a'}, {"and", 0, 'p'}, {"option", 0, 'c'},
    };
    if (!live(w)) return;
    size_t k = 0;
    while (k < countof(K) && strcmp(K[k].kw, kw)) k++;
    if (k == countof(K)) return fail(w, w->b.n, "mmd_seq_block: '%s' is not loop, opt, break, alt, else, par, and, critical or option", kw);
    if (K[k].in && (!w->nopen || w->open[w->nopen - 1] != K[k].in))
        return fail(w, w->b.n, "mmd_seq_block: '%s' outside its %s block", kw, K[k].in == 'a' ? "alt" : K[k].in == 'p' ? "par" : "critical");
    if (K[k].in) w->nopen--; /* else / and / option sit at their opener's depth */
    stmt(w, kw);
    if (K[k].in) w->nopen++;
    if (label && *label) {
        mmd_w_raw(w, " ");
        mmd_w_text(w, label, 0);
    }
    eol(w);
    if (K[k].kind) open_block(w, K[k].kind);
}

/* ---- gantt ---- */

static const char *const GANTT_KW[] = {"gantt", "dateFormat", "inclusiveEndDates", "topAxis", "axisFormat",
                                       "tickInterval", "includes", "excludes", "todayMarker", "weekday",
                                       "weekend", "title", "accDescription", "accTitle", "accDescr",
                                       "section", "click", "call", "href"};
/* task data words that are not ids */
static const char *const GANTT_TAG[] = {"done", "active", "crit", "milestone", "after", "until", "vert"};

void mmd_gantt_begin(mmd_writer *w, const char *title) {
    if (!live(w)) return;
    begin(w, "gantt");
    stmt(w, "dateFormat YYYY-MM-DD");
    if (title && need(w, "mmd_gantt_begin", "title", title)) {
        stmt(w, "title ");
        mmd_w_text(w, title, 0);
    }
    eol(w);
}

void mmd_gantt_section(mmd_writer *w, const char *name) {
    if (!live(w) || !need(w, "mmd_gantt_section", "name", name)) return;
    stmt(w, "section ");
    mmd_w_text(w, name, 0);
    eol(w);
}

/* YYYY-MM-DD naming a real day */
static bool iso_date(const char *s) {
    for (int i = 0; i < 10; i++)
        if (i == 4 || i == 7 ? s[i] != '-' : (unsigned)(s[i] - '0') >= 10u) return false;
    if (s[10]) return false;
    int y = atoi(s), m = atoi(s + 5), d = atoi(s + 8);
    static const int DAYS[] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (y % 4 == 0 && y % 100) || y % 400 == 0;
    return m >= 1 && m <= 12 && d >= 1 && d <= DAYS[m - 1] - (m == 2 && !leap);
}

static void gantt_id(mmd_writer w[static 1], const char *fn, const char *id) {
    if (HAS_KW(id, GANTT_TAG, true) || (unsigned)(id[0] - '0') < 10u)
        return fail(w, w->b.n, "%s: task id '%s' would read as a tag, date or duration", fn, id);
    mmd_w_id(w, id);
}

void mmd_gantt_task(mmd_writer *w, const char *name, const char *id, unsigned flags, const char *start, double days) {
    static const char *const TAG[] = {"done", "active", "crit", "milestone"};
    if (!live(w) || !need(w, "mmd_gantt_task", "name", name)) return;
    if (!(days >= 0) || !isfinite(days)) return fail(w, w->b.n, "mmd_gantt_task: days must be finite and >= 0");
    if (flags >> countof(TAG)) return fail(w, w->b.n, "mmd_gantt_task: unknown flags 0x%x", flags);
    mmd_w_line(w, 1 + w->nopen);
    size_t at = w->b.n;
    bool lead = HAS_KW(name, GANTT_KW, false) || (unsigned)(name[strspn(name, " \t")] - '0') < 10u; /* a date */
    if (!put_esc(w, at, "mmd_gantt_task", name, CT, lead ? W_LEAD : 0)) return;
    mmd_w_raw(w, " :");
    const char *sep = "";
    for (size_t k = 0; k < countof(TAG); k++)
        if (flags & 1u << k) mmd_w_raw(w, sep), mmd_w_raw(w, TAG[k]), sep = ", ";
    if (id) mmd_w_raw(w, sep), gantt_id(w, "mmd_gantt_task", id), sep = ", ";
    if (start) {
        mmd_w_raw(w, sep);
        if (iso_date(start)) mmd_w_raw(w, start);
        else mmd_w_raw(w, "after "), gantt_id(w, "mmd_gantt_task", start);
        sep = ", ";
    }
    mmd_w_raw(w, sep);
    mmd_w_num(w, days);
    mmd_w_raw(w, "d");
    eol(w);
}

/* ---- stateDiagram-v2 ---- */

static const char *const STATE_KW[] = {"state", "note", "class", "classDef", "style", "scale", "direction", "end"};

static void state_id(mmd_writer w[static 1], const char *id) {
    if (!id) return mmd_w_raw(w, "[*]");
    if (HAS_KW(id, STATE_KW, true) || !strncasecmp(id, "stateDiagram", 12))
        return fail(w, w->b.n, "stateDiagram: '%s' is a keyword, not a state id", id);
    mmd_w_id(w, id);
}

static bool dir_ok(mmd_writer w[static 1], const char *fn, const char *dir, bool td) {
    static const char *const DIRS[] = {"TB", "BT", "LR", "RL", "TD"};
    size_t k = 0;
    while (k < countof(DIRS) - !td && strcmp(DIRS[k], dir)) k++;
    if (k < countof(DIRS) - !td) return true;
    fail(w, w->b.n, "%s: direction must be TB, BT, LR or RL%s", fn, td ? " (or TD)" : "");
    return false;
}

void mmd_state_begin(mmd_writer *w, const char *dir) {
    if (!live(w) || (dir && !dir_ok(w, "mmd_state_begin", dir, false))) return;
    begin(w, "stateDiagram-v2");
    if (dir) stmt(w, "direction "), mmd_w_raw(w, dir);
    eol(w);
}

static void state_decl(mmd_writer w[static 1], const char *id, const char *label, bool group) {
    if (!live(w)) return;
    if (!id) return fail(w, w->b.n, "%s: [*] cannot be declared", group ? "mmd_state_group" : "mmd_state");
    if (!label && strchr(id, '_')) label = id; /* a bare id is markdown: a_b_ */
    if (!label && !group) {
        stmt(w, "");
        state_id(w, id);
    } else {
        stmt(w, "state ");
        if (label) {
            mmd_w_raw(w, "\"");
            size_t at = w->b.n - 1;
            if (!put_esc(w, at, group ? "mmd_state_group" : "mmd_state", label, CQ, MMD_W_BR | W_DIR)) return;
            mmd_w_raw(w, "\" as ");
        }
        state_id(w, id);
    }
    if (group) mmd_w_raw(w, " {");
    eol(w);
    if (group) open_block(w, '}');
}
void mmd_state(mmd_writer *w, const char *id, const char *label) { state_decl(w, id, label, false); }
void mmd_state_group(mmd_writer *w, const char *id, const char *label) { state_decl(w, id, label, true); }

void mmd_state_transition(mmd_writer *w, const char *from, const char *to, const char *label) {
    if (!live(w)) return;
    stmt(w, "");
    state_id(w, from);
    mmd_w_raw(w, " --> ");
    state_id(w, to);
    if (label && *label) {
        mmd_w_raw(w, " : ");
        put_esc(w, w->b.n, "mmd_state_transition", label, CT, MMD_W_BR | W_DIR);
    }
    eol(w);
}

void mmd_state_note(mmd_writer *w, mmd_side side, const char *id, const char *text) {
    if (!live(w) || !need(w, "mmd_state_note", "text", text)) return;
    if (side > MMD_RIGHT_OF) return fail(w, w->b.n, "mmd_state_note: a state note is left of or right of a state");
    if (!id) return fail(w, w->b.n, "mmd_state_note: a note needs a state, not [*]");
    stmt(w, side == MMD_LEFT_OF ? "note left of " : "note right of ");
    state_id(w, id);
    mmd_w_raw(w, " : ");
    put_esc(w, w->b.n, "mmd_state_note", text, CT, MMD_W_BR | W_DIR);
    eol(w);
}

/* ---- sankey ---- */

void mmd_sankey_begin(mmd_writer *w) {
    if (!live(w)) return;
    begin(w, "sankey-beta");
    eol(w);
}

void mmd_sankey_flow(mmd_writer *w, const char *source, const char *target, double value) {
    if (!live(w)) return;
    if (!(value >= 0)) return fail(w, w->b.n, "mmd_sankey_flow: value must be >= 0");
    mmd_w_line(w, 0);
    mmd_w_csv(w, source);
    mmd_w_raw(w, ",");
    mmd_w_csv(w, target);
    mmd_w_raw(w, ",");
    mmd_w_num(w, value);
    eol(w);
}

/* ---- mindmap, treeView: nesting by depth ---- */

static bool tree_depth(mmd_writer w[static 1], const char *fn, unsigned depth, bool one_root) {
    if ((int)depth > w->level + 1 || depth > 64)
        return fail(w, w->b.n, "%s: depth %u under a node at depth %d", fn, depth, w->level), false;
    if (one_root && !depth && w->level >= 0) return fail(w, w->b.n, "%s: a mindmap has one root", fn), false;
    if (one_root && depth && w->level < 0) return fail(w, w->b.n, "%s: the first node is the root, depth 0", fn), false;
    w->level = (int)depth;
    return true;
}

void mmd_mindmap_begin(mmd_writer *w) {
    if (!live(w)) return;
    begin(w, "mindmap");
    eol(w);
}

void mmd_mindmap_node(mmd_writer *w, unsigned depth, const char *id, const char *label, mmd_shape shape) {
    static const char DELIM[][2][3] = {
        [MMD_SHAPE_RECT] = {"[", "]"}, [MMD_SHAPE_ROUND] = {"(", ")"},
        [MMD_SHAPE_CIRCLE] = {"((", "))"}, [MMD_SHAPE_HEXAGON] = {"{{", "}}"},
    };
    if (!live(w)) return;
    if (shape >= countof(DELIM) || !DELIM[shape][0][0])
        return fail(w, w->b.n, "mmd_mindmap_node: shape %u is not RECT, ROUND, CIRCLE or HEXAGON", (unsigned)shape);
    if (!tree_depth(w, "mmd_mindmap_node", depth, true)) return;
    mmd_w_line(w, 1 + depth);
    mmd_w_id(w, id);
    if (label || shape != MMD_SHAPE_RECT || strchr(id, '_')) { /* bare text is markdown: a_b_ */
        mmd_w_raw(w, DELIM[shape][0]);
        mmd_w_str(w, label ? label : id, MMD_W_BR);
        mmd_w_raw(w, DELIM[shape][1]);
    }
    eol(w);
}

void mmd_tree_begin(mmd_writer *w) {
    if (!live(w)) return;
    begin(w, "treeView-beta");
    eol(w);
}

static const char *const TREE_KW[] = {"title", "accTitle", "accDescr"};

/* MMD_W_LINESTART: the keywords that start a statement in the diagram the text so far declares
 * (mermaid's lexers: timeline/journey/ishikawa jison, wardley langium); others get the common ones */
static const char *const TIMELINE_KW[] = {"timeline", "title", "accTitle", "accDescr", "section"};
static const char *const JOURNEY_KW[] = {"journey", "title", "accTitle", "accDescr", "section"};
static const char *const ISHIKAWA_KW[] = {"ishikawa"};
static const char *const WARDLEY_KW[] = {"wardley-beta", "title", "accTitle", "accDescr", "size", "evolution",
                                         "anchor", "component", "label", "inertia", "evolve", "pipeline", "note",
                                         "annotations", "annotation", "accelerator", "deaccelerator", "build",
                                         "buy", "outsource", "market"};
static const struct {
    const char *header;
    size_t n;
    const char *const *kw;
} LINE_KW[] = {
    {"timeline", countof(TIMELINE_KW), TIMELINE_KW},  {"journey", countof(JOURNEY_KW), JOURNEY_KW},
    {"ishikawa", countof(ISHIKAWA_KW), ISHIKAWA_KW},  {"ishikawa-beta", countof(ISHIKAWA_KW), ISHIKAWA_KW},
    {"wardley-beta", countof(WARDLEY_KW), WARDLEY_KW}, {"gantt", countof(GANTT_KW), GANTT_KW},
    {"sequenceDiagram", countof(SEQ_KW), SEQ_KW},     {"treeView-beta", countof(TREE_KW), TREE_KW},
};

static bool line_kw(const mmd_writer w[static 1], const char *s) {
    /* the header: first word of the first line that is not blank, a %% comment or --- front matter */
    const char *p = w->b.p, *e = p + w->b.n;
    bool fm = false;
    while (p < e) {
        const char *nl = memchr(p, '\n', (size_t)(e - p)), *q = p;
        if (!nl) nl = e;
        while (q < nl && (*q == ' ' || *q == '\t' || *q == '\r')) q++;
        if (nl - q >= 3 && !memcmp(q, "---", 3)) fm = !fm;
        else if (!fm && q < nl && !(nl - q >= 2 && q[0] == '%' && q[1] == '%')) {
            size_t n = 0;
            while (q + n < nl && q[n] != ' ' && q[n] != '\t' && q[n] != '\r') n++;
            for (size_t k = 0; k < countof(LINE_KW); k++)
                if (strlen(LINE_KW[k].header) == n && !memcmp(q, LINE_KW[k].header, n))
                    return has_kw(s, LINE_KW[k].n, LINE_KW[k].kw, false);
            static const char *const COMMON[] = {"title", "accTitle", "accDescr"};
            if (HAS_KW(s, COMMON, false)) return true;
            /* the header word itself */
            while (*s == ' ' || *s == '\t') s++;
            return !strncasecmp(s, q, n);
        }
        p = nl + 1;
    }
    return false;
}

void mmd_tree_node(mmd_writer *w, unsigned depth, const char *name, bool dir, const char *desc) {
    if (!live(w) || !need(w, "mmd_tree_node", "name", name)) return;
    if (!tree_depth(w, "mmd_tree_node", depth, false)) return;
    mmd_w_line(w, 1 + depth);
    char c0 = name[strspn(name, " \t")];
    bool lead = HAS_KW(name, TREE_KW, false) || c0 == '"' || c0 == '\''; /* "…" and '…' are strings */
    if (!put_esc(w, w->b.n, "mmd_tree_node", name, CT, W_TREE | (lead ? W_LEAD : 0))) return;
    if (dir) mmd_w_raw(w, "/");
    if (desc && *desc) {
        mmd_w_raw(w, " ## ");
        mmd_w_text(w, desc, 0);
    }
    eol(w);
}

/* ---- erDiagram ---- */

void mmd_er_begin(mmd_writer *w) {
    if (!live(w)) return;
    begin(w, "erDiagram");
    eol(w);
}

/* entity names are always quoted: then any text is a name, keywords ("one", "to", "end") included */
static void er_name(mmd_writer w[static 1], const char *fn, const char *name) {
    if (!need(w, fn, "entity name", name)) return;
    size_t at = w->b.n;
    buf_ch(&w->b, '"');
    if (!put_esc(w, at, fn, name, CQ, W_DIR)) return;
    buf_ch(&w->b, '"');
    term(w);
}

void mmd_er_entity(mmd_writer *w, const char *name, const char *alias) {
    if (!live(w)) return;
    stmt(w, "");
    er_name(w, "mmd_er_entity", name);
    if (alias) {
        mmd_w_raw(w, "[");
        er_name(w, "mmd_er_entity", alias);
        mmd_w_raw(w, "]");
    }
    mmd_w_raw(w, " {");
    eol(w);
    open_block(w, '}');
}

/* an attribute type or name: mermaid's ATTRIBUTE_WORD, and not a key */
static void er_word(mmd_writer w[static 1], const char *what, const char *s) {
    static const char *const KEYS[] = {"PK", "FK", "UK"};
    size_t n = strlen(s);
    unsigned char c0 = (unsigned char)s[0];
    bool ok = n && !HAS_KW(s, KEYS, true) && (c0 == '*' || (is_word(c0) && (unsigned)(c0 - '0') >= 10u) || c0 >= 0x80);
    for (size_t i = 1; ok && i < n; i++)
        ok = is_word((unsigned char)s[i]) || strchr("-[]().,*", s[i]) || (unsigned char)s[i] >= 0x80;
    if (!ok) return fail(w, w->b.n, "mmd_er_attr: '%s' is not an attribute %s ([A-Za-z_*] then [A-Za-z0-9_*.,()[]-])", s, what);
    mmd_w_raw(w, s);
}

void mmd_er_attr(mmd_writer *w, const char *type, const char *name, unsigned keys, const char *comment) {
    static const char *const KEY[] = {"PK", "FK", "UK"};
    if (!live(w)) return;
    if (!w->nopen || w->open[w->nopen - 1] != '}') return fail(w, w->b.n, "mmd_er_attr: outside mmd_er_entity()");
    if (keys >> countof(KEY)) return fail(w, w->b.n, "mmd_er_attr: unknown keys 0x%x", keys);
    stmt(w, "");
    er_word(w, "type", type);
    mmd_w_raw(w, " ");
    er_word(w, "name", name);
    const char *sep = " ";
    for (size_t k = 0; k < countof(KEY); k++)
        if (keys & 1u << k) mmd_w_raw(w, sep), mmd_w_raw(w, KEY[k]), sep = ", ";
    if (comment) mmd_w_raw(w, " "), mmd_w_str(w, comment, 0);
    eol(w);
}

void mmd_er_rel(mmd_writer *w, const char *a, mmd_card ca, const char *b, mmd_card cb, bool identifying,
                const char *label) {
    static const char L[][3] = {"||", "|o", "}|", "}o"}, Rt[][3] = {"||", "o|", "|{", "o{"};
    if (!live(w)) return;
    if (ca > MMD_CARD_ZERO_OR_MORE || cb > MMD_CARD_ZERO_OR_MORE) return fail(w, w->b.n, "mmd_er_rel: unknown cardinality");
    stmt(w, "");
    er_name(w, "mmd_er_rel", a);
    mmd_w_raw(w, " ");
    mmd_w_raw(w, L[ca]);
    mmd_w_raw(w, identifying ? "--" : "..");
    mmd_w_raw(w, Rt[cb]);
    mmd_w_raw(w, " ");
    er_name(w, "mmd_er_rel", b);
    mmd_w_raw(w, " : \"");
    size_t at = w->b.n - 1;
    if (!put_esc(w, at, "mmd_er_rel", label ? label : "", CQ, W_DIR)) return;
    mmd_w_raw(w, "\"");
    eol(w);
}

/* ---- gitGraph ---- */

static const char *const GIT_KW[] = {"commit", "branch", "merge", "checkout", "switch", "cherry-pick", "gitGraph",
                                     "id", "msg", "tag", "type", "order", "parent", "NORMAL", "REVERSE",
                                     "HIGHLIGHT", "LR", "TB", "BT", "title", "accTitle", "accDescr", "true", "false"};

/* a REFERENCE: \w([-./\w]*[-\w])?, ASCII, not a keyword and not a number */
static void git_ref(mmd_writer w[static 1], const char *fn, const char *name) {
    size_t n = strlen(name);
    bool ok = n && is_word((unsigned char)name[0]) && (is_word((unsigned char)name[n - 1]) || name[n - 1] == '-');
    for (size_t i = 1; ok && i < n; i++) ok = is_word((unsigned char)name[i]) || strchr("-./", name[i]);
    if (!ok) return fail(w, w->b.n, "%s: branch name '%s' must match \\w([-./\\w]*[-\\w])?", fn, name);
    if (HAS_KW(name, GIT_KW, true) || strspn(name, "0123456789.") == n)
        return fail(w, w->b.n, "%s: '%s' would read as a keyword or number, not a branch", fn, name);
    mmd_w_raw(w, name);
}

void mmd_git_begin(mmd_writer *w, const char *dir) {
    if (!live(w)) return;
    if (dir && strcmp(dir, "LR") && strcmp(dir, "TB") && strcmp(dir, "BT"))
        return fail(w, w->b.n, "mmd_git_begin: direction must be LR, TB or BT");
    begin(w, "gitGraph");
    if (dir) mmd_w_raw(w, " "), mmd_w_raw(w, dir), mmd_w_raw(w, ":");
    eol(w);
}

static void git_attrs(mmd_writer w[static 1], const char *id, const char *tag, mmd_commit type) {
    static const char *const TYPE[] = {"NORMAL", "REVERSE", "HIGHLIGHT"};
    if (type >= countof(TYPE)) return fail(w, w->b.n, "gitGraph: unknown commit type %u", (unsigned)type);
    if (id) mmd_w_raw(w, " id: "), mmd_w_str(w, id, 0);
    if (tag) mmd_w_raw(w, " tag: "), mmd_w_str(w, tag, 0);
    if (type) mmd_w_raw(w, " type: "), mmd_w_raw(w, TYPE[type]);
}

void mmd_git_commit(mmd_writer *w, const char *id, const char *tag, mmd_commit type) {
    if (!live(w)) return;
    stmt(w, "commit");
    git_attrs(w, id, tag, type);
    eol(w);
}

void mmd_git_branch(mmd_writer *w, const char *name) {
    if (!live(w)) return;
    stmt(w, "branch ");
    git_ref(w, "mmd_git_branch", name);
    eol(w);
}

void mmd_git_checkout(mmd_writer *w, const char *name) {
    if (!live(w)) return;
    stmt(w, "checkout ");
    git_ref(w, "mmd_git_checkout", name);
    eol(w);
}

void mmd_git_merge(mmd_writer *w, const char *name, const char *id, const char *tag, mmd_commit type) {
    if (!live(w)) return;
    stmt(w, "merge ");
    git_ref(w, "mmd_git_merge", name);
    git_attrs(w, id, tag, type);
    eol(w);
}

void mmd_git_cherry_pick(mmd_writer *w, const char *id) {
    if (!live(w)) return;
    stmt(w, "cherry-pick id: ");
    mmd_w_str(w, id, 0);
    eol(w);
}
