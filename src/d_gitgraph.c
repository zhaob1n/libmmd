/* gitGraph [LR:|TB:|BT:]: commit [id:"x"] [type: NORMAL|REVERSE|HIGHLIGHT] [tag:"t"] [msg:"m"],
 * branch name [order: n], checkout|switch name, merge name [id/tag/type], cherry-pick id:"x" [parent:"y"].
 * Commits advance along the main axis one slot each; branches are lanes. */
#include <math.h>
#include <stdio.h>

#include "dgraph.h"
#include "dcfg.h"
#include "diagrams.h"
#include "text.h"

enum : uint8_t { C_NORMAL, C_REVERSE, C_HIGHLIGHT, C_MERGE, C_CHERRY };
typedef struct Commit { Str id, tag, msg; uint32_t lane, p1, p2; uint8_t type; } Commit; /* p = UINT32_MAX: none */
typedef struct Branch { Str name; double order; uint32_t head, idx; bool has_head; } Branch;

static constexpr char CSS[] =
    "#mermaid .bl{stroke:#333;stroke-width:1px;stroke-dasharray:2;}#mermaid .ar{fill:none;stroke-width:8px;stroke-linecap:round;}"
    "#mermaid .cl{font-size:10px;fill:#000;}#mermaid .clb{fill:#ffffde;opacity:0.5;}"
    "#mermaid .bn{font-size:14px;fill:#000;}#mermaid .bnx{fill:inherit;}#mermaid .tg{font-size:10px;fill:#131300;}"
    "#mermaid .tgb{fill:#ECECFF;stroke:#9370DB;stroke-width:1px;}#mermaid .ttl{font-size:18px;}";
/* mermaid 11 default theme git0..git7 (hsl(h, 100%, 46.27%)) and their branch-label text colours */
static const char *const COL[] = {"#0000ec", "#dede00", "#9dec00", "#0076ec", "#00ecec", "#00ec76", "#ec00ec", "#ec0000"};
static const char *const LTXT[] = {"#fff", "#000", "#000", "#fff", "#000", "#000", "#000", "#000"};

typedef struct Attr { Str id, tag, msg, parent; uint8_t type; double order; bool has_type, has_order; } Attr;
static bool attrs(Str s, Attr *at) {
    while ((s = str_trim(s)).n) {
        if (s.p[0] == '"') { /* legacy `commit "msg"` */
            const char *q = memchr(s.p + 1, '"', s.n - 1);
            if (!q) return false;
            at->msg = (Str){.p = s.p + 1, .n = (size_t)(q - s.p - 1)};
            s = (Str){.p = q + 1, .n = s.n - (size_t)(q + 1 - s.p)};
            continue;
        }
        const char *c = memchr(s.p, ':', s.n);
        if (!c) return false;
        Str k = str_trim((Str){.p = s.p, .n = (size_t)(c - s.p)});
        s = str_trim((Str){.p = c + 1, .n = s.n - (size_t)(c + 1 - s.p)});
        Str v;
        if (s.n && s.p[0] == '"') {
            const char *q = memchr(s.p + 1, '"', s.n - 1);
            if (!q) return false;
            v = (Str){.p = s.p + 1, .n = (size_t)(q - s.p - 1)};
            s = (Str){.p = q + 1, .n = s.n - (size_t)(q + 1 - s.p)};
        } else v = dg_word(&s);
        if (str_eq(k, S("id"))) at->id = v;
        else if (str_eq(k, S("tag"))) at->tag = v;
        else if (str_eq(k, S("msg"))) at->msg = v;
        else if (str_eq(k, S("parent"))) at->parent = v;
        else if (str_eq(k, S("type"))) {
            at->has_type = true;
            if (str_eq(v, S("NORMAL"))) at->type = C_NORMAL;
            else if (str_eq(v, S("REVERSE"))) at->type = C_REVERSE;
            else if (str_eq(v, S("HIGHLIGHT"))) at->type = C_HIGHLIGHT;
            else return false;
        } else if (str_eq(k, S("order"))) { if (!dg_parse_num(v, &at->order)) return false; at->has_order = true; }
        else return false;
    }
    return true;
}

/* mermaid gives unordered branches order parseFloat(`0.${index}`) (so they precede order >= 1) */
static double mm_order(size_t i) {
    double d = 1;
    for (size_t t = i; t; t /= 10) d *= 10;
    return i ? (double)i / d : 0;
}

DIAG_FN(gitgraph_render) {
    (void)fs;
    LineIt it = {.s = src, .n = n, .i = off};
    Str line;
    size_t lo;
    if (!dg_next_line(&it, &line, &lo)) return diag_fail(err_off, msg, off, "empty gitGraph");
    int dir = 0; /* 0 LR, 1 TB, 2 BT */
    {
        Str r = line;
        (void)dg_word(&r);
        r = str_trim(r);
        if (str_starts(r, S("TB"))) dir = 1;
        else if (str_starts(r, S("BT"))) dir = 2;
        if (str_starts(line, S("gitGraph TB")) || str_starts(line, S("gitGraph:TB"))) dir = 1;
    }
    VEC(Commit) cm = {};
    VEC(Branch) br = {};
    Str mainb = dcfg_get("gitGraph.mainBranchName");
    if (!mainb.n) mainb = S("main");
    if (!VEC_PUSH(a, &br, ((Branch){.name = mainb}))) return CMM_ERR_NOMEM;
    uint32_t cur = 0;
    while (dg_next_line(&it, &line, &lo)) {
        Str rest = line, w = dg_word(&rest);
        if (str_eq(w, S("title"))) { title = str_trim(rest); continue; }
        if (str_starts(w, S("acc"))) continue;
        Attr at = {};
        if (str_eq(w, S("commit")) || str_eq(w, S("merge")) || str_eq(w, S("cherry-pick"))) {
            Commit c = {.lane = cur, .p1 = br.v[cur].has_head ? br.v[cur].head : UINT32_MAX, .p2 = UINT32_MAX};
            if (w.p[0] == 'm') {
                Str bn = dg_word(&rest);
                if (bn.n >= 2 && bn.p[0] == '"') bn = (Str){.p = bn.p + 1, .n = bn.n - 2};
                size_t b = 0;
                while (b < br.n && !str_eq(br.v[b].name, bn)) b++;
                if (b == br.n) return diag_fail(err_off, msg, (size_t)(bn.p - src), "merge of unknown branch");
                if (b == cur) return diag_fail(err_off, msg, lo, "cannot merge a branch into itself");
                if (!br.v[b].has_head) return diag_fail(err_off, msg, lo, "merged branch has no commits");
                c.p2 = br.v[b].head, c.type = C_MERGE;
            }
            if (!attrs(rest, &at)) return diag_fail(err_off, msg, (size_t)(rest.p - src), "bad commit attributes");
            if (w.p[0] == 'c' && w.n > 6) {
                size_t k = 0;
                while (k < cm.n && !str_eq(cm.v[k].id, at.id)) k++;
                if (!at.id.n || k == cm.n) return diag_fail(err_off, msg, lo, "cherry-pick of unknown commit");
                c.type = C_CHERRY, c.p2 = (uint32_t)k;
                c.tag = at.tag;
                if (!c.tag.n) { /* mermaid default tag: cherry-pick:<src>[|parent:<p>] */
                    size_t tl = at.id.n + at.parent.n + 24;
                    char *t2 = arena_alloc(a, tl, 1);
                    if (!t2) return CMM_ERR_NOMEM;
                    int l2 = cm.v[k].type == C_MERGE ? snprintf(t2, tl, "cherry-pick:%.*s|parent:%.*s", (int)at.id.n, at.id.p, (int)at.parent.n, at.parent.p)
                                                     : snprintf(t2, tl, "cherry-pick:%.*s", (int)at.id.n, at.id.p);
                    c.tag = (Str){.p = t2, .n = (size_t)l2};
                }
                char *t = arena_alloc(a, at.id.n + 16, 1);
                if (!t) return CMM_ERR_NOMEM;
                int len = snprintf(t, at.id.n + 16, "cherry:%.*s", (int)at.id.n, at.id.p);
                at.id = (Str){.p = t, .n = (size_t)len};
            } else if (at.has_type && c.type != C_MERGE) c.type = at.type;
            if (!at.id.n) {
                char *idbuf = arena_alloc(a, 16, 1);
                if (!idbuf) return CMM_ERR_NOMEM;
                /* mermaid: "<seq>-<7 hex>"; deterministic FNV-1a over seq, branch and parent */
                uint64_t h = 1469598103934665603u;
                const uint64_t mix[3] = {cm.n, cur, c.p1};
                for (size_t q = 0; q < 3; q++) for (int bt = 0; bt < 8; bt++) h = (h ^ ((mix[q] >> (bt * 8)) & 0xff)) * 1099511628211u;
                for (size_t q = 0; q < br.v[cur].name.n; q++) h = (h ^ (uint8_t)br.v[cur].name.p[q]) * 1099511628211u;
                int len = snprintf(idbuf, 16, "%zu-%07llx", cm.n, (unsigned long long)(h & 0xfffffffu));
                at.id = (Str){.p = idbuf, .n = (size_t)len};
            }
            /* mermaid only warns on duplicate ids */
            c.id = at.id, c.tag = at.tag, c.msg = at.msg;
            if (!VEC_PUSH(a, &cm, c)) return CMM_ERR_NOMEM;
            br.v[cur].head = (uint32_t)cm.n - 1, br.v[cur].has_head = true;
            continue;
        }
        if (str_eq(w, S("branch"))) {
            Str bn = dg_word(&rest);
            if (bn.n >= 2 && bn.p[0] == '"') bn = (Str){.p = bn.p + 1, .n = bn.n - 2};
            if (!bn.n) return diag_fail(err_off, msg, lo, "branch needs a name");
            for (size_t b = 0; b < br.n; b++)
                if (str_eq(br.v[b].name, bn)) return diag_fail(err_off, msg, (size_t)(bn.p - src), "branch already exists");
            if (!attrs(rest, &at)) return diag_fail(err_off, msg, (size_t)(rest.p - src), "bad branch attributes");
            Branch b = {.name = bn, .order = at.has_order ? at.order : mm_order(br.n), .head = br.v[cur].head,
                        .has_head = br.v[cur].has_head, .idx = (uint32_t)br.n};
            if (!VEC_PUSH(a, &br, b)) return CMM_ERR_NOMEM;
            cur = (uint32_t)br.n - 1;
            continue;
        }
        if (str_eq(w, S("checkout")) || str_eq(w, S("switch"))) {
            Str bn = dg_word(&rest);
            if (bn.n >= 2 && bn.p[0] == '"') bn = (Str){.p = bn.p + 1, .n = bn.n - 2};
            size_t b = 0;
            while (b < br.n && !str_eq(br.v[b].name, bn)) b++;
            if (b == br.n) return diag_fail(err_off, msg, (size_t)(bn.p - src), "checkout of unknown branch");
            cur = (uint32_t)b;
            continue;
        }
        return diag_fail(err_off, msg, lo, "unknown gitGraph statement");
    }
    /* lane order: by `order`, then declaration */
    uint32_t *lane = ANEW(a, uint32_t, br.n);
    uint32_t *ord = ANEW(a, uint32_t, br.n);
    if (!lane || !ord) return CMM_ERR_NOMEM;
    for (size_t i = 0; i < br.n; i++) {
        size_t k = i;
        while (k > 0 && br.v[ord[k - 1]].order > br.v[i].order) { ord[k] = ord[k - 1]; k--; }
        ord[k] = (uint32_t)i;
    }
    for (size_t i = 0; i < br.n; i++) lane[ord[i]] = (uint32_t)i;
    constexpr double STEP = 50;
    const bool rot = dcfg_bool("gitGraph.rotateCommitLabel", true);
    const double LANE = rot ? 90 : 50; /* mermaid setBranchPosition: 50 + 40 when rotateCommitLabel */
    constexpr double R = 10;
    double bnw = 0;
    for (size_t i = 0; i < br.n; i++) { double w = text_width(br.v[i].name, 14); if (w > bnw) bnw = w; }
    double p0 = dir == 0 ? bnw + 40 : 70; /* main-axis origin */
    double q0 = dir == 0 ? 50 : bnw / 2 + 30;
    /* parallelCommits: a commit sits one step after its furthest parent instead of at its sequence index */
    double *col = ANEW(a, double, cm.n ? cm.n : 1);
    if (!col) return CMM_ERR_NOMEM;
    const bool par_c = dcfg_bool("gitGraph.parallelCommits", false);
    double ncol = 0;
    for (size_t i = 0; i < cm.n; i++) {
        col[i] = (double)i;
        if (par_c) {
            col[i] = 0;
            if (cm.v[i].p1 != UINT32_MAX) col[i] = fmax(col[i], col[cm.v[i].p1] + 1);
            if (cm.v[i].p2 != UINT32_MAX) col[i] = fmax(col[i], col[cm.v[i].p2] + 1);
        }
        ncol = fmax(ncol, col[i] + 1);
    }
    if (dir == 0 && !rot && !par_c && cm.n) { /* deviate from mermaid: horizontal labels never overlap their lane neighbour */
        double *lx = ANEW(a, double, 2 * br.n + 2);
        if (!lx) return CMM_ERR_NOMEM;
        for (size_t l = 0; l < br.n; l++) lx[2 * l] = -HUGE_VAL, lx[2 * l + 1] = 0;
        double x = 0;
        for (size_t i = 0; i < cm.n; i++) {
            uint32_t l = cm.v[i].lane;
            double w = cm.v[i].type == C_CHERRY ? 0 : text_width(cm.v[i].id, 10) + 4;
            if (i) x += STEP;
            if (l < br.n && lx[2 * l] > -HUGE_VAL) x = fmax(x, lx[2 * l] + (lx[2 * l + 1] + w) / 2 + 8);
            col[i] = x / STEP;
            if (l < br.n) lx[2 * l] = x, lx[2 * l + 1] = w;
        }
        ncol = col[cm.n - 1] + 1;
    }
    double plen = p0 + ncol * STEP; /* branch lines end 40 past the last commit */
    double lanesep = dir == 0 ? LANE : (bnw + 30 > LANE ? bnw + 30 : LANE);
    /* lane offsets: a lane whose rotated commit labels hang deeper than the default gap pushes the
     * lanes below it down so the labels clear the next branch line */
    double *lq = ANEW(a, double, br.n + 1);
    if (!lq) return CMM_ERR_NOMEM;
    {
        double *drop = ANEW(a, double, br.n + 1);
        if (!drop) return CMM_ERR_NOMEM;
        if (dir == 0 && rot)
            for (size_t i = 0; i < cm.n; i++) {
                if (cm.v[i].type == C_CHERRY) continue;
                double tw = text_width(cm.v[i].id, 10);
                double d = (tw / 2 - 8 + 28.5) / sqrt(2) + 10 + tw / 25 * 8.5;
                uint32_t l = lane[cm.v[i].lane];
                if (d > drop[l]) drop[l] = d;
            }
        for (size_t l = 1; l < br.n; l++) lq[l] = lq[l - 1] + fmax(lanesep, drop[l - 1] + 12);
    }
    double qlen = q0 + (br.n ? lq[br.n - 1] : 0) + lanesep;
#define PX(p, q) (dir == 0 ? (p) : (q))
#define PY(p, q) (dir == 0 ? (q) : dir == 1 ? (p) : plen - (p))
    /* LR: mermaid sizes the viewBox to the drawn content (+8): branch labels on top, the rotated commit
     * labels (see below) at the bottom */
    double top = q0 - 11, bot = q0 + (br.n ? lq[br.n - 1] : 0) + 11;
    if (title.n) top = fmin(top, q0 - 41); /* insertTitle: baseline at -titleTopMargin (25) above lane 0 */
    if (dir == 0)
        for (size_t i = 0; i < cm.n; i++) {
            double q = q0 + lq[lane[cm.v[i].lane]], tw = text_width(cm.v[i].id, 10);
            if (cm.v[i].type != C_CHERRY) bot = fmax(bot, rot ? q + (tw / 2 - 8 + 28.5) / sqrt(2) + 10 + tw / 25 * 8.5 /* rotated about (x - 10, y) */
                                                               : q + 28.5);
            if (cm.v[i].tag.n) top = fmin(top, q - R - 28);
        }
    double left = p0 - 10 - 23 - bnw - (rot ? 30 : 0);
    if (dir == 0 && rot) /* rotated commit labels hang down-left of their commit */
        for (size_t i = 0; i < cm.n; i++) {
            if (cm.v[i].type == C_CHERRY) continue;
            double tw = text_width(cm.v[i].id, 10), x = p0 + col[i] * STEP, y = q0 + lq[lane[cm.v[i].lane]];
            double rx = -7.5 - (tw + 10) / 25 * 9.5, cx = x - 10, c = sqrt(.5);
            for (int k = 0; k < 4; k++) {
                double px = (k & 1 ? x + tw / 2 + 2 : x - tw / 2 - 2) - cx, py = (k & 2 ? y + 28.5 : y + 13.5) - y;
                left = fmin(left, rx + cx + px * c + py * c);
            }
        }
    BBox bb = dir == 0 ? (BBox){left - 4, top - 4, plen - 6, bot + 4} : (BBox){0, title.n ? -40 : 0, qlen, plen};
    if (!buf_reserve(out, 4096 + cm.n * 900)) return CMM_ERR_NOMEM;
    DG_HEAD(out, bb, 4, "gitGraph", CSS);
    if (title.n) dg_text(out, title, 18, dir == 0 ? plen / 2 : qlen / 2, dir == 0 ? q0 - 31.3 : -20, "middle", "ttl");
    /* branch lines + labels */
    for (size_t b = 0; b < br.n; b++) {
        double q = q0 + lq[lane[b]];
        const char *col = dcfg_pal("git", lane[b] % countof(COL), COL[lane[b] % countof(COL)]);
        buf_lit(out, "<line class=\"bl\" stroke=\""); buf_put(out, strlen(col), col);
        buf_lit(out, "\" x1=\""); buf_num(out, PX(p0 - 10, q)); buf_lit(out, "\" y1=\""); buf_num(out, PY(p0 - 10, q));
        buf_lit(out, "\" x2=\""); buf_num(out, PX(plen - 10, q)); buf_lit(out, "\" y2=\""); buf_num(out, PY(plen - 10, q));
        buf_lit(out, "\"/>");
        double tw = text_width(br.v[b].name, 14);
        /* LR drawBranches: label box x = -bw - 4 - (rotate ? 30 : 0), width bw + 18, shifted -19 */
        double lx = dir == 0 ? p0 - 10 - 17 - tw - (rot ? 30 : 0) : q - tw / 2, ly = dir == 0 ? q : (dir == 1 ? p0 - 35 : plen - p0 + 35);
        if (dir == 0) dg_rect_fill(out, lx - 6, ly - 11, tw + 18, 22, 4, nullptr, col);
        else dg_rect_fill(out, lx - 6, ly - 11, tw + 12, 22, 4, nullptr, col);
        buf_lit(out, "<g fill=\""); buf_put(out, strlen(LTXT[lane[b] % countof(LTXT)]), LTXT[lane[b] % countof(LTXT)]); buf_lit(out, "\">");
        dg_text(out, br.v[b].name, 14, lx + tw / 2 + (dir == 0 ? 3 : 0), ly, "middle", "bn bnx");
        buf_lit(out, "</g>");
    }
    /* connections */
    for (size_t i = 0; i < cm.n; i++) {
        const Commit *c = &cm.v[i];
        double p = p0 + col[i] * STEP, q = q0 + lq[lane[c->lane]];
        for (int k = 0; k < 2; k++) {
            uint32_t par = k ? c->p2 : c->p1;
            if (par == UINT32_MAX) continue;
            double pp = p0 + col[par] * STEP, pq = q0 + lq[lane[cm.v[par].lane]];
            /* colour: the lane that is not the lower-index main lane (mermaid colours by the branch going away) */
            uint32_t cl = lane[cm.v[par].lane] > lane[c->lane] ? lane[cm.v[par].lane] : lane[c->lane];
            const char *col = dcfg_pal("git", cl % countof(COL), COL[cl % countof(COL)]);
            buf_lit(out, "<path class=\"ar\" stroke=\""); buf_put(out, strlen(col), col); buf_lit(out, "\" d=\"M");
            dg_pt(out, PX(pp, pq), PY(pp, pq));
            /* orthogonal route with rounded corners. Branch-out (and cherry-pick) turns at the parent and runs
             * on the child lane; merges run on the parent lane and turn at the child. If that run would pass
             * over other commits of the lane, detour through a lane-gap track (mermaid's "findLane"). */
            double P[4][2];
            size_t np = 0;
            P[np][0] = pp, P[np++][1] = pq;
            if (pq != q) {
                bool out_ = (lane[c->lane] > lane[cm.v[par].lane] && k == 0) || c->type == C_CHERRY;
                uint32_t run = out_ ? c->lane : cm.v[par].lane;
                bool clash = false;
                for (size_t j = par + 1; j < i && !clash; j++) clash = cm.v[j].lane == run;
                if (clash) {
                    double mid = (pq + q) / 2 + ((double)(i % 3) - 1) * 6;
                    P[np][0] = pp, P[np++][1] = mid;
                    P[np][0] = p, P[np++][1] = mid;
                } else if (out_) P[np][0] = pp, P[np++][1] = q;
                else P[np][0] = p, P[np++][1] = pq;
            }
            P[np][0] = p, P[np++][1] = q;
            for (size_t j = 1; j < np; j++) {
                if (j + 1 < np) { /* corner at P[j]: stop rr short, curve through it */
                    double rr = 10, ax = P[j][0] - P[j - 1][0], ay = P[j][1] - P[j - 1][1];
                    double bx = P[j + 1][0] - P[j][0], by = P[j + 1][1] - P[j][1];
                    double la = fmax(1e-9, fabs(ax) + fabs(ay)), lb = fmax(1e-9, fabs(bx) + fabs(by));
                    double r1 = fmin(rr, la / 2), r2 = fmin(rr, lb / 2);
                    double sx = P[j][0] - ax / la * r1, sy = P[j][1] - ay / la * r1;
                    double ex = P[j][0] + bx / lb * r2, ey = P[j][1] + by / lb * r2;
                    buf_ch(out, 'L'); dg_pt(out, PX(sx, sy), PY(sx, sy));
                    buf_ch(out, 'Q'); dg_pt(out, PX(P[j][0], P[j][1]), PY(P[j][0], P[j][1])); buf_ch(out, ' ');
                    dg_pt(out, PX(ex, ey), PY(ex, ey));
                } else { buf_ch(out, 'L'); dg_pt(out, PX(P[j][0], P[j][1]), PY(P[j][0], P[j][1])); }
            }
            if (c->type == C_CHERRY && k == 1) buf_lit(out, "\" stroke-dasharray=\"6 4");
            buf_lit(out, "\"/>");
        }
    }
    /* commits, labels, tags */
    for (size_t i = 0; i < cm.n; i++) {
        const Commit *c = &cm.v[i];
        double p = p0 + col[i] * STEP, q = q0 + lq[lane[c->lane]], x = PX(p, q), y = PY(p, q);
        const char *col = dcfg_pal("git", lane[c->lane] % countof(COL), COL[lane[c->lane] % countof(COL)]);
        if (c->type == C_HIGHLIGHT) {
            dg_rect_fill(out, x - R, y - R, 2 * R, 2 * R, 0, nullptr, col);
            dg_rect_fill(out, x - R + 3, y - R + 3, 2 * R - 6, 2 * R - 6, 0, nullptr, "#fff");
        } else {
            buf_lit(out, "<circle cx=\""); buf_num(out, x); buf_lit(out, "\" cy=\""); buf_num(out, y);
            buf_lit(out, "\" r=\""); buf_num(out, R); buf_lit(out, "\" fill=\""); buf_put(out, strlen(col), col);
            buf_lit(out, "\"/>");
            if (c->type == C_MERGE) {
                buf_lit(out, "<circle fill=\"#fff\" cx=\""); buf_num(out, x); buf_lit(out, "\" cy=\""); buf_num(out, y);
                buf_lit(out, "\" r=\"6\"/>");
            } else if (c->type == C_REVERSE) {
                buf_lit(out, "<path stroke=\"#fff\" stroke-width=\"3\" d=\"M"); dg_pt(out, x - 5, y - 5); buf_ch(out, 'L');
                dg_pt(out, x + 5, y + 5); buf_ch(out, 'M'); dg_pt(out, x - 5, y + 5); buf_ch(out, 'L');
                dg_pt(out, x + 5, y - 5); buf_lit(out, "\"/>");
            } else if (c->type == C_CHERRY) {
                buf_lit(out, "<circle fill=\"#fff\" r=\"2.75\" cx=\""); buf_num(out, x - 3); buf_lit(out, "\" cy=\"");
                buf_num(out, y + 2); buf_lit(out, "\"/><circle fill=\"#fff\" r=\"2.75\" cx=\""); buf_num(out, x + 3);
                buf_lit(out, "\" cy=\""); buf_num(out, y + 2); buf_lit(out, "\"/>");
            }
        }
        Str lab = c->id;
        double tw = text_width(lab, 10);
        if (c->type == C_CHERRY) { /* mermaid drawCommits: no commit label on cherry-picks */
        } else if (dir == 0 && !rot) {
            dg_rect_fill(out, x - tw / 2 - 2, y + 13.5, tw + 4, 15, 0, "clb", nullptr);
            dg_text(out, lab, 10, x, y + 21, "middle", "cl");
        } else if (dir == 0) { /* mermaid rotateCommitLabel (default on): label hangs down-left at -45 degrees */
            double rx = -7.5 - (tw + 10) / 25 * 9.5, ry = 10 + tw / 25 * 8.5;
            buf_lit(out, "<g transform=\"translate("); dg_pt(out, rx, ry); buf_lit(out, ") rotate(-45 ");
            buf_num(out, x - 10); buf_ch(out, ' '); buf_num(out, y); buf_lit(out, ")\">");
            dg_rect_fill(out, x - tw / 2 - 2, y + 13.5, tw + 4, 15, 0, "clb", nullptr);
            dg_text(out, lab, 10, x, y + 21, "middle", "cl");
            buf_lit(out, "</g>");
        } else {
            double lx = x + R + 6 + tw / 2, ly = y;
            dg_rect_fill(out, lx - tw / 2 - 2, ly - 7, tw + 4, 14, 0, "clb", nullptr);
            dg_text(out, lab, 10, lx, ly, "middle", "cl");
        }
        if (c->tag.n) {
            double gw = text_width(c->tag, 10) + 12;
            double gx = dir == 0 ? x - gw / 2 : x - R - 8 - gw, gy = dir == 0 ? y - R - 28 : y - 9;
            dg_rect_fill(out, gx, gy, gw, 18, 3, "tgb", nullptr);
            dg_text(out, c->tag, 10, gx + gw / 2, gy + 9, "middle", "tg");
        }
    }
#undef PX
#undef PY
    buf_lit(out, "</svg>\n");
    return out->oom ? CMM_ERR_NOMEM : CMM_OK;
}
