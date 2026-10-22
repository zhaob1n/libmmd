/* treeView-beta: file/directory trees from indentation or box-drawing (├── └── │) prefixes. Port of
 * mermaid's treeView renderer: a "/" root row, rows of label height + 2*paddingY, elbow lines, `:::class`
 * (highlight row), `icon(pack:name)` / showIcons file+folder icons, and `## description` columns. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dgraph.h"
#include "dcfg.h"
#include "diagrams.h"
#include "icons.h"
#include "text.h"

typedef struct TNode {
    Str name, cls, icon, desc;
    bool dir, has_icon;
    int level;
    unsigned depth;
    int32_t parent, last_child;
    double y, h, labx, labr;
} TNode;

static const char FOLDER[] = "M10.59 4.59A2 2 0 0 0 9.17 4H4a2 2 0 0 0-2 2v12a2 2 0 0 0 2 2h16a2 2 0 0 0 2-2V8a2 2 0 0 0-2-2h-7.17z";
static const char FILEP[] = "M6 2a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8.83a2 2 0 0 0-.59-1.42l-4.82-4.82A2 2 0 0 0 13.17 2H6Zm7.5 1.9l4.6 4.6h-3.6a1 1 0 0 1-1-1V3.9Z";

/* UTF-8 box-drawing chars used by the preprocessor: ─━│┃└┗├┣ (all E2 94 xx) */
static int box_kind(const char *p, size_t left) { /* 1 branch, 2 dash, 3 vertical, 0 none */
    if (left < 3 || (unsigned char)p[0] != 0xE2 || (unsigned char)p[1] != 0x94) return 0;
    switch ((unsigned char)p[2]) {
    case 0x94: case 0x97: case 0x9c: case 0xa3: return 1;
    case 0x80: case 0x81: return 2;
    case 0x82: case 0x83: return 3;
    default: return 0;
    }
}

static bool css_ok(Str v) {
    if (!v.n || v.n > 40) return false;
    for (size_t i = 0; i < v.n; i++) if (strchr(";{}<>\"'\\", v.p[i])) return false;
    return true;
}

DIAG_FN(treeview_render) {
    (void)fs, (void)title;
    LineIt it = {.s = src, .n = n, .i = off};
    Str ln;
    size_t lo;
    if (!dg_next_line(&it, &ln, &lo)) return diag_fail(err_off, msg, off, "empty treeView");
    /* pass 1: box mode? segment width */
    bool boxm = false;
    size_t segw = 0;
    {
        LineIt j = it;
        while (dg_next_line(&j, &ln, &lo)) {
            size_t ls = lo; /* column = bytes from line start; box chars are 3 bytes but count as 1 column */
            while (ls > 0 && src[ls - 1] != '\n') ls--;
            size_t col = 0;
            for (size_t p = ls; p < n && src[p] != '\n'; ) {
                int k = box_kind(src + p, n - p);
                if (k) boxm = true;
                if (k == 1 && !segw && col > 0) segw = col;
                if (k == 1) break;
                col += src[p] == '\t' ? 4 : 1;
                p += k ? 3 : ((unsigned char)src[p] >= 0xC0 ? 1 : 1);
                while (p < n && ((unsigned char)src[p] & 0xC0) == 0x80) p++;
            }
        }
        if (!segw) segw = 4;
    }
    VEC(TNode) v = {};
    if (!VEC_PUSH(a, &v, ((TNode){.name = S("/"), .dir = true, .level = -1, .parent = -1, .last_child = -1}))) return CMM_ERR_NOMEM;
    int32_t stack[256];
    unsigned sp = 0;
    stack[sp++] = 0;
    while (dg_next_line(&it, &ln, &lo)) {
        Str r = ln, w = dg_word(&r);
        bool block;
        if (str_eq(w, S("title"))) continue;
        if (dcfg_is_acc(ln, &block)) continue;
        size_t ls = lo;
        while (ls > 0 && src[ls - 1] != '\n') ls--;
        int level = 0;
        Str body = ln;
        if (boxm) {
            size_t col = 0, p = ls;
            bool found = false, onlydec = true;
            while (p < n && src[p] != '\n') {
                int k = box_kind(src + p, n - p);
                if (k == 1) {
                    level = (int)lround((double)col / (double)segw) + 1;
                    p += 3;
                    while (box_kind(src + p, n - p) == 2) p += 3;
                    while (p < n && src[p] == ' ') p++;
                    found = true;
                    break;
                }
                if (k == 0 && src[p] != ' ' && src[p] != '\t') { onlydec = false; break; }
                col += src[p] == '\t' ? 4 : 1;
                p += k ? 3 : 1;
            }
            if (found) {
                size_t e = lo + ln.n;
                if (p >= e) continue; /* decoration only */
                body = str_trim((Str){.p = src + p, .n = e - p});
            } else if (onlydec) continue;
            else if (ls != lo && !boxm) level = 0;
            else level = 0;
        } else level = (int)dg_indent(&it, lo);
        /* body: name [:::cls] [icon(x)] [## desc] */
        Str name = body, cls = {}, icon = {}, desc = {};
        bool has_icon = false;
        for (size_t k = 0; k + 1 < body.n; k++) if (body.p[k] == '#' && body.p[k + 1] == '#' && (k == 0 || body.p[k - 1] == ' ')) {
            desc = str_trim((Str){.p = body.p + k + 2, .n = body.n - k - 2});
            name.n = k;
            break;
        }
        name = str_trim(name);
        for (;;) {
            size_t k = name.n;
            while (k > 0 && name.p[k - 1] != ' ') k--;
            Str t = {.p = name.p + k, .n = name.n - k};
            if (k == 0) break;
            if (str_starts(t, S(":::")) && t.n > 3) cls = (Str){.p = t.p + 3, .n = t.n - 3};
            else if (str_starts(t, S("icon(")) && t.p[t.n - 1] == ')') icon = (Str){.p = t.p + 5, .n = t.n - 6}, has_icon = true;
            else break;
            name = str_trim((Str){.p = name.p, .n = k});
        }
        if (name.n >= 3 && str_starts(name, S(":::"))) return diag_fail(err_off, msg, lo, "node needs a name");
        if (name.n >= 2 && name.p[0] == '"' && name.p[name.n - 1] == '"') name.p++, name.n -= 2;
        if (!name.n) return diag_fail(err_off, msg, lo, "empty node");
        bool dir = name.p[name.n - 1] == '/';
        if (dir && name.n > 1) name.n--;
        if (boxm) level *= 4;
        while (sp > 1 && level <= v.v[stack[sp - 1]].level) sp--;
        if (sp >= countof(stack)) return diag_fail(err_off, msg, lo, "tree too deep");
        TNode t = {.name = name, .cls = cls, .icon = icon, .desc = desc, .dir = dir, .has_icon = has_icon, .level = level,
                   .parent = stack[sp - 1], .last_child = -1, .depth = v.v[stack[sp - 1]].depth + 1};
        if (!VEC_PUSH(a, &v, t)) return CMM_ERR_NOMEM;
        v.v[t.parent].last_child = (int32_t)(v.n - 1);
        stack[sp++] = (int32_t)(v.n - 1);
    }
    double ri = fmin(fmax(dcfg_num("treeView.rowIndent", 10), 0), 500), px = fmin(fmax(dcfg_num("treeView.paddingX", 5), 0), 200);
    double py = fmin(fmax(dcfg_num("treeView.paddingY", 5), 0), 200), lt = fmin(fmax(dcfg_num("treeView.lineThickness", 1), 0), 50);
    bool show = dcfg_bool("treeView.showIcons", false);
    double lfs = fmin(fmax(dcfg_num("themeVariables.treeView.labelFontSize", 16), 4), 200);
    Str lcol = dcfg_get("themeVariables.treeView.labelColor"), lncol = dcfg_get("themeVariables.treeView.lineColor");
    Str pack = dcfg_get("treeView.defaultIconPack");
    double lh = lfs * 1.0635, rowh = lh + 2 * py, y = 0, W = 0, maxr = 0;
    Buf b = {};
    buf_lit(&b, "<g class=\"tree-view\">");
    for (size_t i = 0; i < v.n; i++) { /* pre-order already */
        TNode *t = &v.v[i];
        double ind = t->depth * (ri + px);
        /* icon resolution (getNodeIcon) */
        Str ic = {};
        bool want = false;
        if (t->has_icon) want = !str_eq(t->icon, S("none")) && t->icon.n, ic = t->icon;
        else if (show) {
            want = true;
            if (!t->dir) {
                char key[160];
                Str det = {};
                if (t->name.n < 100) {
                    snprintf(key, sizeof key, "treeView.filenameIcons.%.*s", (int)t->name.n, t->name.p);
                    det = dcfg_get(key);
                    const char *dot = nullptr;
                    for (size_t k = 1; k < t->name.n; k++) if (t->name.p[k] == '.') dot = t->name.p + k;
                    if (!det.n && dot) {
                        char ext[100];
                        size_t en = t->name.n - (size_t)(dot - t->name.p);
                        for (size_t k = 0; k < en && k < 99; k++) ext[k] = (char)(dot[k] >= 'A' && dot[k] <= 'Z' ? dot[k] + 32 : dot[k]);
                        ext[en < 99 ? en : 99] = 0;
                        snprintf(key, sizeof key, "treeView.extensionIcons.%s", ext);
                        det = dcfg_get(key);
                        if (!det.n) { snprintf(key, sizeof key, "treeView.extensionIcons.%s", ext + 1); det = dcfg_get(key); }
                    }
                }
                if (str_eq(det, S("none"))) want = false;
                else ic = det;
            }
        }
        t->y = y, t->h = rowh;
        double ix = ind + px, lx = ix + (want ? 18 : 0);
        if (t->cls.n) {
            buf_lit(&b, "<g class=\"");
            if (css_ok(t->cls)) buf_xml(&b, t->cls);
            buf_lit(&b, "\">");
        } else buf_lit(&b, "<g>");
        if (want) {
            buf_lit(&b, "<g class=\"tic\">");
            bool drawn = false;
            if (ic.n) {
                Str q = ic;
                char tmp[160];
                if (!memchr(ic.p, ':', ic.n) && pack.n && ic.n + pack.n < 150 && !str_eq(ic, S("file")) && !str_eq(ic, S("folder"))) {
                    snprintf(tmp, sizeof tmp, "%.*s:%.*s", (int)pack.n, pack.p, (int)ic.n, ic.p);
                    q = (Str){.p = tmp, .n = strlen(tmp)};
                }
                if (!str_eq(ic, S("file")) && !str_eq(ic, S("folder"))) drawn = icon_draw(&b, q, ix, y + py, 14, "tii");
                else ic = str_eq(ic, S("folder")) ? (Str){} : ic, t->dir = str_eq(q, S("folder")) ? true : t->dir;
                if (!drawn && !str_eq(ic, S("file")) && ic.n) { /* unknown pack icon: mermaid's "?" placeholder */
                    buf_lit(&b, "<text x=\""); buf_num(&b, ix + 7); buf_lit(&b, "\" y=\""); buf_num(&b, y + py + 7);
                    buf_lit(&b, "\" text-anchor=\"middle\" dominant-baseline=\"central\" font-size=\"12px\" fill=\"currentColor\">?</text>");
                    drawn = true;
                }
            }
            if (!drawn) {
                buf_lit(&b, "<path fill=\"currentColor\" transform=\"translate("); buf_num(&b, ix); buf_ch(&b, ','); buf_num(&b, y + py);
                buf_lit(&b, ") scale(0.5833)\" ");
                if (t->dir && !(ic.n && str_eq(ic, S("file")))) { buf_lit(&b, "d=\""); buf_lit(&b, FOLDER); }
                else { buf_lit(&b, "fill-rule=\"evenodd\" d=\""); buf_lit(&b, FILEP); }
                buf_lit(&b, "\"/>");
            }
            buf_lit(&b, "</g>");
        }
        double tw = text_width(t->name, lfs) * (t->dir ? 1.07 : 1);
        buf_lit(&b, "<text dominant-baseline=\"middle\" class=\"tl"); if (t->dir) buf_lit(&b, " td");
        buf_lit(&b, "\" x=\""); buf_num(&b, lx); buf_lit(&b, "\" y=\""); buf_num(&b, y + rowh / 2); buf_lit(&b, "\">");
        buf_xml(&b, t->name); buf_lit(&b, "</text></g>");
        t->labx = ind, t->labr = lx + tw;
        maxr = fmax(maxr, t->labr);
        if (t->depth > 0) dg_line(&b, ind - ri, y + rowh / 2, ind, y + rowh / 2, "tln");
        W = fmax(W, ind + tw + 2 * px + (want ? 18 : 0));
        y += rowh;
    }
    for (size_t i = 0; i < v.n; i++) {
        const TNode *t = &v.v[i];
        if (t->last_child < 0) continue;
        const TNode *c = &v.v[t->last_child];
        double x = t->depth * (ri + px) + px;
        dg_line(&b, x, t->y + t->h, x, c->y + c->h / 2 + lt / 2, "tln");
    }
    double dx = maxr + 16;
    for (size_t i = 0; i < v.n; i++) {
        const TNode *t = &v.v[i];
        if (!t->desc.n) continue;
        buf_lit(&b, "<text dominant-baseline=\"middle\" class=\"tds\" x=\""); buf_num(&b, dx); buf_lit(&b, "\" y=\"");
        buf_num(&b, t->y + t->h / 2); buf_lit(&b, "\">"); buf_xml(&b, t->desc); buf_lit(&b, "</text>");
        W = fmax(W, dx + text_width(t->desc, lfs) + px);
    }
    Buf hb = {};
    for (size_t i = 0; i < v.n; i++) {
        const TNode *t = &v.v[i];
        bool hl = false;
        for (size_t k = 0; k + 9 <= t->cls.n; k++) if (!memcmp(t->cls.p + k, "highlight", 9) && (k == 0 || t->cls.p[k - 1] == ' ') && (k + 9 == t->cls.n || t->cls.p[k + 9] == ' ')) hl = true;
        if (!hl) continue;
        double rw = W - t->labx + 8;
        buf_lit(&hb, "<rect class=\"thl\" x=\""); buf_num(&hb, t->labx); buf_lit(&hb, "\" y=\""); buf_num(&hb, t->y + 1);
        buf_lit(&hb, "\" width=\""); buf_num(&hb, rw); buf_lit(&hb, "\" height=\""); buf_num(&hb, t->h - 2); buf_lit(&hb, "\" rx=\"3\"/>");
        W = fmax(W, t->labx + rw + 2);
    }
    /* highlight rects go underneath: prepend */
    Buf body = {};
    buf_put(&body, hb.n, hb.p);
    buf_put(&body, b.n, b.p);
    buf_lit(&body, "</g>");
    bool oom = hb.oom || b.oom;
    free(hb.p), free(b.p);
    if (oom) { free(body.p); return CMM_ERR_NOMEM; }
    char css[640];
    Str lc = css_ok(lcol) ? lcol : S("black"), nc = css_ok(lncol) ? lncol : S("black");
    snprintf(css, sizeof css,
             "#mermaid .tl{font-size:%gpx;fill:%.*s;white-space:pre;}#mermaid .td{font-weight:bold;}"
             "#mermaid .tln{stroke:%.*s;stroke-width:%g;}#mermaid .tic{color:#546e7a;}#mermaid .tii{fill:#546e7a;}"
             "#mermaid .tds{font-size:%gpx;fill:#6a9955;font-style:italic;white-space:pre;}"
             "#mermaid .thl{fill:rgba(255, 193, 7, 0.15);stroke:#ffc107;stroke-width:1;}",
             lfs, (int)lc.n, lc.p, (int)nc.n, nc.p, lt, lfs);
    return dg_finish(out, &body, (BBox){-lt / 2, 0, W - lt / 2, y}, 0, "treeView", strlen(css), css);
}
