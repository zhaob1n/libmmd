/* kanban: top-level lines are columns, indented lines are cards. Both accept `id[Label]`, `[Label]`
 * or bare text; cards take `@{ assigned: 'x', ticket: T-1, priority: 'High' }` metadata.
 * Geometry follows mermaid: 200px columns, 10px gaps, wrapped card text, priority stripe. */
#include <math.h>

#include "dgraph.h"
#include "dcfg.h"
#include "diagrams.h"
#include "text.h"

typedef struct KItem { Str label, assigned, ticket, priority; uint32_t col; double h; } KItem;

static constexpr char CSS[] =
    "#mermaid .kc{stroke:#9370DB;stroke-width:1px;}#mermaid .kt{font-size:16px;fill:#333;}"
    "#mermaid .ki{fill:#fff;stroke:#9370DB;stroke-width:1px;}#mermaid .kl{font-size:16px;fill:#333;}"
    "#mermaid .km{font-size:12px;fill:#666;}#mermaid .kp{stroke-width:4px;}";
static const char *const COLF[] = {"#ECECFF", "#ffffde", "#e8f4ea", "#fde8e8", "#e8f0fd", "#f8e8fd"};

static Str unq(Str s) {
    s = str_trim(s);
    if (s.n >= 2 && (s.p[0] == '"' || s.p[0] == '\'') && s.p[s.n - 1] == s.p[0]) s = (Str){.p = s.p + 1, .n = s.n - 2};
    return s;
}
/* `id[label]`, `[label]`, `id(label)` or text; returns label, strips/parses @{..} into *meta */
static Str node_label(Str s, Str *meta) {
    *meta = (Str){};
    for (size_t i = 0; i + 1 < s.n; i++)
        if (s.p[i] == '@' && s.p[i + 1] == '{') {
            const char *e = memchr(s.p + i, '}', s.n - i);
            *meta = (Str){.p = s.p + i + 2, .n = e ? (size_t)(e - s.p) - i - 2 : s.n - i - 2};
            s = str_trim((Str){.p = s.p, .n = i});
            break;
        }
    for (size_t i = 0; i < s.n; i++) {
        char o = s.p[i], c = o == '[' ? ']' : o == '(' ? ')' : o == '{' ? '}' : 0;
        if (c && s.p[s.n - 1] == c) {
            size_t k = i;
            while (k < s.n && (s.p[k] == '[' || s.p[k] == '(' || s.p[k] == '{')) k++;
            size_t e = s.n;
            while (e > k && (s.p[e - 1] == ']' || s.p[e - 1] == ')' || s.p[e - 1] == '}')) e--;
            return unq((Str){.p = s.p + k, .n = e - k});
        }
    }
    return unq(s);
}

DIAG_FN(kanban_render) {
    (void)fs;
    LineIt it = {.s = src, .n = n, .i = off};
    Str line;
    size_t lo;
    if (!dg_next_line(&it, &line, &lo)) return diag_fail(err_off, msg, off, "empty kanban");
    VEC(Str) cols = {};
    VEC(KItem) items = {};
    unsigned base = UINT32_MAX;
    while (dg_next_line(&it, &line, &lo)) {
        if (str_starts(line, S("title ")) && !cols.n) { title = str_trim((Str){.p = line.p + 6, .n = line.n - 6}); continue; }
        unsigned in = dg_indent(&it, lo);
        if (base == UINT32_MAX) base = in;
        Str meta;
        Str lab = node_label(line, &meta);
        if (in <= base) {
            if (!VEC_PUSH(a, &cols, lab)) return CMM_ERR_NOMEM;
            continue;
        }
        if (!cols.n) return diag_fail(err_off, msg, lo, "card before any column");
        KItem k = {.label = lab, .col = (uint32_t)cols.n - 1};
        for (Str m = meta; m.n;) { /* key: value pairs */
            const char *cm = memchr(m.p, ',', m.n);
            size_t L = cm ? (size_t)(cm - m.p) : m.n;
            Str kv = str_trim((Str){.p = m.p, .n = L});
            m = cm ? (Str){.p = cm + 1, .n = m.n - L - 1} : (Str){};
            const char *col = memchr(kv.p, ':', kv.n);
            if (!col) continue;
            Str key = str_trim((Str){.p = kv.p, .n = (size_t)(col - kv.p)}), val = unq((Str){.p = col + 1, .n = kv.n - (size_t)(col + 1 - kv.p)});
            if (str_eq(key, S("assigned"))) k.assigned = val;
            else if (str_eq(key, S("ticket"))) k.ticket = val;
            else if (str_eq(key, S("priority"))) k.priority = val;
        }
        if (!VEC_PUSH(a, &items, k)) return CMM_ERR_NOMEM;
    }
    if (!cols.n) return diag_fail(err_off, msg, off, "kanban has no columns");
    constexpr double CW = 200;
    constexpr double GAP = 5; /* mermaid 11: 200px columns, 5px apart, 185px cards */
    constexpr double PADC = 7.5;
    double top = title.n ? 40 : 0, colh = 0;
    double *ch = ANEW(a, double, cols.n);
    if (!ch) return CMM_ERR_NOMEM;
    for (size_t c = 0; c < cols.n; c++) ch[c] = 30;
    for (size_t i = 0; i < items.n; i++) {
        KItem *k = &items.v[i];
        Str w = dg_wrap(a, k->label, 16, 175);
        if (!w.p && k->label.n) return CMM_ERR_NOMEM;
        k->label = w;
        double tw;
        unsigned nl;
        label_measure(w, 16, &tw, &nl);
        k->h = nl * 24 + 20 + (k->assigned.n || k->ticket.n ? 20 : 0);
        ch[k->col] += k->h + 5;
    }
    for (size_t c = 0; c < cols.n; c++) if (ch[c] > colh) colh = ch[c];
    double W = (double)cols.n * (CW + GAP) - GAP;
    BBox bb = {0, 0, W, top + colh + 10};
    if (!buf_reserve(out, 1024 + cols.n * 300 + items.n * 600)) return CMM_ERR_NOMEM;
    DG_HEAD(out, bb, 6, "kanban", CSS);
    double *cy = ANEW(a, double, cols.n);
    if (!cy) return CMM_ERR_NOMEM;
    for (size_t c = 0; c < cols.n; c++) {
        double x = (double)c * (CW + GAP);
        dg_rect_fill(out, x, top, CW, ch[c] + 5, 5, "kc", dcfg_pal("cScale", c % 10 + 2, COLF[c % countof(COLF)])); /* mermaid sizes each column to its cards */
        Str t = dg_wrap(a, cols.v[c], 16, CW - 20);
        dg_text(out, t.p ? t : cols.v[c], 16, x + CW / 2, top + 13, "middle", "kt");
        cy[c] = top + 28;
    }
    for (size_t i = 0; i < items.n; i++) {
        const KItem *k = &items.v[i];
        double x = (double)k->col * (CW + GAP) + PADC, y = cy[k->col], w = CW - 2 * PADC;
        dg_rect(out, x, y, w, k->h, 5, "ki");
        const char *pc = nullptr;
        if (str_eq(k->priority, S("Very High"))) pc = "red";
        else if (str_eq(k->priority, S("High"))) pc = "orange";
        else if (str_eq(k->priority, S("Low"))) pc = "blue";
        else if (str_eq(k->priority, S("Very Low"))) pc = "lightblue";
        if (pc) {
            buf_lit(out, "<line class=\"kp\" stroke=\""); buf_put(out, strlen(pc), pc); buf_lit(out, "\" x1=\"");
            buf_num(out, x + 2); buf_lit(out, "\" y1=\""); buf_num(out, y + 3); buf_lit(out, "\" x2=\""); buf_num(out, x + 2);
            buf_lit(out, "\" y2=\""); buf_num(out, y + k->h - 3); buf_lit(out, "\"/>");
        }
        double meta = k->assigned.n || k->ticket.n ? 20 : 0;
        dg_text(out, k->label, 16, x + 10, y + (k->h - meta) / 2, "start", "kl");
        if (k->ticket.n) dg_text(out, k->ticket, 12, x + 10, y + k->h - 14, "start", "km");
        if (k->assigned.n) dg_text(out, k->assigned, 12, x + w - 8, y + k->h - 14, "end", "km");
        cy[k->col] += k->h + 5;
    }
    if (title.n) dg_text(out, title, 18, W / 2, 18, "middle", "kt");
    buf_lit(out, "</svg>\n");
    return out->oom ? CMM_ERR_NOMEM : CMM_OK;
}
