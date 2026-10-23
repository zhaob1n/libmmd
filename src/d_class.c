/* classDiagram: classes with members (attributes/methods, visibility, static $, abstract *),
 * generics (~T~), annotations (<<interface>>), all relation types incl. lollipop, two-way
 * relations, cardinality labels, relation labels, namespaces (clusters), notes, direction.
 * Layout: flow_layout on a pre-sized Graph. */
#include <math.h>
#include <stdio.h>

#include "dgraph.h"
#include "dcfg.h"
#include "diagrams.h"
#include "text.h"

typedef enum : uint8_t { RM_NONE, RM_EXT, RM_COMP, RM_AGG, RM_ARROW, RM_LOLLI } RMark;
enum { MF_METHOD = 1, MF_STATIC = 2, MF_ABSTRACT = 4 };
typedef struct Member { int32_t cls; Str text; uint8_t flags; } Member;
typedef struct Rel { RMark ma, mb; bool dotted; Str ca, cb; } Rel; /* per edge */
typedef struct CInfo { Str annot; uint32_t m0, mn; bool note, iface; } CInfo;

typedef struct CP {
    Arena *A;
    Graph g;
    VEC(CInfo) ci;
    VEC(Member) mem;
    VEC(Rel) rel;
    int32_t ns_stack[32];
    unsigned ns_depth;
    unsigned nnotes, niface;
    size_t *err_off;
    char *msg;
} CP;

static cmm_status cfail(CP *p, size_t off, const char *m) { return diag_fail(p->err_off, p->msg, off, m); }

/* ~T~ -> <T> (zero-copy unless a tilde is present) */
static Str generics(Arena *A, Str s, bool *ok) {
    *ok = true;
    if (!s.n || !memchr(s.p, '~', s.n)) return s;
    char *o = arena_alloc(A, s.n + 1, 1);
    if (!o) { *ok = false; return (Str){}; }
    for (size_t i = 0; i < s.n; i++) {
        char c = s.p[i];
        if (c != '~') { o[i] = c; continue; }
        /* open when glued between identifier chars (List~int~ / List~List~int~~), else close */
        bool prev = i > 0 && s.p[i - 1] != ' ' && s.p[i - 1] != '~' && s.p[i - 1] != ',' && s.p[i - 1] != '(';
        bool next = i + 1 < s.n && s.p[i + 1] != ' ' && s.p[i + 1] != '~' && s.p[i + 1] != ',' &&
                    s.p[i + 1] != ')' && s.p[i + 1] != '[' && s.p[i + 1] != ':';
        o[i] = prev && next ? '<' : !prev && next ? '<' : '>';
    }
    return (Str){.p = o, .n = s.n};
}

[[nodiscard]] static int32_t cls(CP *p, Str id) {
    bool fresh;
    int32_t v = dg_node(&p->g, id, &fresh);
    if (v == NONE) return NONE;
    if (fresh) {
        if (!VEC_PUSH(p->A, &p->ci, (CInfo){})) return NONE;
        p->g.nparent[v] = p->ns_depth ? p->ns_stack[p->ns_depth - 1] : NONE;
        bool ok;
        p->g.nlabel[v] = generics(p->A, id, &ok);
        if (!ok) return NONE;
    }
    return v;
}

[[nodiscard]] static int32_t iface(CP *p, Str label) {
    char nm[32];
    int k = snprintf(nm, sizeof nm, "\x1finterface%u", p->niface++);
    char *id = arena_alloc(p->A, (size_t)k, 1);
    if (!id) return NONE;
    memcpy(id, nm, (size_t)k);
    int32_t v = cls(p, (Str){.p = id, .n = (size_t)k});
    if (v == NONE) return NONE;
    p->ci.v[v].iface = true;
    p->g.nlabel[v] = label;
    return v;
}

/* split "Name~T~" / "Name[\"Label\"]" / "Name:::css" into id and optional label */
static Str class_ref(CP *p, Str s, Str *label) {
    s = str_trim(s);
    *label = (Str){};
    for (size_t k = 0; k + 2 < s.n; k++)
        if (s.p[k] == ':' && s.p[k + 1] == ':' && s.p[k + 2] == ':') { s.n = k; break; }
    const char *b = memchr(s.p, '[', s.n);
    if (b) {
        Str in = {.p = b + 1, .n = s.n - (size_t)(b + 1 - s.p)};
        if (in.n && in.p[in.n - 1] == ']') in.n--;
        in = str_trim(in);
        if (in.n >= 2 && in.p[0] == '"' && in.p[in.n - 1] == '"') in.p++, in.n -= 2;
        *label = in;
        s.n = (size_t)(b - s.p);
    }
    s = str_trim(s);
    if (s.n >= 2 && s.p[0] == '`' && s.p[s.n - 1] == '`') s.p++, s.n -= 2;
    const char *t = s.n ? memchr(s.p, '~', s.n) : nullptr;
    if (t && !label->n) { /* Name~T~: id is Name, label shows Name<T> */
        bool ok;
        *label = generics(p->A, s, &ok);
        if (!ok) *label = s;
        s.n = (size_t)(t - s.p);
    }
    return s;
}

/* a label seen at any reference (Name~T~ / Name["x"]) replaces the default id label */
static void ref_label(Graph *g, int32_t v, Str label) {
    if (label.n && g->nlabel[v].p == g->nid[v].p && g->nlabel[v].n == g->nid[v].n) g->nlabel[v] = label;
}

static cmm_status add_member(CP *p, int32_t v, Str t) {
    t = str_trim(t);
    if (!t.n) return CMM_OK;
    if (t.n >= 4 && str_starts(t, S("<<")) && t.p[t.n - 1] == '>' && t.p[t.n - 2] == '>') {
        p->ci.v[v].annot = (Str){.p = t.p + 2, .n = t.n - 4};
        return CMM_OK;
    }
    uint8_t f = 0;
    if (memchr(t.p, '(', t.n)) f |= MF_METHOD;
    /* classifier: trailing $ or * (also right after ')') */
    for (;;) {
        if (t.n && t.p[t.n - 1] == '$') { f |= MF_STATIC; t.n--; continue; }
        if (t.n && t.p[t.n - 1] == '*') { f |= MF_ABSTRACT; t.n--; continue; }
        break;
    }
    if (f & MF_METHOD) { /* "foo()$ int" style: classifier right after ')' */
        const char *rp = memchr(t.p, ')', t.n);
        if (rp && rp + 1 < t.p + t.n && (rp[1] == '$' || rp[1] == '*')) {
            f |= rp[1] == '$' ? MF_STATIC : MF_ABSTRACT;
            char *o = arena_alloc(p->A, t.n, 1);
            if (!o) return CMM_ERR_NOMEM;
            size_t k = (size_t)(rp + 1 - t.p);
            memcpy(o, t.p, k);
            memcpy(o + k, rp + 2, t.n - k - 1);
            t = (Str){.p = o, .n = t.n - 1};
        }
        /* "name(args) Ret" -> "name(args) : Ret" */
        const char *rp2 = nullptr;
        for (size_t k = t.n; k-- > 0;) if (t.p[k] == ')') { rp2 = t.p + k; break; }
        if (rp2) {
            Str ret = str_trim((Str){.p = rp2 + 1, .n = t.n - (size_t)(rp2 + 1 - t.p)});
            if (ret.n && ret.p[0] != ':') {
                size_t k = (size_t)(rp2 + 1 - t.p);
                char *o = arena_alloc(p->A, k + 3 + ret.n, 1);
                if (!o) return CMM_ERR_NOMEM;
                memcpy(o, t.p, k);
                memcpy(o + k, " : ", 3);
                memcpy(o + k + 3, ret.p, ret.n);
                t = (Str){.p = o, .n = k + 3 + ret.n};
            }
        }
    }
    bool ok;
    t = generics(p->A, str_trim(t), &ok);
    if (!ok) return CMM_ERR_NOMEM;
    return VEC_PUSH(p->A, &p->mem, ((Member){.cls = v, .text = t, .flags = f})) ? CMM_OK : CMM_ERR_NOMEM;
}

typedef struct RelTok { char t[3]; uint8_t len; RMark m; } RelTok;
static constexpr RelTok LEFT[] = {{"<|", 2, RM_EXT}, {"()", 2, RM_LOLLI}, {"*", 1, RM_COMP}, {"o", 1, RM_AGG}, {"<", 1, RM_ARROW}};
static constexpr RelTok RIGHT[] = {{"|>", 2, RM_EXT}, {"()", 2, RM_LOLLI}, {"*", 1, RM_COMP}, {"o", 1, RM_AGG}, {">", 1, RM_ARROW}};

/* find relation operator; returns true and fills [s0,s1) span + marks */
static bool find_rel(Str l, size_t *s0, size_t *s1, Rel *r) {
    bool inq = false;
    for (size_t k = 0; k + 1 < l.n; k++) {
        if (l.p[k] == '"') inq = !inq;
        if (inq) continue;
        if (!((l.p[k] == '-' && l.p[k + 1] == '-') || (l.p[k] == '.' && l.p[k + 1] == '.'))) continue;
        size_t a = k, b = k + 2;
        r->dotted = l.p[k] == '.';
        r->ma = r->mb = RM_NONE;
        for (size_t t = 0; t < countof(LEFT); t++)
            if (a >= LEFT[t].len && memcmp(l.p + a - LEFT[t].len, LEFT[t].t, LEFT[t].len) == 0 &&
                (LEFT[t].len == 2 || a - 1 == 0 || is_sp(l.p[a - 2]) || l.p[a - 2] == '"' || LEFT[t].m == RM_ARROW)) {
                /* single-letter 'o' must be separated from the class name */
                if (LEFT[t].m == RM_AGG && a >= 2 && !is_sp(l.p[a - 2]) && l.p[a - 2] != '"') continue;
                if (LEFT[t].m == RM_COMP && a >= 2 && !is_sp(l.p[a - 2]) && l.p[a - 2] != '"') continue;
                r->ma = LEFT[t].m;
                a -= LEFT[t].len;
                break;
            }
        for (size_t t = 0; t < countof(RIGHT); t++)
            if (b + RIGHT[t].len <= l.n && memcmp(l.p + b, RIGHT[t].t, RIGHT[t].len) == 0) {
                if ((RIGHT[t].m == RM_AGG || RIGHT[t].m == RM_COMP) && b + 1 < l.n && !is_sp(l.p[b + 1]) && l.p[b + 1] != '"') continue;
                r->mb = RIGHT[t].m;
                b += RIGHT[t].len;
                break;
            }
        *s0 = a, *s1 = b;
        return true;
    }
    return false;
}

static Str take_quoted_tail(Str *s) { /* trailing "card" */
    *s = str_trim(*s);
    if (s->n >= 2 && s->p[s->n - 1] == '"') {
        for (size_t k = s->n - 1; k-- > 0;)
            if (s->p[k] == '"') {
                Str q = {.p = s->p + k + 1, .n = s->n - k - 2};
                s->n = k;
                *s = str_trim(*s);
                return q;
            }
    }
    return (Str){};
}
static Str take_quoted_head(Str *s) {
    *s = str_trim(*s);
    if (s->n >= 2 && s->p[0] == '"') {
        const char *q = memchr(s->p + 1, '"', s->n - 1);
        if (q) {
            Str r = {.p = s->p + 1, .n = (size_t)(q - s->p - 1)};
            *s = str_trim((Str){.p = q + 1, .n = s->n - (size_t)(q + 1 - s->p)});
            return r;
        }
    }
    return (Str){};
}

static cmm_status parse(CP *p, LineIt *it) {
    Graph *g = &p->g;
    Str line;
    size_t off;
    int32_t body = NONE; /* class whose { } body is open */
    while (dg_next_line(it, &line, &off)) {
        if (body != NONE) {
            if (str_eq(line, S("}"))) { body = NONE; continue; }
            Str l = line;
            bool close = l.n && l.p[l.n - 1] == '}';
            if (close) l = str_trim((Str){.p = l.p, .n = l.n - 1});
            cmm_status st = add_member(p, body, l);
            if (st) return st;
            if (close) body = NONE;
            continue;
        }
        Str rest = line, w = dg_word(&rest);
        if (str_eq(line, S("}"))) {
            if (!p->ns_depth) return cfail(p, off, "unbalanced '}'");
            p->ns_depth--;
            continue;
        }
        if (str_eq(w, S("direction"))) {
            Str d = dg_word(&rest);
            g->dir = str_eq(d, S("LR")) ? DIR_LR : str_eq(d, S("RL")) ? DIR_RL : str_eq(d, S("BT")) ? DIR_BT : DIR_TB;
            continue;
        }
        if (str_starts(w, S("accTitle")) || str_starts(w, S("accDescr")) || str_eq(w, S("cssClass")) ||
            str_eq(w, S("classDef")) || str_eq(w, S("style")) || str_eq(w, S("callback")) || str_eq(w, S("link")) ||
            str_eq(w, S("click")))
            continue;
        if (str_eq(w, S("namespace"))) {
            Str r = rest;
            if (r.n && r.p[r.n - 1] == '{') r = str_trim((Str){.p = r.p, .n = r.n - 1});
            if (!r.n) return cfail(p, off, "namespace needs a name");
            if (p->ns_depth >= countof(p->ns_stack)) return cfail(p, off, "namespaces nested too deeply");
            Str nid = r, nlab = r;
            for (size_t k = 0; k < r.n; k++)
                if (r.p[k] == '[' && r.p[r.n - 1] == ']') {
                    nid = str_trim((Str){.p = r.p, .n = k});
                    nlab = str_trim((Str){.p = r.p + k + 1, .n = r.n - k - 2});
                    if (nlab.n >= 2 && nlab.p[0] == '"' && nlab.p[nlab.n - 1] == '"') nlab = (Str){.p = nlab.p + 1, .n = nlab.n - 2};
                    break;
                }
            int32_t s = dg_cluster(g, nid, nlab, p->ns_depth ? p->ns_stack[p->ns_depth - 1] : NONE);
            if (s == NONE) return CMM_ERR_NOMEM;
            p->ns_stack[p->ns_depth++] = s;
            continue;
        }
        if (str_eq(w, S("note"))) {
            Str r = rest;
            int32_t target = NONE;
            if (str_starts(r, S("for "))) {
                r = str_trim((Str){.p = r.p + 4, .n = r.n - 4});
                Str id = dg_word(&r);
                if ((target = cls(p, id)) == NONE) return CMM_ERR_NOMEM;
            }
            Str t = take_quoted_head(&r);
            if (!t.p) return cfail(p, off, "note text must be quoted");
            char nid[24];
            int len = snprintf(nid, sizeof nid, "\x01note%u", p->nnotes++);
            Str id = arena_strdup(p->A, (Str){.p = nid, .n = (size_t)len});
            if (!id.n) return CMM_ERR_NOMEM;
            int32_t v = cls(p, id);
            if (v == NONE) return CMM_ERR_NOMEM;
            bool ok;
            g->nlabel[v] = label_normalize(p->A, t, &ok);
            if (!ok) return CMM_ERR_NOMEM;
            for (size_t k = 0; k + 1 < g->nlabel[v].n; k++) /* "\n" escapes in note text */
                if (g->nlabel[v].p[k] == '\\' && g->nlabel[v].p[k + 1] == 'n') {
                    char *o = arena_alloc(p->A, g->nlabel[v].n, 1);
                    if (!o) return CMM_ERR_NOMEM;
                    size_t q = 0;
                    for (size_t i = 0; i < g->nlabel[v].n; i++) {
                        if (g->nlabel[v].p[i] == '\\' && i + 1 < g->nlabel[v].n && g->nlabel[v].p[i + 1] == 'n') { o[q++] = '\n'; i++; }
                        else o[q++] = g->nlabel[v].p[i];
                    }
                    g->nlabel[v] = (Str){.p = o, .n = q};
                    break;
                }
            p->ci.v[v].note = true;
            if (target != NONE) {
                int32_t e = dg_edge(g, v, target, (Str){});
                if (e == NONE || !VEC_PUSH(p->A, &p->rel, ((Rel){.dotted = true}))) return CMM_ERR_NOMEM;
                g->eend[e] = AH_NONE;
            }
            continue;
        }
        if (str_eq(w, S("class"))) {
            Str r = rest;
            bool open = r.n && r.p[r.n - 1] == '{';
            bool inline_close = false;
            Str inner = {};
            const char *br = memchr(r.p, '{', r.n);
            if (br) { /* class A { ... } possibly on one line */
                inner = (Str){.p = br + 1, .n = r.n - (size_t)(br + 1 - r.p)};
                r.n = (size_t)(br - r.p);
                open = true;
                if (inner.n && inner.p[inner.n - 1] == '}') { inline_close = true; inner.n--; }
            }
            Str label, id = class_ref(p, r, &label);
            Str ann = {};
            if (id.n && id.p[id.n - 1] == '>') { /* class A <<interface>> */
                for (size_t k = 0; k + 1 < id.n; k++)
                    if (id.p[k] == '<' && id.p[k + 1] == '<') { ann = str_trim((Str){.p = id.p + k, .n = id.n - k}); id = str_trim((Str){.p = id.p, .n = k}); break; }
            }
            if (!id.n) return cfail(p, off, "expected class name");
            int32_t v = cls(p, id);
            if (v == NONE) return CMM_ERR_NOMEM;
            if (label.n) {
                bool ok;
                g->nlabel[v] = label_normalize(p->A, label, &ok);
                if (!ok) return CMM_ERR_NOMEM;
            }
            if (ann.n) { cmm_status st = add_member(p, v, ann); if (st) return st; }
            if (open) {
                inner = str_trim(inner);
                if (inner.n) { cmm_status st = add_member(p, v, inner); if (st) return st; }
                if (!inline_close) body = v;
            }
            continue;
        }
        if (str_starts(line, S("<<"))) { /* <<interface>> Name */
            const char *e = nullptr;
            for (size_t k = 2; k + 1 < line.n; k++) if (line.p[k] == '>' && line.p[k + 1] == '>') { e = line.p + k + 2; break; }
            if (!e) return cfail(p, off, "unterminated annotation");
            Str ann = {.p = line.p, .n = (size_t)(e - line.p)};
            Str label, id = class_ref(p, (Str){.p = e, .n = line.n - ann.n}, &label);
            int32_t v = id.n ? cls(p, id) : NONE;
            if (v == NONE) return id.n ? CMM_ERR_NOMEM : cfail(p, off, "annotation needs a class");
            cmm_status st = add_member(p, v, ann);
            if (st) return st;
            continue;
        }
        size_t s0, s1;
        Rel r;
        if (find_rel(line, &s0, &s1, &r)) {
            Str lhs = {.p = line.p, .n = s0}, rhs = {.p = line.p + s1, .n = line.n - s1};
            r.ca = take_quoted_tail(&lhs);
            r.cb = take_quoted_head(&rhs);
            const char *c = memchr(rhs.p, ':', rhs.n);
            Str lab = {};
            if (c) { lab = (Str){.p = c + 1, .n = rhs.n - (size_t)(c + 1 - rhs.p)}; rhs.n = (size_t)(c - rhs.p); }
            Str la, lb, ia = class_ref(p, lhs, &la), ib = class_ref(p, rhs, &lb);
            if (!ia.n || !ib.n) return cfail(p, off, "relation needs two classes");
            /* mermaid addRelation: a lollipop end (other end plain) is a bare interface label, one per relation */
            bool lia = r.ma == RM_LOLLI && r.mb == RM_NONE, lib = !lia && r.mb == RM_LOLLI && r.ma == RM_NONE;
            int32_t va = lia ? iface(p, ia) : cls(p, ia), vb = lib ? iface(p, ib) : cls(p, ib);
            if (va == NONE || vb == NONE) return CMM_ERR_NOMEM;
            ref_label(g, va, la), ref_label(g, vb, lb);
            bool ok;
            lab = label_normalize(p->A, lab, &ok);
            if (ok) lab = generics(p->A, lab, &ok);
            if (!ok) return CMM_ERR_NOMEM;
            int32_t e = dg_edge(g, va, vb, lab);
            if (e == NONE || !VEC_PUSH(p->A, &p->rel, r)) return CMM_ERR_NOMEM;
            continue;
        }
        /* "Name : member" */
        const char *c = memchr(line.p, ':', line.n);
        if (c && !(c + 2 < line.p + line.n && c[1] == ':' && c[2] == ':')) {
            Str label, id = class_ref(p, (Str){.p = line.p, .n = (size_t)(c - line.p)}, &label);
            if (!id.n) return cfail(p, off, "expected class name before ':'");
            int32_t v = cls(p, id);
            if (v == NONE) return CMM_ERR_NOMEM;
            ref_label(g, v, label);
            cmm_status st = add_member(p, v, (Str){.p = c + 1, .n = line.n - (size_t)(c + 1 - line.p)});
            if (st) return st;
            continue;
        }
        Str label, id = class_ref(p, line, &label);
        for (size_t k = 0; k < id.n; k++)
            if (is_sp(id.p[k]) || id.p[k] == '{' || id.p[k] == '(' || id.p[k] == '"')
                return cfail(p, off + k, "unrecognised statement in class diagram");
        if (!id.n) return cfail(p, off, "unrecognised statement");
        int32_t v = cls(p, id);
        if (v == NONE) return CMM_ERR_NOMEM;
        ref_label(g, v, label);
    }
    if (body != NONE) return cfail(p, it->n, "unterminated class body (missing '}')");
    if (p->ns_depth) return cfail(p, it->n, "unterminated namespace (missing '}')");
    return CMM_OK;
}

static constexpr char CSS[] =
    "#mermaid .cls{fill:#ECECFF;stroke:#9370DB;stroke-width:1px;}"
    "#mermaid .div{stroke:#9370DB;stroke-width:1px;}"
    "#mermaid .rel{fill:none;stroke:#333;stroke-width:1px;}#mermaid .rel.dot{stroke-dasharray:3,3;}"
    "#mermaid .lbl{fill:#e8e8e8;opacity:0.85;}#mermaid .card{font-size:13px;}"
    "#mermaid .ns{fill:#ffffde;stroke:#aaaa33;stroke-width:1px;}"
    "#mermaid .note{fill:#fff5ad;stroke:#aaaa33;stroke-width:1px;}"
    "#mermaid .static{text-decoration:underline;}#mermaid .abstract{font-style:italic;}"
    "#mermaid .title{font-weight:bold;}#mermaid .ann{font-size:14px;}";

static constexpr char DEFS[] =
    "<defs>"
    "<marker id=\"cl-ext-s\" viewBox=\"0 0 20 14\" refX=\"1\" refY=\"7\" markerWidth=\"20\" markerHeight=\"14\" markerUnits=\"userSpaceOnUse\" orient=\"auto\"><path d=\"M1,7 L19,1 L19,13 z\" fill=\"#fff\" stroke=\"#333\"/></marker>"
    "<marker id=\"cl-ext-e\" viewBox=\"0 0 20 14\" refX=\"19\" refY=\"7\" markerWidth=\"20\" markerHeight=\"14\" markerUnits=\"userSpaceOnUse\" orient=\"auto\"><path d=\"M19,7 L1,1 L1,13 z\" fill=\"#fff\" stroke=\"#333\"/></marker>"
    "<marker id=\"cl-comp-s\" viewBox=\"0 0 24 12\" refX=\"1\" refY=\"6\" markerWidth=\"24\" markerHeight=\"12\" markerUnits=\"userSpaceOnUse\" orient=\"auto\"><path d=\"M1,6 L12,1 L23,6 L12,11 z\" fill=\"#333\" stroke=\"#333\"/></marker>"
    "<marker id=\"cl-comp-e\" viewBox=\"0 0 24 12\" refX=\"23\" refY=\"6\" markerWidth=\"24\" markerHeight=\"12\" markerUnits=\"userSpaceOnUse\" orient=\"auto\"><path d=\"M1,6 L12,1 L23,6 L12,11 z\" fill=\"#333\" stroke=\"#333\"/></marker>"
    "<marker id=\"cl-agg-s\" viewBox=\"0 0 24 12\" refX=\"1\" refY=\"6\" markerWidth=\"24\" markerHeight=\"12\" markerUnits=\"userSpaceOnUse\" orient=\"auto\"><path d=\"M1,6 L12,1 L23,6 L12,11 z\" fill=\"#fff\" stroke=\"#333\"/></marker>"
    "<marker id=\"cl-agg-e\" viewBox=\"0 0 24 12\" refX=\"23\" refY=\"6\" markerWidth=\"24\" markerHeight=\"12\" markerUnits=\"userSpaceOnUse\" orient=\"auto\"><path d=\"M1,6 L12,1 L23,6 L12,11 z\" fill=\"#fff\" stroke=\"#333\"/></marker>"
    "<marker id=\"cl-arrow-s\" viewBox=\"0 0 12 12\" refX=\"1\" refY=\"6\" markerWidth=\"12\" markerHeight=\"12\" markerUnits=\"userSpaceOnUse\" orient=\"auto\"><path d=\"M11,1 L1,6 L11,11\" fill=\"none\" stroke=\"#333\" stroke-width=\"1.3\"/></marker>"
    "<marker id=\"cl-arrow-e\" viewBox=\"0 0 12 12\" refX=\"11\" refY=\"6\" markerWidth=\"12\" markerHeight=\"12\" markerUnits=\"userSpaceOnUse\" orient=\"auto\"><path d=\"M1,1 L11,6 L1,11\" fill=\"none\" stroke=\"#333\" stroke-width=\"1.3\"/></marker>"
    "<marker id=\"cl-lolli-s\" viewBox=\"0 0 14 14\" refX=\"1\" refY=\"7\" markerWidth=\"14\" markerHeight=\"14\" markerUnits=\"userSpaceOnUse\" orient=\"auto\"><circle cx=\"7\" cy=\"7\" r=\"6\" fill=\"#fff\" stroke=\"#333\"/></marker>"
    "<marker id=\"cl-lolli-e\" viewBox=\"0 0 14 14\" refX=\"13\" refY=\"7\" markerWidth=\"14\" markerHeight=\"14\" markerUnits=\"userSpaceOnUse\" orient=\"auto\"><circle cx=\"7\" cy=\"7\" r=\"6\" fill=\"#fff\" stroke=\"#333\"/></marker>"
    "</defs>";

static const char *mark_id(RMark m, bool end) {
    switch (m) {
    case RM_EXT: return end ? "cl-ext-e" : "cl-ext-s";
    case RM_COMP: return end ? "cl-comp-e" : "cl-comp-s";
    case RM_AGG: return end ? "cl-agg-e" : "cl-agg-s";
    case RM_ARROW: return end ? "cl-arrow-e" : "cl-arrow-s";
    case RM_LOLLI: return end ? "cl-lolli-e" : "cl-lolli-s";
    case RM_NONE: break;
    }
    return nullptr;
}

constexpr double CPAD = 12;   /* config.class.padding */
constexpr double SECP = 6;   /* member section padding (CPAD/2) */
constexpr double SECE = 8;   /* height of an empty member section */
constexpr double CLS_BOLD = 1.0617; /* bolder label width factor */

static double sec_h(unsigned k, double lh) { return k ? k * lh + 2 * SECP : SECE; }

static void card_label(Buf *b, const double *P0, const double *P1, Str t, double fs) {
    double dx = P1[0] - P0[0], dy = P1[1] - P0[1], L = sqrt(dx * dx + dy * dy);
    if (L < 1e-9) return;
    dx /= L, dy /= L;
    double x = P0[0] + dx * 22 - dy * 12, y = P0[1] + dy * 22 + dx * 12;
    dg_text(b, t, fs, x, y, "middle", "card");
}

DIAG_FN(class_render) {
    CP p = {.A = a, .err_off = err_off, .msg = msg};
    dg_init(&p.g, a, DIR_TB, fs);
    LineIt it = {.s = src, .n = n, .i = off};
    Str hdr;
    size_t ho;
    if (!dg_next_line(&it, &hdr, &ho)) return diag_fail(err_off, msg, off, "empty class diagram");
    cmm_status st = parse(&p, &it);
    if (st) return st;
    Graph *g = &p.g;
    if (!g->nn) return diag_fail(err_off, msg, off, "class diagram has no classes");
    size_t N = g->nn, M = p.mem.n;
    /* bucket members per class (stable counting sort) */
    Member *ms = ANEW(a, Member, M + 1);
    if (!ms) return CMM_ERR_NOMEM;
    for (size_t i = 0; i < M; i++) p.ci.v[p.mem.v[i].cls].mn++;
    uint32_t acc = 0;
    for (size_t v = 0; v < N; v++) { p.ci.v[v].m0 = acc; acc += p.ci.v[v].mn; p.ci.v[v].mn = 0; }
    for (size_t i = 0; i < M; i++) { CInfo *c = &p.ci.v[p.mem.v[i].cls]; ms[c->m0 + c->mn++] = p.mem.v[i]; }
    double lh = fs * 1.5;
    bool hide_empty = dcfg_bool("class.hideEmptyMembersBox", false);
    for (size_t v = 0; v < N; v++) {
        const CInfo *c = &p.ci.v[v];
        double tw;
        unsigned ln;
        label_measure(g->nlabel[v], fs, &tw, &ln);
        if (c->note) { dg_size(g, (int32_t)v, tw + 20, (ln ? ln : 1) * lh + 14); continue; }
        if (c->iface) { dg_size(g, (int32_t)v, tw, (ln ? ln : 1) * lh); continue; }
        /* mermaid classBox/textHelper: label+annotation groups are centred,
         * member/method groups start at x=0, so bbox = max(half, mw) + half */
        tw *= CLS_BOLD;
        double half = tw / 2, A = 0, L = (ln ? ln : 1) * lh;
        if (c->annot.n) { double aw = text_width(c->annot, fs) + text_width(S("\u00ab\u00bb"), fs); if (aw / 2 > half) half = aw / 2; A = lh; }
        unsigned na = 0, nm = 0;
        double mw = 0;
        for (uint32_t i = 0; i < c->mn; i++) {
            const Member *m = &ms[c->m0 + i];
            double t = text_width(m->text, fs);
            if (t > mw) mw = t;
            if (m->flags & MF_METHOD) nm++; else na++;
        }
        double w = (na || nm) ? (mw > half ? mw : half) + half : 2 * half;
        double h = (!c->mn && hide_empty) ? CPAD + A + L + CPAD : CPAD + A + L + SECP + sec_h(na, lh) + sec_h(nm, lh);
        dg_size(g, (int32_t)v, w + 2 * CPAD, h);
    }
    g->iso_toggle = true;
    /* LR/RL: the label sits in a rank of its own width, leaving only ranksep/2 per side for
     * the relation markers (diamond, triangle) and cardinalities; widen so they don't touch */
    if (g->dir == DIR_LR || g->dir == DIR_RL)
        for (size_t e = 0; e < g->ne; e++)
            if (g->elabel[e].n) { g->rank_sep += 30; break; }
    if ((st = flow_layout(g))) return st;
    BBox bb = dg_bbox(g);
    if (title.n) bb.y0 -= 30;
    if (!buf_reserve(out, 4096 + N * 400 + g->ne * 300 + M * 120)) return CMM_ERR_NOMEM;
    DG_HEAD(out, bb, 8, "classDiagram", CSS);
    buf_lit(out, DEFS);
    if (title.n) dg_text(out, title, 18, (bb.x0 + bb.x1) / 2, bb.y0 + 12, "middle", "title");
    for (size_t s = 0; s < g->ns; s++) {
        if (g->sw[s] <= 0) continue;
        dg_rect(out, g->sx[s], g->sy[s], g->sw[s], g->sh[s], 0, "ns");
        dg_text(out, g->stitle[s], fs, g->sx[s] + g->sw[s] / 2, g->sy[s] + 14, "middle", "nst");
    }
    for (size_t e = 0; e < g->ne; e++) {
        const Rel *r = &p.rel.v[e];
        if (!g->eptn[e] && g->efrom[e] != g->eto[e]) continue;
        buf_lit(out, "<path class=\"rel");
        if (r->dotted) buf_lit(out, " dot");
        buf_lit(out, "\" d=\"");
        dg_edge_d(out, g, e);
        buf_ch(out, '"');
        const char *ms_ = mark_id(r->ma, false), *me = mark_id(r->mb, true);
        if (ms_) { buf_lit(out, " marker-start=\"url(#"); buf_put(out, strlen(ms_), ms_); buf_lit(out, ")\""); }
        if (me) { buf_lit(out, " marker-end=\"url(#"); buf_put(out, strlen(me), me); buf_lit(out, ")\""); }
        buf_lit(out, "/>");
        uint32_t np = g->eptn[e];
        if (np >= 2) {
            const double *P = g->pts.v + g->eptoff[e];
            if (r->ca.n) card_label(out, P, P + 2, r->ca, 13);
            if (r->cb.n) card_label(out, P + 2 * (np - 1), P + 2 * (np - 2), r->cb, 13);
        }
        if (g->elabel[e].n) {
            dg_rect(out, g->elx[e] - g->elw[e] / 2, g->ely[e] - g->elh[e] / 2, g->elw[e], g->elh[e], 0, "lbl");
            dg_text(out, g->elabel[e], fs, g->elx[e], g->ely[e], "middle", nullptr);
        }
    }
    for (size_t v = 0; v < N; v++) {
        const CInfo *c = &p.ci.v[v];
        double w = g->nw[v], h = g->nh[v], x0 = g->nx[v] - w / 2, y = g->ny[v] - h / 2;
        if (c->note) {
            dg_rect(out, x0, y, w, h, 0, "note");
            dg_text(out, g->nlabel[v], fs, g->nx[v], g->ny[v], "middle", nullptr);
            continue;
        }
        if (c->iface) { dg_text(out, g->nlabel[v], fs, g->nx[v], g->ny[v], "middle", nullptr); continue; }
        dg_rect(out, x0, y, w, h, 0, "cls");
        double ltw;
        unsigned lln;
        label_measure(g->nlabel[v], fs, &ltw, &lln);
        double O = y + CPAD, A = c->annot.n ? lh : 0, L = (lln ? lln : 1) * lh;
        unsigned na = 0;
        for (uint32_t i = 0; i < c->mn; i++) if (!(ms[c->m0 + i].flags & MF_METHOD)) na++;
        if (c->annot.n) {
            buf_lit(out, "<text class=\"ann\" x=\""); buf_num(out, g->nx[v]); buf_lit(out, "\" y=\"");
            buf_num(out, O + lh / 2); buf_lit(out, "\" text-anchor=\"middle\" dominant-baseline=\"central\">&#171;");
            buf_xml(out, c->annot); buf_lit(out, "&#187;</text>");
        }
        dg_text(out, g->nlabel[v], fs, g->nx[v], O + A + L / 2, "middle", "title");
        if (!c->mn && hide_empty) continue;
        double d1 = O + A + L + SECP, d2 = d1 + sec_h(na, lh);
        dg_line(out, x0, d1, x0 + w, d1, "div");
        dg_line(out, x0, d2, x0 + w, d2, "div");
        double my[2] = {d1 + SECP, d2 + SECP};
        for (unsigned pass = 0; pass < 2; pass++) {
            double yy = my[pass];
            for (uint32_t i = 0; i < c->mn; i++) {
                const Member *m = &ms[c->m0 + i];
                if (!(m->flags & MF_METHOD) != !pass) continue;
                const char *mc = m->flags & MF_STATIC ? "static" : m->flags & MF_ABSTRACT ? "abstract" : nullptr;
                dg_text(out, m->text, fs, x0 + CPAD, yy + lh / 2, "start", mc);
                yy += lh;
            }
        }
    }
    buf_lit(out, "</svg>\n");
    return out->oom ? CMM_ERR_NOMEM : CMM_OK;
}
