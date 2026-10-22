#include "cmermaid.h"

#include <stdio.h>
#include <stdlib.h>

#include "graph.h"
#include "seq.h"

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

static void apply_cfg_text(Config *c, Str s) {
    double v;
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

cmm_status cmm_render_svg(size_t n, const char src[static n], const cmm_options *opt, cmm_buffer *out,
                          cmm_error *err) {
    if (out) *out = (cmm_buffer){};
    if (!out) return CMM_ERR_EMPTY;
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

    Arena arena = {};
    Buf b = {};
    char msg[160] = {};
    size_t eoff = 0;
    cmm_status st = CMM_OK;
    if (str_eq(kw, S("graph")) || str_eq(kw, S("flowchart")) || str_eq(kw, S("flowchart-elk"))) {
        Graph g = {.arena = &arena, .title = cfg.title, .node_sep = cfg.node_sep,
                   .rank_sep = cfg.rank_sep, .font_size = fs};
        if ((st = flow_parse(&g, n, src, off, &eoff, msg))) goto fail;
        if ((st = flow_layout(&g))) goto fail;
        if (!buf_reserve(&b, 256 + g.nn * 320 + g.ne * 200)) { st = CMM_ERR_NOMEM; goto fail; }
        if ((st = flow_svg(&g, &b))) goto fail;
    } else if (str_eq(kw, S("sequenceDiagram"))) {
        if ((st = seq_render(&arena, n, src, off, cfg.title, fs, &b, &eoff, msg))) goto fail;
    } else {
        st = CMM_ERR_UNSUPPORTED;
        snprintf(msg, sizeof msg, "unsupported diagram type '%.*s'", (int)(kw.n > 40 ? 40 : kw.n), kw.p);
        eoff = off;
        goto fail;
    }
    if (b.oom) { st = CMM_ERR_NOMEM; goto fail; }
    arena_free(&arena);
    b.p[b.n] = 0;
    out->data = b.p;
    out->len = b.n;
    if (err) *err = (cmm_error){};
    return CMM_OK;
fail:
    arena_free(&arena);
    free(b.p);
    set_err(err, st, n, src, eoff, msg);
    return st;
}
