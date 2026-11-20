/* Object motion in the velocity target (cartwheel/pipeline.h, CW_RES_VELOCITY).
 *
 * The velocity pass first writes the camera's motion, rebuilt from depth: exact for everything that stood
 * still. Then this file draws the meshes that moved since the last frame over it, with the scene's depth
 * for the depth test, each through one plain material carrying the "velbuf" program
 * (extensions/velbuf.mjs): the vertex as it is now through this frame's view-projection, minus the same
 * vertex as it was posed a frame ago through the last frame's.
 *
 * What counts as moved (compared bit for bit, once a frame):
 *   a mesh           its world matrix changed
 *   a skinned mesh   its world matrix, its bind matrices or any bone matrix changed
 *   an instanced mesh  its world matrix changed, or its instance matrices were written (the attribute's
 *                    version) this frame
 * A mesh seen for the first time, or again after frames it was not looked at (hidden, the pass off), has
 * no previous pose and counts as still. Nothing is copied or uploaded for a mesh that stood still: the
 * previous pose is kept by swapping two buffers when the pose changed, and the bone and instance textures
 * are written only for the meshes drawn. Blended materials are left to the camera's motion (they write no
 * depth); so are sprites, points and lines. */
#include <stdlib.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/gpu.h"
#include "pipeline_internal.h"
#include "scene/render3d_internal.h"
#if CW_WGPU
#include "pipeline_wgpu.h"
#endif

#define VEL_INST_W 1024 /* texels a row of the previous instance matrices (256 instances) */
#define VEL_MAX_BONES 1024

typedef struct vel_obj {
  uint32_t id;
  uint64_t seen;       /* the pipeline frame `now` was taken on (+ 1; 0 = never) */
  t3_mat4 now, before; /* object to world (a skinned mesh: world x bindMatrixInverse) */
  t3_mat4 bind;
  bool moved;
  /* skinned: bone matrices, [0] this frame's and [1] the last frame's (swapped when they change) */
  float *bones[2];
  int nb;
  t3_texture *bone_tex;
  /* instanced: the instance matrices this frame and the last */
  float *inst[2];
  int inst_n, inst_cap;
  uint32_t inst_ver;
  bool inst_same; /* inst[1] was not written: the last frame's matrices are inst[0]'s */
  t3_texture *inst_tex;
  uint64_t inst_tex_frame; /* the frame the texture holds the previous matrices of (+ 1) */
} vel_obj;

typedef struct vel_state {
  cw_map objs; /* object id -> vel_obj* */
  t3_material *mat;
  t3_renderer *hook;
  uint64_t frame; /* the pipeline frame being drawn (+ 1) */
  t3_mat4 view_proj, prev_view_proj;
  unsigned fbo; /* GL: the velocity colour with the scene's depth */
  int32_t drawn, tracked;
  bool fields;
} vel_state;

static void obj_free(vel_obj *e) {
  cw_free(e->bones[0]);
  cw_free(e->bones[1]);
  cw_free(e->inst[0]);
  cw_free(e->inst[1]);
  if (e->bone_tex) t3_release(e->bone_tex);
  if (e->inst_tex) t3_release(e->inst_tex);
  cw_free(e);
}

static void state_free(void *p) {
  vel_state *v = p;
  for (int i = 0; i < v->objs.count; i++)
    if (cw_map_live(&v->objs, i)) obj_free((vel_obj *)(uintptr_t)v->objs.vals[i]);
  cw_map_free(&v->objs);
  if (v->mat) t3_release(v->mat);
  if (v->fbo && cw_gpu_backend() != CW_GPU_WEBGPU) glDeleteFramebuffers(1, &v->fbo);
  cw_free(v);
}

static vel_state *state_of(cw_tree *t) {
  vel_state *v = cw_tree_slot(t, "velocity");
  if (v) return v;
  v = cw_calloc(1, sizeof *v, CW_MEM_RENDER);
  cw_map_init(&v->objs, CW_MEM_RENDER);
  cw_tree_set_slot(t, "velocity", v, state_free);
  return v;
}

/* CW_TEST_VELOCITY_OBJECTS=0 (a gate control, read as cw_test_env reads on every host): only the camera's
 * motion, as before this file */
static bool objects_off(void) {
  const char *e = cw_test_env("CW_TEST_VELOCITY_OBJECTS");
  return e && *e == '0';
}

/* this frame's pose of a mesh into its record; true when it differs from the last frame's */
static bool obj_update(vel_state *v, vel_obj *e, t3_object *o) {
  bool fresh = e->seen + 1 != v->frame; /* not looked at last frame: no previous pose */
  t3_mat4 m = o->matrix_world;
  bool moved = false;
  if (o->type == T3_SKINNED_MESH) {
    const t3_skinned_mesh *sm = (const t3_skinned_mesh *)o;
    const t3_skeleton *sk = sm->skeleton;
    int nb = sk ? sk->bone_count : 0;
    if (nb < 1 || nb > VEL_MAX_BONES || !sk->bone_matrices) return false;
    t3_mat4_multiply(&m, &o->matrix_world, &sm->bind_matrix_inverse);
    if (e->nb != nb) {
      for (int k = 0; k < 2; k++) {
        cw_free(e->bones[k]);
        e->bones[k] = cw_calloc((size_t)nb * 16, sizeof(float), CW_MEM_RENDER);
      }
      if (e->bone_tex) t3_release(e->bone_tex);
      e->bone_tex = NULL;
      e->nb = nb;
      fresh = true;
    }
    size_t bytes = (size_t)nb * 16 * sizeof(float);
    if (fresh) {
      memcpy(e->bones[0], sk->bone_matrices, bytes);
      memcpy(e->bones[1], sk->bone_matrices, bytes);
    } else if (memcmp(e->bones[0], sk->bone_matrices, bytes) != 0) {
      float *b = e->bones[1];
      e->bones[1] = e->bones[0];
      e->bones[0] = b;
      memcpy(b, sk->bone_matrices, bytes);
      moved = true;
    } else if (memcmp(e->bones[1], e->bones[0], bytes) != 0) {
      memcpy(e->bones[1], e->bones[0], bytes); /* it stopped: the last frame's pose is this one */
    }
    if (memcmp(&e->bind, &sm->bind_matrix, sizeof e->bind) != 0) e->bind = sm->bind_matrix, moved = moved || !fresh;
  } else if (o->type == T3_INSTANCED_MESH) {
    const t3_instanced_mesh *im = (const t3_instanced_mesh *)o;
    const t3_attribute *a = im->instance_matrix;
    int n = a ? CW_MIN(im->count, a->count) : 0;
    if (n < 1 || a->item_size != 16 || a->type != T3_FLOAT32) return false;
    if (n > e->inst_cap) {
      for (int k = 0; k < 2; k++) {
        cw_free(e->inst[k]);
        e->inst[k] = cw_calloc((size_t)n * 16, sizeof(float), CW_MEM_RENDER);
      }
      e->inst_cap = n;
      fresh = true;
    }
    size_t bytes = (size_t)n * 16 * sizeof(float);
    if (fresh || e->inst_n != n) {
      memcpy(e->inst[0], a->array, bytes);
      e->inst_same = true;
      e->inst_ver = a->version;
      e->inst_n = n;
    } else if (a->version != e->inst_ver) {
      float *b = e->inst[1];
      e->inst[1] = e->inst[0];
      e->inst[0] = b;
      memcpy(b, a->array, bytes);
      e->inst_same = false;
      e->inst_ver = a->version;
      moved = true;
    } else {
      e->inst_same = true; /* not written this frame: the last frame's are this frame's */
    }
  }
  if (fresh) e->before = m;
  else {
    e->before = e->now;
    if (memcmp(&e->now, &m, sizeof m) != 0) moved = true;
  }
  e->now = m;
  e->seen = v->frame;
  return moved && !fresh;
}

typedef struct vel_draw { t3_mesh *m; t3_material **mats; int n; t3_material *one; } vel_draw;
typedef CW_ARRAY(vel_draw) draw_list;
typedef CW_ARRAY(t3_object *) hide_list;

/* hide what the velocity draw leaves out, collect the meshes that moved */
static void walk(vel_state *v, t3_object *o, draw_list *draws, hide_list *hidden) {
  if (!o->visible) return;
  bool drawable = o->type == T3_MESH || o->type == T3_SKINNED_MESH || o->type == T3_INSTANCED_MESH || o->type == T3_SPRITE ||
                  o->type == T3_POINTS || o->type == T3_LINE || o->type == T3_LINE_SEGMENTS || o->type == T3_LINE_LOOP;
  if (drawable) {
    bool mesh = o->type == T3_MESH || o->type == T3_SKINNED_MESH || o->type == T3_INSTANCED_MESH;
    bool moved = false;
    if (mesh) {
      t3_mesh *m = (t3_mesh *)o;
      bool solid = m->geometry && m->material_count > 0;
      for (int i = 0; i < m->material_count && solid; i++)
        solid = m->materials[i] && m->materials[i]->visible && !m->materials[i]->transparent && m->materials[i]->depth_write;
      if (solid) {
        uint64_t val = 0;
        vel_obj *e;
        if (cw_map_get(&v->objs, o->id, &val)) e = (vel_obj *)(uintptr_t)val;
        else {
          e = cw_calloc(1, sizeof *e, CW_MEM_RENDER);
          e->id = o->id;
          cw_map_set(&v->objs, o->id, (uint64_t)(uintptr_t)e);
        }
        if (e->seen == v->frame) moved = e->moved; /* a later view of the frame */
        else moved = e->moved = obj_update(v, e, o);
        if (moved) cw_array_push(draws, (vel_draw){ .m = m });
      }
    }
    if (!moved) {
      o->visible = false;
      cw_array_push(hidden, o);
    }
  }
  for (int i = 0; i < o->child_count; i++) walk(v, o->children[i], draws, hidden);
}

static void fill(t3_extension_ctx *c, void *user) {
  vel_state *v = user;
  t3_extension_set_uniform(c, "velViewProj", v->view_proj.e, 16);
  t3_extension_set_uniform(c, "velPrevViewProj", v->prev_view_proj.e, 16);
  const t3_object *o = t3_extension_object(c);
  uint64_t val = 0;
  if (!o || !cw_map_get(&v->objs, o->id, &val)) return;
  vel_obj *e = (vel_obj *)(uintptr_t)val;
  t3_extension_set_uniform(c, "velModel", e->now.e, 16);
  t3_extension_set_uniform(c, "velPrevModel", e->before.e, 16);
  if (o->type == T3_SKINNED_MESH) {
    t3_extension_set_uniform(c, "velBind", e->bind.e, 16);
    if (e->bone_tex) t3_extension_set_texture(c, "velBones", e->bone_tex);
  } else if (o->type == T3_INSTANCED_MESH) {
    float info[4] = { VEL_INST_W, 0, 0, 0 };
    t3_extension_set_uniform(c, "velInfo", info, 4);
    if (e->inst_tex) t3_extension_set_texture(c, "velInst", e->inst_tex);
  }
}

/* the textures a mesh about to be drawn reads */
static void obj_textures(vel_state *v, vel_obj *e, const t3_object *o) {
  if (o->type == T3_SKINNED_MESH) {
    int w = e->nb * 4;
    if (!e->bone_tex) e->bone_tex = t3_data_texture_new_typed(w, 2, NULL, T3_FLOAT_TYPE);
    float *px = (float *)e->bone_tex->pixels;
    size_t bytes = (size_t)e->nb * 16 * sizeof(float);
    if (memcmp(px, e->bones[0], bytes) != 0 || memcmp(px + (size_t)w * 4, e->bones[1], bytes) != 0) {
      memcpy(px, e->bones[0], bytes);
      memcpy(px + (size_t)w * 4, e->bones[1], bytes);
      e->bone_tex->version++;
    }
  } else if (o->type == T3_INSTANCED_MESH) {
    int rows = (e->inst_n * 4 + VEL_INST_W - 1) / VEL_INST_W;
    if (e->inst_tex && e->inst_tex->height < rows) {
      t3_release(e->inst_tex);
      e->inst_tex = NULL;
    }
    if (!e->inst_tex) {
      e->inst_tex = t3_data_texture_new_typed(VEL_INST_W, rows, NULL, T3_FLOAT_TYPE);
      e->inst_tex_frame = 0;
    }
    if (e->inst_tex_frame != v->frame) {
      const float *prev = e->inst_same ? e->inst[0] : e->inst[1];
      size_t bytes = (size_t)e->inst_n * 16 * sizeof(float);
      if (memcmp(e->inst_tex->pixels, prev, bytes) != 0) {
        memcpy(e->inst_tex->pixels, prev, bytes);
        e->inst_tex->version++;
      }
      e->inst_tex_frame = v->frame;
    }
  }
}

/* records of meshes that are gone (not looked at for a while) */
static void sweep(vel_state *v) {
  if (v->frame % 256 != 0) return;
  for (int i = 0; i < v->objs.count; i++) {
    if (!cw_map_live(&v->objs, i)) continue;
    vel_obj *e = (vel_obj *)(uintptr_t)v->objs.vals[i];
    if (e->seen + 240 < v->frame) {
      cw_map_remove(&v->objs, v->objs.keys[i]);
      obj_free(e);
    }
  }
}

int cw__velocity_objects(cw_pass_ctx *ctx, const cw_rt *vel) {
  t3_renderer *r = ctx->renderer;
  if (!r || !ctx->scene3d || !ctx->camera || !ctx->scene || !ctx->scene->depth_tex) return 0;
  vel_state *v = state_of(ctx->tree);
  if (!v->fields) {
    v->fields = true;
    cw_debug_i32("cw.velocity.objects", &v->drawn);   /* meshes drawn into the velocity target last frame */
    cw_debug_i32("cw.velocity.tracked", &v->tracked); /* meshes whose pose is kept */
  }
  v->drawn = 0;
  if (objects_off()) return 0;
  v->frame = ctx->frame + 1;
  v->view_proj = ctx->view_proj;
  v->prev_view_proj = ctx->prev_view_proj;
  draw_list draws = { 0 };
  hide_list hidden = { 0 };
  walk(v, &ctx->scene3d->base, &draws, &hidden);
  v->tracked = cw_map_size(&v->objs);
  if (draws.count) {
    if (!v->mat) {
      v->mat = t3_mesh_basic_material_new(0xffffff);
      v->mat->side = T3_DOUBLE_SIDE;
      v->mat->fog = false;
      v->mat->depth_write = false; /* the scene's depth decides, and stays as it is */
      v->mat->tone_mapped = false;
      t3_material_set_extension(v->mat, "velbuf");
    }
    if (v->hook != r) {
      t3_renderer_set_extension(r, "velbuf", fill, v);
      v->hook = r;
    }
    for (int i = 0; i < draws.count; i++) {
      vel_draw *d = &draws.data[i];
      uint64_t val = 0;
      cw_map_get(&v->objs, d->m->base.id, &val);
      obj_textures(v, (vel_obj *)(uintptr_t)val, &d->m->base);
      /* one material for every group of the mesh, its own put back after the draw */
      d->mats = d->m->materials;
      d->n = d->m->material_count;
      d->one = v->mat;
      d->m->materials = &d->one;
      d->m->material_count = 1;
    }
    t3_scene *sc = ctx->scene3d;
    t3_texture *bg = sc->background_texture;
    bool had_bg = sc->has_background;
    if (bg) t3_retain(bg), t3_scene_set_background_texture(sc, NULL);
    sc->has_background = false;
    t3_renderer_set_shadow_hold(r, true); /* the program reads no shadow map */
#if CW_WGPU
    if (cw_gpu_backend() == CW_GPU_WEBGPU) {
      t3_external_target et = { .samples = 1, .load_color = true, .load_depth = true, .clear_depth = cw_gpu_reversed_depth() ? 0.0f : 1.0f,
                                .x = 0, .y = 0, .w = vel->w, .h = vel->h, .flip_y = true };
      et.color = (uintptr_t)cw__pw_attach(vel->tex), et.depth = (uintptr_t)cw__pw_attach(ctx->scene->depth_tex);
      et.color_format = (int)cw__pw_format(vel->tex), et.depth_format = (int)cw__pw_format(ctx->scene->depth_tex);
      WGPUCommandEncoder enc = cw__pw_encoder();
      t3_renderer_set_external_encoder(r, (uintptr_t)enc);
      t3_renderer_set_external_target(r, &et);
      cw__pw_timing_span((void *)enc, false);
      t3_renderer_render(r, sc, ctx->camera);
      cw__pw_timing_span((void *)enc, true);
      t3_renderer_set_external_target(r, NULL);
    } else
#endif
    {
      if (!v->fbo) glGenFramebuffers(1, &v->fbo);
      glBindFramebuffer(GL_FRAMEBUFFER, v->fbo);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, vel->tex, 0);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, ctx->scene->depth_tex, 0);
      /* (the depth texture is attached, so no unit may still sample it) */
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, 0);
      if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        glViewport(0, 0, vel->w, vel->h);
        int bw, bh, vx, vy, vw, vh, sx, sy, sw, sh;
        t3_renderer_get_size(r, &bw, &bh);
        t3_renderer_get_viewport(r, &vx, &vy, &vw, &vh);
        bool st = t3_renderer_get_scissor(r, &sx, &sy, &sw, &sh);
        bool auto_clear = t3_renderer_get_auto_clear(r);
        t3_renderer_reset_bindings(r);
        t3_renderer_set_framebuffer(r, v->fbo);
        t3_renderer_set_viewport(r, 0, bh - vel->h, vel->w, vel->h);
        t3_renderer_set_scissor(r, 0, bh - vel->h, vel->w, vel->h);
        t3_renderer_set_scissor_test(r, true);
        t3_renderer_set_auto_clear(r, false);
        t3_renderer_render(r, sc, ctx->camera);
        t3_renderer_set_auto_clear(r, auto_clear);
        t3_renderer_set_viewport(r, vx, vy, vw, vh);
        t3_renderer_set_scissor(r, sx, sy, sw, sh);
        t3_renderer_set_scissor_test(r, st);
        t3_renderer_set_framebuffer(r, ctx->pipeline->bound_fbo);
        t3_renderer_reset_bindings(r);
      }
      glBindFramebuffer(GL_FRAMEBUFFER, v->fbo);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
      cw__pass_state_reset();
    }
    t3_renderer_set_shadow_hold(r, false);
    cw__render3d_count(ctx->tree);
    sc->has_background = had_bg;
    if (bg) t3_scene_set_background_texture(sc, bg), t3_release(bg);
    for (int i = 0; i < draws.count; i++) {
      vel_draw *d = &draws.data[i];
      d->m->materials = d->mats;
      d->m->material_count = d->n;
    }
    v->drawn = draws.count;
  }
  for (int i = 0; i < hidden.count; i++) hidden.data[i]->visible = true;
  cw_array_free(&hidden);
  cw_array_free(&draws);
  sweep(v);
  return v->drawn;
}
