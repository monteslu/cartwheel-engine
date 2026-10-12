/* Editor settings: typed keys with defaults, persisted natively in a file
 * and in a cart in the project layer (.cweditor/settings.cfg). */
#include "internal.h"

typedef struct { cwed_setting_desc d; const char *value; /* interned */ } setting;
static CW_ARRAY(setting) settings;
static bool dirty, loaded;
static double dirty_at, now_s;
static char file_path[1024];
static bool persist;

#define CART_SETTINGS ".cweditor/settings.cfg"

static setting *find(const char *key) {
  for (int i = 0; i < settings.count; i++)
    if (!strcmp(settings.data[i].d.key, key)) return &settings.data[i];
  return NULL;
}

int cwed__settings_count(void) { return settings.count; }
const cwed_setting_desc *cwed__settings_at(int i) { return i >= 0 && i < settings.count ? &settings.data[i].d : NULL; }

void cwed_setting_define(const cwed_setting_desc *d) {
  setting *s = find(d->key);
  if (s) {
    s->d = *d;
    return;
  }
  cw_array_push(&settings, ((setting){ *d, cw_intern(d->def ? d->def : "") }));
}

const char *cwed_setting(const char *key) {
  setting *s = find(key);
  return s ? s->value : NULL;
}
double cwed_setting_float(const char *key) {
  const char *v = cwed_setting(key);
  return v ? strtod(v, NULL) : 0;
}
bool cwed_setting_bool(const char *key) {
  const char *v = cwed_setting(key);
  return v && (!strcmp(v, "true") || !strcmp(v, "1") || !strcmp(v, "on"));
}

static void apply(const char *key);

void cwed_setting_put(const char *key, const char *value) {
  setting *s = find(key);
  if (!s) {
    cwed_setting_define(&(cwed_setting_desc){ .key = cw_intern(key), .type = CWED_ARG_STRING, .def = "", .hidden = true });
    s = find(key);
  }
  const char *v = cw_intern(value ? value : "");
  if (s->value == v) return;
  s->value = v;
  dirty = true;
  dirty_at = now_s;
  /* a cart writes to the in-memory project layer: no reason to wait (a vfs
   * export right after a change must carry it); natively writes are batched */
  if (CW_CART && loaded) cwed__settings_flush(true);
  apply(key);
  cwed_emit(&(cwed_event){ .type = CWED_EV_SETTINGS, .key = s->d.key });
}

/* ── built-ins and their effects ──────────────────────────────────────── */

static void apply(const char *key) {
  if (!strcmp(key, "interface.scale")) {
    double s = cwed_setting_float("interface.scale");
    cw_ui_set_scale((float)(s >= 0.5 && s <= 3 ? s : 1));
  } else if (!strcmp(key, "interface.theme") || !strcmp(key, "interface.font_size") || !strcmp(key, "interface.accent")) {
    cwed__theme_apply();
  }
}

void cwed__settings_apply_all(void) {
  apply("interface.scale");
  apply("interface.theme");
}

/* ── persistence ──────────────────────────────────────────────────────── */

static char *serialize(void) {
  size_t cap = 256;
  for (int i = 0; i < settings.count; i++) cap += strlen(settings.data[i].d.key) + strlen(settings.data[i].value) + 8;
  char *t = cw_alloc(cap, CW_MEM_EDITOR);
  size_t n = (size_t)sprintf(t, "# cartwheel editor settings (key = value; only values that differ from the defaults)\n");
  for (int i = 0; i < settings.count; i++) {
    setting *s = &settings.data[i];
    if (!strcmp(s->value, s->d.def ? s->d.def : "")) continue;
    n += (size_t)sprintf(t + n, "%s = %s\n", s->d.key, s->value);
  }
  return t;
}

static void parse_into(char *text) {
  for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
    if (line[0] == '#') continue;
    char *eq = strstr(line, " = ");
    if (!eq) continue;
    *eq = 0;
    char *v = eq + 3;
    size_t l = strlen(v);
    while (l && (v[l - 1] == '\r' || v[l - 1] == ' ')) v[--l] = 0;
    setting *s = find(line);
    if (s) s->value = cw_intern(v);
    else cwed_setting_define(&(cwed_setting_desc){ .key = cw_intern(line), .type = CWED_ARG_STRING, .def = "", .hidden = true }),
         find(line)->value = cw_intern(v);
  }
}

void cwed__settings_load(void) {
  loaded = true;
  char *text = NULL;
#if CW_CART
  persist = true;
  size_t len;
  text = cw_vfs_read(CART_SETTINGS, &len);
#else
  const char *env = getenv("CW_EDITOR_SETTINGS");
  if (env && *env) snprintf(file_path, sizeof file_path, "%s", env);
  else if (!getenv("CW_FRAMES")) {
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && *xdg) snprintf(file_path, sizeof file_path, "%s/cartwheel/editor.cfg", xdg);
    else if (home && *home) snprintf(file_path, sizeof file_path, "%s/.config/cartwheel/editor.cfg", home);
  }
  persist = file_path[0] != 0;
  if (persist) {
    FILE *f = fopen(file_path, "rb");
    if (f) {
      fseek(f, 0, SEEK_END);
      long n = ftell(f);
      fseek(f, 0, SEEK_SET);
      text = malloc((size_t)n + 1);
      text[fread(text, 1, (size_t)n, f)] = 0;
      fclose(f);
    }
  }
#endif
  if (text) {
    parse_into(text);
    free(text);
  }
}

#if !CW_CART
#include <errno.h>
#include <sys/stat.h>
static void mkdirs_for(const char *path) {
  char tmp[1024];
  snprintf(tmp, sizeof tmp, "%s", path);
  for (char *s = tmp + 1; *s; s++)
    if (*s == '/') {
      *s = 0;
      mkdir(tmp, 0755);
      *s = '/';
    }
}
#endif

static bool write_now(char *err, int cap) {
  dirty = false;
  if (!persist) {
    snprintf(err, (size_t)cap, "settings are not persisted in a test run (set CW_EDITOR_SETTINGS=<file>)");
    return false;
  }
  char *t = serialize();
  bool ok;
#if CW_CART
  ok = cw_vfs_write(CART_SETTINGS, t, strlen(t));
#else
  mkdirs_for(file_path);
  FILE *f = fopen(file_path, "wb");
  ok = f && fwrite(t, 1, strlen(t), f) == strlen(t);
  if (f) fclose(f);
#endif
  if (!ok) snprintf(err, (size_t)cap, "could not write the settings");
  cw_free(t);
  return ok;
}

void cwed__settings_flush(bool force) {
  if (!dirty) return;
  if (!force && now_s - dirty_at < 0.5) return;
  char err[256];
  write_now(err, sizeof err);
}

void cwed__settings_tick(double dt);
void cwed__settings_tick(double dt) {
  now_s += dt;
  cwed__settings_flush(false);
}

/* ── commands ─────────────────────────────────────────────────────────── */

typedef struct { const char *key, *old; } set_state;

static bool valid_value(cwed_call *c, setting *s, const char *v) {
  switch (s->d.type) {
  case CWED_ARG_INT:
  case CWED_ARG_FLOAT: {
    char *end;
    double x = strtod(v, &end);
    if (end == v || *end) return cwed_fail(c, "%s wants a number", s->d.key);
    if (s->d.min < s->d.max && (x < s->d.min || x > s->d.max)) return cwed_fail(c, "%s is between %g and %g", s->d.key, s->d.min, s->d.max);
    return true;
  }
  case CWED_ARG_BOOL:
    if (strcmp(v, "true") && strcmp(v, "false")) return cwed_fail(c, "%s is true or false", s->d.key);
    return true;
  default:
    if (s->d.choices) {
      const char *p = s->d.choices;
      size_t l = strlen(v);
      while (*p) {
        const char *e = strchr(p, '|');
        size_t pl = e ? (size_t)(e - p) : strlen(p);
        if (pl == l && !strncmp(p, v, l)) return true;
        p += pl + (e ? 1 : 0);
      }
      return cwed_fail(c, "%s is one of %s", s->d.key, s->d.choices);
    }
    return true;
  }
}

static bool cmd_set(cwed_call *c) {
  const char *key = cwed_arg(c, "key"), *v = cwed_arg(c, "value");
  setting *s = find(key);
  if (!s) return cwed_fail(c, "no setting '%s' (settings.list shows them)", key);
  if (!valid_value(c, s, v)) return false;
  set_state *st = cwed_call_state(c);
  if (!st) {
    st = cw_calloc(1, sizeof *st, CW_MEM_EDITOR);
    cwed_call_set_state(c, st);
  }
  st->key = s->d.key;
  st->old = s->value;
  cwed_setting_put(key, v);
  cwed_json_key(cwed_reply(c), "value");
  cwed_json_str(cwed_reply(c), cwed_setting(key));
  return true;
}
static void undo_set(cwed_call *c) {
  set_state *st = cwed_call_state(c);
  if (st) cwed_setting_put(st->key, st->old);
}
static bool merge_set(cwed_call *prev, cwed_call *next) {
  set_state *a = cwed_call_state(prev), *b = cwed_call_state(next);
  return a && b && a->key == b->key;
}

static bool cmd_get(cwed_call *c) {
  const char *v = cwed_setting(cwed_arg(c, "key"));
  if (!v) return cwed_fail(c, "no setting '%s' (settings.list shows them)", cwed_arg(c, "key"));
  cwed_json_key(cwed_reply(c), "value");
  cwed_json_str(cwed_reply(c), v);
  return true;
}

static bool cmd_list(cwed_call *c) {
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "settings");
  cwed_json_raw(j, "[");
  for (int i = 0; i < settings.count; i++) {
    setting *s = &settings.data[i];
    cwed_json_raw(j, i ? ",{\"key\":" : "{\"key\":");
    cwed_json_str(j, s->d.key);
    cwed_json_raw(j, ",\"value\":");
    cwed_json_str(j, s->value);
    cwed_json_raw(j, ",\"default\":");
    cwed_json_str(j, s->d.def ? s->d.def : "");
    if (s->d.doc) {
      cwed_json_raw(j, ",\"doc\":");
      cwed_json_str(j, s->d.doc);
    }
    if (s->d.hidden) cwed_json_raw(j, ",\"hidden\":true");
    cwed_json_raw(j, "}");
  }
  cwed_json_raw(j, "]");
  cwed_json_key(j, "file");
  cwed_json_str(j, CW_CART ? CART_SETTINGS " (project layer)" : file_path);
  cwed_json_printf(j, ",\"persisted\":%s", persist ? "true" : "false");
  return true;
}

static bool cmd_save(cwed_call *c) {
  char err[256];
  dirty = true;
  if (!write_now(err, sizeof err)) return cwed_fail(c, "%s", err);
  cwed_json_key(cwed_reply(c), "file");
  cwed_json_str(cwed_reply(c), CW_CART ? CART_SETTINGS : file_path);
  return true;
}

static bool cmd_reload(cwed_call *c) {
  for (int i = 0; i < settings.count; i++) settings.data[i].value = cw_intern(settings.data[i].d.def ? settings.data[i].d.def : "");
  cwed__settings_load();
  cwed__settings_apply_all();
  cwed_emit(&(cwed_event){ .type = CWED_EV_SETTINGS, .key = "" });
  (void)c;
  return true;
}

void cwed__settings_init(void) {
  cwed_setting_define(&(cwed_setting_desc){ .key = "interface.scale", .type = CWED_ARG_FLOAT, .def = "1", .min = 0.5, .max = 3,
    .doc = "Size of the whole editor UI (1 = 100%)." });
  cwed_setting_define(&(cwed_setting_desc){ .key = "interface.theme", .type = CWED_ARG_STRING, .def = "dark", .choices = "dark|light",
    .doc = "Editor color theme." });
  cwed_setting_define(&(cwed_setting_desc){ .key = "interface.font_size", .type = CWED_ARG_INT, .def = "14", .min = 10, .max = 24,
    .doc = "Text size in the editor." });
  cwed_setting_define(&(cwed_setting_desc){ .key = "interface.accent", .type = CWED_ARG_STRING, .def = "teal",
    .choices = "teal|blue|orange|purple", .doc = "Accent color for focus, selection and active tabs." });
  cwed_setting_define(&(cwed_setting_desc){ .key = "editor.open_main_scene", .type = CWED_ARG_BOOL, .def = "true",
    .doc = "Open the project's main scene at startup." });
  static const cwed_param set_p[] = {
    { "key", CWED_ARG_STRING, NULL, "setting key (settings.list)" }, { "value", CWED_ARG_STRING, NULL, "new value" }, { 0 } };
  static const cwed_param key_p[] = { { "key", CWED_ARG_STRING, NULL, "setting key" }, { 0 } };
  cwed_command_register(&(cwed_command_desc){ .name = "settings.set", .title = "Change Setting", .params = set_p,
    .flags = CWED_CMD_UNDO | CWED_CMD_HIDDEN, .doc = "Change an editor setting (undoable in the editor history; persisted).",
    .example = "settings.set key=interface.theme value=light", .exec = cmd_set, .undo = undo_set, .merge = merge_set });
  cwed_command_register(&(cwed_command_desc){ .name = "settings.get", .title = "Get Setting", .params = key_p,
    .flags = CWED_CMD_QUERY | CWED_CMD_HIDDEN, .doc = "One setting's value.", .exec = cmd_get });
  cwed_command_register(&(cwed_command_desc){ .name = "settings.list", .title = "List Settings", .flags = CWED_CMD_QUERY | CWED_CMD_HIDDEN,
    .doc = "Every setting with value, default and doc, and where they persist.", .exec = cmd_list });
  cwed_command_register(&(cwed_command_desc){ .name = "settings.save", .title = "Save Settings Now",
    .doc = "Write the settings now (they also save on their own half a second after a change).", .exec = cmd_save });
  cwed_command_register(&(cwed_command_desc){ .name = "settings.reload", .title = "Reload Settings",
    .doc = "Read the settings file again (in a cart: after `vfs import` brought one in).", .exec = cmd_reload });
}
