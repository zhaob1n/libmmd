#include "icons.h"

#include <string.h>

#include "icons_data.h"

static const IconPath *find(size_t n, const IconPath T[static n], Str name) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t m = lo + (hi - lo) / 2, l = strlen(T[m].name);
        int c = memcmp(T[m].name, name.p, l < name.n ? l : name.n);
        if (!c) c = (l > name.n) - (l < name.n);
        if (!c) return &T[m];
        if (c < 0) lo = m + 1; else hi = m;
    }
    return nullptr;
}

static Str strip(Str s, const char *pre) {
    size_t l = strlen(pre);
    return s.n > l && !memcmp(s.p, pre, l) ? (Str){.p = s.p + l, .n = s.n - l} : s;
}

bool icon_draw(Buf *b, Str name, double x, double y, double s, const char *cls) {
    name = str_trim(name);
    const IconPath *ic = nullptr;
    bool fa = false;
    for (const char *const *p = (const char *const[]){"fa ", "fas ", "far ", "fab ", "fa-solid ", "fa-regular ", "fa-brands ", nullptr}; *p; p++)
        if (str_starts(name, (Str){.p = *p, .n = strlen(*p)})) { name = str_trim(strip(name, *p)), fa = true; break; }
    if (str_starts(name, S("fa:"))) name = strip(name, "fa:"), fa = true;
    if (str_starts(name, S("fa-"))) name = strip(name, "fa-"), fa = true;
    name = strip(strip(name, "logos:"), "simple-icons:");
    if (!name.n || name.n > 64) return false;
    if (fa) {
        ic = find(countof(ICON_FA), ICON_FA, name);
        if (!ic) ic = find(countof(ICON_LOGOS), ICON_LOGOS, name);
    } else {
        ic = find(countof(ICON_LOGOS), ICON_LOGOS, name);
        if (!ic) ic = find(countof(ICON_FA), ICON_FA, name);
    }
    if (!ic) return false;
    double k = s / (ic->w > ic->h ? ic->w : ic->h), ox = x + (s - ic->w * k) / 2, oy = y + (s - ic->h * k) / 2;
    buf_lit(b, "<path class=\"");
    buf_put(b, strlen(cls), cls);
    buf_lit(b, "\" transform=\"translate(");
    buf_num(b, ox); buf_ch(b, ','); buf_num(b, oy);
    buf_lit(b, ") scale(");
    buf_num(b, k);
    buf_lit(b, ")\" d=\"");
    buf_put(b, strlen(ic->d), ic->d);
    buf_lit(b, "\"/>");
    return true;
}
