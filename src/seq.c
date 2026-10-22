/* Sequence diagrams: participants/actors, messages (all arrow kinds incl. +/- activation),
 * activate/deactivate, notes, loop/alt/opt/par/critical/break/rect/box blocks, autonumber.
 * Parse -> layout -> SVG in one translation unit. */
#include <stdio.h>
#include <math.h>
#include <stdlib.h>

#include "seq.h"
#include "dcfg.h"
#include "text.h"

typedef enum : uint8_t { EV_MSG, EV_NOTE, EV_BEGIN, EV_ELSE, EV_END, EV_ACT, EV_DEACT, EV_CREATE, EV_DESTROY } EvKind;
typedef enum : uint8_t { NP_LEFT, NP_RIGHT, NP_OVER } NotePos;
typedef enum : uint8_t { HD_NONE, HD_ARROW, HD_CROSS, HD_ASYNC } Head;

/* participant @{type} shapes (mermaid 11): box, boundary, control, entity, database, collections, queue */
enum : uint8_t { PS_BOX, PS_BOUNDARY, PS_CONTROL, PS_ENTITY, PS_DATABASE, PS_COLLECTIONS, PS_QUEUE };
typedef struct Part {
    Str id, label;
    int32_t box;                     /* index into SP.boxes or -1 */
    bool actor, created, destroyed;  /* created/destroyed mid-diagram */
    uint8_t shape;                   /* PS_* from @{type} */
    bool pend_create, pend_destroy;  /* render-time state */
    double x, w, y0, y1;             /* lifeline extent (y1 == 0 => bottom) */
} Part;
typedef struct Box { Str color, label; } Box;
typedef struct Ev {
    EvKind kind;
    uint8_t sub; /* NotePos */
    bool dotted, bidir, act_to, deact_from;
    Head head;
    int32_t a, b;
    Str kw, text;
} Ev;

typedef struct SP {
    Arena *A;
    const char *s;
    size_t n, i, err;
    char *msg;
    VEC(Part) parts;
    VEC(Ev) evs;
    VEC(Box) boxes;
    int32_t cur_box;  /* open box or -1 */
    unsigned open_blk; /* open loop/alt/... blocks */
    bool autonumber, wrap;
    Str title;
} SP;

constexpr double ACT_W = 150;
constexpr double ACT_H = 65;
constexpr double ACT_MARGIN = 50;
constexpr double BOX_MARGIN = 10;

static cmm_status sfail(SP *p, size_t off, const char *m) {
    p->err = off;
    snprintf(p->msg, 160, "%s", m);
    return CMM_ERR_SYNTAX;
}

static int32_t part_get(SP *p, Str id, bool *oom) {
    for (size_t k = 0; k < p->parts.n; k++)
        if (str_eq(p->parts.v[k].id, id)) return (int32_t)k;
    if (!VEC_PUSH(p->A, &p->parts, (Part){.id = id, .label = id, .box = -1})) { *oom = true; return -1; }
    return (int32_t)p->parts.n - 1;
}

static Str word(Str *s) {
    *s = str_trim(*s);
    size_t k = 0;
    while (k < s->n && !is_sp(s->p[k])) k++;
    Str w = {.p = s->p, .n = k};
    s->p += k;
    s->n -= k;
    return w;
}

typedef struct ArrowTok { char t[8]; Head h; bool dotted, bidir; } ArrowTok;
static constexpr ArrowTok ARROWS[] = {
    {"<<-->>", HD_ARROW, true, true}, {"<<->>", HD_ARROW, false, true},
    {"-->>", HD_ARROW, true, false},  {"->>", HD_ARROW, false, false},
    {"--x", HD_CROSS, true, false},   {"-x", HD_CROSS, false, false},
    {"--)", HD_ASYNC, true, false},   {"-)", HD_ASYNC, false, false},
    {"-->", HD_NONE, true, false},    {"->", HD_NONE, false, false},
};

constexpr double WRAP_MSG = 220;
constexpr double WRAP_NOTE = 160;

/* Greedy word wrap of a normalised label to maxw px. Returns s itself when nothing to do. */
static Str wrap_text(Arena *A, Str s, double maxw, double fs, bool *ok) {
    *ok = true;
    if (text_width(s, fs) <= maxw) return s;
    char *o = arena_alloc(A, s.n + 1, 1);
    if (!o) { *ok = false; return (Str){}; }
    memcpy(o, s.p, s.n);
    size_t ls = 0, lastsp = SIZE_MAX;
    for (size_t i = 0; i < s.n; i++) {
        if (o[i] == '\n') { ls = i + 1; lastsp = SIZE_MAX; continue; }
        if (o[i] == ' ') {
            if (lastsp != SIZE_MAX && text_width((Str){.p = o + ls, .n = i - ls}, fs) > maxw) {
                o[lastsp] = '\n';
                ls = lastsp + 1;
            }
            lastsp = i;
        }
    }
    if (lastsp != SIZE_MAX && lastsp >= ls && text_width((Str){.p = o + ls, .n = s.n - ls}, fs) > maxw) o[lastsp] = '\n';
    return (Str){.p = o, .n = s.n};
}

/* strip a leading "wrap:"/"nowrap:" marker; returns effective wrap flag */
static bool wrap_prefix(Str *t, bool dflt) {
    Str x = str_trim(*t);
    if (x.n && x.p[0] == ':') x.p++, x.n--;
    if (str_starts(x, S("wrap:"))) { *t = (Str){.p = x.p + 5, .n = x.n - 5}; return true; }
    if (str_starts(x, S("nowrap:"))) { *t = (Str){.p = x.p + 7, .n = x.n - 7}; return false; }
    return dflt;
}

static bool looks_color(Str w) {
    static const Str NAMES[] = {S("transparent"), S("red"), S("green"), S("blue"), S("yellow"), S("aqua"),
        S("purple"), S("orange"), S("pink"), S("grey"), S("gray"), S("white"), S("black"), S("lightblue"),
        S("lightgreen"), S("lightyellow"), S("lightgrey"), S("lightgray"), S("teal"), S("navy"), S("olive"),
        S("lime"), S("cyan"), S("magenta"), S("silver"), S("maroon"), S("violet"), S("gold"), S("beige"),
        S("coral"), S("salmon"), S("khaki"), S("lavender"), S("wheat"), S("tan")};
    if (!w.n) return false;
    if (w.p[0] == '#' || str_starts(w, S("rgb")) || str_starts(w, S("hsl"))) return true;
    for (size_t k = 0; k < countof(NAMES); k++) if (str_ieq(w, NAMES[k])) return true;
    return false;
}

/* split "color rest" where color may be rgb(...) containing spaces */
static void split_color(Str rest, Str *color, Str *label) {
    rest = str_trim(rest);
    *color = (Str){};
    *label = rest;
    size_t k = 0;
    if (str_starts(rest, S("rgb")) || str_starts(rest, S("hsl"))) {
        const char *cp = memchr(rest.p, ')', rest.n);
        k = cp ? (size_t)(cp - rest.p) + 1 : rest.n;
    } else
        while (k < rest.n && !is_sp(rest.p[k])) k++;
    Str w = {.p = rest.p, .n = k};
    if (!looks_color(w)) return;
    *color = w;
    *label = str_trim((Str){.p = rest.p + k, .n = rest.n - k});
}

static cmm_status parse_line(SP *p, Str line, size_t off) {
    bool oom = false;
    Str rest = line;
    Str w = word(&rest);
    Ev ev = {.a = -1, .b = -1};
    bool create = false;
    if (str_eq(w, S("create"))) { w = word(&rest); create = true; }
    if (str_eq(w, S("participant")) || str_eq(w, S("actor"))) {
        rest = str_trim(rest);
        Str type = {};
        for (size_t k = 0; k + 1 < rest.n; k++) /* id@{ "type": "..." } metadata: keep id, read type */
            if (rest.p[k] == '@' && rest.p[k + 1] == '{') {
                Str meta = {.p = rest.p + k, .n = rest.n - k};
                for (size_t q = 0; q + 4 <= meta.n; q++)
                    if (memcmp(meta.p + q, "type", 4) == 0) {
                        size_t r = q + 4;
                        while (r < meta.n && (meta.p[r] == '"' || meta.p[r] == ':' || is_sp(meta.p[r]))) r++;
                        size_t r0 = r;
                        while (r < meta.n && meta.p[r] != '"' && meta.p[r] != ',' && meta.p[r] != '}' && !is_sp(meta.p[r])) r++;
                        type = (Str){.p = meta.p + r0, .n = r - r0};
                        break;
                    }
                const char *close = memchr(meta.p, '}', meta.n);
                Str tail = close ? (Str){.p = close + 1, .n = meta.n - (size_t)(close + 1 - meta.p)} : (Str){};
                tail = str_trim(tail);
                if (tail.n) { /* "A@{...} as Label": stitch id + tail via a fresh copy */
                    char *o = arena_alloc(p->A, k + 1 + tail.n, 1);
                    if (!o) return CMM_ERR_NOMEM;
                    memcpy(o, rest.p, k);
                    o[k] = ' ';
                    memcpy(o + k + 1, tail.p, tail.n);
                    rest = (Str){.p = o, .n = k + 1 + tail.n};
                } else rest = (Str){.p = rest.p, .n = k};
                break;
            }
        Str id = rest, label = rest;
        for (size_t k = 0; k + 4 <= rest.n; k++)
            if (memcmp(rest.p + k, " as ", 4) == 0) {
                id = str_trim((Str){.p = rest.p, .n = k});
                label = str_trim((Str){.p = rest.p + k + 4, .n = rest.n - k - 4});
                break;
            }
        if (!id.n) return sfail(p, off, "expected participant name");
        int32_t k = part_get(p, id, &oom);
        if (oom) return CMM_ERR_NOMEM;
        bool ok;
        p->parts.v[k].label = label_normalize(p->A, label, &ok);
        if (!ok) return CMM_ERR_NOMEM;
        Part *pp = &p->parts.v[k];
        pp->actor = str_eq(w, S("actor")) || str_eq(type, S("actor"));
        pp->shape = str_eq(type, S("boundary")) ? PS_BOUNDARY : str_eq(type, S("control")) ? PS_CONTROL
                  : str_eq(type, S("entity")) ? PS_ENTITY : str_eq(type, S("database")) ? PS_DATABASE
                  : str_eq(type, S("collections")) ? PS_COLLECTIONS : str_eq(type, S("queue")) ? PS_QUEUE : PS_BOX;
        if (p->cur_box >= 0) pp->box = p->cur_box;
        if (create) {
            pp->created = true;
            ev.kind = EV_CREATE;
            ev.a = k;
            return VEC_PUSH(p->A, &p->evs, ev) ? CMM_OK : CMM_ERR_NOMEM;
        }
        return CMM_OK;
    }
    if (str_eq(w, S("autonumber"))) { p->autonumber = true; return CMM_OK; }
    if (str_starts(w, S("title"))) {
        Str t = str_trim((Str){.p = line.p + 5, .n = line.n - 5});
        if (t.n && t.p[0] == ':') t = str_trim((Str){.p = t.p + 1, .n = t.n - 1});
        p->title = t;
        return CMM_OK;
    }
    if (str_eq(w, S("destroy"))) {
        Str id = str_trim(rest);
        if (!id.n) return sfail(p, off, "expected participant after 'destroy'");
        ev.kind = EV_DESTROY;
        ev.a = part_get(p, id, &oom);
        if (oom) return CMM_ERR_NOMEM;
        p->parts.v[ev.a].destroyed = true;
        return VEC_PUSH(p->A, &p->evs, ev) ? CMM_OK : CMM_ERR_NOMEM;
    }
    if (str_eq(w, S("box"))) {
        if (p->cur_box >= 0 || p->open_blk) return sfail(p, off, "'box' cannot be nested");
        Box bx;
        split_color(rest, &bx.color, &bx.label);
        if (!VEC_PUSH(p->A, &p->boxes, bx)) return CMM_ERR_NOMEM;
        p->cur_box = (int32_t)p->boxes.n - 1;
        return CMM_OK;
    }
    if (str_starts(w, S("accTitle")) || str_starts(w, S("accDescr")) ||
        str_eq(w, S("links")) || str_eq(w, S("link")) || str_eq(w, S("properties")) || str_eq(w, S("details")))
        return CMM_OK;
    if (str_eq(w, S("activate")) || str_eq(w, S("deactivate"))) {
        Str id = str_trim(rest);
        ev.kind = w.n == 8 ? EV_ACT : EV_DEACT;
        ev.a = part_get(p, id, &oom);
        if (oom) return CMM_ERR_NOMEM;
        return VEC_PUSH(p->A, &p->evs, ev) ? CMM_OK : CMM_ERR_NOMEM;
    }
    if (str_ieq(w, S("note"))) {
        Str pw = word(&rest);
        if (str_eq(pw, S("left")) || str_eq(pw, S("right"))) {
            ev.sub = pw.n == 4 ? NP_LEFT : NP_RIGHT;
            Str of = word(&rest);
            if (!str_eq(of, S("of"))) return sfail(p, off, "expected 'of' in note");
        } else if (str_eq(pw, S("over"))) ev.sub = NP_OVER;
        else return sfail(p, off, "expected left of/right of/over in note");
        const char *c = memchr(rest.p, ':', rest.n);
        if (!c) return sfail(p, off, "expected ':' in note");
        Str who = str_trim((Str){.p = rest.p, .n = (size_t)(c - rest.p)});
        Str text = (Str){.p = c + 1, .n = rest.n - (size_t)(c + 1 - rest.p)};
        const char *cm = memchr(who.p, ',', who.n);
        Str a = who, b = {};
        if (cm) { a = str_trim((Str){.p = who.p, .n = (size_t)(cm - who.p)}); b = str_trim((Str){.p = cm + 1, .n = who.n - (size_t)(cm + 1 - who.p)}); }
        ev.kind = EV_NOTE;
        ev.a = part_get(p, a, &oom);
        ev.b = b.n ? part_get(p, b, &oom) : ev.a;
        if (oom) return CMM_ERR_NOMEM;
        bool ok, wr = wrap_prefix(&text, p->wrap);
        ev.text = label_normalize(p->A, text, &ok);
        if (ok && wr) ev.text = wrap_text(p->A, ev.text, WRAP_NOTE, 16, &ok);
        if (!ok) return CMM_ERR_NOMEM;
        return VEC_PUSH(p->A, &p->evs, ev) ? CMM_OK : CMM_ERR_NOMEM;
    }
    static const Str BEG[] = {S("loop"), S("alt"), S("opt"), S("par"), S("critical"), S("break"), S("rect"), S("par_over")};
    for (size_t k = 0; k < countof(BEG); k++)
        if (str_eq(w, BEG[k])) {
            ev.kind = EV_BEGIN;
            ev.kw = str_eq(w, S("par_over")) ? S("par") : w;
            p->open_blk++;
            ev.text = str_trim(rest);
            return VEC_PUSH(p->A, &p->evs, ev) ? CMM_OK : CMM_ERR_NOMEM;
        }
    if (str_eq(w, S("else")) || str_eq(w, S("and")) || str_eq(w, S("option"))) {
        ev.kind = EV_ELSE;
        ev.text = str_trim(rest);
        return VEC_PUSH(p->A, &p->evs, ev) ? CMM_OK : CMM_ERR_NOMEM;
    }
    if (str_eq(w, S("end"))) {
        if (!p->open_blk && p->cur_box >= 0) { p->cur_box = -1; return CMM_OK; }
        if (p->open_blk) p->open_blk--;
        ev.kind = EV_END;
        return VEC_PUSH(p->A, &p->evs, ev) ? CMM_OK : CMM_ERR_NOMEM;
    }
    /* message: A ARROW [+|-]B : text */
    const ArrowTok *at = nullptr;
    size_t apos = 0;
    for (size_t k = 0; k < line.n && !at; k++) {
        if (line.p[k] != '-' && line.p[k] != '<') continue;
        for (size_t t = 0; t < countof(ARROWS); t++) {
            size_t tl = strlen(ARROWS[t].t);
            if (k + tl <= line.n && memcmp(line.p + k, ARROWS[t].t, tl) == 0) { at = &ARROWS[t]; apos = k; break; }
        }
    }
    if (!at || apos == 0) return sfail(p, off, "unrecognised statement in sequence diagram");
    Str from = str_trim((Str){.p = line.p, .n = apos});
    Str r2 = {.p = line.p + apos + strlen(at->t), .n = line.n - apos - strlen(at->t)};
    r2 = str_trim(r2);
    if (r2.n && r2.p[0] == '+') { ev.act_to = true; r2.p++; r2.n--; }
    else if (r2.n && r2.p[0] == '-') { ev.deact_from = true; r2.p++; r2.n--; }
    const char *c = memchr(r2.p, ':', r2.n);
    Str to = str_trim((Str){.p = r2.p, .n = c ? (size_t)(c - r2.p) : r2.n});
    Str text = c ? (Str){.p = c + 1, .n = r2.n - (size_t)(c + 1 - r2.p)} : (Str){};
    if (!from.n || !to.n) return sfail(p, off, "message needs sender and receiver");
    ev.kind = EV_MSG;
    ev.a = part_get(p, from, &oom);
    ev.b = part_get(p, to, &oom);
    if (oom) return CMM_ERR_NOMEM;
    ev.head = at->h;
    ev.dotted = at->dotted;
    ev.bidir = at->bidir;
    bool ok, wr = wrap_prefix(&text, p->wrap);
    ev.text = label_normalize(p->A, text, &ok);
    if (ok && wr) ev.text = wrap_text(p->A, ev.text, WRAP_MSG, 16, &ok);
    if (!ok) return CMM_ERR_NOMEM;
    return VEC_PUSH(p->A, &p->evs, ev) ? CMM_OK : CMM_ERR_NOMEM;
}

static void pt(Buf *b, double x, double y) { buf_num(b, x); buf_ch(b, ','); buf_num(b, y); }
static void rect(Buf *b, double x, double y, double w, double h, const char *cls) {
    buf_lit(b, "<rect class=\""); buf_put(b, strlen(cls), cls); buf_lit(b, "\" x=\""); buf_num(b, x);
    buf_lit(b, "\" y=\""); buf_num(b, y); buf_lit(b, "\" width=\""); buf_num(b, w);
    buf_lit(b, "\" height=\""); buf_num(b, h); buf_lit(b, "\"/>");
}
static void line_(Buf *b, double x1, double y1, double x2, double y2, const char *cls) {
    buf_lit(b, "<line class=\""); buf_put(b, strlen(cls), cls); buf_lit(b, "\" x1=\""); buf_num(b, x1);
    buf_lit(b, "\" y1=\""); buf_num(b, y1); buf_lit(b, "\" x2=\""); buf_num(b, x2);
    buf_lit(b, "\" y2=\""); buf_num(b, y2); buf_lit(b, "\"/>");
}
static void text_(Buf *b, Str s, double fs, double x, double y, const char *anchor, const char *cls) {
    size_t pos = 0;
    Str ln;
    double lh = fs * 1.5;
    while (label_next_line(s, &pos, &ln)) {
        buf_lit(b, "<text class=\""); buf_put(b, strlen(cls), cls); buf_lit(b, "\" x=\""); buf_num(b, x);
        buf_lit(b, "\" y=\""); buf_num(b, y); buf_lit(b, "\" text-anchor=\""); buf_put(b, strlen(anchor), anchor);
        buf_lit(b, "\" dominant-baseline=\"central\">");
        buf_xml(b, ln);
        buf_lit(b, "</text>");
        y += lh;
    }
}
static unsigned nlines(Str s) {
    if (!s.n) return 0;
    unsigned k = 1;
    for (size_t i = 0; i < s.n; i++) k += s.p[i] == '\n';
    return k;
}

static void sq_pt(Buf *b, double x, double y) { buf_num(b, x); buf_ch(b, ','); buf_num(b, y); }

static void actor_box(Buf *b, const Part *pa, double y, double fs) {
    if (pa->actor) {
        double x = pa->x, t = y + 4;
        buf_lit(b, "<g class=\"actor-man\"><circle cx=\""); buf_num(b, x); buf_lit(b, "\" cy=\""); buf_num(b, t + 8);
        buf_lit(b, "\" r=\"8\"/>");
        line_(b, x, t + 16, x, t + 34, "actor-line");
        line_(b, x - 14, t + 22, x + 14, t + 22, "actor-line");
        line_(b, x, t + 34, x - 12, t + 48, "actor-line");
        line_(b, x, t + 34, x + 12, t + 48, "actor-line");
        buf_lit(b, "</g>");
        text_(b, pa->label, fs, x, y + ACT_H - 6, "middle", "actor-label");
        return;
    }
    double x = pa->x, t = y + 4;
    switch (pa->shape) { /* icon on top, label underneath (like actor) */
    case PS_BOUNDARY:
        buf_lit(b, "<g class=\"actor-man\">");
        line_(b, x - 22, t + 8, x - 22, t + 40, "actor-line");
        line_(b, x - 22, t + 24, x - 10, t + 24, "actor-line");
        buf_lit(b, "<circle cx=\""); buf_num(b, x + 4); buf_lit(b, "\" cy=\""); buf_num(b, t + 24); buf_lit(b, "\" r=\"14\"/></g>");
        text_(b, pa->label, fs, x, y + ACT_H - 6, "middle", "actor-label");
        return;
    case PS_CONTROL:
    case PS_ENTITY:
        buf_lit(b, "<g class=\"actor-man\"><circle cx=\""); buf_num(b, x); buf_lit(b, "\" cy=\""); buf_num(b, t + 22);
        buf_lit(b, "\" r=\"14\"/>");
        if (pa->shape == PS_ENTITY) line_(b, x - 14, t + 38, x + 14, t + 38, "actor-line");
        else { buf_lit(b, "<path class=\"actor-line\" d=\"M"); sq_pt(b, x + 6, t + 3); buf_ch(b, 'L'); sq_pt(b, x - 1, t + 8);
               buf_ch(b, 'L'); sq_pt(b, x + 6, t + 13); buf_lit(b, "\"/>"); }
        buf_lit(b, "</g>");
        text_(b, pa->label, fs, x, y + ACT_H - 6, "middle", "actor-label");
        return;
    case PS_DATABASE: {
        double rx = 18, ry = 5, top = t + 4, h = 34;
        buf_lit(b, "<path class=\"actor\" d=\"M"); sq_pt(b, x - rx, top); buf_lit(b, "a"); buf_num(b, rx); buf_ch(b, ','); buf_num(b, ry);
        buf_lit(b, " 0 0 0 "); buf_num(b, 2 * rx); buf_lit(b, ",0a"); buf_num(b, rx); buf_ch(b, ','); buf_num(b, ry);
        buf_lit(b, " 0 0 0 "); buf_num(b, -2 * rx); buf_lit(b, ",0v"); buf_num(b, h); buf_ch(b, 'a'); buf_num(b, rx); buf_ch(b, ',');
        buf_num(b, ry); buf_lit(b, " 0 0 0 "); buf_num(b, 2 * rx); buf_lit(b, ",0v"); buf_num(b, -h); buf_lit(b, "\"/>");
        text_(b, pa->label, fs, x, y + ACT_H - 6, "middle", "actor-label");
        return;
    }
    case PS_COLLECTIONS: rect(b, pa->x - pa->w / 2 + 5, y + 5, pa->w, ACT_H, "actor"); break; /* stacked copy behind */
    default: break;
    }
    if (pa->shape == PS_QUEUE) { /* horizontal cylinder */
        double x0 = pa->x - pa->w / 2, w = pa->w, ry = ACT_H / 2, rx = 8;
        buf_lit(b, "<path class=\"actor\" d=\"M"); sq_pt(b, x0 + rx, y); buf_ch(b, 'h'); buf_num(b, w - 2 * rx);
        buf_ch(b, 'a'); buf_num(b, rx); buf_ch(b, ','); buf_num(b, ry); buf_lit(b, " 0 0 1 0,"); buf_num(b, ACT_H);
        buf_ch(b, 'h'); buf_num(b, -(w - 2 * rx)); buf_ch(b, 'a'); buf_num(b, rx); buf_ch(b, ','); buf_num(b, ry);
        buf_lit(b, " 0 0 1 0,"); buf_num(b, -ACT_H); buf_lit(b, "M"); sq_pt(b, x0 + w - rx, y); buf_ch(b, 'a'); buf_num(b, rx);
        buf_ch(b, ','); buf_num(b, ry); buf_lit(b, " 0 0 0 0,"); buf_num(b, ACT_H); buf_lit(b, "\"/>");
    } else rect(b, pa->x - pa->w / 2, y, pa->w, ACT_H, "actor");
    unsigned k = nlines(pa->label);
    text_(b, pa->label, fs, pa->x, y + ACT_H / 2 - (k - 1) * fs * 0.75, "middle", "actor-label");
}

typedef struct Act { int32_t p; double y0; unsigned depth; } Act;
typedef struct Blk { size_t ev; double y0; int32_t lo, hi; double xl, xr; /* content x extent (notes, self msgs) */ } Blk;

static constexpr char SEQ_CSS[] =
    "#mermaid{font-family:\"trebuchet ms\",verdana,arial,sans-serif;font-size:16px;}"
    "#mermaid .actor{fill:#ECECFF;stroke:#9370DB;stroke-width:1px;}"
    "#mermaid .actor-man circle,#mermaid .actor-line{fill:#ECECFF;stroke:#9370DB;stroke-width:2px;}"
    "#mermaid text{fill:#333;}"
    "#mermaid .lifeline{stroke:#999;stroke-width:0.5px;}"
    "#mermaid .msg{stroke:#333;stroke-width:1.5px;fill:none;}"
    "#mermaid .msg.dotted{stroke-dasharray:3,3;}"
    "#mermaid .note{fill:#fff5ad;stroke:#aaaa33;stroke-width:1px;}"
    "#mermaid .act{fill:#f4f4f4;stroke:#666;stroke-width:1px;}"
    "#mermaid .block{fill:none;stroke:#526e9c;stroke-width:1.5px;}"
    "#mermaid .block-sep{stroke:#526e9c;stroke-width:1.5px;stroke-dasharray:3,3;}"
    "#mermaid .block-tab{fill:#ECECFF;stroke:#526e9c;stroke-width:1.5px;}"
    "#mermaid .block-kw{font-size:14px;font-weight:bold;}"
    "#mermaid .seqnum{fill:#333;}#mermaid .seqnum-t{fill:#fff;font-size:12px;}"
    "#mermaid .title{font-size:18px;}"
    "#mermaid .box{stroke:rgba(0,0,0,0.5);stroke-width:1px;}#mermaid .destroy{stroke:#333;stroke-width:2px;fill:none;}";

static constexpr char SEQ_DEFS[] =
    "<defs><marker id=\"sq-arrow\" viewBox=\"0 0 12 12\" refX=\"11\" refY=\"6\" markerWidth=\"12\" markerHeight=\"12\" "
    "markerUnits=\"userSpaceOnUse\" orient=\"auto\"><path d=\"M0,0 L12,6 L0,12 z\" fill=\"#333\"/></marker>"
    "<marker id=\"sq-arrow-start\" viewBox=\"0 0 12 12\" refX=\"1\" refY=\"6\" markerWidth=\"12\" markerHeight=\"12\" "
    "markerUnits=\"userSpaceOnUse\" orient=\"auto\"><path d=\"M12,0 L0,6 L12,12 z\" fill=\"#333\"/></marker>"
    "<marker id=\"sq-cross\" viewBox=\"0 0 12 12\" refX=\"10\" refY=\"6\" markerWidth=\"12\" markerHeight=\"12\" "
    "markerUnits=\"userSpaceOnUse\" orient=\"auto\"><path d=\"M2,2 L10,10 M10,2 L2,10\" stroke=\"#333\" stroke-width=\"2\"/></marker>"
    "<marker id=\"sq-async\" viewBox=\"0 0 12 12\" refX=\"11\" refY=\"6\" markerWidth=\"12\" markerHeight=\"12\" "
    "markerUnits=\"userSpaceOnUse\" orient=\"auto\"><path d=\"M0,0 L12,6 L0,12\" fill=\"none\" stroke=\"#333\" stroke-width=\"1.5\"/></marker>"
    "</defs>";

cmm_status seq_render(Arena *a, size_t n, const char src[static n], size_t off, Str title, double fs,
                      Buf *out, size_t *err_off, char msg[static 160]) {
    SP p = {.A = a, .s = src, .n = n, .msg = msg, .title = title, .cur_box = -1};
    for (size_t k = 0; k + 4 <= off; k++) /* global wrap from frontmatter/directive config */
        if (memcmp(src + k, "wrap", 4) == 0) {
            size_t j = k + 4;
            while (j < off && (src[j] == '"' || src[j] == '\'' || src[j] == ':' || is_sp(src[j]))) j++;
            if (j + 4 <= off && memcmp(src + j, "true", 4) == 0) p.wrap = true;
        }
    cmm_status st = CMM_OK;
    Buf back = {}, mid = {}, front = {};
    /* skip header line */
    const char *nl = memchr(src + off, '\n', n - off);
    size_t i = nl ? (size_t)(nl - src) + 1 : n;
    while (i < n) {
        const char *e = memchr(src + i, '\n', n - i);
        size_t le = e ? (size_t)(e - src) : n;
        Str line = str_trim((Str){.p = src + i, .n = le - i});
        size_t loff = (size_t)(line.p - src);
        /* strip trailing comment-free semicolons */
        while (line.n && line.p[line.n - 1] == ';') line.n--;
        if (line.n && !str_starts(line, S("%%")) && (st = parse_line(&p, line, loff))) goto done;
        i = le + 1;
    }
    size_t P = p.parts.n;
    if (P == 0) { st = sfail(&p, off, "sequence diagram has no participants"); goto done; }
    Part *pa = p.parts.v;
    /* horizontal layout */
    for (size_t k = 0; k < P; k++) {
        double tw = 0;
        unsigned ln;
        label_measure(pa[k].label, fs, &tw, &ln);
        pa[k].w = tw + 30 > ACT_W ? tw + 30 : ACT_W;
        double gap = ACT_MARGIN + (k && pa[k].box != pa[k - 1].box ? 2 * BOX_MARGIN : 0);
        pa[k].x = k ? pa[k - 1].x + pa[k - 1].w / 2 + gap + pa[k].w / 2 : pa[k].w / 2;
    }
    for (size_t k = 0; k < p.evs.n; k++) {
        const Ev *ev = &p.evs.v[k];
        if (ev->kind != EV_MSG && ev->kind != EV_NOTE) continue;
        double tw = 0;
        unsigned ln;
        label_measure(ev->text, fs, &tw, &ln);
        int32_t lo = ev->a < ev->b ? ev->a : ev->b, hi = ev->a < ev->b ? ev->b : ev->a;
        double need = tw + 2 * BOX_MARGIN + 10;
        if (lo == hi) {
            if (ev->kind == EV_MSG || ev->sub == NP_RIGHT) { if ((size_t)hi + 1 >= P) continue; hi++; need = tw + 60; }
            else if (ev->sub == NP_LEFT) { if (lo == 0) continue; lo--; need = tw + 60; }
            else continue;
        }
        double d = pa[hi].x - pa[lo].x;
        if (d < need)
            for (size_t q = (size_t)hi; q < P; q++) pa[q].x += need - d;
    }
    double lh = fs * 1.5, title_h = p.title.n ? 40 : 0;
    bool box_lbl = false;
    for (size_t k = 0; k < p.boxes.n; k++) box_lbl |= p.boxes.v[k].label.n > 0;
    if (p.boxes.n) title_h += box_lbl ? lh + 10 : 10;
    for (size_t k = 0; k < P; k++) pa[k].y0 = title_h + ACT_H;
    double y = title_h + ACT_H + 20, minx = pa[0].x - pa[0].w / 2, maxx = pa[P - 1].x + pa[P - 1].w / 2;
    Act acts[256];
    size_t nact = 0;
    Blk blks[64];
    size_t nblk = 0;
    unsigned num = 0;
    for (size_t k = 0; k < p.evs.n; k++) {
        const Ev *ev = &p.evs.v[k];
        switch (ev->kind) {
        case EV_MSG: {
            unsigned ln = nlines(ev->text);
            double ly = y + ln * lh + 4;
            double x1 = pa[ev->a].x, x2 = pa[ev->b].x;
            for (size_t q = 0; q < nblk; q++) {
                int32_t lo = ev->a < ev->b ? ev->a : ev->b, hi = ev->a ^ ev->b ^ lo;
                if (lo < blks[q].lo) blks[q].lo = lo;
                if (hi > blks[q].hi) blks[q].hi = hi;
            }
            Part *cr = ev->a != ev->b && pa[ev->b].pend_create ? &pa[ev->b] : nullptr;
            if (cr) { /* created participant: its head box sits on the message line */
                ly += ACT_H / 2 - 10;
                cr->pend_create = false;
                actor_box(&front, cr, ly - ACT_H / 2, fs);
                cr->y0 = ly + ACT_H / 2;
                x2 += x2 > x1 ? -cr->w / 2 : cr->w / 2;
            }
            const char *mk = ev->head == HD_ARROW ? "sq-arrow" : ev->head == HD_CROSS ? "sq-cross" : ev->head == HD_ASYNC ? "sq-async" : nullptr;
            if (ev->a == ev->b) {
                text_(&front, ev->text, fs, x1 + 8, y + lh / 2, "start", "msg-text");
                buf_lit(&front, "<path class=\"msg"); if (ev->dotted) buf_lit(&front, " dotted");
                buf_lit(&front, "\" d=\"M"); pt(&front, x1, ly); buf_lit(&front, " C"); pt(&front, x1 + 60, ly - 10);
                buf_ch(&front, ' '); pt(&front, x1 + 60, ly + 40); buf_ch(&front, ' '); pt(&front, x1, ly + 30); buf_ch(&front, '"');
                ly += 30;
                if (x1 + 70 + text_width(ev->text, fs) > maxx) maxx = x1 + 70 + text_width(ev->text, fs);
                for (size_t q = 0; q < nblk; q++)
                    if (x1 + 70 > blks[q].xr) blks[q].xr = x1 + 70;
            } else {
                text_(&front, ev->text, fs, (x1 + x2) / 2, y + lh / 2, "middle", "msg-text");
                double dir = x2 > x1 ? 1 : -1;
                buf_lit(&front, "<path class=\"msg"); if (ev->dotted) buf_lit(&front, " dotted");
                buf_lit(&front, "\" d=\"M"); pt(&front, x1, ly); buf_lit(&front, " L"); pt(&front, x2 - dir * 1, ly); buf_ch(&front, '"');
            }
            if (mk) { buf_lit(&front, " marker-end=\"url(#"); buf_put(&front, strlen(mk), mk); buf_lit(&front, ")\""); }
            if (ev->bidir) buf_lit(&front, " marker-start=\"url(#sq-arrow-start)\"");
            buf_lit(&front, "/>");
            if (p.autonumber) {
                num++;
                buf_lit(&front, "<circle class=\"seqnum\" cx=\""); buf_num(&front, x1); buf_lit(&front, "\" cy=\"");
                buf_num(&front, ev->a == ev->b ? ly - 30 : ly); buf_lit(&front, "\" r=\"9\"/>");
                char nb[16];
                int len = snprintf(nb, sizeof nb, "%u", num);
                if (len > 0) text_(&front, (Str){.p = nb, .n = (size_t)len}, 12, x1, ev->a == ev->b ? ly - 30 : ly, "middle", "seqnum-t");
            }
            if (ev->act_to && nact < countof(acts)) {
                unsigned d = 0;
                for (size_t q = 0; q < nact; q++) d += acts[q].p == ev->b;
                acts[nact++] = (Act){.p = ev->b, .y0 = ly, .depth = d};
            }
            if (ev->deact_from)
                for (size_t q = nact; q-- > 0;)
                    if (acts[q].p == ev->a) {
                        double ax = pa[acts[q].p].x - 5 + acts[q].depth * 5;
                        rect(&mid, ax, acts[q].y0, 10, ly - acts[q].y0, "act");
                        acts[q] = acts[--nact];
                        break;
                    }
            for (unsigned q = 0; q < 2; q++) {
                Part *dp = &pa[q ? ev->b : ev->a];
                if (!dp->pend_destroy) continue;
                dp->pend_destroy = false;
                dp->y1 = ly;
                buf_lit(&front, "<path class=\"destroy\" d=\"M"); pt(&front, dp->x - 9, ly - 9);
                buf_lit(&front, " l18,18 m0,-18 l-18,18\"/>");
            }
            y = ly + (cr ? ACT_H / 2 : 0) + 22;
            break;
        }
        case EV_CREATE: pa[ev->a].pend_create = true; break;
        case EV_DESTROY: pa[ev->a].pend_destroy = true; break;
        case EV_NOTE: {
            double tw = 0;
            unsigned ln;
            label_measure(ev->text, fs, &tw, &ln);
            double w = tw + 20, h = ln * lh + 14, x;
            if (ev->sub == NP_LEFT) x = pa[ev->a].x - 15 - w;
            else if (ev->sub == NP_RIGHT) x = pa[ev->a].x + 15;
            else {
                double l = pa[ev->a].x < pa[ev->b].x ? pa[ev->a].x : pa[ev->b].x, r = pa[ev->a].x + pa[ev->b].x - l;
                if (r - l + 50 > w) w = r - l + 50;
                x = (l + r) / 2 - w / 2;
            }
            if (x < minx) minx = x;
            if (x + w > maxx) maxx = x + w;
            rect(&front, x, y, w, h, "note");
            for (size_t q = 0; q < nblk; q++) {
                if (x < blks[q].xl) blks[q].xl = x;
                if (x + w > blks[q].xr) blks[q].xr = x + w;
            }
            text_(&front, ev->text, fs, x + w / 2, y + 7 + lh / 2, "middle", "note-text");
            for (size_t q = 0; q < nblk; q++) {
                int32_t lo = ev->a < ev->b ? ev->a : ev->b, hi = ev->a ^ ev->b ^ lo;
                if (lo < blks[q].lo) blks[q].lo = lo;
                if (hi > blks[q].hi) blks[q].hi = hi;
            }
            y += h + 16;
            break;
        }
        case EV_BEGIN:
            if (nblk < countof(blks)) blks[nblk++] = (Blk){.ev = k, .y0 = y, .lo = INT32_MAX, .hi = -1, .xl = 1e300, .xr = -1e300};
            y += lh + 18;
            break;
        case EV_ELSE:
            if (nblk) {
                Ev *be = &p.evs.v[blks[nblk - 1].ev];
                (void)be;
                p.evs.v[k].a = (int32_t)nblk - 1; /* remember owner depth */
                p.evs.v[k].b = 0;
                /* separator drawn at END once x extent known: stash y in text-less slot */
                p.evs.v[k].kw = (Str){.p = nullptr, .n = (size_t)(y * 100)};
            }
            y += lh + 14;
            break;
        case EV_END: {
            if (!nblk) { st = sfail(&p, off, "'end' without block"); goto done; }
            Blk bl = blks[--nblk];
            const Ev *be = &p.evs.v[bl.ev];
            if (bl.hi < 0) { bl.lo = 0; bl.hi = (int32_t)P - 1; }
            double pad = 20 + 10.0 * (double)0;
            unsigned depth = (unsigned)nblk;
            pad = 60 - 8.0 * depth;
            double x0 = pa[bl.lo].x - pad, x1 = pa[bl.hi].x + pad;
            if (bl.lo == bl.hi) { x0 = pa[bl.lo].x - pa[bl.lo].w / 2 + 8.0 * depth; x1 = pa[bl.lo].x + pa[bl.lo].w / 2 + 60 - 8.0 * depth; }
            if (bl.xl - 10 < x0) x0 = bl.xl - 10;
            if (bl.xr + 10 > x1) x1 = bl.xr + 10;
            for (size_t q = 0; q < nblk; q++) { /* parents must enclose this block */
                if (x0 - 8 < blks[q].xl) blks[q].xl = x0 - 8;
                if (x1 + 8 > blks[q].xr) blks[q].xr = x1 + 8;
            }
            double y1 = y;
            if (str_eq(be->kw, S("rect")) || str_eq(be->kw, S("box"))) {
                buf_lit(&back, "<rect x=\""); buf_num(&back, x0); buf_lit(&back, "\" y=\""); buf_num(&back, bl.y0);
                buf_lit(&back, "\" width=\""); buf_num(&back, x1 - x0); buf_lit(&back, "\" height=\""); buf_num(&back, y1 - bl.y0);
                buf_lit(&back, "\" fill=\"");
                Str c = be->text.n ? be->text : S("rgba(200,200,255,0.3)");
                buf_xml(&back, c);
                buf_lit(&back, "\"/>");
            } else {
                rect(&mid, x0, bl.y0, x1 - x0, y1 - bl.y0, "block");
                double tw = text_width(be->kw, 14) * 1.3 + 26; /* bold keyword (fallback fonts are wide) + slanted corner */
                buf_lit(&mid, "<path class=\"block-tab\" d=\"M"); pt(&mid, x0, bl.y0); buf_lit(&mid, " h"); buf_num(&mid, tw);
                buf_lit(&mid, " v14 l-8,8 H"); buf_num(&mid, x0); buf_lit(&mid, " Z\"/>");
                text_(&front, be->kw, 14, x0 + 8, bl.y0 + 11, "start", "block-kw");
                if (be->text.n) {
                    buf_lit(&mid, "<text x=\""); buf_num(&mid, (x0 + x1) / 2); buf_lit(&mid, "\" y=\""); buf_num(&mid, bl.y0 + lh / 2 + 4);
                    buf_lit(&mid, "\" text-anchor=\"middle\" dominant-baseline=\"central\">[");
                    buf_xml(&mid, be->text);
                    buf_lit(&mid, "]</text>");
                }
                for (size_t q = bl.ev + 1; q < k; q++) {
                    Ev *ee = &p.evs.v[q];
                    if (ee->kind != EV_ELSE || ee->a != (int32_t)nblk) continue;
                    double sy = (double)ee->kw.n / 100;
                    buf_lit(&mid, "<line class=\"block-sep\" x1=\""); buf_num(&mid, x0); buf_lit(&mid, "\" y1=\""); buf_num(&mid, sy);
                    buf_lit(&mid, "\" x2=\""); buf_num(&mid, x1); buf_lit(&mid, "\" y2=\""); buf_num(&mid, sy); buf_lit(&mid, "\"/>");
                    if (ee->text.n) {
                        buf_lit(&mid, "<text x=\""); buf_num(&mid, (x0 + x1) / 2); buf_lit(&mid, "\" y=\""); buf_num(&mid, sy + lh / 2 + 2);
                        buf_lit(&mid, "\" text-anchor=\"middle\" dominant-baseline=\"central\">[");
                        buf_xml(&mid, ee->text);
                        buf_lit(&mid, "]</text>");
                    }
                }
            }
            if (x0 < minx) minx = x0;
            if (x1 > maxx) maxx = x1;
            for (size_t q = 0; q < nblk; q++) { /* propagate extent to parents */
                if (bl.lo < blks[q].lo) blks[q].lo = bl.lo;
                if (bl.hi > blks[q].hi) blks[q].hi = bl.hi;
            }
            y += 14;
            break;
        }
        case EV_ACT:
            if (nact < countof(acts)) {
                unsigned d = 0;
                for (size_t q = 0; q < nact; q++) d += acts[q].p == ev->a;
                acts[nact++] = (Act){.p = ev->a, .y0 = y - 10, .depth = d};
            }
            break;
        case EV_DEACT:
            for (size_t q = nact; q-- > 0;)
                if (acts[q].p == ev->a) {
                    double ax = pa[acts[q].p].x - 5 + acts[q].depth * 5;
                    rect(&mid, ax, acts[q].y0, 10, y - 10 - acts[q].y0, "act");
                    acts[q] = acts[--nact];
                    break;
                }
            break;
        }
    }
    if (nblk) { st = sfail(&p, n, "unterminated block (missing 'end')"); goto done; }
    for (size_t q = 0; q < nact; q++) rect(&mid, pa[acts[q].p].x - 5 + acts[q].depth * 5, acts[q].y0, 10, y - acts[q].y0, "act");
    double bottom = y + 10;
    for (size_t k = 0; k < P; k++) {
        if (pa[k].pend_create) continue; /* declared but never messaged: drawn nowhere */
        line_(&back, pa[k].x, pa[k].y0, pa[k].x, pa[k].y1 > 0 ? pa[k].y1 : bottom, "lifeline");
        if (!pa[k].created) actor_box(&back, &pa[k], title_h, fs);
        if (pa[k].y1 <= 0) actor_box(&front, &pa[k], bottom, fs);
    }
    Buf boxes = {};
    for (size_t bi = 0; bi < p.boxes.n; bi++) {
        double x0 = INFINITY, x1 = -INFINITY;
        for (size_t k = 0; k < P; k++)
            if (pa[k].box == (int32_t)bi) {
                if (pa[k].x - pa[k].w / 2 < x0) x0 = pa[k].x - pa[k].w / 2;
                if (pa[k].x + pa[k].w / 2 > x1) x1 = pa[k].x + pa[k].w / 2;
            }
        if (x0 > x1) continue;
        x0 -= BOX_MARGIN, x1 += BOX_MARGIN;
        if (x0 < minx) minx = x0;
        if (x1 > maxx) maxx = x1;
        const Box *bx = &p.boxes.v[bi];
        double by = title_h - (box_lbl ? lh + 10 : 10);
        buf_lit(&boxes, "<rect class=\"box\" x=\""); buf_num(&boxes, x0); buf_lit(&boxes, "\" y=\""); buf_num(&boxes, by);
        buf_lit(&boxes, "\" width=\""); buf_num(&boxes, x1 - x0); buf_lit(&boxes, "\" height=\"");
        buf_num(&boxes, bottom + ACT_H + 10 - by); buf_lit(&boxes, "\" fill=\"");
        if (bx->color.n) buf_xml(&boxes, bx->color); else buf_lit(&boxes, "none");
        buf_lit(&boxes, "\"/>");
        if (bx->label.n) text_(&boxes, bx->label, fs, (x0 + x1) / 2, by + lh / 2 + 4, "middle", "box-label");
    }
    if (back.oom || mid.oom || front.oom || boxes.oom) { free(boxes.p); st = CMM_ERR_NOMEM; goto done; }
    double m = 20, vx = minx - m, vy = -m / 2, vw = maxx - minx + 2 * m, vh = bottom + ACT_H + m;
    buf_lit(out, "<svg xmlns=\"http://www.w3.org/2000/svg\" id=\"mermaid\" class=\"sequence\" width=\"");
    buf_num(out, vw); buf_lit(out, "\" height=\""); buf_num(out, vh); buf_lit(out, "\" viewBox=\"");
    buf_num(out, vx); buf_ch(out, ' '); buf_num(out, vy); buf_ch(out, ' '); buf_num(out, vw); buf_ch(out, ' '); buf_num(out, vh);
    buf_lit(out, "\" role=\"graphics-document document\" aria-roledescription=\"sequence");
    dcfg_svg_aria(out);
    buf_lit(out, "<style>");
    buf_lit(out, SEQ_CSS);
    dcfg_theme_css(out, "sequence");
    buf_lit(out, "</style>");
    buf_lit(out, SEQ_DEFS);
    buf_lit(out, "<rect x=\""); buf_num(out, vx); buf_lit(out, "\" y=\""); buf_num(out, vy);
    buf_lit(out, "\" width=\""); buf_num(out, vw); buf_lit(out, "\" height=\""); buf_num(out, vh); buf_lit(out, "\" fill=\"");
    { Str bg = dcfg_background(); if (bg.n) buf_str(out, bg); else buf_lit(out, "white"); }
    buf_lit(out, "\"/>");
    if (p.title.n) text_(out, p.title, 18, (minx + maxx) / 2, 15, "middle", "title");
    buf_put(out, boxes.n, boxes.p ? boxes.p : "");
    free(boxes.p);
    buf_put(out, back.n, back.p ? back.p : "");
    buf_put(out, mid.n, mid.p ? mid.p : "");
    buf_put(out, front.n, front.p ? front.p : "");
    buf_lit(out, "</svg>\n");
    if (out->oom) st = CMM_ERR_NOMEM;
done:
    free(back.p);
    free(mid.p);
    free(front.p);
    *err_off = p.err;
    return st;
}
