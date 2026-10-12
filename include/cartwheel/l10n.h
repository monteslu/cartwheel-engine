/* cartwheel localization: translation catalogs, locale selection and
 * fallback, CLDR plural rules, tr(), number and date formatting,
 * pseudo-locales for testing, per-locale fonts and assets.
 *
 * ── Catalogs (.cwtr): one per locale, a line per entry, agents diff them ─
 *
 *   # comment lines right before an entry are its translator note
 *   @locale = ar                    # BCP 47 tag (required)
 *   @name = العربية                  # display name for a language menu
 *   @fallback = en                  # next catalog to try (default: the
 *                                   # parent tag, then the project default)
 *   @direction = rtl                # default from the language
 *   @font = fonts/NotoSansArabic.ttf  # fallback fonts while active (comma list)
 *   @number.decimal = ٫             # number/date overrides (defaults: the language's)
 *   @number.group = ٬
 *   @number.digits = arab           # latn | arab
 *   @date.short = d/M/y             # also @date.medium, @date.long
 *   @months = يناير, فبراير, ...       # 12 names (long dates); @months_short likewise
 *   menu.play = العب
 *   score.points[zero] = لا نقاط     # plural forms: key[zero|one|two|few|many|other]
 *   score.points[one] = نقطة واحدة
 *   score.points[other] = {n} نقطة
 *   greeting = Hello, {name}!\nWelcome back.   # \n \t \\ and \s (a space at an end)
 *
 * Keys are letters, digits and _ . - : /. Values hold {name} placeholders
 * ({{ and }} are literal braces). cwtr files written by the engine (and
 * cwpack) are canonical: @locale first, then the other fields, then entries
 * sorted by key, so a diff shows exactly what changed.
 *
 * ── Project (.cwl10n): what a game ships ─────────────────────────────
 *
 *   source = en                     # the language keys were written in
 *   default = en                    # the last fallback (default: source)
 *   catalog = l10n/en.cwtr          # every catalog (cwpack packs them)
 *   catalog = l10n/ar.cwtr
 *   pseudo = en-XA                  # pseudo-locales to offer: en-XA, ar-XB
 *   asset_dir = l10n/assets         # per-locale assets: <asset_dir>/<locale>/<path>
 *   asset = textures/banner.png     # assets that have per-locale variants
 *
 * PO files (gettext) convert both ways (cw_l10n_catalog_to_po /
 * cw_l10n_po_to_catalog): msgctxt carries the key, msgid the source text,
 * msgstr[i] the plural forms in the PO language's CLDR category order,
 * @fields travel as X-Cartwheel-* headers, so a round trip is lossless.
 * cwpack imports a PO source directly (roots.txt: "l10n/ja.cwtr = po/ja.po").
 *
 * ── Lookup ──────────────────────────────────────────────────────────
 * The active locale's chain: the exact catalog, then its parent tags
 * ("pt-BR" -> "pt"), each catalog's @fallback, then the default locale. A
 * key no catalog in the chain has is MISSING: cw_tr returns the key itself,
 * logs once and counts it (cw_l10n_get_stats().missing). UI controls
 * translate their text automatically (ui.h: auto_translate): text that is a
 * key translates, other text passes through unchanged (and is never counted
 * missing unless it looks like a key: no spaces, a '.', lowercase ASCII).
 *
 * Pseudo-locales derive from the source locale for testing layouts:
 * en-XA accents every letter, pads by 40% and brackets the text ("[Ŝéţţîñĝš
 * ·····]"), so truncation and untranslated (unaccented) text stand out;
 * ar-XB makes the source text right-to-left (and the UI mirror). Placeholders
 * are kept.
 *
 * Strings returned by cw_tr and friends stay valid until the catalogs are
 * unloaded (formatted results: until the end of the frame). Threads: main
 * thread only. Memory: CW_MEM_GENERAL. */
#ifndef CW_L10N_H
#define CW_L10N_H

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { CW_PLURAL_ZERO, CW_PLURAL_ONE, CW_PLURAL_TWO, CW_PLURAL_FEW, CW_PLURAL_MANY, CW_PLURAL_OTHER } cw_plural;
typedef enum { CW_DATE_SHORT, CW_DATE_MEDIUM, CW_DATE_LONG } cw_date_style;

/* ── loading ───────────────────────────────────────────────────────── */
/* A project file (asset): its catalogs, pseudo-locales and per-locale
 * assets. false (logged) when it or a catalog fails to load. */
bool cw_l10n_load_project(const char *asset_path);
/* One catalog: an asset path (.cwtr, or .po) or text in memory. */
bool cw_l10n_load_catalog(const char *asset_path);
/* .cwtr text (copied); name labels errors. Re-applies the active locale. */
bool cw_l10n_add_catalog(const char *text, size_t len, const char *name);
bool cw_l10n_add_po(const char *text, size_t len, const char *name); /* a gettext PO file, otherwise as add_catalog */
/* The source and default locales (a project file sets them; default "en"). */
void cw_l10n_set_source_locale(const char *locale);
void cw_l10n_set_default_locale(const char *locale); /* NULL or "" = "en" */
/* Offer a pseudo-locale ("en-XA" or "ar-XB"). */
void cw_l10n_add_pseudo_locale(const char *locale);
/* Forget every catalog and the active locale (back to passthrough). */
void cw_l10n_reset(void);

/* Locales with a catalog, plus pseudo-locales, sorted. */
int cw_l10n_locale_count(void);
const char *cw_l10n_locale_at(int index); /* interned tag; NULL out of range. Rebuilds the sorted list per call */
const char *cw_l10n_locale_name(const char *locale); /* @name, else the tag */

/* ── selection ─────────────────────────────────────────────────────── */
/* Choose the locale: an exact catalog, else one for the same language
 * ("ja-JP" -> "ja"), else the default. true when the request matched;
 * false means the default locale was chosen. Loads the locale's fonts, sets
 * the global font fallback chain, re-lays out the UI. */
bool cw_l10n_set_locale(const char *locale);
const char *cw_l10n_locale(void);    /* active; "" before any set_locale */
/* The user's preferred locale from the platform (native: SDL's preferred
 * locales), "" when unknown. A cart always gets "": the wasmcart ABI has no
 * way for the host to say, so carts pick from a setting or the default. */
const char *cw_l10n_system_locale(void);
/* The lookup chain of the active locale (catalog tags, first wins). */
int cw_l10n_chain(const char **out, int cap);
bool cw_l10n_is_rtl(void);            /* the active locale reads right to left */
bool cw_l10n_is_pseudo(void); /* the active locale is a pseudo-locale */
/* Bumped by every change of locale or catalogs (cache keys). */
uint32_t cw_l10n_revision(void);
typedef void (*cw_l10n_change_fn)(void *user);
void cw_l10n_on_change(cw_l10n_change_fn fn, void *user); /* up to 8 listeners */

/* ── translation ───────────────────────────────────────────────────── */
/* The translation of key, or key itself when missing (counted, logged once). */
const char *cw_tr(const char *key);
/* NULL when no catalog in the chain has the key (nothing counted). */
const char *cw_tr_find(const char *key);
/* The auto-translation the UI uses: the translation when the text is a
 * key, else the text unchanged. Unchanged in a pseudo-locale too: only
 * translated strings are pseudo-localized, so hard-coded text stands out. */
const char *cw_tr_auto(const char *text);
/* Plural: the form for n's CLDR category (falling back to [other]), with
 * {n} replaced by n formatted for the locale. Frame-arena result. */
const char *cw_trn(const char *key, double n);
/* Named arguments, NULL-terminated name/value pairs:
 * cw_trf("greet", "name", "Ana", NULL). Frame-arena result. */
const char *cw_trf(const char *key, ...);
/* Substitute {name} placeholders of any pattern. Frame-arena result. */
const char *cw_l10n_format(const char *pattern, const char *const *names, const char *const *values, int count);

cw_plural cw_l10n_plural(double n, int visible_decimals); /* the active locale's category */
/* A given locale's category (NULL = "en"). */
cw_plural cw_l10n_plural_for(const char *locale, double n, int visible_decimals);
const char *cw_plural_name(cw_plural p); /* "zero" ... "other" */

/* ── formatting (frame-arena results) ──────────────────────────────── */
/* Grouped digits, the locale's separators and digits; decimals fixed. */
const char *cw_l10n_format_number(double v, int decimals);
const char *cw_l10n_format_int(long long v); /* format_number(v, 0), every digit exact (full 64-bit range) */
/* A Gregorian date (month 1..12). */
const char *cw_l10n_format_date(int year, int month, int day, cw_date_style style);
/* The pseudo-localized form of any text (en-XA rules). */
const char *cw_l10n_pseudo(const char *text);

/* ── per-locale assets ─────────────────────────────────────────────── */
/* "<asset_dir>/<locale>/<path>" for the first locale of the chain that
 * ships that variant, else path. Interned. */
const char *cw_l10n_asset_path(const char *path);

/* ── PO conversion (cw_alloc'd text; NULL with the error logged) ───── */
char *cw_l10n_catalog_to_po(const char *cwtr, size_t len, const char *source_cwtr, size_t source_len);
char *cw_l10n_po_to_catalog(const char *po, size_t len); /* canonical .cwtr text; free with cw_free */

/* ── stats and debugging ───────────────────────────────────────────── */
typedef struct cw_l10n_stats {
  uint32_t catalogs, keys; /* loaded */
  uint32_t lookups;        /* cw_tr* calls (all time) */
  uint32_t missing;        /* distinct missing keys (all time) */
  uint32_t fallback_hits;  /* lookups answered by a catalog after the first in the chain */
  uint32_t locale_hash;    /* FNV-1a of the active locale tag */
  uint32_t font_fallbacks; /* fonts in the active global fallback chain */
} cw_l10n_stats;
const cw_l10n_stats *cw_l10n_get_stats(void); /* module-owned, live (updates in place) */
/* Register the stats as debug fields "<prefix>.<name>" and the debug
 * channel command `l10n` (state, `l10n locale <tag>`, `l10n tr <key>`,
 * `l10n missing`). */
void cw_l10n_debug_register(const char *prefix);

#ifdef __cplusplus
}
#endif
#endif
