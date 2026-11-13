/* mcolor.c — khroma 2.1.0's color engine, ported to the subset mermaid 12's themes use.
 * See mcolor.h for the contract.  The port keeps khroma's data model: a color carries
 * whichever channels were parsed plus whichever were derived lazily, and a "type" that
 * records whether an RGB or an HSL setter ran, because stringify picks its output format
 * from that type. */
#include "mcolor.h"

#include <math.h>
#include <string.h>

typedef struct MChannels {
    bool has_r, has_g, has_b, has_h, has_s, has_l;
    double r, g, b, h, s, l, a;
    bool changed;
    Str color;
    unsigned char type; /* 0 all, 1 rgb, 2 hsl */
} MChannels;

#define TYPE_ALL 0
#define TYPE_RGB 1
#define TYPE_HSL 2

#define PI 3.14159265358979323846

/* ---- JavaScript number semantics ---- */

/* Math.round: nearest integer, ties toward +infinity, -0 preserved, NaN/infinite as is.
 * floor(x + 0.5) would be wrong for the double just below .5. */
static double js_round(double x) {
    if (isnan(x) || isinf(x) || x == 0) return x;
    double f = floor(x), d = x - f;
    return d < 0.5 ? f : f + 1;
}

static double js_max3(double a, double b, double c) {
    if (isnan(a) || isnan(b) || isnan(c)) return NAN;
    return fmax(a, fmax(b, c));
}

static double js_min3(double a, double b, double c) {
    if (isnan(a) || isnan(b) || isnan(c)) return NAN;
    return fmin(a, fmin(b, c));
}
/* khroma's _.lang.round: Math.round(x * 1e10) / 1e10 */
static double lang_round(double x) { return js_round(x * 1e10) / 1e10; }

static double clamp_255(double v) { return v >= 255 ? 255 : (v < 0 ? 0 : v); }
static double clamp_100(double v) { return v >= 100 ? 100 : (v < 0 ? 0 : v); }
static double clamp_h(double v) { return fmod(v, 360.0); }
static double clamp_a(double v) { return v >= 1 ? 1 : (v < 0 ? 0 : v); }

static double to_linear(double c) {
    double n = c / 255.0;
    return c > 0.03928 ? pow((n + 0.055) / 1.055, 2.4) : n / 12.92;
}

static double hue2rgb(double p, double q, double t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0 / 6.0) return p + (q - p) * 6 * t;
    if (t < 1.0 / 2.0) return q;
    if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6;
    return p;
}

/* ch: 0 r, 1 g, 2 b */
static double hsl2rgb_one(double h, double s, double l, int ch) {
    if (s == 0 || isnan(s)) return l * 2.55; /* achromatic; !s in JS also catches NaN */
    h /= 360.0;
    s /= 100.0;
    l /= 100.0;
    double q = (l < 0.5) ? l * (1 + s) : (l + s) - (l * s);
    double p = 2 * l - q;
    switch (ch) {
    case 0: return hue2rgb(p, q, h + 1.0 / 3.0) * 255.0;
    case 1: return hue2rgb(p, q, h) * 255.0;
    default: return hue2rgb(p, q, h - 1.0 / 3.0) * 255.0;
    }
}

/* ch: 0 h, 1 s, 2 l */
static double rgb2hsl_one(double r, double g, double b, int ch) {
    r /= 255.0;
    g /= 255.0;
    b /= 255.0;
    double max = js_max3(r, g, b), min = js_min3(r, g, b);
    double l = (max + min) / 2;
    if (ch == 2) return l * 100.0;
    if (max == min) return 0;
    double d = max - min;
    double s = (l > 0.5) ? d / (2 - max - min) : d / (max + min);
    if (ch == 1) return s * 100.0;
    if (isnan(max)) return -1; /* khroma's switch default */
    if (max == r) return ((g - b) / d + (g < b ? 6 : 0)) * 60.0;
    if (max == g) return ((b - r) / d + 2) * 60.0;
    return ((r - g) / d + 4) * 60.0;
}

/* ---- the Channels object ---- */

static void ensure_hsl(MChannels c[static 1]) {
    if (!c->has_h) c->h = rgb2hsl_one(c->r, c->g, c->b, 0), c->has_h = true;
    if (!c->has_s) c->s = rgb2hsl_one(c->r, c->g, c->b, 1), c->has_s = true;
    if (!c->has_l) c->l = rgb2hsl_one(c->r, c->g, c->b, 2), c->has_l = true;
}

static void ensure_rgb(MChannels c[static 1]) {
    if (!c->has_r) c->r = hsl2rgb_one(c->h, c->s, c->l, 0), c->has_r = true;
    if (!c->has_g) c->g = hsl2rgb_one(c->h, c->s, c->l, 1), c->has_g = true;
    if (!c->has_b) c->b = hsl2rgb_one(c->h, c->s, c->l, 2), c->has_b = true;
}
static double get_r(MChannels c[static 1]) {
    if (c->type != TYPE_HSL && c->has_r) return c->r;
    ensure_hsl(c);
    return hsl2rgb_one(c->h, c->s, c->l, 0);
}
static double get_g(MChannels c[static 1]) {
    if (c->type != TYPE_HSL && c->has_g) return c->g;
    ensure_hsl(c);
    return hsl2rgb_one(c->h, c->s, c->l, 1);
}
static double get_b(MChannels c[static 1]) {
    if (c->type != TYPE_HSL && c->has_b) return c->b;
    ensure_hsl(c);
    return hsl2rgb_one(c->h, c->s, c->l, 2);
}
static double get_h(MChannels c[static 1]) {
    if (c->type != TYPE_RGB && c->has_h) return c->h;
    ensure_rgb(c);
    return rgb2hsl_one(c->r, c->g, c->b, 0);
}
static double get_s(MChannels c[static 1]) {
    if (c->type != TYPE_RGB && c->has_s) return c->s;
    ensure_rgb(c);
    return rgb2hsl_one(c->r, c->g, c->b, 1);
}
static double get_l(MChannels c[static 1]) {
    if (c->type != TYPE_RGB && c->has_l) return c->l;
    ensure_rgb(c);
    return rgb2hsl_one(c->r, c->g, c->b, 2);
}

static void set_r(MChannels c[static 1], double v) { c->type = TYPE_RGB, c->changed = true, c->r = v, c->has_r = true; }
static void set_g(MChannels c[static 1], double v) { c->type = TYPE_RGB, c->changed = true, c->g = v, c->has_g = true; }
static void set_b(MChannels c[static 1], double v) { c->type = TYPE_RGB, c->changed = true, c->b = v, c->has_b = true; }
static void set_h(MChannels c[static 1], double v) { c->type = TYPE_HSL, c->changed = true, c->h = v, c->has_h = true; }
static void set_s(MChannels c[static 1], double v) { c->type = TYPE_HSL, c->changed = true, c->s = v, c->has_s = true; }
static void set_l(MChannels c[static 1], double v) { c->type = TYPE_HSL, c->changed = true, c->l = v, c->has_l = true; }

/* ---- parsing ---- */

static bool is_digit(char c) { return (unsigned)(c - '0') < 10u; }
static bool is_hex(char c) { return is_digit(c) || (unsigned)((c | 32) - 'a') < 6u; }
static bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
static size_t skip_ws(Str s, size_t i) {
    while (i < s.n && is_ws(s.p[i])) i++;
    return i;
}

static double c_strtod(Str s) {
    char buf[64];
    size_t n = s.n < sizeof buf - 1 ? s.n : sizeof buf - 1;
    if (n) memcpy(buf, s.p, n);
    buf[n] = 0;
    locale_t host = c_locale_enter();
    double v = strtod(buf, nullptr);
    uselocale(host);
    return v;
}

/* parseFloat: the longest numeric prefix, NaN when there is none.  Tokens reaching this
 * have already matched khroma's number grammar, so no hex or Infinity forms appear. */
static double parse_float(Str s) { return c_strtod(s); }

/* JavaScript's Number(s) for a whole string: optional surrounding whitespace, a sign,
 * then a decimal literal, a hex integer or Infinity.  False for anything else (so
 * "16px", "abc" and "NaN" stay strings, as they are in YAML). */
static size_t scan_js_number(Str s, size_t i, double out[static 1]) {
    size_t start = i;
    if (i < s.n && (s.p[i] == '+' || s.p[i] == '-')) i++;
    if (i + 8 <= s.n && memcmp(s.p + i, "Infinity", 8) == 0) {
        *out = s.p[start] == '-' ? -INFINITY : INFINITY;
        return i + 8;
    }
    if (i + 2 <= s.n && s.p[i] == '0' && (s.p[i + 1] | 32) == 'x') {
        size_t h0 = i + 2, h = h0;
        while (h < s.n && is_hex(s.p[h])) h++;
        if (h > h0) {
            double v = 0;
            for (size_t k = h0; k < h; k++)
                v = v * 16 + (double)(is_digit(s.p[k]) ? s.p[k] - '0' : (s.p[k] | 32) - 'a' + 10);
            *out = s.p[start] == '-' ? -v : v;
            return h;
        }
        return 0;
    }
    size_t d0 = i;
    while (i < s.n && is_digit(s.p[i])) i++;
    bool has_int = i > d0, has_frac = false;
    if (i < s.n && s.p[i] == '.') {
        i++;
        size_t f0 = i;
        while (i < s.n && is_digit(s.p[i])) i++;
        if (i > f0) has_frac = true;
        else i--; /* "1." is 1; "." alone is not a number */
    }
    if (!has_int && !has_frac) return 0;
    if (i < s.n && (s.p[i] == 'e' || s.p[i] == 'E')) {
        size_t e0 = i;
        i++;
        if (i < s.n && (s.p[i] == '+' || s.p[i] == '-')) i++;
        size_t d1 = i;
        while (i < s.n && is_digit(s.p[i])) i++;
        if (i == d1) i = e0;
    }
    *out = c_strtod(str_sub(s, start, i));
    return i;
}

bool mc_js_number(Str s, double out[static 1]) {
    size_t i = 0, j = s.n;
    while (i < j && is_ws(s.p[i])) i++;
    while (j > i && is_ws(s.p[j - 1])) j--;
    if (i == j) { *out = 0; return true; } /* Number("") === 0 */
    Str t = {.p = s.p, .n = j};
    return scan_js_number(t, i, out) == j;
}

/* -?(\d+(\.\d+)?|\.\d+)(e-?\d+)?, the shared number grammar; exp_sign allows e-1 */
static size_t scan_num(Str s, size_t i, bool exp_sign, double out[static 1]) {
    size_t start = i;
    if (i < s.n && s.p[i] == '-') i++;
    size_t d0 = i;
    while (i < s.n && is_digit(s.p[i])) i++;
    bool has_int = i > d0;
    if (i < s.n && s.p[i] == '.') {
        size_t dot = i;
        i++;
        size_t f0 = i;
        while (i < s.n && is_digit(s.p[i])) i++;
        if (i == f0) i = dot; /* "1." leaves the dot to the caller, which then fails */
    }
    if (!has_int && i == d0) return 0;
    if (i < s.n && (s.p[i] == 'e' || s.p[i] == 'E')) {
        size_t e0 = i;
        i++;
        if (exp_sign && i < s.n && s.p[i] == '-') i++;
        size_t d1 = i;
        while (i < s.n && is_digit(s.p[i])) i++;
        if (i == d1) i = e0;
    }
    *out = c_strtod(str_sub(s, start, i));
    return i;
}

/* \s*? then (?:,|\s) then \s*? — at least one separator char, commas or whitespace */
static size_t scan_sep(Str s, size_t i) {
    size_t w = skip_ws(s, i);
    if (w < s.n && s.p[w] == ',') return skip_ws(s, w + 1);
    return w > i ? w : 0;
}

/* the hue unit, case-insensitive: deg|grad|rad|turn */
static size_t match_unit(Str s, size_t i) {
    static constexpr char U[][5] = {"deg", "grad", "rad", "turn"};
    for (size_t u = 0; u < 4; u++) {
        size_t ul = strlen(U[u]);
        if (i + ul <= s.n) {
            bool eq = true;
            for (size_t k = 0; k < ul; k++)
                if ((s.p[i + k] | 32) != (U[u][k] | 32)) { eq = false; break; }
            if (eq) return i + ul;
        }
    }
    return i;
}

/* /^(.+?)(deg|grad|rad|turn)$/i, then khroma's case-sensitive switch on the unit */
static double hue2deg(Str hue) {
    static constexpr char U[][5] = {"deg", "grad", "rad", "turn"};
    for (size_t u = 0; u < 4; u++) {
        size_t ul = strlen(U[u]);
        if (hue.n >= ul) {
            bool eq = true;
            for (size_t k = 0; k < ul; k++)
                if ((hue.p[hue.n - ul + k] | 32) != (U[u][k] | 32)) { eq = false; break; }
            if (eq) {
                Str unit = str_sub(hue, hue.n - ul, hue.n);
                Str num = str_sub(hue, 0, hue.n - ul);
                if (u == 1 && str_eq(unit, S("grad"))) return clamp_h(parse_float(num) * 0.9);
                if (u == 2 && str_eq(unit, S("rad"))) return clamp_h(parse_float(num) * 180.0 / PI);
                if (u == 3 && str_eq(unit, S("turn"))) return clamp_h(parse_float(num) * 360.0);
                break;
            }
        }
    }
    return clamp_h(parse_float(hue));
}

static bool parse_hex(Str s, MChannels c[static 1]) {
    if (s.n < 2 || s.p[0] != '#') return false;
    size_t len = s.n - 1;
    if (len != 3 && len != 4 && len != 6 && len != 8) return false;
    uint32_t dec = 0;
    for (size_t i = 1; i < s.n; i++) {
        char ch = s.p[i];
        if (!is_hex(ch)) return false;
        uint32_t v = (uint32_t)(is_digit(ch) ? ch - '0' : (ch | 32) - 'a' + 10);
        dec = dec * 16 + v;
    }
    bool has_alpha = len % 4 == 0;
    bool full = len > 4;
    int mult = full ? 1 : 17, bits = full ? 8 : 4, off = has_alpha ? 0 : -1, mask = full ? 255 : 15;
    c->has_r = c->has_g = c->has_b = true;
    c->r = (double)((dec >> (bits * (off + 3))) & mask) * mult;
    c->g = (double)((dec >> (bits * (off + 2))) & mask) * mult;
    c->b = (double)((dec >> (bits * (off + 1))) & mask) * mult;
    c->a = has_alpha ? (double)(dec & mask) * mult / 255.0 : 1.0;
    c->changed = false;
    c->color = s;
    c->type = TYPE_ALL;
    c->has_h = c->has_s = c->has_l = false;
    return true;
}

static bool parse_rgb(Str s, MChannels c[static 1]) {
    if (s.n < 5 || (s.p[0] | 32) != 'r' || (s.p[1] | 32) != 'g' || (s.p[2] | 32) != 'b') return false;
    size_t i = (s.p[3] | 32) == 'a' ? 4 : 3;
    if (i >= s.n || s.p[i] != '(') return false;
    double r, g, b, a = 1;
    i = skip_ws(s, i + 1);
    size_t e = scan_num(s, i, false, &r);
    if (!e) return false;
    i = e;
    if (i < s.n && s.p[i] == '%') r *= 2.55, i++;
    size_t j = scan_sep(s, i);
    if (!j) return false;
    i = j;
    e = scan_num(s, i, false, &g);
    if (!e) return false;
    i = e;
    if (i < s.n && s.p[i] == '%') g *= 2.55, i++;
    j = scan_sep(s, i);
    if (!j) return false;
    i = j;
    e = scan_num(s, i, false, &b);
    if (!e) return false;
    i = e;
    if (i < s.n && s.p[i] == '%') b *= 2.55, i++;
    i = skip_ws(s, i);
    if (i < s.n && (s.p[i] == ',' || s.p[i] == '/')) {
        size_t k = skip_ws(s, i + 1);
        if (k < s.n && s.p[k] == '+') k++;
        e = scan_num(s, k, false, &a);
        if (!e) return false;
        i = e;
        if (i < s.n && s.p[i] == '%') a /= 100.0, i++;
    }
    i = skip_ws(s, i);
    if (i >= s.n || s.p[i] != ')') return false;
    if (i + 1 != s.n) return false;
    c->has_r = c->has_g = c->has_b = true;
    c->r = clamp_255(r);
    c->g = clamp_255(g);
    c->b = clamp_255(b);
    c->a = clamp_a(a);
    c->changed = false;
    c->color = s;
    c->type = TYPE_ALL;
    c->has_h = c->has_s = c->has_l = false;
    return true;
}

static bool parse_hsl(Str s, MChannels c[static 1]) {
    if (s.n < 5 || (s.p[0] | 32) != 'h' || (s.p[1] | 32) != 's' || (s.p[2] | 32) != 'l') return false;
    size_t i = (s.p[3] | 32) == 'a' ? 4 : 3;
    if (i >= s.n || s.p[i] != '(') return false;
    i = skip_ws(s, i + 1);
    size_t hstart = i;
    double h, sv, lv, a = 1;
    size_t e = scan_num(s, i, true, &h);
    if (!e) return false;
    size_t huend = match_unit(s, e);
    double hdeg = hue2deg(str_sub(s, hstart, huend));
    i = huend;
    size_t j = scan_sep(s, i);
    if (!j) return false;
    i = j;
    e = scan_num(s, i, true, &sv);
    if (!e) return false;
    if (e >= s.n || s.p[e] != '%') return false;
    i = e + 1;
    j = scan_sep(s, i);
    if (!j) return false;
    i = j;
    e = scan_num(s, i, true, &lv);
    if (!e) return false;
    if (e >= s.n || s.p[e] != '%') return false;
    i = e + 1;
    i = skip_ws(s, i);
    if (i < s.n && (s.p[i] == ',' || s.p[i] == '/')) {
        size_t k = skip_ws(s, i + 1);
        if (k < s.n && s.p[k] == '+') k++;
        e = scan_num(s, k, true, &a);
        if (!e) return false;
        i = e;
        if (i < s.n && s.p[i] == '%') a /= 100.0, i++;
    }
    i = skip_ws(s, i);
    if (i >= s.n || s.p[i] != ')') return false;
    if (i + 1 != s.n) return false;
    c->has_h = c->has_s = c->has_l = true;
    c->h = hdeg;
    c->s = clamp_100(sv);
    c->l = clamp_100(lv);
    c->a = clamp_a(a);
    c->changed = false;
    c->color = s;
    c->type = TYPE_ALL;
    c->has_r = c->has_g = c->has_b = false;
    return true;
}

static const struct { Str name, hex; } KEYWORDS[] = {
    {S("aliceblue"), S("#f0f8ff")},
    {S("antiquewhite"), S("#faebd7")},
    {S("aqua"), S("#00ffff")},
    {S("aquamarine"), S("#7fffd4")},
    {S("azure"), S("#f0ffff")},
    {S("beige"), S("#f5f5dc")},
    {S("bisque"), S("#ffe4c4")},
    {S("black"), S("#000000")},
    {S("blanchedalmond"), S("#ffebcd")},
    {S("blue"), S("#0000ff")},
    {S("blueviolet"), S("#8a2be2")},
    {S("brown"), S("#a52a2a")},
    {S("burlywood"), S("#deb887")},
    {S("cadetblue"), S("#5f9ea0")},
    {S("chartreuse"), S("#7fff00")},
    {S("chocolate"), S("#d2691e")},
    {S("coral"), S("#ff7f50")},
    {S("cornflowerblue"), S("#6495ed")},
    {S("cornsilk"), S("#fff8dc")},
    {S("crimson"), S("#dc143c")},
    {S("cyanaqua"), S("#00ffff")},
    {S("darkblue"), S("#00008b")},
    {S("darkcyan"), S("#008b8b")},
    {S("darkgoldenrod"), S("#b8860b")},
    {S("darkgray"), S("#a9a9a9")},
    {S("darkgreen"), S("#006400")},
    {S("darkgrey"), S("#a9a9a9")},
    {S("darkkhaki"), S("#bdb76b")},
    {S("darkmagenta"), S("#8b008b")},
    {S("darkolivegreen"), S("#556b2f")},
    {S("darkorange"), S("#ff8c00")},
    {S("darkorchid"), S("#9932cc")},
    {S("darkred"), S("#8b0000")},
    {S("darksalmon"), S("#e9967a")},
    {S("darkseagreen"), S("#8fbc8f")},
    {S("darkslateblue"), S("#483d8b")},
    {S("darkslategray"), S("#2f4f4f")},
    {S("darkslategrey"), S("#2f4f4f")},
    {S("darkturquoise"), S("#00ced1")},
    {S("darkviolet"), S("#9400d3")},
    {S("deeppink"), S("#ff1493")},
    {S("deepskyblue"), S("#00bfff")},
    {S("dimgray"), S("#696969")},
    {S("dimgrey"), S("#696969")},
    {S("dodgerblue"), S("#1e90ff")},
    {S("firebrick"), S("#b22222")},
    {S("floralwhite"), S("#fffaf0")},
    {S("forestgreen"), S("#228b22")},
    {S("fuchsia"), S("#ff00ff")},
    {S("gainsboro"), S("#dcdcdc")},
    {S("ghostwhite"), S("#f8f8ff")},
    {S("gold"), S("#ffd700")},
    {S("goldenrod"), S("#daa520")},
    {S("gray"), S("#808080")},
    {S("green"), S("#008000")},
    {S("greenyellow"), S("#adff2f")},
    {S("grey"), S("#808080")},
    {S("honeydew"), S("#f0fff0")},
    {S("hotpink"), S("#ff69b4")},
    {S("indianred"), S("#cd5c5c")},
    {S("indigo"), S("#4b0082")},
    {S("ivory"), S("#fffff0")},
    {S("khaki"), S("#f0e68c")},
    {S("lavender"), S("#e6e6fa")},
    {S("lavenderblush"), S("#fff0f5")},
    {S("lawngreen"), S("#7cfc00")},
    {S("lemonchiffon"), S("#fffacd")},
    {S("lightblue"), S("#add8e6")},
    {S("lightcoral"), S("#f08080")},
    {S("lightcyan"), S("#e0ffff")},
    {S("lightgoldenrodyellow"), S("#fafad2")},
    {S("lightgray"), S("#d3d3d3")},
    {S("lightgreen"), S("#90ee90")},
    {S("lightgrey"), S("#d3d3d3")},
    {S("lightpink"), S("#ffb6c1")},
    {S("lightsalmon"), S("#ffa07a")},
    {S("lightseagreen"), S("#20b2aa")},
    {S("lightskyblue"), S("#87cefa")},
    {S("lightslategray"), S("#778899")},
    {S("lightslategrey"), S("#778899")},
    {S("lightsteelblue"), S("#b0c4de")},
    {S("lightyellow"), S("#ffffe0")},
    {S("lime"), S("#00ff00")},
    {S("limegreen"), S("#32cd32")},
    {S("linen"), S("#faf0e6")},
    {S("magenta"), S("#ff00ff")},
    {S("maroon"), S("#800000")},
    {S("mediumaquamarine"), S("#66cdaa")},
    {S("mediumblue"), S("#0000cd")},
    {S("mediumorchid"), S("#ba55d3")},
    {S("mediumpurple"), S("#9370db")},
    {S("mediumseagreen"), S("#3cb371")},
    {S("mediumslateblue"), S("#7b68ee")},
    {S("mediumspringgreen"), S("#00fa9a")},
    {S("mediumturquoise"), S("#48d1cc")},
    {S("mediumvioletred"), S("#c71585")},
    {S("midnightblue"), S("#191970")},
    {S("mintcream"), S("#f5fffa")},
    {S("mistyrose"), S("#ffe4e1")},
    {S("moccasin"), S("#ffe4b5")},
    {S("navajowhite"), S("#ffdead")},
    {S("navy"), S("#000080")},
    {S("oldlace"), S("#fdf5e6")},
    {S("olive"), S("#808000")},
    {S("olivedrab"), S("#6b8e23")},
    {S("orange"), S("#ffa500")},
    {S("orangered"), S("#ff4500")},
    {S("orchid"), S("#da70d6")},
    {S("palegoldenrod"), S("#eee8aa")},
    {S("palegreen"), S("#98fb98")},
    {S("paleturquoise"), S("#afeeee")},
    {S("palevioletred"), S("#db7093")},
    {S("papayawhip"), S("#ffefd5")},
    {S("peachpuff"), S("#ffdab9")},
    {S("peru"), S("#cd853f")},
    {S("pink"), S("#ffc0cb")},
    {S("plum"), S("#dda0dd")},
    {S("powderblue"), S("#b0e0e6")},
    {S("purple"), S("#800080")},
    {S("rebeccapurple"), S("#663399")},
    {S("red"), S("#ff0000")},
    {S("rosybrown"), S("#bc8f8f")},
    {S("royalblue"), S("#4169e1")},
    {S("saddlebrown"), S("#8b4513")},
    {S("salmon"), S("#fa8072")},
    {S("sandybrown"), S("#f4a460")},
    {S("seagreen"), S("#2e8b57")},
    {S("seashell"), S("#fff5ee")},
    {S("sienna"), S("#a0522d")},
    {S("silver"), S("#c0c0c0")},
    {S("skyblue"), S("#87ceeb")},
    {S("slateblue"), S("#6a5acd")},
    {S("slategray"), S("#708090")},
    {S("slategrey"), S("#708090")},
    {S("snow"), S("#fffafa")},
    {S("springgreen"), S("#00ff7f")},
    {S("tan"), S("#d2b48c")},
    {S("teal"), S("#008080")},
    {S("thistle"), S("#d8bfd8")},
    {S("transparent"), S("#00000000")},
    {S("turquoise"), S("#40e0d0")},
    {S("violet"), S("#ee82ee")},
    {S("wheat"), S("#f5deb3")},
    {S("white"), S("#ffffff")},
    {S("whitesmoke"), S("#f5f5f5")},
    {S("yellow"), S("#ffff00")},
    {S("yellowgreen"), S("#9acd32")},
};

static bool parse_keyword(Str s, MChannels c[static 1]) {
    /* Keyword.parse lowercases before the lookup; toLowerCase is ASCII here */
    char low[32];
    if (s.n >= sizeof low) return false;
    for (size_t i = 0; i < s.n; i++) low[i] = (char)ascii_lower(s.p[i]);
    Str ls = {.p = low, .n = s.n};
    for (size_t i = 0; i < countof(KEYWORDS); i++)
        if (str_eq(ls, KEYWORDS[i].name)) return parse_hex(KEYWORDS[i].hex, c);
    return false;
}

static bool mc_parse(Str s, MChannels c[static 1]) {
    return parse_hex(s, c) || parse_rgb(s, c) || parse_hsl(s, c) || parse_keyword(s, c);
}

/* ---- stringifying ---- */

static Str num_str(Arena a[static 1], double v) {
    char buf[32];
    size_t n = fmt_js(v, buf);
    return arena_strdup(a, (Str){.p = buf, .n = n});
}

static void dec2hex(double v, char out[static 2]) {
    int i = (int)js_round(v);
    static constexpr char H[] = "0123456789abcdef";
    out[0] = H[(i >> 4) & 15];
    out[1] = H[i & 15];
}

static bool is_int(double x) { return !isnan(x) && !isinf(x) && floor(x) == x; }

static Str stringify_hsl(Arena a[static 1], MChannels c[static 1]) {
    Str h = num_str(a, lang_round(get_h(c)));
    Str s = num_str(a, lang_round(get_s(c)));
    Str l = num_str(a, lang_round(get_l(c)));
    if (c->a < 1) {
        Str al = num_str(a, c->a);
        return STR_CAT(a, S("hsla("), h, S(", "), s, S("%, "), l, S("%, "), al, S(")"));
    }
    return STR_CAT(a, S("hsl("), h, S(", "), s, S("%, "), l, S("%)"));
}

static Str stringify_rgb(Arena a[static 1], MChannels c[static 1]) {
    Str r = num_str(a, lang_round(get_r(c)));
    Str g = num_str(a, lang_round(get_g(c)));
    Str b = num_str(a, lang_round(get_b(c)));
    if (c->a < 1) {
        Str al = num_str(a, lang_round(c->a));
        return STR_CAT(a, S("rgba("), r, S(", "), g, S(", "), b, S(", "), al, S(")"));
    }
    return STR_CAT(a, S("rgb("), r, S(", "), g, S(", "), b, S(")"));
}

static Str stringify_hex(Arena a[static 1], MChannels c[static 1]) {
    char hr[2], hg[2], hb[2], ha[2];
    dec2hex(get_r(c), hr);
    dec2hex(get_g(c), hg);
    dec2hex(get_b(c), hb);
    Str r = {.p = hr, .n = 2}, g = {.p = hg, .n = 2}, b = {.p = hb, .n = 2};
    if (c->a < 1) {
        dec2hex(c->a * 255.0, ha);
        Str al = {.p = ha, .n = 2};
        return STR_CAT(a, S("#"), r, g, b, al);
    }
    return STR_CAT(a, S("#"), r, g, b);
}

static Str stringify(Arena a[static 1], MChannels c[static 1]) {
    if (!c->changed && c->color.n > 0) return c->color;
    if (c->type == TYPE_HSL || !c->has_r) return stringify_hsl(a, c);
    if (c->a < 1 || !is_int(get_r(c)) || !is_int(get_g(c)) || !is_int(get_b(c))) return stringify_rgb(a, c);
    return stringify_hex(a, c);
}

/* ---- the methods the themes call ---- */

typedef struct {
    bool has_h, has_s, has_l, has_r, has_g, has_b;
    double h, s, l, r, g, b;
} MChanges;

static bool mc_change(Arena a[static 1], Str col, MChanges ch, Str out[static 1]) {
    MChannels c;
    if (!mc_parse(col, &c)) return false;
    if (ch.has_h) set_h(&c, clamp_h(ch.h));
    if (ch.has_s) set_s(&c, clamp_100(ch.s));
    if (ch.has_l) set_l(&c, clamp_100(ch.l));
    if (ch.has_r) set_r(&c, clamp_255(ch.r));
    if (ch.has_g) set_g(&c, clamp_255(ch.g));
    if (ch.has_b) set_b(&c, clamp_255(ch.b));
    *out = stringify(a, &c);
    return true;
}

bool mc_adjust(Arena a[static 1], Str col, MCAmounts am, Str out[static 1]) {
    MChannels c;
    if (!mc_parse(col, &c)) return false;
    MChanges ch = {};
    /* khroma skips amounts that are falsy: 0 and NaN */
    if ((am.mask & 1) && am.h != 0 && !isnan(am.h)) ch.has_h = true, ch.h = get_h(&c) + am.h;
    if ((am.mask & 2) && am.s != 0 && !isnan(am.s)) ch.has_s = true, ch.s = get_s(&c) + am.s;
    if ((am.mask & 4) && am.l != 0 && !isnan(am.l)) ch.has_l = true, ch.l = get_l(&c) + am.l;
    if ((am.mask & 8) && am.r != 0 && !isnan(am.r)) ch.has_r = true, ch.r = get_r(&c) + am.r;
    if ((am.mask & 16) && am.g != 0 && !isnan(am.g)) ch.has_g = true, ch.g = get_g(&c) + am.g;
    if ((am.mask & 32) && am.b != 0 && !isnan(am.b)) ch.has_b = true, ch.b = get_b(&c) + am.b;
    return mc_change(a, col, ch, out);
}

static bool mc_adjust_channel(Arena a[static 1], Str col, int ch, double amount, Str out[static 1]) {
    MChannels c;
    if (!mc_parse(col, &c)) return false;
    double cur = ch == 0 ? get_h(&c) : ch == 1 ? get_s(&c) : ch == 2 ? get_l(&c)
                 : ch == 3 ? get_r(&c) : ch == 4 ? get_g(&c) : get_b(&c);
    double next = ch == 0 ? clamp_h(cur + amount) : ch == 1 ? clamp_100(cur + amount)
                 : ch == 2 ? clamp_100(cur + amount) : ch == 3 ? clamp_255(cur + amount)
                 : ch == 4 ? clamp_255(cur + amount) : clamp_255(cur + amount);
    if (cur != next) {
        if (ch == 0) set_h(&c, next);
        else if (ch == 1) set_s(&c, next);
        else if (ch == 2) set_l(&c, next);
        else if (ch == 3) set_r(&c, next);
        else if (ch == 4) set_g(&c, next);
        else set_b(&c, next);
    }
    *out = stringify(a, &c);
    return true;
}

bool mc_lighten(Arena a[static 1], Str col, double amount, Str out[static 1]) {
    return mc_adjust_channel(a, col, 2, amount, out);
}

bool mc_darken(Arena a[static 1], Str col, double amount, Str out[static 1]) {
    return mc_adjust_channel(a, col, 2, -amount, out);
}

void mc_rgba(Arena a[static 1], double r, double g, double b, double alpha, Str out[static 1]) {
    MChannels c = {};
    c.has_r = c.has_g = c.has_b = true;
    c.r = clamp_255(r);
    c.g = clamp_255(g);
    c.b = clamp_255(b);
    c.a = clamp_a(alpha);
    *out = stringify(a, &c);
}

static bool mc_mix(Arena a[static 1], MChannels c1[static 1], Str col2, double weight, Str out[static 1]) {
    MChannels c2;
    if (!mc_parse(col2, &c2)) return false;
    double r1 = get_r(c1), g1 = get_g(c1), b1 = get_b(c1), a1 = c1->a;
    double r2 = get_r(&c2), g2 = get_g(&c2), b2 = get_b(&c2), a2 = c2.a;
    double weight_scale = weight / 100.0;
    double weight_normalized = weight_scale * 2 - 1;
    double alpha_delta = a1 - a2;
    double w1c = (weight_normalized * alpha_delta == -1)
                     ? weight_normalized
                     : (weight_normalized + alpha_delta) / (1 + weight_normalized * alpha_delta);
    double w1 = (w1c + 1) / 2, w2 = 1 - w1;
    double r = r1 * w1 + r2 * w2;
    double g = g1 * w1 + g2 * w2;
    double b = b1 * w1 + b2 * w2;
    double al = a1 * weight_scale + a2 * (1 - weight_scale);
    mc_rgba(a, r, g, b, al, out);
    return true;
}

bool mc_invert(Arena a[static 1], Str col, double weight, Str out[static 1]) {
    MChannels inv;
    if (!mc_parse(col, &inv)) return false;
    set_r(&inv, 255 - get_r(&inv));
    set_g(&inv, 255 - get_g(&inv));
    set_b(&inv, 255 - get_b(&inv));
    return mc_mix(a, &inv, col, weight, out);
}

bool mc_is_dark(Str col, bool out[static 1]) {
    MChannels c;
    if (!mc_parse(col, &c)) return false;
    double r = get_r(&c), g = get_g(&c), b = get_b(&c);
    double lum = lang_round(0.2126 * to_linear(r) + 0.7152 * to_linear(g) + 0.0722 * to_linear(b));
    *out = !(lum >= 0.5);
    return true;
}
