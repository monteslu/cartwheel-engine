/* story.c: the story runtime (cartwheel/story.h). Reads what tools/story-compile.mjs packs (the layout is described
 * there), runs scenes beat by beat on the fixed tick, picks pool lines, draws subtitles and choices, saves.
 *
 * The script format and the beat layout follow keyholder's story VM (github.com/virgilvox/keyholder, MIT, see
 * CREDITS): beats with jumps for branches, choices whose options jump past the menu, flags and stats, barks. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/story.h"
#include "cartwheel/speech.h"
#include "cartwheel/debug.h"
#include "cartwheel/input.h"
#include "cartwheel/platform.h"
#include "cartwheel/prompt.h"
#include "cartwheel/render2d.h"
#include "cartwheel/ui.h"

enum { OP_END, OP_SAY, OP_ASIDE, OP_CHOICE, OP_OPTION, OP_JUMP, OP_JUMP_NOT, OP_SET, OP_CLEAR, OP_ADD, OP_SETSTAT, OP_WAIT, OP_SHOT,
       OP_EMIT, OP_GOTO, OP_CALL, OP_SAVE, OP_RET, OP_COUNT };

typedef struct { const char *id, *name, *voice; uint32_t color; float speed; } cast_t;
typedef struct { const char *name; int init; } stat_t;
typedef struct { const char *name; uint32_t start; bool save; } scene_t;
typedef struct { const char *name; uint32_t start, count; float cooldown; bool once; int priority; } pool_t;
typedef struct { int who, emo; float amount; const char *text; uint32_t key; int cond; } pline_t;
typedef struct { int op, who, a, b, c; float f; const char *s, *s2; uint32_t key; int target, cond; } beat_t;
typedef struct { uint32_t first, count; } cond_t;
typedef struct { int kind, cmp, idx, val; } atom_t;

#define MAX_DEPTH 8
#define MAX_STEPS 20000

struct cw_story {
  cw_story_desc d;
  uint8_t *blob; size_t blob_len;
  int ncast, nstat, nflag, nscene, npool, npline, nbeat, ncond, natom;
  cast_t *cast; stat_t *stats; scene_t *scenes; pool_t *pools; pline_t *plines; beat_t *beats; cond_t *conds; atom_t *atoms;
  const char **flag_names;
  uint32_t *flag_hash, *stat_hash, *scene_hash, *pool_hash;
  /* state */
  bool *flags; int *statv; bool *seen; bool *said_once; double *said_at; float *pool_cool; int *pool_last;
  double now;
  int scene, pc; int stack_scene[MAX_DEPTH], stack_pc[MAX_DEPTH], depth;
  bool line_on; cw_story_line line; float line_t, line_dur;
  bool aside_on; cw_story_line aside; float aside_t, aside_dur; int aside_prio;
  bool choosing; int opt_beat[CW_STORY_MAX_OPTIONS]; bool opt_on[CW_STORY_MAX_OPTIONS]; int nopt, cursor, opt_default; float choice_t, choice_timeout;
  const char *prompt;
  float wait_t; bool waiting;
  bool skipping; float skip_hold;
  int resume_scene;
  bool subtitles;
  cw_action a_adv, a_skip, a_up, a_down;
  bool adv_pressed, up_pressed, down_pressed, skip_held;
  /* debug */
  int32_t dbg_scene, dbg_beat, lines, asides, asides_dropped, choices, last_choice, dbg_cursor, skips, scene_skips, repeats, dbg_waiting,
      emits, shots, saves;
  uint32_t line_key, flags_hash_v;
  char **dbg_names; int ndbg_names;
  bool warned_unknown;
};

static uint32_t fnv(const char *a, const char *b) {
  uint32_t h = 2166136261u;
  for (const char *p = a; p && *p; p++) { h ^= (uint8_t)*p; h *= 16777619u; }
  if (b) { h ^= '|'; h *= 16777619u; for (const char *p = b; *p; p++) { h ^= (uint8_t)*p; h *= 16777619u; } }
  return h;
}
uint32_t cw_story__name_hash(const char *s) { return fnv("", s); }

/* ---- loading ---- */
typedef struct { const uint8_t *p, *end; bool bad; } rd;
static uint32_t ru32(rd *r) { if (r->p + 4 > r->end) { r->bad = true; return 0; } uint32_t v; memcpy(&v, r->p, 4); r->p += 4; return v; }
static int32_t ri32(rd *r) { return (int32_t)ru32(r); }
static float rf32(rd *r) { uint32_t u = ru32(r); float f; memcpy(&f, &u, 4); return f; }
static uint16_t ru16(rd *r) { if (r->p + 2 > r->end) { r->bad = true; return 0; } uint16_t v; memcpy(&v, r->p, 2); r->p += 2; return v; }

static const char *str_at(const char *strs, uint32_t n, uint32_t o) { return o < n ? strs + o : ""; }
static bool parse(cw_story *st) {
  rd r = { st->blob, st->blob + st->blob_len, false };
  if (st->blob_len < 48 || memcmp(st->blob, "CWST", 4)) return false;
  r.p += 4;
  if (ru32(&r) != 1) return false;
  st->ncast = (int)ru32(&r); st->nstat = (int)ru32(&r); st->nflag = (int)ru32(&r); st->nscene = (int)ru32(&r); st->npool = (int)ru32(&r);
  st->npline = (int)ru32(&r); st->nbeat = (int)ru32(&r); st->ncond = (int)ru32(&r); st->natom = (int)ru32(&r);
  uint32_t strsz = ru32(&r);
  size_t body = (size_t)st->ncast * 20 + (size_t)st->nstat * 8 + (size_t)st->nflag * 4 + (size_t)st->nscene * 12 + (size_t)st->npool * 24 +
                (size_t)st->npline * 24 + (size_t)st->nbeat * 40 + (size_t)st->ncond * 8 + (size_t)st->natom * 12;
  if (48 + body + strsz != st->blob_len || strsz < 1) return false;
  const char *strs = (const char *)st->blob + 48 + body;
  if (strs[strsz - 1] != 0) return false;
#define STR(o) str_at(strs, strsz, (o))
  st->cast = calloc((size_t)st->ncast + 1, sizeof *st->cast);
  st->stats = calloc((size_t)st->nstat + 1, sizeof *st->stats);
  st->flag_names = calloc((size_t)st->nflag + 1, sizeof *st->flag_names);
  st->scenes = calloc((size_t)st->nscene + 1, sizeof *st->scenes);
  st->pools = calloc((size_t)st->npool + 1, sizeof *st->pools);
  st->plines = calloc((size_t)st->npline + 1, sizeof *st->plines);
  st->beats = calloc((size_t)st->nbeat + 1, sizeof *st->beats);
  st->conds = calloc((size_t)st->ncond + 1, sizeof *st->conds);
  st->atoms = calloc((size_t)st->natom + 1, sizeof *st->atoms);
  for (int i = 0; i < st->ncast; i++) {
    cast_t *c = &st->cast[i];
    c->id = STR(ru32(&r)); c->name = STR(ru32(&r)); c->voice = STR(ru32(&r)); c->color = ru32(&r); c->speed = rf32(&r);
  }
  for (int i = 0; i < st->nstat; i++) { st->stats[i].name = STR(ru32(&r)); st->stats[i].init = ri32(&r); }
  for (int i = 0; i < st->nflag; i++) st->flag_names[i] = STR(ru32(&r));
  for (int i = 0; i < st->nscene; i++) { st->scenes[i].name = STR(ru32(&r)); st->scenes[i].start = ru32(&r); st->scenes[i].save = ru32(&r) & 1; }
  for (int i = 0; i < st->npool; i++) {
    pool_t *p = &st->pools[i];
    p->name = STR(ru32(&r)); p->start = ru32(&r); p->count = ru32(&r); p->cooldown = rf32(&r); p->once = ru32(&r) & 1; p->priority = ri32(&r);
    if (p->start + p->count > (uint32_t)st->npline) r.bad = true;
  }
  for (int i = 0; i < st->npline; i++) {
    pline_t *l = &st->plines[i];
    l->who = ri32(&r); l->emo = ri32(&r); l->amount = rf32(&r); l->text = STR(ru32(&r)); l->key = ru32(&r); l->cond = ri32(&r);
    if (l->who < 0 || l->who >= st->ncast || l->cond >= st->ncond) r.bad = true;
  }
  for (int i = 0; i < st->nbeat; i++) {
    beat_t *b = &st->beats[i];
    b->op = ru16(&r); b->who = (int16_t)ru16(&r); b->a = ri32(&r); b->b = ri32(&r); b->c = ri32(&r); b->f = rf32(&r);
    b->s = STR(ru32(&r)); b->s2 = STR(ru32(&r)); b->key = ru32(&r); b->target = ri32(&r); b->cond = ri32(&r);
    if (b->op >= OP_COUNT || b->who >= st->ncast || b->cond >= st->ncond || b->target > st->nbeat) r.bad = true;
  }
  for (int i = 0; i < st->ncond; i++) { st->conds[i].first = ru32(&r); st->conds[i].count = ru32(&r); if (st->conds[i].first + st->conds[i].count > (uint32_t)st->natom) r.bad = true; }
  for (int i = 0; i < st->natom; i++) {
    if (r.p + 12 > r.end) { r.bad = true; break; }
    st->atoms[i].kind = r.p[0]; st->atoms[i].cmp = r.p[1]; r.p += 4; st->atoms[i].idx = ri32(&r); st->atoms[i].val = ri32(&r);
  }
  for (int i = 0; i < st->nscene; i++) if (st->scenes[i].start >= (uint32_t)st->nbeat) r.bad = true;
#undef STR
  if (r.bad) return false;
  st->flags = calloc((size_t)st->nflag + 1, 1);
  st->statv = calloc((size_t)st->nstat + 1, sizeof(int));
  st->seen = calloc((size_t)st->nscene + 1, 1);
  st->said_once = calloc((size_t)st->npline + 1, 1);
  st->said_at = calloc((size_t)st->npline + 1, sizeof(double));
  st->pool_cool = calloc((size_t)st->npool + 1, sizeof(float));
  st->pool_last = calloc((size_t)st->npool + 1, sizeof(int));
  st->flag_hash = calloc((size_t)st->nflag + 1, 4); st->stat_hash = calloc((size_t)st->nstat + 1, 4);
  st->scene_hash = calloc((size_t)st->nscene + 1, 4); st->pool_hash = calloc((size_t)st->npool + 1, 4);
  for (int i = 0; i < st->nflag; i++) st->flag_hash[i] = fnv("", st->flag_names[i]);
  for (int i = 0; i < st->nstat; i++) st->stat_hash[i] = fnv("", st->stats[i].name);
  for (int i = 0; i < st->nscene; i++) st->scene_hash[i] = fnv("", st->scenes[i].name);
  for (int i = 0; i < st->npool; i++) st->pool_hash[i] = fnv("", st->pools[i].name);
  return true;
}

void cw_story_reset(cw_story *st) {
  if (!st) return;
  memset(st->flags, 0, (size_t)st->nflag);
  for (int i = 0; i < st->nstat; i++) st->statv[i] = st->stats[i].init;
  memset(st->seen, 0, (size_t)st->nscene);
  memset(st->said_once, 0, (size_t)st->npline);
  for (int i = 0; i < st->npline; i++) st->said_at[i] = -1e9;
  for (int i = 0; i < st->npool; i++) { st->pool_cool[i] = 0; st->pool_last[i] = -1; }
  st->scene = -1; st->depth = 0; st->line_on = st->aside_on = st->choosing = st->waiting = st->skipping = false;
  st->resume_scene = -1;
}

static cw_action act(const char *name, const char *def) { return cw_action_find(name ? name : def); }

cw_story *cw_story_new(const cw_story_desc *desc) {
  if (!desc) return NULL;
  cw_story *st = calloc(1, sizeof *st);
  st->d = *desc;
  if (desc->data && desc->size) { st->blob = malloc(desc->size); memcpy(st->blob, desc->data, desc->size); st->blob_len = desc->size; }
  else if (desc->path) {
    long n = cw_asset_size(desc->path);
    if (n > 0) { st->blob = malloc((size_t)n); st->blob_len = (size_t)cw_asset_read(desc->path, st->blob, (size_t)n); }
  }
  if (!st->blob || !parse(st)) {
    cw_error("story: %s is missing or not a compiled story (tools/story-compile.mjs)", desc->path ? desc->path : "(data)");
    cw_story_free(st);
    return NULL;
  }
  if (st->d.reading_cps <= 0) st->d.reading_cps = 14;
  if (st->d.line_gap <= 0) st->d.line_gap = 0.45f;
  st->a_adv = act(desc->advance_action, "ui_accept"); st->a_skip = act(desc->skip_action, "ui_back");
  st->a_up = act(desc->up_action, "ui_up"); st->a_down = act(desc->down_action, "ui_down");
  st->subtitles = true;
  st->dbg_scene = -1; st->last_choice = -1;
  cw_story_reset(st);
  return st;
}

void cw_story_free(cw_story *st) {
  if (!st) return;
  free(st->blob); free(st->cast); free(st->stats); free(st->flag_names); free(st->scenes); free(st->pools); free(st->plines);
  free(st->beats); free(st->conds); free(st->atoms); free(st->flags); free(st->statv); free(st->seen); free(st->said_once);
  free(st->said_at); free(st->pool_cool); free(st->pool_last); free(st->flag_hash); free(st->stat_hash); free(st->scene_hash); free(st->pool_hash);
  free(st);
}

/* ---- conditions ---- */
static bool cond_true(const cw_story *st, int c) {
  if (c < 0) return true;
  const cond_t *k = &st->conds[c];
  bool group = true; /* atoms joined by and; groups by or */
  for (uint32_t i = 0; i < k->count; i++) {
    const atom_t *a = &st->atoms[k->first + i];
    if (a->kind == 5) { if (group) return true; group = true; continue; }
    if (!group) continue;
    bool ok = true;
    switch (a->kind) {
      case 0: ok = a->idx >= 0 && a->idx < st->nflag && st->flags[a->idx]; break;
      case 1: ok = !(a->idx >= 0 && a->idx < st->nflag && st->flags[a->idx]); break;
      case 2: {
        int v = a->idx >= 0 && a->idx < st->nstat ? st->statv[a->idx] : 0;
        switch (a->cmp) { case 0: ok = v >= a->val; break; case 1: ok = v <= a->val; break; case 2: ok = v > a->val; break;
          case 3: ok = v < a->val; break; case 4: ok = v == a->val; break; default: ok = v != a->val; break; }
        break;
      }
      case 3: ok = a->idx >= 0 && a->idx < st->nscene && st->seen[a->idx]; break;
      case 4: ok = !(a->idx >= 0 && a->idx < st->nscene && st->seen[a->idx]); break;
      default: ok = false;
    }
    if (!ok) group = false;
  }
  return group;
}

/* ---- lines ---- */
static float line_seconds(const cw_story *st, uint32_t key, const char *text) {
  float v = st->d.speech ? cw_speech_seconds(st->d.speech, key) : 0;
  if (v > 0) return v;
  float r = (float)strlen(text) / st->d.reading_cps + 0.4f;
  return r < 1.2f ? 1.2f : r;
}
static void fill_line(const cw_story *st, cw_story_line *l, int who, const char *text, uint32_t key, int emo, float amount) {
  const cast_t *c = &st->cast[who];
  *l = (cw_story_line){ .who = who, .speaker = c->name, .cast_id = c->id, .text = text, .key = key,
                        .emotion = emo >= 0 && emo < CW_EMO_COUNT ? (cw_emotion)emo : CW_EMO_CALM, .amount = amount, .to = -1, .shot = -1,
                        .color = c->color };
  l->seconds = line_seconds(st, key, text);
}
static void voice_start(cw_story *st, uint32_t key) {
  if (!st->d.speech) return;
  cw_speech_play(st->d.speech, key, NULL);
  cw_speech_set_playing(st->d.speech, key);
}
static void start_line(cw_story *st, const beat_t *b) {
  fill_line(st, &st->line, b->who, b->s, b->key, b->a, b->f);
  st->line.to = b->c; st->line.shot = b->b;
  st->line_on = true; st->line_t = 0; st->line_dur = st->line.seconds;
  st->line_key = b->key; st->lines++;
  if (st->aside_on) { st->aside_on = false; } /* a scene's line takes the voice */
  voice_start(st, b->key);
  if (b->b >= 0 && st->d.on_shot) {
    cw_story_shot s = { .type = (cw_shot_type)b->b, .subj = { b->who, b->c, -1 }, .nsubj = b->c >= 0 ? 2 : 1 };
    st->shots++;
    st->d.on_shot(&s, st->d.user);
  }
  if (st->d.on_line) st->d.on_line(&st->line, st->d.user);
}
static void start_aside(cw_story *st, int who, const char *text, uint32_t key, int emo, float amount, int prio) {
  fill_line(st, &st->aside, who, text, key, emo, amount);
  st->aside.aside = true;
  st->aside_on = true; st->aside_t = 0; st->aside_dur = st->aside.seconds; st->aside_prio = prio;
  st->asides++;
  voice_start(st, key);
  if (st->d.on_line) st->d.on_line(&st->aside, st->d.user);
}

static void scene_end(cw_story *st) {
  int s = st->scene;
  bool skipped = st->skipping;
  if (s >= 0) st->seen[s] = true;
  st->scene = -1; st->line_on = false; st->choosing = false; st->waiting = false; st->skipping = false; st->depth = 0; st->skip_hold = 0;
  st->line_key = 0;
  if (st->d.speech && !st->aside_on) cw_speech_set_playing(st->d.speech, 0);
  if (s >= 0 && st->d.on_scene_end) st->d.on_scene_end(st->scenes[s].name, skipped, st->d.user);
}

static void pick_option(cw_story *st, int i) {
  if (!st->choosing || i < 0 || i >= st->nopt || !st->opt_on[i]) return;
  st->choosing = false; st->choices++; st->last_choice = i;
  st->pc = st->opt_beat[i] + 1;
}
static int default_option(const cw_story *st) {
  if (st->opt_default >= 0 && st->opt_default < st->nopt && st->opt_on[st->opt_default]) return st->opt_default;
  for (int i = 0; i < st->nopt; i++) if (st->opt_on[i]) return i;
  return -1;
}

/* run beats until one blocks (a line, a choice, a wait) or the scene ends */
static void run(cw_story *st) {
  for (int steps = 0; steps < MAX_STEPS && st->scene >= 0; steps++) {
    if (st->line_on || st->waiting) return;
    if (st->choosing) {
      if (!st->skipping) return;
      int o = default_option(st);
      if (o < 0) { st->choosing = false; continue; }
      pick_option(st, o);
      continue;
    }
    if (st->pc < 0 || st->pc >= st->nbeat) { scene_end(st); return; }
    const beat_t *b = &st->beats[st->pc++];
    st->dbg_beat = st->pc - 1;
    switch (b->op) {
      case OP_SAY:
        if (st->skipping) { st->lines++; break; }
        start_line(st, b);
        return;
      case OP_ASIDE:
        if (!st->skipping) start_aside(st, b->who, b->s, b->key, b->a, b->f, 0);
        break;
      case OP_CHOICE: {
        st->nopt = 0; st->prompt = b->s; st->opt_default = b->b;
        for (int ob = b->a, k = 0; ob >= 0 && ob < st->nbeat && k < b->c && st->nopt < CW_STORY_MAX_OPTIONS; k++) {
          st->opt_beat[st->nopt] = ob; st->opt_on[st->nopt] = cond_true(st, st->beats[ob].cond); st->nopt++;
          ob = st->beats[ob].target;
        }
        st->choosing = true; st->choice_t = 0; st->choice_timeout = b->f;
        st->cursor = default_option(st); if (st->cursor < 0) st->cursor = 0;
        break; /* the top of the loop waits, or takes the default when skipping */
      }
      case OP_OPTION: break; /* reached only by a malformed script: an option's beats start after it */
      case OP_JUMP: st->pc = b->target; break;
      case OP_JUMP_NOT: if (!cond_true(st, b->cond)) st->pc = b->target; break;
      case OP_SET: if (b->a >= 0 && b->a < st->nflag) st->flags[b->a] = true; break;
      case OP_CLEAR: if (b->a >= 0 && b->a < st->nflag) st->flags[b->a] = false; break;
      case OP_ADD: if (b->a >= 0 && b->a < st->nstat) st->statv[b->a] += b->b; break;
      case OP_SETSTAT: if (b->a >= 0 && b->a < st->nstat) st->statv[b->a] = b->b; break;
      case OP_WAIT: if (!st->skipping && b->f > 0) { st->waiting = true; st->wait_t = b->f; return; } break;
      case OP_SHOT: {
        st->shots++;
        if (!st->d.on_shot) break;
        cw_story_shot s = { .type = (cw_shot_type)b->a, .subj = { b->b, b->c, b->target }, .dur = b->f };
        s.nsubj = b->b < 0 ? 0 : b->c < 0 ? 1 : b->target < 0 ? 2 : 3;
        float dist = 0, fov = 0, height = 0; int part = 0;
        if (b->s2 && *b->s2) {
          const char *p;
          if ((p = strstr(b->s2, "\"dist\":"))) dist = (float)atof(p + 7);
          if ((p = strstr(b->s2, "\"fov\":"))) fov = (float)atof(p + 6);
          if ((p = strstr(b->s2, "\"height\":"))) height = (float)atof(p + 9);
          if ((p = strstr(b->s2, "\"part\":"))) part = atoi(p + 7);
        }
        s.dist = dist; s.fov = fov; s.height = height; s.part = (cw_shot_part)part;
        st->d.on_shot(&s, st->d.user);
        break;
      }
      case OP_EMIT: st->emits++; if (st->d.on_emit) st->d.on_emit(b->s, b->b, st->d.user); break;
      case OP_GOTO:
        if (b->a >= 0 && b->a < st->nscene) { st->seen[st->scene] = true; st->scene = b->a; st->pc = (int)st->scenes[b->a].start; st->dbg_scene = b->a; }
        break;
      case OP_CALL:
        if (b->a >= 0 && b->a < st->nscene && st->depth < MAX_DEPTH) {
          st->stack_scene[st->depth] = st->scene; st->stack_pc[st->depth] = st->pc; st->depth++;
          st->scene = b->a; st->pc = (int)st->scenes[b->a].start; st->dbg_scene = b->a;
        }
        break;
      case OP_SAVE: st->resume_scene = st->depth ? st->stack_scene[0] : st->scene; st->saves++; break;
      case OP_END: case OP_RET:
        if (st->depth > 0) {
          st->seen[st->scene] = true;
          st->depth--; st->scene = st->stack_scene[st->depth]; st->pc = st->stack_pc[st->depth]; st->dbg_scene = st->scene;
        } else { scene_end(st); return; }
        break;
      default: break;
    }
  }
}

bool cw_story_play(cw_story *st, const char *scene) {
  if (!st) return false;
  int i = cw_story_scene_index(st, scene);
  if (i < 0) { cw_error("story: no scene \"%s\"", scene ? scene : "(null)"); return false; }
  if (st->scene >= 0) scene_end(st);
  st->scene = i; st->pc = (int)st->scenes[i].start; st->depth = 0; st->dbg_scene = i;
  st->skipping = false; st->skip_hold = 0;
  run(st);
  return true;
}
bool cw_story_active(const cw_story *st) { return st && st->scene >= 0; }
const char *cw_story_scene(const cw_story *st) { return st && st->scene >= 0 ? st->scenes[st->scene].name : NULL; }
int cw_story_scene_index(const cw_story *st, const char *scene) {
  if (!st || !scene) return -1;
  for (int i = 0; i < st->nscene; i++) if (!strcmp(st->scenes[i].name, scene)) return i;
  return -1;
}
bool cw_story_seen(const cw_story *st, const char *scene) { int i = cw_story_scene_index(st, scene); return i >= 0 && st->seen[i]; }
void cw_story_stop(cw_story *st) {
  if (!st || st->scene < 0) return;
  st->scene = -1; st->line_on = st->choosing = st->waiting = st->skipping = false; st->depth = 0; st->line_key = 0;
  if (st->d.speech) { cw_speech_stop(st->d.speech, 0.1f); cw_speech_set_playing(st->d.speech, 0); }
}

void cw_story_advance(cw_story *st) {
  if (!st || st->scene < 0) return;
  if (st->choosing) { pick_option(st, st->cursor); run(st); return; }
  if (st->line_on && st->line_t > 0.15f) {
    st->line_on = false; st->skips += st->line_t < st->line_dur ? 1 : 0;
    if (st->d.speech && st->line_t < st->line_dur) cw_speech_stop(st->d.speech, 0.08f);
    run(st);
  }
}
void cw_story_choose(cw_story *st, int option) { if (st && st->choosing) { st->cursor = option; pick_option(st, option); run(st); } }
void cw_story_skip_scene(cw_story *st) {
  if (!st || st->scene < 0) return;
  st->scene_skips++;
  st->skipping = true; st->line_on = false; st->waiting = false;
  if (st->d.speech) { cw_speech_stop(st->d.speech, 0.1f); cw_speech_set_playing(st->d.speech, 0); }
  run(st);
}

static void read_input(cw_story *st) {
  st->adv_pressed = st->up_pressed = st->down_pressed = st->skip_held = false;
  int p0 = st->d.any_player ? 0 : st->d.player, p1 = st->d.any_player ? CW_INPUT_MAX_PLAYERS - 1 : st->d.player;
  for (int p = p0; p <= p1; p++) {
    if (st->a_adv >= 0 && cw_action_just_pressed(p, st->a_adv)) st->adv_pressed = true;
    if (st->a_up >= 0 && cw_action_just_pressed(p, st->a_up)) st->up_pressed = true;
    if (st->a_down >= 0 && cw_action_just_pressed(p, st->a_down)) st->down_pressed = true;
    if (st->a_skip >= 0 && cw_action_pressed(p, st->a_skip)) st->skip_held = true;
  }
}

static void hash_state(cw_story *st) {
  uint32_t h = 2166136261u;
  for (int i = 0; i < st->nflag; i++) { h ^= st->flags[i] ? 1u : 0u; h *= 16777619u; }
  for (int i = 0; i < st->nstat; i++) { h ^= (uint32_t)st->statv[i]; h *= 16777619u; }
  st->flags_hash_v = h;
}

void cw_story_tick(cw_story *st, float dt) {
  if (!st) return;
  st->now += dt;
  for (int i = 0; i < st->npool; i++) if (st->pool_cool[i] > 0) st->pool_cool[i] -= dt;
  if (st->aside_on) {
    st->aside_t += dt;
    if (st->aside_t >= st->aside_dur + st->d.line_gap) { st->aside_on = false; if (st->d.speech && !st->line_on) cw_speech_set_playing(st->d.speech, 0); }
  }
  read_input(st);
  if (st->scene >= 0) {
    if (st->skip_held && !st->choosing) {
      st->skip_hold += dt;
      if (st->skip_hold >= CW_STORY_SKIP_HOLD) { cw_story_skip_scene(st); goto done; }
    } else st->skip_hold = 0;
    if (st->line_on) {
      st->line_t += dt;
      if (st->adv_pressed) cw_story_advance(st);
      else if (!st->d.wait_for_advance && st->line_t >= st->line_dur + st->d.line_gap) { st->line_on = false; run(st); }
    } else if (st->choosing) {
      st->choice_t += dt;
      int dir = st->down_pressed ? 1 : st->up_pressed ? -1 : 0;
      if (dir) for (int k = 1; k <= st->nopt; k++) { int c = ((st->cursor + dir * k) % st->nopt + st->nopt) % st->nopt; if (st->opt_on[c]) { st->cursor = c; break; } }
      if (st->adv_pressed) { pick_option(st, st->cursor); run(st); }
      else if (st->choice_timeout > 0 && st->choice_t >= st->choice_timeout) { int o = default_option(st); if (o >= 0) { st->cursor = o; pick_option(st, o); } else st->choosing = false; run(st); }
    } else if (st->waiting) {
      st->wait_t -= dt;
      if (st->wait_t <= 0 || st->adv_pressed) { st->waiting = false; run(st); }
    } else run(st);
  }
done:
  st->dbg_cursor = st->cursor; st->dbg_waiting = st->choosing ? 1 : 0;
  if (st->scene < 0) st->dbg_scene = -1;
  if (!st->line_on) st->line_key = 0;
  hash_state(st);
}

const cw_story_line *cw_story_current(const cw_story *st) { return st && st->line_on ? &st->line : NULL; }
const cw_story_line *cw_story_aside(const cw_story *st) { return st && st->aside_on ? &st->aside : NULL; }
float cw_story_line_time(const cw_story *st, bool aside) { return !st ? 0 : aside ? st->aside_t : st->line_t; }
bool cw_story_choosing(const cw_story *st) { return st && st->choosing; }
int cw_story_options(const cw_story *st, const char **text, bool *enabled, int cap) {
  if (!st || !st->choosing) return 0;
  for (int i = 0; i < st->nopt && i < cap; i++) { if (text) text[i] = st->beats[st->opt_beat[i]].s; if (enabled) enabled[i] = st->opt_on[i]; }
  return st->nopt;
}
int cw_story_cursor(const cw_story *st) { return st ? st->cursor : 0; }
const char *cw_story_prompt(const cw_story *st) { return st && st->choosing ? st->prompt : NULL; }

/* ---- pools ---- */
static int pool_find(const cw_story *st, const char *name) {
  if (!st || !name) return -1;
  for (int i = 0; i < st->npool; i++) if (!strcmp(st->pools[i].name, name)) return i;
  return -1;
}
bool cw_story_pool_ready(const cw_story *st, const char *pool) { int p = pool_find(st, pool); return p >= 0 && st->pool_cool[p] <= 0; }
const cw_story_line *cw_story_bark(cw_story *st, const char *pool) {
  int p = pool_find(st, pool);
  if (p < 0) { if (st && !st->warned_unknown) { cw_warn("story: no pool \"%s\"", pool ? pool : "(null)"); st->warned_unknown = true; } return NULL; }
  const pool_t *P = &st->pools[p];
  if (st->pool_cool[p] > 0) return NULL;
  if (st->line_on || st->skipping) { st->asides_dropped++; return NULL; }
  if (st->aside_on && P->priority <= st->aside_prio) { st->asides_dropped++; return NULL; }
  int best = -1; double best_at = 1e300;
  bool no_repeat = !cw_test_env("CW_STORY_ALLOW_REPEAT");
  int eligible = 0;
  for (uint32_t k = 0; k < P->count; k++) {
    int i = (int)(P->start + k);
    if (P->once && st->said_once[i]) continue;
    if (!cond_true(st, st->plines[i].cond)) continue;
    eligible++;
  }
  for (uint32_t k = 0; k < P->count; k++) {
    int i = (int)(P->start + k);
    if (P->once && st->said_once[i]) continue;
    if (!cond_true(st, st->plines[i].cond)) continue;
    if (no_repeat && eligible > 1 && i == st->pool_last[p]) continue;
    double at = no_repeat ? st->said_at[i] : -1e9; /* the control: the first eligible line every time */
    if (at < best_at) { best_at = at; best = i; }
  }
  if (best < 0) return NULL;
  if (best == st->pool_last[p] && P->count > 1) st->repeats++;
  const pline_t *l = &st->plines[best];
  st->pool_last[p] = best; st->said_at[best] = st->now; if (P->once) st->said_once[best] = true;
  st->pool_cool[p] = P->cooldown;
  start_aside(st, l->who, l->text, l->key, l->emo, l->amount, P->priority);
  return &st->aside;
}

/* ---- flags, stats, cast ---- */
static int find_name(const char *const *names, int n, size_t stride, const char *name) {
  for (int i = 0; i < n; i++) { const char *s = *(const char *const *)((const char *)names + (size_t)i * stride); if (!strcmp(s, name)) return i; }
  return -1;
}
static int flag_ix(const cw_story *st, const char *f) { return st && f ? find_name(st->flag_names, st->nflag, sizeof(char *), f) : -1; }
static int stat_ix(const cw_story *st, const char *s) { return st && s ? find_name(&st->stats[0].name, st->nstat, sizeof(stat_t), s) : -1; }
bool cw_story_flag(const cw_story *st, const char *flag) { int i = flag_ix(st, flag); return i >= 0 && st->flags[i]; }
void cw_story_set_flag(cw_story *st, const char *flag, bool on) {
  int i = flag_ix(st, flag); if (i < 0) { cw_warn("story: no flag \"%s\"", flag ? flag : "(null)"); return; }
  st->flags[i] = on; hash_state(st);
}
int cw_story_stat(const cw_story *st, const char *stat) { int i = stat_ix(st, stat); return i >= 0 ? st->statv[i] : 0; }
void cw_story_set_stat(cw_story *st, const char *stat, int v) { int i = stat_ix(st, stat); if (i < 0) { cw_warn("story: no stat \"%s\"", stat ? stat : "(null)"); return; } st->statv[i] = v; hash_state(st); }
void cw_story_add_stat(cw_story *st, const char *stat, int d) { int i = stat_ix(st, stat); if (i < 0) { cw_warn("story: no stat \"%s\"", stat ? stat : "(null)"); return; } st->statv[i] += d; hash_state(st); }
int cw_story_cast_count(const cw_story *st) { return st ? st->ncast : 0; }
int cw_story_cast_find(const cw_story *st, const char *id) { if (!st || !id) return -1; for (int i = 0; i < st->ncast; i++) if (!strcmp(st->cast[i].id, id)) return i; return -1; }
const char *cw_story_cast_id(const cw_story *st, int who) { return st && who >= 0 && who < st->ncast ? st->cast[who].id : NULL; }
const char *cw_story_cast_name(const cw_story *st, int who) { return st && who >= 0 && who < st->ncast ? st->cast[who].name : NULL; }

/* ---- saves: "CWSS", u32 version 1, u32 count, then (u32 kind, u32 hash, i32 value) ---- */
enum { SV_FLAG = 1, SV_STAT, SV_ONCE, SV_SEEN, SV_RESUME };
size_t cw_story_save(const cw_story *st, void *buf, size_t cap) {
  if (!st) return 0;
  int n = 0;
  for (int i = 0; i < st->nflag; i++) n += st->flags[i];
  n += st->nstat;
  for (int i = 0; i < st->npline; i++) n += st->said_once[i];
  for (int i = 0; i < st->nscene; i++) n += st->seen[i];
  n += st->resume_scene >= 0;
  size_t need = 12 + (size_t)n * 12;
  if (!buf || cap < need) return need;
  uint8_t *o = buf; memcpy(o, "CWSS", 4);
  uint32_t v = 1, cnt = (uint32_t)n; memcpy(o + 4, &v, 4); memcpy(o + 8, &cnt, 4); o += 12;
#define PUT(k, h, val) do { uint32_t k_ = (k), h_ = (h); int32_t v_ = (val); memcpy(o, &k_, 4); memcpy(o + 4, &h_, 4); memcpy(o + 8, &v_, 4); o += 12; } while (0)
  for (int i = 0; i < st->nflag; i++) if (st->flags[i]) PUT(SV_FLAG, st->flag_hash[i], 1);
  for (int i = 0; i < st->nstat; i++) PUT(SV_STAT, st->stat_hash[i], st->statv[i]);
  for (int i = 0; i < st->npline; i++) if (st->said_once[i]) PUT(SV_ONCE, st->plines[i].key, 1);
  for (int i = 0; i < st->nscene; i++) if (st->seen[i]) PUT(SV_SEEN, st->scene_hash[i], 1);
  if (st->resume_scene >= 0) PUT(SV_RESUME, st->scene_hash[st->resume_scene], 1);
#undef PUT
  return need;
}
bool cw_story_load(cw_story *st, const void *buf, size_t size) {
  if (!st || !buf || size < 12 || memcmp(buf, "CWSS", 4)) return false;
  const uint8_t *p = buf; uint32_t ver, cnt; memcpy(&ver, p + 4, 4); memcpy(&cnt, p + 8, 4);
  if (ver != 1 || 12 + (size_t)cnt * 12 > size) return false;
  cw_story_reset(st);
  p += 12;
  for (uint32_t e = 0; e < cnt; e++, p += 12) {
    uint32_t k, h; int32_t v; memcpy(&k, p, 4); memcpy(&h, p + 4, 4); memcpy(&v, p + 8, 4);
    switch (k) {
      case SV_FLAG: for (int i = 0; i < st->nflag; i++) if (st->flag_hash[i] == h) st->flags[i] = v != 0; break;
      case SV_STAT: for (int i = 0; i < st->nstat; i++) if (st->stat_hash[i] == h) st->statv[i] = v; break;
      case SV_ONCE: for (int i = 0; i < st->npline; i++) if (st->plines[i].key == h) st->said_once[i] = true; break;
      case SV_SEEN: for (int i = 0; i < st->nscene; i++) if (st->scene_hash[i] == h) st->seen[i] = true; break;
      case SV_RESUME: for (int i = 0; i < st->nscene; i++) if (st->scene_hash[i] == h) st->resume_scene = i; break;
      default: break;
    }
  }
  hash_state(st);
  return true;
}
const char *cw_story_resume_scene(const cw_story *st) { return st && st->resume_scene >= 0 ? st->scenes[st->resume_scene].name : NULL; }

bool cw_story_speaking(const cw_story *st, int *who, uint32_t *key, float *t, cw_emotion *emo, float *amount) {
  const cw_story_line *l = st && st->line_on ? &st->line : st && st->aside_on ? &st->aside : NULL;
  if (!l) return false;
  float lt = l == &st->line ? st->line_t : st->aside_t;
  if (lt > l->seconds) return false;
  if (who) *who = l->who;
  if (key) *key = l->key;
  if (t) *t = lt;
  if (emo) *emo = l->emotion;
  if (amount) *amount = l->amount;
  return true;
}

void cw_story_set_subtitles(cw_story *st, bool on) { if (st) st->subtitles = on; }
bool cw_story_subtitles(const cw_story *st) { return st && st->subtitles; }

/* ---- drawing ---- */
static cw_color col_hex(uint32_t c, float a) { return cw_rgba(((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f, a); }
static float ease_in(float t, float len) { float k = t / len; return k < 0 ? 0 : k > 1 ? 1 : k * k * (3 - 2 * k); }

static void draw_line(cw_font font, const cw_story_line *l, float t, cw_rect area, float y_frac, float size, bool top) {
  float a = ease_in(t, 0.18f);
  float w = area.w * 0.72f, pad = size * 0.6f;
  cw_text_style ts = { .size = size, .color = cw_rgba(1, 1, 1, a), .wrap_width = w - pad * 2, .align = CW_ALIGN_CENTER, .own_backdrop = true };
  cw_text_style ns = { .size = size, .color = col_hex(l->color, a), .own_backdrop = true };
  cw_text_metrics tm = cw_font_measure(font, l->text, &ts), nm = cw_font_measure(font, l->speaker, &ns);
  float h = tm.height + nm.height + pad * 2.2f;
  float x = area.x + (area.w - w) * 0.5f, y = top ? area.y + area.h * y_frac : area.y + area.h * y_frac - h;
  cw_r2d_rect((cw_rect){ x, y, w, h }, cw_rgba(0.02f, 0.02f, 0.04f, 0.72f * a));
  cw_r2d_text(font, l->speaker, cw_v2(x + (w - nm.width) * 0.5f, y + pad * 0.6f), &ns);
  cw_r2d_text(font, l->text, cw_v2(x + pad, y + pad * 0.6f + nm.height + pad * 0.4f), &ts);
}

void cw_story_draw(cw_story *st, cw_font font, cw_rect area) {
  if (!st || !font.id) return;
  if (area.w <= 0 || area.h <= 0) { cw_vec2 cs = cw_r2d_canvas_size(); area = (cw_rect){ 0, 0, cs.x, cs.y }; } /* the whole canvas */
  float size = area.h * 0.026f; /* body text: at least 2.4% of the canvas (the player's Text size scales it) */
  bool subs = st->subtitles && (!st->d.speech || cw_speech_subtitles_on());
  if (st->line_on && subs) draw_line(font, &st->line, st->line_t, area, 0.95f, size, false);
  if (st->aside_on && subs && !st->line_on) draw_line(font, &st->aside, st->aside_t, area, 0.06f, size, true);
  if (st->choosing) {
    float rowh = size * 1.7f, w = area.w * 0.5f, x = area.x + (area.w - w) * 0.5f;
    float h = rowh * (float)(st->nopt + 1) + size;
    float y = area.y + area.h * 0.95f - h;
    cw_r2d_rect((cw_rect){ x, y, w, h }, cw_rgba(0.02f, 0.02f, 0.04f, 0.78f));
    cw_text_style ps = { .size = size, .color = cw_rgba(1, 0.86f, 0.55f, 1), .own_backdrop = true };
    cw_text_metrics pm = cw_font_measure(font, st->prompt ? st->prompt : "", &ps);
    cw_r2d_text(font, st->prompt ? st->prompt : "", cw_v2(x + (w - pm.width) * 0.5f, y + size * 0.4f), &ps);
    for (int i = 0; i < st->nopt; i++) {
      float ry = y + rowh * (float)(i + 1) + size * 0.3f;
      if (i == st->cursor) cw_r2d_rect((cw_rect){ x + size * 0.4f, ry - size * 0.2f, w - size * 0.8f, rowh - size * 0.1f }, cw_rgba(1, 0.78f, 0.35f, 0.35f));
      cw_text_style os = { .size = size, .color = st->opt_on[i] ? cw_rgba(1, 1, 1, 1) : cw_rgba(0.6f, 0.6f, 0.62f, 1), .own_backdrop = true };
      cw_r2d_text(font, st->beats[st->opt_beat[i]].s, cw_v2(x + size * 1.1f, ry), &os);
    }
  }
  if (st->scene >= 0 && !st->choosing) {
    /* the skip hint, and how far a held skip has got */
    const char *skip = st->d.skip_action ? st->d.skip_action : "ui_back";
    char hint[96]; snprintf(hint, sizeof hint, "{%s}: hold to skip", skip);
    float hs = area.h * 0.022f; if (hs < 16) hs = 16;
    cw_vec2 sz = cw_prompt_text_size(font, hint, st->d.player, hs);
    cw_vec2 pos = cw_v2(area.x + area.w - sz.x - area.h * 0.03f, area.y + area.h * 0.03f);
    cw_prompt_text(font, hint, st->d.player, pos, hs, cw_rgba(1, 1, 1, 0.75f), CW_ALIGN_LEFT);
    if (st->skip_hold > 0) cw_r2d_rect((cw_rect){ pos.x, pos.y + sz.y + 4, sz.x * (st->skip_hold / CW_STORY_SKIP_HOLD), 4 }, cw_rgba(1, 0.85f, 0.5f, 0.9f));
  }
}

/* ---- debug ---- */
static const char *dname(cw_story *st, const char *prefix, const char *leaf) {
  size_t n = strlen(prefix) + strlen(leaf) + 2;
  char *s = malloc(n); snprintf(s, n, "%s.%s", prefix, leaf);
  st->dbg_names = realloc(st->dbg_names, sizeof(char *) * (size_t)(st->ndbg_names + 1)); st->dbg_names[st->ndbg_names++] = s;
  return s;
}
void cw_story_publish(cw_story *st, const char *prefix) {
  if (!st) return;
  if (!prefix) prefix = "story";
  cw_debug_i32(dname(st, prefix, "scene"), &st->dbg_scene);
  cw_debug_i32(dname(st, prefix, "beat"), &st->dbg_beat);
  cw_debug_u32(dname(st, prefix, "line_key"), &st->line_key);
  cw_debug_i32(dname(st, prefix, "lines"), &st->lines);
  cw_debug_i32(dname(st, prefix, "asides"), &st->asides);
  cw_debug_i32(dname(st, prefix, "asides_dropped"), &st->asides_dropped);
  cw_debug_i32(dname(st, prefix, "choices"), &st->choices);
  cw_debug_i32(dname(st, prefix, "choice"), &st->last_choice);
  cw_debug_i32(dname(st, prefix, "choice_cursor"), &st->dbg_cursor);
  cw_debug_i32(dname(st, prefix, "skips"), &st->skips);
  cw_debug_i32(dname(st, prefix, "scene_skips"), &st->scene_skips);
  cw_debug_i32(dname(st, prefix, "repeats"), &st->repeats);
  cw_debug_i32(dname(st, prefix, "waiting"), &st->dbg_waiting);
  cw_debug_i32(dname(st, prefix, "emits"), &st->emits);
  cw_debug_i32(dname(st, prefix, "shots"), &st->shots);
  cw_debug_i32(dname(st, prefix, "saves"), &st->saves);
  cw_debug_u32(dname(st, prefix, "flags_hash"), &st->flags_hash_v);
}
