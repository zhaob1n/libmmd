/* gantt: title, dateFormat, axisFormat, tickInterval, excludes (weekends / dates), todayMarker,
 * sections, tasks "name : [done|active|crit|milestone|vert,]* [id,] [start|after a b,] end|duration|until x".
 * Time is UTC milliseconds; dates are civil (no time zones). */
#include <math.h>
#include <stdio.h>

#include "dgraph.h"
#include "diagrams.h"
#include "text.h"

enum : uint8_t { T_DONE = 1, T_ACTIVE = 2, T_CRIT = 4, T_MILE = 8, T_VERT = 16 };
typedef struct Task {
    Str name, id, start, end; /* raw specs */
    double t0, t1;
    uint32_t sec;
    uint8_t tags, state; /* state: 0 unresolved, 1 in progress, 2 done */
} Task;
typedef struct GCtx {
    Arena *a;
    Task *t;
    size_t nt;
    Str fmt;
    bool ex_weekends, inclusive;
    VEC(double) exdays; /* excluded day numbers */
} GCtx;

constexpr double DAY = 86400000.0;

static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400), doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
static void civil_from_days(int64_t z, int64_t *y, unsigned *m, unsigned *d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097), yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int64_t)yoe + era * 400 + (*m <= 2);
}

/* parse s according to a dayjs-like format (YYYY YY MM M DD D HH H mm m ss s SSS X x, literals) */
static bool parse_date(Str fmt, Str s, double *out) {
    s = str_trim(s);
    int64_t y = 1970;
    unsigned mo = 1, d = 1, h = 0, mi = 0, se = 0, ms = 0;
    size_t i = 0, j = 0;
    while (i < fmt.n) {
        static const struct { const char *t; uint8_t len, maxd, which; } TK[] = {
            {"YYYY", 4, 4, 0}, {"SSS", 3, 3, 6}, {"YY", 2, 2, 7}, {"MM", 2, 2, 1}, {"DD", 2, 2, 2}, {"HH", 2, 2, 3},
            {"mm", 2, 2, 4}, {"ss", 2, 2, 5}, {"M", 1, 2, 1}, {"D", 1, 2, 2}, {"H", 1, 2, 3}, {"m", 1, 2, 4},
            {"s", 1, 2, 5}, {"X", 1, 12, 8}, {"x", 1, 15, 9}};
        size_t k = 0;
        for (; k < countof(TK); k++)
            if (fmt.n - i >= TK[k].len && !memcmp(fmt.p + i, TK[k].t, TK[k].len)) break;
        if (k == countof(TK)) { /* literal */
            if (j >= s.n || s.p[j] != fmt.p[i]) return false;
            i++, j++;
            continue;
        }
        i += TK[k].len;
        bool neg = TK[k].which >= 8 && j < s.n && s.p[j] == '-';
        if (neg) j++;
        uint64_t v = 0;
        unsigned nd = 0;
        while (j < s.n && nd < TK[k].maxd && (unsigned)(s.p[j] - '0') < 10u) v = v * 10 + (uint64_t)(s.p[j++] - '0'), nd++;
        if (!nd) return false;
        switch (TK[k].which) {
        case 0: y = (int64_t)v; break;
        case 1: mo = (unsigned)v; break;
        case 2: d = (unsigned)v; break;
        case 3: h = (unsigned)v; break;
        case 4: mi = (unsigned)v; break;
        case 5: se = (unsigned)v; break;
        case 6: ms = (unsigned)v; break;
        case 7: y = 2000 + (int64_t)v - (v > 68 ? 100 : 0); break;
        case 8: *out = (neg ? -1.0 : 1.0) * (double)v * 1000; return j == s.n;
        case 9: *out = (neg ? -1.0 : 1.0) * (double)v; return j == s.n;
        }
    }
    if (j != s.n || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 24 || mi > 59 || se > 60) return false;
    *out = (double)days_from_civil(y, mo, d) * DAY + ((h * 60.0 + mi) * 60 + se) * 1000 + ms;
    return true;
}

static double add_months(double t, double n) {
    int64_t z = (int64_t)floor(t / DAY), y;
    unsigned m, d;
    civil_from_days(z, &y, &m, &d);
    int64_t mm = y * 12 + (m - 1) + (int64_t)n;
    y = mm >= 0 ? mm / 12 : (mm - 11) / 12;
    m = (unsigned)(mm - y * 12) + 1;
    static const unsigned ML[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    unsigned lim = ML[m - 1] + (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0));
    if (d > lim) d = lim;
    return (double)days_from_civil(y, m, d) * DAY + (t - (double)z * DAY);
}

/* "3d", "1.5h", "2w", "1M", "1y", "500ms" -> adds to t; false when not a duration */
static bool add_duration(Str s, double t, double *out) {
    s = str_trim(s);
    size_t i = 0;
    while (i < s.n && ((unsigned)(s.p[i] - '0') < 10u || s.p[i] == '.')) i++;
    double v;
    if (!i || !dg_parse_num((Str){.p = s.p, .n = i}, &v)) return false;
    Str u = (Str){.p = s.p + i, .n = s.n - i};
    double unit;
    if (str_eq(u, S("ms"))) unit = 1;
    else if (str_eq(u, S("s"))) unit = 1000;
    else if (str_eq(u, S("m"))) unit = 60000;
    else if (str_eq(u, S("h"))) unit = 3600000;
    else if (str_eq(u, S("d"))) unit = DAY;
    else if (str_eq(u, S("w"))) unit = 7 * DAY;
    else if (str_eq(u, S("M"))) { *out = add_months(t, v); return true; }
    else if (str_eq(u, S("y"))) { *out = add_months(t, v * 12); return true; }
    else return false;
    *out = t + v * unit;
    return true;
}

static bool excluded(const GCtx *c, double day) {
    if (c->ex_weekends) {
        int64_t wd = ((int64_t)day % 7 + 7 + 4) % 7; /* 1970-01-01 was a Thursday (4) */
        if (wd == 0 || wd == 6) return true;
    }
    for (size_t i = 0; i < c->exdays.n; i++) if (c->exdays.v[i] == day) return true;
    return false;
}
/* mermaid pushes the end of a task forward by the excluded days inside it */
static double fix_excludes(const GCtx *c, double t0, double t1) {
    if (!c->ex_weekends && !c->exdays.n) return t1;
    double d = floor(t0 / DAY);
    for (int guard = 0; d * DAY < t1 && guard < 100000; d++, guard++)
        if (excluded(c, d)) t1 += DAY;
    return t1;
}

static Task *find_id(const GCtx *c, Str id) {
    for (size_t i = 0; i < c->nt; i++) if (c->t[i].id.n && str_eq(c->t[i].id, id)) return &c->t[i];
    return nullptr;
}
static bool resolve(GCtx *c, size_t i);
/* "after a b": latest end; "until a": earliest start */
static bool ref_time(GCtx *c, Str list, bool want_end, double *out) {
    bool any = false;
    double best = want_end ? -1e300 : 1e300;
    for (Str w = dg_word(&list); w.n; w = dg_word(&list)) {
        Task *r = find_id(c, w);
        if (!r || !resolve(c, (size_t)(r - c->t))) continue;
        double v = want_end ? r->t1 : r->t0;
        if (want_end ? v > best : v < best) best = v;
        any = true;
    }
    if (any) *out = best;
    return any;
}
static bool resolve(GCtx *c, size_t i) {
    Task *t = &c->t[i];
    if (t->state == 2) return true;
    if (t->state == 1) return false; /* cycle */
    t->state = 1;
    double t0 = 0, t1;
    Str st = t->start;
    if (!st.n) {
        if (i == 0 || !resolve(c, i - 1)) t0 = (double)days_from_civil(2026, 1, 1) * DAY; /* no anchor: arbitrary */
        else t0 = c->t[i - 1].t1;
    } else if (str_starts(st, S("after "))) {
        if (!ref_time(c, (Str){.p = st.p + 6, .n = st.n - 6}, true, &t0)) t0 = 0;
    } else if (!parse_date(c->fmt, st, &t0) && !add_duration(st, i ? c->t[i - 1].t1 : 0, &t0)) t0 = 0;
    Str en = t->end;
    if (str_starts(en, S("until "))) {
        if (!ref_time(c, (Str){.p = en.p + 6, .n = en.n - 6}, false, &t1)) t1 = t0;
    } else if (add_duration(en, t0, &t1)) t1 = fix_excludes(c, t0, t1);
    else if (parse_date(c->fmt, en, &t1)) { if (c->inclusive) t1 += DAY; }
    else t1 = t0 + DAY;
    if (t->tags & (T_MILE | T_VERT)) t1 = t0;
    if (t1 < t0) t1 = t0;
    t->t0 = t0, t->t1 = t1, t->state = 2;
    return true;
}

/* strftime subset for the axis */
static size_t fmt_time(Str f, double t, size_t cap, char out[static cap]) {
    static const char *const MON[] = {"January", "February", "March", "April", "May", "June", "July",
                                      "August", "September", "October", "November", "December"};
    static const char *const WD[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    int64_t z = (int64_t)floor(t / DAY), y;
    unsigned m, d;
    civil_from_days(z, &y, &m, &d);
    double ms = t - (double)z * DAY;
    unsigned H = (unsigned)(ms / 3600000), M = (unsigned)fmod(ms / 60000, 60), Sx = (unsigned)fmod(ms / 1000, 60);
    unsigned wd = (unsigned)((z % 7 + 7 + 4) % 7);
    size_t o = 0;
    for (size_t i = 0; i < f.n && o + 24 < cap; i++) {
        if (f.p[i] != '%' || i + 1 >= f.n) { out[o++] = f.p[i]; continue; }
        char k = f.p[++i];
        int r = 0;
        switch (k) {
        case 'Y': r = snprintf(out + o, cap - o, "%lld", (long long)y); break;
        case 'y': r = snprintf(out + o, cap - o, "%02d", (int)(((y % 100) + 100) % 100)); break;
        case 'm': r = snprintf(out + o, cap - o, "%02u", m); break;
        case 'd': r = snprintf(out + o, cap - o, "%02u", d); break;
        case 'e': r = snprintf(out + o, cap - o, "%2u", d); break;
        case 'H': r = snprintf(out + o, cap - o, "%02u", H); break;
        case 'I': r = snprintf(out + o, cap - o, "%02u", H % 12 ? H % 12 : 12); break;
        case 'p': r = snprintf(out + o, cap - o, "%s", H < 12 ? "AM" : "PM"); break;
        case 'M': r = snprintf(out + o, cap - o, "%02u", M); break;
        case 'S': r = snprintf(out + o, cap - o, "%02u", Sx); break;
        case 'b': r = snprintf(out + o, cap - o, "%.3s", MON[m - 1]); break;
        case 'B': r = snprintf(out + o, cap - o, "%s", MON[m - 1]); break;
        case 'a': r = snprintf(out + o, cap - o, "%.3s", WD[wd]); break;
        case 'A': r = snprintf(out + o, cap - o, "%s", WD[wd]); break;
        case 'j': r = snprintf(out + o, cap - o, "%03lld", (long long)(z - days_from_civil(y, 1, 1) + 1)); break;
        case '%': out[o] = '%'; r = 1; break;
        default: out[o] = '%'; out[o + 1] = k; r = 2; break;
        }
        if (r > 0) o += (size_t)r < cap - o ? (size_t)r : cap - o - 1;
    }
    return o;
}

static constexpr char CSS[] =
    "#mermaid .sec0,#mermaid .sec2{fill:#6666ff;opacity:0.2;}#mermaid .sec1,#mermaid .sec3{fill:#fff400;opacity:0.2;}"
    "#mermaid .stitle{font-size:11px;fill:#333;}#mermaid .grid{stroke:#ddd;stroke-width:1px;}"
    "#mermaid .tick{font-size:10px;fill:#000;}"
    "#mermaid .task{fill:#8a90dd;stroke:#534fbc;}#mermaid .done{fill:#d3d3d3;stroke:grey;}"
    "#mermaid .active{fill:#bfc7ff;stroke:#534fbc;}#mermaid .crit{fill:#ff8888;stroke:#ff0000;}"
    "#mermaid .crit.done{fill:#d3d3d3;stroke:#ff0000;}#mermaid .crit.active{fill:#ff8888;stroke:#ff0000;}"
    "#mermaid .tin{fill:#fff;font-size:11px;}#mermaid .tout{fill:#000;font-size:11px;}"
    "#mermaid .done+.tin,#mermaid .tin.dk{fill:#000;}#mermaid .vert{stroke:#ff8888;stroke-width:2px;stroke-dasharray:4 2;}"
    "#mermaid .today{stroke:red;stroke-width:2px;}#mermaid .ttl{font-size:18px;}";

DIAG_FN(gantt_render) {
    LineIt it = {.s = src, .n = n, .i = off};
    Str line;
    size_t lo;
    if (!dg_next_line(&it, &line, &lo)) return diag_fail(err_off, msg, off, "empty gantt");
    GCtx c = {.a = a, .fmt = S("YYYY-MM-DD")};
    Str axis = S("%Y-%m-%d"), tick = {};
    VEC(Str) secs = {};
    VEC(Task) tv = {};
    VEC(Str) exraw = {};
    (void)fs;
    while (dg_next_line(&it, &line, &lo)) {
        Str rest = line, w = dg_word(&rest);
        rest = str_trim(rest);
        if (str_eq(w, S("title"))) { title = rest; continue; }
        if (str_eq(w, S("dateFormat"))) { c.fmt = rest; continue; }
        if (str_eq(w, S("axisFormat"))) { axis = rest; continue; }
        if (str_eq(w, S("tickInterval"))) { tick = rest; continue; }
        if (str_eq(w, S("inclusiveEndDates"))) { c.inclusive = true; continue; }
        if (str_eq(w, S("excludes"))) {
            for (Str e = rest; e.n;) {
                const char *cm = memchr(e.p, ',', e.n);
                size_t k = cm ? (size_t)(cm - e.p) : e.n;
                Str x = str_trim((Str){.p = e.p, .n = k});
                e = cm ? (Str){.p = cm + 1, .n = e.n - k - 1} : (Str){};
                for (Str ww = x; ww.n;) {
                    Str y = dg_word(&ww);
                    if (!y.n) break;
                    if (str_eq(y, S("weekends"))) c.ex_weekends = true;
                    else if (!VEC_PUSH(a, &exraw, y)) return CMM_ERR_NOMEM;
                }
            }
            continue;
        }
        if (str_eq(w, S("todayMarker")) || str_eq(w, S("weekday")) || str_eq(w, S("weekend")) ||
            str_eq(w, S("click")) || str_eq(w, S("displayMode")) || str_starts(w, S("acc")))
            continue;
        if (str_eq(w, S("section"))) { if (!VEC_PUSH(a, &secs, rest)) return CMM_ERR_NOMEM; continue; }
        const char *col = memchr(line.p, ':', line.n);
        if (!col) return diag_fail(err_off, msg, lo, "expected 'task name : spec'");
        Task t = {.name = str_trim((Str){.p = line.p, .n = (size_t)(col - line.p)}),
                  .sec = secs.n ? (uint32_t)secs.n - 1 : 0};
        Str spec = (Str){.p = col + 1, .n = line.n - (size_t)(col + 1 - line.p)};
        Str parts[3];
        size_t np = 0;
        while (spec.n) {
            const char *cm = memchr(spec.p, ',', spec.n);
            size_t k = cm ? (size_t)(cm - spec.p) : spec.n;
            Str x = str_trim((Str){.p = spec.p, .n = k});
            spec = cm ? (Str){.p = cm + 1, .n = spec.n - k - 1} : (Str){};
            if (str_eq(x, S("done"))) t.tags |= T_DONE;
            else if (str_eq(x, S("active"))) t.tags |= T_ACTIVE;
            else if (str_eq(x, S("crit"))) t.tags |= T_CRIT;
            else if (str_eq(x, S("milestone"))) t.tags |= T_MILE;
            else if (str_eq(x, S("vert"))) t.tags |= T_VERT;
            else if (np < 3) parts[np++] = x;
            else return diag_fail(err_off, msg, (size_t)(x.p - src), "too many task fields");
        }
        if (!np) return diag_fail(err_off, msg, lo, "task needs an end or duration");
        if (np == 1) t.end = parts[0];
        else if (np == 2) t.start = parts[0], t.end = parts[1];
        else t.id = parts[0], t.start = parts[1], t.end = parts[2];
        if (!t.name.n) return diag_fail(err_off, msg, lo, "empty task name");
        if (!VEC_PUSH(a, &tv, t)) return CMM_ERR_NOMEM;
    }
    if (!tv.n) return diag_fail(err_off, msg, off, "gantt has no tasks");
    for (size_t i = 0; i < exraw.n; i++) {
        double t;
        if (parse_date(c.fmt, exraw.v[i], &t) || parse_date(S("YYYY-MM-DD"), exraw.v[i], &t))
            if (!VEC_PUSH(a, &c.exdays, floor(t / DAY))) return CMM_ERR_NOMEM;
    }
    c.t = tv.v, c.nt = tv.n;
    for (size_t i = 0; i < tv.n; i++)
        if (!resolve(&c, i)) return diag_fail(err_off, msg, (size_t)(tv.v[i].name.p - src), "cyclic task dependency");
    double lo_t = 1e300, hi_t = -1e300;
    for (size_t i = 0; i < tv.n; i++) {
        if (tv.v[i].t0 < lo_t) lo_t = tv.v[i].t0;
        if (tv.v[i].t1 > hi_t) hi_t = tv.v[i].t1;
    }
    if (hi_t <= lo_t) hi_t = lo_t + DAY;

    /* geometry (mermaid defaults: barHeight 20, barGap 4, topPadding 50, leftPadding 75) */
    constexpr double BH = 20, GAP = 4, TOP = 50, LEFT = 75, W = 1000;
    double left = LEFT;
    for (size_t i = 0; i < secs.n; i++) {
        double tw = text_width(secs.v[i], 11) + 20;
        if (tw > left) left = tw;
    }
    double plot = W - left - 75, sc = plot / (hi_t - lo_t);
    double H = TOP + tv.n * (BH + GAP) + 40;
    /* ticks */
    static const double STEPS[] = {1000, 5000, 15000, 60000, 300000, 900000, 3600000, 6 * 3600000.0, 12 * 3600000.0,
                                   DAY, 2 * DAY, 7 * DAY, 14 * DAY, 30 * DAY, 91 * DAY, 365 * DAY, 3650 * DAY};
    double step = STEPS[countof(STEPS) - 1];
    bool months = false;
    if (tick.n) {
        Str tk = tick;
        size_t k = 0;
        while (k < tk.n && (unsigned)(tk.p[k] - '0') < 10u) k++;
        double v;
        Str u = {.p = tk.p + k, .n = tk.n - k};
        if (k && dg_parse_num((Str){.p = tk.p, .n = k}, &v) && v > 0) {
            if (str_eq(u, S("millisecond"))) step = v;
            else if (str_eq(u, S("second"))) step = v * 1000;
            else if (str_eq(u, S("minute"))) step = v * 60000;
            else if (str_eq(u, S("hour"))) step = v * 3600000;
            else if (str_eq(u, S("day"))) step = v * DAY;
            else if (str_eq(u, S("week"))) step = v * 7 * DAY;
            else if (str_eq(u, S("month"))) step = v, months = true;
        }
        if (!months && (hi_t - lo_t) / step > 500) step = (hi_t - lo_t) / 500;
    } else {
        for (size_t i = 0; i < countof(STEPS); i++)
            if ((hi_t - lo_t) / STEPS[i] <= 12) { step = STEPS[i]; break; }
    }
    BBox bb = {0, title.n ? 0 : 20, W, H};
    if (!buf_reserve(out, 4096 + tv.n * 700)) return CMM_ERR_NOMEM;
    DG_HEAD(out, bb, 4, "gantt", CSS);
    /* section stripes */
    for (size_t i = 0; i < tv.n;) {
        size_t j = i;
        while (j < tv.n && tv.v[j].sec == tv.v[i].sec) j++;
        char cls[8];
        snprintf(cls, sizeof cls, "sec%u", tv.v[i].sec % 4);
        dg_rect(out, 0, TOP + i * (BH + GAP) - 2, W, (j - i) * (BH + GAP), 0, cls);
        if (secs.n) dg_text(out, secs.v[tv.v[i].sec], 11, 10, TOP + (i + j) * (BH + GAP) / 2.0 - 2, "start", "stitle");
        i = j;
    }
    /* grid + axis labels */
    double gy0 = TOP - 2, gy1 = TOP + tv.n * (BH + GAP) - 2;
    char lab[96];
    for (int k = 0; k < 600; k++) {
        double t = months ? add_months(lo_t, step * k) : lo_t + step * k;
        if (t > hi_t + 1e-6) break;
        double x = left + (t - lo_t) * sc;
        dg_line(out, x, gy0, x, gy1, "grid");
        size_t len = fmt_time(axis, t, sizeof lab, lab);
        dg_text(out, (Str){.p = lab, .n = len}, 10, x, gy1 + 14, "middle", "tick");
    }
    /* tasks */
    for (size_t i = 0; i < tv.n; i++) {
        const Task *t = &tv.v[i];
        double y = TOP + i * (BH + GAP), x0 = left + (t->t0 - lo_t) * sc, x1 = left + (t->t1 - lo_t) * sc;
        char cls[40];
        snprintf(cls, sizeof cls, "task%s%s%s", t->tags & T_CRIT ? " crit" : "", t->tags & T_DONE ? " done" : "",
                 t->tags & T_ACTIVE ? " active" : "");
        if (t->tags & T_VERT) {
            dg_line(out, x0, gy0, x0, gy1, "vert");
            dg_text(out, t->name, 11, x0, gy1 + 28, "middle", "tout");
            continue;
        }
        if (t->tags & T_MILE) {
            double s = BH * 0.75 / 2, cy = y + BH / 2;
            buf_lit(out, "<path class=\""); buf_put(out, strlen(cls), cls); buf_lit(out, "\" d=\"M");
            dg_pt(out, x0, cy - s); buf_ch(out, 'L'); dg_pt(out, x0 + s, cy); buf_ch(out, 'L'); dg_pt(out, x0, cy + s);
            buf_ch(out, 'L'); dg_pt(out, x0 - s, cy); buf_lit(out, "Z\"/>");
            dg_text(out, t->name, 11, x0 + s + 5, cy, "start", "tout");
            continue;
        }
        dg_rect(out, x0, y, x1 - x0, BH, 3, cls);
        double tw = text_width(t->name, 11);
        if (tw + 8 <= x1 - x0) dg_text(out, t->name, 11, (x0 + x1) / 2, y + BH / 2, "middle", t->tags & (T_DONE | T_ACTIVE) ? "tin dk" : "tin");
        else if (x1 + 5 + tw <= W) dg_text(out, t->name, 11, x1 + 5, y + BH / 2, "start", "tout");
        else dg_text(out, t->name, 11, x0 - 5, y + BH / 2, "end", "tout");
    }
    if (title.n) dg_text(out, title, 18, W / 2, 25, "middle", "ttl");
    buf_lit(out, "</svg>\n");
    return out->oom ? CMM_ERR_NOMEM : CMM_OK;
}
