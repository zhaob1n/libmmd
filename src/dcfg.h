/* Diagram configuration shared by the non-flowchart renderers: %%{init}%% directives and YAML frontmatter
 * (themeVariables + per-diagram keys, flattened to dotted keys) and accTitle/accDescr. */
#pragma once
#include "base.h"

typedef struct DKV { Str k, v; } DKV;
typedef struct DCfg {
    size_t n;
    DKV *kv;
    Str acc_title, acc_descr;
} DCfg;

/* parse preamble src[0..off) and scan the body for acc lines; state lives until dcfg_end() */
void dcfg_begin(Arena *a, size_t n, const char src[static n], size_t off);
void dcfg_end(void);
void dcfg_set_theme(int theme); /* CMM_THEME_* */
Str dcfg_tv(const char *var);
/* palette entry <prefix><i> (pie1.., cScale0.., git0..) from themeVariables / named theme, else def */
const char *dcfg_pal(const char *prefix, size_t i, const char def[static 1]); /* themeVariables.<var> or named-theme preset */
const DCfg *dcfg_cur(void);
Str dcfg_get(const char *key);                  /* last match, or empty; "themeVariables.x" etc. */
double dcfg_num(const char *key, double def);   /* bounded number, optional "px" suffix */
bool dcfg_bool(const char *key, bool def);
Str dcfg_css(const char *key);                  /* value only if safe inside CSS / an attribute */
bool dcfg_is_acc(Str line, bool *block);        /* accTitle:/accDescr: line; *block if "accDescr {" */
void dcfg_theme_css(Buf *b, const char *kind);  /* CSS overrides from themeVariables */
Str dcfg_background(void);                      /* safe themeVariables.background or empty */
void dcfg_svg_aria(Buf *b);                     /* call before the '>' of <svg ...: aria attrs, '>', title/desc */
