/* Translation catalogs as data: .cwtr text, PO import/export, CLDR plural
 * rules, per-language defaults (catalog.h). */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "catalog.h"

#define TAG CW_MEM_GENERAL

const char *const cwtr_cat_names[CWTR_NCAT] = { "zero", "one", "two", "few", "many", "other" };

static int cat_from_name(const char *s, size_t n) {
  for (int i = 0; i < CWTR_NCAT; i++)
    if (strlen(cwtr_cat_names[i]) == n && !strncmp(cwtr_cat_names[i], s, n)) return i;
  return -2;
}

static uint64_t entry_hash(const char *key, int cat) { return cw_hash_bytes(key, strlen(key), (uint64_t)(cat + 2) * 0x9e3779b97f4a7c15ull); }

/* ── catalog object ─────────────────────────────────────────────────── */

static cwtr_catalog *cat_new(const char *name) {
  cwtr_catalog *c = cw_calloc(1, sizeof *c, TAG);
  c->name = cw_strdup(name ? name : "catalog", TAG);
  cw_map_init(&c->index, TAG);
  return c;
}

void cwtr_free(cwtr_catalog *c) {
  if (!c) return;
  for (int i = 0; i < c->entries.count; i++) {
    cw_free(c->entries.data[i].key);
    cw_free(c->entries.data[i].value);
    cw_free(c->entries.data[i].note);
  }
  for (int i = 0; i < c->fields.count; i++) {
    cw_free(c->fields.data[i].name);
    cw_free(c->fields.data[i].value);
  }
  cw_array_free(&c->entries);
  cw_array_free(&c->fields);
  cw_map_free(&c->index);
  cw_free(c->name);
  cw_free(c);
}

const cwtr_entry *cwtr_find(const cwtr_catalog *c, const char *key, int cat) {
  if (!c || !key) return NULL;
  uint64_t v;
  if (!cw_map_get(&c->index, entry_hash(key, cat), &v)) return NULL;
  const cwtr_entry *e = &c->entries.data[v - 1];
  return e->cat == cat && !strcmp(e->key, key) ? e : NULL;
}

bool cwtr_add(cwtr_catalog *c, const char *key, int cat, const char *value, const char *note) {
  if (cwtr_find(c, key, cat)) return false;
  cwtr_entry e = { cw_strdup(key, TAG), cat, cw_strdup(value ? value : "", TAG), note && *note ? cw_strdup(note, TAG) : NULL };
  cw_array_push(&c->entries, e);
  cw_map_set(&c->index, entry_hash(key, cat), (uint64_t)c->entries.count);
  return true;
}

const char *cwtr_field(const cwtr_catalog *c, const char *name) {
  if (!c) return NULL;
  for (int i = 0; i < c->fields.count; i++)
    if (!strcmp(c->fields.data[i].name, name)) return c->fields.data[i].value;
  return NULL;
}

void cwtr_set_field(cwtr_catalog *c, const char *name, const char *value) {
  for (int i = 0; i < c->fields.count; i++)
    if (!strcmp(c->fields.data[i].name, name)) {
      cw_free(c->fields.data[i].value);
      c->fields.data[i].value = cw_strdup(value, TAG);
      return;
    }
  cwtr_kv f = { cw_strdup(name, TAG), cw_strdup(value, TAG) };
  cw_array_push(&c->fields, f);
}

bool cwtr_equal(const cwtr_catalog *a, const cwtr_catalog *b) {
  if (a->entries.count != b->entries.count || a->fields.count != b->fields.count) return false;
  for (int i = 0; i < a->entries.count; i++) {
    const cwtr_entry *x = &a->entries.data[i], *y = cwtr_find(b, x->key, x->cat);
    if (!y || strcmp(x->value, y->value) || strcmp(x->note ? x->note : "", y->note ? y->note : "")) return false;
  }
  for (int i = 0; i < a->fields.count; i++) {
    const char *v = cwtr_field(b, a->fields.data[i].name);
    if (!v || strcmp(v, a->fields.data[i].value)) return false;
  }
  return true;
}

/* ── growable text ──────────────────────────────────────────────────── */

typedef CW_ARRAY(char) sbuf;
static void sb_put(sbuf *b, const char *s, size_t n) {
  cw_array_reserve(b, b->count + (int)n + 1);
  memcpy(b->data + b->count, s, n);
  b->count += (int)n;
  b->data[b->count] = 0;
}
static void sb_str(sbuf *b, const char *s) { sb_put(b, s, strlen(s)); }
static void sb_fmt(sbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sb_fmt(sbuf *b, const char *fmt, ...) {
  char tmp[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  if (n < (int)sizeof tmp) {
    sb_put(b, tmp, (size_t)n);
    return;
  }
  char *big = cw_alloc((size_t)n + 1, TAG);
  va_start(ap, fmt);
  vsnprintf(big, (size_t)n + 1, fmt, ap);
  va_end(ap);
  sb_put(b, big, (size_t)n);
  cw_free(big);
}
static char *sb_take(sbuf *b, size_t *len) {
  if (!b->data) sb_put(b, "", 0);
  if (len) *len = (size_t)b->count;
  return b->data;
}

/* ── .cwtr text ─────────────────────────────────────────────────────── */

static bool key_char(char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' ||
         ch == '-' || ch == ':' || ch == '/';
}

/* value text -> string: \n \t \\ \s (a space, for leading/trailing ones) */
static char *unescape(const char *s, size_t n) {
  char *o = cw_alloc(n + 1, TAG);
  size_t k = 0;
  for (size_t i = 0; i < n; i++) {
    if (s[i] == '\\' && i + 1 < n) {
      char c = s[++i];
      o[k++] = c == 'n' ? '\n' : c == 't' ? '\t' : c == 's' ? ' ' : c;
    } else o[k++] = s[i];
  }
  o[k] = 0;
  return o;
}

static void escape_into(sbuf *b, const char *v) {
  size_t n = strlen(v);
  for (size_t i = 0; i < n; i++) {
    char c = v[i];
    if (c == '\\') sb_str(b, "\\\\");
    else if (c == '\n') sb_str(b, "\\n");
    else if (c == '\t') sb_str(b, "\\t");
    else if (c == ' ' && (i == 0 || i == n - 1)) sb_str(b, "\\s");
    else sb_put(b, &c, 1);
  }
}

static void seterr(char *err, size_t cap, const char *name, int line, const char *msg) {
  if (err && cap) snprintf(err, cap, "%s:%d: %s", name ? name : "catalog", line, msg);
}

cwtr_catalog *cwtr_parse(const char *text, size_t len, const char *name, char *err, size_t errcap) {
  cwtr_catalog *c = cat_new(name);
  sbuf note = { 0 };
  int lineno = 0;
  const char *p = text, *end = text + len;
  if (len >= 3 && (uint8_t)p[0] == 0xef && (uint8_t)p[1] == 0xbb && (uint8_t)p[2] == 0xbf) p += 3; /* BOM */
  while (p < end) {
    const char *nl = memchr(p, '\n', (size_t)(end - p));
    const char *le = nl ? nl : end;
    lineno++;
    const char *s = p, *e = le;
    while (s < e && (*s == ' ' || *s == '\t')) s++;
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;
    p = nl ? nl + 1 : end;
    if (s == e) {
      cw_array_clear(&note);
      continue;
    }
    if (*s == '#') {
      const char *t = s + 1;
      if (t < e && *t == ' ') t++;
      if (note.count) sb_str(&note, "\n");
      sb_put(&note, t, (size_t)(e - t));
      continue;
    }
    const char *eq = memchr(s, '=', (size_t)(e - s));
    if (!eq) {
      seterr(err, errcap, c->name, lineno, "expected 'key = value', '@field = value' or a '#' comment");
      goto fail;
    }
    const char *ke = eq;
    while (ke > s && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
    const char *vs = eq + 1;
    while (vs < e && (*vs == ' ' || *vs == '\t')) vs++;
    char *val = unescape(vs, (size_t)(e - vs));
    if (*s == '@') {
      char fname[128];
      snprintf(fname, sizeof fname, "%.*s", (int)(ke - s - 1), s + 1);
      if (!fname[0]) {
        cw_free(val);
        seterr(err, errcap, c->name, lineno, "empty field name after '@'");
        goto fail;
      }
      cwtr_set_field(c, fname, val);
      cw_free(val);
      cw_array_clear(&note);
      continue;
    }
    const char *kb = s, *kend = ke;
    int cat = CWTR_NONE;
    const char *br = memchr(kb, '[', (size_t)(kend - kb));
    if (br) {
      if (kend[-1] != ']') {
        cw_free(val);
        seterr(err, errcap, c->name, lineno, "a plural key is 'key[category]' (zero one two few many other)");
        goto fail;
      }
      cat = cat_from_name(br + 1, (size_t)(kend - 1 - br - 1));
      if (cat < 0) {
        cw_free(val);
        seterr(err, errcap, c->name, lineno, "unknown plural category (zero one two few many other)");
        goto fail;
      }
      kend = br;
    }
    bool ok = kend > kb;
    for (const char *k = kb; k < kend; k++) ok &= key_char(*k);
    if (!ok) {
      cw_free(val);
      seterr(err, errcap, c->name, lineno, "keys are letters, digits and _ . - : /");
      goto fail;
    }
    char key[256];
    if (kend - kb >= (long)sizeof key) {
      cw_free(val);
      seterr(err, errcap, c->name, lineno, "key longer than 255 bytes");
      goto fail;
    }
    snprintf(key, sizeof key, "%.*s", (int)(kend - kb), kb);
    if (!cwtr_add(c, key, cat, val, note.count ? note.data : NULL)) {
      cw_free(val);
      char m[320];
      snprintf(m, sizeof m, "duplicate key '%s%s%s%s'", key, cat >= 0 ? "[" : "", cat >= 0 ? cwtr_cat_names[cat] : "", cat >= 0 ? "]" : "");
      seterr(err, errcap, c->name, lineno, m);
      goto fail;
    }
    cw_free(val);
    cw_array_clear(&note);
  }
  cw_array_free(&note);
  return c;
fail:
  cw_array_free(&note);
  cwtr_free(c);
  return NULL;
}

static int cmp_entry(const void *a, const void *b) {
  const cwtr_entry *x = a, *y = b;
  int k = strcmp(x->key, y->key);
  return k ? k : x->cat - y->cat;
}

static bool same_group(const char *a, const char *b) {
  size_t la = strcspn(a, "."), lb = strcspn(b, ".");
  return la == lb && !strncmp(a, b, la);
}

static const cwtr_entry *sorted_entries(const cwtr_catalog *c) {
  cwtr_entry *s = cw_alloc(sizeof *s * (size_t)(c->entries.count + 1), TAG);
  if (c->entries.count) memcpy(s, c->entries.data, sizeof *s * (size_t)c->entries.count);
  qsort(s, (size_t)c->entries.count, sizeof *s, cmp_entry);
  return s;
}

char *cwtr_write(const cwtr_catalog *c, size_t *len) {
  sbuf b = { 0 };
  const char *loc = cwtr_field(c, "locale");
  if (loc) {
    sb_str(&b, "@locale = ");
    escape_into(&b, loc);
    sb_str(&b, "\n");
  }
  for (int i = 0; i < c->fields.count; i++) {
    if (!strcmp(c->fields.data[i].name, "locale")) continue;
    sb_fmt(&b, "@%s = ", c->fields.data[i].name);
    escape_into(&b, c->fields.data[i].value);
    sb_str(&b, "\n");
  }
  const cwtr_entry *s = sorted_entries(c);
  for (int i = 0; i < c->entries.count; i++) {
    const cwtr_entry *e = &s[i];
    /* a blank line before each group of keys sharing their first segment
     * ("menu.*"), and before a note */
    if (!i || e->note || !same_group(s[i - 1].key, e->key)) sb_str(&b, b.count ? "\n" : "");
    if (e->note) {
      for (const char *n = e->note; n;) {
        const char *nl = strchr(n, '\n');
        sb_str(&b, "# ");
        sb_put(&b, n, nl ? (size_t)(nl - n) : strlen(n));
        sb_str(&b, "\n");
        n = nl ? nl + 1 : NULL;
      }
    }
    sb_str(&b, e->key);
    if (e->cat >= 0) sb_fmt(&b, "[%s]", cwtr_cat_names[e->cat]);
    sb_str(&b, " = ");
    escape_into(&b, e->value);
    sb_str(&b, "\n");
  }
  cw_free((void *)s);
  return sb_take(&b, len);
}

/* ── PO ─────────────────────────────────────────────────────────────── */

static void po_quote(sbuf *b, const char *v) {
  sb_str(b, "\"");
  for (const char *p = v; *p; p++) {
    if (*p == '"') sb_str(b, "\\\"");
    else if (*p == '\\') sb_str(b, "\\\\");
    else if (*p == '\n') sb_str(b, "\\n");
    else if (*p == '\t') sb_str(b, "\\t");
    else sb_put(b, p, 1);
  }
  sb_str(b, "\"");
}

/* a field name as an X- header: "number.decimal" -> "X-Cartwheel-number.decimal" */
char *cwtr_write_po(const cwtr_catalog *c, const cwtr_catalog *source, size_t *len) {
  sbuf b = { 0 };
  const char *loc = cwtr_field(c, "locale");
  const cwtr_lang *L = cwtr_lang_info(loc ? loc : "en");
  sb_str(&b, "# Generated by cartwheel from a .cwtr catalog; msgctxt is the key.\nmsgid \"\"\nmsgstr \"\"\n");
  sb_fmt(&b, "\"Language: %s\\n\"\n", loc ? loc : "");
  sb_str(&b, "\"MIME-Version: 1.0\\n\"\n\"Content-Type: text/plain; charset=UTF-8\\n\"\n\"Content-Transfer-Encoding: 8bit\\n\"\n");
  sb_fmt(&b, "\"Plural-Forms: %s\\n\"\n", cwtr_rule_po(L->rule));
  for (int i = 0; i < c->fields.count; i++) {
    if (!strcmp(c->fields.data[i].name, "locale")) continue;
    sbuf t = { 0 };
    sb_fmt(&t, "X-Cartwheel-%s: %s\n", c->fields.data[i].name, c->fields.data[i].value);
    po_quote(&b, t.data);
    sb_str(&b, "\n");
    cw_array_free(&t);
  }
  int cats[CWTR_NCAT], nc = cwtr_rule_categories(L->rule, cats);
  const cwtr_lang *SL = cwtr_lang_info(source && cwtr_field(source, "locale") ? cwtr_field(source, "locale") : "en");
  const cwtr_entry *s = sorted_entries(c);
  for (int i = 0; i < c->entries.count;) {
    const cwtr_entry *e = &s[i];
    int j = i;
    while (j < c->entries.count && !strcmp(s[j].key, e->key)) j++;
    sb_str(&b, "\n");
    for (int k = i; k < j; k++)
      if (s[k].note)
        for (const char *n = s[k].note; n;) {
          const char *nl = strchr(n, '\n');
          sb_str(&b, "#. ");
          sb_put(&b, n, nl ? (size_t)(nl - n) : strlen(n));
          sb_str(&b, "\n");
          n = nl ? nl + 1 : NULL;
        }
    /* plural categories outside the language's rule have no PO slot: keep them */
    for (int k = i; k < j; k++) {
      if (s[k].cat < 0) continue;
      bool in = false;
      for (int q = 0; q < nc; q++) in |= cats[q] == s[k].cat;
      if (!in) {
        sb_fmt(&b, "# X-Cartwheel-extra[%s] ", cwtr_cat_names[s[k].cat]);
        po_quote(&b, s[k].value);
        sb_str(&b, "\n");
      }
    }
    sb_str(&b, "msgctxt ");
    po_quote(&b, e->key);
    sb_str(&b, "\n");
    if (e->cat < 0) {
      const cwtr_entry *se = source ? cwtr_find(source, e->key, CWTR_NONE) : NULL;
      sb_str(&b, "msgid ");
      po_quote(&b, se ? se->value : e->key);
      sb_str(&b, "\nmsgstr ");
      po_quote(&b, e->value);
      sb_str(&b, "\n");
    } else {
      /* msgid / msgid_plural: the source's one and other forms */
      int so = CWTR_ONE, sp = CWTR_OTHER;
      (void)SL;
      const cwtr_entry *s1 = source ? cwtr_find(source, e->key, so) : NULL, *s2 = source ? cwtr_find(source, e->key, sp) : NULL;
      sb_str(&b, "msgid ");
      po_quote(&b, s1 ? s1->value : e->key);
      sb_str(&b, "\nmsgid_plural ");
      po_quote(&b, s2 ? s2->value : e->key);
      sb_str(&b, "\n");
      for (int k = 0; k < nc; k++) {
        const cwtr_entry *f = cwtr_find(c, e->key, cats[k]);
        sb_fmt(&b, "msgstr[%d] ", k);
        po_quote(&b, f ? f->value : "");
        sb_str(&b, "\n");
      }
    }
    i = j;
  }
  cw_free((void *)s);
  return sb_take(&b, len);
}

/* one PO string literal "..." starting at *p (after spaces); appends to out */
static bool po_string(const char **pp, const char *end, sbuf *out) {
  const char *p = *pp;
  while (p < end && (*p == ' ' || *p == '\t')) p++;
  if (p >= end || *p != '"') return false;
  p++;
  while (p < end && *p != '"') {
    if (*p == '\\' && p + 1 < end) {
      p++;
      char c = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p == 'r' ? '\r' : *p;
      sb_put(out, &c, 1);
    } else sb_put(out, p, 1);
    p++;
  }
  if (p >= end) return false;
  *pp = p + 1;
  return true;
}

typedef struct po_msg {
  sbuf ctxt, id, id_plural, str[CWTR_NCAT], notes, extra[CWTR_NCAT];
  bool has_extra[CWTR_NCAT];
  bool has_ctxt, has_id, plural, fuzzy;
  int nstr;
} po_msg;

static void po_reset(po_msg *m) {
  cw_array_free(&m->ctxt);
  cw_array_free(&m->id);
  cw_array_free(&m->id_plural);
  cw_array_free(&m->notes);
  for (int i = 0; i < CWTR_NCAT; i++) cw_array_free(&m->str[i]), cw_array_free(&m->extra[i]);
  memset(m, 0, sizeof *m);
}

static const char *sbs(const sbuf *b) { return b->data ? b->data : ""; }

static void po_header(cwtr_catalog *c, const char *h, int *rule) {
  for (const char *line = h; line && *line;) {
    const char *nl = strchr(line, '\n');
    size_t n = nl ? (size_t)(nl - line) : strlen(line);
    char buf[1024];
    snprintf(buf, sizeof buf, "%.*s", (int)(n < sizeof buf ? n : sizeof buf - 1), line);
    char *colon = strchr(buf, ':');
    if (colon) {
      *colon = 0;
      char *v = colon + 1;
      while (*v == ' ') v++;
      if (!strcmp(buf, "Language") && *v) cwtr_set_field(c, "locale", v);
      else if (!strncmp(buf, "X-Cartwheel-", 12)) cwtr_set_field(c, buf + 12, v);
    }
    line = nl ? nl + 1 : NULL;
  }
  const char *loc = cwtr_field(c, "locale");
  *rule = cwtr_lang_info(loc ? loc : "en")->rule;
}

cwtr_catalog *cwtr_parse_po(const char *text, size_t len, const char *name, char *err, size_t errcap) {
  cwtr_catalog *c = cat_new(name);
  po_msg m = { 0 };
  int rule = cwtr_lang_info("en")->rule, lineno = 0;
  sbuf *cur = NULL;
  const char *p = text, *end = text + len;
  bool done = false;
  while (!done) {
    const char *nl = p < end ? memchr(p, '\n', (size_t)(end - p)) : NULL;
    const char *le = nl ? nl : end;
    bool eof = p >= end;
    lineno++;
    const char *s = p;
    while (s < le && (*s == ' ' || *s == '\t' || *s == '\r')) s++;
    const char *e = le;
    while (e > s && (e[-1] == '\r' || e[-1] == ' ')) e--;
    bool starts_msg = !eof && (!strncmp(s, "msgctxt", 7) || (!strncmp(s, "msgid", 5) && strncmp(s, "msgid_plural", 12)) || *s == '#');
    bool blank = eof || s == e;
    /* an entry ends at a blank line, a comment or a new msgctxt/msgid after a msgstr */
    if ((blank || (starts_msg && m.nstr)) && m.has_id) {
      if (!m.id.count) {
        po_header(c, sbs(&m.str[0]), &rule);
      } else if (!m.fuzzy) {
        const char *key = m.has_ctxt ? sbs(&m.ctxt) : sbs(&m.id);
        const char *note = m.notes.count ? m.notes.data : NULL;
        if (m.plural) {
          int cats[CWTR_NCAT], nc = cwtr_rule_categories(rule, cats);
          for (int k = 0; k < m.nstr && k < nc; k++)
            if (m.str[k].count) {
              cwtr_add(c, key, cats[k], sbs(&m.str[k]), note);
              note = NULL;
            }
          for (int k = 0; k < CWTR_NCAT; k++)
            if (m.has_extra[k]) cwtr_add(c, key, k, sbs(&m.extra[k]), NULL);
        } else if (m.str[0].count) {
          if (!cwtr_add(c, key, CWTR_NONE, sbs(&m.str[0]), note)) {
            seterr(err, errcap, c->name, lineno, "duplicate msgctxt/msgid");
            po_reset(&m);
            cwtr_free(c);
            return NULL;
          }
        }
      }
      po_reset(&m);
      cur = NULL;
    }
    if (eof) break;
    p = nl ? nl + 1 : end;
    if (blank) continue;
    if (*s == '#') {
      if (s + 1 < e && s[1] == ',') {
        if (strstr(s, "fuzzy") && (size_t)(strstr(s, "fuzzy") - s) < (size_t)(e - s)) m.fuzzy = true;
      } else if (s + 1 < e && s[1] == '.') {
        const char *t = s + 2;
        if (t < e && *t == ' ') t++;
        if (m.notes.count) sb_str(&m.notes, "\n");
        sb_put(&m.notes, t, (size_t)(e - t));
      } else if (!strncmp(s, "# X-Cartwheel-extra[", 20)) {
        /* a category outside the rule (written by cwtr_write_po) */
        const char *cb = s + 20, *ce = memchr(cb, ']', (size_t)(e - cb));
        int cat = ce ? cat_from_name(cb, (size_t)(ce - cb)) : -2;
        const char *q = ce ? ce + 1 : e;
        if (cat >= 0 && po_string(&q, e, &m.extra[cat])) {
          if (!m.extra[cat].data) sb_put(&m.extra[cat], "", 0);
          m.has_extra[cat] = true;
        }
      }
      continue;
    }
    const char *q = s;
    if (!strncmp(s, "msgctxt", 7)) {
      q = s + 7;
      cur = &m.ctxt;
      m.has_ctxt = true;
    } else if (!strncmp(s, "msgid_plural", 12)) {
      q = s + 12;
      cur = &m.id_plural;
      m.plural = true;
    } else if (!strncmp(s, "msgid", 5)) {
      q = s + 5;
      cur = &m.id;
      m.has_id = true;
    } else if (!strncmp(s, "msgstr[", 7)) {
      int k = atoi(s + 7);
      const char *rb = strchr(s, ']');
      if (k < 0 || k >= CWTR_NCAT || !rb) {
        seterr(err, errcap, c->name, lineno, "msgstr[N] index out of range");
        goto fail;
      }
      q = rb + 1;
      cur = &m.str[k];
      if (k + 1 > m.nstr) m.nstr = k + 1;
    } else if (!strncmp(s, "msgstr", 6)) {
      q = s + 6;
      cur = &m.str[0];
      if (!m.nstr) m.nstr = 1;
    } else if (*s != '"') {
      seterr(err, errcap, c->name, lineno, "expected msgctxt, msgid, msgid_plural, msgstr, a string or a comment");
      goto fail;
    }
    if (!cur || !po_string(&q, e, cur)) {
      seterr(err, errcap, c->name, lineno, "expected a \"string\"");
      goto fail;
    }
    if (!cur->data) sb_put(cur, "", 0);
  }
  po_reset(&m);
  return c;
fail:
  po_reset(&m);
  cwtr_free(c);
  return NULL;
}

/* ── languages and plural rules ─────────────────────────────────────── */

enum {
  R_OTHER,     /* ja zh ko th id ms lo my km ... */
  R_I1V0,      /* en de nl sv fi et: one i=1 and v=0 */
  R_N1,        /* el hu tr nb bg ...: one n=1 */
  R_FR,        /* fr pt: one i=0..1; many (millions) */
  R_IT,        /* it ca gl pt-PT: one i=1 and v=0; many (millions) */
  R_ES,        /* es: one n=1; many (millions) */
  R_RU,        /* ru uk */
  R_PL,        /* pl */
  R_CS,        /* cs sk */
  R_AR,        /* ar */
  R_HE,        /* he */
  R_HI,        /* hi bn fa gu am vi zu: one i=0 or n=1 */
  R_RO,        /* ro */
  R_DA,        /* da: one n=1 or t!=0 and i=0,1 */
  R_COUNT
};

void cwtr_language(const char *locale, char *out, size_t cap) {
  size_t n = 0;
  while (locale && locale[n] && locale[n] != '-' && locale[n] != '_' && n + 1 < cap) {
    char ch = locale[n];
    out[n++] = (char)(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
  }
  out[n] = 0;
}

/* Number separators and date patterns are CLDR's for the language's default
 * region (the basics; a catalog overrides any of them with @fields). */
static const cwtr_lang langs[] = {
  { "en", R_I1V0, false, ".", ",", "latn", "M/d/yy", "MMM d, y", "MMMM d, y" },
  { "de", R_I1V0, false, ",", ".", "latn", "dd.MM.yy", "dd.MM.y", "d. MMMM y" },
  { "nl", R_I1V0, false, ",", ".", "latn", "dd-MM-y", "d MMM y", "d MMMM y" },
  { "sv", R_I1V0, false, ",", "\xC2\xA0", "latn", "y-MM-dd", "d MMM y", "d MMMM y" },
  { "fi", R_I1V0, false, ",", "\xC2\xA0", "latn", "d.M.y", "d.M.y", "d. MMMM y" },
  { "et", R_I1V0, false, ",", "\xC2\xA0", "latn", "dd.MM.yy", "d. MMM y", "d. MMMM y" },
  { "fr", R_FR, false, ",", "\xE2\x80\xAF", "latn", "dd/MM/y", "d MMM y", "d MMMM y" },
  { "pt", R_FR, false, ",", ".", "latn", "dd/MM/y", "d 'de' MMM 'de' y", "d 'de' MMMM 'de' y" },
  { "it", R_IT, false, ",", ".", "latn", "dd/MM/yy", "d MMM y", "d MMMM y" },
  { "ca", R_IT, false, ",", ".", "latn", "d/M/yy", "d MMM y", "d MMMM 'de' y" },
  { "es", R_ES, false, ",", ".", "latn", "d/M/yy", "d MMM y", "d 'de' MMMM 'de' y" },
  { "el", R_N1, false, ",", ".", "latn", "d/M/yy", "d MMM y", "d MMMM y" },
  { "hu", R_N1, false, ",", "\xC2\xA0", "latn", "y. MM. dd.", "y. MMM d.", "y. MMMM d." },
  { "tr", R_N1, false, ",", ".", "latn", "d.MM.y", "d MMM y", "d MMMM y" },
  { "nb", R_N1, false, ",", "\xC2\xA0", "latn", "dd.MM.y", "d. MMM y", "d. MMMM y" },
  { "bg", R_N1, false, ",", "\xC2\xA0", "latn", "d.MM.yy", "d.MM.y", "d MMMM y" },
  { "da", R_DA, false, ",", ".", "latn", "dd.MM.y", "d. MMM y", "d. MMMM y" },
  { "ru", R_RU, false, ",", "\xC2\xA0", "latn", "dd.MM.y", "d MMM y", "d MMMM y" },
  { "uk", R_RU, false, ",", "\xC2\xA0", "latn", "dd.MM.yy", "d MMM y", "d MMMM y" },
  { "pl", R_PL, false, ",", "\xC2\xA0", "latn", "d.MM.y", "d MMM y", "d MMMM y" },
  { "cs", R_CS, false, ",", "\xC2\xA0", "latn", "dd.MM.yy", "d. M. y", "d. MMMM y" },
  { "sk", R_CS, false, ",", "\xC2\xA0", "latn", "d. M. y", "d. M. y", "d. MMMM y" },
  { "ro", R_RO, false, ",", ".", "latn", "dd.MM.y", "d MMM y", "d MMMM y" },
  { "ar", R_AR, true, "\xD9\xAB", "\xD9\xAC", "arab", "d\xE2\x80\x8F/M\xE2\x80\x8F/y", "dd\xE2\x80\x8F/MM\xE2\x80\x8F/y", "d MMMM y" },
  { "he", R_HE, true, ".", ",", "latn", "d.M.y", "d MMM y", "d MMMM y" },
  { "fa", R_HI, true, "\xD9\xAB", "\xD9\xAC", "latn", "y/M/d", "d MMM y", "d MMMM y" },
  { "ur", R_I1V0, true, ".", ",", "latn", "d/M/yy", "d MMM y", "d MMMM y" },
  { "hi", R_HI, false, ".", ",", "latn", "d/M/yy", "d MMM y", "d MMMM y" },
  { "bn", R_HI, false, ".", ",", "latn", "d/M/yy", "d MMM y", "d MMMM y" },
  { "vi", R_HI, false, ",", ".", "latn", "dd/MM/y", "d MMM, y", "d MMMM, y" },
  { "ja", R_OTHER, false, ".", ",", "latn", "y/MM/dd", "y/MM/dd", "y\xE5\xB9\xB4M\xE6\x9C\x88" "d\xE6\x97\xA5" },
  { "zh", R_OTHER, false, ".", ",", "latn", "y/M/d", "y\xE5\xB9\xB4M\xE6\x9C\x88" "d\xE6\x97\xA5", "y\xE5\xB9\xB4M\xE6\x9C\x88" "d\xE6\x97\xA5" },
  { "ko", R_OTHER, false, ".", ",", "latn", "yy. M. d.", "y. M. d.", "y\xEB\x85\x84 M\xEC\x9B\x94 d\xEC\x9D\xBC" },
  { "th", R_OTHER, false, ".", ",", "latn", "d/M/yy", "d MMM y", "d MMMM y" },
  { "id", R_OTHER, false, ",", ".", "latn", "dd/MM/yy", "d MMM y", "d MMMM y" },
  { "ms", R_OTHER, false, ".", ",", "latn", "d/MM/yy", "d MMM y", "d MMMM y" },
};

const cwtr_lang *cwtr_lang_info(const char *locale) {
  char l[16];
  cwtr_language(locale ? locale : "en", l, sizeof l);
  /* pt-PT plurals follow Italian's rule */
  if (locale && !strcmp(l, "pt") && (strstr(locale, "PT") || strstr(locale, "pt-pt"))) {
    static cwtr_lang pt_pt;
    for (size_t i = 0; i < CW_ARRAY_LEN(langs); i++)
      if (!strcmp(langs[i].lang, "pt")) pt_pt = langs[i];
    pt_pt.rule = R_IT;
    return &pt_pt;
  }
  for (size_t i = 0; i < CW_ARRAY_LEN(langs); i++)
    if (!strcmp(langs[i].lang, l)) return &langs[i];
  return &langs[0];
}

/* CLDR operands from n and the visible fraction digit count v */
typedef struct { double n; long long i; int v; long long f, t; } ops;
static ops operands(double n, int v) {
  ops o;
  o.n = fabs(n);
  o.i = (long long)floor(o.n);
  o.v = v < 0 ? 0 : v > 9 ? 9 : v;
  long long p = 1;
  for (int k = 0; k < o.v; k++) p *= 10;
  o.f = (long long)llround((o.n - (double)o.i) * (double)p);
  o.t = o.f;
  while (o.t && o.t % 10 == 0) o.t /= 10;
  return o;
}

static bool n_is(const ops *o, double x) { return o->n == x; }
/* n % m within [a, b] (n integral: the CLDR n is the full value) */
static bool n_mod_in(const ops *o, long long m, long long a, long long b) {
  if (o->n != floor(o->n)) return false;
  long long r = (long long)fmod(o->n, (double)m);
  return r >= a && r <= b;
}

int cwtr_plural(int rule, double num, int v) {
  ops o = operands(num, v);
  long long i10 = o.i % 10, i100 = o.i % 100;
  switch (rule) {
  case R_I1V0: return o.i == 1 && o.v == 0 ? CWTR_ONE : CWTR_OTHER;
  case R_N1: return n_is(&o, 1) ? CWTR_ONE : CWTR_OTHER;
  case R_FR:
    if (o.i <= 1) return CWTR_ONE;
    return o.i != 0 && o.i % 1000000 == 0 && o.v == 0 ? CWTR_MANY : CWTR_OTHER;
  case R_IT:
    if (o.i == 1 && o.v == 0) return CWTR_ONE;
    return o.i != 0 && o.i % 1000000 == 0 && o.v == 0 ? CWTR_MANY : CWTR_OTHER;
  case R_ES:
    if (n_is(&o, 1)) return CWTR_ONE;
    return o.i != 0 && o.i % 1000000 == 0 && o.v == 0 ? CWTR_MANY : CWTR_OTHER;
  case R_RU:
    if (o.v) return CWTR_OTHER;
    if (i10 == 1 && i100 != 11) return CWTR_ONE;
    if (i10 >= 2 && i10 <= 4 && !(i100 >= 12 && i100 <= 14)) return CWTR_FEW;
    return CWTR_MANY;
  case R_PL:
    if (o.i == 1 && o.v == 0) return CWTR_ONE;
    if (o.v) return CWTR_OTHER;
    if (i10 >= 2 && i10 <= 4 && !(i100 >= 12 && i100 <= 14)) return CWTR_FEW;
    return CWTR_MANY;
  case R_CS:
    if (o.i == 1 && o.v == 0) return CWTR_ONE;
    if (o.i >= 2 && o.i <= 4 && o.v == 0) return CWTR_FEW;
    return o.v ? CWTR_MANY : CWTR_OTHER;
  case R_AR:
    if (n_is(&o, 0)) return CWTR_ZERO;
    if (n_is(&o, 1)) return CWTR_ONE;
    if (n_is(&o, 2)) return CWTR_TWO;
    if (n_mod_in(&o, 100, 3, 10)) return CWTR_FEW;
    if (n_mod_in(&o, 100, 11, 99)) return CWTR_MANY;
    return CWTR_OTHER;
  case R_HE:
    if ((o.i == 1 && o.v == 0) || (o.i == 0 && o.v != 0)) return CWTR_ONE;
    if (o.i == 2 && o.v == 0) return CWTR_TWO;
    return CWTR_OTHER;
  case R_HI: return o.i == 0 || n_is(&o, 1) ? CWTR_ONE : CWTR_OTHER;
  case R_RO:
    if (o.i == 1 && o.v == 0) return CWTR_ONE;
    if (o.v != 0 || n_is(&o, 0) || (!n_is(&o, 1) && n_mod_in(&o, 100, 1, 19))) return CWTR_FEW;
    return CWTR_OTHER;
  case R_DA: return n_is(&o, 1) || (o.t != 0 && o.i <= 1) ? CWTR_ONE : CWTR_OTHER;
  default: return CWTR_OTHER;
  }
}

int cwtr_plural_for(const char *locale, double n, int v) { return cwtr_plural(cwtr_lang_info(locale)->rule, n, v); }

int cwtr_rule_categories(int rule, int out[CWTR_NCAT]) {
  static const char *const sets[R_COUNT] = {
    [R_OTHER] = "5", [R_I1V0] = "15", [R_N1] = "15", [R_FR] = "145", [R_IT] = "145", [R_ES] = "145", [R_RU] = "1345",
    [R_PL] = "1345", [R_CS] = "1345", [R_AR] = "012345", [R_HE] = "125", [R_HI] = "15", [R_RO] = "135", [R_DA] = "15",
  };
  const char *s = rule >= 0 && rule < R_COUNT && sets[rule] ? sets[rule] : "5";
  int n = 0;
  for (; s[n]; n++) out[n] = s[n] - '0';
  return n;
}

const char *cwtr_rule_po(int rule) {
  switch (rule) {
  case R_OTHER: return "nplurals=1; plural=0;";
  case R_I1V0: case R_N1: case R_DA: return "nplurals=2; plural=(n != 1);";
  case R_HI: return "nplurals=2; plural=(n > 1);";
  case R_FR: return "nplurals=3; plural=(n == 0 || n == 1) ? 0 : n != 0 && n % 1000000 == 0 ? 1 : 2;";
  case R_IT: case R_ES: return "nplurals=3; plural=(n == 1) ? 0 : n != 0 && n % 1000000 == 0 ? 1 : 2;";
  case R_RU:
    return "nplurals=4; plural=(n % 10 == 1 && n % 100 != 11) ? 0 : (n % 10 >= 2 && n % 10 <= 4 && (n % 100 < 12 || n % 100 > 14)) ? 1 : 2;";
  case R_PL: return "nplurals=4; plural=(n == 1) ? 0 : (n % 10 >= 2 && n % 10 <= 4 && (n % 100 < 12 || n % 100 > 14)) ? 1 : 2;";
  case R_CS: return "nplurals=4; plural=(n == 1) ? 0 : (n >= 2 && n <= 4) ? 1 : 3;";
  case R_AR:
    return "nplurals=6; plural=(n == 0) ? 0 : (n == 1) ? 1 : (n == 2) ? 2 : (n % 100 >= 3 && n % 100 <= 10) ? 3 : (n % 100 >= 11) ? 4 : 5;";
  case R_HE: return "nplurals=3; plural=(n == 1) ? 0 : (n == 2) ? 1 : 2;";
  case R_RO: return "nplurals=3; plural=(n == 1) ? 0 : (n == 0 || (n % 100 > 0 && n % 100 < 20)) ? 1 : 2;";
  default: return "nplurals=2; plural=(n != 1);";
  }
}
