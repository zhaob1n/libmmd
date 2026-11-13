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

/* parse preamble src[0..off) and scan the body for acc lines; state lives until dcfg_end().
 * sect is the diagram's section of mermaid's config (diag_lookup): its theme and look win over
 * the top-level ones. init is where a directive's `config` lands, the key mermaid's detectInit
 * files it under (usually sect). api is the MMD_THEME_* the caller asked for
 * (mmd_options.theme): it wins over the source's theme (mermaid-cli lets the file win over -t),
 * AUTO lets the source decide. */
void dcfg_begin(Arena a[static 1], size_t n, const char src[static n], size_t off, Str sect, Str init, int api);
void dcfg_end(void);
int dcfg_theme(void);           /* MMD_THEME_* in effect: the caller's, else the source's, else AUTO */
void dcfg_set_theme(int theme); /* MMD_THEME_* */
Str dcfg_tv(const char var[static 1]);          /* themeVariables.<var> or the named theme's preset */
/* palette entry <prefix><i> (pie1.., cScale0.., git0..) from themeVariables / named theme, else def */
const char *dcfg_pal(const char prefix[static 1], size_t i, const char def[static 1]);
/* i-th (mod count) item of a comma-separated colour list, e.g. xyChart.plotColorPalette, else def */
const char *dcfg_list(const char var[static 1], size_t i, const char def[static 1]);
const DCfg *dcfg_cur(void);
Str dcfg_get(const char key[static 1]);         /* last match, or empty; "themeVariables.x" etc. */
Str dcfg_get_str(Str key);                      /* same, for a key whose length is known: dcfg_get_str(S("x")) */
double dcfg_num(const char key[static 1], double def); /* bounded number, optional "px" suffix */
/* nodeSpacing (rank: rankSpacing) as mermaid's dagre layout takes it: a top-level one, else the
 * one in config section sect (which is "state" for class and requirement diagrams too), else def;
 * 0 and negative values count as unset */
double dcfg_spacing(const char sect[static 1], bool rank, double def);
bool dcfg_bool(const char key[static 1], bool def);
/* mermaid 12's neo default (agentflow, usecase): the classic look needs `look: classic` and a
 * classic theme, named by the caller or by the source (not redux*) */
bool dcfg_neo(void);
Str dcfg_css(const char key[static 1]);         /* value only if safe inside CSS / an attribute */
bool dcfg_is_acc(Str line, bool block[static 1]); /* accTitle:/accDescr: line; *block if "accDescr {" */
void dcfg_theme_css(Buf b[static 1], const char kind[static 1]); /* CSS overrides from themeVariables */
Str dcfg_background(void);                      /* safe themeVariables.background or empty */
void dcfg_svg_aria(Buf b[static 1]);            /* call before the '>' of <svg ...: aria attrs, '>', title/desc */
