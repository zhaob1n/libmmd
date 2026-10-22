#include "text.h"

/* Advance widths (1/1000 em) for ASCII 32..126, Helvetica/Arial-compatible metrics,
 * a close stand-in for mermaid's default "trebuchet ms", verdana, arial stack. */
static constexpr uint16_t ASCII_W[95] = {
    278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333, 278, 278, /*  !"#$%&'()*+,-./ */
    556, 556, 556, 556, 556, 556, 556, 556, 556, 556, 278, 278, 584, 584, 584, 556, /* 0-9:;<=>? */
    1015, 667, 667, 722, 722, 667, 611, 778, 722, 278, 500, 667, 556, 833, 722, 778, /* @A-O */
    667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 278, 278, 278, 469, 556, /* P-Z[\]^_ */
    333, 556, 556, 500, 556, 556, 278, 556, 556, 222, 222, 500, 222, 833, 556, 556, /* `a-o */
    556, 556, 333, 500, 278, 556, 500, 722, 500, 500, 500, 334, 260, 334, 584,      /* p-z{|}~ */
};

static inline unsigned cp_width(uint32_t cp) {
    if (cp >= 32 && cp < 127) return ASCII_W[cp - 32];
    if (cp < 32) return 0;
    /* East Asian wide / fullwidth ranges */
    if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
        (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1FAFF) ||
        (cp >= 0x20000 && cp <= 0x3FFFD))
        return 1000;
    if (cp >= 0x0300 && cp <= 0x036F) return 0; /* combining */
    return 580;
}

double text_width(Str s, double fs) {
    uint64_t acc = 0;
    const unsigned char *p = (const unsigned char *)s.p;
    size_t i = 0, n = s.n;
    /* fast ASCII path, 8 bytes at a time */
    while (i < n) {
        if (i + 8 <= n) {
            uint64_t w;
            memcpy(&w, p + i, 8);
            if (!(w & 0x8080808080808080ull)) {
                for (unsigned k = 0; k < 8; k++) acc += cp_width(p[i + k]);
                i += 8;
                continue;
            }
        }
        unsigned c = p[i];
        uint32_t cp = c;
        size_t len = 1;
        if (c >= 0xF0 && i + 3 < n) { cp = ((c & 7u) << 18) | ((p[i + 1] & 63u) << 12) | ((p[i + 2] & 63u) << 6) | (p[i + 3] & 63u); len = 4; }
        else if (c >= 0xE0 && i + 2 < n) { cp = ((c & 15u) << 12) | ((p[i + 1] & 63u) << 6) | (p[i + 2] & 63u); len = 3; }
        else if (c >= 0xC0 && i + 1 < n) { cp = ((c & 31u) << 6) | (p[i + 1] & 63u); len = 2; }
        acc += cp_width(cp);
        i += len;
    }
    return (double)acc * fs / 1000.0;
}

static bool is_br(Str s, size_t i, size_t *len) {
    /* <br>, <br/>, <br />, case-insensitive */
    if (i + 4 > s.n || s.p[i] != '<' || (s.p[i + 1] | 32) != 'b' || (s.p[i + 2] | 32) != 'r') return false;
    size_t j = i + 3;
    while (j < s.n && s.p[j] == ' ') j++;
    if (j < s.n && s.p[j] == '/') j++;
    if (j < s.n && s.p[j] == '>') { *len = j + 1 - i; return true; }
    return false;
}

Str label_normalize(Arena *a, Str raw, bool *ok) {
    *ok = true;
    Str s = str_trim(raw);
    bool md = false;
    if (s.n >= 2 && s.p[0] == '"' && s.p[s.n - 1] == '"') {
        s.p++, s.n -= 2;
        if (s.n >= 2 && s.p[0] == '`' && s.p[s.n - 1] == '`') { s.p++, s.n -= 2; md = true; }
    }
    bool need = false;
    for (size_t i = 0; i < s.n; i++) {
        char c = s.p[i];
        if (c == '<' || (md && (c == '*' || c == '_' || c == '\n'))) { need = true; break; }
    }
    if (!need) return s;
    char *o = arena_alloc(a, s.n + 1, 1);
    if (!o) { *ok = false; return (Str){}; }
    size_t k = 0;
    for (size_t i = 0; i < s.n; i++) {
        size_t bl = 0;
        char c = s.p[i];
        if (c == '<' && is_br(s, i, &bl)) { o[k++] = '\n'; i += bl - 1; continue; }
        if (md && (c == '*' || c == '_')) continue; /* basic markdown: drop emphasis markers */
        if (md && c == '\n') {
            while (k && o[k - 1] == ' ') k--;
            o[k++] = '\n';
            while (i + 1 < s.n && is_sp(s.p[i + 1])) i++;
            continue;
        }
        o[k++] = c;
    }
    o[k] = 0;
    return (Str){.p = o, .n = k};
}

bool label_next_line(Str s, size_t *pos, Str *line) {
    if (*pos > s.n) return false;
    const char *st = s.p + *pos;
    const char *nl = memchr(st, '\n', s.n - *pos);
    size_t len = nl ? (size_t)(nl - st) : s.n - *pos;
    *line = (Str){.p = st, .n = len};
    *pos += len + 1;
    return true;
}

void label_measure(Str s, double fs, double *w, unsigned *lines) {
    double mw = 0;
    unsigned cnt = 0;
    size_t pos = 0;
    Str ln;
    if (s.n == 0) { *w = 0; *lines = 0; return; }
    while (label_next_line(s, &pos, &ln)) {
        double lw = text_width(ln, fs);
        if (lw > mw) mw = lw;
        cnt++;
    }
    *w = mw;
    *lines = cnt;
}
