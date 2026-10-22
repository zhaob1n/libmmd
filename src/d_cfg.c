#include "dcfg.h"

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static thread_local DCfg cur;
static thread_local Arena *cur_a;
static thread_local size_t cur_cap;

static constexpr size_t MAX_KV = 512;

static void kv_put(Str k, Str v) {
    static const char *const PRE[] = {"init.", "initialize.", "config."};
    for (size_t i = 0; i < countof(PRE); i++) {
        Str p = {.p = PRE[i], .n = strlen(PRE[i])};
        if (str_starts(k, p)) { k.p += p.n; k.n -= p.n; break; }
    }
    if (!k.n || cur.n >= MAX_KV) return;
    if (cur.n == cur_cap) {
        size_t nc = cur_cap ? cur_cap * 2 : 32;
        DKV *nv = ANEW(cur_a, DKV, nc);
        if (!nv) return;
        for (size_t i = 0; i < cur.n; i++) nv[i] = cur.kv[i];
        cur.kv = nv, cur_cap = nc;
    }
    cur.kv[cur.n++] = (DKV){.k = k, .v = v};
}

/* dotted key = prefix + "." + k, in the arena */
static Str join(Str pre, Str k) {
    if (!pre.n) return k;
    char *p = ANEW(cur_a, char, pre.n + 1 + k.n);
    if (!p) return (Str){};
    memcpy(p, pre.p, pre.n), p[pre.n] = '.', memcpy(p + pre.n + 1, k.p, k.n);
    return (Str){.p = p, .n = pre.n + 1 + k.n};
}

/* ---- relaxed JSON: quoted or bare keys/values, nested objects flattened, arrays skipped ---- */
typedef struct JP { const char *s; size_t n, i; } JP;

static void jp_ws(JP *p) {
    while (p->i < p->n && (is_sp(p->s[p->i]) || p->s[p->i] == '\n')) p->i++;
}
static Str jp_atom(JP *p) {
    jp_ws(p);
    if (p->i >= p->n) return (Str){};
    char q = p->s[p->i];
    if (q == '"' || q == '\'') {
        size_t b = ++p->i;
        while (p->i < p->n && p->s[p->i] != q) p->i += p->s[p->i] == '\\' && p->i + 1 < p->n ? 2 : 1;
        Str r = {.p = p->s + b, .n = (p->i < p->n ? p->i : p->n) - b};
        if (p->i < p->n) p->i++;
        return r;
    }
    size_t b = p->i;
    while (p->i < p->n && !strchr(",:{}[]\n", p->s[p->i])) p->i++;
    return str_trim((Str){.p = p->s + b, .n = p->i - b});
}
static void jp_skip(JP *p, int depth);
static void jp_obj(JP *p, Str pre, int depth) { /* at '{' */
    p->i++;
    while (p->i < p->n) {
        jp_ws(p);
        if (p->i >= p->n) return;
        if (p->s[p->i] == '}') { p->i++; return; }
        if (p->s[p->i] == ',') { p->i++; continue; }
        size_t i0 = p->i;
        Str k = jp_atom(p);
        jp_ws(p);
        if (p->i < p->n && p->s[p->i] == ':') {
            p->i++;
            jp_ws(p);
            if (p->i < p->n && p->s[p->i] == '{') {
                if (depth < 16) jp_obj(p, join(pre, k), depth + 1);
                else jp_skip(p, depth);
            } else if (p->i < p->n && p->s[p->i] == '[') jp_skip(p, depth);
            else kv_put(join(pre, k), jp_atom(p));
        }
        if (p->i == i0) p->i++; /* guarantee progress */
    }
}
static void jp_skip(JP *p, int depth) { /* skip a balanced {..} or [..] */
    (void)depth;
    int d = 0;
    while (p->i < p->n) {
        char c = p->s[p->i++];
        if (c == '"' || c == '\'') {
            while (p->i < p->n && p->s[p->i] != c) p->i++;
            if (p->i < p->n) p->i++;
        } else if (c == '{' || c == '[') d++;
        else if ((c == '}' || c == ']') && --d <= 0) return;
    }
}

static Str unq(Str v) {
    v = str_trim(v);
    if (v.n >= 2 && (v.p[0] == '"' || v.p[0] == '\'') && v.p[v.n - 1] == v.p[0]) v.p++, v.n -= 2;
    return v;
}

/* ---- YAML subset: block mappings by indentation, '#' comments, inline {} mappings ---- */
static void yaml(Str s) {
    Str stack[16];
    unsigned ind[16];
    int sp = 0;
    size_t i = 0;
    while (i < s.n) {
        const char *nl = memchr(s.p + i, '\n', s.n - i);
        size_t e = nl ? (size_t)(nl - s.p) : s.n;
        Str ln = {.p = s.p + i, .n = e - i};
        i = nl ? e + 1 : e;
        unsigned in = 0;
        while (in < ln.n && (ln.p[in] == ' ' || ln.p[in] == '\t')) in++;
        Str t = str_trim(ln);
        if (!t.n || t.p[0] == '#' || t.p[0] == '-') continue;
        /* strip trailing comment outside quotes */
        char q = 0;
        for (size_t k = 0; k < t.n; k++) {
            if (q) { if (t.p[k] == q) q = 0; }
            else if (t.p[k] == '"' || t.p[k] == '\'') q = t.p[k];
            else if (t.p[k] == '#' && k && is_sp(t.p[k - 1])) { t.n = k; break; }
        }
        t = str_trim(t);
        const char *c = memchr(t.p, ':', t.n);
        if (!c) continue;
        while (sp > 0 && ind[sp - 1] >= in) sp--;
        Str k = unq((Str){.p = t.p, .n = (size_t)(c - t.p)});
        Str v = str_trim((Str){.p = c + 1, .n = t.n - (size_t)(c - t.p) - 1});
        Str pre = sp ? stack[sp - 1] : (Str){};
        Str full = join(pre, k);
        if (!v.n) {
            if (sp < 16) stack[sp] = full, ind[sp] = in, sp++;
        } else if (v.p[0] == '{') {
            JP p = {.s = v.p, .n = v.n};
            jp_obj(&p, full, 1);
        } else kv_put(full, unq(v));
    }
}

static void directive(Str body) { /* text after "%%{" up to "}%%" */
    body = str_trim(body);
    const char *c = memchr(body.p, ':', body.n);
    if (!c) return;
    Str name = unq((Str){.p = body.p, .n = (size_t)(c - body.p)});
    if (!str_eq(name, S("init")) && !str_eq(name, S("initialize"))) return;
    JP p = {.s = c + 1, .n = body.n - (size_t)(c + 1 - body.p)};
    jp_ws(&p);
    if (p.i < p.n && p.s[p.i] == '{') jp_obj(&p, (Str){}, 1);
}

bool dcfg_is_acc(Str t, bool *block) {
    *block = false;
    if (str_starts(t, S("accTitle")) || str_starts(t, S("accDescr"))) {
        Str r = str_trim((Str){.p = t.p + 8, .n = t.n - 8});
        if (r.n && r.p[0] == ':') return true;
        if (r.n && r.p[0] == '{' && str_starts(t, S("accDescr"))) { *block = true; return true; }
    }
    return false;
}

void dcfg_begin(Arena *a, size_t n, const char src[static n], size_t off) {
    cur = (DCfg){};
    cur_a = a, cur_cap = 0;
    size_t i = 0;
    /* preamble: frontmatter + directives */
    while (i < off) {
        const char *nl = memchr(src + i, '\n', off - i);
        size_t e = nl ? (size_t)(nl - src) : off;
        Str t = str_trim((Str){.p = src + i, .n = e - i});
        size_t nxt = nl ? e + 1 : e;
        if (str_eq(t, S("---")) && !cur.n) {
            size_t j = nxt, b = nxt;
            while (j < off) {
                const char *n2 = memchr(src + j, '\n', off - j);
                size_t e2 = n2 ? (size_t)(n2 - src) : off;
                if (str_eq(str_trim((Str){.p = src + j, .n = e2 - j}), S("---"))) { yaml((Str){.p = src + b, .n = j - b}); break; }
                j = n2 ? e2 + 1 : e2;
            }
            nxt = j < off ? j : off;
            const char *n3 = memchr(src + nxt, '\n', off - nxt);
            nxt = n3 ? (size_t)(n3 - src) + 1 : off;
        } else if (str_starts(t, S("%%{"))) {
            const char *s0 = t.p + 3, *end = nullptr;
            for (const char *q = s0; q + 2 < src + off; q++)
                if (q[0] == '}' && q[1] == '%' && q[2] == '%') { end = q; break; }
            if (end) {
                directive((Str){.p = s0, .n = (size_t)(end - s0)});
                const char *n3 = memchr(end, '\n', (size_t)(src + off - end));
                nxt = n3 ? (size_t)(n3 - src) + 1 : off;
            }
        }
        i = nxt > i ? nxt : i + 1;
    }
    /* body: acc lines */
    i = off;
    while (i < n) {
        const char *nl = memchr(src + i, '\n', n - i);
        size_t e = nl ? (size_t)(nl - src) : n;
        Str t = str_trim((Str){.p = src + i, .n = e - i});
        i = nl ? e + 1 : e;
        bool blk;
        if (!dcfg_is_acc(t, &blk)) continue;
        if (blk) {
            const char *ob = memchr(t.p, '{', t.n);
            size_t b = (size_t)(ob + 1 - src);
            const char *cb = memchr(src + b, '}', n - b);
            size_t ce = cb ? (size_t)(cb - src) : n;
            cur.acc_descr = str_trim((Str){.p = src + b, .n = ce - b});
            if (cb) { const char *n2 = memchr(cb, '\n', n - ce); i = n2 ? (size_t)(n2 - src) + 1 : n; }
            else i = n;
        } else {
            const char *c = memchr(t.p, ':', t.n);
            Str v = str_trim((Str){.p = c + 1, .n = t.n - (size_t)(c + 1 - t.p)});
            if (t.p[3] == 'T') cur.acc_title = v; else cur.acc_descr = v;
        }
    }
}

void dcfg_end(void) { cur = (DCfg){}; cur_a = nullptr; cur_cap = 0; }
const DCfg *dcfg_cur(void) { return &cur; }

Str dcfg_get(const char *key) {
    Str k = {.p = key, .n = strlen(key)};
    for (size_t i = cur.n; i-- > 0;)
        if (str_eq(cur.kv[i].k, k)) return cur.kv[i].v;
    return (Str){};
}

double dcfg_num(const char *key, double def) {
    Str v = dcfg_get(key);
    if (!v.n || v.n > 31) return def;
    char tmp[32];
    memcpy(tmp, v.p, v.n), tmp[v.n] = 0;
    char *e;
    double d = strtod(tmp, &e);
    if (e == tmp || !isfinite(d) || fabs(d) > 1e6) return def;
    if (*e && strcmp(e, "px")) return def;
    return d;
}

bool dcfg_bool(const char *key, bool def) {
    Str v = dcfg_get(key);
    if (str_eq(v, S("true"))) return true;
    if (str_eq(v, S("false"))) return false;
    return def;
}

Str dcfg_css(const char *key) {
    Str v = dcfg_get(key);
    if (!v.n || v.n > 200) return (Str){};
    for (size_t i = 0; i < v.n; i++) {
        unsigned char c = (unsigned char)v.p[i];
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || strchr("#%.,()- '", c);
        if (!ok || !c) return (Str){};
    }
    /* reject url( / expression( even though the charset allows parentheses */
    for (size_t i = 0; i + 3 < v.n; i++)
        if ((v.p[i] | 32) == 'u' && (v.p[i + 1] | 32) == 'r' && (v.p[i + 2] | 32) == 'l' && v.p[i + 3] == '(') return (Str){};
    return v;
}

Str dcfg_background(void) { return dcfg_css("themeVariables.background"); }

/* themeVariable -> CSS template ('$' = value); kind "*" applies to every diagram */
typedef struct TV { const char *kind, *var, *tpl; } TV;
static const TV TVS[] = {
    {"*", "fontFamily", "#mermaid{font-family:$;}"},
    {"*", "fontSize", "#mermaid{font-size:$;}"},
    {"*", "textColor", "#mermaid{fill:$;}"},
    {"flowchart", "primaryColor", "#mermaid .node rect,#mermaid .node circle,#mermaid .node ellipse,#mermaid .node polygon,#mermaid .node path{fill:$;}"},
    {"flowchart", "mainBkg", "#mermaid .node rect,#mermaid .node circle,#mermaid .node ellipse,#mermaid .node polygon,#mermaid .node path{fill:$;}"},
    {"flowchart", "primaryBorderColor", "#mermaid .node rect,#mermaid .node circle,#mermaid .node ellipse,#mermaid .node polygon,#mermaid .node path{stroke:$;}"},
    {"flowchart", "nodeBorder", "#mermaid .node rect,#mermaid .node circle,#mermaid .node ellipse,#mermaid .node polygon,#mermaid .node path{stroke:$;}"},
    {"flowchart", "primaryTextColor", "#mermaid .node text{fill:$;}"},
    {"flowchart", "nodeTextColor", "#mermaid .node text{fill:$;}"},
    {"flowchart", "lineColor", "#mermaid .flowchart-link{stroke:$;}#mermaid .marker{fill:$;stroke:$;}"},
    {"flowchart", "edgeLabelBackground", "#mermaid .edgeLabel rect{fill:$;}"},
    {"flowchart", "clusterBkg", "#mermaid .cluster rect{fill:$;}"},
    {"flowchart", "clusterBorder", "#mermaid .cluster rect{stroke:$;}"},
    {"flowchart", "titleColor", "#mermaid .cluster-label,#mermaid .title{fill:$;}"},
    {"statediagram", "primaryColor", "#mermaid .st{fill:$;}"},
    {"statediagram", "primaryBorderColor", "#mermaid .st{stroke:$;}"},
    {"statediagram", "lineColor", "#mermaid .tr{stroke:$;}"},
    {"statediagram", "noteBkgColor", "#mermaid .note{fill:$;}"},
    {"statediagram", "noteBorderColor", "#mermaid .note{stroke:$;}"},
    {"classDiagram", "primaryColor", "#mermaid .cls{fill:$;}"},
    {"classDiagram", "primaryBorderColor", "#mermaid .cls,#mermaid .cdiv{stroke:$;}"},
    {"classDiagram", "lineColor", "#mermaid .rel{stroke:$;}"},
    {"classDiagram", "noteBkgColor", "#mermaid .note{fill:$;}"},
    {"erDiagram", "primaryColor", "#mermaid .ent{fill:$;}"},
    {"erDiagram", "primaryBorderColor", "#mermaid .ent,#mermaid .row{stroke:$;}"},
    {"erDiagram", "lineColor", "#mermaid .rel{stroke:$;}"},
    {"requirementDiagram", "primaryColor", "#mermaid .rq{fill:$;}"},
    {"requirementDiagram", "primaryBorderColor", "#mermaid .rq,#mermaid .rh{stroke:$;}"},
    {"requirementDiagram", "lineColor", "#mermaid .rr{stroke:$;}"},
    {"block", "primaryColor", "#mermaid .bn{fill:$;}"},
    {"block", "primaryBorderColor", "#mermaid .bn{stroke:$;}"},
    {"block", "lineColor", "#mermaid .be{stroke:$;}"},
    {"kanban", "primaryBorderColor", "#mermaid .kc{stroke:$;}"},
    {"gantt", "taskBkgColor", "#mermaid .task{fill:$;}"},
    {"gantt", "taskBorderColor", "#mermaid .task{stroke:$;}"},
    {"gantt", "activeTaskBkgColor", "#mermaid .active{fill:$;}"},
    {"gantt", "activeTaskBorderColor", "#mermaid .active{stroke:$;}"},
    {"gantt", "doneTaskBkgColor", "#mermaid .done{fill:$;}"},
    {"gantt", "doneTaskBorderColor", "#mermaid .done{stroke:$;}"},
    {"gantt", "critBkgColor", "#mermaid .crit{fill:$;}"},
    {"gantt", "critBorderColor", "#mermaid .crit{stroke:$;}"},
    {"gantt", "taskTextColor", "#mermaid .taskText{fill:$;}"},
    {"gantt", "taskTextOutsideColor", "#mermaid .taskTextOutside{fill:$;}"},
    {"gantt", "sectionBkgColor", "#mermaid .sec0,#mermaid .sec2{fill:$;}"},
    {"gantt", "altSectionBkgColor", "#mermaid .sec1,#mermaid .sec3{fill:$;}"},
    {"gantt", "gridColor", "#mermaid .grid{stroke:$;}"},
    {"gantt", "excludeBkgColor", "#mermaid .excl{fill:$;}"},
    {"pie", "pieStrokeColor", "#mermaid .slice{stroke:$;}"},
    {"pie", "pieOuterStrokeColor", "#mermaid .outer{stroke:$;}"},
    {"pie", "pieOpacity", "#mermaid .slice{opacity:$;}"},
    {"pie", "pieTitleTextColor", "#mermaid .title{fill:$;}"},
    {"pie", "pieSectionTextColor", "#mermaid .sliceText{fill:$;}"},
    {"pie", "pieLegendTextColor", "#mermaid .legend text{fill:$;}"},
    {"quadrantChart", "quadrant1Fill", "#mermaid .q1{fill:$;}"},
    {"quadrantChart", "quadrant2Fill", "#mermaid .q2{fill:$;}"},
    {"quadrantChart", "quadrant3Fill", "#mermaid .q3{fill:$;}"},
    {"quadrantChart", "quadrant4Fill", "#mermaid .q4{fill:$;}"},
    {"quadrantChart", "quadrant1TextFill", "#mermaid .q1t{fill:$;}"},
    {"quadrantChart", "quadrantPointFill", "#mermaid .qp{fill:$;}"},
    {"quadrantChart", "quadrantInternalBorderStrokeFill", "#mermaid .qib{stroke:$;}"},
    {"quadrantChart", "quadrantExternalBorderStrokeFill", "#mermaid .qeb{stroke:$;}"},
    {"xychart", "xyChart.backgroundColor", "#mermaid .bg{fill:$;}"},
    {"xychart", "xyChart.titleColor", "#mermaid .title{fill:$;}"},
    {"xychart", "xyChart.xAxisLineColor", "#mermaid .xaxis line,#mermaid .xaxis path{stroke:$;}"},
    {"xychart", "xyChart.yAxisLineColor", "#mermaid .yaxis line,#mermaid .yaxis path{stroke:$;}"},
    {"xychart", "xyChart.xAxisLabelColor", "#mermaid .xaxis text{fill:$;}"},
    {"xychart", "xyChart.yAxisLabelColor", "#mermaid .yaxis text{fill:$;}"},
    {"gitGraph", "commitLabelColor", "#mermaid .commit-label{fill:$;}"},
    {"gitGraph", "commitLabelBackground", "#mermaid .commit-label-bkg{fill:$;}"},
    {"gitGraph", "tagLabelColor", "#mermaid .tag-label{fill:$;}"},
    {"gitGraph", "tagLabelBackground", "#mermaid .tag-label-bkg{fill:$;}"},
    {"gitGraph", "tagLabelBorder", "#mermaid .tag-label-bkg{stroke:$;}"},
    {"packet", "packet.blockFillColor", "#mermaid .packetBlock{fill:$;}"},
    {"packet", "packet.blockStrokeColor", "#mermaid .packetBlock{stroke:$;}"},
    {"packet", "packet.labelColor", "#mermaid .packetLabel{fill:$;}"},
    {"packet", "packet.titleColor", "#mermaid .packetTitle{fill:$;}"},
    {"radar", "radar.axisColor", "#mermaid .radarAxisLine{stroke:$;}"},
    {"radar", "radar.graticuleColor", "#mermaid .radarGraticule{stroke:$;}"},
    {"c4", "lineColor", "#mermaid .cr{stroke:$;}#mermaid .crh{fill:$;}"},
    {"architecture", "lineColor", "#mermaid .edge{stroke:$;}"},
    {"sequence", "actorBkg", "#mermaid .actor{fill:$;}"},
    {"sequence", "actorBorder", "#mermaid .actor{stroke:$;}"},
    {"sequence", "actorTextColor", "#mermaid .actor-text{fill:$;}"},
    {"sequence", "actorLineColor", "#mermaid .actor-line{stroke:$;}"},
    {"sequence", "signalColor", "#mermaid .messageLine0,#mermaid .messageLine1{stroke:$;}#mermaid .arrowhead{fill:$;}"},
    {"sequence", "signalTextColor", "#mermaid .messageText{fill:$;}"},
    {"sequence", "noteBkgColor", "#mermaid .note{fill:$;}"},
    {"sequence", "noteBorderColor", "#mermaid .note{stroke:$;}"},
    {"sequence", "noteTextColor", "#mermaid .noteText{fill:$;}"},
    {"sequence", "activationBkgColor", "#mermaid .activation0,#mermaid .activation1,#mermaid .activation2{fill:$;}"},
    {"sequence", "activationBorderColor", "#mermaid .activation0,#mermaid .activation1,#mermaid .activation2{stroke:$;}"},
    {"sequence", "labelBoxBkgColor", "#mermaid .labelBox{fill:$;}"},
    {"sequence", "labelBoxBorderColor", "#mermaid .labelBox{stroke:$;}"},
    {"sequence", "sequenceNumberColor", "#mermaid .sequenceNumber{fill:$;}"},
    {"sankey", "textColor", "#mermaid .sankey-label{fill:$;}"},
};

void dcfg_theme_css(Buf *b, const char *kind) {
    if (!cur.n) return;
    char key[96];
    for (size_t i = 0; i < countof(TVS); i++) {
        if (strcmp(TVS[i].kind, "*") && strcmp(TVS[i].kind, kind)) continue;
        snprintf(key, sizeof key, "themeVariables.%s", TVS[i].var);
        Str v = dcfg_css(key);
        if (!v.n) continue;
        for (const char *t = TVS[i].tpl; *t; t++)
            if (*t == '$') buf_str(b, v); else buf_ch(b, *t);
    }
}

void dcfg_svg_aria(Buf *b) {
    if (cur.acc_title.n) buf_lit(b, "\" aria-labelledby=\"chart-title-mermaid");
    if (cur.acc_descr.n) buf_lit(b, "\" aria-describedby=\"chart-desc-mermaid");
    buf_lit(b, "\">");
    if (cur.acc_title.n) { buf_lit(b, "<title id=\"chart-title-mermaid\">"); buf_xml(b, cur.acc_title); buf_lit(b, "</title>"); }
    if (cur.acc_descr.n) { buf_lit(b, "<desc id=\"chart-desc-mermaid\">"); buf_xml(b, cur.acc_descr); buf_lit(b, "</desc>"); }
}
