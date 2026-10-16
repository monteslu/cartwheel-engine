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
#ifndef GL_DEPTH_STENCIL_ATTACHMENT
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#endif
#ifndef GL_STREAM_DRAW
#define GL_STREAM_DRAW 0x88E0
#endif

#define R2D_PAGE 2048       /* atlas page size (GLES 3.0 guarantees 2048) */
#define R2D_NO_ARRAY 0xfffu /* state: no texture (solid), joins any batch */
#define R2D_RING 3

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
  GLuint tex, normal_tex;  /* normal_tex: mirror of the atlas layout, lazily made */
  void *wtex, *wntex;      /* WebGPU: WGPUTexture (colour, normal mirror) */
  void *wbg;               /* WebGPU: cached bind group (colour, sampler, normal-or-dummy) */
  int w, h, layers;
  void *packers;           /* atlas: stbrp_context per layer */
} r2d_array;

typedef struct r2d_tex {
  uint16_t array, page;
  int x, y, w, h;          /* pixel rect inside the page */
  bool y_up;               /* GL orientation (render targets) */
  bool target, has_normal;
  GLuint fbo, depth_rb;
  void *wdepth;            /* WebGPU: depth-stencil texture of a target made with depth */
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

enum { R2D_KIND_STREAM = 0, R2D_KIND_TILEMAP = 1 };
#define R2D_STATE(array, blend, clip, kind) \
  ((uint32_t)(array) | ((uint32_t)(blend) << 12) | ((uint32_t)(clip) << 14) | ((uint32_t)(kind) << 22))
#define R2D_ST_ARRAY(s) ((s) & 0xfffu)
#define R2D_ST_BLEND(s) (((s) >> 12) & 3u)
#define R2D_ST_CLIP(s) (((s) >> 14) & 0xffu)
#define R2D_ST_KIND(s) (((s) >> 22) & 3u)

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

typedef struct r2d_state {
  bool inited, failed;
  /* resources */
  cw_pool textures;  /* r2d_tex */
  CW_ARRAY(r2d_array) arrays;
  int atlas_array[2];   /* per cw_filter, -1 until made */
  GLuint dummy_normal;  /* 1x1x1 "no normal" array */
  int max_layers;
  GLuint blit_read_fbo, blit_draw_fbo;
  /* programs */
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
  float px_x, px_y;
  float win_px_x, win_px_y;   /* the last window pass: drawable pixels per logical window unit */
  cw_texture canvas_rt;       /* virtual canvas */
  cw_texture scene_rt, normal_rt, light_rt; /* lit layers */
  bool normals_used;          /* a normal-mapped texture was drawn into a lit layer */
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

/* text.c: a complex text's glyphs as source byte offsets in visual (left to
 * right) order, every line; the glyph count (0 for simple text). For tests. */
int r2d_text_visual_bytes(cw_font f, const char *text, const cw_text_style *style, int *out, int cap);

/* texture.c */
r2d_tex *r2d_tex_get(cw_texture t);
int r2d_array_new(int w, int h, int layers, bool nearest, bool repeat, bool atlas);
void r2d_array_free(int a);
cw_texture r2d_texture_new_internal(int w, int h, cw_filter filter, bool depth); /* a render target */
void r2d_textures_shutdown(void);
GLuint r2d_array_normal_tex(int a);
/* UV rect of a source rect (texture pixels) in page UVs, honouring y_up */
void r2d_tex_uv(const r2d_tex *t, cw_rect src, float *u0, float *v0, float *u1, float *v1);
void r2d_tex_uv_point(const r2d_tex *t, float sx, float sy, float *u, float *v);
/* batch.c */
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
/* light.c */
void r2d_lights_render(int layer_lo, int layer_hi);
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
void *r2d_wg_depth_new(int w, int h);
void r2d_wg_tex_fill(void *tex, int from, int to, const uint8_t rgba[4]);
void r2d_wg_tex_copy(void *src, void *dst, int layers);
void r2d_wg_tex_write(void *tex, int x, int y, int layer, int w, int h, const uint8_t *px);
void r2d_wg_tex_release(void *tex);
void r2d_wg_array_changed(r2d_array *a);
typedef struct r2d_batch {
  uint32_t state, first, count, payload;
  int layer;
  bool lit;
} r2d_batch;
void r2d_wg_end(const r2d_batch *b, int n, const uint32_t *idx);
/* tilemaps on WebGPU (tilemap.c draws through these inside r2d_wg_end) */
void *r2d_wg_buffer(void *old, const void *data, size_t bytes, bool index); /* (re)fill a static buffer */
void r2d_wg_buffer_release(void *buf);
void r2d_wg_tilemap_begin(const float u[24], int array, bool lit);
void r2d_wg_tilemap_chunk(void *vbuf, int quads);
/* batch.c / light.c helpers the WebGPU backend shares */
void r2d_ensure_rt(cw_texture *rt, int w, int h, cw_filter f, bool depth);
void r2d_yuv_gl_shutdown(void); /* yuv.c: its GL program */
/* video frames on WebGPU (yuv.c draws through these) */
void *r2d_wg_yuv_new(const int pw[3], const int ph[3]);
void r2d_wg_yuv_convert(void *y, const uint8_t *const planes[3], void *target_tex, int w, int h);
void r2d_wg_yuv_free(void *y);
int r2d_shadow_tris(int lo, int hi, cw_vec2 L, float reach, float **out);

#endif
