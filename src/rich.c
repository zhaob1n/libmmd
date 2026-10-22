/* Rich flowchart labels: markdown emphasis toggles, word wrap, Font Awesome tokens. */
#include "rich.h"

#include "text.h"

typedef struct FaIcon { char name[20]; uint16_t cp; } FaIcon;
/* Font Awesome 6 free codepoints (solid unless brand); sorted by name for bsearch */
static constexpr FaIcon FA[] = {
    {"arrow-right", 0xf061}, {"aws", 0xf375}, {"ban", 0xf05e}, {"bell", 0xf0f3}, {"bolt", 0xf0e7},
    {"box", 0xf466}, {"bug", 0xf188}, {"building", 0xf1ad}, {"calendar", 0xf133}, {"camera-retro", 0xf083},
    {"car", 0xf1b9}, {"chart-bar", 0xf080}, {"check", 0xf00c}, {"clock", 0xf017}, {"cloud", 0xf0c2},
    {"code", 0xf121}, {"cog", 0xf013}, {"comment", 0xf075}, {"credit-card", 0xf09d}, {"cube", 0xf1b2},
    {"cubes", 0xf1b3}, {"database", 0xf1c0}, {"docker", 0xf395}, {"download", 0xf019}, {"edit", 0xf304},
    {"envelope", 0xf0e0}, {"exclamation", 0xf12a}, {"file", 0xf15b}, {"fire", 0xf06d}, {"flag", 0xf024},
    {"folder", 0xf07b}, {"gear", 0xf013}, {"github", 0xf09b}, {"globe", 0xf0ac}, {"heart", 0xf004},
    {"home", 0xf015}, {"house", 0xf015}, {"industry", 0xf275}, {"info", 0xf129}, {"key", 0xf084},
    {"laptop", 0xf109}, {"leaf", 0xf06c}, {"list", 0xf03a}, {"lock", 0xf023}, {"magnifying-glass", 0xf002},
    {"microchip", 0xf2db}, {"mobile", 0xf10b}, {"network-wired", 0xf6ff}, {"pen", 0xf304}, {"phone", 0xf095},
    {"play", 0xf04b}, {"question", 0xf128}, {"robot", 0xf544}, {"rocket", 0xf135}, {"search", 0xf002},
    {"server", 0xf233}, {"shield", 0xf132}, {"shopping-cart", 0xf07a}, {"sitemap", 0xf0e8}, {"spinner", 0xf110},
    {"star", 0xf005}, {"stop", 0xf04d}, {"tag", 0xf02b}, {"times", 0xf00d}, {"trash", 0xf1f8},
    {"truck", 0xf0d1}, {"twitter", 0xf099}, {"upload", 0xf093}, {"user", 0xf007}, {"users", 0xf0c0},
    {"wrench", 0xf0ad}, {"xmark", 0xf00d},
};

static inline bool fa_name_ch(char c) {
    return (unsigned)((c | 32) - 'a') < 26u || (unsigned)(c - '0') < 10u || c == '-';
}

size_t rich_fa_token(Str s, size_t i, uint32_t *cp) {
    /* fa:fa-x  fab:fa-x  fas:fa-x  far:fa-x  fal:fa-x  fak:fa-x */
    size_t j = i;
    if (j + 6 > s.n || s.p[j] != 'f' || s.p[j + 1] != 'a') return 0;
    if (i > 0 && fa_name_ch(s.p[i - 1])) return 0;
    j += 2;
    if (s.p[j] != ':') j++;
    if (j + 4 > s.n || s.p[j] != ':' || s.p[j + 1] != 'f' || s.p[j + 2] != 'a' || s.p[j + 3] != '-') return 0;
    j += 4;
    size_t ns = j;
    while (j < s.n && fa_name_ch(s.p[j])) j++;
    if (j == ns) return 0;
    Str name = {.p = s.p + ns, .n = j - ns};
    size_t lo = 0, hi = countof(FA);
    *cp = 0xf111; /* unknown: generic circle */
    while (lo < hi) {
        size_t m = (lo + hi) / 2;
        size_t ml = strlen(FA[m].name);
        int c = memcmp(FA[m].name, name.p, ml < name.n ? ml : name.n);
        if (!c) c = ml < name.n ? -1 : ml > name.n ? 1 : 0;
        if (!c) { *cp = FA[m].cp; break; }
        if (c < 0) lo = m + 1; else hi = m;
    }
    return j - i;
}

bool rich_needed(Str s) {
    for (size_t i = 0; i < s.n; i++) {
        char c = s.p[i];
        if (c == RICH_MD || c == RICH_BOLD || c == RICH_ITAL) return true;
        if (c == 'f' && i + 1 < s.n && s.p[i + 1] == 'a') {
            uint32_t cp;
            if (rich_fa_token(s, i, &cp)) return true;
        }
    }
    return false;
}

static bool is_br(Str s, size_t i, size_t *len) {
    if (i + 4 > s.n || s.p[i] != '<' || (s.p[i + 1] | 32) != 'b' || (s.p[i + 2] | 32) != 'r') return false;
    size_t j = i + 3;
    while (j < s.n && s.p[j] == ' ') j++;
    if (j < s.n && s.p[j] == '/') j++;
    if (j < s.n && s.p[j] == '>') { *len = j + 1 - i; return true; }
    return false;
}

Str rich_normalize(Arena *a, Str raw, bool *ok) {
    Str s = str_trim(raw);
    if (!(s.n >= 4 && s.p[0] == '"' && s.p[s.n - 1] == '"' && s.p[1] == '`' && s.p[s.n - 2] == '`'))
        return label_normalize(a, raw, ok);
    *ok = true;
    s.p += 2, s.n -= 4;
    char *o = arena_alloc(a, s.n + 2, 1);
    if (!o) { *ok = false; return (Str){}; }
    size_t k = 0;
    o[k++] = RICH_MD;
    for (size_t i = 0; i < s.n; i++) {
        char c = s.p[i];
        size_t bl = 0;
        if (c == '<' && is_br(s, i, &bl)) { o[k++] = '\n'; i += bl - 1; continue; }
        if (c == '*' || c == '_') {
            bool dbl = i + 1 < s.n && s.p[i + 1] == c;
            /* intra-word underscores are literal (snake_case) */
            if (c == '_' && !dbl && i > 0 && i + 1 < s.n && s.p[i - 1] != ' ' && s.p[i + 1] != ' ') { o[k++] = c; continue; }
            o[k++] = dbl ? RICH_BOLD : RICH_ITAL;
            if (dbl) i++;
            continue;
        }
        if (c == '\n') {
            while (k > 1 && o[k - 1] == ' ') k--;
            o[k++] = '\n';
            while (i + 1 < s.n && is_sp(s.p[i + 1])) i++;
            continue;
        }
        o[k++] = c;
    }
    o[k] = 0;
    return (Str){.p = o, .n = k};
}

/* width of one line, honouring bold toggles and fa tokens */
static double line_width(Str ln, double fs, bool *bold) {
    double w = 0;
    size_t i = 0, run = 0;
    while (i < ln.n) {
        char c = ln.p[i];
        uint32_t cp;
        size_t tl = 0;
        if (c == RICH_BOLD || c == RICH_ITAL || c == RICH_MD || (c == 'f' && (tl = rich_fa_token(ln, i, &cp)))) {
            double rw = text_width((Str){.p = ln.p + run, .n = i - run}, fs);
            w += *bold ? rw * 1.08 : rw;
            if (c == RICH_BOLD) *bold = !*bold;
            if (tl) { w += fs * 1.25; i += tl; }
            else i++;
            run = i;
            continue;
        }
        i++;
    }
    double rw = text_width((Str){.p = ln.p + run, .n = i - run}, fs);
    return w + (*bold ? rw * 1.08 : rw);
}

void rich_measure(Str s, double fs, double *w, unsigned *lines) {
    double mw = 0;
    unsigned cnt = 0;
    size_t pos = 0;
    Str ln;
    bool bold = false;
    if (!s.n || (s.n == 1 && s.p[0] == RICH_MD)) { *w = 0; *lines = 0; return; }
    while (label_next_line(s, &pos, &ln)) {
        double lw = line_width(ln, fs, &bold);
        if (lw > mw) mw = lw;
        cnt++;
    }
    *w = mw;
    *lines = cnt;
}

Str rich_wrap(Arena *a, Str s, double fs, double maxw, bool *ok) {
    *ok = true;
    if (!s.n || s.p[0] != RICH_MD) return s;
    double w;
    unsigned nl;
    rich_measure(s, fs, &w, &nl);
    if (w <= maxw) return s;
    char *o = arena_alloc(a, s.n + 1, 1);
    if (!o) { *ok = false; return (Str){}; }
    memcpy(o, s.p, s.n);
    o[s.n] = 0;
    /* greedy: replace a space with '\n' when the line would overflow */
    size_t ls = 0, last_sp = SIZE_MAX;
    bool bold = false, bold_ls = false;
    for (size_t i = 0; i <= s.n; i++) {
        if (i < s.n && o[i] != ' ' && o[i] != '\n') continue;
        bool b2 = bold_ls;
        double lw = line_width((Str){.p = o + ls, .n = i - ls}, fs, &b2);
        if (lw > maxw && last_sp != SIZE_MAX) {
            o[last_sp] = '\n';
            bool b3 = bold_ls;
            (void)line_width((Str){.p = o + ls, .n = last_sp - ls}, fs, &b3);
            bold_ls = b3;
            ls = last_sp + 1;
            last_sp = SIZE_MAX;
            i = ls - 1; /* re-scan the carried word */
            continue;
        }
        if (i == s.n) break;
        if (o[i] == '\n') { bold_ls = b2; ls = i + 1; last_sp = SIZE_MAX; }
        else last_sp = i;
    }
    (void)bold;
    return (Str){.p = o, .n = s.n};
}

static void put_cp(Buf *b, uint32_t cp) {
    char t[8];
    static constexpr char HEX[] = "0123456789abcdef";
    size_t k = 0;
    t[k++] = '&'; t[k++] = '#'; t[k++] = 'x';
    for (int sh = 12; sh >= 0; sh -= 4) t[k++] = HEX[(cp >> sh) & 15u];
    buf_put(b, k, t);
    buf_ch(b, ';');
}

static void open_span(Buf *b, bool bold, bool ital) {
    if (!bold && !ital) return;
    buf_lit(b, "<tspan");
    if (bold) buf_lit(b, " font-weight=\"bold\"");
    if (ital) buf_lit(b, " font-style=\"italic\"");
    buf_ch(b, '>');
}

void rich_emit_line(Buf *b, Str ln, bool *bold, bool *ital) {
    open_span(b, *bold, *ital);
    size_t i = 0, run = 0;
    while (i < ln.n) {
        char c = ln.p[i];
        uint32_t cp = 0;
        size_t tl = 0;
        if (c == RICH_BOLD || c == RICH_ITAL || c == RICH_MD || (c == 'f' && (tl = rich_fa_token(ln, i, &cp)))) {
            buf_xml(b, (Str){.p = ln.p + run, .n = i - run});
            if (tl) {
                buf_lit(b, "<tspan class=\"fa\" font-family=\"'Font Awesome 6 Free','Font Awesome 5 Free',FontAwesome\" font-weight=\"900\">");
                put_cp(b, cp);
                buf_lit(b, "</tspan>");
                i += tl;
            } else {
                if (c != RICH_MD) {
                    if (*bold || *ital) buf_lit(b, "</tspan>");
                    if (c == RICH_BOLD) *bold = !*bold; else *ital = !*ital;
                    open_span(b, *bold, *ital);
                }
                i++;
            }
            run = i;
            continue;
        }
        i++;
    }
    buf_xml(b, (Str){.p = ln.p + run, .n = i - run});
    if (*bold || *ital) buf_lit(b, "</tspan>");
}
