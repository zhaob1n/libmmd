#include "cmermaid.h"

#include <stdio.h>
#include <stdlib.h>

#include "graph.h"
#include "diagrams.h"
#include "seq.h"
#include "dcfg.h"

const char *cmm_status_str(cmm_status s) {
    switch (s) {
    case CMM_OK: return "ok";
    case CMM_ERR_SYNTAX: return "syntax error";
    case CMM_ERR_UNSUPPORTED: return "unsupported diagram type";
    case CMM_ERR_NOMEM: return "out of memory";
    case CMM_ERR_EMPTY: return "empty input";
    case CMM_ERR_LIMIT: return "input too large";
    }
    return "unknown";
}

void cmm_buffer_free(cmm_buffer *buf) {
    if (!buf) return;
    free(buf->data);
    *buf = (cmm_buffer){};
}

typedef struct Config {
    Str title;
    double node_sep, rank_sep;
    cmm_theme theme;
} Config;

static double parse_num_after(Str s, Str key) {
    for (size_t i = 0; i + key.n <= s.n; i++) {
        if (memcmp(s.p + i, key.p, key.n) != 0) continue;
        size_t j = i + key.n;
        while (j < s.n && (s.p[j] == '"' || s.p[j] == '\'' || s.p[j] == ':' || is_sp(s.p[j]))) j++;
        double v = 0;
        bool any = false;
        while (j < s.n && (unsigned)(s.p[j] - '0') < 10u) { v = v * 10 + (s.p[j] - '0'); j++; any = true; }
        if (j < s.n && s.p[j] == '.') {
            double f = 0.1;
            for (j++; j < s.n && (unsigned)(s.p[j] - '0') < 10u; j++, f /= 10) v += (s.p[j] - '0') * f;
        }
        if (any) return v;
    }
    return 0;
}

static cmm_theme parse_theme_after(Str s) {
    static const Str KEY = S("theme");
    for (size_t i = 0; i + KEY.n <= s.n; i++) {
        if (memcmp(s.p + i, KEY.p, KEY.n) != 0) continue;
        size_t j = i + KEY.n;
        if (j < s.n && s.p[j] != '"' && s.p[j] != '\'' && s.p[j] != ':' && !is_sp(s.p[j])) continue;
        while (j < s.n && (s.p[j] == '"' || s.p[j] == '\'' || s.p[j] == ':' || is_sp(s.p[j]))) j++;
        Str w = {.p = s.p + j, .n = 0};
        while (j < s.n && (unsigned)((s.p[j] | 32) - 'a') < 26u) j++, w.n++;
        if (str_eq(w, S("dark"))) return CMM_THEME_DARK;
        if (str_eq(w, S("forest"))) return CMM_THEME_FOREST;
        if (str_eq(w, S("neutral"))) return CMM_THEME_NEUTRAL;
        if (str_eq(w, S("default")) || str_eq(w, S("base"))) return CMM_THEME_DEFAULT;
    }
    return CMM_THEME_AUTO;
}

static void apply_cfg_text(Config *c, Str s) {
    double v;
    cmm_theme t = parse_theme_after(s);
    if (t != CMM_THEME_AUTO) c->theme = t;
    if ((v = parse_num_after(s, S("nodeSpacing"))) > 0) c->node_sep = v;
    if ((v = parse_num_after(s, S("rankSpacing"))) > 0) c->rank_sep = v;
}

static bool next_line(Str src, size_t *i, Str *line) {
    if (*i >= src.n) return false;
    const char *nl = memchr(src.p + *i, '\n', src.n - *i);
    size_t e = nl ? (size_t)(nl - src.p) : src.n;
    *line = (Str){.p = src.p + *i, .n = e - *i};
    *i = e + (nl ? 1 : 0);
    return true;
}

/* skip frontmatter, directives, blank lines and comments; return offset of diagram keyword */
static size_t preamble(Str src, Config *c) {
    size_t i = 0;
    if (src.n >= 3 && memcmp(src.p, "\xEF\xBB\xBF", 3) == 0) i = 3;
    size_t save = i;
    Str line = {};
    /* frontmatter */
    while (next_line(src, &i, &line) && str_trim(line).n == 0) {}
    if (str_eq(str_trim(line), S("---"))) {
        size_t fm0 = i;
        bool closed = false;
        while (next_line(src, &i, &line)) {
            Str t = str_trim(line);
            if (str_eq(t, S("---"))) { closed = true; break; }
            if (line.n && !is_sp(line.p[0]) && str_starts(t, S("title:"))) {
                Str v = str_trim((Str){.p = t.p + 6, .n = t.n - 6});
                if (v.n >= 2 && (v.p[0] == '"' || v.p[0] == '\'') && v.p[v.n - 1] == v.p[0]) v.p++, v.n -= 2;
                c->title = v;
            }
        }
        if (closed) apply_cfg_text(c, (Str){.p = src.p + fm0, .n = i - fm0});
        else i = save;
    } else i = save;
    for (;;) {
        size_t ls = i;
        if (!next_line(src, &i, &line)) return src.n;
        Str t = str_trim(line);
        if (!t.n) continue;
        if (str_starts(t, S("%%{"))) {
            /* directive may span lines until }%% */
            size_t j = (size_t)(t.p - src.p);
            const char *end = nullptr;
            for (size_t k = j; k + 3 <= src.n; k++)
                if (src.p[k] == '}' && src.p[k + 1] == '%' && src.p[k + 2] == '%') { end = src.p + k; break; }
            if (!end) return ls;
            apply_cfg_text(c, (Str){.p = src.p + j, .n = (size_t)(end - (src.p + j))});
            i = (size_t)(end - src.p) + 3;
            continue;
        }
        if (str_starts(t, S("%%"))) continue;
        return (size_t)(t.p - src.p);
    }
}

static void set_err(cmm_error *err, cmm_status st, size_t n, const char src[static n], size_t off, const char *msg) {
    if (!err) return;
    *err = (cmm_error){.code = st};
    if (st == CMM_ERR_SYNTAX) {
        uint32_t line = 1, col = 1;
        for (size_t i = 0; i < off && i < n; i++) {
            if (src[i] == '\n') line++, col = 1;
            else col++;
        }
        err->line = line;
        err->col = col;
    }
    snprintf(err->message, sizeof err->message, "%s", msg && *msg ? msg : cmm_status_str(st));
}

static cmm_status render(Arena *ap, Buf *bp, size_t n, const char src[static n], const cmm_options *opt,
                         cmm_error *err) {
    if (n > (size_t)1 << 30) { set_err(err, CMM_ERR_LIMIT, n, src, 0, nullptr); return CMM_ERR_LIMIT; }
    Config cfg = {.node_sep = 50, .rank_sep = 50};
    Str s = {.p = src, .n = n};
    size_t off = preamble(s, &cfg);
    if (opt && opt->node_spacing > 0) cfg.node_sep = opt->node_spacing;
    if (opt && opt->rank_spacing > 0) cfg.rank_sep = opt->rank_spacing;
    double fs = opt && opt->font_size > 0 ? opt->font_size : 16;
    if (off >= n) { set_err(err, CMM_ERR_EMPTY, n, src, 0, nullptr); return CMM_ERR_EMPTY; }
    size_t k = off;
    while (k < n && !is_sp(src[k]) && src[k] != '\n' && src[k] != ';') k++;
    Str kw = {.p = src + off, .n = k - off};

    Arena arena = *ap;
    Buf b = *bp;
    b.n = 0;
    b.oom = false;
    char msg[160] = {};
    size_t eoff = 0;
    cmm_status st = CMM_OK;
    dcfg_begin(&arena, n, src, off); /* %%{init}%% / frontmatter keys + accTitle/accDescr, all diagram kinds */
    dcfg_set_theme(opt && opt->theme ? opt->theme : cfg.theme);
    if (str_eq(kw, S("graph")) || str_eq(kw, S("flowchart")) || str_eq(kw, S("flowchart-elk"))) {
        dcfg_set_theme(0); /* flowchart applies named themes itself (Graph.theme) */
        Graph g = {.arena = &arena, .title = cfg.title,
                   .theme = (uint8_t)(opt && opt->theme ? opt->theme : cfg.theme ? cfg.theme : CMM_THEME_DEFAULT),
                   .flags = opt ? opt->flags : 0, .node_sep = cfg.node_sep,
                   .rank_sep = cfg.rank_sep, .font_size = fs};
        /* config: flowchart.nodeSpacing / rankSpacing (init or frontmatter; API options win), cycleRemoval */
        if (!(opt && opt->node_spacing > 0)) g.node_sep = dcfg_num("flowchart.nodeSpacing", g.node_sep);
        if (!(opt && opt->rank_spacing > 0)) g.rank_sep = dcfg_num("flowchart.rankSpacing", g.rank_sep);
        Str cr = dcfg_get("flowchart.cycleRemoval");
        g.acyclicer = str_eq(cr, S("dfs")) ? CMM_ACYC_DFS : str_eq(cr, S("greedy")) ? CMM_ACYC_GREEDY : CMM_ACYC_AUTO;
        if (g.flags & CMM_F_FAS_DFS) g.acyclicer = CMM_ACYC_DFS;
        if (g.flags & CMM_F_FAS_GREEDY) g.acyclicer = CMM_ACYC_GREEDY;
        Str cv = dcfg_get("flowchart.curve");
        g.curve = flow_curve_kind(cv);
        if ((st = flow_parse(&g, n, src, off, &eoff, msg))) goto fail;
        if ((st = flow_layout(&g))) goto fail;
        if (!buf_reserve(&b, 256 + g.nn * 320 + g.ne * 200)) { st = CMM_ERR_NOMEM; goto fail; }
        if ((st = flow_svg(&g, &b))) goto fail;
    } else if (str_eq(kw, S("sequenceDiagram"))) {
        if ((st = seq_render(&arena, n, src, off, cfg.title, fs, &b, &eoff, msg))) goto fail;
    } else if (diag_lookup(kw)) {
        if ((st = diag_lookup(kw)(&arena, n, src, off, cfg.title, fs, &b, &eoff, msg))) goto fail;
    } else {
        st = CMM_ERR_UNSUPPORTED;
        snprintf(msg, sizeof msg, "unsupported diagram type '%.*s'", (int)(kw.n > 40 ? 40 : kw.n), kw.p);
        eoff = off;
        goto fail;
    }
    if (b.oom) { st = CMM_ERR_NOMEM; goto fail; }
    b.p[b.n] = 0;
    if (err) *err = (cmm_error){};
fail:
    dcfg_end();
    *ap = arena;
    *bp = b;
    if (st) set_err(err, st, n, src, eoff, msg);
    return st;
}

cmm_status cmm_render_svg(size_t n, const char src[static n], const cmm_options *opt, cmm_buffer *out,
                          cmm_error *err) {
    if (out) *out = (cmm_buffer){};
    if (!out) return CMM_ERR_EMPTY;
    Arena a = {};
    Buf b = {};
    cmm_status st = render(&a, &b, n, src, opt, err);
    arena_free(&a);
    if (st) { free(b.p); return st; }
    out->data = b.p;
    out->len = b.n;
    return CMM_OK;
}

struct cmm_context {
    Arena arena;
    Buf out;
};

cmm_context *cmm_context_new(void) {
    cmm_context *c = calloc(1, sizeof *c);
    return c;
}

void cmm_context_free(cmm_context *c) {
    if (!c) return;
    arena_free(&c->arena);
    free(c->out.p);
    free(c);
}

cmm_status cmm_context_render_svg(cmm_context *c, size_t n, const char src[static n], const cmm_options *opt,
                                  const char **svg, size_t *len, cmm_error *err) {
    if (svg) *svg = nullptr;
    if (len) *len = 0;
    if (!c || !svg) return CMM_ERR_EMPTY;
    arena_reset(&c->arena);
    cmm_status st = render(&c->arena, &c->out, n, src, opt, err);
    if (st) return st;
    *svg = c->out.p;
    if (len) *len = c->out.n;
    return CMM_OK;
}
