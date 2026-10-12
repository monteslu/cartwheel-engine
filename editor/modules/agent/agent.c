/* Agent panel: a change-request inbox between the editor and an external
 * program that edits the project.
 *
 * A person types a request in plain language here; the external program
 * (anything that watches the project's .cwagent/ directory or talks to the
 * editor over the debug channel) picks it up, changes the game and replies;
 * the panel shows what changed and the person approves or rejects it.
 * Nothing leaves the machine and there is no network service: the queue is
 * files in the project, plus editor commands over the debug channel
 * (romdev `wasm({op:'command', text:'ed agent.inbox'})`).
 *
 * Files (natively; project directory, `.cwagent/`):
 *   requests/<id>.json   written by the editor: {"id","text","status",
 *                        "created"}; status pending -> working -> done |
 *                        failed -> approved | rejected
 *   replies/<id>.json    written by the external program (any time, overwritten):
 *                        {"status":"working|done|failed","summary":"...",
 *                         "notes":["..."],"probes":["..."]}
 *   ed/<name>.cmd        command lines for the editor, one per line, run as
 *                        if typed into `ed` (the file bridge for programs with
 *                        no debug channel); lines starting `game ` go to the
 *                        running game (play-in-editor) as debug-channel
 *                        requests (`game get Box color`). Answers land in
 *                        ed/<name>.reply, one JSON line per command line,
 *                        written whole (rename) once every line answered;
 *                        the .cmd is removed.
 *   snapshots/<id>/      the project's text files as they were when the
 *                        request was made (what reject restores)
 *
 * What the panel shows for a request: the text, the program's status and
 * summary, its notes and probes (including every `game` request it sent
 * through the bridge), the editor commands run while it worked (COMMAND
 * events: through `ed`, the bridge or the UI), the build result
 * (code.status) and a unified diff of every project text file it changed
 * (C sources from project.cwproj `sources`, scenes, the project file)
 * against the snapshot. Approve keeps it; Reject restores the snapshot,
 * undoes the scene commands made since the request and rebuilds + reloads
 * the game code (the game goes back to the old behaviour, live).
 *
 * Commands: agent.request, agent.inbox, agent.take, agent.note,
 * agent.reply, agent.show, agent.list, agent.approve, agent.reject. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/platform.h"
#include "cartwheel/vfs.h"
#include "cwed/editor.h"

#if CW_CART
#pragma GCC diagnostic ignored "-Wunused-function" /* helpers of the native-only paths */
#endif

#if !CW_CART
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

enum { R_PENDING, R_WORKING, R_DONE, R_FAILED, R_APPROVED, R_REJECTED };
static const char *const STATUS[] = { "pending", "working", "done", "failed", "approved", "rejected" };

typedef struct snap_file {
  const char *path; /* as listed (project-relative or absolute), interned */
  char *text;       /* NULL = did not exist */
} snap_file;

typedef struct doc_mark {
  cwed_doc *doc;
  int undo;
} doc_mark;

typedef CW_ARRAY(char *) strs;

typedef struct request {
  char id[16];
  char *text;
  int status;
  double created_ms, taken_ms, replied_ms; /* editor clock (ms); 0 = not yet */
  char created_at[32];                     /* wall clock, for the files */
  char *summary;
  strs notes;      /* from agent.note and the bridge's game requests */
  strs file_notes; /* the reply file's notes and probes (replaced on each read) */
  strs commands;
  CW_ARRAY(snap_file) snap;
  CW_ARRAY(doc_mark) marks;
  long long reply_mtime;
  char build[600]; /* code.status summary when it replied */
} request;

static CW_ARRAY(request *) reqs;
static int next_id = 1;
static int selected = -1;
static bool list_dirty = true, detail_dirty = true;
static bool bridging; /* a bridge line is running: its commands are the agent's */
static bool self_running; /* the panel itself runs a command (approve/reject internals): not the agent's */
static cw_node list, detail, input, header;

static double now_ms(void) {
#if !CW_CART
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
#else
  return cw_time() * 1000.0;
#endif
}

static char *dupstr(const char *s) {
  size_t n = strlen(s ? s : "");
  char *d = cw_alloc(n + 1, CW_MEM_EDITOR);
  memcpy(d, s ? s : "", n + 1);
  return d;
}

static void strs_clear(strs *a) {
  for (int i = 0; i < a->count; i++) cw_free(a->data[i]);
  cw_array_clear(a);
}

static request *find_req(const char *id) {
  for (int i = 0; i < reqs.count; i++)
    if (!strcmp(reqs.data[i]->id, id)) return reqs.data[i];
  return NULL;
}

/* the request the agent works on now: the newest working one, else none */
static request *active_req(void) {
  for (int i = reqs.count - 1; i >= 0; i--)
    if (reqs.data[i]->status == R_WORKING) return reqs.data[i];
  return NULL;
}

/* ── a little JSON reading (agent reply files, command replies) ─────── */

/* parse a JSON string at p (which points at the opening quote) into out */
static const char *json_string(const char *p, char *out, size_t cap) {
  size_t n = 0;
  if (*p != '"') return NULL;
  p++;
  while (*p && *p != '"') {
    char ch = *p++;
    if (ch == '\\' && *p) {
      char e = *p++;
      ch = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e;
      if (e == 'u') { /* \uXXXX: keep ASCII, else '?' */
        unsigned v = 0;
        for (int k = 0; k < 4 && *p; k++, p++) v = v * 16 + (unsigned)(*p <= '9' ? *p - '0' : (*p | 32) - 'a' + 10);
        ch = v < 128 ? (char)v : '?';
      }
    }
    if (n + 1 < cap) out[n++] = ch;
  }
  out[n] = 0;
  return *p == '"' ? p + 1 : NULL;
}

/* the value after "key": in a flat-ish object; NULL when absent */
static const char *json_find(const char *json, const char *key) {
  char pat[96];
  snprintf(pat, sizeof pat, "\"%s\"", key);
  const char *p = json ? strstr(json, pat) : NULL;
  if (!p) return NULL;
  p += strlen(pat);
  while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
  if (*p != ':') return NULL;
  p++;
  while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
  return p;
}

static bool json_get_str(const char *json, const char *key, char *out, size_t cap) {
  const char *p = json_find(json, key);
  out[0] = 0;
  return p && *p == '"' && json_string(p, out, cap);
}

/* every string of the array at "key" */
static void json_get_strs(const char *json, const char *key, void (*fn)(const char *s, void *u), void *u) {
  const char *p = json_find(json, key);
  if (!p || *p != '[') return;
  p++;
  char *buf = cw_alloc(65536, CW_MEM_EDITOR);
  for (;;) {
    while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    if (*p != '"') break;
    p = json_string(p, buf, 65536);
    if (!p) break;
    fn(buf, u);
  }
  cw_free(buf);
}

/* ── files ───────────────────────────────────────────────────────────── */

static const char *proj_abs(const char *p) {
  const char *dir = cwed_project_dir();
  if (p[0] == '/' || !dir[0]) return cw_frame_printf("%s", p);
  return cw_frame_printf("%s/%s", dir, p);
}

static char *read_text(const char *path) {
#if !CW_CART
  FILE *f = fopen(proj_abs(path), "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *b = cw_alloc((size_t)n + 1, CW_MEM_EDITOR);
  size_t got = fread(b, 1, (size_t)n, f);
  fclose(f);
  b[got] = 0;
  return b;
#else
  size_t len;
  char *t = cw_vfs_read(path, &len);
  if (!t) return NULL;
  char *b = cw_alloc(len + 1, CW_MEM_EDITOR);
  memcpy(b, t, len);
  b[len] = 0;
  free(t);
  return b;
#endif
}

static bool write_text(const char *path, const char *text) {
#if !CW_CART
  const char *a = proj_abs(path);
  /* mkdir -p the parent */
  char dir[1200];
  snprintf(dir, sizeof dir, "%s", a);
  for (char *s = dir + 1; *s; s++)
    if (*s == '/') {
      *s = 0;
      mkdir(dir, 0755);
      *s = '/';
    }
  char tmp[1300];
  snprintf(tmp, sizeof tmp, "%s.tmp%d", a, (int)getpid());
  FILE *f = fopen(tmp, "wb");
  if (!f) return false;
  fwrite(text, 1, strlen(text), f);
  fclose(f);
  return rename(tmp, a) == 0;
#else
  return cw_vfs_write(path, text, strlen(text));
#endif
}

static void remove_file(const char *path) {
#if !CW_CART
  remove(proj_abs(path));
#else
  cw_vfs_remove(path);
#endif
}

static long long mtime_of(const char *path) {
#if !CW_CART
  struct stat st;
  if (stat(proj_abs(path), &st)) return -1;
  return (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#else
  (void)path;
  return -1;
#endif
}

static bool has_disk(void) { return !CW_CART && cwed_project_dir()[0]; }

/* JSON-escaped string appended to a growing buffer */
typedef struct { char *b; size_t n, cap; } sbuf;
static void sb_put(sbuf *s, const char *t) {
  size_t k = strlen(t);
  if (s->n + k + 1 > s->cap) {
    size_t nc = (s->cap ? s->cap * 2 : 1024) + k;
    char *nb = cw_alloc(nc, CW_MEM_EDITOR);
    if (s->b) memcpy(nb, s->b, s->n), cw_free(s->b);
    s->b = nb;
    s->cap = nc;
  }
  memcpy(s->b + s->n, t, k + 1);
  s->n += k;
}
static void sb_json_str(sbuf *s, const char *t) {
  sb_put(s, "\"");
  char one[8];
  for (const unsigned char *p = (const unsigned char *)(t ? t : ""); *p; p++) {
    if (*p == '"' || *p == '\\') snprintf(one, sizeof one, "\\%c", *p);
    else if (*p == '\n') snprintf(one, sizeof one, "\\n");
    else if (*p == '\t') snprintf(one, sizeof one, "\\t");
    else if (*p < 0x20) snprintf(one, sizeof one, "\\u%04x", *p);
    else one[0] = (char)*p, one[1] = 0;
    sb_put(s, one);
  }
  sb_put(s, "\"");
}

static void save_request(request *r) {
  if (!has_disk()) return;
  sbuf s = { 0 };
  sb_put(&s, "{\"id\":");
  sb_json_str(&s, r->id);
  sb_put(&s, ",\"text\":");
  sb_json_str(&s, r->text);
  sb_put(&s, ",\"status\":");
  sb_json_str(&s, STATUS[r->status]);
  sb_put(&s, ",\"created\":");
  sb_json_str(&s, r->created_at);
  sb_put(&s, ",\"reply_file\":");
  sb_json_str(&s, cw_frame_printf(".cwagent/replies/%s.json", r->id));
  sb_put(&s, "}\n");
  write_text(cw_frame_printf(".cwagent/requests/%s.json", r->id), s.b);
  cw_free(s.b);
}

/* ── snapshots and diffs ─────────────────────────────────────────────── */

typedef CW_ARRAY(const char *) path_list;
static void add_path(const char *s, void *u) {
  path_list *l = u;
  const char *p = cw_intern(s);
  for (int i = 0; i < l->count; i++)
    if (l->data[i] == p) return;
  cw_array_push(l, p);
}

/* every text file an agent may change: C sources (code.files), the project's
 * scenes (project.info) and project.cwproj */
static void project_files(path_list *out) {
  cw_array_clear(out);
  char *json = NULL;
  self_running = true;
  if (cwed_run("code.files", &json)) json_get_strs(json, "files", add_path, out);
  cw_free(json);
  json = NULL;
  if (cwed_run("project.info", &json)) json_get_strs(json, "scenes", add_path, out);
  cw_free(json);
  self_running = false;
  add_path("project.cwproj", out);
}

static void snapshot(request *r) {
  path_list files = { 0 };
  project_files(&files);
  for (int i = 0; i < files.count; i++) {
    snap_file f = { files.data[i], read_text(files.data[i]) };
    cw_array_push(&r->snap, f);
    if (has_disk() && f.text) {
      /* on disk too, for a reject after a restart and for the agent to read */
      const char *p = f.path[0] == '/' ? cw_frame_printf("abs%s", f.path) : f.path;
      write_text(cw_frame_printf(".cwagent/snapshots/%s/%s", r->id, p), f.text);
    }
  }
  cw_array_free(&files);
  r->marks.count = 0;
  for (int i = 0; i < cwed_doc_count(); i++) {
    cwed_doc *d = cwed_doc_at(i);
    cw_array_push(&r->marks, ((doc_mark){ d, cwed_history(d).undo_count }));
  }
}

/* lines of a text (pointers into a copy) */
typedef struct { char *buf; const char **l; int n; } lines;
static lines split(const char *t) {
  lines L = { 0 };
  if (!t) return L;
  L.buf = dupstr(t);
  int cap = 64;
  L.l = cw_alloc(sizeof(char *) * (size_t)cap, CW_MEM_EDITOR);
  char *p = L.buf;
  while (*p) {
    if (L.n == cap) {
      const char **nl = cw_alloc(sizeof(char *) * (size_t)cap * 2, CW_MEM_EDITOR);
      memcpy(nl, L.l, sizeof(char *) * (size_t)cap);
      cw_free(L.l);
      L.l = nl;
      cap *= 2;
    }
    L.l[L.n++] = p;
    char *e = strchr(p, '\n');
    if (!e) break;
    *e = 0;
    p = e + 1;
  }
  return L;
}
static void lines_free(lines *L) {
  cw_free(L->buf);
  cw_free(L->l);
}

/* A unified diff of a -> b (3 lines of context). Common prefix and suffix
 * are trimmed; the middle is aligned by LCS when it is small enough, else
 * shown as replaced. Returns the number of changed lines. */
static int diff_text(sbuf *out, const char *name, const char *a, const char *b) {
  lines A = split(a), B = split(b);
  int pre = 0;
  while (pre < A.n && pre < B.n && !strcmp(A.l[pre], B.l[pre])) pre++;
  int suf = 0;
  while (suf < A.n - pre && suf < B.n - pre && !strcmp(A.l[A.n - 1 - suf], B.l[B.n - 1 - suf])) suf++;
  int na = A.n - pre - suf, nb = B.n - pre - suf;
  if (!na && !nb) {
    lines_free(&A), lines_free(&B);
    return 0;
  }
  /* ops: ' ' keep, '-' a only, '+' b only (over the middle) */
  int cap = na + nb + 1, nops = 0;
  char *op = cw_alloc((size_t)cap, CW_MEM_EDITOR);
  int *ia = cw_alloc(sizeof(int) * (size_t)cap, CW_MEM_EDITOR), *ib = cw_alloc(sizeof(int) * (size_t)cap, CW_MEM_EDITOR);
  if ((long)na * nb <= 600L * 600L && na && nb) {
    uint16_t *t = cw_alloc(sizeof(uint16_t) * (size_t)(na + 1) * (size_t)(nb + 1), CW_MEM_EDITOR);
#define T(i, j) t[(size_t)(i) * (size_t)(nb + 1) + (size_t)(j)]
    for (int i = na; i >= 0; i--)
      for (int j = nb; j >= 0; j--)
        T(i, j) = (i == na || j == nb) ? 0
                : !strcmp(A.l[pre + i], B.l[pre + j]) ? (uint16_t)(T(i + 1, j + 1) + 1)
                : (T(i + 1, j) >= T(i, j + 1) ? T(i + 1, j) : T(i, j + 1));
    int i = 0, j = 0;
    while (i < na || j < nb) {
      if (i < na && j < nb && !strcmp(A.l[pre + i], B.l[pre + j])) op[nops] = ' ', ia[nops] = i++, ib[nops++] = j++;
      else if (j < nb && (i == na || T(i, j + 1) > T(i + 1, j))) op[nops] = '+', ia[nops] = i, ib[nops++] = j++;
      else op[nops] = '-', ia[nops] = i++, ib[nops++] = j;
    }
#undef T
    cw_free(t);
  } else {
    for (int i = 0; i < na; i++) op[nops] = '-', ia[nops] = i, ib[nops++] = 0;
    for (int j = 0; j < nb; j++) op[nops] = '+', ia[nops] = na, ib[nops++] = j;
  }
  int changed = 0;
  sb_put(out, cw_frame_printf("--- %s\n+++ %s\n", name, name));
  /* one hunk: up to 3 context lines before and after the middle */
  int ctx0 = pre < 3 ? pre : 3, ctx1 = suf < 3 ? suf : 3;
  sb_put(out, cw_frame_printf("@@ -%d +%d @@\n", pre - ctx0 + 1, pre - ctx0 + 1));
  for (int k = pre - ctx0; k < pre; k++) sb_put(out, cw_frame_printf(" %s\n", A.l[k]));
  int run_keep = 0;
  for (int k = 0; k < nops; k++) {
    if (op[k] == ' ') {
      /* long unchanged stretches inside the middle collapse to "..." */
      int end = k;
      while (end < nops && op[end] == ' ') end++;
      int len = end - k;
      if (len > 6) {
        for (int m = k; m < k + 3; m++) sb_put(out, cw_frame_printf(" %s\n", A.l[pre + ia[m]]));
        sb_put(out, cw_frame_printf("@@ ... @@\n"));
        for (int m = end - 3; m < end; m++) sb_put(out, cw_frame_printf(" %s\n", A.l[pre + ia[m]]));
      } else
        for (int m = k; m < end; m++) sb_put(out, cw_frame_printf(" %s\n", A.l[pre + ia[m]]));
      k = end - 1;
      run_keep = 0;
      continue;
    }
    (void)run_keep;
    changed++;
    if (op[k] == '-') sb_put(out, cw_frame_printf("-%s\n", A.l[pre + ia[k]]));
    else sb_put(out, cw_frame_printf("+%s\n", B.l[pre + ib[k]]));
  }
  for (int k = A.n - suf; k < A.n - suf + ctx1; k++) sb_put(out, cw_frame_printf(" %s\n", A.l[k]));
  cw_free(op), cw_free(ia), cw_free(ib);
  lines_free(&A), lines_free(&B);
  return changed;
}

/* every file that differs from the snapshot (and files added since) */
static int diff_request(request *r, sbuf *out, path_list *changed_out) {
  int files = 0;
  for (int i = 0; i < r->snap.count; i++) {
    snap_file *f = &r->snap.data[i];
    char *now = read_text(f->path);
    bool same = (!now && !f->text) || (now && f->text && !strcmp(now, f->text));
    if (!same) {
      files++;
      if (changed_out) cw_array_push(changed_out, f->path);
      if (out) {
        if (!now) sb_put(out, cw_frame_printf("--- %s (deleted)\n", f->path));
        else diff_text(out, f->path, f->text ? f->text : "", now);
      }
    }
    cw_free(now);
  }
  path_list cur = { 0 };
  project_files(&cur);
  for (int i = 0; i < cur.count; i++) {
    bool known = false;
    for (int k = 0; k < r->snap.count && !known; k++) known = r->snap.data[k].path == cur.data[i];
    if (known) continue;
    char *now = read_text(cur.data[i]);
    if (!now) continue;
    files++;
    if (changed_out) cw_array_push(changed_out, cur.data[i]);
    if (out) {
      sb_put(out, cw_frame_printf("(new file)\n"));
      diff_text(out, cur.data[i], "", now);
    }
    cw_free(now);
  }
  cw_array_free(&cur);
  return files;
}

/* ── requests ───────────────────────────────────────────────────────── */

static request *new_request(const char *text) {
  request *r = cw_calloc(1, sizeof *r, CW_MEM_EDITOR);
  snprintf(r->id, sizeof r->id, "r%04d", next_id++);
  r->text = dupstr(text);
  r->status = R_PENDING;
  r->created_ms = now_ms();
#if !CW_CART
  time_t t = time(NULL);
  strftime(r->created_at, sizeof r->created_at, "%Y-%m-%dT%H:%M:%S", localtime(&t));
#endif
  snapshot(r);
  cw_array_push(&reqs, r);
  save_request(r);
#if !CW_CART
  if (has_disk()) { /* where the agent answers: there from the first request on */
    mkdir(proj_abs(".cwagent/replies"), 0755);
    mkdir(proj_abs(".cwagent/ed"), 0755);
  }
#endif
  selected = reqs.count - 1;
  list_dirty = detail_dirty = true;
  cwed_log(CWED_LOG_INFO, "agent request %s: %s", r->id, text);
  return r;
}

static void build_summary(char *out, size_t cap) {
  char *json = NULL;
  self_running = true;
  out[0] = 0;
  if (cwed_run("code.status", &json)) json_get_str(json, "summary", out, cap);
  self_running = false;
  cw_free(json);
}

static void set_status(request *r, int st) {
  if (r->status == st) return;
  r->status = st;
  if (st == R_WORKING && !r->taken_ms) r->taken_ms = now_ms();
  if ((st == R_DONE || st == R_FAILED) && !r->replied_ms) {
    r->replied_ms = now_ms();
    build_summary(r->build, sizeof r->build);
  }
  save_request(r);
  list_dirty = detail_dirty = true;
}

static void add_note(request *r, const char *kind, const char *text) {
  cw_array_push(&r->notes, dupstr(cw_frame_printf("%s: %s", kind, text)));
  detail_dirty = true;
}

/* the agent's reply file changed: take its status, summary, notes, probes */
static void note_cb(const char *s, void *u) {
  request *r = u;
  cw_array_push(&r->file_notes, dupstr(cw_frame_printf("note: %s", s)));
}
static void probe_cb(const char *s, void *u) {
  request *r = u;
  cw_array_push(&r->file_notes, dupstr(cw_frame_printf("probe: %s", s)));
}

static void read_reply_file(request *r) {
  const char *path = cw_frame_printf(".cwagent/replies/%s.json", r->id);
  long long m = mtime_of(path);
  if (m < 0 || m == r->reply_mtime) return;
  char *json = read_text(path);
  if (!json) return;
  r->reply_mtime = m;
  char st[32], sum[4096];
  json_get_str(json, "status", st, sizeof st);
  if (json_get_str(json, "summary", sum, sizeof sum)) {
    cw_free(r->summary);
    r->summary = dupstr(sum);
  }
  /* the file holds the agent's whole list each time */
  strs_clear(&r->file_notes);
  json_get_strs(json, "notes", note_cb, r);
  json_get_strs(json, "probes", probe_cb, r);
  cw_free(json);
  if (!strcmp(st, "working")) set_status(r, R_WORKING);
  else if (!strcmp(st, "done")) set_status(r, R_DONE);
  else if (!strcmp(st, "failed")) set_status(r, R_FAILED);
  detail_dirty = list_dirty = true;
}

/* load requests a previous session left (status, text, summary) */
static void load_existing(void) {
#if !CW_CART
  if (!has_disk()) return;
  DIR *d = opendir(proj_abs(".cwagent/requests"));
  if (!d) return;
  path_list names = { 0 };
  struct dirent *e;
  while ((e = readdir(d)))
    if (e->d_name[0] == 'r' && strstr(e->d_name, ".json")) add_path(e->d_name, &names);
  closedir(d);
  for (int i = 0; i < names.count; i++)
    for (int k = i + 1; k < names.count; k++)
      if (strcmp(names.data[k], names.data[i]) < 0) {
        const char *t = names.data[i];
        names.data[i] = names.data[k];
        names.data[k] = t;
      }
  for (int i = 0; i < names.count; i++) {
    char *json = read_text(cw_frame_printf(".cwagent/requests/%s", names.data[i]));
    if (!json) continue;
    request *r = cw_calloc(1, sizeof *r, CW_MEM_EDITOR);
    char buf[4096];
    json_get_str(json, "id", r->id, sizeof r->id);
    json_get_str(json, "text", buf, sizeof buf);
    r->text = dupstr(buf);
    json_get_str(json, "status", buf, sizeof buf);
    for (int k = 0; k < 6; k++)
      if (!strcmp(buf, STATUS[k])) r->status = k;
    json_get_str(json, "created", r->created_at, sizeof r->created_at);
    cw_free(json);
    int n = atoi(r->id + 1);
    if (n >= next_id) next_id = n + 1;
    cw_array_push(&reqs, r);
    read_reply_file(r);
  }
  cw_array_free(&names);
#endif
}

/* ── the command bridge (.cwagent/ed/NAME.cmd) ─────────────────────────── */

typedef struct batch {
  char name[256];
  strs lines, replies;
  int next;
  bool waiting; /* a `game` line is in flight */
} batch;
static batch *cur_batch;

static void game_cb(const char *json, void *user) {
  batch *b = user;
  if (b != cur_batch) return;
  const char *line = b->lines.data[b->next];
  char *reply = dupstr(json ? json : "{\"error\":\"no reply from the game\"}");
  /* keep replies one line each */
  for (char *p = reply; *p; p++)
    if (*p == '\n') *p = ' ';
  cw_array_push(&b->replies, reply);
  request *r = active_req();
  if (r) add_note(r, "game", cw_frame_printf("%s -> %.300s", line + 5, reply));
  b->next++;
  b->waiting = false;
}

static void finish_batch(batch *b) {
  sbuf s = { 0 };
  for (int i = 0; i < b->replies.count; i++) {
    sb_put(&s, b->replies.data[i]);
    sb_put(&s, "\n");
  }
  write_text(cw_frame_printf(".cwagent/ed/%s.reply", b->name), s.b ? s.b : "");
  cw_free(s.b);
  strs_clear(&b->lines);
  strs_clear(&b->replies);
  cw_array_free(&b->lines);
  cw_array_free(&b->replies);
  cw_free(b);
}

static void run_batch(batch *b) {
  while (b->next < b->lines.count && !b->waiting) {
    const char *line = b->lines.data[b->next];
    if (!strncmp(line, "game ", 5)) {
      const cwed_play_backend *pb = cwed_play_backend_get();
      if (!pb || !pb->running || !pb->running() || !pb->request) {
        cw_array_push(&b->replies, dupstr("{\"error\":\"the game is not running: run `play.start` first (play-in-editor)\"}"));
        b->next++;
        continue;
      }
      b->waiting = true;
      if (!pb->request(line + 5, game_cb, b)) {
        b->waiting = false;
        cw_array_push(&b->replies, dupstr("{\"error\":\"the play backend refused the request\"}"));
        b->next++;
      }
      continue;
    }
    char *json = NULL;
    bridging = true;
    cwed_run(line, &json);
    bridging = false;
    cw_array_push(&b->replies, json ? json : dupstr("{\"ok\":false}"));
    b->next++;
  }
  if (b->next >= b->lines.count && !b->waiting) {
    finish_batch(b);
    cur_batch = NULL;
  }
}

static void poll_bridge(void) {
#if !CW_CART
  if (!has_disk()) return;
  if (cur_batch) {
    run_batch(cur_batch);
    return;
  }
  DIR *d = opendir(proj_abs(".cwagent/ed"));
  if (!d) return;
  char best[256] = "";
  struct dirent *e;
  while ((e = readdir(d))) {
    size_t n = strlen(e->d_name);
    if (n > 4 && !strcmp(e->d_name + n - 4, ".cmd") && (!best[0] || strcmp(e->d_name, best) < 0))
      snprintf(best, sizeof best, "%s", e->d_name);
  }
  closedir(d);
  if (!best[0]) return;
  const char *rel = cw_frame_printf(".cwagent/ed/%s", best);
  char *text = read_text(rel);
  remove_file(rel);
  if (!text) return;
  batch *b = cw_calloc(1, sizeof *b, CW_MEM_EDITOR);
  snprintf(b->name, sizeof b->name, "%.*s", (int)(strlen(best) - 4), best);
  for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
    while (*line == ' ') line++;
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
    if (n && line[0] != '#') cw_array_push(&b->lines, dupstr(line));
  }
  cw_free(text);
  cur_batch = b;
  run_batch(b);
#endif
}

/* ── commands seen while the agent works ─────────────────────────────── */

static void on_command(const cwed_event *e, void *u) {
  (void)u;
  if (self_running || !e->line) return;
  request *r = active_req();
  if (!r) return;
  /* queries change nothing; the agent.* bookkeeping is shown elsewhere */
  char name[64];
  int n = 0;
  while (e->line[n] && e->line[n] != ' ' && n < 63) n++;
  snprintf(name, sizeof name, "%.*s", n, e->line);
  const cwed_command_desc *d = cwed_command_find(name);
  if (!strncmp(name, "agent.", 6) || (d && (d->flags & CWED_CMD_QUERY))) return;
  cw_array_push(&r->commands, dupstr(cw_frame_printf("%s%s%s", e->ok ? "" : "(failed) ", bridging ? "(bridge) " : "", e->line)));
  detail_dirty = true;
}

/* ── the detail text ─────────────────────────────────────────────────── */

static char *detail_text(request *r, bool with_diff) {
  sbuf s = { 0 };
  sb_put(&s, cw_frame_printf("## %s  [%s]\n%s\n", r->id, STATUS[r->status], r->text));
  if (r->taken_ms) sb_put(&s, cw_frame_printf("taken by the agent after %.1f s", (r->taken_ms - r->created_ms) / 1000));
  if (r->replied_ms) sb_put(&s, cw_frame_printf("%sreplied after %.1f s", r->taken_ms ? ", " : "", (r->replied_ms - r->created_ms) / 1000));
  if (r->taken_ms || r->replied_ms) sb_put(&s, "\n");
  if (r->status == R_PENDING) sb_put(&s, cw_frame_printf("waiting for an agent (.cwagent/requests/%s.json, or `ed agent.inbox`)\n", r->id));
  if (r->summary) sb_put(&s, cw_frame_printf("\n## Agent says\n%s\n", r->summary));
  if (r->notes.count || r->file_notes.count) {
    sb_put(&s, "\n## Notes and probes\n");
    for (int i = 0; i < r->file_notes.count; i++) sb_put(&s, cw_frame_printf("%s\n", r->file_notes.data[i]));
    for (int i = 0; i < r->notes.count; i++) sb_put(&s, cw_frame_printf("%s\n", r->notes.data[i]));
  }
  sb_put(&s, cw_frame_printf("\n## Commands run while it worked (%d)\n", r->commands.count));
  for (int i = 0; i < r->commands.count; i++) sb_put(&s, cw_frame_printf("> %s\n", r->commands.data[i]));
  char build[600];
  if (r->build[0]) snprintf(build, sizeof build, "%s", r->build);
  else build_summary(build, sizeof build);
  sb_put(&s, cw_frame_printf("\n## Build\n%s\n", build[0] ? build : "no build"));
  if (with_diff) {
    sbuf d = { 0 };
    int files = diff_request(r, &d, NULL);
    sb_put(&s, cw_frame_printf("\n## Files changed since the request (%d)\n", files));
    if (d.b) sb_put(&s, d.b);
    cw_free(d.b);
  }
  return s.b;
}

/* ── approve / reject ────────────────────────────────────────────────── */

static int revert(request *r, char *msg, int cap) {
  path_list changed = { 0 };
  diff_request(r, NULL, &changed);
  bool sources = false;
  int files = 0;
  for (int i = 0; i < changed.count; i++) {
    const char *p = changed.data[i];
    snap_file *f = NULL;
    for (int k = 0; k < r->snap.count && !f; k++)
      if (r->snap.data[k].path == p) f = &r->snap.data[k];
    if (f && f->text) write_text(p, f->text);
    else remove_file(p);
    files++;
    size_t n = strlen(p);
    if ((n > 2 && (!strcmp(p + n - 2, ".c") || !strcmp(p + n - 2, ".h"))) || (n > 4 && !strcmp(p + n - 4, ".cpp"))) sources = true;
  }
  cw_array_free(&changed);
  /* scene commands made since the request, newest first */
  int undone = 0;
  self_running = true;
  cwed_doc *was = cwed_doc_active();
  for (int i = 0; i < r->marks.count; i++) {
    cwed_doc *d = r->marks.data[i].doc;
    int idx = cwed_doc_index(d);
    if (idx < 0) continue;
    if (cwed_doc_active() != d) cwed_runf("scene.switch doc=%d", idx);
    while (cwed_history(d).undo_count > r->marks.data[i].undo && cwed_run("edit.undo", NULL)) undone++;
  }
  if (was && cwed_doc_index(was) >= 0 && cwed_doc_active() != was) cwed_runf("scene.switch doc=%d", cwed_doc_index(was));
  bool rebuilt = false;
  if (sources) rebuilt = cwed_run("code.build reload=true", NULL);
  self_running = false;
  snprintf(msg, (size_t)cap, "restored %d file(s), undid %d scene command(s)%s", files, undone,
           sources ? (rebuilt ? ", rebuilding the game code" : ", the rebuild did not start (code.status)") : "");
  return files;
}

/* ── commands ────────────────────────────────────────────────────────── */

static request *req_arg(cwed_call *c) {
  const char *id = cwed_arg(c, "id");
  request *r = find_req(id);
  if (!r) cwed_fail(c, "no request '%s' (agent.list shows them)", id);
  return r;
}

static void reply_req(cwed_json *j, request *r) {
  cwed_json_raw(j, "{");
  cwed_json_key(j, "id");
  cwed_json_str(j, r->id);
  cwed_json_key(j, "status");
  cwed_json_str(j, STATUS[r->status]);
  cwed_json_key(j, "text");
  cwed_json_str(j, r->text);
  cwed_json_key(j, "created");
  cwed_json_str(j, r->created_at);
  if (r->summary) {
    cwed_json_key(j, "summary");
    cwed_json_str(j, r->summary);
  }
  cwed_json_raw(j, "}");
}

static bool cmd_request(cwed_call *c) {
  const char *t = cwed_arg(c, "text");
  if (!*t) return cwed_fail(c, "say what to change: agent.request text=\"make the box blue\"");
  request *r = new_request(t);
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "id");
  cwed_json_str(j, r->id);
  cwed_json_key(j, "files");
  cwed_json_printf(j, "%d", r->snap.count);
  if (has_disk()) {
    cwed_json_key(j, "request_file");
    cwed_json_str(j, cw_frame_printf(".cwagent/requests/%s.json", r->id));
  }
  return true;
}

static bool cmd_inbox(cwed_call *c) {
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "requests");
  cwed_json_raw(j, "[");
  int k = 0;
  for (int i = 0; i < reqs.count; i++) {
    request *r = reqs.data[i];
    if (r->status != R_PENDING && r->status != R_WORKING) continue;
    if (k++) cwed_json_raw(j, ",");
    reply_req(j, r);
  }
  cwed_json_raw(j, "]");
  cwed_json_printf(j, ",\"how\":\"agent.take id, change the game (C, scenes, ed commands), code.build reload=true, "
                      "agent.reply id status=done summary=...\"");
  return true;
}

static bool cmd_list(cwed_call *c) {
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "requests");
  cwed_json_raw(j, "[");
  for (int i = 0; i < reqs.count; i++) {
    if (i) cwed_json_raw(j, ",");
    reply_req(j, reqs.data[i]);
  }
  cwed_json_raw(j, "]");
  return true;
}

static bool cmd_take(cwed_call *c) {
  request *r = req_arg(c);
  if (!r) return false;
  if (r->status != R_PENDING && r->status != R_WORKING)
    return cwed_fail(c, "%s is %s: only pending requests can be taken", r->id, STATUS[r->status]);
  set_status(r, R_WORKING);
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "request");
  reply_req(j, r);
  return true;
}

static bool cmd_note(cwed_call *c) {
  request *r = req_arg(c);
  if (!r) return false;
  add_note(r, cwed_arg(c, "kind"), cwed_arg(c, "text"));
  return true;
}

static bool cmd_reply(cwed_call *c) {
  request *r = req_arg(c);
  if (!r) return false;
  const char *st = cwed_arg(c, "status");
  int s = !strcmp(st, "done") ? R_DONE : !strcmp(st, "failed") ? R_FAILED : !strcmp(st, "working") ? R_WORKING : -1;
  if (s < 0) return cwed_fail(c, "status is done, failed or working (got '%s')", st);
  if (r->status >= R_APPROVED) return cwed_fail(c, "%s was already %s by the human", r->id, STATUS[r->status]);
  if (cwed_arg_given(c, "summary")) {
    cw_free(r->summary);
    r->summary = dupstr(cwed_arg(c, "summary"));
  }
  set_status(r, s);
  return true;
}

static bool cmd_show(cwed_call *c) {
  request *r = req_arg(c);
  if (!r) return false;
  cwed_json *j = cwed_reply(c);
  cwed_json_key(j, "request");
  reply_req(j, r);
  cwed_json_key(j, "notes");
  cwed_json_raw(j, "[");
  for (int i = 0; i < r->file_notes.count + r->notes.count; i++) {
    if (i) cwed_json_raw(j, ",");
    cwed_json_str(j, i < r->file_notes.count ? r->file_notes.data[i] : r->notes.data[i - r->file_notes.count]);
  }
  cwed_json_raw(j, "]");
  cwed_json_key(j, "commands");
  cwed_json_raw(j, "[");
  for (int i = 0; i < r->commands.count; i++) {
    if (i) cwed_json_raw(j, ",");
    cwed_json_str(j, r->commands.data[i]);
  }
  cwed_json_raw(j, "]");
  path_list changed = { 0 };
  sbuf d = { 0 };
  diff_request(r, &d, &changed);
  cwed_json_key(j, "changed_files");
  cwed_json_raw(j, "[");
  for (int i = 0; i < changed.count; i++) {
    if (i) cwed_json_raw(j, ",");
    cwed_json_str(j, changed.data[i]);
  }
  cwed_json_raw(j, "]");
  cwed_json_key(j, "diff");
  cwed_json_str(j, d.b ? d.b : "");
  cw_free(d.b);
  cw_array_free(&changed);
  char build[600];
  if (r->build[0]) snprintf(build, sizeof build, "%s", r->build);
  else build_summary(build, sizeof build);
  cwed_json_key(j, "build");
  cwed_json_str(j, build);
  if (r->replied_ms) {
    cwed_json_key(j, "reply_ms");
    cwed_json_printf(j, "%.0f", r->replied_ms - r->created_ms);
  }
  return true;
}

static bool cmd_approve(cwed_call *c) {
  request *r = req_arg(c);
  if (!r) return false;
  if (r->status >= R_APPROVED) return cwed_fail(c, "%s is already %s", r->id, STATUS[r->status]);
  set_status(r, R_APPROVED);
  cwed_log(CWED_LOG_INFO, "agent request %s approved", r->id);
  cwed_json_key(cwed_reply(c), "status");
  cwed_json_str(cwed_reply(c), "approved");
  return true;
}

static bool cmd_reject(cwed_call *c) {
  request *r = req_arg(c);
  if (!r) return false;
  if (r->status >= R_APPROVED) return cwed_fail(c, "%s is already %s", r->id, STATUS[r->status]);
  char msg[300];
  revert(r, msg, sizeof msg);
  set_status(r, R_REJECTED);
  add_note(r, "rejected", msg);
  cwed_log(CWED_LOG_INFO, "agent request %s rejected: %s", r->id, msg);
  cwed_json_key(cwed_reply(c), "reverted");
  cwed_json_str(cwed_reply(c), msg);
  return true;
}

/* ── the panel ───────────────────────────────────────────────────────── */

static int detail_highlight(cw_node te, const char *line, int len, int idx, cw_text_span *out, int cap, void *user) {
  (void)idx, (void)user, (void)cap;
  if (len <= 0) return 0;
  const char *name = "comment_color";
  if (!strncmp(line, "## ", 3)) name = "type_color";
  else if (!strncmp(line, "+++", 3) || !strncmp(line, "---", 3) || !strncmp(line, "@@", 2)) name = "type_color";
  else if (line[0] == '+') name = "string_color";
  else if (line[0] == '-') name = "keyword_color";
  else if (line[0] == '>') name = "number_color";
  else if (strstr(line, "FAILED") || strstr(line, "(failed)")) name = "keyword_color";
  else return 0;
  out[0] = (cw_text_span){ 0, len, cw_control_theme_color(te, "TextEdit", name) };
  return 1;
}

static void refresh_list(void) {
  list_dirty = false;
  if (!cw_node_valid(list)) return;
  cw_item_list_clear(list);
  int pending = 0;
  for (int i = 0; i < reqs.count; i++) {
    request *r = reqs.data[i];
    if (r->status <= R_WORKING) pending++;
    int len = (int)strlen(r->text);
    /* short: an ItemList is as wide as its longest item and would widen the dock */
    cw_item_list_add_item(list, cw_frame_printf("%s %s  %.*s%s", r->id, STATUS[r->status], len > 26 ? 24 : len, r->text, len > 26 ? "..." : ""),
                          (cw_texture){ 0 });
  }
  if (selected >= 0 && selected < reqs.count) {
    cw_item_list_select(list, selected, true);
    cw_item_list_ensure_visible(list, selected);
  }
  cw_label_set_text(header, has_disk() ? cw_frame_printf("%d open request(s); agents read .cwagent/requests/ or `ed agent.inbox`", pending)
                                       : cw_frame_printf("%d open request(s); agents use `ed agent.inbox`", pending));
}

/* TextEdit does not wrap: prose lines (not diff, command or heading
 * lines) wrap at words to the panel width */
static int wrap_cols;
static char *wrap_prose(const char *t) {
  sbuf s = { 0 };
  int cols = wrap_cols > 20 ? wrap_cols : 20;
  const char *p = t;
  while (*p) {
    const char *e = strchr(p, '\n');
    int len = e ? (int)(e - p) : (int)strlen(p);
    bool prose = len > cols && !strchr("+- @>#", p[0]);
    if (!prose) sb_put(&s, cw_frame_printf("%.*s\n", len, p));
    else {
      const char *q = p, *end = p + len;
      while (q < end) {
        int n = (int)(end - q);
        if (n > cols) {
          n = cols;
          while (n > cols / 2 && q[n] != ' ') n--;
          if (q[n] != ' ') n = cols;
        }
        sb_put(&s, cw_frame_printf("%.*s\n", n, q));
        q += n;
        while (q < end && *q == ' ') q++;
      }
    }
    if (!e) break;
    p = e + 1;
  }
  return s.b;
}

static void refresh_detail(void) {
  detail_dirty = false;
  if (cw_node_valid(detail)) wrap_cols = (int)((cw_control_rect(detail).w - 40) / 8.6f);
  if (!cw_node_valid(detail)) return;
  if (selected < 0 || selected >= reqs.count) {
    char *w = wrap_prose("Ask the agent for a change below. It edits the game (C, scenes) and hot reloads it; "
                         "its commands, notes, build and file diffs show here. Approve keeps the change; Reject "
                         "restores the files, undoes its scene edits and reloads the old code.");
    cw_text_edit_set_text(detail, w ? w : "");
    cw_free(w);
    return;
  }
  char *t = detail_text(reqs.data[selected], true);
  char *w = t ? wrap_prose(t) : NULL;
  cw_text_edit_set_text(detail, w ? w : "");
  cw_free(t);
  cw_free(w);
}

static void on_pick(cw_node n, const cw_value *a, int argc, void *u) {
  (void)n, (void)argc, (void)u;
  selected = (int)a[0].i;
  detail_dirty = true;
}

static void send_input(void) {
  const char *t = cw_line_edit_text(input);
  if (!t || !*t) return;
  cwed_runf("agent.request text=%s", cwed_quote(t));
  cw_line_edit_set_text(input, "");
}
static void on_send(cw_node n, const cw_value *a, int argc, void *u) {
  (void)n, (void)a, (void)argc, (void)u;
  send_input();
}
static void on_submit(cw_node n, const cw_value *a, int argc, void *u) {
  (void)n, (void)a, (void)argc, (void)u;
  send_input();
}
static void on_approve(cw_node n, const cw_value *a, int argc, void *u) {
  (void)n, (void)a, (void)argc, (void)u;
  if (selected >= 0 && selected < reqs.count) cwed_runf("agent.approve id=%s", reqs.data[selected]->id);
}
static void on_reject(cw_node n, const cw_value *a, int argc, void *u) {
  (void)n, (void)a, (void)argc, (void)u;
  if (selected >= 0 && selected < reqs.count) cwed_runf("agent.reject id=%s", reqs.data[selected]->id);
}

static cw_node button(const char *name, const char *text, cw_signal_fn fn) {
  cw_node b = cw_node_new("Button", name);
  cw_button_set_text(b, text);
  cw_connect(b, "pressed", (cw_node){ 0 }, NULL, fn, NULL, 0);
  return b;
}

static cw_node create(void *u) {
  (void)u;
  cw_node box = cw_node_new("VBoxContainer", NULL);
  header = cw_node_new("Label", "Header");
  cw_node_set(header, "ellipsis", cw_val_bool(true));
  cw_node_add_child(box, header);
  list = cw_node_new("ItemList", "Requests");
  cw_node_set(list, "custom_minimum_size", cw_val_vec2(0, 70));
  cw_connect(list, "item_selected", (cw_node){ 0 }, NULL, on_pick, NULL, 0);
  cw_node_add_child(box, list);
  detail = cw_node_new("TextEdit", "Detail");
  cw_node_set(detail, "editable", cw_val_bool(false));
  cw_node_set(detail, "highlight_current_line", cw_val_bool(false));
  cw_control_set_size_flags(detail, CW_SIZE_EXPAND_FILL, CW_SIZE_EXPAND_FILL);
  cw_text_edit_set_highlighter(detail, detail_highlight, NULL);
  cw_node_add_child(box, detail);
  cw_node row = cw_node_new("HBoxContainer", "Decide");
  cw_node_add_child(row, button("Approve", "Approve", on_approve));
  cw_node_add_child(row, button("Reject", "Reject (revert)", on_reject));
  cw_node_add_child(box, row);
  cw_node ask = cw_node_new("HBoxContainer", "Ask");
  input = cw_node_new("LineEdit", "Input");
  cw_node_set(input, "placeholder_text", cw_val_string("Ask the agent for a change..."));
  cw_control_set_size_flags(input, CW_SIZE_EXPAND_FILL, CW_SIZE_SHRINK_CENTER);
  cw_connect(input, "text_submitted", (cw_node){ 0 }, NULL, on_submit, NULL, 0);
  cw_node_add_child(ask, input);
  cw_node_add_child(ask, button("Send", "Send", on_send));
  cw_node_add_child(box, ask);
  refresh_list();
  refresh_detail();
  return box;
}

/* debug fields (tests read them by name): requests, open ones, a hash of their texts and statuses */
static int32_t dbg_requests, dbg_open;
static uint32_t dbg_hash;

static int frames;
static void frame(double dt) {
  (void)dt;
  frames++;
  dbg_requests = reqs.count;
  dbg_open = 0;
  dbg_hash = CW_HASH32_INIT;
  for (int i = 0; i < reqs.count; i++) {
    dbg_open += reqs.data[i]->status <= R_WORKING;
    dbg_hash = cw_hash32(reqs.data[i]->text, strlen(reqs.data[i]->text), dbg_hash);
    dbg_hash = cw_hash32(STATUS[reqs.data[i]->status], strlen(STATUS[reqs.data[i]->status]), dbg_hash);
  }
  if (frames % 3 == 0) poll_bridge();
  if (frames % 10 == 0 && has_disk())
    for (int i = 0; i < reqs.count; i++)
      if (reqs.data[i]->status <= R_FAILED) read_reply_file(reqs.data[i]);
  /* while the agent works the diff moves: refresh twice a second */
  if (frames % 30 == 0 && active_req()) detail_dirty = true;
  if (frames % 15 == 0 && cw_node_valid(detail) && (int)((cw_control_rect(detail).w - 40) / 8.6f) != wrap_cols) detail_dirty = true;
  if (list_dirty) refresh_list();
  if (detail_dirty) refresh_detail();
}

static void free_req(request *r) {
  cw_free(r->text);
  cw_free(r->summary);
  strs_clear(&r->notes);
  strs_clear(&r->file_notes);
  strs_clear(&r->commands);
  cw_array_free(&r->notes);
  cw_array_free(&r->file_notes);
  cw_array_free(&r->commands);
  for (int i = 0; i < r->snap.count; i++) cw_free(r->snap.data[i].text);
  cw_array_free(&r->snap);
  cw_array_free(&r->marks);
  cw_free(r);
}

static void on_project(const cwed_event *e, void *u) {
  (void)e, (void)u;
  for (int i = 0; i < reqs.count; i++) free_req(reqs.data[i]);
  cw_array_clear(&reqs);
  next_id = 1;
  selected = -1;
  load_existing();
  if (reqs.count) selected = reqs.count - 1;
  list_dirty = detail_dirty = true;
}

static void init(void) {
  static const cwed_param p_text[] = { { "text", CWED_ARG_STRING, NULL, "what the human wants, in plain language" }, { 0 } };
  static const cwed_param p_id[] = { { "id", CWED_ARG_STRING, NULL, "request id (r0001)" }, { 0 } };
  static const cwed_param p_note[] = { { "id", CWED_ARG_STRING, NULL, "request id" },
                                       { "text", CWED_ARG_STRING, NULL, "what the agent did, read or found" },
                                       { "kind", CWED_ARG_STRING, "note", "note | probe | build | plan" }, { 0 } };
  static const cwed_param p_reply[] = { { "id", CWED_ARG_STRING, NULL, "request id" },
                                        { "status", CWED_ARG_STRING, "done", "done | failed | working" },
                                        { "summary", CWED_ARG_STRING, "", "what changed, for the human" }, { 0 } };
  cwed_command_register(&(cwed_command_desc){ .name = "agent.request", .title = "Ask the Agent", .params = p_text,
    .doc = "Queue a plain-language request for an agent (.cwagent/requests/<id>.json natively; `agent.inbox` over ed). "
           "Snapshots the project's text files so a reject can restore them.", .exec = cmd_request });
  cwed_command_register(&(cwed_command_desc){ .name = "agent.inbox", .title = "Agent Inbox", .flags = CWED_CMD_QUERY,
    .doc = "Requests waiting for an agent or being worked on (for agents polling over ed).", .exec = cmd_inbox });
  cwed_command_register(&(cwed_command_desc){ .name = "agent.list", .title = "Agent Requests", .flags = CWED_CMD_QUERY | CWED_CMD_HIDDEN,
    .doc = "Every request this project has, with status.", .exec = cmd_list });
  cwed_command_register(&(cwed_command_desc){ .name = "agent.take", .title = "Take Request", .params = p_id, .flags = CWED_CMD_HIDDEN,
    .doc = "The agent starts on a request (status working): the panel records the commands it runs from now on.", .exec = cmd_take });
  cwed_command_register(&(cwed_command_desc){ .name = "agent.note", .title = "Agent Note", .params = p_note, .flags = CWED_CMD_HIDDEN,
    .doc = "The agent records what it did or read (a probe of the game, a plan) on a request.", .exec = cmd_note });
  cwed_command_register(&(cwed_command_desc){ .name = "agent.reply", .title = "Agent Reply", .params = p_reply, .flags = CWED_CMD_HIDDEN,
    .doc = "The agent finishes (done / failed) or updates (working) a request with a summary for the human.", .exec = cmd_reply });
  cwed_command_register(&(cwed_command_desc){ .name = "agent.show", .title = "Show Request", .params = p_id, .flags = CWED_CMD_QUERY,
    .doc = "A request in full: status, summary, notes and probes, commands run, changed files and their unified diff, build.",
    .exec = cmd_show });
  cwed_command_register(&(cwed_command_desc){ .name = "agent.approve", .title = "Approve Agent Change", .params = p_id,
    .doc = "Keep what the agent did for a request.", .exec = cmd_approve });
  cwed_command_register(&(cwed_command_desc){ .name = "agent.reject", .title = "Reject Agent Change", .params = p_id,
    .doc = "Revert what the agent did: restore the project's files from the request's snapshot, undo the scene commands "
           "made since, and rebuild + reload the game code.", .exec = cmd_reject });
  cwed_panel_register(&(cwed_panel_desc){ .id = "agent", .title = "Agent", .slot = "right_bottom", .order = 10, .create = create });
  cwed_menu_add("View/Agent Inbox", "panel.show id=agent", 400);
  cwed_subscribe(CWED_EV_COMMAND, on_command, NULL);
  cwed_subscribe(CWED_EV_PROJECT, on_project, NULL);
  load_existing();
  if (reqs.count) selected = reqs.count - 1;
  cw_debug_i32("agent.requests", &dbg_requests);
  cw_debug_i32("agent.open", &dbg_open);
  cw_debug_u32("agent.hash", &dbg_hash);
}

static void shutdown(void) {
  for (int i = 0; i < reqs.count; i++) free_req(reqs.data[i]);
  cw_array_free(&reqs);
}

CWED_MODULE(agent) = { .title = "Agent", .init = init, .frame = frame, .shutdown = shutdown };
