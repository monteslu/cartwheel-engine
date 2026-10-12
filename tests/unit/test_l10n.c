/* Localization and accessibility suite: CLDR plural rules against CLDR's
 * own sample numbers, locale selection and fallback chains, tr/trn/trf,
 * number and date formatting, pseudo-locales, catalog parse errors, the
 * .cwtr <-> PO round trip, RTL mirroring of container layouts (exact
 * rects), auto-translation, the accessibility tree, captions, binding
 * display names, text scale, shaping and font fallback measurements.
 * Assets: the l10n demo's pack (catalogs and Noto subsets). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/a11y.h"
#include "cartwheel/l10n.h"
#include "cartwheel/render2d.h"
#include "cartwheel/scene.h"
#include "cartwheel/ui.h"
#include "test.h"

/* src/render2d/internal.h (test hook): glyph source bytes in visual order */
int r2d_text_visual_bytes(cw_font f, const char *text, const cw_text_style *style, int *out, int cap);

#define STR_EQ(a, b) \
  do { \
    const char *_a = (a), *_b = (b); \
    if (_a && _b && !strcmp(_a, _b)) cw_test_passed++, cw_test_checksum = cw_hash32(_a, strlen(_a), cw_test_checksum); \
    else { cw_test_failed++; cw_error("CHECK_STR failed %s:%d: %s = \"%s\", expected \"%s\"", __FILE__, __LINE__, #a, _a ? _a : "(null)", _b); } \
  } while (0)

/* ── plural rules: CLDR's sample numbers (common/supplemental/plurals.xml,
 *    @integer / @decimal lists; ranges left out) ─────────────────────── */
typedef struct { const char *locale, *num; cw_plural cat; } sample;
static const sample SAMPLES[] = {
  { "ja", "100", CW_PLURAL_OTHER },
  { "ja", "1000", CW_PLURAL_OTHER },
  { "ja", "10000", CW_PLURAL_OTHER },
  { "ja", "100000", CW_PLURAL_OTHER },
  { "ja", "10.0", CW_PLURAL_OTHER },
  { "ja", "100.0", CW_PLURAL_OTHER },
  { "ja", "1000.0", CW_PLURAL_OTHER },
  { "hi", "0", CW_PLURAL_ONE },
  { "hi", "1", CW_PLURAL_ONE },
  { "hi", "100", CW_PLURAL_OTHER },
  { "hi", "1000", CW_PLURAL_OTHER },
  { "hi", "10000", CW_PLURAL_OTHER },
  { "hi", "100000", CW_PLURAL_OTHER },
  { "hi", "10.0", CW_PLURAL_OTHER },
  { "hi", "100.0", CW_PLURAL_OTHER },
  { "hi", "1000.0", CW_PLURAL_OTHER },
  { "en", "1", CW_PLURAL_ONE },
  { "en", "0", CW_PLURAL_OTHER },
  { "en", "100", CW_PLURAL_OTHER },
  { "en", "1000", CW_PLURAL_OTHER },
  { "en", "10000", CW_PLURAL_OTHER },
  { "en", "10.0", CW_PLURAL_OTHER },
  { "en", "100.0", CW_PLURAL_OTHER },
  { "en", "1000.0", CW_PLURAL_OTHER },
  { "de", "1", CW_PLURAL_ONE },
  { "de", "0", CW_PLURAL_OTHER },
  { "de", "100", CW_PLURAL_OTHER },
  { "de", "1000", CW_PLURAL_OTHER },
  { "de", "10000", CW_PLURAL_OTHER },
  { "de", "10.0", CW_PLURAL_OTHER },
  { "de", "100.0", CW_PLURAL_OTHER },
  { "de", "1000.0", CW_PLURAL_OTHER },
  { "tr", "1", CW_PLURAL_ONE },
  { "tr", "1.0", CW_PLURAL_ONE },
  { "tr", "1.00", CW_PLURAL_ONE },
  { "tr", "1.000", CW_PLURAL_ONE },
  { "tr", "0", CW_PLURAL_OTHER },
  { "tr", "100", CW_PLURAL_OTHER },
  { "tr", "1000", CW_PLURAL_OTHER },
  { "tr", "10000", CW_PLURAL_OTHER },
  { "tr", "10.0", CW_PLURAL_OTHER },
  { "tr", "100.0", CW_PLURAL_OTHER },
  { "tr", "1000.0", CW_PLURAL_OTHER },
  { "da", "1", CW_PLURAL_ONE },
  { "da", "0", CW_PLURAL_OTHER },
  { "da", "100", CW_PLURAL_OTHER },
  { "da", "1000", CW_PLURAL_OTHER },
  { "da", "10000", CW_PLURAL_OTHER },
  { "da", "0.0", CW_PLURAL_OTHER },
  { "da", "10.0", CW_PLURAL_OTHER },
  { "da", "100.0", CW_PLURAL_OTHER },
  { "he", "1", CW_PLURAL_ONE },
  { "he", "2", CW_PLURAL_TWO },
  { "he", "0", CW_PLURAL_OTHER },
  { "he", "100", CW_PLURAL_OTHER },
  { "he", "1000", CW_PLURAL_OTHER },
  { "he", "10000", CW_PLURAL_OTHER },
  { "he", "10.0", CW_PLURAL_OTHER },
  { "he", "100.0", CW_PLURAL_OTHER },
  { "he", "1000.0", CW_PLURAL_OTHER },
  { "ro", "1", CW_PLURAL_ONE },
  { "ro", "0", CW_PLURAL_FEW },
  { "ro", "101", CW_PLURAL_FEW },
  { "ro", "1001", CW_PLURAL_FEW },
  { "ro", "10.0", CW_PLURAL_FEW },
  { "ro", "100.0", CW_PLURAL_FEW },
  { "ro", "1000.0", CW_PLURAL_FEW },
  { "ro", "100", CW_PLURAL_OTHER },
  { "ro", "1000", CW_PLURAL_OTHER },
  { "ro", "10000", CW_PLURAL_OTHER },
  { "ro", "100000", CW_PLURAL_OTHER },
  { "fr", "0", CW_PLURAL_ONE },
  { "fr", "1", CW_PLURAL_ONE },
  { "fr", "1000000", CW_PLURAL_MANY },
  { "fr", "100", CW_PLURAL_OTHER },
  { "fr", "1000", CW_PLURAL_OTHER },
  { "fr", "10000", CW_PLURAL_OTHER },
  { "fr", "100000", CW_PLURAL_OTHER },
  { "fr", "10.0", CW_PLURAL_OTHER },
  { "fr", "100.0", CW_PLURAL_OTHER },
  { "fr", "1000.0", CW_PLURAL_OTHER },
  { "pt", "0", CW_PLURAL_ONE },
  { "pt", "1", CW_PLURAL_ONE },
  { "pt", "1000000", CW_PLURAL_MANY },
  { "pt", "100", CW_PLURAL_OTHER },
  { "pt", "1000", CW_PLURAL_OTHER },
  { "pt", "10000", CW_PLURAL_OTHER },
  { "pt", "100000", CW_PLURAL_OTHER },
  { "pt", "10.0", CW_PLURAL_OTHER },
  { "pt", "100.0", CW_PLURAL_OTHER },
  { "pt", "1000.0", CW_PLURAL_OTHER },
  { "it", "1", CW_PLURAL_ONE },
  { "it", "1000000", CW_PLURAL_MANY },
  { "it", "0", CW_PLURAL_OTHER },
  { "it", "100", CW_PLURAL_OTHER },
  { "it", "1000", CW_PLURAL_OTHER },
  { "it", "10000", CW_PLURAL_OTHER },
  { "it", "10.0", CW_PLURAL_OTHER },
  { "it", "100.0", CW_PLURAL_OTHER },
  { "it", "1000.0", CW_PLURAL_OTHER },
  { "es", "1", CW_PLURAL_ONE },
  { "es", "1.0", CW_PLURAL_ONE },
  { "es", "1.00", CW_PLURAL_ONE },
  { "es", "1.000", CW_PLURAL_ONE },
  { "es", "1000000", CW_PLURAL_MANY },
  { "es", "0", CW_PLURAL_OTHER },
  { "es", "100", CW_PLURAL_OTHER },
  { "es", "1000", CW_PLURAL_OTHER },
  { "es", "10000", CW_PLURAL_OTHER },
  { "es", "10.0", CW_PLURAL_OTHER },
  { "es", "100.0", CW_PLURAL_OTHER },
  { "es", "1000.0", CW_PLURAL_OTHER },
  { "cs", "1", CW_PLURAL_ONE },
  { "cs", "10.0", CW_PLURAL_MANY },
  { "cs", "100.0", CW_PLURAL_MANY },
  { "cs", "1000.0", CW_PLURAL_MANY },
  { "cs", "0", CW_PLURAL_OTHER },
  { "cs", "100", CW_PLURAL_OTHER },
  { "cs", "1000", CW_PLURAL_OTHER },
  { "cs", "10000", CW_PLURAL_OTHER },
  { "pl", "1", CW_PLURAL_ONE },
  { "pl", "62", CW_PLURAL_FEW },
  { "pl", "102", CW_PLURAL_FEW },
  { "pl", "1002", CW_PLURAL_FEW },
  { "pl", "0", CW_PLURAL_MANY },
  { "pl", "100", CW_PLURAL_MANY },
  { "pl", "1000", CW_PLURAL_MANY },
  { "pl", "10000", CW_PLURAL_MANY },
  { "pl", "10.0", CW_PLURAL_OTHER },
  { "pl", "100.0", CW_PLURAL_OTHER },
  { "pl", "1000.0", CW_PLURAL_OTHER },
  { "ru", "1", CW_PLURAL_ONE },
  { "ru", "21", CW_PLURAL_ONE },
  { "ru", "31", CW_PLURAL_ONE },
  { "ru", "41", CW_PLURAL_ONE },
  { "ru", "62", CW_PLURAL_FEW },
  { "ru", "102", CW_PLURAL_FEW },
  { "ru", "1002", CW_PLURAL_FEW },
  { "ru", "0", CW_PLURAL_MANY },
  { "ru", "100", CW_PLURAL_MANY },
  { "ru", "1000", CW_PLURAL_MANY },
  { "ru", "10000", CW_PLURAL_MANY },
  { "ru", "10.0", CW_PLURAL_OTHER },
  { "ru", "100.0", CW_PLURAL_OTHER },
  { "ru", "1000.0", CW_PLURAL_OTHER },
  { "ar", "0", CW_PLURAL_ZERO },
  { "ar", "0.0", CW_PLURAL_ZERO },
  { "ar", "0.00", CW_PLURAL_ZERO },
  { "ar", "0.000", CW_PLURAL_ZERO },
  { "ar", "1", CW_PLURAL_ONE },
  { "ar", "1.0", CW_PLURAL_ONE },
  { "ar", "1.00", CW_PLURAL_ONE },
  { "ar", "1.000", CW_PLURAL_ONE },
  { "ar", "2", CW_PLURAL_TWO },
  { "ar", "2.0", CW_PLURAL_TWO },
  { "ar", "2.00", CW_PLURAL_TWO },
  { "ar", "2.000", CW_PLURAL_TWO },
  { "ar", "1003", CW_PLURAL_FEW },
  { "ar", "3.0", CW_PLURAL_FEW },
  { "ar", "4.0", CW_PLURAL_FEW },
  { "ar", "5.0", CW_PLURAL_FEW },
  { "ar", "111", CW_PLURAL_MANY },
  { "ar", "1011", CW_PLURAL_MANY },
  { "ar", "11.0", CW_PLURAL_MANY },
  { "ar", "12.0", CW_PLURAL_MANY },
  { "ar", "13.0", CW_PLURAL_MANY },
  { "ar", "600", CW_PLURAL_OTHER },
  { "ar", "1000", CW_PLURAL_OTHER },
  { "ar", "10000", CW_PLURAL_OTHER },
  { "ar", "100000", CW_PLURAL_OTHER },
  { "ar", "10.1", CW_PLURAL_OTHER },
  { "ar", "100.0", CW_PLURAL_OTHER },
  { "ar", "1000.0", CW_PLURAL_OTHER },
};

static void test_plural_cldr(void) {
  int bad = 0;
  for (size_t i = 0; i < CW_ARRAY_LEN(SAMPLES); i++) {
    const char *dot = strchr(SAMPLES[i].num, '.');
    int v = dot ? (int)strlen(dot + 1) : 0;
    cw_plural got = cw_l10n_plural_for(SAMPLES[i].locale, atof(SAMPLES[i].num), v);
    if (got != SAMPLES[i].cat) {
      bad++;
      cw_error("plural %s %s: %s, CLDR says %s", SAMPLES[i].locale, SAMPLES[i].num, cw_plural_name(got), cw_plural_name(SAMPLES[i].cat));
    }
    FOLD(got);
  }
  CHECK_EQ_INT(bad, 0);
  CHECK(CW_ARRAY_LEN(SAMPLES) > 150);
}

/* ── catalogs in memory ──────────────────────────────────────────────── */
static const char *EN = "@locale = en\n@name = English\nmenu.play = Play\nmenu.quit = Quit\nonly.en = English only\n"
                        "items[one] = {n} item\nitems[other] = {n} items\ngreet = Hello, {name}! {{braces}}\n"
                        "lang = Language: {name}\nbracketed = [a coin chimes]\n";
static const char *PT = "@locale = pt\n@name = Português\nmenu.play = Jogar\nmenu.quit = Sair\n"
                        "items[one] = {n} item\nitems[many] = {n} de itens\nitems[other] = {n} itens\n";
static const char *PT_BR = "@locale = pt-BR\n@name = Português (Brasil)\nmenu.play = Jogar!\n";
static const char *AR = "@locale = ar\n@number.digits = arab\nmenu.play = العب\n"
                        "items[zero] = لا عناصر\nitems[one] = عنصر واحد\nitems[two] = عنصران\nitems[few] = {n} عناصر\n"
                        "items[many] = {n} عنصرًا\nitems[other] = {n} عنصر\n"
                        "@months = يناير, فبراير, مارس, أبريل, مايو, يونيو, يوليو, أغسطس, سبتمبر, أكتوبر, نوفمبر, ديسمبر\n";

static void load_all(void) {
  cw_l10n_reset();
  CHECK(cw_l10n_add_catalog(EN, strlen(EN), "en"));
  CHECK(cw_l10n_add_catalog(PT, strlen(PT), "pt"));
  CHECK(cw_l10n_add_catalog(PT_BR, strlen(PT_BR), "pt-BR"));
  CHECK(cw_l10n_add_catalog(AR, strlen(AR), "ar"));
}

static void test_fallback(void) {
  load_all();
  CHECK(cw_l10n_set_locale("pt-BR"));
  const char *chain[8];
  int n = cw_l10n_chain(chain, 8);
  CHECK_EQ_INT(n, 3);
  STR_EQ(chain[0], "pt-BR");
  STR_EQ(chain[1], "pt");
  STR_EQ(chain[2], "en");
  STR_EQ(cw_tr("menu.play"), "Jogar!");       /* pt-BR itself */
  STR_EQ(cw_tr("menu.quit"), "Sair");         /* its parent tag */
  uint32_t hits = cw_l10n_get_stats()->fallback_hits;
  STR_EQ(cw_tr("only.en"), "English only");   /* the default locale */
  CHECK_EQ_INT(cw_l10n_get_stats()->fallback_hits, hits + 1);
  uint32_t missing = cw_l10n_get_stats()->missing;
  STR_EQ(cw_tr("no.such.key"), "no.such.key"); /* missing: the key, counted once */
  STR_EQ(cw_tr("no.such.key"), "no.such.key");
  CHECK_EQ_INT(cw_l10n_get_stats()->missing, missing + 1);
  CHECK(cw_tr_find("no.such.key") == NULL);
  /* auto-translation: keys translate, text passes, key-like text counts */
  STR_EQ(cw_tr_auto("menu.play"), "Jogar!");
  STR_EQ(cw_tr_auto("Plain text."), "Plain text.");
  STR_EQ(cw_tr_auto("also.missing"), "also.missing");
  CHECK_EQ_INT(cw_l10n_get_stats()->missing, missing + 2);
  /* language match, no match */
  CHECK(cw_l10n_set_locale("pt-PT"));
  STR_EQ(cw_l10n_locale(), "pt");
  CHECK(!cw_l10n_set_locale("de-AT"));
  STR_EQ(cw_l10n_locale(), "en");
  STR_EQ(cw_tr("menu.play"), "Play");
  CHECK(cw_l10n_set_locale("ar-EG"));
  CHECK(cw_l10n_is_rtl());
  CHECK_EQ_INT(cw_l10n_locale_count(), 4);
  STR_EQ(cw_l10n_locale_name("pt"), "Português");
  /* the debug command answers the same */
  char *r = cw_debug_run("l10n tr menu.play");
  CHECK(r && strstr(r, "\"value\":\"العب\""));
  cw_free(r);
  r = cw_debug_run("l10n tr nope.nope");
  CHECK(r && strstr(r, "\"error\""));
  cw_free(r);
}

static void test_plural_forms(void) {
  load_all();
  cw_l10n_set_locale("en");
  STR_EQ(cw_trn("items", 1), "1 item");
  STR_EQ(cw_trn("items", 2), "2 items");
  STR_EQ(cw_trn("items", 1.5), "1.5 items");
  STR_EQ(cw_trn("items", 1234), "1,234 items");
  cw_l10n_set_locale("pt");
  STR_EQ(cw_trn("items", 0), "0 item");       /* pt: one is i = 0..1 */
  STR_EQ(cw_trn("items", 1000000), "1.000.000 de itens");
  STR_EQ(cw_trn("items", 7), "7 itens");
  cw_l10n_set_locale("ar");
  STR_EQ(cw_trn("items", 0), "لا عناصر");
  STR_EQ(cw_trn("items", 1), "عنصر واحد");
  STR_EQ(cw_trn("items", 2), "عنصران");
  STR_EQ(cw_trn("items", 3), "٣ عناصر");
  STR_EQ(cw_trn("items", 11), "١١ عنصرًا");
  STR_EQ(cw_trn("items", 100), "١٠٠ عنصر");
  /* a key the active catalog lacks: the fallback catalog picks its own form */
  cw_l10n_set_locale("pt-BR");
  STR_EQ(cw_trn("items", 2), "2 itens");
  cw_l10n_set_locale("en");
  const char *names[] = { "name" }, *values[] = { "Ana" };
  STR_EQ(cw_l10n_format("Hi {name}, {other} {{x}}", names, values, 1), "Hi Ana, {other} {x}");
  STR_EQ(cw_trf("greet", "name", "Ana", NULL), "Hello, Ana! {braces}");
}

static void test_formatting(void) {
  load_all();
  cw_l10n_set_locale("en");
  STR_EQ(cw_l10n_format_number(1234567.891, 2), "1,234,567.89");
  STR_EQ(cw_l10n_format_number(-0.5, 1), "-0.5");
  STR_EQ(cw_l10n_format_int(-1234), "-1,234");
  /* every digit of a 64-bit integer: a double would round these */
  STR_EQ(cw_l10n_format_int(9007199254740993LL), "9,007,199,254,740,993");
  STR_EQ(cw_l10n_format_int(-9223372036854775807LL - 1), "-9,223,372,036,854,775,808");
  STR_EQ(cw_l10n_format_date(2026, 10, 6, CW_DATE_SHORT), "10/6/26");
  STR_EQ(cw_l10n_format_date(2026, 10, 6, CW_DATE_LONG), "October 6, 2026");
  cw_l10n_set_locale("pt");
  STR_EQ(cw_l10n_format_number(1234567.891, 2), "1.234.567,89");
  STR_EQ(cw_l10n_format_date(2026, 1, 9, CW_DATE_SHORT), "09/01/2026");
  cw_l10n_set_locale("ar");
  STR_EQ(cw_l10n_format_number(1234.5, 1), "١٬٢٣٤٫٥");
  STR_EQ(cw_l10n_format_date(2026, 10, 6, CW_DATE_LONG), "٦ أكتوبر ٢٠٢٦");
}

static void test_pseudo(void) {
  load_all();
  cw_l10n_add_pseudo_locale("en-XA");
  CHECK(cw_l10n_set_locale("en-XA"));
  CHECK(cw_l10n_is_pseudo() && !cw_l10n_is_rtl());
  STR_EQ(cw_tr("menu.play"), "[Pļåý \xC2\xB7\xC2\xB7]");
  STR_EQ(cw_l10n_pseudo("Hi {name} [b]x[/b]"), "[Ĥî {name} [b]x[/b] \xC2\xB7\xC2\xB7]");
  CHECK(strstr(cw_trn("items", 2), "2 ") != NULL); /* {n} kept and substituted */
  CHECK(cw_l10n_set_locale("ar-XB"));
  CHECK(cw_l10n_is_rtl());
  STR_EQ(cw_tr("menu.play"), "\xE2\x80\x8FPlay");
  /* only translated text is pseudo-localized: hard-coded (untranslated) text
   * stays exactly as written, so it stands out in a pseudo-locale */
  STR_EQ(cw_tr_auto("menu.play"), "\xE2\x80\x8FPlay");
  STR_EQ(cw_tr_auto("Plain text."), "Plain text.");
  CHECK(cw_l10n_set_locale("en-XA"));
  STR_EQ(cw_tr_auto("menu.play"), "[Pļåý \xC2\xB7\xC2\xB7]");
  STR_EQ(cw_tr_auto("Plain text."), "Plain text.");
  STR_EQ(cw_tr_auto("Hard-coded label"), "Hard-coded label");
}

/* ar-XB through the shaper: words keep their order inside a right-to-left
 * paragraph, brackets pair up (UAX #9 N0) */
static bool ascending(const int *b, int n) {
  for (int i = 1; i < n; i++)
    if (b[i] <= b[i - 1]) return false;
  return n > 0;
}
/* the visual byte order without format marks (an RLM may draw as an empty glyph) */
static int visual(cw_font f, const char *t, const cw_text_style *st, int *out, int cap) {
  int raw[128], n = r2d_text_visual_bytes(f, t, st, raw, 128), k = 0;
  for (int i = 0; i < n && i < 128; i++)
    if (strncmp(t + raw[i], "\xE2\x80\x8F", 3) && strncmp(t + raw[i], "\xE2\x80\x8E", 3) && k < cap) out[k++] = raw[i];
  return k;
}
static void test_pseudo_bidi(void) {
  load_all();
  cw_l10n_add_pseudo_locale("ar-XB");
  CHECK(cw_l10n_set_locale("ar-XB"));
  cw_font sans = cw_ui_font_sans();
  cw_text_style rtl = { .size = 20, .direction = CW_TEXT_DIR_RTL };
  int vb[128];
  /* "Language: {name}" with ar-XB's own name: one left-to-right run, read in
   * order (an RLM inside the name made ": " a right-to-left neutral and drew
   * "Pseudo RTL :Language") */
  const char *lang = cw_trf("lang", "name", cw_l10n_locale_name("ar-XB"), NULL);
  STR_EQ(lang, "\xE2\x80\x8FLanguage: Pseudo RTL");
  STR_EQ(cw_trf("lang", "name", cw_tr("menu.play"), NULL), "\xE2\x80\x8FLanguage: Play"); /* a translated value: no inner RLM */
  const char *nm[] = { "speaker" }, *vl[] = { cw_tr("menu.play") };
  STR_EQ(cw_l10n_format("{speaker}:", nm, vl, 1), "\xE2\x80\x8FPlay:"); /* at the start: the paragraph's own mark, kept */
  int n = visual(sans, lang, &rtl, vb, 128);
  CHECK(n >= 19 && ascending(vb, n));
  n = visual(sans, lang, &(cw_text_style){ .size = 20 }, vb, 128);
  CHECK(n >= 19 && ascending(vb, n));
  /* "[a coin chimes]": the brackets take the paragraph's direction, so they
   * end up around the text: ']' (drawn mirrored as '[') leftmost, '['
   * rightmost ("a coin chimes]]" before: the closing bracket joined the text) */
  const char *br = cw_tr("bracketed"); /* RLM + "[a coin chimes]" */
  int open = 3, close = (int)strlen(br) - 1;
  n = visual(sans, br, &rtl, vb, 128);
  CHECK_EQ_INT(n, 15);
  CHECK_EQ_INT(vb[0], close);
  CHECK_EQ_INT(vb[n - 1], open);
  CHECK(ascending(vb + 1, n - 2) && vb[1] == open + 1);
  /* a one-letter pair, and a nested one: same rule */
  n = visual(sans, "\xE2\x80\x8F(x)", &rtl, vb, 128);
  CHECK(n == 3 && vb[0] == 5 && vb[1] == 4 && vb[2] == 3);
  n = visual(sans, "\xE2\x80\x8F[ab (cd) ef]", &rtl, vb, 128);
  CHECK(n == 12 && vb[0] == 14 && vb[n - 1] == 3 && ascending(vb + 1, n - 2));
  /* the same in a left-to-right paragraph (no RLM) reads in order */
  n = visual(sans, "\xD8\xA8 [a coin chimes]", &(cw_text_style){ .size = 20, .direction = CW_TEXT_DIR_LTR }, vb, 128);
  CHECK(n > 0 && ascending(vb, n));
  cw_l10n_reset();
}

static bool add_text(const char *t, const char *name) { return cw_l10n_add_catalog(t, strlen(t), name); }

static void test_parse_errors(void) {
  cw_l10n_reset();
  CHECK(!add_text("@locale = en\nno equals here\n", "bad1"));
  CHECK(!add_text("@locale = en\nk[several] = x\n", "bad2"));
  CHECK(!add_text("@locale = en\nk = a\nk = b\n", "bad3"));
  CHECK(!add_text("k = no locale\n", "bad4"));
  CHECK(!add_text("@locale = en\nbad key! = x\n", "bad5"));
  CHECK(add_text("@locale = en\n# note\nok = \\sspaced\\s\n", "good"));
  cw_l10n_set_locale("en");
  STR_EQ(cw_tr("ok"), " spaced ");
}

/* canonical text -> PO -> canonical text, byte for byte */
static void round_trip(const char *cat, const char *source) {
  char *po = cw_l10n_catalog_to_po(cat, strlen(cat), source, source ? strlen(source) : 0);
  CHECK(po != NULL);
  if (!po) return;
  char *back = cw_l10n_po_to_catalog(po, strlen(po));
  CHECK(back != NULL);
  /* the canonical form of the original */
  char *po2 = cw_l10n_catalog_to_po(back, strlen(back), source, source ? strlen(source) : 0);
  char *back2 = cw_l10n_po_to_catalog(po2, strlen(po2));
  STR_EQ(back2, back);
  STR_EQ(po2, po);
  cw_free(po);
  cw_free(back);
  cw_free(po2);
  cw_free(back2);
}

static void test_po_round_trip(void) {
  round_trip(EN, NULL);
  round_trip(AR, EN);
  round_trip(PT, EN);
  /* notes, fields with odd characters, quotes, newlines, a plural
   * category outside the language's rule (ja has only [other]) */
  const char *odd = "@locale = ja\n@name = 日本語\n@font = a.ttf, b.ttf\n# translator note\n# second line\n"
                    "q = say \"hi\"\\nnext line\\twith tab\nitems[one] = extra form\nitems[other] = {n}個\n";
  round_trip(odd, EN);
  char *po = cw_l10n_catalog_to_po(odd, strlen(odd), EN, strlen(EN));
  CHECK(po && strstr(po, "msgctxt \"q\"") && strstr(po, "#. translator note") && strstr(po, "X-Cartwheel-font: a.ttf, b.ttf") &&
        strstr(po, "Plural-Forms: nplurals=1; plural=0;") && strstr(po, "X-Cartwheel-extra[one]"));
  char *back = cw_l10n_po_to_catalog(po, strlen(po));
  /* back in as a catalog: the same lookups */
  cw_l10n_reset();
  CHECK(cw_l10n_add_catalog(back, strlen(back), "odd"));
  cw_l10n_set_locale("ja");
  STR_EQ(cw_tr("q"), "say \"hi\"\nnext line\twith tab");
  STR_EQ(cw_trn("items", 5), "5個");
  cw_free(po);
  cw_free(back);
  /* a hand-written PO (msgid as the key, fuzzy and empty entries skipped) */
  const char *hand = "msgid \"\"\nmsgstr \"\"\n\"Language: ru\\n\"\n\n#, fuzzy\nmsgid \"fz\"\nmsgstr \"нечётко\"\n\n"
                     "msgid \"empty\"\nmsgstr \"\"\n\nmsgid \"apples\"\nmsgid_plural \"apples\"\nmsgstr[0] \"{n} яблоко\"\n"
                     "msgstr[1] \"{n} яблока\"\nmsgstr[2] \"{n} яблок\"\nmsgstr[3] \"{n} яблока\"\n";
  char *cat = cw_l10n_po_to_catalog(hand, strlen(hand));
  CHECK(cat && strstr(cat, "@locale = ru") && !strstr(cat, "fz") && !strstr(cat, "empty"));
  cw_l10n_reset();
  CHECK(cat && cw_l10n_add_catalog(cat, strlen(cat), "ru"));
  cw_l10n_set_locale("ru");
  STR_EQ(cw_trn("apples", 1), "1 яблоко");
  STR_EQ(cw_trn("apples", 3), "3 яблока");
  STR_EQ(cw_trn("apples", 5), "5 яблок");
  STR_EQ(cw_trn("apples", 1.5), "1,5 яблока");
  cw_free(cat);
}

/* ── UI: mirroring, auto-translation, accessibility ─────────────────── */
static cw_tree *T;
static cw_node mk(const char *type, const char *name, cw_node parent, float mw, float mh) {
  cw_node n = cw_node_new(type, name);
  if (mw || mh) cw_control_set_custom_min_size(n, (cw_vec2){ mw, mh });
  cw_node_add_child(parent, n);
  return n;
}
static void ui_reset(void) {
  cw_scene_register_builtin_types();
  if (T) cw_tree_free(T);
  T = cw_tree_new();
  cw_set_current_tree(T);
}
static bool rect_is(cw_node n, float x, float y, float w, float h) {
  cw_rect r = cw_control_rect(n);
  FOLD(r);
  if (r.x == x && r.y == y && r.w == w && r.h == h) return true;
  cw_error("rect of %s = (%g %g %g %g), expected (%g %g %g %g)", cw_node_name(n), (double)r.x, (double)r.y, (double)r.w,
           (double)r.h, (double)x, (double)y, (double)w, (double)h);
  return false;
}
#define RECT(n, x, y, w, h) CHECK(rect_is(n, x, y, w, h))

static void test_rtl_layout(void) {
  cw_l10n_reset();
  ui_reset();
  cw_node f = mk("HBoxContainer", "Row", cw_tree_root(T), 0, 0);
  cw_control_set_rect(f, (cw_rect){ 0, 0, 400, 50 });
  cw_control_override_constant(f, "separation", 4);
  cw_node a = mk("Control", "A", f, 50, 20), b = mk("Control", "B", f, 60, 20), c = mk("Control", "C", f, 70, 20);
  cw_ui_layout(T);
  RECT(a, 0, 0, 50, 50);
  RECT(b, 54, 0, 60, 50);
  RECT(c, 118, 0, 70, 50);
  CHECK(!cw_control_is_rtl(a));
  cw_control_set_layout_direction(f, CW_LAYOUT_RTL);
  cw_ui_layout(T);
  RECT(a, 350, 0, 50, 50); /* mirrored inside the row: first child at the right */
  RECT(b, 286, 0, 60, 50);
  RECT(c, 212, 0, 70, 50);
  CHECK(cw_control_is_rtl(a));
  /* anchors mirror too (a top-left box goes top-right) */
  cw_node p = mk("Control", "Plain", cw_tree_root(T), 0, 0);
  cw_control_set_rect(p, (cw_rect){ 0, 60, 400, 100 });
  cw_node box = mk("Control", "Box", p, 0, 0);
  cw_control_set_rect(box, (cw_rect){ 10, 5, 100, 30 });
  cw_ui_layout(T);
  RECT(box, 10, 5, 100, 30);
  cw_control_set_layout_direction(p, CW_LAYOUT_RTL);
  cw_ui_layout(T);
  RECT(box, 290, 5, 100, 30);
  /* LOCALE follows the active locale; INHERITED at the top does too */
  cw_control_set_layout_direction(p, CW_LAYOUT_INHERITED);
  load_all();
  cw_l10n_set_locale("ar");
  cw_ui_layout(T);
  RECT(box, 290, 5, 100, 30);
  CHECK(cw_control_is_rtl(box));
  cw_l10n_set_locale("en");
  cw_ui_layout(T);
  RECT(box, 10, 5, 100, 30);
  /* an explicit LTR subtree inside an RTL locale stays put */
  cw_l10n_set_locale("ar");
  cw_control_set_layout_direction(p, CW_LAYOUT_LTR);
  cw_ui_layout(T);
  RECT(box, 10, 5, 100, 30);
}

static void test_auto_translate(void) {
  load_all();
  ui_reset();
  cw_node l = mk("Label", "L", cw_tree_root(T), 0, 0);
  cw_label_set_text(l, "menu.play");
  cw_l10n_set_locale("en");
  cw_ui_layout(T);
  float w_en = cw_control_min_size(l).x;
  STR_EQ(cw_control_tr(l, "menu.play"), "Play");
  cw_l10n_set_locale("pt-BR");
  cw_ui_layout(T);
  float w_pt = cw_control_min_size(l).x; /* re-measured on the locale change */
  CHECK(w_pt > w_en);
  STR_EQ(cw_control_tr(l, "menu.play"), "Jogar!");
  cw_control_set_auto_translate(l, false);
  STR_EQ(cw_control_tr(l, "menu.play"), "menu.play");
  /* text size: every theme font size scales */
  cw_control_set_auto_translate(l, true);
  cw_ui_layout(T);
  float h1 = cw_control_min_size(l).y;
  cw_ui_set_text_scale(2);
  cw_ui_layout(T);
  float h2 = cw_control_min_size(l).y;
  CHECK(h2 > h1 * 1.8f && h2 < h1 * 2.2f);
  cw_ui_set_text_scale(1);
  cw_ui_layout(T);
  CHECK(cw_control_min_size(l).y == h1);
}

static void test_a11y_tree(void) {
  load_all();
  cw_l10n_set_locale("en");
  ui_reset();
  cw_node root = mk("VBoxContainer", "Form", cw_tree_root(T), 0, 0);
  cw_control_set_rect(root, (cw_rect){ 0, 0, 400, 300 });
  cw_node row = mk("HBoxContainer", "Row", root, 0, 0);
  cw_node lab = mk("Label", "NameLabel", row, 0, 0);
  cw_label_set_text(lab, "Player name");
  cw_node edit = mk("LineEdit", "Name", row, 100, 0);
  cw_line_edit_set_text(edit, "Ana");
  cw_node btn = mk("Button", "Play", root, 0, 0);
  cw_button_set_text(btn, "menu.play");
  cw_control_set_tooltip(btn, "menu.quit");
  cw_node chk = mk("CheckBox", "Subs", root, 0, 0);
  cw_button_set_text(chk, "Subtitles");
  cw_button_set_pressed(chk, true);
  cw_node sl = mk("HSlider", "Vol", root, 100, 0);
  cw_range_set_limits(sl, 0, 10, 1);
  cw_range_set_value(sl, 7);
  cw_node dis = mk("Button", "Off", root, 0, 0);
  cw_button_set_text(dis, "Off");
  cw_button_set_disabled(dis, true);
  cw_control_grab_focus(btn);
  cw_ui_layout(T);
  cw_a11y_node nodes[32];
  int n = cw_a11y_snapshot(T, nodes, 32);
  CHECK_EQ_INT(n, 6); /* containers without a name are transparent */
  const cw_a11y_node *by[6] = { 0 };
  for (int i = 0; i < n && i < 6; i++) by[i] = &nodes[i];
  CHECK(by[0]->role == CW_ROLE_LABEL);
  STR_EQ(by[0]->name, "Player name");
  CHECK(by[1]->role == CW_ROLE_TEXTFIELD);
  STR_EQ(by[1]->name, "Player name"); /* named by the label before it */
  STR_EQ(by[1]->value, "Ana");
  CHECK(by[2]->role == CW_ROLE_BUTTON);
  STR_EQ(by[2]->name, "Play"); /* translated */
  STR_EQ(by[2]->description, "Quit");
  CHECK((by[2]->states & CW_A11Y_FOCUSED) && (by[2]->states & CW_A11Y_FOCUSABLE));
  CHECK(by[3]->role == CW_ROLE_CHECKBOX && (by[3]->states & CW_A11Y_CHECKED) && (by[3]->states & CW_A11Y_CHECKABLE));
  CHECK(by[4]->role == CW_ROLE_SLIDER && by[4]->value_now == 7 && by[4]->value_max == 10);
  STR_EQ(by[4]->value, "7");
  CHECK((by[5]->states & CW_A11Y_DISABLED) && !(by[5]->states & CW_A11Y_FOCUSABLE));
  for (int i = 0; i < n; i++) CHECK(nodes[i].parent == -1);
  /* events: focus and value changes, announcements */
  uint32_t seq = cw_a11y_event_seq();
  cw_control_grab_focus(edit);
  cw_range_set_value(sl, 3);
  cw_a11y_announce("Saved", true);
  cw_a11y_event ev[8];
  int k = cw_a11y_events_since(seq, ev, 8);
  CHECK_EQ_INT(k, 3);
  CHECK(ev[0].type == CW_A11Y_EVENT_FOCUS && ev[0].node.id == edit.id);
  CHECK(ev[1].type == CW_A11Y_EVENT_VALUE && ev[1].node.id == sl.id);
  CHECK(ev[2].type == CW_A11Y_EVENT_ANNOUNCE && ev[2].assertive);
  STR_EQ(ev[2].text, "Saved");
  /* the JSON a bridge reads */
  char *j = cw_a11y_json(T);
  CHECK(j && strstr(j, "\"role\":\"textfield\",\"name\":\"Player name\",\"value\":\"Ana\""));
  cw_free(j);
  /* an accessibility_name wins over the text */
  cw_node_set(btn, "accessibility_name", cw_val_string("menu.quit"));
  n = cw_a11y_snapshot(T, nodes, 32);
  STR_EQ(nodes[2].name, "Quit");
}

static void test_captions(void) {
  load_all();
  cw_l10n_set_locale("en");
  const char *track = "# test\n0.0 1.0 menu.play | menu.quit\n1.0 2.0 | [door]\n";
  int t = cw_captions_parse(track, strlen(track), "test");
  CHECK(t >= 0);
  CHECK(cw_captions_parse("0 1 nope\n", 9, "bad") < 0); /* no '|' */
  cw_captions_play(t, (cw_voice){ 0 }, (cw_sound){ 0 });
  cw_captions_update(0.5);
  cw_caption_line lines[4];
  int n = cw_captions_active(lines, 4);
  CHECK_EQ_INT(n, 1);
  STR_EQ(lines[0].speaker, "Play");
  STR_EQ(lines[0].text, "Quit");
  cw_captions_update(1.0);
  n = cw_captions_active(lines, 4);
  CHECK_EQ_INT(n, 1);
  STR_EQ(lines[0].speaker, "");
  STR_EQ(lines[0].text, "[door]");
  cw_captions_show("[thunder]", NULL, 0.25f);
  CHECK_EQ_INT(cw_captions_active(lines, 4), 2);
  /* the speaker label comes from the catalog: "{speaker}:" unless the locale
   * says otherwise (French puts a space before the colon) */
  STR_EQ(cw_captions_speaker_label("Narrator"), "Narrator:");
  CHECK(cw_l10n_add_catalog("@locale = fr\ncaption.speaker_label = {speaker} :\n", 49, "fr"));
  cw_l10n_set_locale("fr");
  STR_EQ(cw_captions_speaker_label("Narrateur"), "Narrateur :");
  cw_l10n_set_locale("en");
  cw_captions_update(1.0); /* the track ends at 2.0, the one-off after 0.25 s */
  CHECK_EQ_INT(cw_captions_active(lines, 4), 0);
  CHECK(!cw_captions_playing(t));
}

static void test_binding_names(void) {
  load_all();
  cw_l10n_set_locale("en");
  char buf[64];
  cw_binding b;
  CHECK(cw_binding_parse("key:space", &b));
  STR_EQ(cw_binding_display_name(b, buf, sizeof buf), "Space");
  CHECK(cw_binding_parse("key:q", &b));
  STR_EQ(cw_binding_display_name(b, buf, sizeof buf), "Q");
  CHECK(cw_binding_parse("pad:south", &b));
  STR_EQ(cw_binding_display_name(b, buf, sizeof buf), "South Button");
  CHECK(cw_binding_parse("axis:ly-@0.4", &b));
  STR_EQ(cw_binding_display_name(b, buf, sizeof buf), "Left Stick Up");
  const char *cat = "@locale = de\ninput.key.space = Leertaste\n";
  CHECK(cw_l10n_add_catalog(cat, strlen(cat), "de"));
  cw_l10n_set_locale("de");
  CHECK(cw_binding_parse("key:space", &b));
  STR_EQ(cw_binding_display_name(b, buf, sizeof buf), "Leertaste");
}

/* ── text: font fallback, shaping, CJK breaking ─────────────────────── */
static void test_text(void) {
  cw_l10n_reset();
  CHECK(cw_l10n_load_project("l10n/project.cwl10n"));
  cw_font sans = cw_ui_font_sans();
  CHECK(cw_text_is_complex("مرحبا") && cw_text_is_complex("\xE2\x80\x8F" "abc") && !cw_text_is_complex("Hello") &&
        !cw_text_is_complex("日本語"));
  CHECK(!cw_font_has_glyph(sans, 0x65E5) && !cw_font_covers(sans, 0x65E5));
  cw_l10n_set_locale("ja");
  CHECK(cw_font_covers(sans, 0x65E5)); /* 日 from the Noto Sans JP subset */
  uint32_t missing0 = cw_r2d_get_stats()->glyphs_missing;
  cw_text_metrics m = cw_font_measure(sans, "ローカライズのデモ", &(cw_text_style){ .size = 20 });
  CHECK(m.width > 100 && m.lines == 1);
  CHECK_EQ_INT(cw_r2d_get_stats()->glyphs_missing, missing0);
  /* CJK wraps between characters (no spaces needed); kinsoku keeps 。 off a line start */
  cw_text_metrics w = cw_font_measure(sans, "この画面の文字列はすべて翻訳カタログから読み込まれます。", &(cw_text_style){ .size = 20, .wrap_width = 200 });
  CHECK(w.lines >= 3 && w.width <= 200);
  FOLD(w.lines);
  /* Arabic shapes: joined letters are narrower than the same letters apart */
  cw_l10n_set_locale("ar");
  float joined = cw_font_measure(sans, "ببب", &(cw_text_style){ .size = 30 }).width;
  CHECK(joined > 0);
  float isolated = 3 * cw_font_measure(sans, "ب", &(cw_text_style){ .size = 30 }).width;
  CHECK(joined < isolated); /* initial/medial forms are narrower than three isolated ones */
  CHECK(cw_r2d_get_stats()->shaped_runs > 0);
  /* a paragraph laid out again from the shape cache still counts its runs
   * (shaped_runs read 0 for cached text), and counts as a cache hit */
  {
    uint32_t runs0 = cw_r2d_get_stats()->shaped_runs, hits0 = cw_r2d_get_stats()->shape_cache_hits;
    cw_font_measure(sans, "ببب", &(cw_text_style){ .size = 30 });
    CHECK(cw_r2d_get_stats()->shaped_runs > runs0);
    CHECK_EQ_INT(cw_r2d_get_stats()->shape_cache_hits, hits0 + 1);
  }
  /* measuring with an explicit RTL paragraph gives the same width (bidi reorders, never resizes) */
  float rtl = cw_font_measure(sans, "مرحبا 123 abc", &(cw_text_style){ .size = 20, .direction = CW_TEXT_DIR_RTL }).width;
  float autod = cw_font_measure(sans, "مرحبا 123 abc", &(cw_text_style){ .size = 20 }).width;
  CHECK(rtl == autod && rtl > 50);
  /* no fallback font: the glyphs are missing and counted */
  cw_font_set_global_fallbacks(NULL, 0);
  cw_font_measure(sans, "ㄅ", &(cw_text_style){ .size = 20 }); /* bopomofo: in no font here */
  CHECK(cw_r2d_get_stats()->glyphs_missing > missing0);
  /* per-locale assets */
  STR_EQ(cw_l10n_asset_path("textures/banner.png"), "l10n/assets/ar/textures/banner.png");
  cw_l10n_set_locale("en");
  STR_EQ(cw_l10n_asset_path("textures/banner.png"), "textures/banner.png");
  cw_l10n_reset();
}

/* ── accessibility themes ───────────────────────────────────────────── */
static bool color_is(cw_color c, float r, float g, float b) {
  return fabsf(c.r - r) < 0.01f && fabsf(c.g - g) < 0.01f && fabsf(c.b - b) < 0.01f;
}
static void test_themes(void) {
  cw_color c;
  cw_ui_style st;
  cw_theme hc = cw_theme_high_contrast(), cb = cw_theme_colorblind();
  CHECK(cw_theme_get_color(hc, "Label", "font_color", &c) && color_is(c, 1, 1, 1));
  CHECK(cw_theme_get_style(hc, "Panel", "panel", &st) && color_is(st.bg, 0, 0, 0));
  CHECK(cw_theme_get_style(hc, "Button", "focus", &st) && color_is(st.border_color, 1, 1, 0)); /* yellow focus ring */
  CHECK(cw_theme_get_style(cb, "Button", "focus", &st) && color_is(st.border_color, 0x56 / 255.0f, 0xb4 / 255.0f, 0xe9 / 255.0f));
  /* a theme can extend them by name, and saves that name */
  cw_theme t = cw_theme_parse("extends = high_contrast\nLabel.font_size = 20\n", "t");
  CHECK(!CW_HANDLE_IS_NULL(t));
  CHECK(cw_theme_get_color(t, "Label", "font_color", &c) && color_is(c, 1, 1, 1));
  char *txt = cw_theme_save(t);
  CHECK(txt && strstr(txt, "extends = high_contrast"));
  cw_free(txt);
  cw_theme_destroy(t);
  /* reduced motion is a flag games read */
  cw_ui_set_reduced_motion(true);
  CHECK(cw_ui_reduced_motion());
  cw_ui_set_reduced_motion(false);
}

CW_TEST_SUITE("l10n", { "plural_cldr", test_plural_cldr }, { "fallback", test_fallback }, { "plural_forms", test_plural_forms },
              { "formatting", test_formatting }, { "pseudo", test_pseudo }, { "pseudo_bidi", test_pseudo_bidi }, { "parse_errors", test_parse_errors },
              { "po_round_trip", test_po_round_trip }, { "rtl_layout", test_rtl_layout }, { "auto_translate", test_auto_translate },
              { "a11y_tree", test_a11y_tree }, { "captions", test_captions }, { "binding_names", test_binding_names },
              { "text", test_text }, { "themes", test_themes })
