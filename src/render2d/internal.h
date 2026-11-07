/* render2d internals shared by the files of src/render2d. */
#ifndef CW_R2D_INTERNAL_H
#define CW_R2D_INTERNAL_H

#include "cartwheel/core.h"
#include "cartwheel/gl.h"
#include "cartwheel/render2d.h"

#ifndef GL_MAX_ARRAY_TEXTURE_LAYERS
#define GL_MAX_ARRAY_TEXTURE_LAYERS 0x88FF
#endif
#ifndef GL_TEXTURE_2D_ARRAY
#define GL_TEXTURE_2D_ARRAY 0x8C1A
#endif
#ifndef GL_COLOR_ATTACHMENT1
#define GL_COLOR_ATTACHMENT1 0x8CE1
#endif
#ifndef GL_COLOR_ATTACHMENT2
#define GL_COLOR_ATTACHMENT2 0x8CE2
#endif
#ifndef GL_DEPTH_STENCIL_ATTACHMENT
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#endif
#ifndef GL_STREAM_DRAW
#define GL_STREAM_DRAW 0x88E0
#endif

#define R2D_PAGE 2048       /* atlas page size (GLES 3.0 guarantees 2048) */
#define R2D_NO_ARRAY 0xfffu /* state: no texture (solid), joins any batch */
#define R2D_RING 3
#define R2D_MIP_LEVELS 4 /* level 0 and three halvings */
#define R2D_MIP_PAD 8    /* gutter of mipmapped atlas images: 1 texel at the smallest level */

/* vertex mode bits (a_info.y) */
enum { R2D_MODE_SOLID = 1, R2D_MODE_SDF = 2, R2D_MODE_ADD = 4, R2D_MODE_YUP = 8 };

/* the stream vertex: 28 bytes */
typedef struct r2d_vertex {
  float x, y, u, v;
  uint8_t rgba[4];   /* straight alpha; premultiplied in the vertex shader */
  uint8_t info[4];   /* page, mode, canvas layer, SDF: distance change per screen pixel (x255) */
  uint8_t sdf[4];    /* SDF text: dilation (edge = 0.5 - x/255), extra edge softness (x/255), 0, 0 */
} r2d_vertex;
CW_STATIC_ASSERT(sizeof(r2d_vertex) == 28, "stream vertex is 28 bytes");

/* a texture array: an atlas (many images, packed) or a standalone image */
typedef struct r2d_array {
  bool live, atlas, repeat, nearest;
  bool hdr;                /* RGBA16F storage (targets of the HDR canvas: the screen texture) */
  int levels;              /* mip levels (1 = none; mipmapped atlas pages have R2D_MIP_LEVELS) */
  GLuint tex, normal_tex;  /* normal_tex: mirror of the atlas layout, lazily made */
  GLuint mat_tex;          /* material mirror (emission, specular, gloss), lazily made */
  void *wtex, *wntex;      /* WebGPU: WGPUTexture (colour, normal mirror) */
  void *wmtex;             /* WebGPU: the material mirror */
  void *wbg;               /* WebGPU: cached bind group (colour, sampler, normal-or-dummy) */
  int w, h, layers;
  void *packers;           /* atlas: stbrp_context per layer */
} r2d_array;

typedef struct r2d_tex {
  uint16_t array, page;
  int x, y, w, h;          /* pixel rect inside the page */
  bool y_up;               /* GL orientation (render targets) */
  bool target, has_normal, has_material;
  bool hdr;                /* a target with RGBA16F storage */
  GLuint fbo, depth_rb;
  void *wdepth;            /* WebGPU: depth-stencil texture of a target made with depth */
  void *rt;                /* cw_target_rt: the cw_rt naming this target (made on first ask) */
  void *wview;             /* WebGPU: a view of the target for three.c (cw__r2d_target_three_view) */
  cw_filter filter;
  bool premultiplied;
} r2d_tex;

/* a sorted-stream command */
typedef struct r2d_cmd {
  uint64_t key;
  uint32_t state;   /* array | blend << 12 | clip << 14 | kind << 22 */
  uint32_t ifirst, icount;
  uint32_t payload; /* kind TILEMAP: index into the pass's tilemap draws */
  uint32_t src;     /* sorted copies: ifirst in the submission index stream */
} r2d_cmd;

enum { R2D_KIND_STREAM = 0, R2D_KIND_TILEMAP = 1, R2D_KIND_PARTICLES = 2 };
/* a particle emitter of the pass: its uniform block (shaders: u_pa / pa) and count */
typedef struct r2d_part_draw { float u[9][4]; int count; uint32_t array; } r2d_part_draw;
#define R2D_STATE(array, blend, clip, kind) \
  ((uint32_t)(array) | ((uint32_t)(blend) << 12) | ((uint32_t)(clip) << 14) | ((uint32_t)(kind) << 22))
#define R2D_ST_ARRAY(s) ((s) & 0xfffu)
#define R2D_ST_BLEND(s) (((s) >> 12) & 3u)
#define R2D_ST_CLIP(s) (((s) >> 14) & 0xffu)
#define R2D_ST_KIND(s) (((s) >> 22) & 3u)
#define R2D_ST_OCCL (1u << 24) /* lit layers: the draw also casts shadows by its alpha (cw_r2d_set_occluder) */

typedef struct r2d_tilemap_draw {
  uint64_t map_id;
  int map_layer;
  cw_vec2 origin;
  cw_color tint;
  cw_xform2d local;
  int canvas_layer;
} r2d_tilemap_draw;

typedef struct r2d_light {
  cw_light2d l;
  cw_xform2d local;
  int layer;
} r2d_light;

typedef struct r2d_occluder {
  int first, count; /* into occluder points (already in layer space after local transform) */
  bool closed;
  int layer;
} r2d_occluder;

typedef struct r2d_program {
  GLuint prog;
  GLint u_layers, u_target, u_tex, u_nrm, u_time;
  GLint u[16]; /* program-specific */
} r2d_program;

typedef struct r2d_batch {
  uint32_t state, first, count, payload;
  int layer;
  int layer_last; /* the last canvas layer merged into it */
  bool lit;
} r2d_batch;

typedef struct r2d_state {
  bool inited, failed;
  /* resources */
  cw_pool textures;  /* r2d_tex */
  CW_ARRAY(r2d_array) arrays;
  int atlas_array[2];   /* per cw_filter, -1 until made */
  GLuint dummy_normal;  /* 1x1x1 "no normal" array */
  GLuint dummy_material; /* 1x1x1 "no material" array (zeros) */
  int max_layers;
  GLuint blit_read_fbo, blit_draw_fbo;
  /* programs */
  r2d_program sprite_distort;  /* distortion layers: offsets into the distortion buffer */
  r2d_program part, part_lit;   /* GPU particles (u[0] = u_pa) */
  GLuint empty_vao;
  CW_ARRAY(r2d_part_draw) pdraws; /* this pass's particle emitters */
  r2d_program sprite_mask, tile_mask; /* occluders from art: alpha over half into the occluder mask */
  bool occluder_on;            /* cw_r2d_set_occluder */
  bool mask_mode;              /* tilemap.c draws with the mask program (inside build_mask) */
  CW_ARRAY(r2d_batch) occ_batches; /* this lit run's batches with R2D_ST_OCCL */  /* distortion layers: offsets into the distortion buffer */
  r2d_program sprite, sprite_lit, tile, tile_lit, light, shadow, composite, blit;
  /* streams */
  GLuint vbo[R2D_RING], ibo[R2D_RING], vao[R2D_RING];
  size_t vbo_cap[R2D_RING], ibo_cap[R2D_RING];
  int ring;
  GLuint quad_vbo, quad_vao;       /* small scratch geometry (lights, composite, blit) */
  GLuint tile_ibo; int tile_ibo_quads;
  /* layers */
  cw_r2d_layer_desc layers[CW_R2D_MAX_LAYERS];
  cw_xform2d layer_view[CW_R2D_MAX_LAYERS]; /* world -> canvas, per pass */
  /* pass */
  bool in_pass;
  cw_r2d_pass_desc pass;
  int canvas_w, canvas_h;     /* what draws address */
  int target_w, target_h;     /* the framebuffer the pass ends in */
  GLuint target_fbo;
  cw_rect canvas_rect;        /* window passes: where the canvas lands */
  int win_w, win_h;
  int win_virtual_w, win_virtual_h; /* the last window pass's virtual canvas (0 = none) */
  /* render scale: draws address a logical canvas (view_w x view_h) that the
   * pass scales by px into its canvas_w x canvas_h pixels. 1 for render
   * targets and virtual canvases; a plain window pass gets the platform's
   * pixel ratio (cw_pixel_ratio), so text and shapes rasterize at full
   * resolution while games keep their logical coordinates */
  int view_w, view_h;
  int win_view_h;             /* the last window pass's canvas height (text: the scale's cap, the size check) */
  float px_x, px_y;
  float win_px_x, win_px_y;   /* the last window pass: drawable pixels per logical window unit */
  cw_texture canvas_rt;       /* virtual canvas */
  cw_texture scene_rt, normal_rt, light_rt; /* lit layers */
  bool normals_used;          /* lit layers draw normals and materials: a normal or material map, or emission, was drawn into one */
  bool material_used;         /* a material map or emission was drawn into a lit layer (the lights add specular) */
  /* HDR and post (cw_r2d_post): the pass's chain, copied at begin */
  bool has_post;
  cw_r2d_post post;
  bool has_gi;
  bool offscreen;              /* this pass draws offscreen: a layer reads the screen or distorts it */
  cw_texture screen_tex;       /* cw_r2d_screen_texture */
  bool next_array_hdr;        /* r2d_array_new makes RGBA16F storage (set round the call) */
  int next_array_levels;       /* r2d_array_new makes this many mip levels (0 = 1; set round the call) */
  int atlas_mip;               /* the mipmapped atlas page being filled, -1 until made */
  cw_r2d_gi gi;
  /* draw state packed into every vertex's sdf[2], sdf[3]: emission share, intensity (1 + x * 15 / 255) */
  uint8_t emissive_b, intensity_b;
  uint8_t flash_b;             /* flash toward the vertex colour (x255): vertex sdf[0] of sprites and shapes */
  uint8_t sway_b;              /* wind sway share of later sprites' top edges (x255): vertex info[3] */
  /* draw state */
  int cur_layer, cur_z;
  cw_blend blend;
  bool has_xform;
  cw_xform2d xform;
  int clip_stack[32], clip_depth, cur_clip;
  CW_ARRAY(cw_rect) clips;    /* index 0 unused (no clip) */
  /* submission */
  CW_ARRAY(r2d_vertex) verts;
  CW_ARRAY(uint32_t) idx;
  CW_ARRAY(r2d_cmd) cmds;
  CW_ARRAY(r2d_tilemap_draw) tdraws;
  CW_ARRAY(r2d_light) lights;
  CW_ARRAY(r2d_occluder) occluders;
  CW_ARRAY(cw_vec2) occ_pts;
  /* sort scratch */
  CW_ARRAY(uint32_t) order, order_tmp;
  CW_ARRAY(uint64_t) keys, keys_tmp;
  CW_ARRAY(r2d_cmd) scmds; /* commands in sorted order */
  CW_ARRAY(uint32_t) out_idx;
  /* fonts, tilemaps */
  cw_pool fonts, tilemaps;
  /* stats */
  cw_r2d_stats stats;
  uint64_t stats_frame;
  bool stats_frame_valid;
} r2d_state;

extern r2d_state R;

bool r2d_lazy_init(void);
cw_rect r2d_clip_px(cw_rect canvas_rect); /* a clip rect, logical canvas -> pass pixels */
float r2d_px(void);                       /* the pass's render scale (the larger axis) */
/* gl.c */
bool r2d_programs_init(void);
/* program-specific uniform slots in r2d_program.u[] */
enum { U_M0, U_M1, U_SET0, U_SET1, U_INV_PAGE, U_TINT };      /* tile */
enum { U_CENTER, U_LIGHT, U_PARAMS };                          /* light */
enum { U_PAGE };                                               /* blit */
/* batch.c: zero the per-frame stats when a new engine frame started (the
 * first count of a frame, a pass or a text layout, calls it) */
void r2d_stats_frame_check(void);
/* text.c: a new frame's text sizes start empty (called from r2d_stats_frame_check) */
void r2d_text_frame_reset(void);

/* text.c: a complex text's glyphs as source byte offsets in visual (left to
 * right) order, every line; the glyph count (0 for simple text). For tests. */
int r2d_text_visual_bytes(cw_font f, const char *text, const cw_text_style *style, int *out, int cap);

/* texture.c */
r2d_tex *r2d_tex_get(cw_texture t);
int r2d_array_new(int w, int h, int layers, bool nearest, bool repeat, bool atlas);
void r2d_array_free(int a);
cw_texture r2d_texture_new_internal(int w, int h, cw_filter filter, bool depth); /* a render target */
cw_texture r2d_texture_new_hdr(int w, int h); /* a render target with RGBA16F storage, linear, no depth */
void r2d_textures_shutdown(void);
GLuint r2d_array_normal_tex(int a);
GLuint r2d_array_material_tex(int a); /* the material mirror, or the "no material" texture */
/* UV rect of a source rect (texture pixels) in page UVs, honouring y_up */
void r2d_tex_uv(const r2d_tex *t, cw_rect src, float *u0, float *v0, float *u1, float *v1);
void r2d_tex_uv_point(const r2d_tex *t, float sx, float sy, float *u, float *v);
/* batch.c */
void r2d_fog_uniform(float out[CW_R2D_MAX_LAYERS * 4]); /* batch.c: the layers' fog */
void r2d_wind_uniform(float out[4]); /* batch.c: the pass's wind for the sprite vertex stage */
void r2d_note_tex(const r2d_tex *t);
void r2d_particles_gl(const r2d_part_draw *d, bool lit); /* batch.c: one emitter, instanced */
void r2d_wg_particles(const r2d_part_draw *d, bool lit); /* wgpu.c */
void r2d_wg_mask_batches(void *mask, const r2d_batch *b, int n); /* wgpu.c: occluder batches into the mask */
/* timers.c: GPU time of each part of cw_r2d_end (native GL, CW_R2D_TIMERS=1; no-ops otherwise) */
enum { R2D_T_DRAW, R2D_T_FIELD, R2D_T_LIGHTS, R2D_T_GI, R2D_T_COMPOSITE, R2D_T_POST, R2D_T_COUNT };
void r2d_timer_begin(int part);
void r2d_timer_end(void);
void r2d_timers_frame(void); /* collect finished queries (once per cw_r2d_end) */
bool r2d_timers_on(void);    /* CW_R2D_TIMERS asked for them and the host has them */
void r2d_timers_wg_result(const float ms[R2D_T_COUNT], const bool seen[R2D_T_COUNT]);
/* wgpu.c: timestamps written by empty compute passes between render passes (no pass may be open) */
bool r2d_wg_timers_init(void);
bool r2d_wg_timer_mark(int part, bool end);
void r2d_wg_timers_resolve(void); /* before the submit */
void r2d_wg_timers_map(void);     /* after the submit */ /* a lit layer draws this texture: normals and materials on when it has maps */
r2d_vertex *r2d_alloc(int nv, int ni, uint32_t **idx_out, uint32_t *base_out, uint32_t array, float sort_y,
                      int extra_z);
void r2d_pack_color(uint8_t out[4], cw_color c);
uint8_t r2d_mode(bool solid); /* SOLID / ADD bits for the current blend */
void r2d_scratch_draw(const float *xyuv, int nverts, GLenum mode);
void r2d_draw_quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1);
static inline cw_color r2d_default_white(cw_color c) {
  return (c.r == 0 && c.g == 0 && c.b == 0 && c.a == 0) ? CW_WHITE : c;
}
void r2d_count_draw(void);
uint64_t r2d_make_key(int extra_z, float sort_y);
uint32_t r2d_current_state(uint32_t array);
void r2d_push_cmd(uint64_t key, uint32_t state, uint32_t ifirst, uint32_t icount);
void r2d_push_special(uint32_t array, float sort_y, uint32_t payload); /* a tilemap command */
/* tilemap.c */
void r2d_tilemaps_init(void);
void r2d_tilemap_render(const r2d_tilemap_draw *d, bool lit, int target_h);
void r2d_tilemaps_shutdown(void);
/* text.c */
void r2d_fonts_init(void);
void r2d_fonts_shutdown(void);
uint32_t r2d_glyphs_missing(void); /* distinct codepoints no font in a chain had */
/* light.c: a lit run (the scene buffers, the distance field, the lights, the composite) */
void r2d_lit_begin_gl(void);                                   /* bind and clear the scene buffers (GL) */
void r2d_lit_targets(void **scene_tex, void **normal_tex, void **mat_tex, int *scene_wfmt); /* the same, WebGPU textures */
struct fx_dst;
void r2d_lit_end(int layer_lo, int layer_hi, const struct fx_dst *dst); /* lights + composite onto dst */
void r2d_lights_shutdown(void);
/* gi2d.c: global illumination of a lit run (cw_r2d_gi) */
struct fx_rt;
bool r2d_gi_enabled(void); /* the pass asks for it and the effects detail allows it */
void r2d_gi_run(const struct fx_rt *mask, const struct fx_rt *scene, const struct fx_rt *mat, struct fx_rt *light, float light_scale);
void r2d_gi_shutdown(void);
/* wgpu.c: the instanced lights and the occluder mask */
void r2d_wg_mask_draw(void *mask, int w, int h, const float *tris, int nverts, float canvas_w, float canvas_h);
void r2d_wg_lights_draw(void *light, void *add, int fmt, const float amb[4], const float *data, int rows, const float up[4],
                        const float uq[4], const float ut[4], void *normal, void *seed, void *mask, void *mat, int w, int h);
/* shared quad drawing (lights, composite, blit): pos in target pixels */
void r2d_draw_quad_px(float x0, float y0, float x1, float y1, int target_w, int target_h);
void r2d_set_target_fbo(GLuint fbo, int w, int h);
void r2d_upload_layer_uniforms(const r2d_program *p);

/* wgpu.c: the WebGPU backend of the 2D renderer (CW_WGPU; stubs otherwise).
 * The CPU side (submission, sort, batches) is shared; only GPU work differs. */
bool r2d_wg(void); /* this run renders 2D with WebGPU */
bool r2d_wg_init(void);
void r2d_wg_shutdown(void);
int r2d_wg_max_layers(void);
void *r2d_wg_tex_new(int w, int h, int layers);
void *r2d_wg_tex_new_hdr(int w, int h, int layers); /* RGBA16F */
void *r2d_wg_tex_new_levels(int w, int h, int layers, int levels); /* RGBA8 with mip levels */
void r2d_wg_tex_write_level(void *tex, int level, int x, int y, int layer, int w, int h, const uint8_t *px);
void *r2d_wg_depth_new(int w, int h);
void r2d_wg_tex_fill(void *tex, int from, int to, const uint8_t rgba[4]);
void r2d_wg_tex_copy(void *src, void *dst, int layers);
void r2d_wg_tex_write(void *tex, int x, int y, int layer, int w, int h, const uint8_t *px);
void r2d_wg_tex_release(void *tex);
/* cw_target_rt on WebGPU: a pipeline target id drawing into the texture, and its release */
unsigned r2d_wg_target_rt(void *tex, int w, int h);
void r2d_wg_target_rt_free(unsigned id);
/* a render-attachment view of layer 0 of a WebGPU texture, and its release */
void *r2d_wg_layer_view(void *tex);
void r2d_wg_view_release(void *view);
void r2d_wg_array_changed(r2d_array *a);
void r2d_wg_end(const r2d_batch *b, int n, const uint32_t *idx);
/* tilemaps on WebGPU (tilemap.c draws through these inside r2d_wg_end) */
void *r2d_wg_buffer(void *old, const void *data, size_t bytes, bool index); /* (re)fill a static buffer */
void r2d_wg_buffer_release(void *buf);
void r2d_wg_tilemap_begin(const float u[24], int array, bool lit);
void r2d_wg_tilemap_chunk(void *vbuf, int quads);
/* batch.c / light.c helpers the WebGPU backend shares */
void r2d_ensure_rt(cw_texture *rt, int w, int h, cw_filter f, bool depth);
void r2d_yuv_gl_shutdown(void); /* yuv.c: its GL program */
/* post.c: the HDR canvas and the post chain */
bool r2d_post_active(void);          /* this pass has a chain (and the host can draw it) */
int r2d_post_hud_layer(void);        /* first canvas layer drawn after the chain */
void r2d_post_begin_gl(void);        /* bind the HDR canvas (GL), cleared like the pass */
GLuint r2d_post_fbo(void);           /* the HDR canvas framebuffer (GL) */
void *r2d_post_wtex(void);           /* the HDR canvas texture (WebGPU) */
int r2d_post_wfmt(void);             /* its WGPUTextureFormat */
void r2d_post_ensure(void);          /* (re)make the HDR canvas at the pass's size */
struct fx_dst;
void r2d_post_run(const struct fx_dst *out); /* the chain: HDR canvas -> out (premultiplied over) */
void r2d_post_shutdown(void);
bool r2d_canvas_hdr(void);           /* the offscreen canvas is RGBA16F */
bool r2d_dist_ok(void);              /* distortion layers can draw (signed float targets) */
GLuint r2d_post_dist_begin_gl(void); /* bind the distortion buffer (cleared on first use in the pass) */
void *r2d_post_dist_wtex(bool *first); /* the same on WebGPU: the texture, and whether to clear it */
void r2d_screen_copy_gl(GLuint from); /* the offscreen canvas into the screen texture */
void *r2d_screen_wtex(void);         /* the screen texture's WebGPU texture */
/* video frames on WebGPU (yuv.c draws through these) */
void *r2d_wg_yuv_new(const int pw[3], const int ph[3]);
void r2d_wg_yuv_convert(void *y, const uint8_t *const planes[3], void *target_tex, int w, int h);
void r2d_wg_yuv_free(void *y);

#endif
