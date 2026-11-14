#define _GNU_SOURCE
#include "dcfg.h"
#include "dgraph.h"
#include "mmd.h"
#include "mtheme.h"

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static thread_local DCfg cur;
static thread_local Arena *cur_a;
static thread_local size_t cur_cap;

static constexpr size_t MAX_KV = 512;
static thread_local Str cur_sect; /* the diagram's section of mermaid's config: flowchart, xyChart ... */
static thread_local Str cfg_to;   /* what a leading "config." becomes: nothing in the front matter,
                                   * cur_sect in a directive */

static Str join(Str pre, Str k);
static void kv_put(Str k, Str v) {
    static const Str PRE[] = {S("init."), S("initialize.")};
    for (size_t i = 0; i < countof(PRE); i++)
        if (str_starts(k, PRE[i])) { k.p += PRE[i].n; k.n -= PRE[i].n; break; }
    if (str_starts(k, S("config."))) k = join(cfg_to, (Str){.p = k.p + 7, .n = k.n - 7});
    if (!k.n || cur.n >= MAX_KV) return;
    if (cur.n == cur_cap) {
        size_t nc = cur_cap ? cur_cap * 2 : 32;
        DKV *nv = ANEW(cur_a, DKV, nc);
        if (!nv) return;
        if (cur.n) memcpy(nv, cur.kv, cur.n * sizeof *nv);
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

static void jp_ws(JP p[static 1]) {
    while (p->i < p->n && (is_sp(p->s[p->i]) || p->s[p->i] == '\n')) p->i++;
}
static Str jp_atom(JP p[static 1]) {
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
static void jp_skip(JP p[static 1], int depth);
static void jp_obj(JP p[static 1], Str pre, int depth) { /* at '{' */
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
static void jp_skip(JP p[static 1], int depth) { /* skip a balanced {..} or [..] */
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
        Str k = dg_unquote((Str){.p = t.p, .n = (size_t)(c - t.p)}, true);
        Str v = str_trim((Str){.p = c + 1, .n = t.n - (size_t)(c - t.p) - 1});
        Str pre = sp ? stack[sp - 1] : (Str){};
        Str full = join(pre, k);
        if (!v.n) {
            if (sp < 16) stack[sp] = full, ind[sp] = in, sp++;
        } else if (v.p[0] == '{') {
            JP p = {.s = v.p, .n = v.n};
            jp_obj(&p, full, 1);
        } else kv_put(full, dg_unquote(v, true));
    }
}

/* %%{init: ...}%% or %%{initialize: ...}%%: body is the text after "%%{" */
static bool init_directive(Str body, const char *c[static 1]) {
    body = str_trim(body);
    *c = memchr(body.p, ':', body.n);
    if (!*c) return false;
    Str name = dg_unquote((Str){.p = body.p, .n = (size_t)(*c - body.p)}, true);
    return str_eq(name, S("init")) || str_eq(name, S("initialize"));
}

/* a directive's `config` object lands in the diagram's section. mermaid's detectInit files it under
 * the detector id, which no renderer reads for class, state, xychart, flowchart-elk and the railroad
 * dialects, and loses it when there are several directives (sanitizeDirective drops the key):
 * settings the author wrote that silently do nothing, which libmmd does not copy */
static void directive(Str body) {
    const char *c;
    if (!init_directive(body, &c)) return;
    JP p = {.s = c + 1, .n = (size_t)(body.p + body.n - c - 1)};
    jp_ws(&p);
    cfg_to = cur_sect;
    if (p.i < p.n && p.s[p.i] == '{') jp_obj(&p, (Str){}, 1);
    cfg_to = (Str){};
}

bool dcfg_is_acc(Str t, bool block[static 1]) {
    *block = false;
    if (str_starts(t, S("accTitle")) || str_starts(t, S("accDescr"))) {
        Str r = str_trim((Str){.p = t.p + 8, .n = t.n - 8});
        if (r.n && r.p[0] == ':') return true;
        if (r.n && r.p[0] == '{' && str_starts(t, S("accDescr"))) { *block = true; return true; }
    }
    return false;
}

static thread_local int cur_theme; /* MMD_THEME_* chosen by CLI option or init/frontmatter */
static thread_local int api_theme; /* MMD_THEME_* the caller asked for, AUTO when the source decides */
static thread_local MThemeId eng_id, own_id; /* own_id: what a dcfg_theme_own() renderer draws */
static thread_local bool own_theme, derive, eng_tried, eng_user, user_vars; /* eng_user: eng holds the overrides */
static thread_local const MTheme *eng, *eng_base;

static MThemeId mt_of(int theme) {
    return theme == MMD_THEME_DARK ? MT_DARK : theme == MMD_THEME_FOREST ? MT_FOREST
         : theme == MMD_THEME_NEUTRAL ? MT_NEUTRAL : MT_DEFAULT;
}

/* a value mermaid's readAppearance takes: a theme in its registry (or 'null'), a look it knows */
static bool appearance_ok(Str key, Str v) {
    static const Str THEMES[] = {S("base"), S("dark"), S("default"), S("forest"), S("neutral"), S("neo"), S("neo-dark"),
                                 S("redux"), S("redux-dark"), S("redux-color"), S("redux-dark-color"), S("null")};
    static const Str LOOKS[] = {S("classic"), S("handDrawn"), S("neo")};
    bool theme = str_eq(key, S("theme"));
    const Str *ok = theme ? THEMES : LOOKS;
    for (size_t i = 0, n = theme ? countof(THEMES) : countof(LOOKS); i < n; i++)
        if (str_eq(v, ok[i])) return true;
    return false;
}

/* theme or look as mermaid resolves it: the diagram's section first, then the top level */
static Str appearance(Str key) {
    char k[48];
    int kn = snprintf(k, sizeof k, "%.*s.%.*s", (int)cur_sect.n, cur_sect.p, (int)key.n, key.p);
    Str v = cur_sect.n && kn > 0 && (size_t)kn < sizeof k ? dcfg_get_str((Str){.p = k, .n = (size_t)kn}) : (Str){};
    if (appearance_ok(key, v)) return v;
    v = dcfg_get_str(key);
    return appearance_ok(key, v) ? v : (Str){};
}

static int theme_named(Str w) {
    if (str_eq(w, S("dark"))) return MMD_THEME_DARK;
    if (str_eq(w, S("forest"))) return MMD_THEME_FOREST;
    if (str_eq(w, S("neutral"))) return MMD_THEME_NEUTRAL;
    if (str_eq(w, S("default")) || str_eq(w, S("base"))) return MMD_THEME_DEFAULT;
    return MMD_THEME_AUTO;
}

void dcfg_begin(Arena a[static 1], size_t n, const char src[static n], size_t off, Str sect, int api) {
    cur = (DCfg){};
    cur_a = a, cur_cap = 0, cur_sect = sect, api_theme = api;
    size_t i = off >= 3 && !memcmp(src, "\xEF\xBB\xBF", 3) ? 3 : 0; /* a BOM, as preamble() skips it */
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
                nxt = (size_t)(end - src) + 3; /* another directive may follow on the line */
            }
        }
        i = nxt > i ? nxt : i + 1;
    }
    Str tname = appearance(S("theme"));
    cur_theme = api ? api : theme_named(tname);
    eng_id = !api && str_eq(tname, S("base")) ? MT_BASE : mt_of(cur_theme);
    /* mermaid derives from the themeVariables only when the source names a theme or the theme
     * differs from the site's (updateCurrentConfig); else they land, as given, on the site theme's */
    bool named = dcfg_get_str(S("theme")).n;
    derive = api ? named : !str_eq(tname, S("null")) && (named || (tname.n && !str_eq(tname, S("default"))));
    /* body: acc lines (one memmem pass first: most diagrams have none) */
    if (n <= off || !memmem(src + off, n - off, "acc", 3)) return;
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

void dcfg_end(void) {
    cur_theme = api_theme = 0;
    cur = (DCfg){}, cur_a = nullptr, cur_cap = 0, cur_sect = (Str){};
    own_theme = derive = eng_tried = eng_user = user_vars = false, eng = eng_base = nullptr;
}
const DCfg *dcfg_cur(void) { return &cur; }

Str dcfg_get_str(Str key) {
    for (size_t i = cur.n; i-- > 0;)
        if (str_eq(cur.kv[i].k, key)) return cur.kv[i].v;
    return (Str){};
}

Str dcfg_get(const char key[static 1]) { return dcfg_get_str((Str){.p = key, .n = strlen(key)}); }

double dcfg_num(const char key[static 1], double def) {
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

/* positive only: mermaid's `a || b` takes 0 as unset, and a negative spacing only folds the
 * layout onto itself */
static double pos_num(const char key[static 1]) {
    double d = dcfg_num(key, 0);
    return d > 0 ? d : 0;
}

double dcfg_spacing(const char sect[static 1], bool rank, double def) {
    const char *k = rank ? "rankSpacing" : "nodeSpacing";
    char sk[48];
    double v = pos_num(k);
    if (!v && snprintf(sk, sizeof sk, "%s.%s", sect, k) < (int)sizeof sk) v = pos_num(sk);
    return v ? v : def;
}

bool dcfg_bool(const char key[static 1], bool def) {
    Str v = dcfg_get(key);
    if (str_eq(v, S("true"))) return true;
    if (str_eq(v, S("false"))) return false;
    return def;
}

/* v if it is safe inside CSS / an attribute, else empty */
static Str css_safe(Str v) {
    if (!v.n || v.n > 200) return (Str){};
    for (size_t i = 0; i < v.n; i++) {
        unsigned char c = (unsigned char)v.p[i];
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || strchr("#%.,()- '", c);
        if (!ok || !c) return (Str){};
    }
    /* reject url( / expression( even though the charset allows parentheses (and spaces before them) */
    for (size_t i = 0; i < v.n; i++) {
        if (v.p[i] != '(') continue;
        size_t e = i;
        while (e && v.p[e - 1] == ' ') e--;
        size_t s = e;
        while (s && (unsigned)((v.p[s - 1] | 32) - 'a') < 26u) s--;
        Str fn = {.p = v.p + s, .n = e - s};
        if (str_ieq(fn, S("url")) || str_ieq(fn, S("expression"))) return (Str){};
    }
    return v;
}

Str dcfg_css(const char key[static 1]) { return css_safe(dcfg_get(key)); }

/* mermaid's theme engine (src/mtheme.c) for the named theme with the source's themeVariables when
 * mermaid derives from them, and for what the renderer draws on its own: the default theme, or for
 * a renderer that draws named themes itself (dcfg_theme_own) that theme. dcfg_tv reports the
 * variables where the two differ. Built on first use; nothing to build when there is nothing to
 * derive and the themes agree. */
static bool engine(void) {
    if (eng_tried) return eng;
    eng_tried = true;
    size_t nov = 0;
    for (size_t i = 0; i < cur.n; i++) nov += str_starts(cur.kv[i].k, S("themeVariables."));
    user_vars = nov > 0;
    if (!derive) nov = 0;
    MThemeId base_id = own_theme ? own_id : MT_DEFAULT;
    if (!nov && eng_id == base_id) return false;
    if (nov) {
        Str *ks = ANEW(cur_a, Str, nov), *vs = ANEW(cur_a, Str, nov);
        if (!ks || !vs) return false;
        for (size_t i = 0, j = 0; i < cur.n; i++)
            if (str_starts(cur.kv[i].k, S("themeVariables.")))
                ks[j] = (Str){.p = cur.kv[i].k.p + 15, .n = cur.kv[i].k.n - 15}, vs[j++] = cur.kv[i].v;
        eng = mtheme_new(cur_a, eng_id, nov, ks, vs);
        eng_user = eng != nullptr;
    }
    /* where khroma cannot read an override mermaid fails the render; draw the theme instead, with the
     * overrides as given */
    if (!eng && !cur_a->oom) eng = mtheme_preset(eng_id);
    eng_base = mtheme_preset(base_id);
    if (!eng || !eng_base) cur_a->oom = true, eng = nullptr; /* render() reports it */
    return eng;
}

/* anything for dcfg_tv to report */
static bool themed(void) { return engine() || user_vars; }

void dcfg_theme_own(void) { own_theme = true, own_id = mt_of(cur_theme), cur_theme = 0; }
int dcfg_theme(void) { return cur_theme; }

/* themeVariables.<var> as the source gives it, or empty */
static Str user_tv(Str var) {
    char key[96];
    int kn = snprintf(key, sizeof key, "themeVariables.%.*s", (int)var.n, var.p);
    return kn > 0 && (size_t)kn < sizeof key ? dcfg_get_str((Str){.p = key, .n = (size_t)kn}) : (Str){};
}

/* the named theme's variable, derived from the source's themeVariables where mermaid derives, when
 * it differs from what the renderer draws anyway (or the source sets it); empty otherwise */
Str dcfg_tv(const char var[static 1]) {
    Str k = {.p = var, .n = strlen(var)}, user = user_tv(k);
    bool on = engine();
    if (user.n && !eng_user) return css_safe(user); /* not derived from: as given */
    if (!on) return (Str){};
    Str v = css_safe(mtheme_get(eng, k));
    return v.n && (user.n || !str_eq(v, mtheme_get(eng_base, k))) ? v : (Str){};
}

Str dcfg_theme_var(const char var[static 1]) {
    Str k = {.p = var, .n = strlen(var)}, user = user_tv(k);
    const MTheme *t = engine() ? eng : mtheme_preset(eng_id);
    if (user.n && !eng_user) return css_safe(user);
    if (!t) cur_a->oom = true;
    return t ? css_safe(mtheme_get(t, k)) : (Str){};
}

Str dcfg_preset_var(const char var[static 1]) {
    const MTheme *t = mtheme_preset(eng_id);
    if (!t) cur_a->oom = true;
    return t ? css_safe(mtheme_get(t, (Str){.p = var, .n = strlen(var)})) : (Str){};
}

bool dcfg_dark(void) { return str_eq(dcfg_theme_var("darkMode"), S("true")); }

/* NUL-terminated copy of v that outlives the call (eight live at once), or def when v is empty or long */
static const char *ring_str(Str v, const char def[static 1]) {
    static thread_local char ring[8][208];
    static thread_local unsigned slot;
    if (!v.n || v.n >= sizeof ring[0]) return def;
    char *o = ring[slot++ % countof(ring)];
    memcpy(o, v.p, v.n);
    o[v.n] = 0;
    return o;
}

const char *dcfg_pal(const char prefix[static 1], size_t i, const char def[static 1]) {
    if (!themed()) return def;
    char var[48];
    snprintf(var, sizeof var, "%s%zu", prefix, i);
    return ring_str(dcfg_tv(var), def);
}

const char *dcfg_list(const char var[static 1], size_t i, const char def[static 1]) {
    if (!themed()) return def;
    Str v = dcfg_tv(var);
    size_t cnt = v.n ? 1 : 0;
    for (size_t k = 0; k < v.n; k++) cnt += v.p[k] == ',';
    if (!cnt) return def;
    i %= cnt;
    size_t b = 0;
    for (size_t k = 0; k < i; k++) b = (size_t)((const char *)memchr(v.p + b, ',', v.n - b) - v.p) + 1;
    const char *c = memchr(v.p + b, ',', v.n - b);
    return ring_str(str_trim((Str){.p = v.p + b, .n = (c ? (size_t)(c - v.p) : v.n) - b}), def);
}

bool dcfg_neo(void) {
    if (!str_eq(appearance(S("look")), S("classic"))) return true;
    if (api_theme) return false; /* mmd_theme names only classic themes, never redux* */
    Str theme = appearance(S("theme"));
    return !theme.n || str_starts(theme, S("redux"));
}

/* themeVariable -> CSS template ('$' = value), for the classes the renderers emit; kind "*" applies to
 * every diagram. Per kind, the variables mermaid's styles for that diagram read, so a variable the
 * source sets without naming a theme changes what it changes in mermaid; later rows win, as mermaid's
 * `a || b` fallbacks do. tools/css_audit.py fails on a selector no element carries. */
typedef struct TV { const char *kind, *var, *tpl; } TV;
static const TV TVS[] = {
    {"*", "fontFamily", "#mermaid{font-family:$;}"},
    {"*", "fontSize", "#mermaid{font-size:$;}"},
    {"*", "textColor", "#mermaid{fill:$;}"},
    {"flowchart", "mainBkg", "#mermaid .node rect,#mermaid .node circle,#mermaid .node ellipse,#mermaid .node polygon,#mermaid .node path{fill:$;}"},
    {"flowchart", "nodeBorder", "#mermaid .node rect,#mermaid .node circle,#mermaid .node ellipse,#mermaid .node polygon,#mermaid .node path{stroke:$;}"},
    {"flowchart", "nodeTextColor", "#mermaid .node text{fill:$;}"},
    {"flowchart", "lineColor", "#mermaid .flowchart-link{stroke:$;}#mermaid .marker{fill:$;stroke:$;}"},
    {"flowchart", "edgeLabelBackground", "#mermaid .edgeLabel rect{fill:$;}"},
    {"flowchart", "clusterBkg", "#mermaid .cluster rect{fill:$;}"},
    {"flowchart", "clusterBorder", "#mermaid .cluster rect{stroke:$;}"},
    {"flowchart", "titleColor", "#mermaid .cluster-label,#mermaid .title{fill:$;}"},
    {"usecase", "mainBkg", "#mermaid .gl,#mermaid .uc,#mermaid .js,#mermaid .mke{fill:$;}"},
    {"usecase", "actorBkg", "#mermaid .gl{fill:$;}"},
    {"usecase", "nodeBorder", "#mermaid .gl,#mermaid .uc,#mermaid .bm,#mermaid .js{stroke:$;}"},
    {"usecase", "actorBorder", "#mermaid .gl{stroke:$;}"},
    {"usecase", "clusterBkg", "#mermaid .bd{fill:$;}"},
    {"usecase", "clusterBorder", "#mermaid .bd{stroke:$;}"},
    {"usecase", "lineColor", "#mermaid .rel,#mermaid .mkx,#mermaid .mke{stroke:$;}#mermaid .mk{fill:$;stroke:$;}"},
    {"usecase", "edgeLabelBackground", "#mermaid .elbl{fill:$;}"},
    {"usecase", "primaryTextColor", "#mermaid .ul,#mermaid .el,#mermaid .tl{fill:$;}"},
    {"usecase", "actorTextColor", "#mermaid .al{fill:$;}"},
    {"usecase", "titleColor", "#mermaid .bt{fill:$;}"},
    {"usecase", "noteBkgColor", "#mermaid .nt{fill:$;}"},
    {"usecase", "noteBorderColor", "#mermaid .nt{stroke:$;}"},
    {"usecase", "noteTextColor", "#mermaid .nl{fill:$;}"},
    {"usecase", "usecaseActorBkg", "#mermaid .gl{fill:$;}"},
    {"usecase", "usecaseActorBorder", "#mermaid .gl{stroke:$;}"},
    {"usecase", "usecaseBkg", "#mermaid .uc{fill:$;}"},
    {"usecase", "usecaseBorder", "#mermaid .uc,#mermaid .bm{stroke:$;}"},
    {"usecase", "usecaseBoundaryBkg", "#mermaid .bd{fill:$;}"},
    {"usecase", "usecaseBoundaryBorder", "#mermaid .bd{stroke:$;}"},
    {"usecase", "usecaseIncludeLine", "#mermaid .rel.inc{stroke:$;}"},
    {"usecase", "usecaseExtendLine", "#mermaid .rel.ext{stroke:$;}"},
    {"sequence", "actorBkg", "#mermaid .actor,#mermaid .actor-man circle{fill:$;}"},
    {"sequence", "actorBorder", "#mermaid .actor,#mermaid .actor-man circle,#mermaid .actor-line{stroke:$;}"},
    {"sequence", "actorTextColor", "#mermaid .actor+text,#mermaid .actor~text{fill:$;}"},
    {"sequence", "textColor", "#mermaid text{fill:$;}"},
    {"sequence", "actorLineColor", "#mermaid .lifeline{stroke:$;}"},
    {"sequence", "signalColor", "#mermaid .msg,#mermaid .destroy{stroke:$;}#mermaid .seqnum{fill:$;}#mermaid marker path,#mermaid marker polygon{fill:$;stroke:$;}"},
    {"sequence", "signalTextColor", "#mermaid .msg-text{fill:$;}"},
    {"sequence", "sequenceNumberColor", "#mermaid .seqnum-t{fill:$;}"},
    {"sequence", "noteBkgColor", "#mermaid .note{fill:$;}"},
    {"sequence", "noteBorderColor", "#mermaid .note{stroke:$;}"},
    {"sequence", "noteTextColor", "#mermaid .note+text{fill:$;}"},
    {"sequence", "activationBkgColor", "#mermaid .act{fill:$;}"},
    {"sequence", "activationBorderColor", "#mermaid .act{stroke:$;}"},
    {"sequence", "labelBoxBorderColor", "#mermaid .block,#mermaid .block-sep,#mermaid .block-tab{stroke:$;}"},
    {"sequence", "labelBoxBkgColor", "#mermaid .block-tab{fill:$;}"},
    {"statediagram", "noteBkgColor", "#mermaid .note{fill:$;}"},
    {"statediagram", "noteBorderColor", "#mermaid .note,#mermaid .note-l{stroke:$;}"},
    {"statediagram", "mainBkg", "#mermaid .choice,#mermaid .endo{fill:$;}"},
    {"statediagram", "stateBkg", "#mermaid .st{fill:$;}"},
    {"statediagram", "compositeTitleBackground", "#mermaid .comp-t{fill:$;}"},
    {"statediagram", "nodeBorder", "#mermaid .st,#mermaid .choice,#mermaid .comp,#mermaid .comp-t,#mermaid .div{stroke:$;}#mermaid .endi{fill:$;}"},
    {"statediagram", "stateBorder", "#mermaid .st,#mermaid .choice,#mermaid .comp,#mermaid .comp-t,#mermaid .div{stroke:$;}#mermaid .endi{fill:$;}"},
    {"statediagram", "lineColor", "#mermaid .bar{fill:$;}#mermaid .endo{stroke:$;}"},
    {"statediagram", "specialStateColor", "#mermaid .start{fill:$;}"},
    {"statediagram", "transitionColor", "#mermaid .tr{stroke:$;}#mermaid marker path{fill:$;stroke:$;}"},
    {"statediagram", "background", "#mermaid .comp{fill:$;}"},
    {"statediagram", "edgeLabelBackground", "#mermaid .lbl{fill:$;}"},
    {"statediagram", "textColor", "#mermaid text{fill:$;}"},
    {"statediagram", "noteTextColor", "#mermaid .note+text{fill:$;}"},
    {"classDiagram", "mainBkg", "#mermaid .cls{fill:$;}"},
    {"classDiagram", "nodeBorder", "#mermaid .cls,#mermaid .div{stroke:$;}"},
    {"classDiagram", "lineColor", "#mermaid .rel,#mermaid marker path{stroke:$;}"},
    {"classDiagram", "edgeLabelBackground", "#mermaid .lbl{fill:$;}"},
    {"classDiagram", "clusterBkg", "#mermaid .ns{fill:$;}"},
    {"classDiagram", "clusterBorder", "#mermaid .ns{stroke:$;}"},
    {"classDiagram", "noteBkgColor", "#mermaid .note{fill:$;}"},
    {"classDiagram", "noteBorderColor", "#mermaid .note{stroke:$;}"},
    {"classDiagram", "textColor", "#mermaid text{fill:$;}"},
    {"erDiagram", "mainBkg", "#mermaid .ent{fill:$;}"},
    {"erDiagram", "nodeBorder", "#mermaid .ent,#mermaid .row0,#mermaid .row1{stroke:$;}"},
    {"erDiagram", "rowOdd", "#mermaid .row0{fill:$;}"},
    {"erDiagram", "rowEven", "#mermaid .row1{fill:$;}"},
    {"erDiagram", "lineColor", "#mermaid .rel,#mermaid .mk,#mermaid .mkc{stroke:$;}"},
    {"erDiagram", "background", "#mermaid .mkc,#mermaid .lbl{fill:$;}"},
    {"erDiagram", "textColor", "#mermaid text{fill:$;}"},
    {"requirementDiagram", "requirementBackground", "#mermaid .rq{fill:$;}"},
    {"requirementDiagram", "requirementBorderColor", "#mermaid .rq,#mermaid .rqd{stroke:$;}"},
    {"requirementDiagram", "relationColor", "#mermaid .rr{stroke:$;}#mermaid .rh{fill:$;stroke:$;}"},
    {"requirementDiagram", "relationLabelBackground", "#mermaid .rl{fill:$;}"},
    {"requirementDiagram", "requirementTextColor", "#mermaid text{fill:$;}"},
    {"block", "mainBkg", "#mermaid .bn{fill:$;}"},
    {"block", "nodeBorder", "#mermaid .bn,#mermaid .bg{stroke:$;}"},
    {"block", "clusterBkg", "#mermaid .bg{fill:$;}"},
    {"block", "edgeLabelBackground", "#mermaid .bl{fill:$;}"},
    {"block", "lineColor", "#mermaid .be{stroke:$;}#mermaid marker path{fill:$;stroke:$;}"},
    {"block", "textColor", "#mermaid text{fill:$;}"},
    {"kanban", "background", "#mermaid .ki{fill:$;}"},
    {"kanban", "nodeBorder", "#mermaid .ki{stroke:$;}"},
    {"kanban", "textColor", "#mermaid .kt,#mermaid .kl{fill:$;}"},
    {"gantt", "taskBkgColor", "#mermaid .task{fill:$;}"},
    {"gantt", "taskBorderColor", "#mermaid .task{stroke:$;}"},
    {"gantt", "activeTaskBkgColor", "#mermaid .task.active,#mermaid .task.crit.active{fill:$;}"},
    {"gantt", "activeTaskBorderColor", "#mermaid .task.active{stroke:$;}"},
    {"gantt", "doneTaskBkgColor", "#mermaid .task.done,#mermaid .task.crit.done{fill:$;}"},
    {"gantt", "doneTaskBorderColor", "#mermaid .task.done{stroke:$;}"},
    {"gantt", "critBkgColor", "#mermaid .task.crit{fill:$;}"},
    {"gantt", "critBorderColor", "#mermaid .task.crit,#mermaid .task.crit.active,#mermaid .task.crit.done{stroke:$;}"},
    {"gantt", "vertLineColor", "#mermaid .vert{stroke:$;}#mermaid .vtx{fill:$;}"},
    {"gantt", "sectionBkgColor", "#mermaid .sec0,#mermaid .sec2{fill:$;}"},
    {"gantt", "altSectionBkgColor", "#mermaid .sec1,#mermaid .sec3{fill:$;}"},
    {"gantt", "gridColor", "#mermaid .grid{stroke:$;}"},
    {"gantt", "taskTextColor", "#mermaid .tin{fill:$;}"},
    {"gantt", "taskTextOutsideColor", "#mermaid .tout{fill:$;}"},
    {"gantt", "taskTextDarkColor", "#mermaid .done+.tin,#mermaid .tin.dk{fill:$;}"},
    {"gantt", "excludeBkgColor", "#mermaid .exr{fill:$;}"},
    {"gantt", "todayLineColor", "#mermaid .today{stroke:$;}"},
    {"gantt", "textColor", "#mermaid .tick,#mermaid .stitle{fill:$;}"},
    {"gantt", "titleColor", "#mermaid .ttl{fill:$;}"},
    {"pie", "pieStrokeColor", "#mermaid .slice{stroke:$;}"},
    {"pie", "pieOuterStrokeColor", "#mermaid .outer{stroke:$;}"},
    {"pie", "pieOpacity", "#mermaid .slice{opacity:$;}"},
    {"pie", "pieTitleTextColor", "#mermaid .ttl{fill:$;}"},
    {"pie", "pieSectionTextColor", "#mermaid .pct{fill:$;}"},
    {"pie", "pieLegendTextColor", "#mermaid text:not(.pct):not(.ttl){fill:$;}"},
    {"quadrantChart", "quadrant1Fill", "#mermaid .q1{fill:$;}"},
    {"quadrantChart", "quadrant2Fill", "#mermaid .q2{fill:$;}"},
    {"quadrantChart", "quadrant3Fill", "#mermaid .q3{fill:$;}"},
    {"quadrantChart", "quadrant4Fill", "#mermaid .q4{fill:$;}"},
    {"quadrantChart", "quadrantExternalBorderStrokeFill", "#mermaid .bd{stroke:$;}"},
    {"quadrantChart", "quadrantInternalBorderStrokeFill", "#mermaid .in{stroke:$;}"},
    {"quadrantChart", "quadrant1TextFill", "#mermaid .ql{fill:$;}"},
    {"quadrantChart", "quadrantXAxisTextFill", "#mermaid .al{fill:$;}"},
    {"quadrantChart", "quadrantPointTextFill", "#mermaid .pl{fill:$;}"},
    {"quadrantChart", "quadrantTitleFill", "#mermaid .ttl{fill:$;}"},
    {"xychart", "xyChart.titleColor", "#mermaid .ttl{fill:$;}"},
    {"xychart", "xyChart.xAxisLineColor", "#mermaid .ax.x{stroke:$;}"},
    {"xychart", "xyChart.xAxisTickColor", "#mermaid .tk.x{stroke:$;}"},
    {"xychart", "xyChart.xAxisLabelColor", "#mermaid .tl.x{fill:$;}"},
    {"xychart", "xyChart.xAxisTitleColor", "#mermaid .at.x{fill:$;}"},
    {"xychart", "xyChart.yAxisLineColor", "#mermaid .ax.y{stroke:$;}"},
    {"xychart", "xyChart.yAxisTickColor", "#mermaid .tk.y{stroke:$;}"},
    {"xychart", "xyChart.yAxisLabelColor", "#mermaid .tl.y{fill:$;}"},
    {"xychart", "xyChart.yAxisTitleColor", "#mermaid .at.y{fill:$;}"},
    {"xychart", "xyChart.legendTextColor", "#mermaid .lt{fill:$;}#mermaid .lg{stroke:$;}"},
    {"gitGraph", "commitLabelColor", "#mermaid .cl{fill:$;}"},
    {"gitGraph", "commitLabelBackground", "#mermaid .clb{fill:$;}"},
    {"gitGraph", "tagLabelColor", "#mermaid .tg{fill:$;}"},
    {"gitGraph", "tagLabelBackground", "#mermaid .tgb{fill:$;}"},
    {"gitGraph", "tagLabelBorder", "#mermaid .tgb{stroke:$;}"},
    {"gitGraph", "textColor", "#mermaid .bn,#mermaid .ttl{fill:$;}"},
    {"gitGraph", "lineColor", "#mermaid .bl{stroke:$;}"},
    {"packet", "packet.blockFillColor", "#mermaid .pb{fill:$;}"},
    {"packet", "packet.blockStrokeColor", "#mermaid .pb{stroke:$;}"},
    {"packet", "packet.labelColor", "#mermaid .pl,#mermaid .pn{fill:$;}"},
    {"packet", "packet.titleColor", "#mermaid .ttl{fill:$;}"},
    {"radar", "radar.axisColor", "#mermaid .ax{stroke:$;}"},
    {"radar", "radar.graticuleColor", "#mermaid .gr{fill:$;stroke:$;}"},
    {"radar", "textColor", "#mermaid .al,#mermaid .lg,#mermaid .ttl{fill:$;}"},
    {"treemap", "textColor", "#mermaid .sh,#mermaid .sv,#mermaid .ttl{fill:$;}"},
    {"sankey", "textColor", "#mermaid .st{fill:$;}"},
    {"journey", "lineColor", "#mermaid .axis,#mermaid .adot,#mermaid .tl{stroke:$;}"},
    {"journey", "textColor", "#mermaid .ttl{fill:$;}"},
    {"architecture", "textColor", "#mermaid text{fill:$;}"},
    {"c4", "lineColor", "#mermaid .cr,#mermaid .cb{stroke:$;}#mermaid .crh{fill:$;}"},
    {"c4", "textColor", "#mermaid .cbt,#mermaid .cbs,#mermaid .crl,#mermaid .crt{fill:$;}"},
};

/* tpl with each '$' replaced by v */
static void rule_put(Buf b[static 1], const char tpl[static 1], Str v) {
    for (const char *t = tpl; *t; t++)
        if (*t == '$') buf_str(b, v); else buf_ch(b, *t);
}

void dcfg_theme_css(Buf b[static 1], const char kind[static 1]) {
    if (!themed()) return;
    for (size_t i = 0; i < countof(TVS); i++) {
        if (strcmp(TVS[i].kind, "*") && strcmp(TVS[i].kind, kind)) continue;
        Str v = dcfg_tv(TVS[i].var);
        if (v.n) rule_put(b, TVS[i].tpl, v);
    }
}

void dcfg_rules(Buf b[static 1], size_t n, const DRule rules[static n]) {
    for (size_t i = 0; i < n; i++) {
        Str v = dcfg_theme_var(rules[i].var);
        rule_put(b, rules[i].tpl, v.n ? v : (Str){.p = rules[i].def, .n = strlen(rules[i].def)});
    }
}

void dcfg_svg_aria(Buf b[static 1]) {
    if (cur.acc_title.n) buf_lit(b, "\" aria-labelledby=\"chart-title-mermaid");
    if (cur.acc_descr.n) buf_lit(b, "\" aria-describedby=\"chart-desc-mermaid");
    buf_lit(b, "\">");
    if (cur.acc_title.n) { buf_lit(b, "<title id=\"chart-title-mermaid\">"); buf_xml(b, cur.acc_title); buf_lit(b, "</title>"); }
    if (cur.acc_descr.n) { buf_lit(b, "<desc id=\"chart-desc-mermaid\">"); buf_xml(b, cur.acc_descr); buf_lit(b, "</desc>"); }
}
