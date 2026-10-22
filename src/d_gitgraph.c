/* gitGraph [LR:|TB:|BT:]: commit [id:"x"] [type: NORMAL|REVERSE|HIGHLIGHT] [tag:"t"] [msg:"m"],
 * branch name [order: n], checkout|switch name, merge name [id/tag/type], cherry-pick id:"x" [parent:"y"].
 * Commits advance along the main axis one slot each; branches are lanes. */
#include <stdio.h>

#include "dgraph.h"
#include "diagrams.h"
#include "text.h"

enum : uint8_t { C_NORMAL, C_REVERSE, C_HIGHLIGHT, C_MERGE, C_CHERRY };
typedef struct Commit { Str id, tag, msg; uint32_t lane, p1, p2; uint8_t type; } Commit; /* p = UINT32_MAX: none */
typedef struct Branch { Str name; double order; uint32_t head, idx; bool has_head; } Branch;

static constexpr char CSS[] =
    "#mermaid .bl{stroke-width:2px;stroke-dasharray:2 2;}#mermaid .ar{fill:none;stroke-width:4px;}"
    "#mermaid .cl{font-size:10px;fill:#000;}#mermaid .clb{fill:#ffffde;opacity:0.5;}"
    "#mermaid .bn{font-size:14px;fill:#000;}#mermaid .tg{font-size:10px;fill:#131300;}"
    "#mermaid .tgb{fill:#ECECFF;stroke:#9370DB;stroke-width:1px;}#mermaid .ttl{font-size:18px;}";
static const char *const COL[] = {"#9370DB", "#e6d46c", "#78d070", "#e0a0c0", "#70b0e8", "#e08850", "#70d0c8", "#b07ae0"};
static const char *const LCOL[] = {"#e8e0ff", "#fff8c8", "#e0f8dc", "#f8e0ec", "#ddeefa", "#fbe5d6", "#ddf6f4", "#efe2fb"};

typedef struct Attr { Str id, tag, msg, parent; uint8_t type; double order; bool has_type, has_order; } Attr;
static bool attrs(Str s, Attr *at) {
    while ((s = str_trim(s)).n) {
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
    if (!VEC_PUSH(a, &br, ((Branch){.name = S("main")}))) return CMM_ERR_NOMEM;
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
                char *t = arena_alloc(a, at.id.n + 16, 1);
                if (!t) return CMM_ERR_NOMEM;
                int len = snprintf(t, at.id.n + 16, "cherry:%.*s", (int)at.id.n, at.id.p);
                at.id = (Str){.p = t, .n = (size_t)len};
            } else if (at.has_type && c.type != C_MERGE) c.type = at.type;
            if (!at.id.n) {
                char *idbuf = arena_alloc(a, 16, 1);
                if (!idbuf) return CMM_ERR_NOMEM;
                uint32_t h = (uint32_t)cm.n * 2654435761u ^ 0x5bd1e995u;
                int len = snprintf(idbuf, 16, "%zu-%07x", cm.n, h & 0xfffffffu);
                at.id = (Str){.p = idbuf, .n = (size_t)len};
            }
            for (size_t k = 0; k < cm.n; k++)
                if (str_eq(cm.v[k].id, at.id)) return diag_fail(err_off, msg, lo, "duplicate commit id");
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
            Branch b = {.name = bn, .order = at.has_order ? at.order : (double)br.n, .head = br.v[cur].head,
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
    constexpr double LANE = 60;
    constexpr double R = 10;
    double bnw = 0;
    for (size_t i = 0; i < br.n; i++) { double w = text_width(br.v[i].name, 14); if (w > bnw) bnw = w; }
    double p0 = dir == 0 ? bnw + 40 : 70; /* main-axis origin */
    double q0 = dir == 0 ? (title.n ? 90 : 50) : bnw / 2 + 30;
    double plen = p0 + (double)cm.n * STEP + 30;
    double lanesep = dir == 0 ? LANE : (bnw + 30 > LANE ? bnw + 30 : LANE);
    double qlen = q0 + (double)br.n * lanesep;
#define PX(p, q) (dir == 0 ? (p) : (q))
#define PY(p, q) (dir == 0 ? (q) : dir == 1 ? (p) : plen - (p))
    BBox bb = dir == 0 ? (BBox){0, 0, plen, qlen + 10} : (BBox){0, title.n ? -40 : 0, qlen, plen};
    if (!buf_reserve(out, 4096 + cm.n * 900)) return CMM_ERR_NOMEM;
    DG_HEAD(out, bb, 4, "gitGraph", CSS);
    if (title.n) dg_text(out, title, 18, dir == 0 ? plen / 2 : qlen / 2, dir == 0 ? 20 : -20, "middle", "ttl");
    /* branch lines + labels */
    for (size_t b = 0; b < br.n; b++) {
        double q = q0 + lane[b] * lanesep;
        const char *col = COL[lane[b] % countof(COL)];
        buf_lit(out, "<line class=\"bl\" stroke=\""); buf_put(out, strlen(col), col);
        buf_lit(out, "\" x1=\""); buf_num(out, PX(p0 - 10, q)); buf_lit(out, "\" y1=\""); buf_num(out, PY(p0 - 10, q));
        buf_lit(out, "\" x2=\""); buf_num(out, PX(plen - 10, q)); buf_lit(out, "\" y2=\""); buf_num(out, PY(plen - 10, q));
        buf_lit(out, "\"/>");
        double tw = text_width(br.v[b].name, 14);
        double lx = dir == 0 ? p0 - 20 - tw : q - tw / 2, ly = dir == 0 ? q : (dir == 1 ? p0 - 35 : plen - p0 + 35);
        dg_rect_fill(out, lx - 6, ly - 11, tw + 12, 22, 4, nullptr, LCOL[lane[b] % countof(LCOL)]);
        dg_text(out, br.v[b].name, 14, lx + tw / 2, ly, "middle", "bn");
    }
    /* connections */
    for (size_t i = 0; i < cm.n; i++) {
        const Commit *c = &cm.v[i];
        double p = p0 + (double)i * STEP, q = q0 + lane[c->lane] * lanesep;
        for (int k = 0; k < 2; k++) {
            uint32_t par = k ? c->p2 : c->p1;
            if (par == UINT32_MAX) continue;
            double pp = p0 + (double)par * STEP, pq = q0 + lane[cm.v[par].lane] * lanesep;
            /* colour: the lane that is not the lower-index main lane (mermaid colours by the branch going away) */
            uint32_t cl = lane[cm.v[par].lane] > lane[c->lane] ? lane[cm.v[par].lane] : lane[c->lane];
            const char *col = COL[cl % countof(COL)];
            buf_lit(out, "<path class=\"ar\" stroke=\""); buf_put(out, strlen(col), col); buf_lit(out, "\" d=\"M");
            dg_pt(out, PX(pp, pq), PY(pp, pq));
            if (pq == q) { buf_ch(out, 'L'); dg_pt(out, PX(p, q), PY(p, q)); }
            else {
                /* branching out: turn at the parent; merging in: turn at the child; rounded corner r=10 */
                bool out_ = (lane[c->lane] > lane[cm.v[par].lane] && k == 0) || c->type == C_CHERRY;
                double cp = out_ ? pp : p, s = q > pq ? 1 : -1, rr = 10;
                if (out_) {
                    buf_ch(out, 'L'); dg_pt(out, PX(cp, q - s * rr), PY(cp, q - s * rr));
                    buf_ch(out, 'Q'); dg_pt(out, PX(cp, q), PY(cp, q)); buf_ch(out, ' ');
                    dg_pt(out, PX(cp + rr, q), PY(cp + rr, q));
                    buf_ch(out, 'L'); dg_pt(out, PX(p, q), PY(p, q));
                } else {
                    buf_ch(out, 'L'); dg_pt(out, PX(cp - rr, pq), PY(cp - rr, pq));
                    buf_ch(out, 'Q'); dg_pt(out, PX(cp, pq), PY(cp, pq)); buf_ch(out, ' ');
                    dg_pt(out, PX(cp, pq + s * rr), PY(cp, pq + s * rr));
                    buf_ch(out, 'L'); dg_pt(out, PX(p, q), PY(p, q));
                }
            }
            if (c->type == C_CHERRY && k == 1) buf_lit(out, "\" stroke-dasharray=\"6 4");
            buf_lit(out, "\"/>");
        }
    }
    /* commits, labels, tags */
    for (size_t i = 0; i < cm.n; i++) {
        const Commit *c = &cm.v[i];
        double p = p0 + (double)i * STEP, q = q0 + lane[c->lane] * lanesep, x = PX(p, q), y = PY(p, q);
        const char *col = COL[lane[c->lane] % countof(COL)];
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
        double lx = dir == 0 ? x : x + R + 6 + tw / 2, ly = dir == 0 ? y + R + 14 : y;
        dg_rect_fill(out, lx - tw / 2 - 2, ly - 7, tw + 4, 14, 0, "clb", nullptr);
        dg_text(out, lab, 10, lx, ly, "middle", "cl");
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
