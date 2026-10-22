/* zenuml: native renderer in ZenUML's own look (not mermaid's sequence look): an outer frame with
 * the title, boxed participant headers with type icons, an unnamed starter column for calls with no
 * explicit caller, hierarchical message numbers (1, 2, 2.1, 2.1.1...), `// comments` above the
 * following message, sync calls with `{ ... }` bodies (activations; `x = A.m() {return y}` shows
 * both the explicit return and the assignment), `new X()` creating an `x:X` box mid-diagram,
 * async `A->B: msg`, and fragments with "Alt/Loop/Opt/Par/Try/Critical/Section" headers
 * (if/else if/else, while/for/forEach/loop, opt, par, try/catch/finally, critical, section/group).
 * Unknown statements are ignored. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base.h"
#include "dgraph.h"
#include "diagrams.h"
#include "dcfg.h"
#include "text.h"

enum { IC_NONE, IC_ACTOR, IC_BOUNDARY, IC_CONTROL, IC_ENTITY, IC_DB, IC_COLL, IC_QUEUE };
typedef struct Part { Str name, label; uint8_t icon; bool used; int32_t create_ev; double x, w, y; } Part;

enum { E_MSG, E_FRAG, E_SEC, E_END, E_ACT_END };
enum { M_SYNC, M_ASYNC, M_RET, M_CREATE };
typedef struct Ev {
    uint8_t k, mk;
    bool block;            /* sync call with a body: activation until its E_ACT_END */
    int32_t from, to, ref; /* E_ACT_END/E_END: index of the opening event */
    Str label, num, comment, cond;
    double y, y1, x0, x1;  /* layout: arrow/header y, activation/fragment end, fragment x extent */
    int depth_from, depth_to;
} Ev;

enum { BK_CALL, BK_CTRL, BK_NONE };
typedef struct Blk { uint8_t kind; bool returned; int32_t callee, caller, ev; Str ret, num; int cnt; } Blk;

/* Consecutive `//` lines form one multi-line comment; `**bold**` markers are dropped. */
static Str cm_join(Arena *a, Str prev, Str cm, bool *oom) {
    size_t n = prev.n + (prev.n ? 1 : 0) + cm.n;
    char *p = ANEW(a, char, n + 1);
    if (!p) { *oom = true; return prev; }
    size_t k = 0;
    for (size_t i = 0; i < prev.n; i++) p[k++] = prev.p[i];
    if (prev.n) p[k++] = '\n';
    for (size_t i = 0; i < cm.n; i++) {
        if (cm.p[i] == '*' && i + 1 < cm.n && cm.p[i + 1] == '*') { i++; continue; }
        p[k++] = cm.p[i];
    }
    return (Str){.p = p, .n = k};
}
static double cm_width(Str c) {
    if (!c.n) return 0;
    double w;
    unsigned nl;
    label_measure(c, 10, &w, &nl);
    return w;
}
static double cm_extra(Str c) {
    if (!c.n) return 0;
    double w;
    unsigned nl;
    label_measure(c, 10, &w, &nl);
    return nl > 1 ? (nl - 1) * 15.0 : 0;
}

typedef struct ZT {
    Arena *a;
    VEC(Part) P;
    VEC(Ev) E;
    Blk st[64];
    int sp;
    size_t depth_skip;
    bool pending_end, oom;
    Blk last_ctrl;     /* fragment level just closed (else/catch continue its numbering) */
    int32_t starter;   /* participant index of the starter */
    int top_cnt;
    Str title, comment;
} ZT;

static bool idch(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || (unsigned char)c >= 0x80; }

static Str unq(Str s) {
    s = str_trim(s);
    if (s.n >= 2 && (s.p[0] == '"' || s.p[0] == '\'') && s.p[s.n - 1] == s.p[0]) s.p++, s.n -= 2;
    return s;
}

static int32_t part(ZT *z, Str name, uint8_t icon) {
    name = unq(name);
    for (size_t i = 1; i < z->P.n; i++)
        if (str_eq(z->P.v[i].name, name)) {
            if (icon) z->P.v[i].icon = icon;
            return (int32_t)i;
        }
    if (z->P.n > 4096 || !VEC_PUSH(z->a, &z->P, (Part){.name = name, .label = name, .icon = icon, .used = true, .create_ev = -1})) {
        z->oom = true;
        return 0;
    }
    return (int32_t)z->P.n - 1;
}

static int32_t starter(ZT *z) {
    if (z->starter < 0) { z->starter = 0; z->P.v[0].used = true; }
    return z->starter;
}

static Blk *numblk(ZT *z) {
    for (int i = z->sp; i-- > 0;)
        if (z->st[i].kind != BK_NONE) return &z->st[i];
    return nullptr;
}

static Str mknum(ZT *z, Str prefix, int k) {
    char t[24];
    int m = snprintf(t, sizeof t, "%d", k);
    Str s = {.n = prefix.n + (prefix.n ? 1 : 0) + (size_t)m};
    char *p = ANEW(z->a, char, s.n + 1);
    if (!p) { z->oom = true; return (Str){}; }
    if (prefix.n) { memcpy(p, prefix.p, prefix.n); p[prefix.n] = '.'; }
    memcpy(p + s.n - (size_t)m, t, (size_t)m);
    s.p = p;
    return s;
}
static Str next_num(ZT *z) {
    Blk *b = numblk(z);
    if (!b) return mknum(z, (Str){}, ++z->top_cnt);
    return mknum(z, b->num, ++b->cnt);
}

static int32_t ev(ZT *z, Ev e) {
    if (z->E.n > 100000 || !VEC_PUSH(z->a, &z->E, e)) { z->oom = true; return -1; }
    return (int32_t)z->E.n - 1;
}

static int32_t msg(ZT *z, uint8_t mk, int32_t from, int32_t to, Str label, Str num, bool block) {
    Ev e = {.k = E_MSG, .mk = mk, .from = from, .to = to, .label = label, .num = num, .comment = z->comment, .block = block};
    z->comment = (Str){};
    return ev(z, e);
}

static int32_t cur_callee(ZT *z) {
    for (int i = z->sp; i-- > 0;)
        if (z->st[i].kind == BK_CALL) return z->st[i].callee;
    return starter(z);
}

static void flush_end(ZT *z) {
    if (!z->pending_end) return;
    z->pending_end = false;
    ev(z, (Ev){.k = E_END, .ref = z->last_ctrl.ev});
}

static void push(ZT *z, Blk b) {
    if (z->sp < (int)countof(z->st)) z->st[z->sp++] = b;
    else z->depth_skip++;
}

static void close_block(ZT *z) {
    flush_end(z);
    if (z->depth_skip) { z->depth_skip--; return; }
    if (!z->sp) return;
    Blk *b = &z->st[z->sp - 1];
    if (b->kind == BK_CALL) {
        if (b->ret.n) msg(z, M_RET, b->callee, b->caller, b->ret, mknum(z, b->num, ++b->cnt), false);
        ev(z, (Ev){.k = E_ACT_END, .to = b->callee, .ref = b->ev});
    } else if (b->kind == BK_CTRL) {
        z->last_ctrl = *b;
        z->pending_end = true;
    }
    z->sp--;
}

static bool kw(Str s, const char *k, Str *rest) {
    size_t n = strlen(k);
    if (s.n < n || memcmp(s.p, k, n) || (s.n > n && idch(s.p[n]))) return false;
    *rest = str_trim((Str){.p = s.p + n, .n = s.n - n});
    return true;
}
static Str paren(Str s) {
    s = str_trim(s);
    if (s.n >= 2 && s.p[0] == '(' && s.p[s.n - 1] == ')') s.p++, s.n -= 2;
    return str_trim(s);
}
static Str cat3(ZT *z, const char *a, Str m, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    char *p = ANEW(z->a, char, la + m.n + lb + 1);
    if (!p) { z->oom = true; return (Str){}; }
    memcpy(p, a, la);
    if (m.n) memcpy(p + la, m.p, m.n);
    memcpy(p + la + m.n, b, lb);
    return (Str){.p = p, .n = la + m.n + lb};
}

static void open_frag(ZT *z, const char *label, Str cond, bool block) {
    Str num = next_num(z);
    Ev e = {.k = E_FRAG, .label = {.p = label, .n = strlen(label)}, .cond = cond, .num = num, .comment = z->comment};
    z->comment = (Str){};
    int32_t i = ev(z, e);
    push(z, (Blk){.kind = BK_CTRL, .ev = i, .num = num});
    if (!block) close_block(z);
}

static void stmt(ZT *z, Str s, bool block) {
    s = str_trim(s);
    Str r;
    bool cont = kw(s, "else", &r) || kw(s, "catch", &r) || kw(s, "finally", &r);
    if (!cont) {
        if (!s.n && !block) return;
        flush_end(z);
    }
    if (!s.n) { push(z, (Blk){.kind = BK_NONE}); return; }
    if (cont) {
        if (!z->pending_end) { open_frag(z, s.p[0] == 'e' ? "Alt" : "Try", (Str){}, true); }
        else {
            z->pending_end = false;
            push(z, z->last_ctrl);
        }
        Str c, lab;
        if (s.p[0] == 'e') lab = kw(r, "if", &c) ? cat3(z, "[ ", paren(c), " ]") : S("[else]");
        else if (s.p[0] == 'c') lab = cat3(z, "catch ", paren(r), "");
        else lab = S("finally");
        Blk *b = &z->st[z->sp - 1];
        if (z->sp && b->kind == BK_CTRL) ev(z, (Ev){.k = E_SEC, .cond = lab, .ref = b->ev});
        if (!block) close_block(z);
        return;
    }
    static const struct { const char *k, *frag; bool cond; } CT[] = {
        {"if", "Alt", true}, {"while", "Loop", true}, {"for", "Loop", true}, {"forEach", "Loop", true},
        {"foreach", "Loop", true}, {"loop", "Loop", true}, {"opt", "Opt", true}, {"par", "Par", false},
        {"try", "Try", false}, {"critical", "Critical", true}, {"section", "Section", true}, {"group", "Group", true},
    };
    for (size_t i = 0; i < countof(CT); i++)
        if (kw(s, CT[i].k, &r)) {
            Str c = paren(r);
            open_frag(z, CT[i].frag, c.n ? (CT[i].cond && c.p[0] != '"' ? cat3(z, "[ ", c, " ]") : unq(c)) : (Str){}, block);
            return;
        }
    if (kw(s, "title", &r)) { z->title = r; return; }
    if (kw(s, "return", &r) || kw(s, "@return", &r)) {
        int32_t from = cur_callee(z), to = starter(z);
        for (int i = z->sp; i-- > 0;)
            if (z->st[i].kind == BK_CALL) {
                to = z->st[i].caller;
                if (i == z->sp - 1) z->st[i].returned = true;
                break;
            }
        msg(z, M_RET, from, to, r, next_num(z), false);
        return;
    }
    if (s.p[0] == '@') { /* @Actor A, @Database "B", @Starter(A) */
        Str t = {.p = s.p + 1, .n = 0};
        while (t.n + 1 < s.n && idch(s.p[1 + t.n])) t.n++;
        Str nm = str_trim((Str){.p = t.p + t.n, .n = s.n - 1 - t.n});
        if (str_eq(t, S("Starter"))) { z->starter = part(z, paren(nm), 0); z->P.v[0].used = false; return; }
        static const struct { const char *z; uint8_t ic; } TY[] = {
            {"Actor", IC_ACTOR}, {"Database", IC_DB}, {"Boundary", IC_BOUNDARY}, {"Control", IC_CONTROL},
            {"Entity", IC_ENTITY}, {"Collections", IC_COLL}, {"Queue", IC_QUEUE},
        };
        uint8_t ic = IC_NONE;
        for (size_t i = 0; i < countof(TY); i++)
            if (str_eq(t, (Str){.p = TY[i].z, .n = strlen(TY[i].z)})) ic = TY[i].ic;
        if (nm.n) part(z, nm, ic);
        return;
    }
    /* messages: [Type assign =] [From->]Target.method(args) | [From->]Target: text | new Target(args) */
    Str assign = {};
    {
        size_t i = 0;
        while (i < s.n && (idch(s.p[i]) || s.p[i] == ' ' || s.p[i] == '<' || s.p[i] == '>' || s.p[i] == ',')) i++;
        if (i < s.n && s.p[i] == '=' && (i + 1 >= s.n || s.p[i + 1] != '=')) {
            Str lhs = str_trim((Str){.p = s.p, .n = i});
            const char *sp = nullptr; /* "Type var = ..." -> var */
            for (size_t k = lhs.n; k-- > 0;) if (lhs.p[k] == ' ') { sp = lhs.p + k; break; }
            assign = sp ? str_trim((Str){.p = sp, .n = (size_t)(lhs.p + lhs.n - sp)}) : lhs;
            s = str_trim((Str){.p = s.p + i + 1, .n = s.n - i - 1});
        }
    }
    Str from = {};
    bool dashed = false;
    for (size_t i = 0; i + 1 < s.n; i++) {
        if (s.p[i] == '(' || s.p[i] == ':' || s.p[i] == '"') break;
        if (s.p[i] == '-' && s.p[i + 1] == '>') {
            from = str_trim((Str){.p = s.p, .n = i});
            if (from.n && from.p[from.n - 1] == '-') { dashed = true; from.n--; from = str_trim(from); }
            s = str_trim((Str){.p = s.p + i + 2, .n = s.n - i - 2});
            break;
        }
    }
    bool isnew = kw(s, "new", &r);
    if (isnew) s = r;
    size_t i = 0;
    if (s.n && (s.p[0] == '"' || s.p[0] == '\'')) {
        const char *q = memchr(s.p + 1, s.p[0], s.n - 1);
        i = q ? (size_t)(q - s.p) + 1 : s.n;
    } else while (i < s.n && idch(s.p[i])) i++;
    Str target = {.p = s.p, .n = i};
    Str tail = str_trim((Str){.p = s.p + i, .n = s.n - i});
    if (!target.n) return;
    if (!tail.n && !isnew && !from.n) { /* bare participant declaration */
        part(z, target, 0);
        if (block) push(z, (Blk){.kind = BK_NONE});
        return;
    }
    int32_t src0 = from.n ? part(z, from, 0) : cur_callee(z);
    if (tail.n && tail.p[0] == ':') { /* async message */
        int32_t t = part(z, target, 0);
        msg(z, dashed ? M_RET : M_ASYNC, src0, t, str_trim((Str){.p = tail.p + 1, .n = tail.n - 1}), next_num(z), false);
        if (block) push(z, (Blk){.kind = BK_NONE});
        return;
    }
    Str num = next_num(z), label = tail;
    int32_t t;
    uint8_t mk = M_SYNC;
    if (isnew) {
        t = part(z, assign.n ? cat3(z, "", assign, ":") : (Str){}, 0); /* placeholder name, fixed below */
        Part *p = &z->P.v[t];
        p->name = assign.n ? assign : unq(target);
        p->label = assign.n ? cat3(z, "", assign, "") : unq(target);
        if (assign.n) {
            char *q = ANEW(z->a, char, assign.n + 1 + target.n);
            if (!q) { z->oom = true; return; }
            memcpy(q, assign.p, assign.n); q[assign.n] = ':'; memcpy(q + assign.n + 1, target.p, target.n);
            p->label = (Str){.p = q, .n = assign.n + 1 + target.n};
        }
        p->create_ev = (int32_t)z->E.n;
        Str args = paren(tail);
        label = args.n ? cat3(z, "« ", args, " »") : S("«create»");
        mk = M_CREATE;
    } else {
        t = part(z, target, 0);
        if (label.n && label.p[0] == '.') label.p++, label.n--;
    }
    int32_t e = msg(z, mk, src0, t, label, num, block);
    if (block) push(z, (Blk){.kind = BK_CALL, .callee = t, .caller = src0, .ret = assign, .ev = e, .num = num});
    else if (assign.n) msg(z, M_RET, t, src0, assign, mknum(z, num, 1), false);
}

static constexpr char CSS[] =
    "#mermaid .zx{fill:none;stroke:#666;stroke-width:1px;}#mermaid .zti{font-size:13px;font-weight:bold;fill:#000;}"
    "#mermaid .zb{fill:#fff;stroke:#555;stroke-width:1px;}#mermaid .zt{font-size:13px;fill:#000;}"
    "#mermaid .zi{fill:none;stroke:#000;stroke-width:1px;}"
    "#mermaid .zl{stroke:#888;stroke-width:1px;stroke-dasharray:5 5;}"
    "#mermaid .za{fill:#ddd;stroke:#555;stroke-width:1px;}"
    "#mermaid .zm{fill:none;stroke:#000;stroke-width:1.5px;}#mermaid .zr{fill:none;stroke:#000;stroke-width:1.5px;stroke-dasharray:4 3;}"
    "#mermaid .zml{font-size:13px;fill:#000;}#mermaid .zn{font-size:9px;fill:#999;}#mermaid .zc{font-size:10px;fill:#999;}"
    "#mermaid .zf{fill:none;stroke:#bbb;stroke-width:1px;}#mermaid .zh{fill:#f3f3f3;stroke:#bbb;stroke-width:1px;}"
    "#mermaid .zht{font-size:13px;font-weight:bold;fill:#000;}#mermaid .zs{stroke:#e2e2e2;stroke-width:1px;}"
    "#mermaid .zfc{font-size:11px;fill:#666;}";

static constexpr double BH = 32;
static constexpr double AW = 5;
static constexpr double TB = 26;

/* 14x14 stroked fragment-header glyphs (ZenUML style), top-left at (x, y) */
static void frag_icon(Buf *o, Str label, double x, double y) {
    static const struct { const char *k, *d; } IC[] = {
        {"Alt", "M2 2v10M2 7h5l3-4h3M7 7l3 4h3"},
        {"Loop", "M11.5 4.5A5 5 0 1 0 12 9M12 1.5v3.5h-3.5"},
        {"Opt", "M12 7A5 5 0 1 1 2 7A5 5 0 1 1 12 7"},
        {"Par", "M3 2v10M7 2v10M11 2v10"},
        {"Try", "M7 1.5l5 2v4c0 3-2.5 4.5-5 5.5-2.5-1-5-2.5-5-5.5v-4z"},
        {"Critical", "M7 1.5l5.5 10.5h-11zM7 5.5v3.5M7 10.2v.6"},
        {"Section", "M2 3h10M2 7h10M2 11h6"},
    };
    for (size_t i = 0; i < countof(IC); i++) {
        if (!str_eq(label, (Str){.p = IC[i].k, .n = strlen(IC[i].k)})) continue;
        buf_lit(o, "<path class=\"zfi\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"1.3\" transform=\"translate(");
        buf_num(o, x); buf_ch(o, ','); buf_num(o, y); buf_lit(o, ")\" d=\"");
        buf_put(o, strlen(IC[i].d), IC[i].d); buf_lit(o, "\"/>");
        return;
    }
}

static void icon(Buf *o, uint8_t ic, double x, double y) { /* 14x14 glyph, top-left (x,y) */
    buf_lit(o, "<path class=\"zi\" d=\"");
    switch (ic) {
    case IC_ACTOR:
        buf_lit(o, "M"); buf_pt(o, x + 7, y + 4); buf_lit(o, "V"); buf_num(o, y + 9);
        buf_lit(o, "M"); buf_pt(o, x + 3, y + 6); buf_lit(o, "H"); buf_num(o, x + 11);
        buf_lit(o, "M"); buf_pt(o, x + 3, y + 14); buf_lit(o, "L"); buf_pt(o, x + 7, y + 9); buf_lit(o, "L"); buf_pt(o, x + 11, y + 14);
        buf_lit(o, "M"); buf_pt(o, x + 5, y + 2); buf_lit(o, "a2,2 0 1,0 4,0a2,2 0 1,0 -4,0");
        break;
    case IC_BOUNDARY:
        buf_lit(o, "M"); buf_pt(o, x, y + 2); buf_lit(o, "V"); buf_num(o, y + 12);
        buf_lit(o, "M"); buf_pt(o, x, y + 7); buf_lit(o, "H"); buf_num(o, x + 4);
        buf_lit(o, "M"); buf_pt(o, x + 4, y + 7); buf_lit(o, "a5,5 0 1,0 10,0a5,5 0 1,0 -10,0");
        break;
    case IC_CONTROL:
        buf_lit(o, "M"); buf_pt(o, x + 2, y + 8); buf_lit(o, "a5,5 0 1,0 10,0a5,5 0 1,0 -10,0");
        buf_lit(o, "M"); buf_pt(o, x + 8, y); buf_lit(o, "L"); buf_pt(o, x + 6, y + 3); buf_lit(o, "L"); buf_pt(o, x + 9, y + 5);
        break;
    case IC_ENTITY:
        buf_lit(o, "M"); buf_pt(o, x + 2, y + 6); buf_lit(o, "a5,5 0 1,0 10,0a5,5 0 1,0 -10,0");
        buf_lit(o, "M"); buf_pt(o, x + 2, y + 13); buf_lit(o, "H"); buf_num(o, x + 12);
        break;
    case IC_DB:
        buf_lit(o, "M"); buf_pt(o, x + 1, y + 3); buf_lit(o, "a6,2.5 0 1,0 12,0a6,2.5 0 1,0 -12,0V");
        buf_num(o, y + 12); buf_lit(o, "a6,2.5 0 0,0 12,0V"); buf_num(o, y + 3);
        buf_lit(o, "M"); buf_pt(o, x + 1, y + 7.5); buf_lit(o, "a6,2.5 0 0,0 12,0");
        break;
    case IC_COLL:
        buf_lit(o, "M"); buf_pt(o, x, y + 4); buf_lit(o, "h10v10h-10zM"); buf_pt(o, x + 3, y + 4); buf_lit(o, "V"); buf_num(o, y + 1);
        buf_lit(o, "h10v10h-3");
        break;
    case IC_QUEUE:
        buf_lit(o, "M"); buf_pt(o, x + 2, y + 3); buf_lit(o, "h10a2,4 0 0,1 0,8h-10a2,4 0 0,1 0,-8a2,4 0 0,1 0,8");
        break;
    default: break;
    }
    buf_lit(o, "\"/>");
}

static void arrow(Buf *o, double x1, double x2, double y, uint8_t mk) {
    { const char *cs_ = mk == M_SYNC || mk == M_ASYNC ? "<path class=\"zm\" d=\"M" : "<path class=\"zr\" d=\"M"; buf_put(o, strlen(cs_), cs_); }
    buf_pt(o, x1, y); buf_lit(o, "H"); buf_num(o, x2); buf_lit(o, "\"/>");
    double d = x2 > x1 ? -1 : 1;
    if (mk == M_SYNC || mk == M_CREATE) {
        buf_lit(o, "<path fill=\"#000\" d=\"M"); buf_pt(o, x2, y); buf_lit(o, "L"); buf_pt(o, x2 + 9 * d, y - 4);
        buf_lit(o, "L"); buf_pt(o, x2 + 9 * d, y + 4); buf_lit(o, "Z\"/>");
    } else {
        buf_lit(o, "<path class=\"zm\" d=\"M"); buf_pt(o, x2 + 8 * d, y - 4); buf_lit(o, "L"); buf_pt(o, x2, y);
        buf_lit(o, "L"); buf_pt(o, x2 + 8 * d, y + 4); buf_lit(o, "\"/>");
    }
}

DIAG_FN(zenuml_render) {
    (void)fs;
    ZT z = {.a = a, .starter = -1};
    if (!VEC_PUSH(a, &z.P, (Part){.icon = IC_ACTOR, .create_ev = -1})) return CMM_ERR_NOMEM;
    size_t i = off;
    while (i < n && src[i] != '\n') i++; /* header line */
    size_t s0 = i;
    int paren_d = 0;
    char q = 0;
    for (; i < n && !z.oom; i++) {
        char c = src[i];
        if (q) { if (c == q || c == '\n') q = 0; continue; }
        if (c == '"') { q = c; continue; }
        if (c == '/' && i + 1 < n && src[i + 1] == '/') { /* comment to end of line: shown above the next message */
            Str before = {.p = src + s0, .n = i - s0};
            stmt(&z, before, false);
            if (str_trim(before).n) z.comment = (Str){}; /* a statement in between ends the comment block */
            size_t c0 = i + 2;
            while (i < n && src[i] != '\n') i++;
            Str cm = str_trim((Str){.p = src + c0, .n = i - c0});
            if (cm.n) z.comment = cm_join(z.a, z.comment, cm, &z.oom);
            s0 = i + 1;
            continue;
        }
        if (c == '(') paren_d++;
        else if (c == ')' && paren_d) paren_d--;
        if (paren_d && c != '\n') continue;
        if (c == '\n') paren_d = 0;
        if (c == '{' || c == '}' || c == '\n' || c == ';') {
            Str t = str_trim((Str){.p = src + s0, .n = i - s0});
            bool acc;
            if (c == '{' || !dcfg_is_acc(t, &acc)) stmt(&z, t, c == '{');
            if (t.n) z.comment = (Str){}; /* only the next message/fragment carries a comment */
            if (c == '}') close_block(&z);
            s0 = i + 1;
        }
    }
    if (s0 < n && !z.oom) stmt(&z, (Str){.p = src + s0, .n = n - s0}, false);
    while (!z.oom && (z.sp || z.depth_skip)) close_block(&z);
    flush_end(&z);
    if (z.oom) return CMM_ERR_NOMEM;
    if (!title.n) title = z.title;

    /* ---- columns ---- */
    size_t np = z.P.n;
    Part *P = z.P.v;
    double x = 10;
    bool first = true;
    for (size_t k = 0; k < np; k++) {
        if (!P[k].used) continue;
        P[k].w = P[k].label.n ? text_width(P[k].label, 13) + (P[k].icon ? 40 : 20) : 54;
        if (P[k].w < 54) P[k].w = 54;
        x += first ? P[k].w / 2 : P[k].w / 2 + 30;
        first = false;
        P[k].x = x;
        x += P[k].w / 2;
    }
    Ev *E = z.E.v;
    size_t ne = z.E.n;
    for (int pass = 0; pass < 2; pass++) /* widen gaps so labels fit (second pass settles chains) */
        for (size_t k = 0; k < ne; k++) {
            if (E[k].k != E_MSG) continue;
            int32_t f = E[k].from, t = E[k].to;
            double need = fmax(text_width(E[k].label, 13), cm_width(E[k].comment)) + 34;
            double lo = fmin(P[f].x, P[t].x), hi = fmax(P[f].x, P[t].x);
            int32_t hiidx = P[f].x > P[t].x ? f : t;
            if (f == t) {
                hiidx = -1;
                for (size_t j = 0; j < np; j++) if (P[j].used && P[j].x > lo && (hiidx < 0 || P[j].x < P[hiidx].x)) hiidx = (int32_t)j;
                if (hiidx < 0) continue;
                hi = P[hiidx].x;
                need += 30;
            }
            if (hi - lo >= need) continue;
            double d = need - (hi - lo), lim = P[hiidx].x;
            for (size_t j = 0; j < np; j++) if (P[j].x >= lim) P[j].x += d;
        }

    /* ---- vertical layout ---- */
    int *dep = ANEW(a, int, np);
    int32_t *fst = ANEW(a, int32_t, ne + 1);
    if (!dep || !fst) return CMM_ERR_NOMEM;
    memset(dep, 0, np * sizeof *dep);
    size_t fsp = 0;
    double top = TB + 12; /* zenuml always draws the (possibly empty) title bar */
    double cy = top + BH + 18;
    for (size_t k = 0; k < np; k++) P[k].y = top;
    for (size_t k = 0; k < ne; k++) {
        Ev *e = &E[k];
        switch (e->k) {
        case E_MSG: {
            if (e->comment.n) cy += 14 + cm_extra(e->comment);
            cy += 20;
            e->y = cy;
            if (e->mk == M_CREATE) { P[e->to].y = cy - BH / 2; cy += BH / 2 - 4; }
            e->depth_from = dep[e->from];
            if (e->mk == M_SYNC || e->mk == M_CREATE) {
                dep[e->to]++;
                e->depth_to = dep[e->to];
                if (!e->block) dep[e->to]--, e->y1 = e->y + (e->mk == M_CREATE ? BH / 2 + 16 : 16), cy += e->mk == M_CREATE ? 26 : 10;
            } else e->depth_to = dep[e->to];
            if (e->from == e->to) cy += 18;
            cy += 20;
            double l = fmin(P[e->from].x, P[e->to].x), r = fmax(P[e->from].x, P[e->to].x) + (e->from == e->to ? 50 : 0);
            for (size_t j = 0; j < fsp; j++) {
                Ev *f = &E[fst[j]];
                f->x0 = fmin(f->x0, l), f->x1 = fmax(f->x1, r);
            }
            break;
        }
        case E_ACT_END:
            if (dep[e->to] > 0) dep[e->to]--;
            E[e->ref].y1 = fmax(cy - 4, E[e->ref].y + 16);
            cy += 6;
            break;
        case E_FRAG:
            cy += 8 + (e->comment.n ? 14 + cm_extra(e->comment) : 0);
            e->y = cy;
            e->x0 = 1e300, e->x1 = -1e300;
            cy += 24 + (e->cond.n ? 20 : 0);
            fst[fsp++] = (int32_t)k;
            break;
        case E_SEC:
            e->y = cy + 4;
            cy += 30;
            break;
        case E_END: {
            cy += 8;
            if (!fsp) break;
            Ev *f = &E[fst[--fsp]];
            if (f->x0 > f->x1) { f->x0 = 0, f->x1 = 0; for (size_t j = 0; j < np; j++) if (P[j].used) { f->x0 = P[j].x; f->x1 = P[j].x; break; } }
            f->x0 -= 50, f->x1 += 30;
            f->x1 = fmax(f->x1, f->x0 + text_width(f->label, 11) + 40);
            if (f->cond.n) f->x1 = fmax(f->x1, f->x0 + text_width(f->cond, 10) + 16);
            f->y1 = cy;
            e->y = cy;
            if (fsp) { Ev *g = &E[fst[fsp - 1]]; g->x0 = fmin(g->x0, f->x0 - 4); g->x1 = fmax(g->x1, f->x1 + 4); }
            cy += 6;
            break;
        }
        default: break;
        }
    }
    double bottom = cy + 16;
    double W = 0, X0 = 1e300;
    for (size_t k = 0; k < np; k++) if (P[k].used) { W = fmax(W, P[k].x + P[k].w / 2); X0 = fmin(X0, P[k].x - P[k].w / 2); }
    for (size_t k = 0; k < ne; k++) {
        if (E[k].k == E_FRAG) { W = fmax(W, E[k].x1); X0 = fmin(X0, E[k].x0 - (E[k].num.n * 5.0 + 6)); }
        if (E[k].k == E_MSG) {
            double l = fmin(P[E[k].from].x, P[E[k].to].x);
            X0 = fmin(X0, l - (E[k].num.n * 5.0 + 6));
            if (E[k].from == E[k].to) W = fmax(W, P[E[k].to].x + 50 + text_width(E[k].label, 13));
        }
    }
    if (X0 > W) X0 = 0, W = 100;
    double FX0 = X0 - 8, FX1 = W + 10;
    if (title.n) FX1 = fmax(FX1, FX0 + text_width(title, 13) + 20);

    /* ---- draw ---- */
    Buf *o = out;
    if (!buf_reserve(o, 4096 + ne * 400 + np * 400)) return CMM_ERR_NOMEM;
    BBox bb = {FX0, 0, FX1, bottom};
    DG_HEAD(o, bb, 1, "zenuml", CSS);
    dg_rect(o, FX0, 0, FX1 - FX0, bottom, 0, "zx");
    if (title.n) dg_text(o, title, 13, FX0 + 6, TB / 2, "start", "zti");
    dg_line(o, FX0, TB, FX1, TB, "zx");
    for (size_t k = 0; k < ne; k++) { /* fragments behind everything */
        Ev *e = &E[k];
        if (e->k == E_FRAG) {
            dg_rect(o, e->x0, e->y, e->x1 - e->x0, e->y1 - e->y, 0, "zf");
            dg_rect(o, e->x0, e->y, e->x1 - e->x0, 22, 0, "zh");
            frag_icon(o, e->label, e->x0 + 6, e->y + 4);
            dg_text(o, e->label, 13, e->x0 + 24, e->y + 11, "start", "zht");
            dg_text(o, e->num, 9, e->x0 - 3, e->y + 6, "end", "zn");
            if (e->comment.n) dg_text(o, e->comment, 10, e->x0, e->y - 8 - cm_extra(e->comment) / 2, "start", "zc");
            if (e->cond.n) dg_text(o, e->cond, 11, e->x0 + 6, e->y + 33, "start", "zfc");
        } else if (e->k == E_SEC) {
            const Ev *f = &E[e->ref];
            dg_line(o, f->x0, e->y, f->x1, e->y, "zs");
            dg_text(o, e->cond, 10, f->x0 + 6, e->y + 12, "start", "zfc");
        }
    }
    for (size_t k = 0; k < np; k++) {
        if (!P[k].used) continue;
        dg_line(o, P[k].x, P[k].y + BH, P[k].x, bottom - 6, "zl");
    }
    for (size_t k = 0; k < ne; k++) { /* activations */
        Ev *e = &E[k];
        if (e->k != E_MSG || (e->mk != M_SYNC && e->mk != M_CREATE)) continue;
        double ax = P[e->to].x - AW + (e->depth_to - 1) * AW;
        dg_rect(o, ax, e->mk == M_CREATE ? e->y + BH / 2 : e->y, 2 * AW, fmax(e->y1 - e->y, 8), 0, "za");
    }
    for (size_t k = 0; k < np; k++) { /* participant boxes (created ones sit at their creation point) */
        if (!P[k].used) continue;
        double bx = P[k].x - P[k].w / 2;
        dg_rect(o, bx, P[k].y, P[k].w, BH, 2, "zb");
        if (!P[k].label.n) { icon(o, IC_ACTOR, P[k].x - 7, P[k].y + 9); continue; }
        double tw = text_width(P[k].label, 13), tx = P[k].x - tw / 2;
        if (P[k].icon) { tx += 10; icon(o, P[k].icon, tx - 20, P[k].y + 9); }
        dg_text(o, P[k].label, 13, tx, P[k].y + BH / 2, "start", "zt");
    }
    for (size_t k = 0; k < ne; k++) {
        Ev *e = &E[k];
        if (e->k != E_MSG) continue;
        double fx = P[e->from].x, tx = P[e->to].x, y = e->y;
        if (e->from == e->to) {
            double x1 = fx + (e->depth_from ? AW + (e->depth_from - 1) * AW : 0), x2 = tx + (e->depth_to ? AW + (e->depth_to - 1) * AW : 0);
            { const char *cs_ = e->mk == M_RET ? "<path class=\"zr\" d=\"M" : "<path class=\"zm\" d=\"M"; buf_put(o, strlen(cs_), cs_); }
            buf_pt(o, x1, y); buf_lit(o, "H"); buf_num(o, fx + 40); buf_lit(o, "V"); buf_num(o, y + 18);
            buf_lit(o, "H"); buf_num(o, x2 + 8); buf_lit(o, "\"/>");
            arrow(o, x2 + 9, x2, y + 18, e->mk == M_SYNC ? M_SYNC : M_ASYNC);
            dg_text(o, e->label, 13, fx + 46, y + 9, "start", "zml");
            dg_text(o, e->num, 9, x1 - 4, y, "end", "zn");
            if (e->comment.n) dg_text(o, e->comment, 10, x1 + 2, y - 12 - cm_extra(e->comment) / 2, "start", "zc");
            continue;
        }
        double dir = tx > fx ? 1 : -1;
        double x1 = e->depth_from ? fx + (e->depth_from - 1) * AW + AW * dir : fx;
        double x2 = e->depth_to ? tx + (e->depth_to - 1) * AW - AW * dir : tx;
        if (e->mk == M_CREATE) x2 = tx - dir * P[e->to].w / 2;
        arrow(o, x1, x2, y, e->mk);
        dg_text(o, e->label, 13, (x1 + x2) / 2, y - 7, "middle", "zml");
        dg_text(o, e->num, 9, fmin(x1, x2) - 4, y, "end", "zn");
        if (e->comment.n) dg_text(o, e->comment, 10, fmin(x1, x2) + 2, y - 20 - cm_extra(e->comment) / 2, "start", "zc");
    }
    buf_lit(o, "</svg>\n");
    (void)err_off; (void)msg;
    return o->oom ? CMM_ERR_NOMEM : CMM_OK;
}
