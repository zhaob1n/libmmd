/* packet(-beta): `start-end: "label"`, `start: "label"` or `+bits: "label"` fields packed into
 * 32-bit rows (mermaid defaults: bitWidth 32, rowHeight 32, paddingX 5, paddingY 5). Fields that
 * straddle a row boundary are split; bit numbers are printed above each field's ends. */
#include <math.h>
#include <stdio.h>

#include "dgraph.h"
#include "dcfg.h"
#include "diagrams.h"
#include "text.h"

typedef struct Field { uint32_t s, e; Str label; } Field;

static constexpr char CSS[] =
    "#mermaid .pb{fill:#efefef;stroke:#000;stroke-width:1px;}#mermaid .pl{font-size:12px;fill:#000;}"
    "#mermaid .pn{font-size:10px;fill:#000;}#mermaid .ttl{font-size:14px;fill:#000;}";

static bool num(Str *s, uint32_t *v) {
    size_t i = 0;
    uint64_t x = 0;
    while (i < s->n && (unsigned)(s->p[i] - '0') < 10u && x < (1u << 24)) x = x * 10 + (uint64_t)(s->p[i++] - '0');
    if (!i || x >= (1u << 24)) return false;
    *v = (uint32_t)x, s->p += i, s->n -= i;
    return true;
}

DIAG_FN(packet_render) {
    (void)fs;
    const uint32_t BPR = (uint32_t)fmin(fmax(dcfg_num("packet.bitsPerRow", 32), 1), 256);
    constexpr double BW = 32;
    constexpr double RH = 32;
    constexpr double PX = 5;
    constexpr double PY = 5;
    LineIt it = {.s = src, .n = n, .i = off};
    Str line;
    size_t lo;
    if (!dg_next_line(&it, &line, &lo)) return diag_fail(err_off, msg, off, "empty packet");
    VEC(Field) fv = {};
    uint32_t next = 0;
    while (dg_next_line(&it, &line, &lo)) {
        if (str_starts(line, S("title"))) { title = str_trim((Str){.p = line.p + 5, .n = line.n - 5}); continue; }
        if (str_starts(line, S("acc"))) continue;
        Str s = line;
        Field f = {};
        if (s.p[0] == '+') {
            s.p++, s.n--;
            uint32_t bits;
            if (!num(&s, &bits) || !bits) return diag_fail(err_off, msg, lo, "expected +bits");
            f.s = next, f.e = next + bits - 1;
        } else {
            if (!num(&s, &f.s)) return diag_fail(err_off, msg, lo, "expected bit number");
            f.e = f.s;
            if (s.n && s.p[0] == '-') { s.p++, s.n--; if (!num(&s, &f.e)) return diag_fail(err_off, msg, lo, "expected end bit"); }
            if (f.e < f.s) return diag_fail(err_off, msg, lo, "end bit before start bit");
            if (f.s != next) return diag_fail(err_off, msg, lo, "packet fields must be contiguous");
        }
        s = str_trim(s);
        if (!s.n || s.p[0] != ':') return diag_fail(err_off, msg, (size_t)(s.p - src), "expected ':'");
        s = str_trim((Str){.p = s.p + 1, .n = s.n - 1});
        if (s.n >= 2 && s.p[0] == '"' && s.p[s.n - 1] == '"') s = (Str){.p = s.p + 1, .n = s.n - 2};
        f.label = s;
        next = f.e + 1;
        if (next > (1u << 20)) return diag_fail(err_off, msg, lo, "packet too large");
        if (!VEC_PUSH(a, &fv, f)) return CMM_ERR_NOMEM;
    }
    if (!fv.n) return diag_fail(err_off, msg, off, "packet has no fields");
    uint32_t rows = (next + BPR - 1) / BPR;
    double top = 0, W = BPR * BW + 2 * PX, H = rows * (RH + PY + 14) + PY + (title.n ? 30 : 0); /* mermaid puts the title below */
    BBox bb = {0, 0, W, H};
    if (!buf_reserve(out, 1024 + fv.n * 400)) return CMM_ERR_NOMEM;
    DG_HEAD(out, bb, 4, "packet", CSS);
    char nb[16];
    for (size_t i = 0; i < fv.n; i++) {
        const Field *f = &fv.v[i];
        for (uint32_t s = f->s; s <= f->e;) { /* one block per row the field touches */
            uint32_t row = s / BPR, e = f->e < (row + 1) * BPR - 1 ? f->e : (row + 1) * BPR - 1;
            double x = PX + (s % BPR) * BW, w = (e - s + 1) * BW, y = top + row * (RH + PY + 14) + 14;
            dg_rect(out, x, y, w - 1, RH, 0, "pb");
            dg_text(out, f->label, 12, x + w / 2, y + RH / 2, "middle", "pl");
            dg_text(out, (Str){.p = nb, .n = (size_t)snprintf(nb, sizeof nb, "%u", s)}, 10, x + 2, y - 6,
                    s == e ? "start" : "start", "pn");
            if (e != s) dg_text(out, (Str){.p = nb, .n = (size_t)snprintf(nb, sizeof nb, "%u", e)}, 10, x + w - 3, y - 6, "end", "pn");
            s = e + 1;
        }
    }
    if (title.n) dg_text(out, title, 14, W / 2, H - 16, "middle", "ttl");
    buf_lit(out, "</svg>\n");
    return out->oom ? CMM_ERR_NOMEM : CMM_OK;
}
