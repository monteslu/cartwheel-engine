/* Localization runtime (cartwheel/l10n.h): loaded catalogs, the active
 * locale's lookup chain, tr(), plural selection, number/date formatting,
 * pseudo-locales, per-locale fonts and assets, the `l10n` debug command. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/core.h"
#include "cartwheel/debug.h"
#include "cartwheel/l10n.h"
#include "cartwheel/platform.h"
#include "cartwheel/render2d.h"
#include "catalog.h"
#include "platform/hooks.h"

#define TAG CW_MEM_GENERAL
#define MAX_CHAIN 16
#define MAX_LISTENERS 8

enum { PSEUDO_NONE, PSEUDO_XA, PSEUDO_XB };

typedef struct font_slot { const char *path; cw_font font; } font_slot;

static struct {
  bool inited;
  CW_ARRAY(cwtr_catalog *) cats;
  CW_ARRAY(const char *) cat_locale; /* interned @locale per catalog */
  const char *source, *def;          /* interned */
  CW_ARRAY(const char *) pseudo;     /* offered pseudo-locales */
  const char *active;                /* interned; "" = none */
  int pseudo_kind;
  const cwtr_catalog *chain[MAX_CHAIN];
  int nchain;
  bool rtl;
  uint32_t revision;
  cw_l10n_stats stats;
  cw_map missing;                    /* hash(key) -> 1 */
  cw_map pseudo_cache;               /* source string address -> interned pseudo text */
  struct { cw_l10n_change_fn fn; void *user; } listeners[MAX_LISTENERS];
  int nlisteners;
  const char *asset_dir;             /* interned or NULL */
  CW_ARRAY(const char *) assets;
  cw_map asset_cache;                /* hash(path) -> interned resolved path, per revision */
  CW_ARRAY(font_slot) fonts;
  char system[64];
} L;

static void cmd_l10n(int argc, const char **argv, cw_reply *r);

static void init(void) {
  if (L.inited) return;
  L.inited = true;
  L.source = L.def = cw_intern("en");
  L.active = cw_intern("");
  cw_map_init(&L.missing, TAG);
  cw_map_init(&L.pseudo_cache, TAG);
  cw_map_init(&L.asset_cache, TAG);
  cw_debug_command("l10n", "l10n [locale <tag> | tr <key> | missing | locales]: the active locale, its lookup chain, "
                           "catalogs and stats; switch locale; translate a key; the missing keys so far", cmd_l10n);
}

static void changed(void) {
  L.revision++;
  cw_map_free(&L.asset_cache);
  cw_map_init(&L.asset_cache, TAG);
  for (int i = 0; i < L.nlisteners; i++) L.listeners[i].fn(L.listeners[i].user);
}

static bool tag_eq(const char *a, const char *b) {
  for (; *a && *b; a++, b++) {
    char x = *a == '_' ? '-' : (*a >= 'A' && *a <= 'Z' ? *a + 32 : *a);
    char y = *b == '_' ? '-' : (*b >= 'A' && *b <= 'Z' ? *b + 32 : *b);
    if (x != y) return false;
  }
  return !*a && !*b;
}

/* ── loading ────────────────────────────────────────────────────────── */

static bool add_parsed(cwtr_catalog *c) {
  const char *loc = cwtr_field(c, "locale");
  if (!loc || !*loc) {
    cw_error("l10n: %s has no @locale", c->name);
    cwtr_free(c);
    return false;
  }
  cw_array_push(&L.cats, c);
  cw_array_push(&L.cat_locale, cw_intern(loc));
  L.stats.catalogs = (uint32_t)L.cats.count;
  L.stats.keys += (uint32_t)c->entries.count;
  return true;
}

bool cw_l10n_add_catalog(const char *text, size_t len, const char *name) {
  init();
  char err[256];
  cwtr_catalog *c = cwtr_parse(text, len, name, err, sizeof err);
  if (!c) {
    cw_error("l10n: %s", err);
    return false;
  }
  bool ok = add_parsed(c);
  if (ok && L.active[0]) cw_l10n_set_locale(L.active);
  return ok;
}

bool cw_l10n_add_po(const char *text, size_t len, const char *name) {
  init();
  char err[256];
  cwtr_catalog *c = cwtr_parse_po(text, len, name, err, sizeof err);
  if (!c) {
    cw_error("l10n: %s", err);
    return false;
  }
  bool ok = add_parsed(c);
  if (ok && L.active[0]) cw_l10n_set_locale(L.active);
  return ok;
}

static bool ends_with(const char *s, const char *suf) {
  size_t a = strlen(s), b = strlen(suf);
  return a >= b && !strcmp(s + a - b, suf);
}

bool cw_l10n_load_catalog(const char *path) {
  init();
  size_t len;
  char *t = cw_asset_load(path, &len);
  if (!t) {
    cw_error("l10n: catalog '%s' not found", path);
    return false;
  }
  bool ok = ends_with(path, ".po") ? cw_l10n_add_po(t, len, path) : cw_l10n_add_catalog(t, len, path);
  free(t);
  return ok;
}

void cw_l10n_set_source_locale(const char *locale) {
  init();
  L.source = cw_intern(locale && *locale ? locale : "en");
}
void cw_l10n_set_default_locale(const char *locale) {
  init();
  L.def = cw_intern(locale && *locale ? locale : "en");
}
void cw_l10n_add_pseudo_locale(const char *locale) {
  init();
  const char *t = cw_intern(locale);
  for (int i = 0; i < L.pseudo.count; i++)
    if (L.pseudo.data[i] == t) return;
  cw_array_push(&L.pseudo, t);
}

bool cw_l10n_load_project(const char *path) {
  init();
  size_t len;
  char *t = cw_asset_load(path, &len);
  if (!t) {
    cw_error("l10n: project '%s' not found", path);
    return false;
  }
  bool ok = true, have_default = false;
  int lineno = 0;
  for (char *line = t; line && *line;) {
    char *nl = strchr(line, '\n');
    if (nl) *nl = 0;
    lineno++;
    char *s = line;
    while (*s == ' ' || *s == '\t') s++;
    char *hash = strchr(s, '#');
    if (hash) *hash = 0;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = 0;
    if (*s) {
      char *eq = strchr(s, '=');
      if (!eq) {
        cw_error("l10n: %s:%d: expected 'name = value'", path, lineno);
        ok = false;
      } else {
        char *k = s, *v = eq + 1;
        char *ke = eq;
        while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
        *ke = 0;
        while (*v == ' ' || *v == '\t') v++;
        if (!strcmp(k, "source")) cw_l10n_set_source_locale(v);
        else if (!strcmp(k, "default")) cw_l10n_set_default_locale(v), have_default = true;
        else if (!strcmp(k, "catalog")) ok &= cw_l10n_load_catalog(v);
        else if (!strcmp(k, "pseudo")) cw_l10n_add_pseudo_locale(v);
        else if (!strcmp(k, "asset_dir")) L.asset_dir = cw_intern(v);
        else if (!strcmp(k, "asset")) cw_array_push(&L.assets, cw_intern(v));
        else {
          cw_error("l10n: %s:%d: unknown setting '%s' (source default catalog pseudo asset_dir asset)", path, lineno, k);
          ok = false;
        }
      }
    }
    line = nl ? nl + 1 : NULL;
  }
  if (!have_default) L.def = L.source;
  free(t);
  return ok;
}

void cw_l10n_reset(void) {
  init();
  for (int i = 0; i < L.cats.count; i++) cwtr_free(L.cats.data[i]);
  cw_array_clear(&L.cats);
  cw_array_clear(&L.cat_locale);
  cw_array_clear(&L.pseudo);
  cw_array_clear(&L.assets);
  L.asset_dir = NULL;
  L.nchain = 0;
  L.active = cw_intern("");
  L.pseudo_kind = PSEUDO_NONE;
  L.rtl = false;
  L.source = L.def = cw_intern("en");
  uint32_t lookups = L.stats.lookups, missing = L.stats.missing;
  memset(&L.stats, 0, sizeof L.stats);
  L.stats.lookups = lookups, L.stats.missing = missing;
  cw_map_free(&L.pseudo_cache);
  cw_map_init(&L.pseudo_cache, TAG);
  cw_font_set_global_fallbacks(NULL, 0);
  changed();
}

/* ── locales ────────────────────────────────────────────────────────── */

static int cmp_str(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

static const char **locale_list(int *n) {
  const char **out = cw_frame_alloc(sizeof(char *) * (size_t)(L.cats.count + L.pseudo.count + 1));
  int k = 0;
  for (int i = 0; i < L.cats.count; i++) {
    bool dup = false;
    for (int j = 0; j < k; j++) dup |= out[j] == L.cat_locale.data[i];
    if (!dup) out[k++] = L.cat_locale.data[i];
  }
  for (int i = 0; i < L.pseudo.count; i++) out[k++] = L.pseudo.data[i];
  qsort(out, (size_t)k, sizeof *out, cmp_str);
  *n = k;
  return out;
}

int cw_l10n_locale_count(void) {
  init();
  int n;
  locale_list(&n);
  return n;
}
const char *cw_l10n_locale_at(int i) {
  init();
  int n;
  const char **l = locale_list(&n);
  return i >= 0 && i < n ? l[i] : NULL;
}

static const cwtr_catalog *catalog_for(const char *tag) {
  for (int i = 0; i < L.cats.count; i++)
    if (tag_eq(L.cat_locale.data[i], tag)) return L.cats.data[i];
  return NULL;
}

const char *cw_l10n_locale_name(const char *locale) {
  init();
  const cwtr_catalog *c = locale ? catalog_for(locale) : NULL;
  const char *n = c ? cwtr_field(c, "name") : NULL;
  if (n) return n;
  if (locale && tag_eq(locale, "en-XA")) return "[Ƥšéûðö Éñĝļîšĥ]";
  /* no RLM: the name is substituted into other text ("Language: {name}"),
   * where a mark mid-string turns the neutrals before it right to left */
  if (locale && tag_eq(locale, "ar-XB")) return "Pseudo RTL";
  return locale ? locale : "";
}

static void chain_push(const cwtr_catalog *c) {
  for (int i = 0; i < L.nchain; i++)
    if (L.chain[i] == c) return;
  if (L.nchain < MAX_CHAIN) L.chain[L.nchain++] = c;
}

/* every catalog for tag (load order), its parent tags, the @fallbacks */
static void chain_visit(const char *tag, int depth) {
  if (!tag || !*tag || depth > 8) return;
  int before = L.nchain;
  for (int i = 0; i < L.cats.count; i++)
    if (tag_eq(L.cat_locale.data[i], tag)) chain_push(L.cats.data[i]);
  char parent[64];
  snprintf(parent, sizeof parent, "%s", tag);
  char *dash = strrchr(parent, '-');
  if (!dash) dash = strrchr(parent, '_');
  if (dash) {
    *dash = 0;
    chain_visit(parent, depth + 1);
  }
  int after = L.nchain;
  for (int i = before; i < after; i++) {
    const char *fb = cwtr_field(L.chain[i], "fallback");
    if (fb) chain_visit(fb, depth + 1);
  }
}

static font_slot *font_for(const char *path) {
  const char *p = cw_intern(path);
  for (int i = 0; i < L.fonts.count; i++)
    if (L.fonts.data[i].path == p) return &L.fonts.data[i];
  font_slot s = { p, cw_font_load(p, &(cw_font_desc){ .mode = CW_FONT_SDF, .bake_size = 48 }) };
  cw_array_push(&L.fonts, s);
  return &L.fonts.data[L.fonts.count - 1];
}

/* the active chain's @font lists become the global fallback chain */
static void apply_fonts(void) {
  cw_font list[32];
  int n = 0;
  for (int i = 0; i < L.nchain; i++) {
    const char *fl = cwtr_field(L.chain[i], "font");
    if (!fl) continue;
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", fl);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
      while (*tok == ' ') tok++;
      size_t k = strlen(tok);
      while (k && tok[k - 1] == ' ') tok[--k] = 0;
      if (!*tok) continue;
      font_slot *s = font_for(tok);
      bool dup = false;
      for (int j = 0; j < n; j++) dup |= list[j].id == s->font.id;
      if (!CW_HANDLE_IS_NULL(s->font) && !dup && n < 32) list[n++] = s->font;
    }
  }
  /* then every other catalog's fonts: a language menu shows each language's
   * name in its own script whatever the locale */
  for (int i = 0; i < L.cats.count; i++) {
    const char *fl = cwtr_field(L.cats.data[i], "font");
    if (!fl) continue;
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", fl);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
      while (*tok == ' ') tok++;
      size_t k = strlen(tok);
      while (k && tok[k - 1] == ' ') tok[--k] = 0;
      if (!*tok) continue;
      font_slot *s = font_for(tok);
      bool dup = false;
      for (int j = 0; j < n; j++) dup |= list[j].id == s->font.id;
      if (!CW_HANDLE_IS_NULL(s->font) && !dup && n < 32) list[n++] = s->font;
    }
  }
  cw_font_set_global_fallbacks(list, n);
  L.stats.font_fallbacks = (uint32_t)n;
}

bool cw_l10n_set_locale(const char *req) {
  init();
  if (!req) req = "";
  bool matched = true;
  const char *tag = NULL;
  L.pseudo_kind = PSEUDO_NONE;
  if (tag_eq(req, "en-XA")) L.pseudo_kind = PSEUDO_XA, tag = "en-XA";
  else if (tag_eq(req, "ar-XB")) L.pseudo_kind = PSEUDO_XB, tag = "ar-XB";
  else if (catalog_for(req)) tag = cwtr_field(catalog_for(req), "locale");
  else {
    char lang[16];
    cwtr_language(req, lang, sizeof lang);
    if (catalog_for(lang)) tag = cwtr_field(catalog_for(lang), "locale");
    else
      for (int i = 0; i < L.cats.count && !tag; i++) {
        char l2[16];
        cwtr_language(L.cat_locale.data[i], l2, sizeof l2);
        if (!strcmp(l2, lang)) tag = L.cat_locale.data[i];
      }
  }
  if (!tag) {
    matched = false;
    tag = L.def;
    if (*req) cw_warn("l10n: no catalog for locale '%s'; using the default '%s'", req, L.def);
  }
  L.active = cw_intern(tag);
  L.nchain = 0;
  if (L.pseudo_kind) chain_visit(L.source, 0);
  else chain_visit(L.active, 0);
  chain_visit(L.def, 0);
  if (L.pseudo_kind == PSEUDO_XB) L.rtl = true;
  else if (L.pseudo_kind == PSEUDO_XA) L.rtl = false;
  else {
    const cwtr_catalog *c = catalog_for(L.active);
    const char *d = c ? cwtr_field(c, "direction") : NULL;
    L.rtl = d ? !strcmp(d, "rtl") : cwtr_lang_info(L.active)->rtl;
  }
  L.stats.locale_hash = cw_hash32(L.active, strlen(L.active), CW_HASH32_INIT);
  apply_fonts();
  changed();
  return matched;
}

const char *cw_l10n_locale(void) {
  init();
  return L.active;
}

const char *cw_l10n_system_locale(void) {
  init();
  if (!L.system[0]) cw__platform_system_locale(L.system, sizeof L.system);
  return L.system;
}

int cw_l10n_chain(const char **out, int cap) {
  init();
  int n = 0;
  for (int i = 0; i < L.nchain && n < cap; i++) {
    const char *t = cwtr_field(L.chain[i], "locale");
    bool dup = false;
    for (int j = 0; j < n; j++) dup |= !strcmp(out[j], t);
    if (!dup) out[n++] = t;
  }
  return n;
}

bool cw_l10n_is_rtl(void) { return L.inited && L.rtl; }
bool cw_l10n_is_pseudo(void) { return L.inited && L.pseudo_kind != PSEUDO_NONE; }
uint32_t cw_l10n_revision(void) { return L.revision; }

void cw_l10n_on_change(cw_l10n_change_fn fn, void *user) {
  init();
  for (int i = 0; i < L.nlisteners; i++)
    if (L.listeners[i].fn == fn && L.listeners[i].user == user) return;
  if (L.nlisteners < MAX_LISTENERS) L.listeners[L.nlisteners].fn = fn, L.listeners[L.nlisteners++].user = user;
}

/* ── pseudo-localization ────────────────────────────────────────────── */

static const char *accent(char c) {
  switch (c) {
  case 'a': return "å"; case 'c': return "ç"; case 'd': return "ď"; case 'e': return "é"; case 'g': return "ĝ";
  case 'h': return "ĥ"; case 'i': return "î"; case 'j': return "ĵ"; case 'k': return "ķ"; case 'l': return "ļ";
  case 'n': return "ñ"; case 'o': return "ö"; case 'r': return "ŕ"; case 's': return "š"; case 't': return "ţ";
  case 'u': return "û"; case 'w': return "ŵ"; case 'y': return "ý"; case 'z': return "ž";
  case 'A': return "Å"; case 'C': return "Ç"; case 'D': return "Ď"; case 'E': return "É"; case 'G': return "Ĝ";
  case 'H': return "Ĥ"; case 'I': return "Î"; case 'J': return "Ĵ"; case 'K': return "Ķ"; case 'L': return "Ļ";
  case 'N': return "Ñ"; case 'O': return "Ö"; case 'R': return "Ŕ"; case 'S': return "Š"; case 'T': return "Ţ";
  case 'U': return "Û"; case 'W': return "Ŵ"; case 'Y': return "Ý"; case 'Z': return "Ž";
  default: return NULL;
  }
}

typedef CW_ARRAY(char) sbuf;
static void sb_put(sbuf *b, const char *s, size_t n) {
  cw_array_reserve(b, b->count + (int)n + 1);
  memcpy(b->data + b->count, s, n);
  b->count += (int)n;
  b->data[b->count] = 0;
}

static char *pseudo_xa(const char *t) {
  sbuf b = { 0 };
  sb_put(&b, "[", 1);
  int letters = 0;
  for (const char *p = t; *p;) {
    if (*p == '{' && p[1] != '{') { /* placeholder: kept */
      const char *e = strchr(p, '}');
      if (e) {
        sb_put(&b, p, (size_t)(e - p + 1));
        p = e + 1;
        continue;
      }
    }
    if (*p == '[' && (p[1] == '/' || (p[1] >= 'a' && p[1] <= 'z'))) { /* a BBCode tag: kept */
      const char *e = strchr(p, ']');
      const char *sp = strchr(p, ' ');
      if (e && (!sp || sp > e)) {
        sb_put(&b, p, (size_t)(e - p + 1));
        p = e + 1;
        continue;
      }
    }
    const char *a = accent(*p);
    if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) letters++;
    if (a) sb_put(&b, a, strlen(a));
    else sb_put(&b, p, 1);
    p++;
  }
  /* about 40% longer: what German or Finnish do to an English layout */
  int pad = (letters * 2 + 4) / 5;
  if (pad < 1) pad = 1;
  sb_put(&b, " ", 1);
  for (int i = 0; i < pad; i++) sb_put(&b, "\xC2\xB7", 2);
  sb_put(&b, "]", 1);
  return b.data;
}

static char *pseudo_xb(const char *t) {
  sbuf b = { 0 };
  sb_put(&b, "\xE2\x80\x8F", 3); /* RLM: the paragraph reads right to left */
  sb_put(&b, t, strlen(t));
  return b.data;
}

static const char *pseudo_of(const char *s, int kind) {
  if (!kind || !s) return s;
  uint64_t key = (uint64_t)(uintptr_t)s * 2 + (uint64_t)(kind - 1), v;
  if (cw_map_get(&L.pseudo_cache, key, &v)) return (const char *)(uintptr_t)v;
  char *x = kind == PSEUDO_XA ? pseudo_xa(s) : pseudo_xb(s);
  const char *in = cw_intern(x);
  cw_free(x);
  cw_map_set(&L.pseudo_cache, key, (uint64_t)(uintptr_t)in);
  return in;
}

const char *cw_l10n_pseudo(const char *text) {
  init();
  char *x = pseudo_xa(text ? text : "");
  char *o = cw_frame_alloc(strlen(x) + 1);
  strcpy(o, x);
  cw_free(x);
  return o;
}

/* ── lookup ─────────────────────────────────────────────────────────── */

static const cwtr_entry *find(const char *key, int cat, int *at) {
  for (int i = 0; i < L.nchain; i++) {
    const cwtr_entry *e = cwtr_find(L.chain[i], key, cat);
    if (e) {
      if (at) *at = i;
      return e;
    }
  }
  return NULL;
}

static void note_missing(const char *key) {
  uint64_t h = cw_hash_str(key), v;
  if (cw_map_get(&L.missing, h, &v)) return;
  cw_map_set(&L.missing, h, (uint64_t)(uintptr_t)cw_intern(key));
  L.stats.missing++;
  cw_warn("l10n: missing key '%s' in locale '%s' (chain of %d catalogs)", key, L.active, L.nchain);
}

/* text that reads like a key: lowercase ASCII, digits, _ . -, with a '.' */
static bool key_like(const char *s) {
  bool dot = false;
  if (!*s) return false;
  for (const char *p = s; *p; p++) {
    if (*p == '.') dot = true;
    else if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return false;
  }
  return dot && s[0] != '.' && s[strlen(s) - 1] != '.';
}

static const char *hit(const cwtr_entry *e, int at) {
  if (at > 0) L.stats.fallback_hits++;
  return pseudo_of(e->value, L.pseudo_kind);
}

const char *cw_tr_find(const char *key) {
  if (!L.inited || !key) return NULL;
  L.stats.lookups++;
  int at = 0;
  const cwtr_entry *e = find(key, CWTR_NONE, &at);
  return e ? hit(e, at) : NULL;
}

const char *cw_tr(const char *key) {
  if (!key) return "";
  if (!L.inited || !L.nchain) return key;
  const char *v = cw_tr_find(key);
  if (v) return v;
  note_missing(key);
  return key;
}

const char *cw_tr_auto(const char *text) {
  if (!text || !L.inited || !L.nchain || !*text) return text ? text : "";
  const char *v = cw_tr_find(text);
  if (v) return v;
  if (key_like(text)) {
    note_missing(text);
    return text;
  }
  /* untranslated text stays exactly as written, in a pseudo-locale too: a
   * hard-coded string must stand out there, not look translated */
  return text;
}

/* ── formatting ─────────────────────────────────────────────────────── */

const char *cw_l10n_format(const char *pat, const char *const *names, const char *const *values, int count) {
  sbuf b = { 0 };
  for (const char *p = pat ? pat : ""; *p;) {
    if (p[0] == '{' && p[1] == '{') { sb_put(&b, "{", 1); p += 2; continue; }
    if (p[0] == '}' && p[1] == '}') { sb_put(&b, "}", 1); p += 2; continue; }
    if (*p == '{') {
      const char *e = strchr(p, '}');
      if (e) {
        int k = -1;
        for (int i = 0; i < count; i++)
          if (strlen(names[i]) == (size_t)(e - p - 1) && !strncmp(names[i], p + 1, (size_t)(e - p - 1))) k = i;
        if (k >= 0) {
          const char *v = values[k] ? values[k] : "";
          /* an ar-XB value (another translation) carries the RLM that starts
           * its own paragraph; inside this text it would turn the neutrals
           * before it (": ") right to left and swap the words around them.
           * At the very start it is this text's paragraph mark: kept. */
          if (L.pseudo_kind == PSEUDO_XB && b.count && !strncmp(v, "\xE2\x80\x8F", 3)) v += 3;
          sb_put(&b, v, strlen(v));
        }
        else sb_put(&b, p, (size_t)(e - p + 1));
        p = e + 1;
        continue;
      }
    }
    sb_put(&b, p, 1);
    p++;
  }
  char *o = cw_frame_alloc((size_t)b.count + 1);
  memcpy(o, b.data ? b.data : "", (size_t)b.count);
  o[b.count] = 0;
  cw_array_free(&b);
  return o;
}

const char *cw_trf(const char *key, ...) {
  const char *names[16], *values[16];
  int n = 0;
  va_list ap;
  va_start(ap, key);
  for (;;) {
    const char *nm = va_arg(ap, const char *);
    if (!nm) break;
    const char *v = va_arg(ap, const char *);
    if (n < 16) names[n] = nm, values[n++] = v;
  }
  va_end(ap);
  return cw_l10n_format(cw_tr(key), names, values, n);
}

static const char *active_or_def(void) { return L.inited && L.active[0] ? (L.pseudo_kind ? L.source : L.active) : "en"; }

/* a number field: catalog chain first, then the language default */
static const char *num_field(const char *name, const char *def) {
  for (int i = 0; L.inited && i < L.nchain; i++) {
    const char *v = cwtr_field(L.chain[i], name);
    if (v) return v;
  }
  return def;
}

static int visible_decimals(double n, char *digits, size_t cap) {
  if (n == floor(n) && fabs(n) < 1e15) {
    snprintf(digits, cap, "%.0f", n);
    return 0;
  }
  snprintf(digits, cap, "%.6f", n);
  char *dot = strchr(digits, '.');
  size_t len = strlen(digits);
  while (len && digits[len - 1] == '0') digits[--len] = 0;
  return dot ? (int)(len - (size_t)(dot - digits) - 1) : 0;
}

/* raw: unsigned ASCII digits with an optional '.'; grouped, localized digits */
static const char *localize_digits(const char *raw, bool neg) {
  const cwtr_lang *li = cwtr_lang_info(active_or_def());
  const char *dec = num_field("number.decimal", li->decimal), *grp = num_field("number.group", li->group);
  bool arab = !strcmp(num_field("number.digits", li->digits), "arab");
  const char *dot = strchr(raw, '.');
  size_t il = dot ? (size_t)(dot - raw) : strlen(raw);
  sbuf b = { 0 };
  if (neg) sb_put(&b, "-", 1);
  for (size_t i = 0; i < il; i++) {
    if (i && (il - i) % 3 == 0) sb_put(&b, grp, strlen(grp));
    if (arab) {
      char u[3] = { (char)0xD9, (char)(0xA0 + (raw[i] - '0')), 0 };
      sb_put(&b, u, 2);
    } else sb_put(&b, &raw[i], 1);
  }
  if (dot) {
    sb_put(&b, dec, strlen(dec));
    for (const char *p = dot + 1; *p; p++) {
      if (arab) {
        char u[3] = { (char)0xD9, (char)(0xA0 + (*p - '0')), 0 };
        sb_put(&b, u, 2);
      } else sb_put(&b, p, 1);
    }
  }
  char *o = cw_frame_alloc((size_t)b.count + 1);
  memcpy(o, b.data, (size_t)b.count + 1);
  cw_array_free(&b);
  return o;
}

const char *cw_l10n_format_number(double v, int decimals) {
  if (decimals < 0) decimals = 0;
  if (decimals > 9) decimals = 9;
  char raw[400];
  snprintf(raw, sizeof raw, "%.*f", decimals, fabs(v));
  return localize_digits(raw, v < 0 && strspn(raw, "0.") != strlen(raw));
}

/* every digit of a 64-bit integer (a double holds only 53 bits of them) */
const char *cw_l10n_format_int(long long v) {
  char raw[32];
  unsigned long long m = v < 0 ? 0ull - (unsigned long long)v : (unsigned long long)v;
  snprintf(raw, sizeof raw, "%llu", m);
  return localize_digits(raw, v < 0);
}

static int plural_cat(const cwtr_catalog *c, double n, int v) {
  const char *loc = cwtr_field(c, "locale");
  return cwtr_plural_for(loc ? loc : "en", n, v);
}

const char *cw_trn(const char *key, double n) {
  char digits[64];
  int v = visible_decimals(n, digits, sizeof digits);
  const char *num = cw_l10n_format_number(n, v);
  const char *names[] = { "n" }, *values[] = { num };
  if (!L.inited || !L.nchain) return cw_l10n_format(key, names, values, 1);
  L.stats.lookups++;
  /* each catalog in the chain picks the form by its own language's rule */
  for (int i = 0; i < L.nchain; i++) {
    int cat = plural_cat(L.chain[i], n, v);
    const cwtr_entry *e = cwtr_find(L.chain[i], key, cat);
    if (!e) e = cwtr_find(L.chain[i], key, CWTR_OTHER);
    if (!e) e = cwtr_find(L.chain[i], key, CWTR_NONE);
    if (e) return cw_l10n_format(hit(e, i), names, values, 1);
  }
  note_missing(key);
  return key;
}

cw_plural cw_l10n_plural_for(const char *locale, double n, int v) { return (cw_plural)cwtr_plural_for(locale ? locale : "en", n, v); }
cw_plural cw_l10n_plural(double n, int v) { return cw_l10n_plural_for(active_or_def(), n, v); }
const char *cw_plural_name(cw_plural p) { return (int)p >= 0 && (int)p < CWTR_NCAT ? cwtr_cat_names[p] : "other"; }

/* ── dates ──────────────────────────────────────────────────────────── */

static const char *const en_months[12] = { "January", "February", "March", "April", "May", "June", "July", "August",
                                           "September", "October", "November", "December" };
static const char *const en_months_short[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct",
                                                 "Nov", "Dec" };

/* the m-th (1-based) comma-separated name of a field, into out */
static bool nth_name(const char *list, int m, char *out, size_t cap) {
  const char *p = list;
  for (int k = 1; p && k < m; k++) {
    p = strchr(p, ',');
    if (p) p++;
  }
  if (!p) return false;
  while (*p == ' ') p++;
  const char *e = strchr(p, ',');
  size_t n = e ? (size_t)(e - p) : strlen(p);
  while (n && p[n - 1] == ' ') n--;
  snprintf(out, cap, "%.*s", (int)n, p);
  return n > 0;
}

static void put_digits(sbuf *b, long v, int width, bool arab) {
  char d[32];
  snprintf(d, sizeof d, "%0*ld", width, v);
  for (const char *p = d; *p; p++) {
    if (arab) {
      char u[2] = { (char)0xD9, (char)(0xA0 + (*p - '0')) };
      sb_put(b, u, 2);
    } else sb_put(b, p, 1);
  }
}

const char *cw_l10n_format_date(int y, int m, int d, cw_date_style style) {
  const cwtr_lang *li = cwtr_lang_info(active_or_def());
  const char *pat = style == CW_DATE_LONG ? num_field("date.long", li->date_long)
                  : style == CW_DATE_MEDIUM ? num_field("date.medium", li->date_medium)
                                            : num_field("date.short", li->date_short);
  bool arab = !strcmp(num_field("number.digits", li->digits), "arab");
  if (m < 1) m = 1;
  if (m > 12) m = 12;
  sbuf b = { 0 };
  for (const char *p = pat; *p;) {
    if (*p == '\'') { /* quoted literal; '' is a quote */
      p++;
      while (*p && !(*p == '\'' && p[1] != '\'')) {
        if (*p == '\'') p++;
        sb_put(&b, p, 1);
        p++;
      }
      if (*p) p++;
      continue;
    }
    char c = *p;
    int run = 0;
    while (p[run] == c) run++;
    if (c == 'y') {
      if (run == 2) put_digits(&b, y % 100, 2, arab);
      else put_digits(&b, y, 1, arab);
    } else if (c == 'M') {
      if (run >= 3) {
        char name[128];
        const char *list = num_field(run >= 4 ? "months" : "months_short", NULL);
        if (!list || !nth_name(list, m, name, sizeof name))
          snprintf(name, sizeof name, "%s", run >= 4 ? en_months[m - 1] : en_months_short[m - 1]);
        const char *shown = pseudo_of(cw_intern(name), L.inited ? L.pseudo_kind == PSEUDO_XA ? PSEUDO_XA : 0 : 0);
        sb_put(&b, shown, strlen(shown));
      } else put_digits(&b, m, run, arab);
    } else if (c == 'd') put_digits(&b, d, run, arab);
    else {
      sb_put(&b, p, (size_t)run);
    }
    p += run;
  }
  char *o = cw_frame_alloc((size_t)b.count + 1);
  memcpy(o, b.data ? b.data : "", (size_t)b.count);
  o[b.count] = 0;
  cw_array_free(&b);
  return o;
}

/* ── per-locale assets ──────────────────────────────────────────────── */

const char *cw_l10n_asset_path(const char *path) {
  if (!path) return "";
  if (!L.inited || !L.asset_dir || !L.nchain) return cw_intern(path);
  uint64_t h = cw_hash_str(path), v;
  if (cw_map_get(&L.asset_cache, h, &v)) return (const char *)(uintptr_t)v;
  const char *out = NULL;
  const char *tags[MAX_CHAIN];
  int n = cw_l10n_chain(tags, MAX_CHAIN);
  for (int i = 0; i < n && !out; i++) {
    char p[1024];
    snprintf(p, sizeof p, "%s/%s/%s", L.asset_dir, tags[i], path);
    if (cw_asset_size(p) >= 0) out = cw_intern(p);
  }
  if (!out) out = cw_intern(path);
  cw_map_set(&L.asset_cache, h, (uint64_t)(uintptr_t)out);
  return out;
}

/* ── PO conversion ──────────────────────────────────────────────────── */

char *cw_l10n_catalog_to_po(const char *cwtr, size_t len, const char *src, size_t src_len) {
  char err[256];
  cwtr_catalog *c = cwtr_parse(cwtr, len, "catalog", err, sizeof err);
  if (!c) {
    cw_error("l10n: %s", err);
    return NULL;
  }
  cwtr_catalog *s = src ? cwtr_parse(src, src_len, "source", err, sizeof err) : NULL;
  if (src && !s) cw_error("l10n: %s (msgids fall back to keys)", err);
  char *po = cwtr_write_po(c, s, NULL);
  cwtr_free(c);
  cwtr_free(s);
  return po;
}

char *cw_l10n_po_to_catalog(const char *po, size_t len) {
  char err[256];
  cwtr_catalog *c = cwtr_parse_po(po, len, "po", err, sizeof err);
  if (!c) {
    cw_error("l10n: %s", err);
    return NULL;
  }
  char *t = cwtr_write(c, NULL);
  cwtr_free(c);
  return t;
}

/* ── stats and the debug command ────────────────────────────────────── */

const cw_l10n_stats *cw_l10n_get_stats(void) { return &L.stats; }

void cw_l10n_debug_register(const char *prefix) {
  init();
  static const struct { const char *name; size_t off; } f[] = {
#define F(n) { #n, offsetof(cw_l10n_stats, n) }
    F(catalogs), F(keys), F(lookups), F(missing), F(fallback_hits), F(locale_hash), F(font_fallbacks),
#undef F
  };
  for (size_t i = 0; i < CW_ARRAY_LEN(f); i++)
    cw_debug_u32(cw_intern(cw_frame_printf("%s.%s", prefix, f[i].name)), (uint8_t *)&L.stats + f[i].off);
}

static void cmd_l10n(int argc, const char **argv, cw_reply *r) {
  if (argc >= 3 && !strcmp(argv[1], "locale")) {
    bool ok = cw_l10n_set_locale(argv[2]);
    cw_reply_printf(r, "{\"locale\":");
    cw_reply_str(r, L.active);
    cw_reply_printf(r, ",\"matched\":%s,\"rtl\":%s}", ok ? "true" : "false", L.rtl ? "true" : "false");
    return;
  }
  if (argc >= 3 && !strcmp(argv[1], "tr")) {
    const char *v = cw_tr_find(argv[2]);
    if (!v) {
      cw_reply_error(r, "no catalog in the chain of '%s' has the key '%s' (add it to a catalog)", L.active, argv[2]);
      return;
    }
    cw_reply_raw(r, "{\"key\":");
    cw_reply_str(r, argv[2]);
    cw_reply_raw(r, ",\"value\":");
    cw_reply_str(r, v);
    cw_reply_raw(r, "}");
    return;
  }
  if (argc >= 2 && !strcmp(argv[1], "missing")) {
    cw_reply_raw(r, "{\"missing\":[");
    int k = 0;
    for (int i = 0; i < L.missing.count; i++) { /* insertion order */
      if (!cw_map_live(&L.missing, i)) continue;
      if (k++) cw_reply_raw(r, ",");
      cw_reply_str(r, (const char *)(uintptr_t)L.missing.vals[i]);
    }
    cw_reply_raw(r, "]}");
    return;
  }
  if (argc >= 2 && strcmp(argv[1], "locales")) {
    cw_reply_error(r, "usage: l10n [locale <tag> | tr <key> | missing | locales]");
    return;
  }
  cw_reply_raw(r, "{\"locale\":");
  cw_reply_str(r, L.active);
  cw_reply_printf(r, ",\"rtl\":%s,\"pseudo\":%s,\"source\":", L.rtl ? "true" : "false", L.pseudo_kind ? "true" : "false");
  cw_reply_str(r, L.source);
  cw_reply_raw(r, ",\"default\":");
  cw_reply_str(r, L.def);
  cw_reply_raw(r, ",\"chain\":[");
  const char *tags[MAX_CHAIN];
  int n = cw_l10n_chain(tags, MAX_CHAIN);
  for (int i = 0; i < n; i++) {
    if (i) cw_reply_raw(r, ",");
    cw_reply_str(r, tags[i]);
  }
  cw_reply_raw(r, "],\"locales\":[");
  int nl;
  const char **ls = locale_list(&nl);
  for (int i = 0; i < nl; i++) {
    if (i) cw_reply_raw(r, ",");
    cw_reply_raw(r, "{\"tag\":");
    cw_reply_str(r, ls[i]);
    cw_reply_raw(r, ",\"name\":");
    cw_reply_str(r, cw_l10n_locale_name(ls[i]));
    cw_reply_raw(r, "}");
  }
  cw_reply_printf(r, "],\"catalogs\":%u,\"keys\":%u,\"lookups\":%u,\"missing\":%u,\"fallback_hits\":%u,\"font_fallbacks\":%u}",
                  L.stats.catalogs, L.stats.keys, L.stats.lookups, L.stats.missing, L.stats.fallback_hits, L.stats.font_fallbacks);
}
