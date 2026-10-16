/* Native loop owner: main(), the frame loop, SDL audio, asset files.
 *
 * Test hooks (environment):
 *   CW_FRAMES=N        run exactly N frames on a fixed 1/60 s clock (CW_FRAME_HZ), then exit
 *                      (the same virtual clock a deterministic cart run uses)
 *   CW_PLAYER_TIMING=1 with CW_FRAMES: not a deterministic run (cw_is_deterministic_run false):
 *                      loads finish when their decode is done, under the millisecond budget, as
 *                      in a player's game, so frame times measure what a player gets
 *   CW_SCREENSHOT=f    with CW_FRAMES: write the last frame to f (PNG)
 *   CW_DEBUG_DUMP=f    write the debug fields as JSON at the end of a CW_FRAMES run,
 *                      or when the game quits (cw_quit)
 *   CW_DEBUG_CMDS="a;b" with CW_FRAMES only: after the last frame, run each debug
 *                      command and print one JSON reply per line (before CW_DEBUG_DUMP)
 *   CW_ASSETS=dir      asset root for loose files (default ./assets)
 *   CW_PAK=file        asset pack (default <executable>.cwpak when present)
 *   CW_SEED=n          the seed cw_host_seed() reports (default 0)
 *   CW_REMOTE=fd:N | tcp:HOST:PORT   serve the debug channel on a socket (what
 *                      cartwheel/remote.h launches games with); turns the
 *                      channel on even without .debug; EOF on it quits
 *   CW_START_PAUSED=1  the debugger's pause after frame 1 (cartwheel/debug.h) */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "cartwheel/jobs.h"
#include "cartwheel/profile.h"
#include "cartwheel/plugin.h"
#include "cartwheel/gl.h"
#include "cartwheel/core.h"
#include "cartwheel/debug.h"
#include "cartwheel/vfs.h"
#include "cartwheel/resource.h" /* cw_decompress_block */
#include "resource/cwpak.h"

#if !defined(_WIN32)
#include <dirent.h>
#include <sys/stat.h>
#endif

#if defined(__linux__)
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#include <windows.h>
#endif
#if defined(__ANDROID__)
#include <android/log.h>
#endif

/* stb_image's inflate (compiled into three.c) */
int stbi_zlib_decode_noheader_buffer(char *obuffer, int olen, const char *ibuffer, int ilen);

#include "stb_image_write.h" /* compiled once, in src/debug/png.c */

void cw__profile_thread_name(const char *name); /* debug/profile.c */
static void audio_cb(void *user, Uint8 *stream, int len) {
  (void)user;
  cw__profile_thread_name("audio");
  cw_platform *p = &cw__plat;
  int frames = len / (int)(2 * sizeof(float));
  if (p->audio_fill) p->audio_fill(p->audio_user, (float *)stream, frames);
  else memset(stream, 0, (size_t)len);
}

bool cw_audio_open(int rate, cw_audio_fill_fn fill, void *user) {
  cw_platform *p = &cw__plat;
  SDL_AudioSpec want = { 0 }, have;
  want.freq = rate;
  want.format = AUDIO_F32SYS;
  want.channels = 2;
  want.samples = 512;
  want.callback = audio_cb;
  if (p->audio_dev) { /* open: swap the callback under the device lock */
    SDL_LockAudioDevice(p->audio_dev);
    p->audio_fill = fill;
    p->audio_user = user;
    SDL_UnlockAudioDevice(p->audio_dev);
    return true;
  }
  p->audio_fill = fill; /* not open: no callback thread can be reading these */
  p->audio_user = user;
  p->audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
  if (!p->audio_dev) {
    cw_warn("no audio device (%s): running silent", SDL_GetError());
    p->audio_rate = rate;
    return false;
  }
  p->audio_rate = have.freq;
  SDL_PauseAudioDevice(p->audio_dev, 0);
  return true;
}
int cw_audio_rate(void) { return cw__plat.audio_rate; }
void cw_audio_close(void) {
  if (cw__plat.audio_dev) SDL_CloseAudioDevice(cw__plat.audio_dev);
  cw__plat.audio_dev = 0;
  cw__plat.audio_fill = NULL; /* as the cart platform: a later open starts with no stale callback */
  cw__plat.audio_user = NULL;
}

/* ── assets: a .cwpak when there is one, else loose files ───────────
 * The pack is CW_PAK, else <executable>.cwpak beside the binary (cwpack
 * writes it there for apps built with cw_add_app(... PACK manifest)). A
 * path the pack does not hold falls back to <asset root>/<path>, so a dev
 * build can override single files. The pack stays open; reads take a mutex
 * (assets may be read from job threads). Files open through SDL_RWops, so on
 * Android a relative path reads the APK's assets/ (where an export puts the
 * pack as game.cwpak). */
static char asset_root[1024] = "assets";
static char exe_path[1024];

typedef struct {
  SDL_RWops *f;
  cwpak_entry *index;
  char *strings;
  uint32_t count;
  char path[1024];
  SDL_mutex *lock;
  bool tried;
} cw_pak;
static cw_pak pak;

/* the running executable's path (for the default <exe>.cwpak) */
static void find_exe(const char *argv0) {
#if defined(__ANDROID__)
  (void)argv0;
  snprintf(exe_path, sizeof exe_path, "game"); /* the APK's assets/game.cwpak */
  return;
#elif defined(__linux__)
  ssize_t n = readlink("/proc/self/exe", exe_path, sizeof exe_path - 1);
  if (n > 0) { exe_path[n] = 0; return; }
#elif defined(__APPLE__)
  uint32_t cap = sizeof exe_path;
  if (_NSGetExecutablePath(exe_path, &cap) == 0) return;
#elif defined(_WIN32)
  DWORD n = GetModuleFileNameA(NULL, exe_path, sizeof exe_path);
  if (n > 0 && n < sizeof exe_path) {
    /* <name>.exe -> <name>, so the pack is <name>.cwpak on every OS */
    if (n > 4 && !_stricmp(exe_path + n - 4, ".exe")) exe_path[n - 4] = 0;
    return;
  }
#endif
  snprintf(exe_path, sizeof exe_path, "%s", argv0 ? argv0 : "");
}

static void pak_close(void) {
  if (pak.f) SDL_RWclose(pak.f);
  free(pak.index);
  free(pak.strings);
  pak.f = NULL;
  pak.index = NULL;
  pak.strings = NULL;
  pak.count = 0;
}

static bool rw_at(SDL_RWops *f, uint64_t off, void *dst, size_t n) {
  return SDL_RWseek(f, (Sint64)off, RW_SEEK_SET) == (Sint64)off && SDL_RWread(f, dst, 1, n) == n;
}

static bool pak_open(const char *path) {
  SDL_RWops *f = SDL_RWFromFile(path, "rb");
  if (!f) return false;
  cwpak_header h;
  if (!rw_at(f, 0, &h, sizeof h) || memcmp(h.magic, CWPAK_MAGIC, 8) || h.version < CWPAK_VERSION_MIN ||
      h.version > CWPAK_VERSION) {
    cw_error("%s is not a cwpak v%u..%u: rebuild it with cwpack", path, CWPAK_VERSION_MIN, CWPAK_VERSION);
    SDL_RWclose(f);
    return false;
  }
  pak.index = malloc((size_t)h.count * sizeof *pak.index + 1);
  pak.strings = malloc((size_t)h.strings_size + 1);
  if (!rw_at(f, h.index_offset, pak.index, (size_t)h.count * sizeof *pak.index) ||
      !rw_at(f, h.strings_offset, pak.strings, (size_t)h.strings_size)) {
    cw_error("%s: truncated pack (index or strings unreadable): rebuild it with cwpack", path);
    SDL_RWclose(f);
    free(pak.index);
    free(pak.strings);
    pak.index = NULL;
    pak.strings = NULL;
    return false;
  }
  pak.f = f;
  pak.count = h.count;
  snprintf(pak.path, sizeof pak.path, "%s", path);
  cw_log(CW_LOG_DEBUG, "assets: %s (%u entries)", path, h.count);
  return true;
}

static void pak_mount(void) {
  if (pak.tried) return;
  pak.tried = true;
  if (!pak.lock) pak.lock = SDL_CreateMutex();
  const char *env = getenv("CW_PAK");
  if (env && *env) {
    if (!pak_open(env)) cw_error("CW_PAK=%s: cannot open it; reading loose files from %s", env, asset_root);
    return;
  }
  char p[1100];
  snprintf(p, sizeof p, "%s.cwpak", exe_path);
  if (pak_open(p)) return;
#if defined(__APPLE__)
  /* an app bundle keeps it in Contents/Resources (iOS: the bundle root),
   * which is what SDL_GetBasePath answers inside a bundle */
  char *base = SDL_GetBasePath();
  const char *name = strrchr(exe_path, '/');
  if (base && name) {
    snprintf(p, sizeof p, "%s%s.cwpak", base, name + 1);
    pak_open(p);
  }
  SDL_free(base);
#endif
}

void cw_asset_remount(void) {
  if (pak.lock) SDL_LockMutex(pak.lock);
  pak_close();
  pak.tried = false;
  pak_mount();
  if (pak.lock) SDL_UnlockMutex(pak.lock);
}

bool cw_asset_set_root(const char *dir) {
  if (!dir || !*dir) return false;
  if (pak.lock) SDL_LockMutex(pak.lock);
  snprintf(asset_root, sizeof asset_root, "%s", dir);
  if (pak.lock) SDL_UnlockMutex(pak.lock);
  cw_vfs_rescan(); /* the asset layer's listing changed */
  return true;
}

const char *cw_asset_pack_path(void) {
  pak_mount();
  return pak.f ? pak.path : NULL;
}

static const cwpak_entry *pak_find(const char *path) {
  uint32_t lo = 0, hi = pak.count;
  size_t n = strlen(path);
  while (lo < hi) {
    uint32_t mid = (lo + hi) / 2;
    const cwpak_entry *e = &pak.index[mid];
    size_t m = e->path_len < n ? e->path_len : n;
    int c = memcmp(pak.strings + e->path_offset, path, m);
    if (!c) c = e->path_len < n ? -1 : e->path_len > n ? 1 : 0;
    if (!c) return e;
    if (c < 0) lo = mid + 1;
    else hi = mid;
  }
  return NULL;
}

/* -2 when not in the pack (or no pack); -1 on a read or decode error; else
 * bytes copied (min(raw, cap)). The lock covers the index lookup and the
 * file read only: decompression runs outside it, so resource jobs reading
 * several entries at once decompress in parallel. */
static long pak_read(const char *path, void *dst, size_t cap, long *size_only) {
  pak_mount();
  if (!pak.f) return -2;
  SDL_LockMutex(pak.lock);
  const cwpak_entry *e = pak.f ? pak_find(path) : NULL;
  if (!e) { SDL_UnlockMutex(pak.lock); return -2; }
  cwpak_entry ent = *e;
  if (size_only) { *size_only = (long)ent.raw_size; SDL_UnlockMutex(pak.lock); return 0; }
  if (!(ent.flags & (CWPAK_DEFLATE | CWPAK_ZSTD | CWPAK_LZ4))) {
    size_t n = ent.size < cap ? (size_t)ent.size : cap;
    long r = rw_at(pak.f, ent.offset, dst, n) ? (long)n : -1;
    SDL_UnlockMutex(pak.lock);
    return r;
  }
  char *z = malloc((size_t)ent.size + 1);
  bool ok = z && rw_at(pak.f, ent.offset, z, (size_t)ent.size);
  SDL_UnlockMutex(pak.lock);
  long r = -1;
  if (ok) {
    /* straight into dst when it holds the whole entry, else through a copy */
    char *out = cap >= ent.raw_size ? dst : malloc((size_t)ent.raw_size + 1);
    bool dec = false;
    if (!out) dec = false;
    else if (ent.flags & CWPAK_DEFLATE)
      dec = stbi_zlib_decode_noheader_buffer(out, (int)ent.raw_size, z, (int)ent.size) == (int)ent.raw_size;
    else
      dec = cw_decompress_block(ent.flags & CWPAK_ZSTD ? CW_COMPRESS_ZSTD : CW_COMPRESS_LZ4, z, (size_t)ent.size, out,
                                (size_t)ent.raw_size);
    if (dec) {
      r = (long)(cap < ent.raw_size ? cap : ent.raw_size);
      if (out != dst) memcpy(dst, out, (size_t)r);
    }
    if (out != dst) free(out);
  }
  free(z);
  if (r < 0) cw_error("%s: entry %s does not decompress: the pack is corrupt, rebuild it", pak.path, path);
  return r;
}

static SDL_RWops *open_asset(const char *path) {
  char full[2048];
  if (!path || strstr(path, "..")) return NULL; /* same rule a cart bundle enforces */
  snprintf(full, sizeof full, "%s/%s", asset_root, path);
  return SDL_RWFromFile(full, "rb");
}
long cw_asset_size(const char *path) {
  if (!path || strstr(path, "..")) return -1;
  long sz;
  if (pak_read(path, NULL, 0, &sz) == 0) return sz;
  SDL_RWops *f = open_asset(path);
  if (!f) return -1;
  long n = (long)SDL_RWsize(f);
  SDL_RWclose(f);
  return n;
}
long cw_asset_read(const char *path, void *dst, size_t cap) {
  if (!path || strstr(path, "..")) return -1;
  long r = pak_read(path, dst, cap, NULL);
  if (r != -2) return r;
  SDL_RWops *f = open_asset(path);
  if (!f) return -1;
  long n = (long)SDL_RWread(f, dst, 1, cap);
  SDL_RWclose(f);
  return n;
}

/* Every asset path (pack entries, then the loose files under the asset
 * root), for cw_vfs_list_dir. Dotfiles and dot-directories are skipped, as
 * wasmcart-pack skips them in a cart bundle. */
static void list_loose(char *rel, size_t rl, int depth, void (*cb)(const char *, int64_t, void *), void *user, int *count) {
  if (depth > 16 || *count > 50000) return;
  char full[2048];
  snprintf(full, sizeof full, "%s%s%s", asset_root, rl ? "/" : "", rel);
#if defined(_WIN32)
  (void)cb, (void)user;
#else
  DIR *d = opendir(full);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.') continue;
    size_t nl = strlen(e->d_name);
    if (rl + nl + 2 >= 1024) continue;
    if (rl) rel[rl] = '/';
    memcpy(rel + rl + (rl ? 1 : 0), e->d_name, nl + 1);
    size_t sub = rl + (rl ? 1 : 0) + nl;
    snprintf(full, sizeof full, "%s/%s", asset_root, rel);
    struct stat st;
    if (!stat(full, &st)) {
      if (S_ISDIR(st.st_mode)) list_loose(rel, sub, depth + 1, cb, user, count);
      else if (S_ISREG(st.st_mode)) { cb(rel, (int64_t)st.st_size, user); (*count)++; }
    }
    rel[rl] = 0;
  }
  closedir(d);
#endif
}
int cw__asset_list_all(void (*cb)(const char *path, int64_t size, void *user), void *user);
int cw__asset_list_all(void (*cb)(const char *path, int64_t size, void *user), void *user) {
  pak_mount();
  int count = 0;
  if (pak.f) {
    SDL_LockMutex(pak.lock);
    char p[1024];
    for (uint32_t i = 0; i < pak.count; i++) {
      const cwpak_entry *e = &pak.index[i];
      if (e->path_len >= sizeof p) continue;
      memcpy(p, pak.strings + e->path_offset, e->path_len);
      p[e->path_len] = 0;
      if (!strchr(p, '#')) { cb(p, (int64_t)e->raw_size, user); count++; }
    }
    SDL_UnlockMutex(pak.lock);
  }
  char rel[1024] = "";
  list_loose(rel, 0, 0, cb, user, &count);
  return count;
}

void cw_debug_mark(uint32_t id) { cw_log(CW_LOG_DEBUG, "mark %u at frame %llu", id, (unsigned long long)cw__plat.frame); }

void cw__platform_log(cw_log_level level, const char *msg) {
  static const char *names[] = { "debug", "info", "warn", "error" };
  if (level == CW_LOG_DEBUG && !getenv("CW_DEBUG")) return;
#if defined(__ANDROID__) /* logcat, tag "cartwheel" */
  static const int prio[] = { ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_WARN, ANDROID_LOG_ERROR };
  __android_log_print(prio[level], "cartwheel", "[%s] %s", names[level], msg);
#else
  fprintf(stderr, "[%s] %s\n", names[level], msg);
#endif
}

static void write_screenshot(const char *path) {
  int w, h;
  if (cw_gpu_backend() == CW_GPU_WEBGPU) { /* the frame texture, read back (top-down) */
    unsigned char *px = NULL;
    if (!cw__wgpu_readback(&px, &w, &h)) { cw_error("screenshot: WebGPU readback failed; %s not written", path); return; }
    stbi_flip_vertically_on_write(0);
    if (!stbi_write_png(path, w, h, 4, px, w * 4)) cw_error("screenshot: could not write %s", path);
    free(px);
    return;
  }
  cw_drawable_size(&w, &h);
  unsigned char *px = malloc((size_t)w * h * 4);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
  stbi_flip_vertically_on_write(1);
  if (!stbi_write_png(path, w, h, 4, px, w * 4)) cw_error("screenshot: could not write %s", path);
  free(px);
}

/* Called once per frame before the frame runs (the hot-reload host checks
 * for new code here). */
void (*cw__native_frame_hook)(void);

void cw__programs_register(void); /* src/render/programs.c */
/* ── save data (cw_app_desc.save) ───────────────────────────────────── */

static char save_path[1024];

static const char *save_file(void) {
  if (save_path[0]) return save_path;
  const char *env = getenv("CW_SAVE_FILE");
  if (env && env[0]) {
    snprintf(save_path, sizeof save_path, "%s", env);
    return save_path;
  }
  char *dir = SDL_GetPrefPath("cartwheel", cw__plat.app.title ? cw__plat.app.title : "app");
  if (!dir) return NULL;
  snprintf(save_path, sizeof save_path, "%ssave.dat", dir);
  SDL_free(dir);
  return save_path;
}

/* CW_START_CMDS="cmd1;cmd2": debug-channel requests a test host posts at load
 * (run-cart --start-cmds): request i runs at the start of frame i + 2, where
 * a cart host's mailbox delivers it (the host runs frame 1 on load). A
 * request written "@N cmd" runs at the start of frame N instead (N >= 2): a
 * test that changes something mid-run (a graphics preset) */
static char *start_all, *start_list[32];
static uint64_t start_at[32];
static int start_n;
static void start_cmds_load(void) {
  const char *env = getenv("CW_START_CMDS");
  if (!env || !*env) return;
  start_all = strdup(env);
  char *save = NULL;
  for (char *c = strtok_r(start_all, ";", &save); c && start_n < 32; c = strtok_r(NULL, ";", &save)) {
    uint64_t at = (uint64_t)start_n + 1;
    if (c[0] == '@') {
      char *end;
      at = strtoull(c + 1, &end, 10) - 1;
      while (*end == ' ') end++;
      c = end;
    }
    start_at[start_n] = at;
    start_list[start_n++] = c;
  }
}
static void start_cmd(uint64_t frame) {
  for (int i = 0; i < start_n; i++) {
    if (start_at[i] != frame) continue;
    char *rep = cw_debug_run(start_list[i]);
    fprintf(stderr, "start cmd: %s -> %s\n", start_list[i], rep);
    cw_free(rep);
  }
}

/* before init: the app's save buffer from its file (zeros in a test run) */
static void save_load(void) {
  cw_platform *p = &cw__plat;
  if (!p->app.save || !p->app.save_size) return;
  memset(p->app.save, 0, p->app.save_size);
  if (p->deterministic_run) return;
  const char *f = save_file();
  FILE *fp = f ? fopen(f, "rb") : NULL;
  if (!fp) return;
  size_t n = fread(p->app.save, 1, p->app.save_size, fp);
  fclose(fp);
  cw_info("save data: %zu bytes from %s", n, f);
}

void cw_save_commit(void) {
  cw_platform *p = &cw__plat;
  if (!p->app.save || !p->app.save_size || p->deterministic_run) return;
  const char *f = save_file();
  FILE *fp = f ? fopen(f, "wb") : NULL;
  if (!fp) {
    cw_warn("save data: cannot write %s", f ? f : "(no preference directory)");
    return;
  }
  fwrite(p->app.save, 1, p->app.save_size, fp);
  fclose(fp);
}

int cw_native_main(int argc, char **argv, cw_app_desc (*app_fn)(int, char **)) {
  cw_platform *p = &cw__plat;
  /* CW_PROGRAMS_LOG_DIR=<dir> (tools/programs.mjs, the gates): the states no
   * program table has go to <dir>/<app>.log, one file per app */
  const char *plog = getenv("CW_PROGRAMS_LOG_DIR");
  if (plog && *plog && argc > 0 && !getenv("T3_MISSING_LOG")) {
    const char *base = strrchr(argv[0], '/');
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.log", plog, base ? base + 1 : argv[0]);
#ifdef _WIN32
    _putenv_s("T3_MISSING_LOG", path);
#else
    setenv("T3_MISSING_LOG", path, 1);
#endif
  }
  cw__programs_register(); /* the app's three.c program tables, before any renderer */
  find_exe(argc > 0 ? argv[0] : NULL);
  const char *root = getenv("CW_ASSETS");
  if (root) snprintf(asset_root, sizeof asset_root, "%s", root);
  else if (argc > 0) { /* the build puts an app's assets beside it as <exe>.assets */
    char beside[1024];
    snprintf(beside, sizeof beside, "%s.assets", argv[0]);
    FILE *probe = fopen(beside, "r");
    if (probe) {
      fclose(probe);
      snprintf(asset_root, sizeof asset_root, "%s", beside);
    }
  }
  if (getenv("CW_SEED")) p->host_seed = (uint32_t)strtoul(getenv("CW_SEED"), NULL, 0);
  p->app = app_fn(argc, argv);
  if (p->app.width <= 0) p->app.width = 1280;
  if (p->app.height <= 0) p->app.height = 720;
  cw__resolve_scale();
  p->fixed_dt = 1.0 / (p->app.fixed_hz > 0 ? p->app.fixed_hz : 60);
  long fixed_frames = getenv("CW_FRAMES") ? strtol(getenv("CW_FRAMES"), NULL, 0) : 0;
  /* CW_FRAME_HZ: the virtual render rate of a test run (default 60); the
   * simulation still ticks at fixed_hz */
  double frame_hz = getenv("CW_FRAME_HZ") ? strtod(getenv("CW_FRAME_HZ"), NULL) : 60.0;
  if (frame_hz <= 0) frame_hz = 60.0;
  if (fixed_frames > 0) {
    p->deterministic_run = !getenv("CW_PLAYER_TIMING");
    p->app.vsync_off = true;
  }
  cw_jobs_init(p->app.workers == 0 ? -1 : p->app.workers < 0 ? 0 : p->app.workers);
  if (!cw__gpu_select(p->app.gpu, false)) return 1;
  if (!cw__sdl_init()) return 1;
  cw_plugins_init(); /* the build's runtime plugins register before the app (and its scenes) start */
  save_load(); /* the app's init runs in the first frame (cw__frame) */
  start_cmds_load();
  Uint64 freq = SDL_GetPerformanceFrequency(), t0 = SDL_GetPerformanceCounter(), last_present = t0;
  const char *vd = SDL_GetCurrentVideoDriver();
  bool headless = vd && (!strcmp(vd, "offscreen") || !strcmp(vd, "dummy"));
  bool dumped = false;
  while (!p->quit) {
    double now = fixed_frames > 0 ? (double)p->frame / frame_hz : (double)(SDL_GetPerformanceCounter() - t0) / (double)freq;
    if (cw__native_frame_hook) cw__native_frame_hook();
    start_cmd(p->frame);
    cw__frame(now);
    /* CW_START_PAUSED: the debugger's pause after the first frame, the frame a
     * cart's host runs on load, so play-in-editor starts at tick 1 everywhere */
    if (p->frame == 1 && getenv("CW_START_PAUSED") && atoi(getenv("CW_START_PAUSED"))) cw_debug_pause(true);
    if (fixed_frames > 0 && (long)p->frame >= fixed_frames) {
      const char *cmds = getenv("CW_DEBUG_CMDS");
      if (cmds) { /* "cmd1;cmd2": run each, print one JSON reply per line */
        char *all = strdup(cmds), *save = NULL;
        for (char *c = strtok_r(all, ";", &save); c; c = strtok_r(NULL, ";", &save)) {
          char *rep = cw_debug_run(c);
          printf("%s\n", rep);
          cw_free(rep);
        }
        free(all);
        fflush(stdout);
      }
      const char *dump = getenv("CW_DEBUG_DUMP");
      if (dump) cw_debug_dump_json(dump);
      dumped = true;
      const char *shot = getenv("CW_SCREENSHOT");
      if (shot) write_screenshot(shot);
      p->quit = true;
    }
    CW_PROFILE_BEGIN("present");
    if (cw_gpu_backend() == CW_GPU_WEBGPU) cw__wgpu_frame_end(true);
    else SDL_GL_SwapWindow(p->window);
    CW_PROFILE_END();
    cw__perf_mark(CW_PERF_PRESENT);
    /* no spinning where nothing paces the loop: a frozen game (the
     * debugger's pause) and a game with no display (offscreen, under an
     * editor or a tool) run at most 60 frames a second; steps run flat out */
    if (fixed_frames <= 0 && !cw_debug_steps_left() && (cw_debug_paused() || headless)) {
      Uint64 now_c = SDL_GetPerformanceCounter();
      double spent = (double)(now_c - last_present) / (double)freq;
      if (spent < 1.0 / 60) SDL_Delay((Uint32)((1.0 / 60 - spent) * 1000));
    }
    last_present = SDL_GetPerformanceCounter();
  }
  /* a game that quit by itself (cw_quit) still leaves its state behind */
  if (!dumped && getenv("CW_DEBUG_DUMP")) cw_debug_dump_json(getenv("CW_DEBUG_DUMP"));
  cw__shutdown();
  cw_save_commit();
  return 0;
}

/* ── text input ─────────────────────────────────────────────────────── */

static bool text_active;
void cw_text_input_start(void) {
  if (text_active) return;
  text_active = true;
  SDL_StartTextInput();
}
void cw_text_input_stop(void) {
  if (!text_active) return;
  text_active = false;
  SDL_StopTextInput();
}
bool cw_text_input_active(void) { return text_active; }
