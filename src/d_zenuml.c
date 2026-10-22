/* zenuml: translates ZenUML (participants with @Type annotations, `A->B: msg` async messages, `A.m(args)`
 * sync calls with `{ ... }` bodies (activation + implicit return), `x = A.m()`, `return x`, `new A()`,
 * if/else if/else, while/for/forEach/loop, opt, par, try/catch/finally, `// comments`, title) into
 * mermaid sequence source and renders it with the sequence renderer. Unknown statements are ignored. */
#include "base.h"
#include "dgraph.h"
#include "diagrams.h"
#include "seq.h"
#include "dcfg.h"

#include <stdlib.h>
#include <string.h>

enum { BK_CALL, BK_CTRL, BK_PAR, BK_NONE };
typedef struct Blk { uint8_t kind; bool returned; Str callee, caller, ret; } Blk;

typedef struct ZT {
    Buf o;
    Blk st[64];
    int sp;
    bool pending_end; /* a control block just closed; an else/catch/finally may continue it */
    Str starter, title;
    size_t depth_skip; /* blocks opened beyond the stack limit */
} ZT;

static bool idch(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || (unsigned char)c >= 0x80; }

/* participant name: identifier or "quoted name"; written so the sequence parser reads one token */
static void put_name(Buf *b, Str s) {
    s = str_trim(s);
    if (s.n >= 2 && (s.p[0] == '"' || s.p[0] == '\'')) s.p++, s.n -= 2;
    if (!s.n) { buf_lit(b, "_"); return; }
    for (size_t i = 0; i < s.n; i++) buf_ch(b, idch(s.p[i]) ? s.p[i] : '_');
}
/* message text: one line, ';' and '#' escaped as mermaid entities */
static void put_text(Buf *b, Str s) {
    s = str_trim(s);
    for (size_t i = 0; i < s.n; i++) {
        char c = s.p[i];
        if (c == ';') buf_lit(b, "#59;");
        else if (c == '#') buf_lit(b, "#35;");
        else if (c == '\n' || c == '\r') buf_ch(b, ' ');
        else buf_ch(b, c);
    }
}

static Str caller(const ZT *z) {
    for (int i = z->sp; i-- > 0;)
        if (z->st[i].kind == BK_CALL) return z->st[i].callee;
    return z->starter;
}

static void flush_end(ZT *z) {
    if (z->pending_end) { buf_lit(&z->o, "end\n"); z->pending_end = false; }
}

static void push(ZT *z, Blk b) {
    if (z->sp < (int)countof(z->st)) z->st[z->sp++] = b;
    else z->depth_skip++;
}

static void close_block(ZT *z) {
    flush_end(z);
    if (z->depth_skip) { z->depth_skip--; return; }
    if (!z->sp) return;
    Blk b = z->st[--z->sp];
    if (b.kind == BK_CALL && b.returned) {}
    else if (b.kind == BK_CALL) { /* implicit return closes the activation */
        put_name(&z->o, b.callee); buf_lit(&z->o, "-->>-"); put_name(&z->o, b.caller);
        buf_lit(&z->o, ": "); put_text(&z->o, b.ret); buf_ch(&z->o, '\n');
    } else if (b.kind == BK_CTRL || b.kind == BK_PAR) z->pending_end = true;
}

static bool kw(Str s, const char *k, Str *rest) {
    size_t n = strlen(k);
    if (s.n < n || memcmp(s.p, k, n) || (s.n > n && idch(s.p[n]))) return false;
    *rest = str_trim((Str){.p = s.p + n, .n = s.n - n});
    return true;
}
/* "(cond)" -> cond */
static Str paren(Str s) {
    s = str_trim(s);
    if (s.n >= 2 && s.p[0] == '(' && s.p[s.n - 1] == ')') s.p++, s.n -= 2;
    return str_trim(s);
}

static void stmt(ZT *z, Str s, bool block) {
    s = str_trim(s);
    Buf *o = &z->o;
    Str r;
    bool cont = kw(s, "else", &r) || kw(s, "catch", &r) || kw(s, "finally", &r);
    if (!cont) {
        if (!s.n && !block) return;
        flush_end(z);
    }
    if (!s.n) { push(z, (Blk){.kind = BK_NONE}); return; }
    if (cont) {
        bool was = z->pending_end;
        z->pending_end = false;
        if (!was) buf_lit(o, "alt\n"); /* stray else: open a fragment of its own */
        if (s.p[0] == 'e') buf_lit(o, "else "); else buf_lit(o, "option ");
        Str c;
        if (kw(r, "if", &c)) put_text(o, paren(c));
        else if (s.p[0] != 'e') { put_text(o, (Str){.p = s.p, .n = s.p[0] == 'c' ? 5 : 7}); if (r.n) { buf_ch(o, ' '); put_text(o, paren(r)); } }
        buf_ch(o, '\n');
        push(z, (Blk){.kind = BK_CTRL});
        if (!block) close_block(z);
        return;
    }
    static const struct { const char *k, *frag; } CT[] = {
        {"if", "alt"}, {"while", "loop"}, {"for", "loop"}, {"forEach", "loop"}, {"foreach", "loop"},
        {"loop", "loop"}, {"opt", "opt"}, {"par", "par"}, {"try", "critical"}, {"critical", "critical"},
        {"section", "rect rgb(240,240,255)"}, {"group", "rect rgb(245,245,245)"},
    };
    for (size_t i = 0; i < countof(CT); i++)
        if (kw(s, CT[i].k, &r)) {
            buf_put(o, strlen(CT[i].frag), CT[i].frag);
            Str c = paren(r);
            if (c.n && strncmp(CT[i].frag, "rect", 4)) { buf_ch(o, ' '); put_text(o, c); }
            if (!strcmp(CT[i].k, "try")) buf_lit(o, " try");
            buf_ch(o, '\n');
            if (!c.n && !strcmp(CT[i].frag, "par")) {} /* par contents are simply sequential here */
            push(z, (Blk){.kind = !strcmp(CT[i].frag, "par") ? BK_PAR : BK_CTRL});
            if (!block) close_block(z);
            return;
        }
    if (kw(s, "title", &r)) { z->title = r; return; }
    if (kw(s, "return", &r) || kw(s, "@return", &r)) {
        Str from = caller(z);
        Str to = z->starter;
        bool direct = false; /* a return directly in a call body ends that activation */
        for (int i = z->sp; i-- > 0;)
            if (z->st[i].kind == BK_CALL) {
                to = z->st[i].caller;
                direct = i == z->sp - 1 && !z->st[i].returned;
                if (direct) z->st[i].returned = true;
                break;
            }
        put_name(o, from); buf_lit(o, "-->>");
        if (direct) buf_ch(o, '-'); put_name(o, to); buf_lit(o, ": "); put_text(o, r); buf_ch(o, '\n');
        return;
    }
    if (s.p[0] == '@') { /* @Actor A, @Database "B", @Starter(A) */
        Str t = {.p = s.p + 1, .n = 0};
        while (t.n + 1 < s.n && idch(s.p[1 + t.n])) t.n++;
        Str nm = str_trim((Str){.p = t.p + t.n, .n = s.n - 1 - t.n});
        if (str_eq(t, S("Starter"))) { z->starter = paren(nm); buf_lit(o, "participant "); put_name(o, z->starter); buf_ch(o, '\n'); return; }
        static const struct { const char *z, *m; } TY[] = {
            {"Database", "database"}, {"Boundary", "boundary"}, {"Control", "control"},
            {"Entity", "entity"}, {"Collections", "collections"}, {"Queue", "queue"},
        };
        if (str_eq(t, S("Actor"))) { buf_lit(o, "actor "); put_name(o, nm); buf_ch(o, '\n'); return; }
        buf_lit(o, "participant "); put_name(o, nm);
        for (size_t i = 0; i < countof(TY); i++)
            if (str_eq(t, (Str){.p = TY[i].z, .n = strlen(TY[i].z)})) {
                buf_lit(o, "@{\"type\": \""); buf_put(o, strlen(TY[i].m), TY[i].m); buf_lit(o, "\"}");
            }
        buf_ch(o, '\n');
        return;
    }
    /* messages: [assign =] [From->]Target.method(args) | [From->]Target: text | new Target(args) */
    Str assign = {};
    {
        size_t i = 0;
        while (i < s.n && (idch(s.p[i]) || s.p[i] == ' ' || s.p[i] == '<' || s.p[i] == '>' || s.p[i] == ',')) {
            if (s.p[i] == '-' ) break;
            i++;
        }
        if (i < s.n && s.p[i] == '=' && (i + 1 >= s.n || s.p[i + 1] != '=') && (i == 0 || s.p[i - 1] != '-')) {
            Str lhs = str_trim((Str){.p = s.p, .n = i});
            const char *sp = nullptr; /* "Type var = ..." -> var */
            for (size_t k = lhs.n; k-- > 0;) if (lhs.p[k] == ' ') { sp = lhs.p + k; break; }
            assign = sp ? str_trim((Str){.p = sp, .n = (size_t)(lhs.p + lhs.n - sp)}) : lhs;
            s = str_trim((Str){.p = s.p + i + 1, .n = s.n - i - 1});
        }
    }
    Str from = {};
    for (size_t i = 0; i + 1 < s.n; i++) {
        if (s.p[i] == '(' || s.p[i] == ':' || s.p[i] == '"') break;
        if (s.p[i] == '-' && s.p[i + 1] == '>') {
            from = str_trim((Str){.p = s.p, .n = i});
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
    Str src0 = from.n ? from : caller(z);
    if (!from.n && !src0.n) { z->starter = S("Client"); src0 = z->starter; }
    if (!tail.n && !isnew && !from.n) { /* bare participant declaration */
        buf_lit(o, "participant "); put_name(o, target); buf_ch(o, '\n');
        if (block) push(z, (Blk){.kind = BK_NONE});
        return;
    }
    if (tail.n && tail.p[0] == ':') { /* async message */
        put_name(o, src0); buf_lit(o, "->>"); put_name(o, target); buf_lit(o, ": ");
        put_text(o, (Str){.p = tail.p + 1, .n = tail.n - 1}); buf_ch(o, '\n');
        if (block) push(z, (Blk){.kind = BK_NONE});
        return;
    }
    Str label = isnew ? (Str){} : tail;
    if (label.n && label.p[0] == '.') label.p++, label.n--;
    if (isnew) { buf_lit(o, "create participant "); put_name(o, target); buf_ch(o, '\n'); }
    put_name(o, src0); if (block) buf_lit(o, "->>+"); else buf_lit(o, "->>"); put_name(o, target); buf_lit(o, ": ");
    if (isnew) { buf_lit(o, "«create»"); if (tail.n > 2) { buf_ch(o, ' '); put_text(o, tail); } }
    else put_text(o, label);
    buf_ch(o, '\n');
    if (block) push(z, (Blk){.kind = BK_CALL, .callee = target, .caller = src0, .ret = assign});
    else if (assign.n) {
        put_name(o, target); buf_lit(o, "-->>"); put_name(o, src0); buf_lit(o, ": "); put_text(o, assign); buf_ch(o, '\n');
    }
}

DIAG_FN(zenuml_render) {
    ZT z = {};
    buf_lit(&z.o, "sequenceDiagram\n");
    size_t i = off;
    while (i < n && src[i] != '\n') i++; /* header line */
    size_t s0 = i;
    int paren_d = 0;
    char q = 0;
    for (; i < n; i++) {
        char c = src[i];
        if (q) { if (c == q || c == '\n') q = 0; continue; }
        if (c == '"') { q = c; continue; }
        if (c == '/' && i + 1 < n && src[i + 1] == '/') { /* comment to end of line */
            stmt(&z, (Str){.p = src + s0, .n = i - s0}, false);
            while (i < n && src[i] != '\n') i++;
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
            if (c == '}') close_block(&z);
            s0 = i + 1;
        }
    }
    if (s0 < n) stmt(&z, (Str){.p = src + s0, .n = n - s0}, false);
    while (z.sp || z.depth_skip) close_block(&z);
    flush_end(&z);
    if (z.o.oom) { free(z.o.p); return CMM_ERR_NOMEM; }
    Str txt = arena_strdup(a, (Str){.p = z.o.p ? z.o.p : "", .n = z.o.n});
    free(z.o.p);
    if (!txt.p) return CMM_ERR_NOMEM;
    size_t eo = 0;
    cmm_status st = seq_render(a, txt.n, txt.p, 0, title.n ? title : z.title, fs, out, &eo, msg);
    if (st) *err_off = off;
    return st;
}
