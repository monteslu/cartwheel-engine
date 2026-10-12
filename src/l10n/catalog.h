/* Translation catalogs as data: the .cwtr text format, PO import/export,
 * CLDR plural rules and per-language defaults. No platform or GL code, so
 * cwpack links it too (validation, PO import at pack time, the codepoints a
 * font subset needs). The engine-facing API is cartwheel/l10n.h. */
#ifndef CW_L10N_CATALOG_H
#define CW_L10N_CATALOG_H

#include "cartwheel/core.h"

/* plural categories, CLDR order (the order PO forms are numbered in) */
enum { CWTR_ZERO, CWTR_ONE, CWTR_TWO, CWTR_FEW, CWTR_MANY, CWTR_OTHER, CWTR_NCAT, CWTR_NONE = -1 };
extern const char *const cwtr_cat_names[CWTR_NCAT];

typedef struct cwtr_entry {
  char *key;
  int cat;     /* CWTR_NONE for a plain entry, else the plural category */
  char *value; /* unescaped UTF-8 */
  char *note;  /* comment lines right before the entry (without '#'), or NULL */
} cwtr_entry;

typedef struct cwtr_kv { char *name, *value; } cwtr_kv; /* @name = value */

typedef struct cwtr_catalog {
  char *name;   /* for messages: the asset path */
  CW_ARRAY(cwtr_kv) fields;
  CW_ARRAY(cwtr_entry) entries;
  cw_map index; /* hash(key, cat) -> entry index + 1 */
} cwtr_catalog;

/* Parse .cwtr text. NULL with a "name:line: message" in err on an error. */
cwtr_catalog *cwtr_parse(const char *text, size_t len, const char *name, char *err, size_t errcap);
/* Parse a gettext PO file: msgctxt is the key (else msgid), msgstr[i] are
 * the plural forms in CLDR category order for the PO's language, empty or
 * fuzzy translations are skipped, "#." comments become notes, X-Cartwheel-*
 * header fields become @fields, Language becomes @locale. */
cwtr_catalog *cwtr_parse_po(const char *text, size_t len, const char *name, char *err, size_t errcap);
void cwtr_free(cwtr_catalog *c);

/* Canonical .cwtr text: fields in file order (@locale first), then entries
 * sorted by key (plural categories in CLDR order). cw_alloc'd, NUL-terminated. */
char *cwtr_write(const cwtr_catalog *c, size_t *len);
/* PO text. source (may be NULL) gives each msgid its source-language text;
 * without it msgid is the key. */
char *cwtr_write_po(const cwtr_catalog *c, const cwtr_catalog *source, size_t *len);

const char *cwtr_field(const cwtr_catalog *c, const char *name); /* NULL when absent */
const cwtr_entry *cwtr_find(const cwtr_catalog *c, const char *key, int cat);
bool cwtr_add(cwtr_catalog *c, const char *key, int cat, const char *value, const char *note); /* false on a duplicate */
void cwtr_set_field(cwtr_catalog *c, const char *name, const char *value);
/* Every catalog-visible entry and field equal (order-insensitive). */
bool cwtr_equal(const cwtr_catalog *a, const cwtr_catalog *b);

/* ── languages ─────────────────────────────────────────────────────── */
/* "pt-BR" -> "pt" (into out) */
void cwtr_language(const char *locale, char *out, size_t cap);

typedef struct cwtr_lang {
  const char *lang;
  int rule;                  /* plural rule id (cwtr_plural) */
  bool rtl;
  const char *decimal, *group; /* number separators */
  const char *digits;        /* "latn" or "arab" */
  const char *date_short, *date_medium, *date_long; /* CLDR-style patterns */
} cwtr_lang;
/* Defaults for a locale's language (an unknown language gets English's). */
const cwtr_lang *cwtr_lang_info(const char *locale);
/* Plural category of a number written with v visible fraction digits. */
int cwtr_plural(int rule, double n, int v);
int cwtr_plural_for(const char *locale, double n, int v);
/* The categories a rule uses, CLDR order (returns the count). */
int cwtr_rule_categories(int rule, int out[CWTR_NCAT]);
/* gettext Plural-Forms for a rule ("nplurals=2; plural=(n != 1);") */
const char *cwtr_rule_po(int rule);

#endif
