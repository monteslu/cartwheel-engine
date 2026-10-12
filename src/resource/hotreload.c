/* Hot reload for native dev builds: poll the mtimes of the sources cwpack
 * recorded, rerun cwpack (incremental through its import cache) when one
 * changes, remount the pack and reload every loaded resource whose bytes
 * changed. Carts get the same effect from the editor or romdev pushing bytes
 * through cw_res_reload_from_memory. */
#include <stdio.h>
#include <stdlib.h>

#include "cartwheel/platform.h"
#include "resource_internal.h"

#if CW_CART

bool cw_res_hotreload_watch(const char *cwpack, const char *args, const char *out_dir, int interval_frames) {
  cw_warn("hot reload watches files: native builds only");
  return false;
}
int cw_res_hotreload_poll(void) { return 0; }
void cw_res_hotreload_stop(void) {}
void cw__res_hotreload_tick(void) {}

#else

#include <sys/stat.h>

typedef struct { char *path; int64_t mtime_ns, size; } watched;

static struct {
  bool on;
  char *cmd;
  char *sources;
  int interval;
  uint64_t last;
  watched *files;
  int count;
} H;

static void free_files(void) {
  for (int i = 0; i < H.count; i++) cw_free(H.files[i].path);
  cw_free(H.files);
  H.files = NULL;
  H.count = 0;
}

/* modification time (ns) and size; -1 when the file is gone */
static void stat_one(watched *w) {
#if defined(_WIN32)
  struct _stat64 st;
  bool ok = _stat64(w->path, &st) == 0;
  /* whole seconds there: the size catches most same-second edits */
  int64_t mt = ok ? (int64_t)st.st_mtime * 1000000000 : -1;
#else
  struct stat st;
  bool ok = stat(w->path, &st) == 0;
#if defined(__APPLE__)
  int64_t mt = ok ? (int64_t)st.st_mtimespec.tv_sec * 1000000000 + st.st_mtimespec.tv_nsec : -1;
#else
  int64_t mt = ok ? (int64_t)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec : -1;
#endif
#endif
  w->mtime_ns = mt;
  w->size = ok ? (int64_t)st.st_size : -1;
}

/* sources.txt: one source file per line, as cwpack read them */
static void load_sources(void) {
  free_files();
  FILE *f = fopen(H.sources, "rb");
  if (!f) { cw_warn("hot reload: %s missing (run cwpack first)", H.sources); return; }
  char line[4096];
  int cap = 0;
  while (fgets(line, sizeof line, f)) {
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (!n) continue;
    if (H.count == cap) {
      cap = cap ? cap * 2 : 64;
      H.files = cw_realloc(H.files, (size_t)cap * sizeof *H.files, CW_MEM_RESOURCE);
    }
    watched *w = &H.files[H.count++];
    w->path = cw_strdup(line, CW_MEM_RESOURCE);
    stat_one(w);
  }
  fclose(f);
}

bool cw_res_hotreload_watch(const char *cwpack, const char *args, const char *out_dir, int interval_frames) {
  cw_res_hotreload_stop();
  const char *exe = cwpack ? cwpack : getenv("CW_CWPACK") ? getenv("CW_CWPACK") : "cwpack";
  size_t n = strlen(exe) + strlen(args) + 64;
  H.cmd = cw_alloc(n, CW_MEM_RESOURCE);
  snprintf(H.cmd, n, "\"%s\" %s", exe, args);
  n = strlen(out_dir) + 32;
  H.sources = cw_alloc(n, CW_MEM_RESOURCE);
  snprintf(H.sources, n, "%s/sources.txt", out_dir);
  H.interval = interval_frames > 0 ? interval_frames : 30;
  H.on = true;
  H.last = cw__res_frame_index();
  load_sources();
  cw_info("hot reload: watching %d sources (%s)", H.count, H.sources);
  return H.count > 0;
}

void cw_res_hotreload_stop(void) {
  if (!H.on) return;
  free_files();
  cw_free(H.cmd);
  cw_free(H.sources);
  memset(&H, 0, sizeof H);
}

int cw_res_hotreload_poll(void) {
  if (!H.on) return 0;
  bool changed = false;
  for (int i = 0; i < H.count; i++) {
    watched w = H.files[i];
    stat_one(&w);
    if (w.mtime_ns != H.files[i].mtime_ns || w.size != H.files[i].size) {
      cw_info("hot reload: %s changed", w.path);
      changed = true;
    }
  }
  if (!changed) return 0;
  int rc = system(H.cmd);
  if (rc != 0) {
    cw_error("hot reload: cwpack failed (exit %d): %s", rc, H.cmd);
    load_sources(); /* do not retry until something changes again */
    return -1;
  }
  cw_asset_remount();
  int n = cw__res_reload_changed();
  load_sources();
  cw_info("hot reload: %d resource%s reloaded", n, n == 1 ? "" : "s");
  return n;
}

void cw__res_hotreload_tick(void) {
  if (!H.on) return;
  uint64_t f = cw__res_frame_index();
  if (f - H.last < (uint64_t)H.interval) return;
  H.last = f;
  cw_res_hotreload_poll();
}

#endif
