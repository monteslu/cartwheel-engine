/* Explosion3D (explosions.h): one trigger for a whole explosion.
 *
 * The node is a conductor. Its children are the explosion's parts, authored
 * like any scene (GPUParticles3D fire, smoke, sparks and debris with
 * emitting off and one_shot on, a Shockwave3D with autostart off); firing it
 *   - restarts every GPUParticles3D and Shockwave3D under it,
 *   - borrows a light from a small shared pool for a flash that dies away
 *     quickly (the pool is bounded: the effects preset sets its size, a
 *     busy scene takes the oldest flash back, so a chain of blasts or a
 *     four-way split screen never grows the light count),
 *   - stamps a scorch mark on the ground under it: a dark layer that fades
 *     out and a glowing layer that cools from orange to nothing (two pooled
 *     Decal3D nodes, the oldest recycled),
 *   - records a shake event for cw_explosion_shake.
 * Everything runs on the fixed step and the tree's tick count: the same
 * ticks give the same light, scorch and shake on every host. The pool's
 * lights and decals are ordinary nodes under the tree's root, made once the
 * first Explosion3D enters; they are render state, not simulation. */
#include <math.h>
#include <string.h>

#include "cartwheel/explosions.h"
#include "cartwheel/math.h"
#include "cartwheel/particles.h"
#include "cartwheel/quality.h"
#include "fx_internal.h"

enum { MAX_FLASH = 8, MAX_SCORCH = 12, MAX_SHAKE = 16 };

typedef struct explo {
  bool autostart, loop;
  float duration;
  float flash_energy, flash_color[4], flash_range, flash_duration;
  const char *scorch_texture;
  float scorch_size, scorch_time, scorch_life, scorch_glow, scorch_depth, scorch_offset;
  float shake_amount, shake_radius, shake_time;
  bool playing, started;
  float age;
  /* runtime */
  bool pending;
  int flash_slot, scorch_slot;
  uint64_t flash_stamp, scorch_stamp; /* the pool slot's fire stamp this node holds */
  t3_vec3 fire_pos;
} explo;
static const cw_node_type *explo_t;

typedef struct flash_slot { cw_node light; uint64_t stamp; bool busy; float age; float peak; } flash_slot;
typedef struct scorch_slot {
  cw_node dark, glow;
  uint64_t stamp;
  bool busy;
  float age, time, life, glow_energy, dark_alpha;
} scorch_slot;
typedef struct shake_ev { t3_vec3 pos; float amount, radius, duration; double t0; bool on; } shake_ev;
typedef struct pool {
  flash_slot fl[MAX_FLASH];
  int nfl;
  scorch_slot sc[MAX_SCORCH];
  int nsc;
  shake_ev sh[MAX_SHAKE];
  int next_shake;
  uint64_t stamp;
  double time; /* seconds of fixed steps the hook has seen */
} pool;

static void pool_free(void *p) { cw_free(p); }
static pool *pool_of(cw_tree *t) {
  pool *p = cw_tree_slot(t, "fx_explosions");
  if (!p) {
    p = cw_calloc(1, sizeof *p, CW_MEM_RENDER);
    cw_tree_set_slot(t, "fx_explosions", p, pool_free);
  }
  return p;
}

static int pool_lights_wanted(void) {
  float s = cw_quality_scale("flash_lights");
  int n = (int)((float)MAX_FLASH * s + 0.5f);
  return CW_CLAMP(n, 1, MAX_FLASH);
}

/* ── firing ───────────────────────────────────────────────────────────── */

static void fire_children(cw_node n) {
  static const cw_node_type *pt, *st;
  if (!pt) {
    pt = cw_node_type_find("GPUParticles3D");
    st = cw_node_type_find("Shockwave3D");
  }
  for (int i = 0; i < cw_node_child_count(n); i++) {
    cw_node c = cw_node_child(n, i);
    if (cw_node_data_t(c, pt)) cw_particles_restart(c);
    else if (cw_node_data_t(c, st)) cw_shockwave_fire(c);
    fire_children(c);
  }
}

void cw_explosion_fire(cw_node n) {
  explo *e = cw_node_data_t(n, explo_t);
  if (!e) return;
  e->started = true;
  e->playing = true;
  e->age = 0;
  e->pending = true;
  e->fire_pos = cw_node3d_global_position(n);
  fire_children(n);
}

bool cw_explosion_playing(cw_node n) {
  explo *e = cw_node_data_t(n, explo_t);
  return e && e->playing;
}

/* the oldest busy slot, or a free one */
static int take_flash(pool *p, int active) {
  int best = -1;
  for (int i = 0; i < p->nfl && i < active; i++) {
    if (!p->fl[i].busy) return i;
    if (best < 0 || p->fl[i].age > p->fl[best].age) best = i;
  }
  return best;
}
static int take_scorch(pool *p) {
  int best = -1;
  for (int i = 0; i < p->nsc; i++) {
    if (!p->sc[i].busy) return i;
    if (best < 0 || p->sc[i].age > p->sc[best].age) best = i;
  }
  return best;
}

static void set_f(cw_node n, const char *prop, float v) { cw_node_set(n, prop, cw_val_float(v)); }

static void make_pool_nodes(cw_tree *t, pool *p, bool want_scorch) {
  cw_node root = cw_tree_root(t);
  int want = pool_lights_wanted();
  while (p->nfl < want && p->nfl < MAX_FLASH) {
    cw_node l = cw_node_new("OmniLight3D", "ExplosionFlash");
    cw_node_add_child(root, l);
    set_f(l, "energy", 0);
    cw_node_set(l, "color", cw_val_color(1, 0.6f, 0.25f, 1));
    p->fl[p->nfl++].light = l;
  }
  while (want_scorch && p->nsc < MAX_SCORCH) {
    scorch_slot *s = &p->sc[p->nsc++];
    for (int k = 0; k < 2; k++) {
      cw_node d = cw_node_new("Decal3D", k ? "ScorchGlow" : "ScorchDark");
      cw_node_add_child(root, d);
      cw_node_set(d, "blend", cw_val_enum(k ? "ADD" : "MULTIPLY"));
      cw_node_set(d, "color", cw_val_color(k ? 1 : 0.05f, k ? 0.45f : 0.04f, k ? 0.1f : 0.035f, 0));
      cw_node_set(d, "normal_fade", cw_val_float(0.4));
      cw_node3d_set_rotation(d, (t3_vec3){ -CW_PI * 0.5f, 0, 0 });
      cw_node_set(d, "priority", cw_val_int(k));
      if (k) s->glow = d;
      else s->dark = d;
    }
  }
}

static float smoothstep01(float x) {
  x = CW_CLAMP(x, 0.0f, 1.0f);
  return x * x * (3 - 2 * x);
}

static void drive_flash(pool *p, explo *e, float dt) {
  (void)dt;
  if (e->flash_slot < 0) return;
  flash_slot *f = &p->fl[e->flash_slot];
  if (f->stamp != e->flash_stamp || !f->busy) {
    e->flash_slot = -1; /* a later explosion took it */
    return;
  }
  f->age = e->age;
  float u = e->flash_duration > 0 ? e->age / e->flash_duration : 1;
  if (u >= 1) {
    set_f(f->light, "energy", 0);
    f->busy = false;
    e->flash_slot = -1;
    return;
  }
  float fall = (1 - u) * (1 - u);
  float flicker = 0.9f + 0.1f * cw_sin(e->age * 70.0f);
  set_f(f->light, "energy", f->peak * fall * flicker);
}

static void drive_scorch(pool *p, explo *e) {
  if (e->scorch_slot < 0) return;
  scorch_slot *s = &p->sc[e->scorch_slot];
  if (s->stamp != e->scorch_stamp || !s->busy) {
    e->scorch_slot = -1;
    return;
  }
  s->age = e->age;
  float cool = s->time > 0 ? CW_CLAMP(e->age / s->time, 0.0f, 1.0f) : 1.0f;
  float fade_from = s->life * 0.7f;
  float fade = e->age <= fade_from ? 0.0f : smoothstep01((e->age - fade_from) / CW_MAX(s->life - fade_from, 1e-3f));
  float glow = s->glow_energy * (1 - cool) * (1 - cool) * (1 - fade);
  float dark = s->dark_alpha * smoothstep01(e->age / 0.15f) * (1 - fade);
  cw_node_set(s->dark, "color", cw_val_color(0.05f, 0.04f, 0.035f, dark));
  /* the glow's colour cools from white-orange to red as its energy falls */
  cw_node_set(s->glow, "color", cw_val_color(1, 0.35f + 0.35f * (1 - cool), 0.08f + 0.2f * (1 - cool), glow > 0.002f ? 1 : 0));
  set_f(s->glow, "energy", glow);
  if (e->age >= s->life) {
    cw_node_set(s->dark, "color", cw_val_color(0.05f, 0.04f, 0.035f, 0));
    cw_node_set(s->glow, "color", cw_val_color(1, 0.35f, 0.08f, 0));
    s->busy = false;
    e->scorch_slot = -1;
  }
}

static void explosions_hook(cw_tree *t, double dt, void *user) {
  (void)user;
  cw__fx_tree *ft = cw__fx_tree_of(t);
  pool *p = pool_of(t);
  bool want_scorch = false;
  p->time += dt;
  for (int i = 0; i < ft->explosions.count; i++) {
    explo *e = cw_node_data_t(ft->explosions.data[i], explo_t);
    if (e && e->scorch_size > 0) want_scorch = true;
  }
  if (ft->explosions.count) make_pool_nodes(t, p, want_scorch);
  int active = pool_lights_wanted();
  for (int i = 0; i < ft->explosions.count; i++) {
    cw_node n = ft->explosions.data[i];
    explo *e = cw_node_data_t(n, explo_t);
    if (!e) continue;
    if (e->autostart && !e->started) cw_explosion_fire(n);
    if (e->playing) {
      if (e->pending) { /* just fired: take a light, a scorch and shake slot */
        e->pending = false;
        if (e->flash_energy > 0 && p->nfl > 0) {
          int s = take_flash(p, active);
          if (s >= 0) {
            flash_slot *f = &p->fl[s];
            f->busy = true;
            f->stamp = ++p->stamp;
            f->peak = e->flash_energy;
            f->age = 0;
            e->flash_slot = s;
            e->flash_stamp = f->stamp;
            cw_node3d_set_position(f->light, (t3_vec3){ e->fire_pos.x, e->fire_pos.y + 0.6f, e->fire_pos.z });
            cw_node_set(f->light, "color", cw_val_color(e->flash_color[0], e->flash_color[1], e->flash_color[2], 1));
            set_f(f->light, "range", e->flash_range);
          }
        }
        if (e->scorch_size > 0 && p->nsc > 0) {
          int s = take_scorch(p);
          if (s >= 0) {
            scorch_slot *c = &p->sc[s];
            c->busy = true;
            c->stamp = ++p->stamp;
            c->age = 0;
            c->time = e->scorch_time;
            c->life = CW_MAX(e->scorch_life, e->scorch_time);
            c->glow_energy = e->scorch_glow;
            c->dark_alpha = 0.9f;
            e->scorch_slot = s;
            e->scorch_stamp = c->stamp;
            t3_vec3 pos = { e->fire_pos.x, e->fire_pos.y + e->scorch_offset, e->fire_pos.z }; /* the box straddles the ground */
            /* a different mark each time: one of the sheet's four shapes, turned about the vertical, stretched a little */
            uint32_t h = cw_particle_hash((uint32_t)c->stamp * 2654435761u + (uint32_t)n.id);
            int variant = (int)(h & 3u);
            float turn = (float)(cw_particle_hash(h + 1u) >> 8) / 16777216.0f * 2.0f * CW_PI;
            float sx = 0.85f + 0.35f * ((float)(cw_particle_hash(h + 2u) >> 8) / 16777216.0f);
            float sy = 0.85f + 0.35f * ((float)(cw_particle_hash(h + 3u) >> 8) / 16777216.0f);
            cw_value region = { CW_T_VEC4 };
            region.v[0] = e->scorch_texture && *e->scorch_texture ? (float)(variant % 2) * 256.0f : 0;
            region.v[1] = e->scorch_texture && *e->scorch_texture ? (float)(variant / 2) * 256.0f : 0;
            region.v[2] = region.v[3] = e->scorch_texture && *e->scorch_texture ? 256.0f : 0;
            cw_node ds[2] = { c->dark, c->glow };
            for (int k = 0; k < 2; k++) {
              cw_node3d_set_position(ds[k], pos);
              cw_node3d_set_rotation(ds[k], (t3_vec3){ -CW_PI * 0.5f, 0, turn });
              cw_node_set(ds[k], "size", cw_val_vec3(e->scorch_size * sx * (k ? 0.8f : 1.0f), e->scorch_size * sy * (k ? 0.8f : 1.0f), e->scorch_depth));
              cw_node_set(ds[k], "texture", cw_val_string(e->scorch_texture ? e->scorch_texture : ""));
              cw_node_set(ds[k], "region", region);
            }
          }
        }
        if (e->shake_amount > 0) {
          shake_ev *s = &p->sh[p->next_shake++ % MAX_SHAKE];
          s->pos = e->fire_pos;
          s->amount = e->shake_amount;
          s->radius = e->shake_radius;
          s->duration = e->shake_time;
          s->t0 = p->time;
          s->on = true;
        }
      } else {
        e->age += (float)dt;
      }
      drive_flash(p, e, (float)dt);
      drive_scorch(p, e);
      if (e->age >= e->duration) {
        if (e->loop) cw_explosion_fire(n);
        else e->playing = false;
      }
    } else {
      /* the blast is over but its scorch mark goes on cooling and fading: the age keeps counting */
      if (e->started) e->age += (float)dt;
      drive_flash(p, e, (float)dt);
      drive_scorch(p, e);
    }
  }
}

/* ── node ─────────────────────────────────────────────────────────────── */

static void explo_init(cw_node n, void *data) {
  (void)n;
  explo *e = data;
  e->autostart = true;
  e->duration = 3;
  e->flash_energy = 120;
  e->flash_color[0] = 1;
  e->flash_color[1] = 0.62f;
  e->flash_color[2] = 0.28f;
  e->flash_color[3] = 1;
  e->flash_range = 14;
  e->flash_duration = 0.45f;
  e->scorch_texture = cw_intern("scorch.png");
  e->scorch_size = 4;
  e->scorch_time = 7;
  e->scorch_life = 40;
  e->scorch_glow = 3;
  e->scorch_depth = 2;
  e->shake_amount = 1;
  e->shake_radius = 25;
  e->shake_time = 0.9f;
  e->flash_slot = e->scorch_slot = -1;
}

static void explo_enter(cw_node n, void *data) {
  (void)data;
  cw_tree *t = cw_node_tree(n);
  cw__fx_tree *ft = cw__fx_tree_of(t);
  cw_array_push(&ft->explosions, n);
  if (!ft->explosion_hook) {
    ft->explosion_hook = true;
    cw_tree_add_hook(t, 0, -20, explosions_hook, NULL);
  }
}

static void explo_exit(cw_node n, void *data) {
  (void)data;
  cw_tree *t = cw_node_tree(n);
  if (t) cw__fx_remove_node(&cw__fx_tree_of(t)->explosions, n);
}

#define EF(field, ty, doc) CW_PROP_FIELD(explo, field, ty, CW_PROP_DEFAULT, doc)
#define ER(field, lo, hi, doc) CW_PROP_FIELD_RANGE(explo, field, CW_T_FLOAT, lo, hi, doc)
static const cw_prop explo_props[] = {
  EF(autostart, CW_T_BOOL, "Fire once when the node enters the tree."),
  EF(loop, CW_T_BOOL, "Fire again every `duration` seconds."),
  ER(duration, 0.05, 3600, "Seconds the explosion plays (the light, the particles' one shot and the shake all fit inside it); loop fires again then."),
  ER(flash_energy, 0, 100000, "Peak light of the flash (0 = no flash light)."),
  CW_PROP_FIELD(explo, flash_color, CW_T_COLOR, CW_PROP_DEFAULT, "Flash colour (linear)."),
  ER(flash_range, 0, 1e4, "Reach of the flash light, world units."),
  ER(flash_duration, 0.01, 60, "Seconds the flash takes to die away (fast at first)."),
  EF(scorch_texture, CW_T_STRING, "Image of the scorch marks: a 2 x 2 sheet of four 256 pixel shapes (tools/fx/bake-flipbooks.mjs writes scorch.png). Each mark picks one, turns it and stretches it. Empty: a plain round mark."),
  ER(scorch_size, 0, 1e3, "Width of the scorch mark on the ground under the node (0 = no mark). The node stands at ground level."),
  ER(scorch_time, 0.1, 3600, "Seconds the mark takes to cool from glowing to dark."),
  ER(scorch_life, 0.1, 36000, "Seconds the mark stays before it fades away (at least scorch_time)."),
  ER(scorch_glow, 0, 100, "Peak glow of the mark (HDR energy)."),
  ER(scorch_depth, 0.1, 100, "Height of the mark's box, centred on the node's height: geometry within half of it above or below lands the mark."),
  CW_PROP_FIELD(explo, scorch_offset, CW_T_FLOAT, CW_PROP_DEFAULT, "Move the mark's box up (+) or down (-) from the node's height."),
  ER(shake_amount, 0, 100, "Camera shake at the blast (1 = a strong shake); read with cw_explosion_shake."),
  ER(shake_radius, 0.1, 1e5, "Shake fades to nothing at this distance."),
  ER(shake_time, 0.05, 60, "Seconds the shake lasts."),
  CW_PROP_FIELD(explo, playing, CW_T_BOOL, CW_PROP_EDITOR | CW_PROP_READONLY, "Playing now."),
  CW_PROP_FIELD(explo, age, CW_T_FLOAT, CW_PROP_EDITOR | CW_PROP_READONLY, "Seconds since it fired."),
  CW_PROP_END,
};
static cw_node_type explo_type = {
  .name = "Explosion3D", .parent = "Node3D", .data_size = sizeof(explo), .props = explo_props,
  .doc = "Fires a whole explosion: restarts the particle systems and shockwave under it, flashes a pooled light, stamps a cooling scorch mark, records a camera shake (explosions.h).",
  .init = explo_init, .enter_tree = explo_enter, .exit_tree = explo_exit,
};

void cw__register_explosion_types(void) {
  cw_node_type_register(&explo_type);
  explo_t = &explo_type;
}

/* ── shake and stats ──────────────────────────────────────────────────── */

float cw_explosion_shake(cw_tree *t, t3_vec3 at) {
  pool *p = t ? cw_tree_slot(t, "fx_explosions") : NULL;
  if (!p) return 0;
  double now = p->time;
  float sum = 0;
  for (int i = 0; i < MAX_SHAKE; i++) {
    const shake_ev *s = &p->sh[i];
    if (!s->on) continue;
    float age = (float)(now - s->t0);
    if (age < 0 || age >= s->duration) continue;
    float d = t3_vec3_length(t3_vec3_sub(at, s->pos));
    if (d >= s->radius) continue;
    float near_k = 1 - d / s->radius, time_k = 1 - age / s->duration;
    sum += s->amount * near_k * near_k * time_k * time_k;
  }
  return CW_MIN(sum, 4.0f);
}

void cw_explosion_get_stats(cw_tree *t, cw_explosion_stats *out) {
  memset(out, 0, sizeof *out);
  pool *p = t ? cw_tree_slot(t, "fx_explosions") : NULL;
  if (!p) return;
  out->flash_pool = p->nfl;
  for (int i = 0; i < p->nfl; i++) {
    if (p->fl[i].busy) out->flashes_active++;
    cw_value v;
    if (cw_node_get_prop(p->fl[i].light, "energy", &v) && v.f > 0.01) out->lights_lit++;
  }
  out->scorch_pool = p->nsc;
  for (int i = 0; i < p->nsc; i++)
    if (p->sc[i].busy) out->scorches_active++;
}
