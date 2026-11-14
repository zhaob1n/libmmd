/* packet(-beta): `start-end: "label"`, `start: "label"` or `+bits: "label"` fields packed into
 * rows of packet.bitsPerRow bits, laid out as mermaid's renderer does: each bit bitWidth wide, rows
 * rowHeight tall, paddingX cut off each block's right end, paddingY between rows (plus 10 for the
 * bit numbers when showBits). Fields that straddle a row boundary are split. */
#include <math.h>
#include <stdio.h>

#include "dgraph.h"
#include "dcfg.h"
#include "diagrams.h"
#include "text.h"

typedef struct Field { uint32_t s, e; Str label; } Field;

[[nodiscard]] static bool num(Str s[static 1], uint32_t v[static 1]) {
    size_t i = 0;
    uint64_t x = 0;
    while (i < s->n && (unsigned)(s->p[i] - '0') < 10u && x < (1u << 24)) x = x * 10 + (uint64_t)(s->p[i++] - '0');
    if (!i || x >= (1u << 24)) return false;
    *v = (uint32_t)x, s->p += i, s->n -= i;
    return true;
}

DIAG_FN(packet_render) {
    const uint32_t BPR = (uint32_t)fmin(fmax(dcfg_num("packet.bitsPerRow", 32), 1), 256);
    const double BW = fmin(fmax(dcfg_num("packet.bitWidth", 32), 1), 500);
    const double RH = fmin(fmax(dcfg_num("packet.rowHeight", 32), 1), 500);
    const double PX = fmin(fmax(dcfg_num("packet.paddingX", 5), 0), BW - 1); /* a one-bit block keeps some width */
    const bool BITS = dcfg_bool("packet.showBits", true);
    const double PY = fmin(fmax(dcfg_num("packet.paddingY", 5), 0), 500) + (BITS ? 10 : 0);
    /* themeVariables.packet sizes (mermaid's sanitizer drops byteFontSize and blockStrokeWidth, which
     * are not keys it knows elsewhere; libmmd draws them as asked) */
    const double BF = fmin(fmax(dcfg_num("themeVariables.packet.byteFontSize", 10), 1), 100);
    const double LF = fmin(fmax(dcfg_num("themeVariables.packet.labelFontSize", 12), 1), 100);
    const double TF = fmin(fmax(dcfg_num("themeVariables.packet.titleFontSize", 14), 1), 100);
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
        if (!VEC_PUSH(a, &fv, f)) return MMD_ERR_NOMEM;
    }
    if (!fv.n) return diag_fail(err_off, msg, off, "packet has no fields");
    uint32_t rows = (next + BPR - 1) / BPR;
    /* mermaid puts the title below, centred in a band of rowHeight; a bigger title font widens the band */
    const double TB = title.n ? fmax(RH, TF * 1.2) : 0, TY = rows * (RH + PY) + (PY + TB) / 2;
    double W = BPR * BW + 2, H = rows * (RH + PY) + PY + TB;
    BBox bb = {0, 0, W, H};
    if (BITS) bb.y0 = fmin(bb.y0, PY - 1 - BF * 1.1); /* the first row's bit numbers stand BF tall above it */
    if (title.n) {
        double hw = text_width(title, TF) / 2;
        bb.x0 = fmin(bb.x0, W / 2 - hw), bb.x1 = fmax(bb.x1, W / 2 + hw);
    }
    if (!buf_reserve(out, 1024 + fv.n * 400)) return MMD_ERR_NOMEM;
    char css[320];
    int cn = snprintf(css, sizeof css,
                      "#mermaid .pb{fill:#efefef;stroke:#000;stroke-width:%gpx;}#mermaid .pl{font-size:%gpx;fill:#000;}"
                      "#mermaid .pn{font-size:%gpx;fill:#000;}#mermaid .ttl{font-size:%gpx;fill:#000;}",
                      fmin(fmax(dcfg_num("themeVariables.packet.blockStrokeWidth", 1), 0), 50), LF, BF, TF);
    dg_head(out, bb, 4, "packet", (size_t)cn, css);
    char nb[16];
    for (size_t i = 0; i < fv.n; i++) {
        const Field *f = &fv.v[i];
        for (uint32_t s = f->s; s <= f->e;) { /* one block per row the field touches */
            uint32_t row = s / BPR, e = f->e < (row + 1) * BPR - 1 ? f->e : (row + 1) * BPR - 1;
            double x = 1 + (s % BPR) * BW, w = (e - s + 1) * BW - PX, y = row * (RH + PY) + PY;
            dg_rect(out, x, y, w, RH, 0, "pb");
            dg_text(out, f->label, LF, x + w / 2, y + RH / 2, "middle", "pl");
            if (BITS) { /* mermaid sets them 2 above the block on their baseline */
                double by = y - 1 - BF / 2;
                dg_text(out, (Str){.p = nb, .n = (size_t)snprintf(nb, sizeof nb, "%u", s)}, BF, e == s ? x + w / 2 : x,
                        by, e == s ? "middle" : "start", "pn");
                if (e != s) dg_text(out, (Str){.p = nb, .n = (size_t)snprintf(nb, sizeof nb, "%u", e)}, BF, x + w, by, "end", "pn");
            }
            s = e + 1;
        }
    }
    if (title.n) dg_text(out, title, TF, W / 2, TY, "middle", "ttl");
    buf_lit(out, "</svg>\n");
    return out->oom ? MMD_ERR_NOMEM : MMD_OK;
}
