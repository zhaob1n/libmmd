/* eventmodeling: port of mermaid's event-modeling renderer. Frames (`tf`/`timeframe`, reset frames
 * `rf`/`resetframe`) are placed left to right with a 90px overlap into swimlanes chosen by entity type
 * (UI/Automation 0.., Command/Read Model 100.., Events 200..; `Ns.Name` opens a namespace lane), with
 * relations from explicit `->> id` sources or the previous box in another lane. Box sizes follow
 * mermaid's measurement of its HTML label string (including tags), so widths match. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dgraph.h"
#include "dcfg.h"
#include "diagrams.h"
#include "text.h"

constexpr int EM_MAXREF = 16;
constexpr double LANE_MIN = 70;
constexpr double LANE_PAD = 15;
constexpr double LANE_GAP = 10;
constexpr double BOX_PAD = 10;
constexpr double OVERLAP = 90;
constexpr double START_X = 250;
constexpr double LH = 17;
constexpr double CODE_CW = 7.8; /* 13px monospace advance */
constexpr double NAME_FS = 16;

typedef struct Lane { int idx; Str label, ns; double r, y, h, maxh; } Lane;
typedef struct Frame {
    Str name, type, ent, inl, dref;
    Str refs[EM_MAXREF];
    unsigned nref;
    bool reset;
    int lane; /* index into lanes */
    int kind;  /* 0 ui 1 processor 2 readmodel 3 command 4 event */
    double x, w, h;
    Str data;  /* resolved data text ('\n' separated, trimmed lines) */
} Frame;
typedef struct DataEnt { Str name, body; } DataEnt;

static int kind_of(Str t) {
    if (str_eq(t, S("ui"))) return 0;
    if (str_eq(t, S("pcr")) || str_eq(t, S("processor"))) return 1;
    if (str_eq(t, S("rmo")) || str_eq(t, S("readmodel"))) return 2;
    if (str_eq(t, S("cmd")) || str_eq(t, S("command"))) return 3;
    if (str_eq(t, S("evt")) || str_eq(t, S("event"))) return 4;
    return -1;
}
static int group_of(int k) { return k <= 1 ? 0 : k <= 3 ? 1 : 2; }

static const char *const FILL[] = {"white", "#edb3f6", "#d3f1a2", "#bcd6fe", "#ffb778"};
static const char *const STROKE[] = {"#dbdada", "#b88cbf", "#a3b732", "#679ac3", "#c19a0f"};
static const char *const TV_FILL[] = {"themeVariables.emUiFill", "themeVariables.emProcessorFill",
    "themeVariables.emReadModelFill", "themeVariables.emCommandFill", "themeVariables.emEventFill"};
static const char *const TV_STROKE[] = {"themeVariables.emUiStroke", "themeVariables.emProcessorStroke",
    "themeVariables.emReadModelStroke", "themeVariables.emCommandStroke", "themeVariables.emEventStroke"};

static bool css_ok(Str v) {
    if (!v.n || v.n > 40) return false;
    for (size_t i = 0; i < v.n; i++) if (strchr(";{}<>\"'\\", v.p[i])) return false;
    return true;
}
static void put_col(Buf *b, Str v, const char *def) {
    if (css_ok(v)) buf_xml(b, v); else buf_put(b, strlen(def), def);
}

/* does segment (x0,y0)-(x1,y1) pass through a box other than frames a and b? (Liang-Barsky clip) */
static bool em_blocked(const Frame *F, size_t n, const Lane *L, int a, int b, double x0, double y0, double x1, double y1) {
    for (size_t k = 0; k < n; k++) {
        if ((int)k == a || (int)k == b) continue;
        double bx0 = F[k].x - 2, bx1 = F[k].x + F[k].w + 2, by0 = L[F[k].lane].y + LANE_PAD - 2, by1 = by0 + F[k].h + 4;
        double t0 = 0, t1 = 1, d[2] = {x1 - x0, y1 - y0}, o[2] = {x0, y0}, lo[2] = {bx0, by0}, hi[2] = {bx1, by1};
        bool hit = true;
        for (unsigned ax = 0; ax < 2 && hit; ax++) {
            if (fabs(d[ax]) < 1e-12) { hit = o[ax] > lo[ax] && o[ax] < hi[ax]; continue; }
            double u = (lo[ax] - o[ax]) / d[ax], v = (hi[ax] - o[ax]) / d[ax];
            if (u > v) { double w = u; u = v; v = w; }
            t0 = fmax(t0, u), t1 = fmin(t1, v);
            hit = t0 < t1;
        }
        if (hit) return true;
    }
    return false;
}

/* code words: data lines; html collapses newlines to spaces but spaces are &nbsp;, so lines only
 * break between data lines */
static unsigned code_lines(Str d, double maxw, double *wout) {
    unsigned nl = d.n ? 1 : 0;
    double cur = 0, mw = 0;
    size_t i = 0;
    while (i < d.n) {
        size_t j = i;
        while (j < d.n && d.p[j] != '\n') j++;
        double w = (double)(j - i) * CODE_CW;
        if (cur > 0 && cur + CODE_CW + w > maxw) nl++, cur = w;
        else cur += (cur > 0 ? CODE_CW : 0) + w;
        mw = fmax(mw, cur);
        i = j + 1;
    }
    if (wout) *wout = mw;
    return nl;
}

DIAG_FN(eventmodeling_render) {
    (void)fs, (void)title;
    LineIt it = {.s = src, .n = n, .i = off};
    Str ln;
    size_t lo;
    if (!dg_next_line(&it, &ln, &lo)) return diag_fail(err_off, msg, off, "empty eventmodeling");
    VEC(Frame) fr = {};
    VEC(DataEnt) de = {};
    while (dg_next_line(&it, &ln, &lo)) {
        bool block;
        if (dcfg_is_acc(ln, &block)) continue;
        Str r = ln, w = dg_word(&r);
        if (str_eq(w, S("title"))) continue;
        if (str_eq(w, S("data"))) {
            Str nm = dg_word(&r);
            while (nm.n && nm.p[nm.n - 1] == '{') nm.n--;
            DataEnt d = {.name = nm};
            size_t bs = it.i;
            /* body: until a line that is just "}" */
            Str l2;
            size_t lo2, be = bs;
            bool closed = false;
            while (dg_next_line(&it, &l2, &lo2)) {
                if (l2.n && l2.p[0] == '}') { be = lo2; closed = true; break; }
                be = lo2 + l2.n;
            }
            if (!closed) return diag_fail(err_off, msg, lo, "unterminated data block");
            d.body = (Str){.p = src + bs, .n = be > bs ? be - bs : 0};
            if (!VEC_PUSH(a, &de, d)) return CMM_ERR_NOMEM;
            continue;
        }
        bool reset = str_eq(w, S("rf")) || str_eq(w, S("resetframe"));
        if (!reset && !str_eq(w, S("tf")) && !str_eq(w, S("timeframe")))
            return diag_fail(err_off, msg, lo, "expected tf/timeframe, rf/resetframe or data");
        Frame f = {.reset = reset};
        f.name = dg_word(&r), f.type = dg_word(&r), f.ent = dg_word(&r);
        f.kind = kind_of(f.type);
        if (!f.name.n || !f.ent.n || f.kind < 0) return diag_fail(err_off, msg, lo, "expected: tf <id> <ui|cmd|evt|rmo|pcr> <Name>");
        for (size_t k = 0; k < fr.n; k++)
            if (str_eq(fr.v[k].name, f.name)) return diag_fail(err_off, msg, lo, "duplicate event modeling frame ID");
        r = str_trim(r);
        while (r.n >= 3 && r.p[0] == '-' && r.p[1] == '>' && r.p[2] == '>') {
            r = (Str){.p = r.p + 3, .n = r.n - 3};
            Str id = dg_word(&r);
            if (!id.n) return diag_fail(err_off, msg, lo, "expected frame id after ->>");
            if (f.nref < EM_MAXREF) f.refs[f.nref++] = id;
            r = str_trim(r);
        }
        if (r.n && r.p[0] == '{') {
            size_t e = r.n;
            while (e > 0 && r.p[e - 1] != '}') e--;
            if (!e) return diag_fail(err_off, msg, lo, "unterminated {");
            f.inl = (Str){.p = r.p + 1, .n = e - 2};
            r = str_trim((Str){.p = r.p + e, .n = r.n - e});
        }
        if (r.n >= 4 && r.p[0] == '[' && r.p[1] == '[') {
            size_t e = 2;
            while (e + 1 < r.n && !(r.p[e] == ']' && r.p[e + 1] == ']')) e++;
            if (e + 1 >= r.n) return diag_fail(err_off, msg, lo, "unterminated [[");
            f.dref = str_trim((Str){.p = r.p + 2, .n = e - 2});
            r = str_trim((Str){.p = r.p + e + 2, .n = r.n - e - 2});
        }
        if (r.n) return diag_fail(err_off, msg, lo, "unexpected text after frame");
        if (!VEC_PUSH(a, &fr, f)) return CMM_ERR_NOMEM;
    }
    if (!fr.n) return diag_fail(err_off, msg, off, "eventmodeling needs at least one frame");
    /* data text: trimmed lines, '\n' joined */
    for (size_t i = 0; i < fr.n; i++) {
        Frame *f = &fr.v[i];
        Str raw = {};
        bool have = false;
        if (f->inl.n || f->inl.p) raw = f->inl, have = true;
        if (f->dref.n)
            for (size_t k = 0; k < de.n; k++)
                if (str_eq(de.v[k].name, f->dref)) { raw = de.v[k].body, have = true; break; }
        if (!have) continue;
        char *o = arena_alloc_array(a, raw.n + 1, 1, 1);
        if (!o) return CMM_ERR_NOMEM;
        size_t on = 0, i2 = 0;
        while (i2 < raw.n) {
            size_t j = i2;
            while (j < raw.n && raw.p[j] != '\n') j++;
            Str l = str_trim((Str){.p = raw.p + i2, .n = j - i2});
            if (l.n) {
                if (on) o[on++] = '\n';
                memcpy(o + on, l.p, l.n), on += l.n;
            }
            i2 = j + 1;
        }
        f->data = (Str){.p = o, .n = on};
        if (!on) f->data.p = o; /* present but empty */
    }
    /* lanes + x positions (mermaid's evolveFramePositioned) */
    VEC(Lane) ln_ = {};
    int prev_lane = -1;
    double maxr = 0;
    for (size_t i = 0; i < fr.n; i++) {
        Frame *f = &fr.v[i];
        Str ns = {}, nm = f->ent;
        const char *dot = memchr(f->ent.p, '.', f->ent.n);
        if (dot && !memchr(dot + 1, '.', f->ent.n - (size_t)(dot + 1 - f->ent.p)))
            ns = (Str){.p = f->ent.p, .n = (size_t)(dot - f->ent.p)}, nm = (Str){.p = dot + 1, .n = f->ent.n - ns.n - 1};
        f->ent = nm;
        int grp = group_of(f->kind), idx = grp * 100;
        static const char *const LBL[] = {"UI/Automation", "Command/Read Model", "Events"};
        static const char *const PFX[] = {"UI/A: ", "C/RM: ", "Stream: "};
        Str label = {.p = LBL[grp], .n = strlen(LBL[grp])};
        int found = -1;
        if (ns.n) for (size_t k = 0; k < ln_.n; k++) if (str_eq(ln_.v[k].ns, ns)) { found = (int)k; break; }
        if (found >= 0) idx = ln_.v[found].idx;
        else if (ns.n) {
            int m = idx;
            for (size_t k = 0; k < ln_.n; k++) if (ln_.v[k].idx > idx && ln_.v[k].idx < idx + 100 && ln_.v[k].idx > m) m = ln_.v[k].idx;
            idx = m + 1;
            size_t pl = strlen(PFX[grp]);
            char *lb = arena_alloc_array(a, pl + ns.n + 1, 1, 1);
            if (!lb) return CMM_ERR_NOMEM;
            memcpy(lb, PFX[grp], pl), memcpy(lb + pl, ns.p, ns.n);
            label = (Str){.p = lb, .n = pl + ns.n};
        }
        int li = -1;
        for (size_t k = 0; k < ln_.n; k++) if (ln_.v[k].idx == idx) { li = (int)k; break; }
        if (li < 0) {
            if (!VEC_PUSH(a, &ln_, ((Lane){.idx = idx, .label = label, .ns = ns, .maxh = LANE_MIN, .h = LANE_MIN}))) return CMM_ERR_NOMEM;
            li = (int)ln_.n - 1;
        }
        f->lane = li;
        /* text size: mermaid measures the literal HTML label */
        double nw = text_width(f->ent, NAME_FS) * 1.08 + text_width(S("<b></b>"), NAME_FS);
        unsigned nlines = 1;
        double tw = nw;
        if (f->data.p) {
            /* "<code style=...>" + data with &nbsp; for spaces, line breaks only at <br/> */
            size_t sp = 0;
            for (size_t k = 0; k < f->data.n; k++) sp += f->data.p[k] == ' ';
            double cw = text_width(S("<code style=\"text-align: left; display: block;max-width:430px\">"), NAME_FS)
                        + text_width(f->data, NAME_FS) * 1.08 + (double)sp * text_width(S("&nbsp;"), NAME_FS);
            tw = fmax(nw, cw * 1.111) / 3; /* + newline/indent &nbsp;s mermaid keeps */
            nlines = f->dref.n ? 4 : 3;
        }
        double bw = fmax(80, fmin(450, tw + 20)) + 2 * BOX_PAD;
        double bh = fmax(80, fmin(750, nlines * LH + 20)) + 2 * BOX_PAD;
        Lane *L = &ln_.v[li];
        double x;
        if (prev_lane < 0) x = START_X;
        else if (prev_lane == li && L->r > 0) x = L->r + BOX_PAD;
        else x = i ? fr.v[i - 1].x + fr.v[i - 1].w + BOX_PAD - OVERLAP + BOX_PAD : START_X;
        f->x = x, f->w = bw, f->h = bh;
        maxr = fmax(maxr, x + bw + BOX_PAD);
        for (size_t k = 0; k < ln_.n; k++) maxr = fmax(maxr, ln_.v[k].r);
        L->r = x + bw;
        L->maxh = fmax(L->maxh, bh);
        L->h = fmax(LANE_MIN, L->maxh) + 2 * LANE_PAD;
        prev_lane = li;
    }
    /* lane y: sorted by idx */
    int *ord = ANEW(a, int, ln_.n);
    if (!ord) return CMM_ERR_NOMEM;
    for (size_t k = 0; k < ln_.n; k++) ord[k] = (int)k;
    for (size_t k = 1; k < ln_.n; k++)
        for (size_t j = k; j > 0 && ln_.v[ord[j - 1]].idx > ln_.v[ord[j]].idx; j--) {
            int t = ord[j]; ord[j] = ord[j - 1], ord[j - 1] = t;
        }
    for (size_t k = 0; k < ln_.n; k++)
        ln_.v[ord[k]].y = k ? ln_.v[ord[k - 1]].y + ln_.v[ord[k - 1]].h + LANE_GAP : 0;

    Buf b = {};
    BBox bb = BB_EMPTY;
    for (size_t k = 0; k < ln_.n; k++) {
        const Lane *L = &ln_.v[ord[k]];
        buf_lit(&b, "<g class=\"em-swimlane\"><rect x=\"0\" y=\""); buf_num(&b, L->y);
        buf_lit(&b, "\" rx=\"3\" width=\""); buf_num(&b, maxr + LANE_PAD); buf_lit(&b, "\" height=\""); buf_num(&b, L->h);
        buf_lit(&b, "\" fill=\""); put_col(&b, dcfg_get("themeVariables.emSwimlaneBackgroundOdd"), "rgb(250,250,250)");
        buf_lit(&b, "\" stroke=\""); put_col(&b, dcfg_get("themeVariables.emSwimlaneBackgroundStroke"), "rgb(240,240,240)");
        buf_lit(&b, "\"/><text font-weight=\"bold\" class=\"em-lane\" x=\"30\" y=\""); buf_num(&b, L->y + 30);
        buf_lit(&b, "\">"); buf_xml(&b, L->label); buf_lit(&b, "</text></g>");
        bb_add(&bb, 0, L->y, maxr + LANE_PAD, L->y + L->h);
    }
    for (size_t i = 0; i < fr.n; i++) {
        const Frame *f = &fr.v[i];
        double y = ln_.v[f->lane].y + LANE_PAD;
        buf_lit(&b, "<g class=\"em-box\"><rect x=\""); buf_num(&b, f->x); buf_lit(&b, "\" y=\""); buf_num(&b, y);
        buf_lit(&b, "\" rx=\"3\" width=\""); buf_num(&b, f->w); buf_lit(&b, "\" height=\""); buf_num(&b, f->h);
        buf_lit(&b, "\" stroke=\""); put_col(&b, dcfg_get(TV_STROKE[f->kind]), STROKE[f->kind]);
        buf_lit(&b, "\" fill=\""); put_col(&b, dcfg_get(TV_FILL[f->kind]), FILL[f->kind]); buf_lit(&b, "\"/>");
        bb_add(&bb, f->x, y, f->x + f->w, y + f->h);
        double iw = f->w - 4 * BOX_PAD, cx = f->x + f->w / 2;
        if (!f->data.p) {
            dg_text(&b, f->ent, NAME_FS, cx, y + f->h / 2, "middle", "em-name");
        } else {
            double cwid;
            unsigned cl = code_lines(f->data, iw, &cwid);
            double th = LH * 2 + cl * 15, ty = y + f->h / 2 - th / 2;
            dg_text(&b, f->ent, NAME_FS, cx, ty + LH / 2, "middle", "em-name");
            /* flowing code lines */
            double lx = f->x + 2 * BOX_PAD, cy = ty + 2 * LH + 7.5, cur = 0;
            buf_lit(&b, "<text class=\"em-code\" x=\""); buf_num(&b, lx); buf_lit(&b, "\" y=\""); buf_num(&b, cy);
            buf_lit(&b, "\" dominant-baseline=\"middle\" xml:space=\"preserve\">");
            size_t p = 0;
            bool open = false;
            while (p < f->data.n) {
                size_t j = p;
                while (j < f->data.n && f->data.p[j] != '\n') j++;
                Str w = {.p = f->data.p + p, .n = j - p};
                double ww = (double)w.n * CODE_CW;
                if (cur > 0 && cur + CODE_CW + ww > iw) {
                    if (open) buf_lit(&b, "</tspan>");
                    cy += 15, cur = 0, open = false;
                }
                if (!open) {
                    buf_lit(&b, "<tspan x=\""); buf_num(&b, lx); buf_lit(&b, "\" y=\""); buf_num(&b, cy); buf_lit(&b, "\">");
                    open = true;
                } else buf_ch(&b, ' ');
                buf_xml(&b, w);
                cur += (cur > 0 ? CODE_CW : 0) + ww;
                p = j + 1;
            }
            if (open) buf_lit(&b, "</tspan>");
            buf_lit(&b, "</text>");
        }
        buf_lit(&b, "</g>");
    }
    /* relations */
    Str rs = dcfg_get("themeVariables.emRelationStroke");
    for (size_t i = 0; i < fr.n; i++) {
        const Frame *t = &fr.v[i];
        int srcs[EM_MAXREF];
        unsigned ns = 0;
        if (t->nref) {
            for (size_t k = 0; k < fr.n; k++)
                for (unsigned q = 0; q < t->nref; q++)
                    if (str_eq(fr.v[k].name, t->refs[q])) { if (ns < EM_MAXREF) srcs[ns++] = (int)k; break; }
        } else if (i > 0 && !t->reset) {
            for (size_t k = i; k-- > 0;)
                if (fr.v[k].lane != t->lane) { srcs[ns++] = (int)k; break; }
        }
        for (unsigned q = 0; q < ns; q++) {
            const Frame *s = &fr.v[srcs[q]];
            double sy0 = ln_.v[s->lane].y + LANE_PAD, ty0 = ln_.v[t->lane].y + LANE_PAD;
            bool up = sy0 > ty0;
            double sx = s->x + s->w * 2 / 3, tx = t->x + t->w / 3;
            double sy = up ? sy0 : sy0 + s->h, ty = up ? ty0 + t->h : ty0;
            /* deviate from mermaid: a straight relation that would cut through another box is routed
             * orthogonally through the lane gaps (source gap, a clear column, target gap) */
            double P[12] = {sx, sy, tx, ty};
            unsigned np = 2;
            if (em_blocked(fr.v, fr.n, ln_.v, srcs[q], (int)i, sx, sy, tx, ty)) {
                const Lane *SL = &ln_.v[s->lane], *TL = &ln_.v[t->lane];
                double gs = up ? SL->y - LANE_GAP / 2 : SL->y + SL->h + LANE_GAP / 2;
                double gt = up ? TL->y + TL->h + LANE_GAP / 2 : TL->y - LANE_GAP / 2;
                double cx = tx;
                if (em_blocked(fr.v, fr.n, ln_.v, srcs[q], (int)i, tx, gs, tx, gt)) { /* nearest clear column */
                    double best = 1e300;
                    for (size_t k = 0; k < fr.n; k++)
                        for (size_t j = 0; j <= fr.n; j++) { /* middle of the gap to box j, or 12 outside box k */
                            double r = fr.v[k].x + fr.v[k].w, c = j < fr.n ? (r + fr.v[j].x) / 2 : r + 12;
                            if (j < fr.n && fr.v[j].x - r < 16) c = fr.v[k].x - 12;
                            if (fabs(c - tx) < best && !em_blocked(fr.v, fr.n, ln_.v, srcs[q], (int)i, c, gs, c, gt)) best = fabs(c - tx), cx = c;
                        }
                }
                double R[12] = {sx, sy, sx, gs, cx, gs, cx, gt, tx, gt, tx, ty};
                memcpy(P, R, sizeof R), np = 6;
            }
            buf_lit(&b, "<path class=\"em-relation\" fill=\"none\" stroke=\""); put_col(&b, rs, "#333333");
            buf_lit(&b, "\" stroke-width=\"1\" marker-end=\"url(#em-arrowhead)\" d=\"M");
            for (unsigned k = 0; k < np; k++) {
                if (k) buf_lit(&b, " L");
                buf_num(&b, P[2 * k]); buf_ch(&b, ' '); buf_num(&b, P[2 * k + 1]);
            }
            buf_lit(&b, "\"/>");
        }
    }
    buf_lit(&b, "<defs><marker id=\"em-arrowhead\" markerWidth=\"10\" markerHeight=\"7\" refX=\"10\" refY=\"3.5\" orient=\"auto\"><polygon points=\"0 0, 10 3.5, 0 7\" fill=\"");
    put_col(&b, dcfg_get("themeVariables.emArrowhead"), "#333333");
    buf_lit(&b, "\"/></marker></defs>");
    double pad = dcfg_num("eventmodeling.padding", 30);
    if (!(pad >= 0 && pad < 1000)) pad = 30;
    return DG_FINISH(out, &b, bb, pad, "eventmodeling",
        "#mermaid .em-lane{font-size:16px;fill:#333;}#mermaid .em-name{font-weight:bold;fill:#000;}"
        "#mermaid .em-code{font-family:monospace;font-size:13px;fill:#000;}");
}
