#include <errno.h>
#include <stdarg.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cwpack.h"

/* ── settings ────────────────────────────────────────────────────── */
void cwp_settings_set(cwp_settings *s, const char *key, const char *value) {
  for (int i = 0; i < s->count; i++)
    if (!strcmp(s->kv[i].key, key)) {
      cw_free(s->kv[i].value);
      s->kv[i].value = cw_strdup(value, CW_MEM_GENERAL);
      return;
    }
  if (s->count == s->cap) {
    s->cap = s->cap ? s->cap * 2 : 16;
    s->kv = cw_realloc(s->kv, (size_t)s->cap * sizeof *s->kv, CW_MEM_GENERAL);
  }
  s->kv[s->count].key = cw_strdup(key, CW_MEM_GENERAL);
  s->kv[s->count].value = cw_strdup(value, CW_MEM_GENERAL);
  s->count++;
}
const char *cwp_settings_get(const cwp_settings *s, const char *key, const char *def) {
  for (int i = 0; i < s->count; i++)
    if (!strcmp(s->kv[i].key, key)) return s->kv[i].value;
  return def;
}
bool cwp_settings_bool(const cwp_settings *s, const char *key, bool def) {
  const char *v = cwp_settings_get(s, key, NULL);
  if (!v) return def;
  return !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes") || !strcmp(v, "on");
}
double cwp_settings_num(const cwp_settings *s, const char *key, double def) {
  const char *v = cwp_settings_get(s, key, NULL);
  return v ? strtod(v, NULL) : def;
}
void cwp_settings_copy(cwp_settings *d, const cwp_settings *s) {
  for (int i = 0; i < s->count; i++) cwp_settings_set(d, s->kv[i].key, s->kv[i].value);
}
void cwp_settings_free(cwp_settings *s) {
  for (int i = 0; i < s->count; i++) { cw_free(s->kv[i].key); cw_free(s->kv[i].value); }
  cw_free(s->kv);
  memset(s, 0, sizeof *s);
}
static int kv_cmp(const void *a, const void *b) { return strcmp(((const cwp_kv *)a)->key, ((const cwp_kv *)b)->key); }
char *cwp_settings_canon(const cwp_settings *s, const char *const *prefixes, int n) {
  cwp_kv *sorted = cw_alloc((size_t)(s->count + 1) * sizeof *sorted, CW_MEM_GENERAL);
  int k = 0;
  for (int i = 0; i < s->count; i++)
    for (int p = 0; p < n; p++)
      if (!strncmp(s->kv[i].key, prefixes[p], strlen(prefixes[p]))) { sorted[k++] = s->kv[i]; break; }
  qsort(sorted, (size_t)k, sizeof *sorted, kv_cmp);
  size_t len = 1;
  for (int i = 0; i < k; i++) len += strlen(sorted[i].key) + strlen(sorted[i].value) + 2;
  char *o = cw_alloc(len, CW_MEM_GENERAL), *w = o;
  *w = 0;
  for (int i = 0; i < k; i++) w += sprintf(w, "%s=%s\n", sorted[i].key, sorted[i].value);
  cw_free(sorted);
  return o;
}

/* ── files ───────────────────────────────────────────────────────── */
uint8_t *cwp_read_file(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  struct stat st;
  if (fstat(fileno(f), &st) || !S_ISREG(st.st_mode)) { fclose(f); return NULL; }
  size_t n = (size_t)st.st_size;
  uint8_t *b = cw_alloc(n + 1, CW_MEM_GENERAL);
  if (fread(b, 1, n, f) != n) { fclose(f); cw_free(b); return NULL; }
  fclose(f);
  b[n] = 0;
  *len = n;
  return b;
}

bool cwp_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}

bool cwp_mkdirs(const char *dir) {
  char p[CWP_PATH_MAX];
  snprintf(p, sizeof p, "%s", dir);
  size_t n = strlen(p);
  while (n > 1 && p[n - 1] == '/') p[--n] = 0;
  for (char *c = p + 1; *c; c++) {
    if (*c != '/') continue;
    *c = 0;
    if (mkdir(p, 0755) && errno != EEXIST) return false;
    *c = '/';
  }
  return !mkdir(p, 0755) || errno == EEXIST;
}

void cwp_dirname(const char *path, char *out, size_t cap) {
  const char *s = strrchr(path, '/');
  snprintf(out, cap, "%.*s", s ? (int)(s - path + 1) : 0, path);
}

bool cwp_write_file(const char *path, const void *data, size_t len) {
  char dir[CWP_PATH_MAX], tmp[CWP_PATH_MAX + 32];
  cwp_dirname(path, dir, sizeof dir);
  if (dir[0] && !cwp_mkdirs(dir)) return false;
#if defined(__wasi__)
  snprintf(tmp, sizeof tmp, "%s.tmp", path); /* a cart (the importers in the editor cart) has no process ids */
#else
  snprintf(tmp, sizeof tmp, "%s.tmp%d", path, (int)getpid());
#endif
  FILE *f = fopen(tmp, "wb");
  if (!f) return false;
  bool ok = fwrite(data, 1, len, f) == len;
  ok &= fclose(f) == 0;
  if (ok) ok = rename(tmp, path) == 0;
  if (!ok) remove(tmp);
  return ok;
}

void cwp_join(const char *dir, const char *rel, char *out, size_t cap) {
  char buf[CWP_PATH_MAX * 2];
  if (rel[0] == '/') snprintf(buf, sizeof buf, "%s", rel);
  else snprintf(buf, sizeof buf, "%s%s%s", dir, dir[0] && dir[strlen(dir) - 1] != '/' ? "/" : "", rel);
  /* normalize segments */
  char *segs[512];
  int n = 0;
  bool abs = buf[0] == '/';
  for (char *t = strtok(buf, "/"); t && n < 512; t = strtok(NULL, "/")) {
    if (!strcmp(t, ".")) continue;
    if (!strcmp(t, "..") && n > 0 && strcmp(segs[n - 1], "..")) { n--; continue; }
    segs[n++] = t;
  }
  size_t k = 0;
  out[0] = 0;
  if (abs && k + 1 < cap) out[k++] = '/';
  for (int i = 0; i < n; i++) {
    int w = snprintf(out + k, cap - k, "%s%s", i ? "/" : "", segs[i]);
    if (w < 0 || (size_t)w >= cap - k) break;
    k += (size_t)w;
  }
  out[k < cap ? k : cap - 1] = 0;
}

void cwp_absolute(const char *path, char *out, size_t cap) {
  char r[PATH_MAX];
  if (realpath(path, r)) { snprintf(out, cap, "%s", r); return; }
  char cwd[PATH_MAX];
  if (path[0] != '/' && getcwd(cwd, sizeof cwd)) cwp_join(cwd, path, out, cap);
  else snprintf(out, cap, "%s", path);
}

void cwp_uri_decode(const char *s, char *o, size_t cap) {
  size_t k = 0;
  for (size_t i = 0; s[i] && k + 1 < cap; i++) {
    if (s[i] == '%' && s[i + 1] && s[i + 2]) {
      char h[3] = { s[i + 1], s[i + 2], 0 };
      o[k++] = (char)strtol(h, NULL, 16);
      i += 2;
    } else o[k++] = s[i];
  }
  o[k] = 0;
}

/* ── messages: stderr for the CLI, a sink for an editor ──────────── */
static void (*report_fn)(const char *msg, void *user);
static void *report_user;
void cwp_set_reporter(void (*fn)(const char *msg, void *user), void *user) {
  report_fn = fn;
  report_user = user;
}
void cwp_report(const char *fmt, ...) {
  char msg[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  if (report_fn) report_fn(msg, report_user);
  else fprintf(stderr, "cwpack: %s\n", msg);
}
