#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !CW_CART
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "cartwheel/core.h"
#include "cartwheel/debug.h"
#include "cartwheel/platform.h"
#include "cartwheel/vfs.h"

/* path interned; removed = a removal mark (no data): the file is hidden in
 * the layers below */
typedef struct { const char *path; uint8_t *data; size_t len; bool removed; } vfile;

static CW_ARRAY(vfile) files;      /* sorted by path */
static CW_ARRAY(const char *) names;
static char root[1024];
static bool root_set, cmds_registered;
static uint32_t generation = 1;

/* the asset layer's file list (sorted, built on first use) */
typedef struct { const char *path; int64_t size; } afile;
static CW_ARRAY(afile) assets;
static bool assets_built;

/* The platform lists the asset layer natively (pack index + loose asset
 * root); in a cart it returns -1 and the bundle manifest is read instead. */
int cw__asset_list_all(void (*cb)(const char *path, int64_t size, void *user), void *user);

#define BUNDLE_MANIFEST "cwfiles.txt"

static void register_cmds(void);
void cw__vfs_register_commands(void);

static bool valid(const char *p) {
  return p && *p && p[0] != '/' && !strstr(p, "..") && !strchr(p, '\\');
}

void cw_vfs_set_root(const char *dir) {
  register_cmds();
  root_set = dir && *dir;
  snprintf(root, sizeof root, "%s", dir ? dir : "");
  generation++;
  assets_built = false;
}

const char *cw_vfs_root(void) {
  if (!root_set && getenv("CW_PROJECT") && !CW_CART) cw_vfs_set_root(getenv("CW_PROJECT"));
  return root_set ? root : NULL;
}

uint32_t cw_vfs_generation(void) { return generation; }

static int find(const char *path, bool *found) {
  int lo = 0, hi = files.count;
  while (lo < hi) {
    int mid = (lo + hi) / 2;
    int c = strcmp(files.data[mid].path, path);
    if (c == 0) { *found = true; return mid; }
    if (c < 0) lo = mid + 1;
    else hi = mid;
  }
  *found = false;
  return lo;
}

#if !CW_CART
static bool mkdirs(const char *full) {
  char tmp[2048];
  snprintf(tmp, sizeof tmp, "%s", full);
  for (char *s = tmp + 1; *s; s++)
    if (*s == '/') {
      *s = 0;
#if defined(_WIN32)
      if (mkdir(tmp) != 0 && errno != EEXIST) return false;
#else
      if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return false;
#endif
      *s = '/';
    }
  return true;
}
/* 1 regular file, 2 directory, 0 neither (or no project directory) */
static int disk_kind(const char *path, int64_t *size) {
  const char *r = cw_vfs_root();
  if (!r) return 0;
  char full[2048];
  snprintf(full, sizeof full, "%s%s%s", r, path && *path ? "/" : "", path ? path : "");
  struct stat st;
  if (stat(full, &st)) return 0;
  if (size) *size = (int64_t)st.st_size;
  return S_ISDIR(st.st_mode) ? 2 : S_ISREG(st.st_mode) ? 1 : 0;
}
#else
static int disk_kind(const char *path, int64_t *size) { (void)path, (void)size; return 0; }
#endif

/* ── the asset layer's listing ──────────────────────────────────────── */
static int cmp_afile(const void *a, const void *b) { return strcmp(((const afile *)a)->path, ((const afile *)b)->path); }
static void asset_add(const char *path, int64_t size, void *user) {
  (void)user;
  if (!valid(path) || !strcmp(path, BUNDLE_MANIFEST)) return;
  cw_array_push(&assets, ((afile){ cw_intern(path), size }));
}
static void assets_from_text(char *t, bool res_index) {
  for (char *line = t; line && *line;) {
    char *nl = strchr(line, '\n');
    if (nl) *nl = 0;
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
    if (line[0] && line[0] != '#') {
      if (!res_index) asset_add(line, -1, NULL);
      else { /* res.index: "<uid> <type> <path>"; sub-resources ("#") are not files */
        char *sp = strchr(line, ' ');
        char *sp2 = sp ? strchr(sp + 1, ' ') : NULL;
        if (sp2 && !strchr(sp2 + 1, '#')) asset_add(sp2 + 1, -1, NULL);
      }
    }
    line = nl ? nl + 1 : NULL;
  }
}
static void assets_build(void) {
  if (assets_built) return;
  assets_built = true;
  cw_array_clear(&assets);
  if (cw__asset_list_all(asset_add, NULL) < 0) {
    size_t len;
    char *t = cw_asset_load(BUNDLE_MANIFEST, &len);
    if (t) assets_from_text(t, false);
    else if ((t = cw_asset_load("res.index", &len))) assets_from_text(t, true);
    free(t);
  }
  if (assets.count > 1) qsort(assets.data, (size_t)assets.count, sizeof *assets.data, cmp_afile);
  int k = 0; /* a pack and the loose root may both hold a path */
  for (int i = 0; i < assets.count; i++)
    if (!k || assets.data[k - 1].path != assets.data[i].path) assets.data[k++] = assets.data[i];
  assets.count = k;
}
void cw_vfs_rescan(void) {
  assets_built = false;
  generation++;
}

/* ── files ──────────────────────────────────────────────────────────── */
cw_vfs_layer cw_vfs_where(const char *path) {
  if (!valid(path)) return CW_VFS_NONE;
  bool f;
  int i = find(path, &f);
  if (f && files.data[i].removed) return CW_VFS_NONE;
  if (disk_kind(path, NULL) == 1) return CW_VFS_DISK;
  if (f) return CW_VFS_MEMORY;
  return cw_asset_size(path) >= 0 ? CW_VFS_ASSET : CW_VFS_NONE;
}

bool cw_vfs_exists(const char *path) { return cw_vfs_where(path) != CW_VFS_NONE; }

void *cw_vfs_read(const char *path, size_t *len) {
  register_cmds();
  if (!valid(path)) return NULL;
  bool f;
  int i = find(path, &f);
  if (f) {
    if (files.data[i].removed) return NULL;
    char *out = malloc(files.data[i].len + 1);
    memcpy(out, files.data[i].data, files.data[i].len);
    out[files.data[i].len] = 0;
    if (len) *len = files.data[i].len;
    return out;
  }
#if !CW_CART
  const char *r = cw_vfs_root();
  if (r) {
    char full[2048];
    snprintf(full, sizeof full, "%s/%s", r, path);
    FILE *fp = fopen(full, "rb");
    if (fp) {
      fseek(fp, 0, SEEK_END);
      long n = ftell(fp);
      fseek(fp, 0, SEEK_SET);
      char *out = malloc((size_t)n + 1);
      size_t got = fread(out, 1, (size_t)n, fp);
      fclose(fp);
      out[got] = 0;
      if (len) *len = got;
      return out;
    }
  }
#endif
  return cw_asset_load(path, len);
}

static vfile *mem_slot(const char *path) {
  bool f;
  int i = find(path, &f);
  if (!f) {
    vfile v = { cw_intern(path), NULL, 0, false };
    cw_array_push(&files, v);
    memmove(files.data + i + 1, files.data + i, sizeof(vfile) * (size_t)(files.count - 1 - i));
    files.data[i] = v;
  }
  return &files.data[i];
}

bool cw_vfs_write(const char *path, const void *data, size_t len) {
  register_cmds();
  if (!valid(path)) {
    cw_error("vfs: bad path '%s' (relative, '/'-separated, no '..')", path ? path : "(null)");
    return false;
  }
#if !CW_CART
  const char *r = cw_vfs_root();
  if (r) {
    char full[2048];
    snprintf(full, sizeof full, "%s/%s", r, path);
    if (!mkdirs(full)) return false;
    FILE *fp = fopen(full, "wb");
    if (!fp) { cw_error("vfs: cannot write %s", full); return false; }
    bool ok = fwrite(data, 1, len, fp) == len;
    fclose(fp);
    if (!ok) return false;
  }
#endif
  vfile *v = mem_slot(path);
  cw_free(v->data);
  v->data = cw_alloc(len ? len : 1, CW_MEM_EDITOR);
  memcpy(v->data, data, len);
  v->len = len;
  v->removed = false;
  generation++;
  return true;
}

bool cw_vfs_remove(const char *path) {
  if (!valid(path)) return false;
  bool f, had = false;
  int i = find(path, &f);
  if (f && files.data[i].removed) return false; /* already gone */
  if (f) {
    cw_free(files.data[i].data);
    memmove(files.data + i, files.data + i + 1, sizeof(vfile) * (size_t)(files.count - i - 1));
    files.count--;
    had = true;
  }
#if !CW_CART
  const char *r = cw_vfs_root();
  if (r && disk_kind(path, NULL) == 1) {
    char full[2048];
    snprintf(full, sizeof full, "%s/%s", r, path);
    if (remove(full) == 0) had = true;
  }
#endif
  if (cw_asset_size(path) >= 0) { /* a read-only layer still serves it: hide it */
    vfile *v = mem_slot(path);
    v->removed = true;
    had = true;
  }
  if (had) generation++;
  return had;
}

int cw_vfs_list(const char ***out) {
  cw_array_clear(&names);
  for (int i = 0; i < files.count; i++)
    if (!files.data[i].removed) cw_array_push(&names, files.data[i].path);
  *out = names.data;
  return names.count;
}

int cw_vfs_changed_count(void) { return files.count; }

void cw_vfs_clear(void) {
  for (int i = 0; i < files.count; i++) cw_free(files.data[i].data);
  cw_array_clear(&files);
  generation++;
}

/* ── directories ────────────────────────────────────────────────────── */
static CW_ARRAY(cw_vfs_entry) listing;

static bool is_removed(const char *path) {
  bool f;
  int i = find(path, &f);
  return f && files.data[i].removed;
}

/* the child of dir that `path` (under dir) falls in: its name and whether it is a directory */
static bool child_of(const char *dir, size_t dl, const char *path, char *name, size_t cap, bool *is_dir) {
  if (dl && (strncmp(path, dir, dl) || path[dl] != '/')) return false;
  const char *rest = path + (dl ? dl + 1 : 0);
  const char *slash = strchr(rest, '/');
  size_t n = slash ? (size_t)(slash - rest) : strlen(rest);
  if (!n || n >= cap) return false;
  memcpy(name, rest, n);
  name[n] = 0;
  *is_dir = slash != NULL;
  return true;
}

static void list_add(const char *name, bool is_dir, int64_t size, cw_vfs_layer layer, int flags) {
  if (name[0] == '.' && !(flags & CW_VFS_LIST_HIDDEN)) return;
  const char *in = cw_intern(name);
  for (int i = 0; i < listing.count; i++)
    if (listing.data[i].name == in && listing.data[i].is_dir == is_dir) {
      if (layer > listing.data[i].layer) listing.data[i].layer = layer;
      return;
    }
  cw_array_push(&listing, ((cw_vfs_entry){ in, is_dir, is_dir ? -1 : size, layer }));
}

static int cmp_entry(const void *a, const void *b) {
  const cw_vfs_entry *x = a, *y = b;
  if (x->is_dir != y->is_dir) return x->is_dir ? -1 : 1;
  return strcmp(x->name, y->name);
}

int cw_vfs_list_dir(const char *dir, int flags, const cw_vfs_entry **out) {
  register_cmds();
  if (!dir) dir = "";
  char d[1024];
  snprintf(d, sizeof d, "%s", dir);
  size_t dl = strlen(d);
  while (dl && d[dl - 1] == '/') d[--dl] = 0;
  if (dl && !valid(d)) return -1;
  cw_array_clear(&listing);
  bool found = dl == 0;
  char name[512], full[1024];
  bool sub;
  /* memory layer */
  for (int i = 0; i < files.count; i++) {
    if (files.data[i].removed || !child_of(d, dl, files.data[i].path, name, sizeof name, &sub)) continue;
    found = true;
    list_add(name, sub, (int64_t)files.data[i].len, CW_VFS_MEMORY, flags);
  }
#if !CW_CART
  /* project directory */
  if (disk_kind(d, NULL) == 2) {
    found = true;
    char p[2048];
    snprintf(p, sizeof p, "%s%s%s", cw_vfs_root(), dl ? "/" : "", d);
    DIR *dh = opendir(p);
    struct dirent *e;
    while (dh && (e = readdir(dh))) {
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
      snprintf(full, sizeof full, "%s%s%s", d, dl ? "/" : "", e->d_name);
      int64_t size = 0;
      int k = disk_kind(full, &size);
      if (!k || (k == 1 && is_removed(full))) continue;
      list_add(e->d_name, k == 2, size, CW_VFS_DISK, flags);
    }
    if (dh) closedir(dh);
  }
#endif
  /* assets */
  assets_build();
  for (int i = 0; i < assets.count; i++) {
    const afile *a = &assets.data[i];
    if (!child_of(d, dl, a->path, name, sizeof name, &sub) || is_removed(a->path)) continue;
    found = true;
    int64_t size = a->size;
    if (!sub && size < 0) size = cw_asset_size(a->path);
    list_add(name, sub, size, CW_VFS_ASSET, flags);
  }
  if (!found) return -1;
  /* a file's layer is where a read is served from */
  for (int i = 0; i < listing.count; i++) {
    cw_vfs_entry *e = &listing.data[i];
    if (e->is_dir) continue;
    snprintf(full, sizeof full, "%s%s%s", d, dl ? "/" : "", e->name);
    e->layer = cw_vfs_where(full);
  }
  if (!dl) /* the bundle manifest describes the bundle; it is not a project file */
    for (int i = 0; i < listing.count; i++)
      if (!listing.data[i].is_dir && !strcmp(listing.data[i].name, BUNDLE_MANIFEST)) cw_array_remove_swap(&listing, i--);
  if (listing.count > 1) qsort(listing.data, (size_t)listing.count, sizeof *listing.data, cmp_entry);
  *out = listing.data;
  return listing.count;
}

bool cw_vfs_is_dir(const char *path) {
  const cw_vfs_entry *e;
  return path && *path && disk_kind(path, NULL) != 1 && cw_vfs_list_dir(path, CW_VFS_LIST_HIDDEN, &e) >= 0;
}

typedef CW_ARRAY(const char *) path_list;
static void collect_files(const char *dir, path_list *out, int depth) {
  const cw_vfs_entry *e;
  int n = cw_vfs_list_dir(dir, CW_VFS_LIST_HIDDEN, &e);
  if (n <= 0 || depth > 32) return;
  /* the listing buffer is reused by the recursion: copy the names first */
  const char **sub = cw_alloc(sizeof *sub * (size_t)n, CW_MEM_EDITOR);
  bool *isd = cw_alloc((size_t)n, CW_MEM_EDITOR);
  for (int i = 0; i < n; i++) sub[i] = e[i].name, isd[i] = e[i].is_dir;
  char p[1024];
  for (int i = 0; i < n; i++) {
    if (snprintf(p, sizeof p, "%s/%s", dir, sub[i]) >= (int)sizeof p) continue;
    if (isd[i]) collect_files(p, out, depth + 1);
    else cw_array_push(out, cw_intern(p));
  }
  cw_free(sub);
  cw_free(isd);
}

typedef struct { const char *path; cw_vfs_entry e; } walk_item;
static int walk_rec(const char *dir, int flags, void (*cb)(const char *, const cw_vfs_entry *, void *), void *user, int depth) {
  const cw_vfs_entry *e;
  int n = cw_vfs_list_dir(dir, flags, &e);
  if (n <= 0 || depth > 32) return 0;
  walk_item *items = cw_alloc(sizeof *items * (size_t)n, CW_MEM_EDITOR); /* the listing buffer is reused below */
  char p[1024];
  int k = 0, files_n = 0;
  for (int i = 0; i < n; i++) {
    if (snprintf(p, sizeof p, "%s%s%s", dir, *dir ? "/" : "", e[i].name) >= (int)sizeof p) continue;
    items[k++] = (walk_item){ cw_intern(p), e[i] };
  }
  for (int i = 0; i < k; i++) {
    if (items[i].e.is_dir) files_n += walk_rec(items[i].path, flags, cb, user, depth + 1);
    else {
      cb(items[i].path, &items[i].e, user);
      files_n++;
    }
  }
  cw_free(items);
  return files_n;
}
int cw_vfs_walk(const char *dir, int flags, void (*cb)(const char *path, const cw_vfs_entry *e, void *user), void *user) {
  return walk_rec(dir ? dir : "", flags, cb, user, 0);
}

bool cw_vfs_revert(const char *path) {
  bool f;
  int i = find(path ? path : "", &f);
  if (!f) return false;
  cw_free(files.data[i].data);
  memmove(files.data + i, files.data + i + 1, sizeof(vfile) * (size_t)(files.count - i - 1));
  files.count--;
  generation++;
  return true;
}

static bool move_file(const char *from, const char *to) {
  size_t len;
  void *b = cw_vfs_read(from, &len);
  if (!b) return false;
  bool ok = cw_vfs_write(to, b, len);
  free(b);
  return ok && cw_vfs_remove(from);
}

bool cw_vfs_rename(const char *from, const char *to) {
  if (!valid(from) || !valid(to)) { cw_error("vfs: bad path in rename '%s' -> '%s'", from ? from : "", to ? to : ""); return false; }
  size_t fl = strlen(from);
  if (!strncmp(to, from, fl) && to[fl] == '/') { cw_error("vfs: cannot move '%s' into itself", from); return false; }
  if (cw_vfs_exists(to) || cw_vfs_is_dir(to)) { cw_error("vfs: '%s' already exists", to); return false; }
  if (cw_vfs_exists(from)) return move_file(from, to);
  if (!cw_vfs_is_dir(from)) { cw_error("vfs: no file or directory '%s'", from); return false; }
  path_list all = { 0 };
  collect_files(from, &all, 0);
  bool ok = true;
  char dst[1024];
  for (int i = 0; ok && i < all.count; i++) {
    snprintf(dst, sizeof dst, "%s%s", to, all.data[i] + fl);
    ok = move_file(all.data[i], dst);
  }
  cw_array_free(&all);
#if !CW_CART
  if (ok && disk_kind(from, NULL) == 2) { /* empty directories left on disk */
    char full[2048];
    snprintf(full, sizeof full, "%s/%s", cw_vfs_root(), from);
    path_list dirs = { 0 };
    cw_array_push(&dirs, cw_intern(full));
    for (int i = 0; i < dirs.count; i++) {
      DIR *dh = opendir(dirs.data[i]);
      struct dirent *e;
      while (dh && (e = readdir(dh)))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
          char sp[2048];
          snprintf(sp, sizeof sp, "%s/%s", dirs.data[i], e->d_name);
          cw_array_push(&dirs, cw_intern(sp));
        }
      if (dh) closedir(dh);
    }
    for (int i = dirs.count - 1; i >= 0; i--) rmdir(dirs.data[i]);
    cw_array_free(&dirs);
  }
#endif
  generation++;
  return ok;
}

/* ── archive ────────────────────────────────────────────────────────── */
void *cw_vfs_export(size_t *len) {
  size_t total = 7;
  for (int i = 0; i < files.count; i++) total += strlen(files.data[i].path) + 1 + 21 + files.data[i].len;
  char *out = cw_alloc(total + 1, CW_MEM_EDITOR);
  size_t n = 0;
  memcpy(out, "CWVFS1\n", 7);
  n = 7;
  for (int i = 0; i < files.count; i++) {
    if (files.data[i].removed) {
      n += (size_t)sprintf(out + n, "%s\nremoved\n", files.data[i].path);
      continue;
    }
    n += (size_t)sprintf(out + n, "%s\n%zu\n", files.data[i].path, files.data[i].len);
    memcpy(out + n, files.data[i].data, files.data[i].len);
    n += files.data[i].len;
  }
  *len = n;
  return out;
}

bool cw_vfs_import(const void *data, size_t len) {
  const char *p = data, *end = p + len;
  if (len < 7 || memcmp(p, "CWVFS1\n", 7)) return false;
  p += 7;
  while (p < end) {
    const char *nl = memchr(p, '\n', (size_t)(end - p));
    if (!nl) return false;
    char path[1024];
    size_t pl = (size_t)(nl - p);
    if (pl == 0 || pl >= sizeof path) return false;
    memcpy(path, p, pl);
    path[pl] = 0;
    p = nl + 1;
    if ((size_t)(end - p) >= 8 && !memcmp(p, "removed\n", 8)) {
      if (!valid(path)) return false;
      cw_vfs_remove(path);
      p += 8;
      continue;
    }
    char *lenend;
    unsigned long long flen = strtoull(p, &lenend, 10);
    if (lenend == p || *lenend != '\n') return false;
    p = lenend + 1;
    if ((size_t)(end - p) < flen) return false;
    if (!cw_vfs_write(path, p, (size_t)flen)) return false;
    p += flen;
  }
  generation++;
  return true;
}

/* ── base64 ───────────────────────────────────────────────────────────── */

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *cw_base64_encode(const void *data, size_t len) {
  const uint8_t *s = data;
  char *out = cw_alloc((len + 2) / 3 * 4 + 1, CW_MEM_CORE);
  size_t o = 0;
  for (size_t i = 0; i < len; i += 3) {
    uint32_t v = (uint32_t)s[i] << 16 | (i + 1 < len ? (uint32_t)s[i + 1] << 8 : 0) | (i + 2 < len ? s[i + 2] : 0);
    out[o++] = b64[v >> 18 & 63];
    out[o++] = b64[v >> 12 & 63];
    out[o++] = i + 1 < len ? b64[v >> 6 & 63] : '=';
    out[o++] = i + 2 < len ? b64[v & 63] : '=';
  }
  out[o] = 0;
  return out;
}

void *cw_base64_decode(const char *t, size_t *out_len) {
  size_t n = strlen(t);
  uint8_t *out = cw_alloc(n / 4 * 3 + 3, CW_MEM_CORE);
  size_t o = 0;
  uint32_t acc = 0;
  int bits = 0;
  for (size_t i = 0; i < n; i++) {
    char c = t[i];
    if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
    const char *k = strchr(b64, c);
    if (!k || !c) { cw_free(out); return NULL; }
    acc = acc << 6 | (uint32_t)(k - b64);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out[o++] = (uint8_t)(acc >> bits);
    }
  }
  *out_len = o;
  return out;
}

/* ── debug commands ──────────────────────────────────────────────────── */

static void cmd_vfs(int argc, const char **argv, cw_reply *r) {
  if (argc < 2) { cw_reply_error(r, "usage: vfs ls | dir [path] | cat <path> | write <path> <base64> | rm <path> | mv <from> <to> | export | import <base64> | clear"); return; }
  const char *op = argv[1];
  if (!strcmp(op, "ls")) {
    const char **list;
    int n = cw_vfs_list(&list);
    cw_reply_raw(r, "{\"files\":[");
    for (int i = 0; i < n; i++) {
      if (i) cw_reply_raw(r, ",");
      cw_reply_str(r, list[i]);
    }
    cw_reply_printf(r, "],\"root\":");
    if (cw_vfs_root()) cw_reply_str(r, cw_vfs_root());
    else cw_reply_raw(r, "null");
    cw_reply_raw(r, "}");
  } else if (!strcmp(op, "cat") && argc > 2) {
    size_t len;
    char *d = cw_vfs_read(argv[2], &len);
    if (!d) { cw_reply_error(r, "no file '%s'", argv[2]); return; }
    char *e = cw_base64_encode(d, len);
    cw_reply_printf(r, "{\"length\":%zu,\"base64\":\"%s\"}", len, e);
    cw_free(e);
    free(d);
  } else if (!strcmp(op, "write") && argc > 3) {
    size_t len;
    void *d = cw_base64_decode(argv[3], &len);
    if (!d || !cw_vfs_write(argv[2], d, len)) { cw_free(d); cw_reply_error(r, "write failed (path or base64)"); return; }
    cw_free(d);
    cw_reply_printf(r, "{\"written\":%zu}", len);
  } else if (!strcmp(op, "append") && argc > 3) { /* requests are capped at 4 KB: send big files in pieces */
    size_t len, old = 0;
    void *d = cw_base64_decode(argv[3], &len);
    if (!d) { cw_reply_error(r, "bad base64"); return; }
    char *prev = cw_vfs_exists(argv[2]) ? cw_vfs_read(argv[2], &old) : NULL;
    char *joined = cw_alloc(old + len + 1, CW_MEM_EDITOR);
    if (prev) memcpy(joined, prev, old);
    memcpy(joined + old, d, len);
    bool ok = cw_vfs_write(argv[2], joined, old + len);
    free(prev);
    cw_free(d);
    cw_free(joined);
    if (!ok) { cw_reply_error(r, "append failed"); return; }
    cw_reply_printf(r, "{\"length\":%zu}", old + len);
  } else if (!strcmp(op, "dir")) {
    const cw_vfs_entry *e;
    static const char *lname[] = { "none", "asset", "disk", "memory" };
    int n = cw_vfs_list_dir(argc > 2 ? argv[2] : "", 0, &e);
    if (n < 0) { cw_reply_error(r, "no directory '%s' in any layer (vfs dir lists the root)", argc > 2 ? argv[2] : ""); return; }
    cw_reply_raw(r, "{\"entries\":[");
    for (int i = 0; i < n; i++) {
      cw_reply_raw(r, i ? ",{\"name\":" : "{\"name\":");
      cw_reply_str(r, e[i].name);
      cw_reply_printf(r, ",\"dir\":%s,\"size\":%lld,\"layer\":\"%s\"}", e[i].is_dir ? "true" : "false", (long long)e[i].size,
                      lname[e[i].layer]);
    }
    cw_reply_raw(r, "]}");
  } else if (!strcmp(op, "mv") && argc > 3) {
    if (!cw_vfs_rename(argv[2], argv[3])) { cw_reply_error(r, "cannot move '%s' to '%s' (the log says why)", argv[2], argv[3]); return; }
    cw_reply_raw(r, "{\"moved\":true}");
  } else if (!strcmp(op, "rm") && argc > 2) {
    cw_reply_printf(r, "{\"removed\":%s}", cw_vfs_remove(argv[2]) ? "true" : "false");
  } else if (!strcmp(op, "export")) {
    size_t len;
    void *a = cw_vfs_export(&len);
    char *e = cw_base64_encode(a, len);
    cw_reply_printf(r, "{\"files\":%d,\"length\":%zu,\"base64\":\"%s\"}", cw_vfs_changed_count(), len, e);
    cw_free(e);
    cw_free(a);
  } else if (!strcmp(op, "import") && argc > 2) {
    size_t len;
    void *d = cw_base64_decode(argv[2], &len);
    bool ok = d && cw_vfs_import(d, len);
    cw_free(d);
    if (!ok) { cw_reply_error(r, "not a CWVFS1 archive"); return; }
    cw_reply_printf(r, "{\"files\":%d}", cw_vfs_changed_count());
  } else if (!strcmp(op, "clear")) {
    cw_vfs_clear();
    cw_reply_raw(r, "{\"cleared\":true}");
  } else {
    cw_reply_error(r, "unknown vfs op '%s'", op);
  }
}

void cw__vfs_register_commands(void) { register_cmds(); }

static void register_cmds(void) {
  if (cmds_registered) return;
  cmds_registered = true;
  cw_debug_command("vfs", "vfs ls | dir [path] | cat <path> | write|append <path> <base64> | rm <path> | mv <from> <to> | export | import <base64> | clear: "
                          "the writable project layer (a cart keeps changes in memory; export carries them out)", cmd_vfs);
}
