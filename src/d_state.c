/* stateDiagram / stateDiagram-v2: composite states (nested clusters), concurrency regions (--),
 * [*] start/end per scope, fork/join/choice, descriptions, notes, classDef/class/:::, direction.
 * Layout reuses flow_layout via a pre-sized Graph (dgraph.h). */
#include <stdio.h>

#include <math.h>

#include "dgraph.h"
#include "diagrams.h"
#include "text.h"

typedef enum : uint8_t { SK_STATE, SK_START, SK_END, SK_FORK, SK_JOIN, SK_CHOICE, SK_COMPOSITE } SKind;
typedef struct Note { int32_t v; bool left; Str text; } Note;
typedef struct Frame { int32_t comp, region; unsigned nregions; } Frame;
typedef struct CDef { Str name, css; } CDef;

typedef struct StP {
    Arena *A;
    Graph g;
    VEC(uint8_t) kind; /* SKind per node */
    VEC(int32_t) ccl;  /* composite node -> its cluster, else NONE */
    VEC(Str) desc;     /* description lines joined by '\n' */
    VEC(Note) notes;
    VEC(CDef) cdefs;
    Frame stk[64];
    unsigned depth;
    size_t *err_off;
    char *msg;
} StP;

static cmm_status sfail(StP *p, size_t off, const char *m) { return diag_fail(p->err_off, p->msg, off, m); }

static int32_t cur_scope(const StP *p) { return p->depth ? p->stk[p->depth - 1].region : NONE; }

static Str cat3(Arena *A, Str a, Str b, Str c) {
    char *o = arena_alloc(A, a.n + b.n + c.n + 1, 1);
    if (!o) return (Str){};
    if (a.n) memcpy(o, a.p, a.n);
    if (b.n) memcpy(o + a.n, b.p, b.n);
    if (c.n) memcpy(o + a.n + b.n, c.p, c.n);
    return (Str){.p = o, .n = a.n + b.n + c.n};
}

[[nodiscard]] static int32_t state(StP *p, Str id, SKind k) {
    bool fresh;
    int32_t v = dg_node(&p->g, id, &fresh);
    if (v == NONE) return NONE;
    if (fresh) {
        if (!VEC_PUSH(p->A, &p->kind, (uint8_t)k) || !VEC_PUSH(p->A, &p->desc, (Str){}) ||
            !VEC_PUSH(p->A, &p->ccl, NONE))
            return NONE;
        p->g.nparent[v] = cur_scope(p);
    } else if (p->g.nparent[v] == NONE && cur_scope(p) != NONE && p->kind.v[v] != SK_COMPOSITE) {
        p->g.nparent[v] = cur_scope(p); /* first referenced outside, defined inside a composite */
    }
    if (k != SK_STATE) p->kind.v[v] = (uint8_t)k;
    return v;
}

/* resolve "[*]" to a per-scope start/end pseudo state */
[[nodiscard]] static int32_t endpoint(StP *p, Str id, bool src) {
    Str cls = {};
    for (size_t k = 0; k + 3 <= id.n; k++)
        if (id.p[k] == ':' && id.p[k + 1] == ':' && id.p[k + 2] == ':') {
            cls = (Str){.p = id.p + k + 3, .n = id.n - k - 3};
            id.n = k;
            break;
        }
    id = str_trim(id);
    if (str_eq(id, S("[*]"))) {
        int32_t sc = cur_scope(p);
        Str scope = sc == NONE ? S("") : p->g.sid[sc];
        Str nid = cat3(p->A, scope, S("\x01"), src ? S("start") : S("end"));
        if (!nid.n) return NONE;
        return state(p, nid, src ? SK_START : SK_END);
    }
    int32_t v = state(p, id, SK_STATE);
    if (v != NONE && cls.n) p->g.nclass[v] = str_trim(cls);
    return v;
}

static cmm_status open_composite(StP *p, int32_t v, size_t off) {
    if (p->depth >= countof(p->stk)) return sfail(p, off, "states nested too deeply");
    Graph *g = &p->g;
    p->kind.v[v] = SK_COMPOSITE;
    g->nflags[v] |= NF_SUBREF | NF_HIDDEN;
    int32_t c = NONE;
    for (size_t s = 0; s < g->ns; s++) if (str_eq(g->sid[s], g->nid[v])) { c = (int32_t)s; break; }
    if (c == NONE) {
        c = dg_cluster(g, g->nid[v], g->nlabel[v], g->nparent[v]);
        if (c == NONE) return CMM_ERR_NOMEM;
    }
    p->ccl.v[v] = c;
    p->stk[p->depth++] = (Frame){.comp = c, .region = c, .nregions = 1};
    return CMM_OK;
}

static cmm_status new_region(StP *p, size_t off) {
    if (!p->depth) return sfail(p, off, "'--' outside a composite state");
    Graph *g = &p->g;
    Frame *f = &p->stk[p->depth - 1];
    char buf[24];
    if (f->nregions == 1) { /* move existing children into region 0 */
        Str id = cat3(p->A, g->sid[f->comp], S("\x02r0"), S(""));
        int32_t r0 = id.n ? dg_cluster(g, id, (Str){}, f->comp) : NONE;
        if (r0 == NONE) return CMM_ERR_NOMEM;
        g->sclass[r0] = S("region");
        for (size_t v = 0; v < g->nn; v++) if (g->nparent[v] == f->comp) g->nparent[v] = r0;
        for (size_t s = 0; s < g->ns; s++) if ((int32_t)s != r0 && g->sparent[s] == f->comp) g->sparent[s] = r0;
    }
    int len = snprintf(buf, sizeof buf, "\x02r%u", f->nregions++);
    Str id = cat3(p->A, g->sid[f->comp], (Str){.p = buf, .n = (size_t)len}, S(""));
    int32_t r = id.n ? dg_cluster(g, id, (Str){}, f->comp) : NONE;
    if (r == NONE) return CMM_ERR_NOMEM;
    g->sclass[r] = S("region");
    f->region = r;
    return CMM_OK;
}

static cmm_status add_desc(StP *p, int32_t v, Str d) {
    bool ok;
    d = label_normalize(p->A, d, &ok);
    if (!ok) return CMM_ERR_NOMEM;
    if (!d.n) return CMM_OK;
    Str *dst = &p->desc.v[v];
    /* mermaid: a lone description replaces the id as the label (plain state); a second one, or one
       on an aliased state, gives the title + divider shape */
    if (!dst->n && str_eq(p->g.nlabel[v], p->g.nid[v])) { p->g.nlabel[v] = d; return CMM_OK; }
    if (!dst->n) { *dst = d; return CMM_OK; }
    *dst = cat3(p->A, *dst, S("\n"), d);
    return dst->n ? CMM_OK : CMM_ERR_NOMEM;
}

static const char *find_sub(Str s, Str needle) {
    for (size_t k = 0; k + needle.n <= s.n; k++)
        if (memcmp(s.p + k, needle.p, needle.n) == 0) return s.p + k;
    return nullptr;
}

static cmm_status parse(StP *p, LineIt *it) {
    Str line;
    size_t off;
    Graph *g = &p->g;
    while (dg_next_line(it, &line, &off)) {
        Str rest = line, w = dg_word(&rest);
        if (str_eq(w, S("direction"))) {
            Str d = dg_word(&rest);
            Dir dd;
            if (str_eq(d, S("LR"))) dd = DIR_LR;
            else if (str_eq(d, S("RL"))) dd = DIR_RL;
            else if (str_eq(d, S("BT"))) dd = DIR_BT;
            else if (str_eq(d, S("TB")) || str_eq(d, S("TD"))) dd = DIR_TB;
            else return sfail(p, off, "invalid direction");
            if (p->depth) g->sdir[p->stk[p->depth - 1].comp] = dd; /* per-composite hint */
            else g->dir = dd;
            continue;
        }
        if (str_eq(line, S("}"))) {
            if (!p->depth) return sfail(p, off, "unbalanced '}'");
            p->depth--;
            continue;
        }
        if (str_eq(line, S("--"))) { cmm_status st = new_region(p, off); if (st) return st; continue; }
        if (str_starts(w, S("accTitle")) || str_starts(w, S("accDescr")) || str_eq(w, S("hide")) ||
            str_eq(w, S("scale")) || str_eq(w, S("style")) || str_eq(w, S("click")))
            continue;
        if (str_eq(w, S("classDef"))) {
            Str name = dg_word(&rest);
            if (!name.n) return sfail(p, off, "classDef needs a name");
            if (!VEC_PUSH(p->A, &p->cdefs, ((CDef){.name = name, .css = rest}))) return CMM_ERR_NOMEM;
            continue;
        }
        if (str_eq(w, S("class"))) {
            Str ids = dg_word(&rest), cls = dg_word(&rest);
            while (ids.n) {
                const char *c = memchr(ids.p, ',', ids.n);
                Str id = {.p = ids.p, .n = c ? (size_t)(c - ids.p) : ids.n};
                int32_t v = state(p, str_trim(id), SK_STATE);
                if (v == NONE) return CMM_ERR_NOMEM;
                g->nclass[v] = cls;
                ids = c ? (Str){.p = c + 1, .n = ids.n - id.n - 1} : (Str){};
            }
            continue;
        }
        if (str_ieq(w, S("note"))) {
            Str side = dg_word(&rest), of = dg_word(&rest);
            if ((!str_eq(side, S("left")) && !str_eq(side, S("right"))) || !str_eq(of, S("of")))
                return sfail(p, off, "expected 'note left of' or 'note right of'");
            const char *c = memchr(rest.p, ':', rest.n);
            Str who = str_trim((Str){.p = rest.p, .n = c ? (size_t)(c - rest.p) : rest.n});
            int32_t v = state(p, who, SK_STATE);
            if (v == NONE) return CMM_ERR_NOMEM;
            Str text;
            if (c) text = (Str){.p = c + 1, .n = rest.n - (size_t)(c + 1 - rest.p)};
            else { /* multi-line until "end note" */
                size_t t0 = it->i, t1 = it->i;
                Str l2;
                size_t o2;
                bool closed = false;
                while (dg_next_line(it, &l2, &o2)) {
                    if (str_ieq(l2, S("end note"))) { closed = true; break; }
                    t1 = o2 + l2.n;
                }
                if (!closed) return sfail(p, off, "note without 'end note'");
                text = t1 > t0 ? (Str){.p = it->s + t0, .n = t1 - t0} : (Str){};
            }
            bool ok;
            Str nt = label_normalize(p->A, text, &ok);
            if (!ok) return CMM_ERR_NOMEM;
            /* collapse indentation of multi-line notes */
            if (memchr(nt.p, '\n', nt.n)) {
                char *o = arena_alloc(p->A, nt.n + 1, 1);
                if (!o) return CMM_ERR_NOMEM;
                size_t k = 0;
                bool bol = true;
                for (size_t i = 0; i < nt.n; i++) {
                    if (bol && is_sp(nt.p[i])) continue;
                    bol = nt.p[i] == '\n';
                    o[k++] = nt.p[i];
                }
                nt = (Str){.p = o, .n = k};
            }
            if (!VEC_PUSH(p->A, &p->notes, ((Note){.v = v, .left = side.n == 4, .text = nt}))) return CMM_ERR_NOMEM;
            continue;
        }
        if (str_eq(w, S("state"))) {
            Str r = rest, label = {};
            bool open = r.n && r.p[r.n - 1] == '{';
            if (open) r = str_trim((Str){.p = r.p, .n = r.n - 1});
            if (r.n && r.p[0] == '"') { /* state "Label" as Id */
                const char *q = memchr(r.p + 1, '"', r.n - 1);
                if (!q) return sfail(p, off, "unterminated string");
                label = (Str){.p = r.p + 1, .n = (size_t)(q - r.p - 1)};
                r = str_trim((Str){.p = q + 1, .n = r.n - (size_t)(q + 1 - r.p)});
                if (!str_starts(r, S("as"))) return sfail(p, off, "expected 'as' after state label");
                r = str_trim((Str){.p = r.p + 2, .n = r.n - 2});
            }
            Str id = dg_word(&r);
            SKind k = SK_STATE;
            if (str_starts(id, S("[*]"))) return sfail(p, off, "invalid state name");
            const char *cc = memchr(id.p, ':', id.n);
            Str desc = {};
            if (cc && !(cc + 2 < id.p + id.n && cc[1] == ':' && cc[2] == ':')) { /* state A:desc */
                desc = (Str){.p = cc + 1, .n = id.n - (size_t)(cc + 1 - id.p)};
                id.n = (size_t)(cc - id.p);
            }
            if (str_eq(r, S("<<fork>>")) || str_eq(r, S("[[fork]]"))) k = SK_FORK;
            else if (str_eq(r, S("<<join>>")) || str_eq(r, S("[[join]]"))) k = SK_JOIN;
            else if (str_eq(r, S("<<choice>>")) || str_eq(r, S("[[choice]]"))) k = SK_CHOICE;
            else if (r.n && r.p[0] == ':') desc = (Str){.p = r.p + 1, .n = r.n - 1};
            if (!id.n) return sfail(p, off, "expected state name");
            int32_t v = endpoint(p, id, true);
            if (v == NONE) return CMM_ERR_NOMEM;
            if (k != SK_STATE) p->kind.v[v] = (uint8_t)k;
            if (label.n) {
                bool ok;
                g->nlabel[v] = label_normalize(p->A, label, &ok);
                if (!ok) return CMM_ERR_NOMEM;
            }
            if (desc.n) { cmm_status st = add_desc(p, v, desc); if (st) return st; }
            if (open) { cmm_status st = open_composite(p, v, off); if (st) return st; }
            continue;
        }
        const char *arr = find_sub(line, S("-->"));
        if (arr) {
            Str a = str_trim((Str){.p = line.p, .n = (size_t)(arr - line.p)});
            Str r = {.p = arr + 3, .n = line.n - (size_t)(arr + 3 - line.p)};
            const char *c = memchr(r.p, ':', r.n);
            while (c && c + 2 < r.p + r.n && c[1] == ':' && c[2] == ':') /* skip ::: */
                c = memchr(c + 3, ':', (size_t)(r.p + r.n - c - 3));
            Str b = str_trim((Str){.p = r.p, .n = c ? (size_t)(c - r.p) : r.n});
            Str lab = c ? (Str){.p = c + 1, .n = r.n - (size_t)(c + 1 - r.p)} : (Str){};
            if (!a.n || !b.n) return sfail(p, off, "transition needs two states");
            int32_t va = endpoint(p, a, true), vb = endpoint(p, b, false);
            if (va == NONE || vb == NONE) return CMM_ERR_NOMEM;
            bool ok;
            lab = label_normalize(p->A, lab, &ok);
            if (!ok || dg_edge(g, va, vb, lab) == NONE) return CMM_ERR_NOMEM;
            continue;
        }
        /* "Id : description" or bare "Id" */
        const char *c = memchr(line.p, ':', line.n);
        Str id = str_trim((Str){.p = line.p, .n = c ? (size_t)(c - line.p) : line.n});
        if (c && c + 2 < line.p + line.n && c[1] == ':' && c[2] == ':') { id = line; c = nullptr; }
        for (size_t k = 0; k < id.n; k++)
            if (is_sp(id.p[k]) || id.p[k] == '{' || id.p[k] == '}' || id.p[k] == '-' || id.p[k] == '<' || id.p[k] == '>')
                return sfail(p, off + k, "unrecognised statement in state diagram");
        if (!id.n) return sfail(p, off, "unrecognised statement in state diagram");
        int32_t v = endpoint(p, id, true);
        if (v == NONE) return CMM_ERR_NOMEM;
        if (c) {
            cmm_status st = add_desc(p, v, (Str){.p = c + 1, .n = line.n - (size_t)(c + 1 - line.p)});
            if (st) return st;
        }
    }
    if (p->depth) return sfail(p, it->n, "unterminated composite state (missing '}')");
    return CMM_OK;
}

static constexpr char CSS[] =
    "#mermaid .st{fill:#ECECFF;stroke:#9370DB;stroke-width:1px;}"
    "#mermaid .bar,#mermaid .start{fill:#333;stroke:none;}"
    "#mermaid .endo{fill:#fff;stroke:#333;stroke-width:1.5px;}#mermaid .endi{fill:#333;}"
    "#mermaid .choice{fill:#ECECFF;stroke:#9370DB;stroke-width:1px;}"
    "#mermaid .comp{fill:#fff;stroke:#9370DB;stroke-width:1px;}"
    "#mermaid .comp-t{fill:#ECECFF;stroke:#9370DB;stroke-width:1px;}"
    "#mermaid .region{fill:none;stroke:#888;stroke-width:1px;stroke-dasharray:5,4;}"
    "#mermaid .div{stroke:#9370DB;stroke-width:1px;}"
    "#mermaid .tr{fill:none;stroke:#333;stroke-width:1.3px;}"
    "#mermaid .lbl{fill:#e8e8e8;opacity:0.85;}"
    "#mermaid .note{fill:#fff5ad;stroke:#aaaa33;stroke-width:1px;}"
    "#mermaid .note-l{stroke:#aaaa33;stroke-width:1px;stroke-dasharray:3,3;}";

static void css_attr(Buf *b, const StP *p, Str cls) {
    for (size_t i = 0; i < p->cdefs.n; i++) {
        if (!str_eq(p->cdefs.v[i].name, cls)) continue;
        Str c = p->cdefs.v[i].css;
        buf_lit(b, " style=\"");
        for (size_t k = 0; k < c.n; k++) {
            char ch = c.p[k] == ',' ? ';' : c.p[k];
            if (ch == '"' || ch == '<' || ch == '&') continue;
            buf_ch(b, ch);
        }
        buf_ch(b, '"');
        return;
    }
}


/* ---- recursive layout: each composite / region is laid out on its own and becomes a sized node
 * in its parent (as mermaid does), so clusters stay compact and nested `direction` is honoured. */
constexpr double C_PAD = 16;   /* composite inner padding */
constexpr double C_TITLE = 26; /* composite title bar */
constexpr double R_PAD = 10;   /* region padding */


typedef struct Scope {
    Graph lg;
    int32_t *elem;      /* local node -> element (node v >= 0, cluster s -> -(s+2)) */
    double w, h, bx0, by0;
    bool regions;       /* composite whose children are concurrent regions */
    size_t nreal;       /* layout edges [0, nreal) are note connectors (mermaid adds them first) */
} Scope;
typedef struct EdgeLoc { int32_t scope, ra, rb; } EdgeLoc; /* scope as cluster id (NONE = root) */

static inline int32_t el_cl(int32_t s) { return -(s + 2); }
/* notes are layout nodes too (mermaid: note node + undirected edge, inside an invisible noteGroup) */
constexpr int32_t NOTE_EL0 = INT32_MIN + 1;
static inline bool el_is_note(int32_t e) { return e >= NOTE_EL0 && e < NOTE_EL0 + (1 << 24); }
static inline int32_t sc_ix(int32_t c) { return c + 1; } /* scope index: 0 = root */

static int32_t elem_parent(const StP *p, int32_t e) {
    return e >= 0 ? p->g.nparent[e] : p->g.sparent[-e - 2];
}
static int32_t node_elem(const StP *p, int32_t v) { return p->ccl.v[v] != NONE ? el_cl(p->ccl.v[v]) : v; }

/* path root..leaf of elements; returns length */
static unsigned elem_path(const StP *p, int32_t e, int32_t out[static 160]) {
    int32_t tmp[160];
    unsigned k = 0;
    tmp[k++] = e;
    for (int32_t c = elem_parent(p, e); c != NONE && k < 160; c = p->g.sparent[c]) tmp[k++] = el_cl(c);
    for (unsigned i = 0; i < k; i++) out[i] = tmp[k - 1 - i];
    return k;
}

typedef struct Ctx {
    StP *p;
    Scope *sc;
    EdgeLoc *eloc;
    double *ax, *ay;   /* absolute centre per global node */
    double *cx, *cy, *cw, *ch; /* absolute box per cluster */
    double *relx;      /* region x offset inside its composite */
    double *npad, *nph; /* per global node: horizontal note reservation each side, note height */
    double *cpad, *cph; /* same per cluster */
    double *ntx, *nty, *ntw, *nth; /* per note: absolute centre and size */
    double fs;
} Ctx;

static cmm_status layout_scope(Ctx *c, int32_t cl) {
    StP *p = c->p;
    Graph *g = &p->g;
    Scope *s = &c->sc[sc_ix(cl)];
    if (cl != NONE) {
        bool any = false;
        for (size_t r = 0; r < g->ns; r++) any |= g->sparent[r] == cl && g->sclass[r].n;
        if (any) { /* concurrent regions side by side */
            s->regions = true;
            double x = 0, h = 0;
            for (size_t r = 0; r < g->ns; r++) {
                if (g->sparent[r] != cl || !g->sclass[r].n) continue;
                const Scope *rs = &c->sc[sc_ix((int32_t)r)];
                c->relx[r] = x;
                x += rs->w + 2 * R_PAD;
                if (rs->h + 2 * R_PAD > h) h = rs->h + 2 * R_PAD;
            }
            s->w = x, s->h = h, s->bx0 = s->by0 = 0;
            for (size_t r = 0; r < g->ns; r++)
                if (g->sparent[r] == cl && g->sclass[r].n) c->ch[r] = h;
            return CMM_OK;
        }
    }
    Graph *lg = &s->lg;
    dg_init(lg, p->A, cl == NONE ? g->dir : g->sdir[cl], c->fs);
    size_t cap = 16, cnt = 0;
    s->elem = ANEW(p->A, int32_t, cap);
    if (!s->elem) return CMM_ERR_NOMEM;
    #define ADD_ELEM(E, ID, W, H, SH)                                                     \
        do {                                                                              \
            int32_t lv_ = dg_node(lg, (ID), nullptr);                                     \
            if (lv_ == NONE) return CMM_ERR_NOMEM;                                        \
            if (cnt == cap) {                                                             \
                int32_t *ne_ = arena_grow(p->A, s->elem, &cap, sizeof(int32_t), alignof(int32_t)); \
                if (!ne_) return CMM_ERR_NOMEM;                                           \
                s->elem = ne_;                                                            \
            }                                                                             \
            s->elem[cnt++] = (E);                                                         \
            lg->nshape[lv_] = (SH);                                                       \
            dg_size(lg, lv_, (W), (H));                                                   \
        } while (0)
    /* mermaid inserts a state's note node before the state itself (dagre initial order) */
    #define ADD_NOTES(V, R)                                                                   \
        for (size_t i = 0; i < p->notes.n; i++) {                                             \
            int32_t nv_ = p->notes.v[i].v;                                                    \
            if ((R) == NONE ? nv_ != (int32_t)(V) || p->ccl.v[nv_] != NONE : p->ccl.v[nv_] != (R)) continue; \
            char tag[24];                                                                     \
            int len = snprintf(tag, sizeof tag, "----note-%zu", i);                           \
            Str nid = cat3(p->A, (R) == NONE ? g->nid[nv_] : g->sid[R], (Str){.p = tag, .n = (size_t)len}, S("")); \
            if (!nid.n) return CMM_ERR_NOMEM;                                                 \
            ADD_ELEM(NOTE_EL0 + (int32_t)i, nid, c->ntw[i], c->nth[i], SH_RECT);              \
        }
    for (size_t v = 0; v < g->nn; v++)
        if (g->nparent[v] == cl && p->ccl.v[v] == NONE) {
            ADD_NOTES(v, NONE);
            ADD_ELEM((int32_t)v, g->nid[v], g->nw[v] + 2 * c->npad[v], g->nh[v] > c->nph[v] ? g->nh[v] : c->nph[v],
                     g->nshape[v]);
        }
    for (size_t r = 0; r < g->ns; r++) {
        if (g->sparent[r] != cl || g->sclass[r].n) continue;
        const Scope *rs = &c->sc[sc_ix((int32_t)r)];
        double w = rs->w + 2 * C_PAD, h = rs->h + 2 * C_PAD + C_TITLE, tw = text_width(g->stitle[r], c->fs) + 30;
        if (w < tw) w = tw;
        if (w < 60) w = 60;
        c->cw[r] = w, c->ch[r] = h; /* true size; the layout node may be widened for notes */
        ADD_NOTES(r, (int32_t)r);
        ADD_ELEM(el_cl((int32_t)r), g->sid[r], w + 2 * c->cpad[r], h > c->cph[r] ? h : c->cph[r], SH_RECT);
    }
    #undef ADD_NOTES
    #undef ADD_ELEM
    for (size_t lv = 0; lv < lg->nn; lv++) {
        if (!el_is_note(s->elem[lv])) continue;
        size_t i = (size_t)(s->elem[lv] - NOTE_EL0);
        int32_t v = p->notes.v[i].v, r = p->ccl.v[v];
        int32_t sv = dg_find(lg, r == NONE ? g->nid[v] : g->sid[r]);
        if (sv == NONE) continue;
        bool left = p->notes.v[i].left; /* mermaid: right of = state -> note, left of = note -> state */
        if (dg_edge(lg, left ? (int32_t)lv : sv, left ? sv : (int32_t)lv, (Str){}) == NONE) return CMM_ERR_NOMEM;
    }
    s->nreal = lg->ne;
    for (size_t e = 0; e < g->ne; e++) {
        const EdgeLoc *L = &c->eloc[e];
        if (L->scope != cl || L->ra == INT32_MIN) continue;
        Str ia = L->ra >= 0 ? g->nid[L->ra] : g->sid[-L->ra - 2], ib = L->rb >= 0 ? g->nid[L->rb] : g->sid[-L->rb - 2];
        int32_t la = dg_find(lg, ia), lb = dg_find(lg, ib);
        if (la == NONE || lb == NONE) continue;
        if (dg_edge(lg, la, lb, g->elabel[e]) == NONE) return CMM_ERR_NOMEM;
    }
    if (!lg->nn) { s->w = s->h = 0; s->bx0 = s->by0 = 0; return CMM_OK; }
    cmm_status st = flow_layout(lg);
    if (st) return st;
    /* re-clip edge ends at the true box of elements widened to reserve note space */
    for (size_t e = 0; e < lg->ne; e++) {
        uint32_t np = lg->eptn[e];
        if (np < 2 || lg->efrom[e] == lg->eto[e]) continue;
        double *P = lg->pts.v + lg->eptoff[e];
        for (unsigned end = 0; end < 2; end++) {
            int32_t lv = end ? lg->eto[e] : lg->efrom[e], el = s->elem[lv];
            if (el_is_note(el)) continue;
            double pad = el >= 0 ? c->npad[el] : c->cpad[-el - 2];
            if (pad <= 0) continue;
            double hw = (lg->nw[lv] - 2 * pad) / 2, hh = (el >= 0 ? g->nh[el] : c->ch[-el - 2]) / 2;
            double *E0 = end ? P + 2 * (np - 1) : P, *Q = end ? P + 2 * (np - 2) : P + 2;
            double cx = lg->nx[lv], cy = lg->ny[lv], dx = Q[0] - cx, dy = Q[1] - cy;
            double tx = fabs(dx) > 1e-9 ? hw / fabs(dx) : 1e300, ty = fabs(dy) > 1e-9 ? hh / fabs(dy) : 1e300;
            double t = tx < ty ? tx : ty;
            if (t < 1) E0[0] = cx + dx * t, E0[1] = cy + dy * t;
        }
    }
    BBox bb = dg_bbox(lg);
    s->bx0 = bb.x0, s->by0 = bb.y0, s->w = bb.x1 - bb.x0, s->h = bb.y1 - bb.y0;
    return CMM_OK;
}

static void translate(Graph *lg, double dx, double dy) {
    for (size_t v = 0; v < lg->nn; v++) lg->nx[v] += dx, lg->ny[v] += dy;
    for (size_t i = 0; i + 1 < lg->pts.n; i += 2) lg->pts.v[i] += dx, lg->pts.v[i + 1] += dy;
    for (size_t e = 0; e < lg->ne; e++) lg->elx[e] += dx, lg->ely[e] += dy;
}

static void place(Ctx *c, int32_t cl, double ox, double oy, unsigned depth) {
    StP *p = c->p;
    Graph *g = &p->g;
    Scope *s = &c->sc[sc_ix(cl)];
    if (depth > 200) return;
    if (s->regions) {
        for (size_t r = 0; r < g->ns; r++) {
            if (g->sparent[r] != cl || !g->sclass[r].n) continue;
            const Scope *rs = &c->sc[sc_ix((int32_t)r)];
            c->cx[r] = ox + c->relx[r], c->cy[r] = oy, c->cw[r] = rs->w + 2 * R_PAD;
            place(c, (int32_t)r, ox + c->relx[r] + R_PAD - rs->bx0, oy + R_PAD - rs->by0, depth + 1);
        }
        return;
    }
    Graph *lg = &s->lg;
    translate(lg, ox, oy);
    for (size_t lv = 0; lv < lg->nn; lv++) {
        int32_t e = s->elem[lv];
        double x = lg->nx[lv], y = lg->ny[lv], w = lg->nw[lv], h = lg->nh[lv];
        if (e >= 0) { c->ax[e] = x, c->ay[e] = y; continue; }
        if (el_is_note(e)) { c->ntx[e - NOTE_EL0] = x, c->nty[e - NOTE_EL0] = y; continue; }
        int32_t r = -e - 2;
        (void)w;
        h = c->ch[r];
        c->cx[r] = x - c->cw[r] / 2, c->cy[r] = y - h / 2;
        const Scope *rs = &c->sc[sc_ix(r)];
        double ix = x - rs->w / 2, iy = y - h / 2 + C_TITLE + C_PAD; /* centre content horizontally */
        place(c, r, ix - rs->bx0, iy - rs->by0, depth + 1);
    }
}

DIAG_FN(state_render) {
    StP p = {.A = a, .err_off = err_off, .msg = msg};
    dg_init(&p.g, a, DIR_TB, fs);
    LineIt it = {.s = src, .n = n, .i = off};
    Str hdr;
    size_t ho;
    if (!dg_next_line(&it, &hdr, &ho)) return diag_fail(err_off, msg, off, "empty state diagram");
    cmm_status st = parse(&p, &it);
    if (st) return st;
    Graph *g = &p.g;
    if (!g->nn) return diag_fail(err_off, msg, off, "state diagram has no states");
    double lh = fs * 1.5;
    bool horiz = g->dir == DIR_LR || g->dir == DIR_RL;
    for (size_t v = 0; v < g->nn; v++) {
        double w = 0, h = 0;
        unsigned ln;
        switch ((SKind)p.kind.v[v]) {
        case SK_START: case SK_END: w = h = 14; g->nshape[v] = SH_CIRCLE; break;
        case SK_FORK: case SK_JOIN: {
            int32_t c = g->nparent[v];
            Dir d = c == NONE ? g->dir : g->sdir[c];
            bool hz = d == DIR_LR || d == DIR_RL;
            w = hz ? 10 : 70, h = hz ? 70 : 10;
            break;
        }
        case SK_CHOICE: w = h = 28; g->nshape[v] = SH_RHOMBUS; break;
        case SK_COMPOSITE: w = h = 1; break;
        case SK_STATE: {
            double tw, dw = 0;
            label_measure(g->nlabel[v], fs, &tw, &ln);
            unsigned dl = 0;
            label_measure(p.desc.v[v], fs, &dw, &dl);
            w = (tw > dw ? tw : dw) + 30;
            if (w < 50) w = 50;
            h = (ln ? ln : 1) * lh + 16 + (dl ? dl * lh + 10 : 0);
            g->nshape[v] = SH_ROUND;
            break;
        }
        }
        g->nw[v] = w, g->nh[v] = h;
    }
    (void)horiz;
    size_t S = g->ns, N = g->nn, E = g->ne;
    Ctx c = {.p = &p, .fs = fs};
    c.sc = ANEW(a, Scope, S + 1);
    c.eloc = ANEW(a, EdgeLoc, E + 1);
    c.ax = ANEW(a, double, N); c.ay = ANEW(a, double, N);
    c.cx = ANEW(a, double, S + 1); c.cy = ANEW(a, double, S + 1);
    c.cw = ANEW(a, double, S + 1); c.ch = ANEW(a, double, S + 1); c.relx = ANEW(a, double, S + 1);
    int32_t *sdep = ANEW(a, int32_t, S + 1);
    if (!c.sc || !c.eloc || !c.ax || !c.ay || !c.cx || !c.cy || !c.cw || !c.ch || !c.relx || !sdep) return CMM_ERR_NOMEM;
    memset(c.sc, 0, (S + 1) * sizeof *c.sc);
    memset(c.ax, 0, N * sizeof *c.ax);
    memset(c.ay, 0, N * sizeof *c.ay);
    for (size_t r = 0; r <= S; r++) c.cx[r] = c.cy[r] = c.cw[r] = c.ch[r] = c.relx[r] = 0;
    /* where does each edge live? deepest common scope, with the children-of-scope representatives */
    for (size_t e = 0; e < E; e++) {
        int32_t pa[160], pb[160];
        unsigned na = elem_path(&p, node_elem(&p, g->efrom[e]), pa), nb = elem_path(&p, node_elem(&p, g->eto[e]), pb);
        unsigned k = 0;
        while (k < na && k < nb && pa[k] == pb[k]) k++;
        c.eloc[e] = (EdgeLoc){.scope = NONE, .ra = INT32_MIN, .rb = INT32_MIN};
        if (k == na && k == nb) { /* self loop */
            c.eloc[e] = (EdgeLoc){.scope = elem_parent(&p, pa[na - 1]), .ra = pa[na - 1], .rb = pa[na - 1]};
            continue;
        }
        if (k == na || k == nb) continue; /* edge between a composite and its own content */
        int32_t scope = k ? -pa[k - 1] - 2 : NONE;
        if (scope != NONE && g->sclass[scope].n == 0) {
            bool has_regions = false; /* edges across concurrent regions are not drawn */
            for (size_t r = 0; r < S; r++) has_regions |= g->sparent[r] == scope && g->sclass[r].n;
            if (has_regions) continue;
        }
        c.eloc[e] = (EdgeLoc){.scope = scope, .ra = pa[k], .rb = pb[k]};
    }
    c.npad = ANEW(a, double, N); c.nph = ANEW(a, double, N);
    c.cpad = ANEW(a, double, S + 1); c.cph = ANEW(a, double, S + 1);
    double *nx = ANEW(a, double, p.notes.n + 1), *ny = ANEW(a, double, p.notes.n + 1),
           *nw = ANEW(a, double, p.notes.n + 1), *nh = ANEW(a, double, p.notes.n + 1);
    if (!c.npad || !c.nph || !c.cpad || !c.cph || !nx || !ny || !nw || !nh) return CMM_ERR_NOMEM;
    for (size_t v = 0; v < N; v++) c.npad[v] = c.nph[v] = 0;
    for (size_t r = 0; r <= S; r++) c.cpad[r] = c.cph[r] = 0;
    c.ntx = ANEW(a, double, p.notes.n + 1), c.nty = ANEW(a, double, p.notes.n + 1);
    c.ntw = nw, c.nth = nh;
    if (!c.ntx || !c.nty) return CMM_ERR_NOMEM;
    for (size_t i = 0; i < p.notes.n; i++) { /* mermaid note: text wrapped at 200px, padding 15 */
        Note *no = &p.notes.v[i];
        no->text = dg_wrap(a, no->text, fs, 200);
        double tw;
        unsigned ln;
        label_measure(no->text, fs, &tw, &ln);
        nw[i] = tw + 30, nh[i] = (ln ? ln : 1) * lh + 30;
        c.ntx[i] = c.nty[i] = 0;
    }
    int32_t maxd = 0;
    for (size_t r = 0; r < S; r++) {
        int32_t d = 1;
        for (int32_t q = g->sparent[r]; q != NONE && d < 1000; q = g->sparent[q]) d++;
        sdep[r] = d;
        if (d > maxd) maxd = d;
    }
    for (int32_t d = maxd; d >= 1; d--)
        for (size_t r = 0; r < S; r++)
            if (sdep[r] == d && (st = layout_scope(&c, (int32_t)r))) return st;
    if ((st = layout_scope(&c, NONE))) return st;
    place(&c, NONE, 0, 0, 0);

    BBox bb = BB_EMPTY;
    for (size_t r = 0; r <= S; r++) if (c.sc[r].lg.nn) { BBox b2 = dg_bbox(&c.sc[r].lg); bb_add(&bb, b2.x0, b2.y0, b2.x1, b2.y1); }
    for (size_t r = 0; r < S; r++) if (c.cw[r] > 0) bb_add(&bb, c.cx[r], c.cy[r], c.cx[r] + c.cw[r], c.cy[r] + c.ch[r]);
    for (size_t i = 0; i < p.notes.n; i++) nx[i] = c.ntx[i] - nw[i] / 2, ny[i] = c.nty[i] - nh[i] / 2;
    if (bb.x0 > bb.x1) bb = (BBox){0, 0, 100, 100};
    if (title.n) bb.y0 -= 30;
    if (!buf_reserve(out, 1024 + N * 300 + E * 250)) return CMM_ERR_NOMEM;
    DG_HEAD(out, bb, 10, "statediagram", CSS);
    buf_lit(out, "<defs><marker id=\"st-arrow\" viewBox=\"0 0 10 10\" refX=\"9\" refY=\"5\" markerWidth=\"8\" "
                 "markerHeight=\"8\" orient=\"auto\"><path d=\"M0,0 L10,5 L0,10 z\" fill=\"#333\"/></marker></defs>");
    if (title.n) dg_text(out, title, 18, (bb.x0 + bb.x1) / 2, bb.y0 + 12, "middle", "title");
    for (int32_t d = 1; d <= maxd; d++) /* outer clusters first */
        for (size_t r = 0; r < S; r++) {
            if (sdep[r] != d || c.cw[r] <= 0) continue;
            double x = c.cx[r], y = c.cy[r], w = c.cw[r], h = c.ch[r];
            if (g->sclass[r].n) {
                dg_rect(out, x + 2, y + 2, w - 4, h - 4, 0, "region"); /* concurrent region: dashed box */
                continue;
            }
            dg_rect(out, x, y, w, h, 5, "comp");
            buf_lit(out, "<path class=\"comp-t\" d=\"M"); dg_pt(out, x, y + C_TITLE);
            buf_lit(out, " v-21 a5,5 0 0 1 5,-5 h"); buf_num(out, w - 10);
            buf_lit(out, " a5,5 0 0 1 5,5 v21 z\"/>");
            dg_text(out, g->stitle[r], fs, x + w / 2, y + C_TITLE / 2, "middle", nullptr);
        }
    for (size_t si = 0; si <= S; si++) {
        const Graph *lg = &c.sc[si].lg;
        for (size_t e = 0; e < lg->ne; e++) {
            if (!lg->eptn[e] && lg->efrom[e] != lg->eto[e]) continue;
            bool note = e < c.sc[si].nreal;
            if (note) buf_lit(out, "<path class=\"note-l\" fill=\"none\" d=\"");
            else buf_lit(out, "<path class=\"tr\" d=\"");
            dg_edge_d(out, lg, e);
            if (note) buf_lit(out, "\"/>");
            else buf_lit(out, "\" marker-end=\"url(#st-arrow)\"/>");
        }
        for (size_t e = 0; e < lg->ne; e++) {
            if (!lg->elabel[e].n) continue;
            dg_rect(out, lg->elx[e] - lg->elw[e] / 2, lg->ely[e] - lg->elh[e] / 2, lg->elw[e], lg->elh[e], 0, "lbl");
            dg_text(out, lg->elabel[e], fs, lg->elx[e], lg->ely[e], "middle", nullptr);
        }
    }
    for (size_t v = 0; v < N; v++) {
        double x = c.ax[v], y = c.ay[v], w = g->nw[v], h = g->nh[v];
        switch ((SKind)p.kind.v[v]) {
        case SK_COMPOSITE: break;
        case SK_START:
            buf_lit(out, "<circle class=\"start\" cx=\""); buf_num(out, x); buf_lit(out, "\" cy=\""); buf_num(out, y);
            buf_lit(out, "\" r=\"7\"/>");
            break;
        case SK_END:
            buf_lit(out, "<circle class=\"endo\" cx=\""); buf_num(out, x); buf_lit(out, "\" cy=\""); buf_num(out, y);
            buf_lit(out, "\" r=\"7\"/><circle class=\"endi\" cx=\""); buf_num(out, x); buf_lit(out, "\" cy=\"");
            buf_num(out, y); buf_lit(out, "\" r=\"4\"/>");
            break;
        case SK_FORK: case SK_JOIN: dg_rect(out, x - w / 2, y - h / 2, w, h, 0, "bar"); break;
        case SK_CHOICE:
            buf_lit(out, "<path class=\"choice\" d=\"M"); dg_pt(out, x, y - h / 2); buf_ch(out, 'L'); dg_pt(out, x + w / 2, y);
            buf_ch(out, 'L'); dg_pt(out, x, y + h / 2); buf_ch(out, 'L'); dg_pt(out, x - w / 2, y); buf_lit(out, "Z\"/>");
            break;
        case SK_STATE: {
            buf_lit(out, "<rect class=\"st\" x=\""); buf_num(out, x - w / 2); buf_lit(out, "\" y=\""); buf_num(out, y - h / 2);
            buf_lit(out, "\" width=\""); buf_num(out, w); buf_lit(out, "\" height=\""); buf_num(out, h);
            buf_lit(out, "\" rx=\"5\"");
            if (g->nclass[v].n) css_attr(out, &p, g->nclass[v]);
            buf_lit(out, "/>");
            if (p.desc.v[v].n) {
                unsigned ln = 0, dl = 0;
                double tw;
                label_measure(g->nlabel[v], fs, &tw, &ln);
                label_measure(p.desc.v[v], fs, &tw, &dl);
                double dy = y - h / 2 + 8 + ln * lh + 5;
                dg_text(out, g->nlabel[v], fs, x, y - h / 2 + 8 + ln * lh / 2, "middle", nullptr);
                dg_line(out, x - w / 2, dy, x + w / 2, dy, "div");
                dg_text(out, p.desc.v[v], fs, x, dy + 5 + dl * lh / 2, "middle", nullptr);
            } else dg_text(out, g->nlabel[v], fs, x, y, "middle", nullptr);
            break;
        }
        }
    }
    for (size_t i = 0; i < p.notes.n; i++) {
        dg_rect(out, nx[i], ny[i], nw[i], nh[i], 0, "note");
        dg_text(out, p.notes.v[i].text, fs, nx[i] + nw[i] / 2, ny[i] + nh[i] / 2, "middle", nullptr);
    }
    buf_lit(out, "</svg>\n");
    return out->oom ? CMM_ERR_NOMEM : CMM_OK;
}
