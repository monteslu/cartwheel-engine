/* project.cwproj for games (cartwheel/project.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/input.h"
#include "cartwheel/project.h"
#include "cartwheel/vfs.h"

#define MAX_KEYS 512

static struct {
  bool loaded;
  char *text; /* the file, cut into NUL-terminated keys and values */
  int count;
  const char *key[MAX_KEYS], *val[MAX_KEYS];
} P;

static char *trim(char *s) {
  while (*s == ' ' || *s == '\t') s++;
  char *e = s + strlen(s);
  while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
  return s;
}

static void load(void) {
  if (P.loaded) return;
  P.loaded = true;
  size_t len;
  P.text = cw_vfs_read("project.cwproj", &len);
  if (!P.text) return;
  for (char *line = P.text; line && *line;) {
    char *nl = strchr(line, '\n');
    if (nl) *nl = 0;
    char *l = trim(line), *eq = strchr(l, '=');
    if (*l && *l != '#' && *l != ';' && *l != '[' && eq && P.count < MAX_KEYS) {
      *eq = 0;
      char *k = trim(l), *v = trim(eq + 1);
      size_t vl = strlen(v);
      if (vl >= 2 && v[0] == '"' && v[vl - 1] == '"') v[vl - 1] = 0, v++;
      P.key[P.count] = k;
      P.val[P.count] = v;
      P.count++;
    }
    line = nl ? nl + 1 : NULL;
  }
}

void cw_project_reload(void) {
  free(P.text);
  memset(&P, 0, sizeof P);
}

const char *cw_project_get(const char *key) {
  load();
  for (int i = P.count - 1; i >= 0; i--) /* the last line for a key wins */
    if (!strcmp(P.key[i], key)) return P.val[i];
  return NULL;
}

int cw_project_get_int(const char *key, int def) {
  const char *v = cw_project_get(key);
  if (!v || !*v) return def;
  char *end;
  long n = strtol(v, &end, 0);
  return *end ? def : (int)n;
}

bool cw_project_get_bool(const char *key, bool def) {
  const char *v = cw_project_get(key);
  if (!v) return def;
  if (!strcmp(v, "true") || !strcmp(v, "1") || !strcmp(v, "on")) return true;
  if (!strcmp(v, "false") || !strcmp(v, "0") || !strcmp(v, "off")) return false;
  return def;
}

int cw_project_count(void) {
  load();
  return P.count;
}

const char *cw_project_key(int i) {
  load();
  return i >= 0 && i < P.count ? P.key[i] : NULL;
}

int cw_project_load_input(void) {
  load();
  int n = 0;
  for (int i = 0; i < P.count; i++) {
    if (strncmp(P.key[i], "input.", 6) || !P.key[i][6]) continue;
    char line[1024];
    int len = snprintf(line, sizeof line, "%s = %s\n", P.key[i] + 6, P.val[i]);
    if (len <= 0 || len >= (int)sizeof line) {
      cw_warn("project.cwproj: %s: the binding list is too long", P.key[i]);
      continue;
    }
    if (cw_input_load_bindings(line, (size_t)len)) n++;
    else cw_warn("project.cwproj: %s = %s: not a valid binding list (cartwheel/input.h)", P.key[i], P.val[i]);
  }
  cw_input_commit_defaults();
  return n;
}

void cw_project_apply_app(cw_app_desc *d) {
  d->width = cw_project_get_int("render.width", d->width);
  d->height = cw_project_get_int("render.height", d->height);
  d->msaa = cw_project_get_int("render.msaa", d->msaa);
  d->fixed_hz = cw_project_get_int("render.fixed_hz", d->fixed_hz);
  d->vsync_off = !cw_project_get_bool("render.vsync", !d->vsync_off);
  const char *name = cw_project_get("name");
  if (name && *name && (!d->title || !*d->title)) {
    static char title[128];
    snprintf(title, sizeof title, "%s", name);
    d->title = title;
  }
}

static bool key_char(char c) { return c && c != '=' && c != '\n' && c != '\r'; }

char *cw_project_text_set(const char *old, const char *key, const char *value) {
  if (!key || !*key || strchr(key, '=') || strchr(key, '\n') || (value && strchr(value, '\n'))) return NULL;
  if (!old) old = "";
  size_t cap = strlen(old) + strlen(key) + (value ? strlen(value) : 0) + 16;
  char *out = malloc(cap), *w = out;
  bool done = false;
  for (const char *line = old; *line;) {
    const char *nl = strchr(line, '\n');
    size_t ll = nl ? (size_t)(nl - line) : strlen(line);
    /* this line's key, trimmed */
    const char *k = line, *ke;
    while (k < line + ll && (*k == ' ' || *k == '\t')) k++;
    ke = k;
    while (ke < line + ll && key_char(*ke)) ke++;
    bool is_kv = ke < line + ll && *ke == '=' && *k != '#' && *k != ';';
    const char *kt = ke;
    while (kt > k && (kt[-1] == ' ' || kt[-1] == '\t')) kt--;
    bool match = is_kv && (size_t)(kt - k) == strlen(key) && !strncmp(k, key, (size_t)(kt - k));
    if (match) {
      if (value && !done) w += sprintf(w, "%s = %s\n", key, value);
      done = true; /* later duplicates of the key go away */
    } else {
      memcpy(w, line, ll);
      w += ll;
      *w++ = '\n';
    }
    line += ll + (nl ? 1 : 0);
  }
  if (!done && value) w += sprintf(w, "%s = %s\n", key, value);
  *w = 0;
  return out;
}

bool cw_project_set(const char *key, const char *value) {
  size_t len = 0;
  char *old = cw_vfs_read("project.cwproj", &len);
  char *text = cw_project_text_set(old, key, value);
  free(old);
  if (!text) return false;
  bool ok = cw_vfs_write("project.cwproj", text, strlen(text));
  free(text);
  cw_project_reload();
  return ok;
}
