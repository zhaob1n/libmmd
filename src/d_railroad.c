/* railroad-beta / railroad-ebnf-beta / railroad-abnf-beta / railroad-peg-beta: port of mermaid's
 * railroad renderer (terminal, nonterminal, special, sequence, choice, optional, repetition with
 * the same arc geometry) over hand-written EBNF, ABNF, PEG and function-call ("railroad-beta")
 * grammar parsers. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dgraph.h"
#include "dcfg.h"
#include "diagrams.h"
#include "text.h"

typedef enum : uint8_t { RN_TERM, RN_NONTERM, RN_SPECIAL, RN_SEQ, RN_CHOICE, RN_OPT, RN_REP } RKind;
typedef struct RNode {
    RKind k;
    uint8_t min; /* repetition: 0 => bypass */
    Str s;
    struct RNode **kid;
    size_t nk;
    double w, h, up; /* measured */
} RNode;

typedef enum : uint8_t { G_EBNF, G_ABNF, G_PEG, G_FN } Gram;
typedef enum : uint8_t { T_EOF, T_ID, T_STR, T_NUM, T_PUNCT, T_SPECIAL } TKind;
typedef struct Tok { TKind k; Str s; size_t off; } Tok;

typedef struct RP {
    Arena *a;
    Gram g;
    const char *s;
    size_t n, i;
    size_t base; /* offset of s within src, for errors */
    const char *err;
    size_t eoff;
    unsigned depth;
} RP;

static bool idc(char c, bool first, Gram g) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') return true;
    if (first) return false;
    return (c >= '0' && c <= '9') || (c == '-' && g == G_ABNF);
}

static void skip_ws(RP *p) {
    for (;;) {
        while (p->i < p->n && (p->s[p->i] == ' ' || p->s[p->i] == '\t' || p->s[p->i] == '\n' || p->s[p->i] == '\r')) p->i++;
        if (p->g == G_EBNF && p->i + 1 < p->n && p->s[p->i] == '(' && p->s[p->i + 1] == '*') {
            p->i += 2;
            while (p->i + 1 < p->n && !(p->s[p->i] == '*' && p->s[p->i + 1] == ')')) p->i++;
            p->i = p->i + 2 > p->n ? p->n : p->i + 2;
            continue;
        }
        if ((p->g == G_PEG || p->g == G_FN) && p->i < p->n && p->s[p->i] == '#') {
            while (p->i < p->n && p->s[p->i] != '\n') p->i++;
            continue;
        }
        if (p->i + 1 < p->n && ((p->s[p->i] == '/' && p->s[p->i + 1] == '/') || (p->s[p->i] == '%' && p->s[p->i + 1] == '%'))) {
            while (p->i < p->n && p->s[p->i] != '\n') p->i++;
            continue;
        }
        return;
    }
}

static Tok peek(RP *p) {
    skip_ws(p);
    Tok t = {.k = T_EOF, .off = p->i};
    if (p->i >= p->n) return t;
    char c = p->s[p->i];
    size_t j = p->i;
    if (c == '"' || c == '\'') {
        j++;
        while (j < p->n && p->s[j] != c && p->s[j] != '\n') j++;
        t.k = T_STR, t.s = (Str){.p = p->s + p->i + 1, .n = j - p->i - 1};
        if (j >= p->n || p->s[j] != c) t.k = T_EOF, p->err = p->err ? p->err : "unterminated string";
        return t;
    }
    if (c == '?' && p->g == G_EBNF) { /* ? special ? only when another ? closes on the line before ; */
        size_t q = j + 1;
        while (q < p->n && p->s[q] != '?' && p->s[q] != '\n' && p->s[q] != ';') q++;
        if (q < p->n && p->s[q] == '?' && q > j + 1 && p->s[j + 1] == ' ') {
            t.k = T_SPECIAL, t.s = str_trim((Str){.p = p->s + j + 1, .n = q - j - 1});
            return t;
        }
    }
    if (c == '%' && p->g == G_ABNF) {
        j++;
        while (j < p->n && (idc(p->s[j], false, G_FN) || p->s[j] == '.' || p->s[j] == '-')) j++;
        t.k = T_STR, t.s = (Str){.p = p->s + p->i, .n = j - p->i};
        return t;
    }
    if (idc(c, true, p->g)) {
        while (j < p->n && idc(p->s[j], false, p->g)) j++;
        t.k = T_ID, t.s = (Str){.p = p->s + p->i, .n = j - p->i};
        return t;
    }
    if (c >= '0' && c <= '9') {
        while (j < p->n && p->s[j] >= '0' && p->s[j] <= '9') j++;
        t.k = T_NUM, t.s = (Str){.p = p->s + p->i, .n = j - p->i};
        return t;
    }
    static const char *const MULTI[] = {"::=", "<-", "=/"};
    for (size_t k = 0; k < countof(MULTI); k++) {
        size_t l = strlen(MULTI[k]);
        if (p->n - p->i >= l && !memcmp(p->s + p->i, MULTI[k], l)) {
            t.k = T_PUNCT, t.s = (Str){.p = p->s + p->i, .n = l};
            return t;
        }
    }
    t.k = T_PUNCT, t.s = (Str){.p = p->s + p->i, .n = 1};
    return t;
}
static void adv(RP *p, Tok t) {
    p->i = t.off + t.s.n;
    if (t.k == T_STR && t.s.p != p->s + t.off) p->i = t.off + t.s.n + 2; /* quotes */
    if (t.k == T_SPECIAL) {
        while (p->i < p->n && p->s[p->i] != '?') p->i++;
        size_t q = t.off + 1;
        while (q < p->n && p->s[q] != '?') q++;
        p->i = q + 1;
    }
}
static bool is_p(Tok t, const char *c) { return t.k == T_PUNCT && t.s.n == strlen(c) && !memcmp(t.s.p, c, t.s.n); }
static RNode *fail_at(RP *p, Tok t, const char *m) {
    if (!p->err) p->err = m, p->eoff = t.off;
    return nullptr;
}

static RNode *mk(RP *p, RKind k, Str s) {
    RNode *r = ANEW(p->a, RNode, 1);
    if (!r) { p->err = "out of memory"; return nullptr; }
    *r = (RNode){.k = k, .s = s};
    return r;
}
static bool add(RP *p, RNode *par, RNode *c) {
    RNode **nk = ANEW(p->a, RNode *, par->nk + 1);
    if (!nk) { p->err = "out of memory"; return false; }
    if (par->nk) memcpy(nk, par->kid, par->nk * sizeof *nk);
    nk[par->nk++] = c, par->kid = nk;
    return true;
}
static RNode *wrap1(RP *p, RKind k, RNode *c, uint8_t min) {
    if (!c) return nullptr;
    RNode *r = mk(p, k, (Str){});
    if (!r || !add(p, r, c)) return nullptr;
    r->min = min;
    return r;
}
/* collapse single-child seq/choice like mermaid's transforms */
static RNode *collapse(RNode *r) { return r && (r->k == RN_SEQ || r->k == RN_CHOICE) && r->nk == 1 ? r->kid[0] : r; }

static RNode *p_choice(RP *p);

/* is the next token the start of a new rule (name followed by a definition operator)? */
static bool rule_start(RP *p) {
    size_t save = p->i;
    Tok t = peek(p);
    bool r = false;
    if (t.k == T_ID) {
        adv(p, t);
        Tok o = peek(p);
        r = is_p(o, "=") || is_p(o, "::=") || is_p(o, "<-") || is_p(o, "=/");
    }
    p->i = save;
    return r;
}

static RNode *p_primary(RP *p) {
    Tok t = peek(p);
    if (++p->depth > 200) return fail_at(p, t, "expression nested too deeply");
    RNode *r = nullptr;
    if (t.k == T_STR) adv(p, t), r = mk(p, RN_TERM, t.s);
    else if (t.k == T_SPECIAL) adv(p, t), r = mk(p, RN_SPECIAL, t.s);
    else if (t.k == T_NUM && p->g == G_ABNF) adv(p, t), r = mk(p, RN_TERM, t.s);
    else if (t.k == T_ID && p->g == G_FN) {
        adv(p, t);
        Tok o = peek(p);
        if (!is_p(o, "(")) r = mk(p, RN_NONTERM, t.s);
        else {
            adv(p, o);
            static const char *const F[] = {"sequence", "choice", "optional", "oneOrMore", "zeroOrMore", "terminal", "nonterminal", "special", "skip"};
            int f = -1;
            for (int k = 0; k < (int)countof(F); k++) if (str_eq(t.s, (Str){.p = F[k], .n = strlen(F[k])})) f = k;
            if (f < 0) return fail_at(p, t, "unknown railroad function");
            if (f >= 5 && f <= 7) {
                Tok s = peek(p);
                if (s.k != T_STR && s.k != T_ID) return fail_at(p, s, "expected a string");
                adv(p, s);
                r = mk(p, f == 5 ? RN_TERM : f == 6 ? RN_NONTERM : RN_SPECIAL, s.s);
            } else {
                r = mk(p, f == 1 ? RN_CHOICE : RN_SEQ, (Str){});
                if (!r) return nullptr;
                Tok c = peek(p);
                while (!is_p(c, ")")) {
                    RNode *e = p_primary(p);
                    if (!e || !add(p, r, e)) return nullptr;
                    c = peek(p);
                    if (is_p(c, ",")) adv(p, c), c = peek(p);
                    else if (!is_p(c, ")")) return fail_at(p, c, "expected ',' or ')'");
                }
                if (f == 8) r->nk = 0;
                if (f == 2) r = wrap1(p, RN_OPT, collapse(r), 0);
                else if (f == 3 || f == 4) r = wrap1(p, RN_REP, collapse(r), f == 3);
                else if (!r->nk) return fail_at(p, c, "empty sequence/choice");
            }
            Tok c = peek(p);
            if (!is_p(c, ")")) return fail_at(p, c, "expected ')'");
            adv(p, c);
        }
    } else if (t.k == T_ID) adv(p, t), r = mk(p, RN_NONTERM, t.s);
    else if (is_p(t, ".") && p->g == G_PEG) adv(p, t), r = mk(p, RN_SPECIAL, t.s);
    else if (is_p(t, "(") || is_p(t, "[") || (is_p(t, "{") && p->g == G_EBNF)) {
        adv(p, t);
        const char *close = is_p(t, "(") ? ")" : is_p(t, "[") ? "]" : "}";
        RNode *in = p_choice(p);
        if (!in) return nullptr;
        Tok c = peek(p);
        if (!is_p(c, close)) return fail_at(p, c, "unbalanced bracket");
        adv(p, c);
        r = is_p(t, "(") ? in : is_p(t, "[") ? wrap1(p, RN_OPT, in, 0) : wrap1(p, RN_REP, in, 0);
    } else return fail_at(p, t, t.k == T_EOF ? "unexpected end of rule" : "unexpected token");
    p->depth--;
    return r;
}

/* one sequence element with its grammar's prefix/postfix operators */
static RNode *p_term(RP *p) {
    Tok t = peek(p);
    if (p->g == G_ABNF && (t.k == T_NUM || is_p(t, "*"))) { /* repeat prefix: n, n*m, *m, * */
        unsigned mn = 0, mx = 0;
        bool star = false, hasmx = false;
        if (t.k == T_NUM) {
            for (size_t k = 0; k < t.s.n && k < 6; k++) mn = mn * 10 + (unsigned)(t.s.p[k] - '0');
            adv(p, t), t = peek(p);
        }
        if (is_p(t, "*") && t.off == p->i) {
            star = true, adv(p, t);
            Tok m = peek(p);
            if (m.k == T_NUM && m.off == t.off + 1) {
                for (size_t k = 0; k < m.s.n && k < 6; k++) mx = mx * 10 + (unsigned)(m.s.p[k] - '0');
                hasmx = true, adv(p, m);
            }
        }
        if (!star) mx = mn, hasmx = true;
        RNode *in = p_primary(p);
        if (!in) return nullptr;
        if (mn == 0 && hasmx && mx == 1) return wrap1(p, RN_OPT, in, 0);
        return wrap1(p, RN_REP, in, mn ? 1 : 0);
    }
    char pre = 0;
    if (p->g == G_PEG && (is_p(t, "&") || is_p(t, "!"))) pre = t.s.p[0], adv(p, t);
    RNode *r = p_primary(p);
    for (;;) {
        if (!r) return nullptr;
        Tok o = peek(p);
        if (p->g == G_EBNF || p->g == G_PEG) {
            if (is_p(o, "?")) { adv(p, o), r = wrap1(p, RN_OPT, r, 0); continue; }
            if (is_p(o, "*")) { adv(p, o), r = wrap1(p, RN_REP, r, 0); continue; }
            if (is_p(o, "+")) { adv(p, o), r = wrap1(p, RN_REP, r, 1); continue; }
        }
        if (p->g == G_EBNF && is_p(o, "-")) {
            adv(p, o);
            RNode *ex = p_primary(p), *s = mk(p, RN_SEQ, (Str){}), *m = mk(p, RN_TERM, S("-"));
            if (!ex || !s || !m || !add(p, s, r) || !add(p, s, m) || !add(p, s, ex)) return nullptr;
            r = s;
            continue;
        }
        break;
    }
    if (pre) {
        const char *lbl = r->k == RN_TERM ? "\"" : "";
        Str in = r->k == RN_TERM || r->k == RN_NONTERM || r->k == RN_SPECIAL ? r->s : S("(...)");
        size_t ll = strlen(lbl);
        char *o = arena_alloc_array(p->a, in.n + 2 * ll + 2, 1, 1);
        if (!o) { p->err = "out of memory"; return nullptr; }
        size_t on = 0;
        o[on++] = pre;
        memcpy(o + on, lbl, ll), on += ll;
        memcpy(o + on, in.p, in.n), on += in.n;
        memcpy(o + on, lbl, ll), on += ll;
        r = mk(p, RN_SPECIAL, (Str){.p = o, .n = on});
    }
    return r;
}

static bool seq_end(RP *p, Tok t) {
    if (t.k == T_EOF || is_p(t, ";") || is_p(t, ")") || is_p(t, "]") || is_p(t, "}")) return true;
    if (p->g == G_EBNF && (is_p(t, "|") || (is_p(t, ".") ))) return true;
    if ((p->g == G_ABNF || p->g == G_PEG) && is_p(t, "/")) return true;
    if (p->g == G_FN && is_p(t, "|")) return true;
    return rule_start(p);
}

static RNode *p_seq(RP *p) {
    RNode *s = mk(p, RN_SEQ, (Str){});
    if (!s) return nullptr;
    Tok t = peek(p);
    while (!seq_end(p, t)) {
        RNode *e = p_term(p);
        if (!e || !add(p, s, e)) return nullptr;
        t = peek(p);
        if (p->g == G_EBNF && is_p(t, ",")) adv(p, t), t = peek(p);
    }
    if (!s->nk) return fail_at(p, t, "empty alternative");
    return collapse(s);
}

static RNode *p_choice(RP *p) {
    RNode *c = mk(p, RN_CHOICE, (Str){});
    if (!c) return nullptr;
    for (;;) {
        RNode *s = p_seq(p);
        if (!s || !add(p, c, s)) return nullptr;
        Tok t = peek(p);
        if ((p->g == G_EBNF || p->g == G_FN) && is_p(t, "|")) { adv(p, t); continue; }
        if ((p->g == G_ABNF || p->g == G_PEG) && is_p(t, "/")) { adv(p, t); continue; }
        break;
    }
    return collapse(c);
}

/* ---- layout + drawing (mermaid RailroadRenderer) ---- */
constexpr double PADDING = 10;
constexpr double VSEP = 8;
constexpr double HSEP = 10;
constexpr double AR = 10;
constexpr double MR = 5;
constexpr double TFS = 16; /* mermaid measures with the svg's default font */
constexpr double TH = 17;

static Str label_of(Arena *a, const RNode *r) {
    if (r->k != RN_SPECIAL) return r->s;
    char *o = arena_alloc_array(a, r->s.n + 5, 1, 1);
    if (!o) return r->s;
    memcpy(o, "? ", 2), memcpy(o + 2, r->s.p, r->s.n), memcpy(o + 2 + r->s.n, " ?", 2);
    return (Str){.p = o, .n = r->s.n + 4};
}

static void measure(Arena *a, RNode *r) {
    switch (r->k) {
    case RN_TERM: case RN_NONTERM: case RN_SPECIAL:
    {
        Str l = label_of(a, r); /* an empty label has an empty bbox */
        r->w = (l.n ? text_width(l, TFS) : 0) + 2 * PADDING, r->h = (l.n ? TH : 0) + 2 * PADDING, r->up = r->h / 2;
    }
        break;
    case RN_SEQ: {
        double w = 0, up = 0, dn = 0;
        for (size_t k = 0; k < r->nk; k++) {
            measure(a, r->kid[k]);
            w += r->kid[k]->w, up = fmax(up, r->kid[k]->up), dn = fmax(dn, r->kid[k]->h - r->kid[k]->up);
        }
        r->w = w + (r->nk ? (double)(r->nk - 1) * HSEP : 0), r->h = up + dn, r->up = up;
        break;
    }
    case RN_CHOICE: {
        double mw = 0, th = 0;
        for (size_t k = 0; k < r->nk; k++) measure(a, r->kid[k]), mw = fmax(mw, r->kid[k]->w), th += r->kid[k]->h;
        th += (double)(r->nk - 1) * VSEP;
        r->w = mw + 4 * AR, r->h = th, r->up = th / 2;
        break;
    }
    case RN_OPT:
        measure(a, r->kid[0]);
        r->w = r->kid[0]->w + 4 * AR, r->h = r->kid[0]->h + 2 * AR, r->up = 2 * AR + r->kid[0]->up;
        break;
    case RN_REP: {
        measure(a, r->kid[0]);
        bool by = r->min == 0;
        r->w = r->kid[0]->w + 4 * AR, r->h = r->kid[0]->h + 2 * AR + (by ? 2 * AR : 0);
        r->up = (by ? 2 * AR : 0) + r->kid[0]->up;
        break;
    }
    }
}

typedef struct DC { Buf *b; BBox *bb; Arena *a; } DC;

static void M(Buf *b, double x, double y) { buf_lit(b, "M "); buf_num(b, x); buf_ch(b, ' '); buf_num(b, y); buf_ch(b, ' '); }
static void L(Buf *b, double x, double y) { buf_lit(b, "L "); buf_num(b, x); buf_ch(b, ' '); buf_num(b, y); buf_ch(b, ' '); }
static void A(Buf *b, bool sweep, double x, double y) {
    buf_lit(b, "A 10 10 0 0 "); buf_ch(b, sweep ? '1' : '0'); buf_ch(b, ' '); buf_num(b, x); buf_ch(b, ' '); buf_num(b, y); buf_ch(b, ' ');
}
static void P0(Buf *b) { buf_lit(b, "<path class=\"railroad-line\" d=\""); }
static void P1(Buf *b) { if (b->n && b->p[b->n - 1] == ' ') b->n--; buf_lit(b, "\"/>"); }

/* draw r with its top-left at (ox, oy) */
static void draw(DC *c, const RNode *r, double ox, double oy) {
    Buf *b = c->b;
    switch (r->k) {
    case RN_TERM: case RN_NONTERM: case RN_SPECIAL: {
        static const char *const CL[] = {"railroad-terminal", "railroad-nonterminal", "railroad-special"};
        buf_lit(b, "<g class=\""); buf_put(b, strlen(CL[r->k]), CL[r->k]); buf_lit(b, "\"><rect x=\"");
        buf_num(b, ox); buf_lit(b, "\" y=\""); buf_num(b, oy); buf_lit(b, "\" width=\""); buf_num(b, r->w);
        buf_lit(b, "\" height=\""); buf_num(b, r->h);
        if (r->k == RN_TERM) buf_lit(b, "\" rx=\"10\" ry=\"10\"/>");
        else buf_lit(b, "\"/>");
        buf_lit(b, "<text x=\""); buf_num(b, ox + r->w / 2); buf_lit(b, "\" y=\""); buf_num(b, oy + r->h / 2);
        buf_lit(b, "\">"); buf_xml(b, label_of(c->a, r)); buf_lit(b, "</text></g>");
        bb_add(c->bb, ox, oy, ox + r->w, oy + r->h);
        break;
    }
    case RN_SEQ: {
        double x = ox;
        for (size_t k = 0; k < r->nk; k++) {
            const RNode *e = r->kid[k];
            draw(c, e, x, oy + r->up - e->up);
            if (k + 1 < r->nk) { P0(b); M(b, x + e->w, oy + r->up); L(b, x + e->w + HSEP, oy + r->up); P1(b); }
            x += e->w + HSEP;
        }
        break;
    }
    case RN_CHOICE: {
        double y = 0, cy = r->h / 2, mw = r->w - 4 * AR, tw = r->w;
        for (size_t k = 0; k < r->nk; k++) {
            const RNode *e = r->kid[k];
            double ecy = y + e->up, ex = 2 * AR + (mw - e->w) / 2;
            draw(c, e, ox + ex, oy + y);
            bool below = ecy > cy;
            double s = below ? AR : -AR;
            P0(b);
            if (fabs(ecy - cy) < 1e-9) { M(b, ox, oy + cy); L(b, ox + ex, oy + ecy); }
            else {
                M(b, ox, oy + cy); A(b, below, ox + AR, oy + cy + s); L(b, ox + AR, oy + ecy - s);
                A(b, !below, ox + 2 * AR, oy + ecy); L(b, ox + ex, oy + ecy);
            }
            P1(b);
            P0(b);
            double rs = ex + e->w;
            if (fabs(ecy - cy) < 1e-9) { M(b, ox + rs, oy + ecy); L(b, ox + tw, oy + cy); }
            else {
                M(b, ox + rs, oy + ecy); L(b, ox + tw - 2 * AR, oy + ecy); A(b, !below, ox + tw - AR, oy + ecy - s);
                L(b, ox + tw - AR, oy + cy + s); A(b, below, ox + tw, oy + cy);
            }
            P1(b);
            y += e->h + VSEP;
        }
        break;
    }
    case RN_OPT: {
        const RNode *e = r->kid[0];
        double ex = 2 * AR, ey = 2 * AR, cy = ey + e->up, tw = r->w;
        draw(c, e, ox + ex, oy + ey);
        P0(b); M(b, ox, oy + cy); L(b, ox + 2 * AR, oy + cy); P1(b);
        P0(b); M(b, ox + ex + e->w, oy + cy); L(b, ox + tw, oy + cy); P1(b);
        P0(b); M(b, ox, oy + cy); A(b, false, ox + AR, oy + cy - AR); L(b, ox + AR, oy + AR); A(b, true, ox + 2 * AR, oy);
        L(b, ox + tw - 2 * AR, oy); A(b, true, ox + tw - AR, oy + AR); L(b, ox + tw - AR, oy + cy - AR); A(b, false, ox + tw, oy + cy);
        P1(b);
        bb_add(c->bb, ox, oy, ox + tw, oy + r->h);
        break;
    }
    case RN_REP: {
        const RNode *e = r->kid[0];
        bool by = r->min == 0;
        double ex = 2 * AR, ey = by ? 2 * AR : 0, cy = ey + e->up, tw = r->w, ly = ey + e->h + AR;
        draw(c, e, ox + ex, oy + ey);
        P0(b); M(b, ox, oy + cy); L(b, ox + 2 * AR, oy + cy); P1(b);
        P0(b); M(b, ox + ex + e->w, oy + cy); L(b, ox + tw, oy + cy); P1(b);
        double rx = ex + e->w;
        P0(b); M(b, ox + rx, oy + cy); A(b, true, ox + rx + AR, oy + cy + AR); L(b, ox + rx + AR, oy + ly);
        A(b, true, ox + rx, oy + ly + AR); L(b, ox + 2 * AR, oy + ly + AR); A(b, true, ox + AR, oy + ly);
        L(b, ox + AR, oy + cy + AR); A(b, true, ox + 2 * AR, oy + cy); P1(b);
        if (by) {
            P0(b); M(b, ox, oy + cy); A(b, false, ox + AR, oy + cy - AR); L(b, ox + AR, oy + AR); A(b, true, ox + 2 * AR, oy);
            L(b, ox + tw - 2 * AR, oy); A(b, true, ox + tw - AR, oy + AR); L(b, ox + tw - AR, oy + cy - AR); A(b, false, ox + tw, oy + cy);
            P1(b);
        }
        bb_add(c->bb, ox, oy, ox + tw, oy + ly + AR + 1);
        break;
    }
    }
}

typedef struct Rule { Str name; RNode *def; } Rule;

DIAG_FN(railroad_render) {
    (void)fs, (void)title;
    LineIt it = {.s = src, .n = n, .i = off};
    Str ln;
    size_t lo;
    if (!dg_next_line(&it, &ln, &lo)) return diag_fail(err_off, msg, off, "empty railroad diagram");
    Str kw = ln, w0 = dg_word(&kw);
    Gram g = str_eq(w0, S("railroad-ebnf-beta")) ? G_EBNF : str_eq(w0, S("railroad-abnf-beta")) ? G_ABNF
           : str_eq(w0, S("railroad-peg-beta")) ? G_PEG : G_FN;
    /* body text without title/acc lines (blanked in a copy so offsets stay valid) */
    size_t bs = it.i;
    char *body = arena_alloc_array(a, n - bs + 1, 1, 1);
    if (!body) return CMM_ERR_NOMEM;
    memcpy(body, src + bs, n - bs);
    while (dg_next_line(&it, &ln, &lo)) {
        Str r = ln, w = dg_word(&r);
        bool block;
        if (str_eq(w, S("title")) || dcfg_is_acc(ln, &block) || (ln.n >= 2 && ln.p[0] == '%' && ln.p[1] == '%'))
            memset(body + (lo - bs), ' ', ln.n);
    }
    RP p = {.a = a, .g = g, .s = body, .n = n - bs, .base = bs};
    VEC(Rule) rules = {};
    for (;;) {
        Tok t = peek(&p);
        if (p.err) break;
        if (t.k == T_EOF) break;
        if (t.k != T_ID) { fail_at(&p, t, "expected a rule name"); break; }
        adv(&p, t);
        Tok o = peek(&p);
        bool okop = g == G_PEG ? is_p(o, "<-") || is_p(o, "=") : is_p(o, "=") || is_p(o, "::=") || is_p(o, "=/");
        if (!okop) { fail_at(&p, o, g == G_PEG ? "expected '<-'" : "expected '='"); break; }
        adv(&p, o);
        RNode *d = p_choice(&p);
        if (!d) break;
        Tok e = peek(&p);
        if (is_p(e, ";") || (g == G_EBNF && is_p(e, "."))) adv(&p, e);
        else if (e.k != T_EOF && !rule_start(&p)) { fail_at(&p, e, "expected ';'"); break; }
        if (!VEC_PUSH(a, &rules, ((Rule){.name = t.s, .def = d}))) return CMM_ERR_NOMEM;
    }
    if (p.err) {
        if (!strcmp(p.err, "out of memory")) return CMM_ERR_NOMEM;
        return diag_fail(err_off, msg, bs + p.eoff, p.err);
    }
    Buf b = {};
    BBox bb = BB_EMPTY;
    DC dc = {.b = &b, .bb = &bb, .a = a};
    double y = PADDING, maxw = 0;
    for (size_t k = 0; k < rules.n; k++) {
        RNode *d = rules.v[k].def;
        measure(a, d);
        char nm[512];
        int nl = snprintf(nm, sizeof nm, "%.*s =", (int)(rules.v[k].name.n > 500 ? 500 : rules.v[k].name.n), rules.v[k].name.p);
        Str rn = {.p = nm, .n = (size_t)nl};
        double nw = text_width(rn, TFS) + 20, dx = nw + 20, base = fmax(20, d->up), dy = base - d->up;
        buf_lit(&b, "<g class=\"railroad-rule\">");
        draw(&dc, d, dx, y + dy);
        buf_lit(&b, "<text class=\"railroad-rule-name\" x=\"0\" y=\""); buf_num(&b, y + base); buf_lit(&b, "\">");
        buf_xml(&b, rn); buf_lit(&b, "</text>");
        buf_lit(&b, "<circle class=\"railroad-marker\" cx=\""); buf_num(&b, nw); buf_lit(&b, "\" cy=\""); buf_num(&b, y + base); buf_lit(&b, "\" r=\"5\"/>");
        buf_lit(&b, "<circle class=\"railroad-marker\" cx=\""); buf_num(&b, dx + d->w + 10); buf_lit(&b, "\" cy=\""); buf_num(&b, y + base); buf_lit(&b, "\" r=\"5\"/>");
        P0(&b); M(&b, nw + MR, y + base); L(&b, dx, y + base); P1(&b);
        P0(&b); M(&b, dx + d->w, y + base); L(&b, dx + d->w + 10 - MR, y + base); P1(&b);
        buf_lit(&b, "</g>");
        double rh = fmax(40, dy + d->h + 2 * PADDING), rw = dx + d->w + 10 + MR;
        y += rh + VSEP, maxw = fmax(maxw, rw);
    }
    if (!rules.n) maxw = 180, y = 80;
    BBox vb = {0, 0, maxw + 2 * PADDING, y + PADDING};
    return DG_FINISH(out, &b, vb, 0, "railroad",
        "#mermaid{font-family:\"trebuchet ms\",verdana,arial,sans-serif;font-size:16px;}"
        "#mermaid .railroad-terminal rect{fill:#ffffde;stroke:hsl(60, 60%, 83.5294117647%);stroke-width:2px;}"
        "#mermaid .railroad-nonterminal rect{fill:#ECECFF;stroke:hsl(240, 60%, 86.2745098039%);stroke-width:2px;}"
        "#mermaid .railroad-special rect{fill:hsl(80, 100%, 96.2745098039%);stroke:hsl(80, 60%, 86.2745098039%);stroke-width:2px;stroke-dasharray:5,3;}"
        "#mermaid .railroad-terminal text{fill:#000021;font-family:\"trebuchet ms\",verdana,arial,sans-serif;font-size:16px;text-anchor:middle;dominant-baseline:middle;}"
        "#mermaid .railroad-nonterminal text,#mermaid .railroad-special text{fill:#131300;font-family:\"trebuchet ms\",verdana,arial,sans-serif;font-size:16px;text-anchor:middle;dominant-baseline:middle;}"
        "#mermaid .railroad-line{stroke:#333333;stroke-width:2px;fill:none;}"
        "#mermaid .railroad-marker{fill:#333333;}"
        "#mermaid .railroad-rule-name{font-weight:bold;fill:#333;font-family:\"trebuchet ms\",verdana,arial,sans-serif;font-size:16px}");
}
