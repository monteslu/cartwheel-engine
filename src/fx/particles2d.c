/* GPUParticles2D: the GPUParticles3D emission model on the CPU (fixed ticks,
 * threaded on the job pool, deterministic), drawn as sprites through the
 * render2d batcher (one cw_r2d_sprites call per system). */
#include <math.h>
#include <string.h>

#include "cartwheel/jobs.h"
#include "cartwheel/render2d.h"
#include "cartwheel/scene2d.h"
#include "fx_internal.h"

enum { S2_POINT, S2_CIRCLE, S2_RECT };
static const char *const shape2_names[] = { "POINT", "CIRCLE", "RECT", NULL };
enum { B2_ALPHA, B2_ADD };
static const char *const blend2_names[] = { "ALPHA", "ADD", NULL };
#define CURVE2_N 128

typedef struct p2d {
  int amount;
  float lifetime, lifetime_random, explosiveness;
  bool one_shot, emitting;
  int seed;
  int emission_shape;
  float emission_radius;
  cw_vec2 emission_extents;
  float direction, spread, speed_min, speed_max;
  cw_vec2 gravity;
  float damping, size, size_random;
  float color[4];
  const char *color_ramp, *size_curve, *texture;
  int blend;
  float spin;
  bool local_coords;
  int alive;
  /* runtime */
  struct p2_state *st;
} p2d;

typedef struct p2_state {
  int cap;
  float *px, *py, *vx, *vy, *age;
  int32_t *cycle;
  double sim_time, emit_start, emit_end;
  bool was_emitting;
  float m0[6], m1[6]; /* spawn frame at the step's start / end */
  bool have_m;
  float t0, t1;
  int counts[256];
  int chunk;
  const char *baked_ramp, *baked_size;
  float ramp[CURVE2_N * 4], sizes[CURVE2_N];
  cw_texture tex;
  const char *tex_path;
  const p2d *d;
} p2_state;

static const cw_node_type *p2d_t;

#define H cw_particle_hash
static uint32_t pseed2(uint32_t seed, int i, int k) {
  return H(seed ^ H((uint32_t)i * 0x9E3779B9u ^ H((uint32_t)k + 0x632BE5ABu)));
}
static float prnd2(uint32_t *s) {
  *s = H(*s);
  return (float)(*s >> 8) * (1.0f / 16777216.0f);
}

static void state_alloc(p2_state *s, int n) {
  if (s->cap == n) return;
  cw_free(s->px);
  s->cap = n;
  /* one block: 5 float arrays + cycles */
  s->px = cw_calloc((size_t)n * 6 + 1, sizeof(float), CW_MEM_RENDER);
  s->py = s->px + n;
  s->vx = s->py + n;
  s->vy = s->vx + n;
  s->age = s->vy + n;
  s->cycle = (int32_t *)(s->age + n);
  for (int i = 0; i < n; i++) {
    s->age[i] = -1;
    s->cycle[i] = -1;
  }
}

static void step_range(int begin, int end, int worker, void *ctx) {
  (void)worker;
  p2_state *s = ctx;
  const p2d *d = s->d;
  float L = d->lifetime, dt = s->t1 - s->t0;
  float ee = s->emit_end < 0 ? -1.0f : (float)s->emit_end;
  float damp = 1.0f / (1.0f + d->damping * dt);
  float cs_spread = d->spread * (CW_PI / 180);
  int alive = 0;
  for (int i = begin; i < end; i++) {
    float off = (float)i / (float)d->amount * L * (1.0f - d->explosiveness);
    float rel = s->t1 - off;
    int k = rel >= 0 ? (int)floorf(rel / L) : -1;
    float spawn = off + (float)k * L;
    bool ok = k >= 0 && (ee < 0 || spawn < ee);
    uint32_t sd = ok ? pseed2((uint32_t)d->seed, i, k) : 0;
    float life = ok ? L * (1.0f - d->lifetime_random * prnd2(&sd)) : 0;
    float age = rel - (float)k * L;
    if (!ok || age >= life) {
      s->age[i] = -1;
      continue;
    }
    alive++;
    if (s->age[i] < 0 || s->cycle[i] != k) {
      float f = dt > 0 ? CW_CLAMP((spawn - s->t0) / dt, 0, 1) : 1;
      float m[6];
      for (int j = 0; j < 6; j++) m[j] = s->m0[j] + (s->m1[j] - s->m0[j]) * f;
      float lx = 0, ly = 0;
      if (d->emission_shape == S2_CIRCLE) {
        float a = prnd2(&sd) * CW_TAU, r = d->emission_radius * sqrtf(prnd2(&sd));
        float sn, cs;
        cw_sincos(a, &sn, &cs);
        lx = cs * r;
        ly = sn * r;
      } else if (d->emission_shape == S2_RECT) {
        lx = (prnd2(&sd) * 2 - 1) * d->emission_extents.x;
        ly = (prnd2(&sd) * 2 - 1) * d->emission_extents.y;
      }
      float ang = d->direction * (CW_PI / 180) + (prnd2(&sd) * 2 - 1) * cs_spread;
      float sp = d->speed_min + (d->speed_max - d->speed_min) * prnd2(&sd);
      float sn, cs;
      cw_sincos(ang, &sn, &cs);
      /* direction through the frame's linear part, normalized */
      float dx = m[0] * cs + m[2] * sn, dy = m[1] * cs + m[3] * sn;
      float dl = sqrtf(dx * dx + dy * dy);
      if (dl > 0) {
        dx /= dl;
        dy /= dl;
      }
      float vx = dx * sp + d->gravity.x * age, vy = dy * sp + d->gravity.y * age;
      s->px[i] = m[0] * lx + m[2] * ly + m[4] + vx * age;
      s->py[i] = m[1] * lx + m[3] * ly + m[5] + vy * age;
      s->vx[i] = vx;
      s->vy[i] = vy;
      s->age[i] = age;
      s->cycle[i] = k;
      continue;
    }
    float vx = (s->vx[i] + d->gravity.x * dt) * damp, vy = (s->vy[i] + d->gravity.y * dt) * damp;
    s->vx[i] = vx;
    s->vy[i] = vy;
    s->px[i] += vx * dt;
    s->py[i] += vy * dt;
    s->age[i] = age;
  }
  s->counts[begin / s->chunk] = alive;
}

static void frame_of(cw_node n, const p2d *d, float out[6]) {
  if (d->local_coords) {
    out[0] = 1;
    out[1] = 0;
    out[2] = 0;
    out[3] = 1;
    out[4] = out[5] = 0;
    return;
  }
  memcpy(out, cw_node2d_global_transform(n), sizeof(float) * 6);
}

static void p2_fixed(cw_node n, void *data, double dt) {
  p2d *d = data;
  p2_state *s = d->st;
  if (d->amount <= 0 || d->lifetime <= 0) {
    d->alive = 0;
    return;
  }
  state_alloc(s, d->amount);
  float m[6];
  frame_of(n, d, m);
  if (!s->have_m) {
    memcpy(s->m1, m, sizeof m);
    s->have_m = true;
  }
  if (d->emitting && !s->was_emitting) {
    s->emit_start = s->sim_time;
    s->emit_end = d->one_shot ? (double)d->lifetime : -1;
    for (int i = 0; i < s->cap; i++) s->age[i] = -1;
  } else if (!d->emitting && s->was_emitting && s->emit_end < 0) {
    s->emit_end = s->sim_time - s->emit_start;
  }
  s->was_emitting = d->emitting;
  memcpy(s->m0, s->m1, sizeof m);
  memcpy(s->m1, m, sizeof m);
  if (s->emit_start < 0 && !d->emitting) {
    s->sim_time += dt;
    return;
  }
  s->t0 = (float)(s->sim_time - s->emit_start);
  s->sim_time += dt;
  s->t1 = (float)(s->sim_time - s->emit_start);
  s->d = d;
  s->chunk = CW_MAX(512, (d->amount + 255) / 256);
  int chunks = (d->amount + s->chunk - 1) / s->chunk;
  memset(s->counts, 0, sizeof s->counts);
  cw_parallel_for(d->amount, s->chunk, step_range, s);
  int alive = 0;
  for (int i = 0; i < chunks; i++) alive += s->counts[i];
  d->alive = alive;
  if (d->one_shot && d->emitting && s->t1 >= 2.0f * d->lifetime) d->emitting = false;
}

static cw_texture dot2d(void) {
  static cw_texture t;
  if (cw_texture_valid(t)) return t;
  enum { S = 32 };
  static uint8_t px[S * S * 4];
  for (int y = 0; y < S; y++)
    for (int x = 0; x < S; x++) {
      float dx = (x + 0.5f) / S * 2 - 1, dy = (y + 0.5f) / S * 2 - 1;
      float r2 = dx * dx + dy * dy;
      float a = r2 >= 1 ? 0 : (1 - r2) * (1 - r2);
      uint8_t *o = px + 4 * (y * S + x);
      o[0] = o[1] = o[2] = 255;
      o[3] = (uint8_t)(a * 255 + 0.5f);
    }
  t = cw_texture_from_pixels(S, S, px, NULL);
  return t;
}

static void p2_draw(cw_node n, void *data, const cw_canvas_item *item) {
  (void)n;
  p2d *d = data;
  p2_state *s = d->st;
  if (!s->cap || !d->alive) return;
  if (s->baked_ramp != d->color_ramp || s->baked_size != d->size_curve) {
    cw__fx_curve_bake(d->color_ramp, 4, 1, CURVE2_N, s->ramp, "GPUParticles2D.color_ramp");
    cw__fx_curve_bake(d->size_curve, 1, 1, CURVE2_N, s->sizes, "GPUParticles2D.size_curve");
    s->baked_ramp = d->color_ramp;
    s->baked_size = d->size_curve;
  }
  if (s->tex_path != d->texture) {
    s->tex_path = d->texture;
    s->tex = d->texture && *d->texture ? cw_texture_load(d->texture, NULL) : (cw_texture){ 0 };
  }
  cw_texture tex = cw_texture_valid(s->tex) ? s->tex : dot2d();
  if (!d->local_coords) {
    cw_xform2d id = cw_xform2d_identity();
    cw_r2d_set_transform(&id);
  }
  cw_r2d_set_blend(d->blend == B2_ADD ? CW_BLEND_ADD : CW_BLEND_ALPHA);
  cw_sprite *sp = cw_frame_alloc(sizeof(cw_sprite) * (size_t)d->alive);
  int k = 0;
  for (int i = 0; i < s->cap && k < d->alive; i++) {
    if (s->age[i] < 0) continue;
    uint32_t sd = pseed2((uint32_t)d->seed, i, s->cycle[i]);
    float life = d->lifetime * (1.0f - d->lifetime_random * prnd2(&sd));
    float t = CW_CLAMP(s->age[i] / life, 0, 1);
    int ci = CW_CLAMP((int)(t * (CURVE2_N - 1) + 0.5f), 0, CURVE2_N - 1);
    uint32_t r = sd ^ 0x5bd1e995u;
    float r1 = prnd2(&r), r2 = prnd2(&r), r3 = prnd2(&r);
    float sz = d->size * s->sizes[ci] * (1.0f - d->size_random * r1);
    const float *c = s->ramp + 4 * ci;
    sp[k] = (cw_sprite){ .texture = tex, .pos = { s->px[i], s->py[i] }, .size = { sz, sz }, .pivot = { 0.5f, 0.5f },
                         .rotation = r2 * CW_TAU + d->spin * (r3 < 0.5f ? -1.0f : 1.0f) * s->age[i],
                         .color = { c[0] * d->color[0] * item->modulate.r, c[1] * d->color[1] * item->modulate.g,
                                    c[2] * d->color[2] * item->modulate.b, c[3] * d->color[3] * item->modulate.a } };
    if (sp[k].color.a <= 0) sp[k].color.a = 1e-6f; /* zero-init colour means white to render2d */
    k++;
  }
  cw_r2d_sprites(sp, k);
  cw_r2d_set_blend(CW_BLEND_ALPHA);
}

static void p2_init(cw_node n, void *data) {
  (void)n;
  p2d *d = data;
  d->amount = 64;
  d->lifetime = 1;
  d->emitting = true;
  d->seed = 1;
  d->emission_radius = 8;
  d->emission_extents = cw_v2(8, 8);
  d->direction = -90;
  d->spread = 20;
  d->speed_min = 60;
  d->speed_max = 120;
  d->gravity = cw_v2(0, 98);
  d->size = 8;
  d->color[0] = d->color[1] = d->color[2] = d->color[3] = 1;
  d->color_ramp = cw_intern("");
  d->size_curve = cw_intern("");
  d->texture = cw_intern("");
  d->st = cw_calloc(1, sizeof *d->st, CW_MEM_RENDER);
  d->st->emit_start = -1;
  d->st->emit_end = -1;
}

static void p2_destroy(cw_node n, void *data) {
  (void)n;
  p2d *d = data;
  if (!d->st) return;
  cw_free(d->st->px);
  cw_free(d->st);
  d->st = NULL;
}

static void p2_changed(cw_node n, void *data, const cw_prop *p) {
  (void)n;
  p2d *d = data;
  if (!strcmp(p->name, "seed") || !strcmp(p->name, "amount") || !strcmp(p->name, "lifetime") ||
      !strcmp(p->name, "explosiveness") || !strcmp(p->name, "one_shot"))
    d->st->was_emitting = false;
  if (!strcmp(p->name, "amount")) d->amount = CW_CLAMP(d->amount, 0, 1 << 20);
}

void cw__particles2d_restart(cw_node n) {
  p2d *d = cw_node_data_t(n, p2d_t);
  if (!d) return;
  d->emitting = true;
  d->st->was_emitting = false;
}

int cw__particles2d_alive(cw_node n) {
  p2d *d = cw_node_data_t(n, p2d_t);
  return d ? d->alive : 0;
}

#define F2(field, ty, doc) CW_PROP_FIELD(p2d, field, ty, CW_PROP_DEFAULT, doc)
#define E2(field, names, ds) \
  { .name = #field, .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(p2d, field), .size = sizeof(int), \
    .enum_names = names, .doc = ds }
static const cw_prop p2d_props[] = {
  F2(amount, CW_T_INT, "Particle count."),
  CW_PROP_FIELD_RANGE(p2d, lifetime, CW_T_FLOAT, 0.001, 1e6, "Seconds each particle lives."),
  CW_PROP_FIELD_RANGE(p2d, lifetime_random, CW_T_FLOAT, 0, 1, "Lives shortened by up to this fraction."),
  CW_PROP_FIELD_RANGE(p2d, explosiveness, CW_T_FLOAT, 0, 1, "0 = spread over the lifetime, 1 = all at once."),
  F2(one_shot, CW_T_BOOL, "Emit one cycle, then stop."),
  F2(emitting, CW_T_BOOL, "Emitting (on starts a new window)."),
  F2(seed, CW_T_INT, "Random seed."),
  E2(emission_shape, shape2_names, "POINT, CIRCLE (emission_radius) or RECT (emission_extents)."),
  CW_PROP_FIELD_RANGE(p2d, emission_radius, CW_T_FLOAT, 0, 1e6, "CIRCLE radius, pixels."),
  F2(emission_extents, CW_T_VEC2, "RECT half sizes, pixels."),
  F2(direction, CW_T_FLOAT, "Start direction, degrees clockwise from +x (-90 = up)."),
  CW_PROP_FIELD_RANGE(p2d, spread, CW_T_FLOAT, 0, 180, "Half angle around direction, degrees."),
  F2(speed_min, CW_T_FLOAT, "Start speed range, pixels/s."),
  F2(speed_max, CW_T_FLOAT, "Start speed range, pixels/s."),
  F2(gravity, CW_T_VEC2, "Acceleration, pixels/s^2 (y down)."),
  CW_PROP_FIELD_RANGE(p2d, damping, CW_T_FLOAT, 0, 1000, "Velocity damping per second."),
  CW_PROP_FIELD_RANGE(p2d, size, CW_T_FLOAT, 0, 1e6, "Sprite size, pixels (times size_curve)."),
  CW_PROP_FIELD_RANGE(p2d, size_random, CW_T_FLOAT, 0, 1, "Size shrunk by up to this fraction."),
  F2(color, CW_T_COLOR, "Tint (times color_ramp)."),
  F2(color_ramp, CW_T_STRING, "Colour over life: \"t r g b a, ...\"."),
  F2(size_curve, CW_T_STRING, "Size over life: \"t s, ...\"."),
  F2(texture, CW_T_STRING, "Sprite image (asset path; empty = a soft dot)."),
  E2(blend, blend2_names, "ALPHA or ADD."),
  F2(spin, CW_T_FLOAT, "Rotation speed, rad/s (random direction)."),
  F2(local_coords, CW_T_BOOL, "Particles follow the node."),
  CW_PROP_FIELD(p2d, alive, CW_T_INT, CW_PROP_EDITOR | CW_PROP_READONLY, "Particles alive now."),
  CW_PROP_END,
};

static cw_node_type p2d_type = {
  .name = "GPUParticles2D", .parent = "Node2D", .data_size = sizeof(p2d), .props = p2d_props,
  .doc = "2D particles: the GPUParticles3D emission model on the CPU, drawn through render2d.",
  .init = p2_init, .destroy = p2_destroy, .fixed_update = p2_fixed, .prop_changed = p2_changed,
};

void cw__register_particles2d_types(void) {
  cw_node_type_register(&p2d_type);
  p2d_t = &p2d_type;
  cw_canvas_register_type("GPUParticles2D", p2_draw);
}
