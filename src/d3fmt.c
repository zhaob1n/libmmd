#include "d3fmt.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- JavaScript's Number methods, on x >= 0 ----
 * toFixed, toExponential and toPrecision take the larger of two equally near results, where
 * glibc's printf rounds half to even. printf's expansions are exact at any precision, so take
 * one past the last digit kept and round it half up by hand. */

/* d[0..n) + 1 in decimal, stepping over '.'; false when it carries out of d[0] */
static bool dec_inc(char *d, size_t n) {
    while (n--) {
        if (d[n] == '.') continue;
        if (d[n] != '9') { d[n]++; return true; }
        d[n] = '0';
    }
    return false;
}

/* x > 0 finite: its first p (1..21) significant digits rounded half up into d; returns the decimal
 * exponent of d[0] */
static int sig_digits(double x, int p, char d[static 21]) {
    char t[800]; /* a double has at most 767 significant digits */
    snprintf(t, sizeof t, "%.780e", x);
    int e = atoi(strchr(t, 'e') + 1);
    d[0] = t[0];
    memcpy(d + 1, t + 2, (size_t)p - 1);
    if (t[p + 1] >= '5' && !dec_inc(d, (size_t)p)) d[0] = '1', e++;
    return e;
}

static size_t put_exp(char o[static 8], int e) {
    o[0] = 'e', o[1] = e < 0 ? '-' : '+';
    return 2 + (size_t)snprintf(o + 2, 6, "%d", abs(e));
}

/* x.toExponential(f); f < 0: as many digits as x needs */
static size_t to_exp(double x, int f, char o[static 40]) {
    char d[21];
    int e = 0;
    size_t k;
    if (x == 0) k = f < 0 ? 1 : (size_t)f + 1, memset(d, '0', k);
    else if (f < 0) k = num_shortest(x, d, &e), e--;
    else k = (size_t)f + 1, e = sig_digits(x, f + 1, d);
    size_t n = 0;
    o[n++] = d[0];
    if (k > 1) o[n++] = '.', memcpy(o + n, d + 1, k - 1), n += k - 1;
    return n + put_exp(o + n, e);
}

/* x.toPrecision(p), p in 1..21 */
static size_t to_prec(double x, int p, char o[static 40]) {
    char d[21];
    int e = 0;
    size_t n = 0, P = (size_t)p;
    if (x == 0) memset(d, '0', P);
    else e = sig_digits(x, p, d);
    if (e < -6 || e >= p) {
        o[n++] = d[0];
        if (P > 1) o[n++] = '.', memcpy(o + n, d + 1, P - 1), n += P - 1;
        return n + put_exp(o + n, e);
    }
    if (e < 0) {
        o[n++] = '0', o[n++] = '.';
        memset(o + n, '0', (size_t)(-e - 1)), n += (size_t)(-e - 1);
        memcpy(o + n, d, P);
        return n + P;
    }
    size_t ip = (size_t)e + 1;
    memcpy(o, d, ip);
    if (P == ip) return ip;
    o[ip] = '.';
    memcpy(o + ip + 1, d + ip, P - ip);
    return P + 1;
}

/* x.toFixed(f), f in 0..20 */
static size_t to_fixed(double x, int f, char o[static 48]) {
    if (!(x < 1e21)) return fmt_js(x, o);
    char t[1200]; /* 21 integer digits and at most 1074 fraction digits */
    snprintf(t, sizeof t, "%.1100f", x);
    size_t ip = (size_t)(strchr(t, '.') - t), n = ip + (f ? 1 + (size_t)f : 0);
    o[0] = '1';
    memcpy(o + 1, t, n);
    if (t[ip + 1 + (size_t)f] >= '5' && !dec_inc(o + 1, n)) return n + 1;
    memmove(o, o + 1, n);
    return n;
}

/* Math.round(x).toString(1 << bits), bits 1, 3 or 4 */
static size_t to_radix(double x, unsigned bits, char o[static 1100]) {
    static constexpr char DIG[] = "0123456789abcdef";
    double r = round(x);
    uint64_t m;
    size_t z = 0; /* trailing zero digits */
    if (r < 0x1p64) m = (uint64_t)r;
    else { /* r = m·2^k exactly, k >= 11 */
        int e;
        double fr = frexp(r, &e);
        unsigned k = (unsigned)(e - 53);
        m = (uint64_t)ldexp(fr, 53) << (k % bits), z = k / bits;
    }
    char t[64];
    size_t n = 0;
    do t[n++] = DIG[m & ((1u << bits) - 1)], m >>= bits; while (m);
    for (size_t i = 0; i < n; i++) o[i] = t[n - 1 - i];
    memset(o + n, '0', z);
    return n + z;
}

/* ---- d3-format ---- */

/* formatDecimal ("d"): from 1e21 on, toLocaleString("en") without its commas */
static size_t to_decimal(double x, char o[static 1100]) {
    double r = round(x);
    if (r < 1e21) return fmt_js(r, o);
    char d[17];
    int e;
    size_t k = num_shortest(r, d, &e);
    memcpy(o, d, k);
    memset(o + k, '0', (size_t)e - k);
    return (size_t)e;
}

/* formatDecimalParts: x's digits to p significant ones (0: as many as x needs) and the exponent;
 * false for 0 and Infinity ("p" scales by 100 and can get there) */
static bool dec_parts(double x, int p, char c[static 21], size_t k[static 1], int e[static 1]) {
    if (x == 0 || isinf(x)) return false;
    if (p) *k = (size_t)p, *e = sig_digits(x, p, c);
    else *k = num_shortest(x, c, e), --*e;
    return true;
}

/* formatRounded ("r", and "p" on x·100) */
static size_t to_rounded(double x, int p, char o[static 1100]) {
    char c[21];
    size_t k;
    int e;
    if (!dec_parts(x, p, c, &k, &e)) return fmt_js(x, o);
    if (e < 0) {
        size_t z = (size_t)(-e - 1);
        o[0] = '0', o[1] = '.';
        memset(o + 2, '0', z);
        memcpy(o + 2 + z, c, k);
        return 2 + z + k;
    }
    size_t ip = (size_t)e + 1;
    if (k > ip) {
        memcpy(o, c, ip);
        o[ip] = '.';
        memcpy(o + ip + 1, c + ip, k - ip);
        return k + 1;
    }
    memcpy(o, c, k);
    memset(o + k, '0', ip - k);
    return ip;
}

static const char *const SI[] = {"y", "z", "a", "f", "p", "n", "µ", "m", "", "k", "M", "G", "T", "P", "E", "Z", "Y"};

/* formatPrefixAuto ("s"): x in units of the SI prefix of its exponent, *si that prefix's index
 * (-1: none, for 0) */
static size_t to_si(double x, int p, char o[static 1100], int si[static 1]) {
    char c[21];
    size_t k;
    int e;
    if (!dec_parts(x, p, c, &k, &e)) return *si = -1, to_prec(x, p, o);
    int pe = (int)fmax(-8, fmin(8, floor(e / 3.0))) * 3;
    *si = 8 + pe / 3;
    long i = e - pe + 1, n = (long)k;
    if (i >= n) {
        memcpy(o, c, k);
        memset(o + k, '0', (size_t)(i - n));
        return (size_t)i;
    }
    if (i > 0) {
        memcpy(o, c, (size_t)i);
        o[i] = '.';
        memcpy(o + i + 1, c + i, (size_t)(n - i));
        return k + 1;
    }
    /* below 1y */
    char c2[21];
    size_t k2 = 0;
    int e2;
    (void)dec_parts(x, (int)fmax(0, (double)p + (double)i - 1), c2, &k2, &e2);
    o[0] = '0', o[1] = '.';
    memset(o + 2, '0', (size_t)-i);
    memcpy(o + 2 - i, c2, k2);
    return (size_t)(2 - i) + k2;
}

/* formatTrim: "1.2000" -> "1.2", "1.0e+5" -> "1e+5" */
static size_t trim_zeros(char s[static 1], size_t n) {
    long i0 = -1, i1 = 0;
    for (size_t i = 1; i < n; i++) {
        if (s[i] == '.') i0 = i1 = (long)i;
        else if (s[i] == '0') { if (i0 == 0) i0 = (long)i; i1 = (long)i; }
        else if (s[i] >= '1' && s[i] <= '9') { if (i0 > 0) i0 = 0; }
        else break;
    }
    if (i0 <= 0) return n;
    memmove(s + i0, s + i1 + 1, n - (size_t)i1 - 1);
    return n - (size_t)(i1 + 1 - i0);
}

/* JavaScript's +s === 0 */
static bool js_zero(const char *s, size_t n) {
    char t[64];
    if (n >= sizeof t) return false; /* only nonzero numbers run this long */
    memcpy(t, s, n), t[n] = 0;
    char *e;
    return strtod(t, &e) == 0 && !*e;
}

/* formatGroup([3], ","): from the right, stopping once width is used */
static size_t group(const char *v, size_t n, long width, char o[static 1]) {
    size_t cut[1024], nc = 0; /* chunk starts, right to left */
    long i = (long)n, len = 0;
    while (i > 0 && nc < countof(cut)) {
        long g = len + 4 > width ? (width - len > 1 ? width - len : 1) : 3;
        i -= g;
        cut[nc++] = i > 0 ? (size_t)i : 0;
        if ((len += g + 1) > width) break;
    }
    size_t m = 0; /* anything left of the last chunk is dropped */
    for (size_t j = nc; j-- > 0;) {
        size_t s = cut[j], e = j ? cut[j - 1] : n;
        memcpy(o + m, v + s, e - s), m += e - s;
        if (j) o[m++] = ',';
    }
    return m;
}

static size_t cps(const char *s, size_t n) { /* JavaScript's length: every character here is in the BMP */
    size_t c = 0;
    for (size_t i = 0; i < n; i++) c += ((unsigned char)s[i] & 0xC0) != 0x80;
    return c;
}

static bool after_align(Str s, size_t i, D3Fmt f[static 1]);

bool d3fmt_parse(Str s, D3Fmt f[static 1]) {
    /* (?:(.)?([<>=^]))? : the first reading that lets the rest match */
    size_t cl = !s.n ? 0 : (unsigned char)s.p[0] < 0xC0 ? 1 : (unsigned char)s.p[0] < 0xE0 ? 2 : (unsigned char)s.p[0] < 0xF0 ? 3 : 4;
    static constexpr D3Fmt DEF = {.fill = " ", .fill_n = 1, .align = '>', .sign = '-', .width = -1, .precision = -1};
    *f = DEF;
    if (cl < 4 && cl < s.n && memchr("<>=^", s.p[cl], 4)) {
        memcpy(f->fill, s.p, cl), f->fill_n = (uint8_t)cl, f->align = s.p[cl];
        if (after_align(s, cl + 1, f)) return true;
        *f = DEF;
    }
    if (s.n && memchr("<>=^", s.p[0], 4)) {
        f->align = s.p[0];
        if (after_align(s, 1, f)) return true;
        *f = DEF;
    }
    return after_align(s, 0, f);
}

static int digits(Str s, size_t i[static 1], int cap) {
    int v = 0;
    while (*i < s.n && (unsigned)(s.p[*i] - '0') < 10u)
        if ((v = v * 10 + (s.p[(*i)++] - '0')) > cap) v = cap;
    return v;
}

/* the rest after fill and align, then newFormat's setup */
static bool after_align(Str s, size_t i, D3Fmt f[static 1]) {
    if (i < s.n && memchr("+-( ", s.p[i], 4)) f->sign = s.p[i++];
    if (i < s.n && (s.p[i] == '$' || s.p[i] == '#')) f->symbol = s.p[i++];
    if (i < s.n && s.p[i] == '0') f->zero = true, i++;
    if (i < s.n && (unsigned)(s.p[i] - '0') < 10u) f->width = digits(s, &i, 1000); /* d3: no limit */
    if (i < s.n && s.p[i] == ',') f->comma = true, i++;
    if (i + 1 < s.n && s.p[i] == '.' && (unsigned)(s.p[i + 1] - '0') < 10u) i++, f->precision = digits(s, &i, 100);
    if (i < s.n && s.p[i] == '~') f->trim = true, i++;
    if (i < s.n && (s.p[i] == '%' || (unsigned)((s.p[i] | 32) - 'a') < 26u)) f->type = s.p[i++];
    if (i != s.n) return false;
    if (f->type == 'n') f->comma = true, f->type = 'g';
    else if (!f->type || !strchr("%bcdefgoprsXx", f->type)) { /* "" and unknown types: ".12~g" */
        if (f->precision < 0) f->precision = 12;
        f->trim = true, f->type = 'g';
    }
    if (f->zero || (f->fill_n == 1 && f->fill[0] == '0' && f->align == '=')) f->zero = true, f->fill_n = 1, f->fill[0] = '0', f->align = '=';
    bool sig = strchr("gprs", f->type);
    f->precision = f->precision < 0 ? 6 : sig ? (int)fmax(1, fmin(21, f->precision)) : (int)fmin(20, f->precision);
    return true;
}

void d3fmt(Buf b[static 1], const D3Fmt f[static 1], double v) {
    char vs[2200], suf[1200], gs[3000], pre[8]; /* vs: the digits, and up to 1000 zeros of padding */
    size_t vn = 0, sn = 0, pn = 0;
    char ty = f->type;
    const char *sym = f->symbol == '$' ? "$" : f->symbol != '#' || !strchr("boxX", ty) ? "" : ty == 'b' ? "0b" : ty == 'o' ? "0o" : "0x";
    const char *tail = f->symbol != '$' && (ty == '%' || ty == 'p') ? "%" : "";
    if (ty == 'c') {
        sn = fmt_js(v, suf);
        pn = strlen(sym), memcpy(pre, sym, pn);
    } else {
        bool neg = signbit(v) && !isnan(v); /* -0 too */
        double x = fabs(v);
        int si = -1;
        if (isnan(v)) memcpy(vs, "NaN", vn = 3);
        else if (isinf(x) && ty == 'd') memcpy(vs, "∞", vn = 3); /* toLocaleString */
        else if (isinf(x)) memcpy(vs, ty == 'X' ? "INFINITY" : "Infinity", vn = 8);
        else switch (ty) {
        case '%': vn = to_fixed(x * 100, f->precision, vs); break;
        case 'b': vn = to_radix(x, 1, vs); break;
        case 'o': vn = to_radix(x, 3, vs); break;
        case 'x': vn = to_radix(x, 4, vs); break;
        case 'X':
            vn = to_radix(x, 4, vs);
            for (size_t i = 0; i < vn; i++) vs[i] -= vs[i] >= 'a' ? 'a' - 'A' : 0;
            break;
        case 'd': vn = to_decimal(x, vs); break;
        case 'e': vn = to_exp(x, f->precision, vs); break;
        case 'f': vn = to_fixed(x, f->precision, vs); break;
        case 'p': vn = to_rounded(x * 100, f->precision, vs); break;
        case 'r': vn = to_rounded(x, f->precision, vs); break;
        case 's': vn = to_si(x, f->precision, vs, &si); break;
        default: vn = to_prec(x, f->precision, vs); break; /* g */
        }
        if (f->trim) vn = trim_zeros(vs, vn);
        if (neg && js_zero(vs, vn) && f->sign != '+') neg = false;
        if (neg) pn = f->sign == '(' ? (pre[0] = '(', 1) : (memcpy(pre, "−", 3), 3);
        else if (f->sign != '-' && f->sign != '(') pre[pn++] = f->sign;
        memcpy(pre + pn, sym, strlen(sym)), pn += strlen(sym);
        size_t cut = vn;
        if (strchr("defgprs%", ty))
            for (size_t i = 0; i < vn; i++) if ((unsigned)(vs[i] - '0') >= 10u) { cut = i; break; }
        memcpy(suf, vs + cut, vn - cut), sn = vn - cut, vn = cut;
        if (ty == 's' && si >= 0) memcpy(suf + sn, SI[si], strlen(SI[si])), sn += strlen(SI[si]);
        memcpy(suf + sn, tail, strlen(tail)), sn += strlen(tail);
        if (neg && f->sign == '(') suf[sn++] = ')';
    }
    const char *val = vs;
    if (f->comma && !f->zero) vn = group(vs, vn, LONG_MAX, gs), val = gs;
    size_t len = cps(pre, pn) + cps(val, vn) + cps(suf, sn);
    size_t pad = f->width >= 0 && len < (size_t)f->width ? (size_t)f->width - len : 0;
    if (f->comma && f->zero) { /* fill is "0": grouped with the digits */
        memmove(vs + pad, vs, vn); /* vs still holds the digits: !zero grouping did not run */
        memset(vs, '0', pad);
        vn = group(vs, pad + vn, pad ? f->width - (long)cps(suf, sn) : LONG_MAX, gs), val = gs, pad = 0;
    }
    size_t lpad = f->align == '<' ? 0 : f->align == '^' ? pad >> 1 : pad, rpad = pad - lpad;
    if (f->align == '=') {
        buf_put(b, pn, pre);
        for (size_t i = 0; i < pad; i++) buf_put(b, f->fill_n, f->fill);
    } else {
        for (size_t i = 0; i < lpad; i++) buf_put(b, f->fill_n, f->fill);
        buf_put(b, pn, pre);
    }
    buf_put(b, vn, val);
    buf_put(b, sn, suf);
    if (f->align != '=')
        for (size_t i = 0; i < rpad; i++) buf_put(b, f->fill_n, f->fill);
}
