/* Mermaid source writer: a Buf with a sticky error. Labels are escaped with
 * mermaid entities (#35; ...), which every diagram type decodes on output;
 * docs/dev/writer.md has the per-diagram audit behind the escape sets. */
#include "mmd_writer.h"

#include <math.h>
#include <stdarg.h>

#include "base.h"

struct mmd_writer {
    Buf b;
    mmd_status st;
    size_t err_at; /* text offset where the rejected value would have gone */
    char msg[120];
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
    w->err_at = at;
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
/* always escaped: they end or split a label somewhere (audit in docs/dev/writer.md) */
static constexpr uint8_t ESC[128] = {
    ['"'] = CQ | CT, [':'] = CQ | CT, ['['] = CQ | CT, [']'] = CQ | CT, ['{'] = CQ | CT,
    ['}'] = CQ | CT, ['|'] = CQ | CT, ['<'] = CQ | CT | CC, ['`'] = CQ, [';'] = CT, [','] = CT, ['~'] = CT,
};

static inline bool u8_cont(unsigned char c) { return (c & 0xC0u) == 0x80u; }
/* length of the valid UTF-8 sequence at s[0] (NUL-terminated), 0 if invalid */
static size_t utf8_len(const unsigned char s[static 2]) {
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

/* byte at s[i] must become an entity in context ctx */
static bool must_esc(const char *s, size_t i, unsigned ctx) {
    unsigned char c = (unsigned char)s[i];
    if (c >= 0x80) return false;
    if (ESC[c] & ctx) return true;
    switch (c) {
    case '#': { /* would start an entity, mermaid's #\w+; */
        size_t j = i + 1;
        while ((unsigned)((s[j] | 32) - 'a') < 26u || (unsigned)(s[j] - '0') < 10u || s[j] == '_') j++;
        return j > i + 1 && s[j] == ';';
    }
    case '%': return s[i + 1] == '%';                                                /* comment */
    case '-': return ctx == CT && (s[i + 1] == '-' || s[i + 1] == '>' || s[i + 1] == '.'); /* arrow */
    case '/': return ctx == CT && s[i + 1] == '/';                                    /* zenuml comment */
    }
    return false;
}

static void put_esc(mmd_writer w[static 1], size_t at, const char *fn, const char *s, unsigned ctx, unsigned flags) {
    size_t run = 0, i = 0;
    for (; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 0x80) {
            size_t k = utf8_len((const unsigned char *)s + i);
            if (!k) return fail(w, at, "%s: invalid UTF-8 at byte %zu", fn, i);
            i += k - 1;
            continue;
        }
        if (c < 0x20 && c != '\t') {
            if (c != '\n') return fail(w, at, "%s: control character 0x%02x at byte %zu", fn, c, i);
            if (!(flags & MMD_W_BR)) return fail(w, at, "%s: line break at byte %zu (MMD_W_BR where the diagram draws them)", fn, i);
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
        if (!must_esc(s, i, ctx)) continue;
        buf_put(&w->b, i - run, s + run);
        buf_ch(&w->b, '#');
        buf_int(&w->b, c);
        buf_ch(&w->b, ';');
        run = i + 1;
    }
    buf_put(&w->b, i - run, s + run);
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
    put_esc(w, at, "mmd_w_str", s, CQ, flags);
    buf_ch(&w->b, '"');
    term(w);
}

void mmd_w_text(mmd_writer *w, const char *s, unsigned flags) {
    if (!live(w)) return;
    put_esc(w, w->b.n, "mmd_w_text", s, CT, flags);
    term(w);
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
    put_esc(w, at, "mmd_w_csv", s, CC, 0);
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
    while (lo < hi) {
        int m = (lo + hi) / 2;
        snprintf(t, sizeof t, "%.*e", m, a);
        if (strtod(t, nullptr) == a) hi = m; else lo = m + 1;
    }
    snprintf(t, sizeof t, "%.*e", lo, a);
    char dig[20];
    size_t nd = 0;
    char *e = strchr(t, 'e');
    for (char *q = t; q < e; q++) if ((unsigned)(*q - '0') < 10u) dig[nd++] = *q; /* any locale's point */
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
    if (!isfinite(v)) return fail(w, w->b.n, "mmd_w_num: %g is not a finite number", v);
    put_num(&w->b, v);
    term(w);
}

/* ---- typed helpers ---- */

static void stmt(mmd_writer w[static 1], const char kw[static 1]) {
    mmd_w_line(w, 1);
    mmd_w_raw(w, kw);
}
static void eol(mmd_writer w[static 1]) { mmd_w_raw(w, "\n"); }

void mmd_xy_begin(mmd_writer *w, const char *title, bool horizontal) {
    if (!live(w)) return;
    mmd_w_line(w, 0);
    mmd_w_raw(w, horizontal ? "xychart horizontal" : "xychart");
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
    mmd_w_line(w, 0);
    mmd_w_raw(w, show_data ? "pie showData" : "pie");
    if (title) {
        stmt(w, "title ");
        mmd_w_text(w, title, 0);
    }
    eol(w);
}

void mmd_pie_slice(mmd_writer *w, const char *label, double v) {
    if (!live(w)) return;
    if (v < 0) return fail(w, w->b.n, "mmd_pie_slice: negative value %g", v);
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
    mmd_w_line(w, 0);
    mmd_w_raw(w, "flowchart ");
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
