/* cartwheel 2D renderer: a batched, immediate-mode-style submission API.
 *
 * Every frame, between cw_r2d_begin() and cw_r2d_end(), the game submits
 * sprites, shapes, text, tilemaps and lights into canvas LAYERS. Nothing is
 * drawn while submitting: cw_r2d_end() sorts what was submitted (layer, then
 * z, then y when the layer y-sorts, then submission order; the sort is
 * stable), cuts it into batches and draws. A batch only ends where the GL
 * state has to change: a different texture ARRAY, blend mode, clip rect or
 * shader (tilemaps, lit layers). Every texture lives in a GL_TEXTURE_2D_ARRAY
 * page, so sprites from many atlas pages share one draw, and solid shapes,
 * SDF text and additive sprites batch with textured sprites (the fragment
 * shader branches on a per-vertex mode). A typical frame is a handful of
 * draws.
 *
 * Coordinates: canvas pixels, origin top-left, y down. Angles in radians,
 * positive = clockwise on screen. Colours are straight (not premultiplied)
 * 0..1 floats; the renderer premultiplies internally. All textures are stored
 * premultiplied, and blending is premultiplied-alpha throughout.
 *
 * GL: OpenGL ES 3.0 / WebGL2 through cartwheel/gl.h, the same code natively
 * and in carts. The renderer needs a current context (call from init()
 * onward) and owns no context state between passes: each cw_r2d_end() sets
 * everything it uses (framebuffer, viewport, program, VAO, blend, scissor,
 * stencil, depth and cull off, texture unit 0..2) and leaves those changed.
 * Sharing the context with three.c: call t3_renderer_reset_bindings(r)
 * after any 2D pass and before the next t3_renderer_render(), so three.c's
 * GL state cache matches GL again (the 2D renderer never touches three.c's
 * programs, so the cheaper reset is enough). Draw three.c INTO a 2D render
 * target with t3_renderer_set_framebuffer(r, cw_target_fbo(rt)) plus
 * t3_renderer_set_size(r, w, h); the target then draws as a sprite.
 *
 * Costs: submission is O(vertices) on the calling thread (cw_r2d_sprites
 * spreads big arrays over the job pool); cw_r2d_end() is one radix sort when
 * the submission is not already in order, one upload per stream (vertices,
 * indices; orphaned each pass, a ring of 3 buffers), and one draw call per
 * batch. Memory is tagged CW_MEM_RENDER2D.
 *
 * Threads: call everything from the main (GL) thread. */
#ifndef CW_RENDER2D_H
#define CW_RENDER2D_H

#include "base.h"
#include "math.h"

/* What this render2d offers, for code that builds against older SDKs too:
 *   1  sprites, shapes, text, tilemaps, lit layers with stencil shadows
 *   2  HDR and the post chain, intensity, emission, material maps, distance
 *      field shadows (soft, spot, directional, volumetric), occluder_depth,
 *      global illumination, screen reads, distortion, water, wind, trails,
 *      layer fog, flash, mipmapped atlas pages (engine v0.37)
 *   #if defined(CW_R2D_FEATURES) && CW_R2D_FEATURES >= 2 */
#define CW_R2D_FEATURES 2

#ifdef __cplusplus
extern "C" {
#endif

/* ── value types ─────────────────────────────────────────────────────── */

typedef struct cw_color { float r, g, b, a; } cw_color; /* straight alpha, 0..1 */
/* straight-alpha colour, components 0..1 */
static inline cw_color cw_rgba(float r, float g, float b, float a) { return (cw_color){ r, g, b, a }; }
/* 0xRRGGBBAA */
static inline cw_color cw_hex(uint32_t c) {
  return (cw_color){ (float)(c >> 24) / 255.0f, (float)((c >> 16) & 255) / 255.0f, (float)((c >> 8) & 255) / 255.0f,
                     (float)(c & 255) / 255.0f };
}
#define CW_WHITE ((cw_color){ 1, 1, 1, 1 })
#define CW_BLACK ((cw_color){ 0, 0, 0, 1 })

typedef struct cw_rect { float x, y, w, h; } cw_rect;
/* x, y = top-left; w, h in the same units */
static inline cw_rect cw_rect_make(float x, float y, float w, float h) { return (cw_rect){ x, y, w, h }; }

/* 2D affine transform: x' = a*x + c*y + tx, y' = b*x + d*y + ty. */
typedef struct cw_xform2d { float a, b, c, d, tx, ty; } cw_xform2d;
/* a = d = 1, the rest 0 */
static inline cw_xform2d cw_xform2d_identity(void) { return (cw_xform2d){ 1, 0, 0, 1, 0, 0 }; }
/* transform a point (translation included) */
static inline cw_vec2 cw_xform2d_apply(const cw_xform2d *m, cw_vec2 p) {
  return (cw_vec2){ m->a * p.x + m->c * p.y + m->tx, m->b * p.x + m->d * p.y + m->ty };
}
/* m * n: apply n first, then m. */
cw_xform2d cw_xform2d_mul(const cw_xform2d *m, const cw_xform2d *n);
cw_xform2d cw_xform2d_invert(const cw_xform2d *m); /* identity when m is singular (determinant 0) */
/* translate(pos) * rotate(rot) * scale(scale): the usual node transform */
cw_xform2d cw_xform2d_trs(cw_vec2 pos, float rot, cw_vec2 scale);

/* ── module ──────────────────────────────────────────────────────────── */

/* Compiles the shaders and creates the streams. Every other call initializes
 * lazily, so calling this is optional; it returns false (and logs why) when
 * a shader fails to build. */
bool cw_r2d_init(void);
/* Frees every texture, font, tilemap and GL object the renderer made. */
void cw_r2d_shutdown(void);

/* ── textures ────────────────────────────────────────────────────────────
 * A cw_texture is a rectangle inside a GL_TEXTURE_2D_ARRAY. Loose images are
 * packed at runtime into shared 2048x2048 atlas pages (one array per filter
 * mode, grown a page at a time), with a 1-pixel extruded border so linear
 * filtering never bleeds a neighbour in. Draws of anything in the same array
 * batch together. Standalone textures (repeat wrap, images bigger than a
 * page, render targets) get an array of their own. */
CW_HANDLE_TYPE(cw_texture);

typedef enum { CW_FILTER_LINEAR, CW_FILTER_NEAREST } cw_filter;

typedef struct cw_texture_desc {
  cw_filter filter;   /* NEAREST for pixel art */
  bool standalone;    /* own array instead of an atlas page */
  bool repeat;        /* GL_REPEAT wrap (implies standalone): UVs past 1 tile */
  bool premultiplied; /* pixels are already premultiplied: skip conversion */
  /* LINEAR textures drawn smaller than their pixels (HD art on a 720p screen,
   * a zoomed-out camera, a far parallax layer): 3 smaller copies (half,
   * quarter, eighth size, made on the CPU at load with a box filter) let the
   * GPU blend the nearest two, so the art stays smooth instead of shimmering.
   * These textures pack into atlas pages of their own with 8-pixel gutters;
   * a third more memory. Ignored for NEAREST (pixel art keeps its pixels). */
  bool mipmaps;
} cw_texture_desc;

/* PNG / JPEG from the asset bundle (cw_asset_load), decoded by stb_image to
 * RGBA8 and premultiplied. desc may be NULL (linear, atlas). A missing or
 * undecodable file logs the path and returns the null handle; drawing a null
 * handle draws a solid colour. */
cw_texture cw_texture_load(const char *asset_path, const cw_texture_desc *desc);
/* PNG / JPEG bytes (copied; caller keeps data); null handle (logged) when undecodable */
cw_texture cw_texture_from_memory(const void *data, size_t len, const cw_texture_desc *desc);
/* w*h RGBA8, row 0 = top, straight alpha unless desc->premultiplied. Copied. */
cw_texture cw_texture_from_pixels(int w, int h, const uint8_t *rgba, const cw_texture_desc *desc);
/* Replace the pixels (same size and alpha convention as creation). */
void cw_texture_update(cw_texture t, const uint8_t *rgba);
/* Attach a tangent-space normal map (w*h RGBA8 matching the texture's size,
 * OpenGL convention: +y up in the image is green). Lit layers shade sprites
 * of this texture with it; without one a sprite is lit flat. Atlas textures
 * only. Returns false (logged) on a size mismatch. */
bool cw_texture_set_normal_map(cw_texture t, const uint8_t *rgba);
/* as cw_texture_set_normal_map from a PNG asset; must decode to the texture's size */
bool cw_texture_load_normal_map(cw_texture t, const char *asset_path);
/* Attach a material map (w*h RGBA8 matching the texture's size): per texel
 *   R  emission: the share of the colour that ignores the 2D lights (a lit
 *      window, a glowing rune; as cw_r2d_set_emissive, per texel)
 *   G  specular: how strongly the lights glint off it (0 = matte, as cloth
 *      and stone are; 1 = wet rock, metal, glass)
 *   B  gloss: how tight the glint is (0 = broad sheen, 1 = a sharp spot)
 * (A is unused.) Lit layers shade sprites and tiles of this texture with it.
 * Atlas textures only. Returns false (logged) on a size mismatch. */
bool cw_texture_set_material_map(cw_texture t, const uint8_t *rgba);
/* as cw_texture_set_material_map from a PNG asset; must decode to the texture's size */
bool cw_texture_load_material_map(cw_texture t, const char *asset_path);
/* pixels; 0 x 0 for a null or destroyed handle; w or h may be NULL */
void cw_texture_size(cw_texture t, int *w, int *h);
bool cw_texture_valid(cw_texture t); /* false for a null or destroyed handle */
/* Atlas space is not reclaimed (pages only grow); standalone arrays and
 * targets are freed. */
void cw_texture_destroy(cw_texture t);

/* ── render targets ──────────────────────────────────────────────────────
 * A target is a standalone texture you can draw into (cw_r2d_begin with
 * .target) and then draw as a sprite. Content follows GL's orientation
 * (row 0 = bottom); the renderer flips it back when sampling, so a target
 * drawn as a sprite shows what was drawn into it upright, whether a 2D pass
 * or three.c filled it. */
typedef struct cw_target_desc {
  int width, height;
  cw_filter filter;
  bool depth; /* depth24 + stencil8 renderbuffer (three.c needs one) */
} cw_target_desc;
/* Create a render target (free with cw_texture_destroy). Null handle (logged) for a non-positive size or no GL
 * context. */
cw_texture cw_target_create(const cw_target_desc *desc);
/* The framebuffer object, for t3_renderer_set_framebuffer. */
unsigned cw_target_fbo(cw_texture t);
/* Reallocates (contents lost). */
void cw_target_resize(cw_texture t, int width, int height);
struct cw_rt; /* cartwheel/pipeline.h */
/* The target as a 3D pipeline output, on either GPU API: cw_pipeline_set_output(p,
 * cw_target_rt(t)) and cw_pipeline_render draw the scene into it, and the target then
 * draws as a sprite or a UI image (cw_ui_draw_texture) the right way up. NULL for a
 * handle that is not a target. Owned by the target: valid until it is resized or
 * destroyed (ask again after cw_target_resize). (cw_target_fbo is GL only.) */
const struct cw_rt *cw_target_rt(cw_texture t);

/* ── cameras ─────────────────────────────────────────────────────────────
 * A plain value the game owns. The view maps world -> canvas pixels:
 *   screen = anchor + offset + R(-rotation) * ((world - position*parallax) * zoom)
 * where anchor is the viewport's centre (or its top-left corner with
 * anchor_top_left). Zero-initialized fields mean zoom 1, no rotation. */
typedef struct cw_camera2d {
  cw_vec2 position;     /* world point shown at the anchor */
  float zoom;           /* >1 magnifies; 0 = 1 */
  float rotation;       /* radians: the WORLD appears rotated by -rotation */
  cw_vec2 offset;       /* extra canvas-pixel offset of the anchor (shake) */
  bool anchor_top_left; /* position sits at the viewport's top-left, not its centre */
  bool use_limits;      /* keep the view inside `limits` (world rect) */
  cw_rect limits;
} cw_camera2d;

/* World -> canvas transform for a viewport (canvas pixels) and a parallax
 * factor ((0,0) = (1,1); (0.5, 0.5) scrolls at half speed). */
cw_xform2d cw_camera2d_view(const cw_camera2d *cam, cw_rect viewport, cw_vec2 parallax);
/* world -> canvas pixels (parallax 1) */
cw_vec2 cw_camera2d_world_to_screen(const cw_camera2d *cam, cw_rect viewport, cw_vec2 world);
/* canvas pixels -> world (parallax 1) */
cw_vec2 cw_camera2d_screen_to_world(const cw_camera2d *cam, cw_rect viewport, cw_vec2 screen);
/* Exponential smoothing toward target: position moves 1 - e^(-speed*dt) of
 * the way per call, frame-rate independent and deterministic (cw_exp).
 * speed <= 0 snaps. Call cw_camera2d_apply_limits after it to keep limits. */
void cw_camera2d_follow(cw_camera2d *cam, cw_vec2 target, float speed, float dt);
/* Clamp position so the view (of viewport size) stays inside limits. */
void cw_camera2d_apply_limits(cw_camera2d *cam, cw_rect viewport);

/* ── passes ──────────────────────────────────────────────────────────────
 * One pass draws into one target. Passes do not nest: finish a target's pass
 * before drawing it in another. */
typedef enum {
  CW_SCALE_NONE,    /* canvas = the target's pixels */
  CW_SCALE_INTEGER, /* virtual canvas, scaled by the largest whole factor that fits, centred (pixel-perfect) */
  CW_SCALE_FIT,     /* virtual canvas, scaled to fit keeping aspect, centred */
  CW_SCALE_STRETCH, /* virtual canvas stretched over the whole window */
  CW_SCALE_EXPAND,  /* FIT's scale over the whole window: the canvas grows to the window's aspect
                     * instead of bars (a 320x180 canvas is 240x180 on 4:3, 432x180 on 21:9;
                     * cw_r2d_canvas_size says); Camera2D limits still hold */
} cw_scale_mode;

/* ── HDR and the post chain ─────────────────────────────────────────────
 * A pass with .post draws its canvas layers below post->hud_layer into a
 * high dynamic range canvas (RGBA16F where the host renders it; RGBA8 on a
 * GL host without float targets, where nothing passes 1), so colour can go
 * past 1: cw_r2d_set_intensity, lights brighter than 2x, additive glows.
 * Then the chain turns that canvas into the pass's output:
 *
 *   exposure  x exposure
 *   bloom     what is brighter than bloom_threshold (with a soft knee) is
 *             blurred over bloom_levels halvings (a 13-tap filtered
 *             downsample, a tent upsample) and added back x bloom_intensity
 *   tone      SOFT (default): values up to tone_knee stay exactly as drawn
 *             (art at 0..1 is untouched), above it they roll off smoothly
 *             toward 1, each channel on its own, so hot colours go to white;
 *             ACES (a filmic fit), REINHARD, CLAMP
 *   grade     contrast and saturation around mid grey, lift (raises the
 *             darks), gain (scales), gamma, tint (multiplies)
 *   vignette  darkens the corners by vignette (0..1)
 *   dither    the display value gets less than a level of noise, so smooth
 *             HDR gradients do not band in 8 bits
 *
 * Layers from hud_layer up draw after the chain, straight into the output:
 * a HUD over a glowing world. Each pass has its own chain, so two regions
 * (an underwater view and a sky) can be graded differently. Zero fields
 * take the defaults in the comments; a NULL post draws as before, exactly.
 *
 * Quality (cartwheel/quality.h effects detail): Very Low draws no bloom,
 * Low at most 3 levels, Medium at most 5, High and Ultra as asked. The
 * grade and the tone curve hold on every preset.
 *
 * Cost (890M, 1080p): about 0.3 ms with 6 bloom levels; 14 full-screen
 * passes, about 100 WebGPU calls. */
typedef enum { CW_R2D_TONE_SOFT, CW_R2D_TONE_ACES, CW_R2D_TONE_REINHARD, CW_R2D_TONE_CLAMP } cw_r2d_tone;
typedef struct cw_r2d_post {
  int hud_layer;          /* canvas layers >= this draw after the chain (0 = every layer goes through it) */
  float exposure;         /* 0 = 1 */
  bool bloom;
  float bloom_threshold;  /* 0 = 1: what passes it blooms */
  float bloom_knee;       /* 0 = 0.5: soft transition, a share of the threshold */
  float bloom_intensity;  /* 0 = 0.6 */
  float bloom_radius;     /* 0 = 1: the upsample tent's spread, in texels of each level */
  int bloom_levels;       /* 0 = 6 (1..8) */
  cw_r2d_tone tone;
  float tone_knee;        /* SOFT: 0 = 0.75; values under it are unchanged */
  float contrast;         /* 0 = 1 */
  float saturation;       /* 0 = 1 (negative: none at all) */
  float gamma;            /* 0 = 1 */
  cw_color lift;          /* rgb added to the darks (fades out toward white); zero = none */
  cw_color gain;          /* rgb multiplier; zero = white */
  cw_color tint;          /* rgb multiplier after the grade; zero = white */
  float vignette;         /* 0..1 darkening at the corners; 0 = none */
  float vignette_softness;/* 0 = 0.5 */
  bool no_dither;         /* leave out the dither (a test comparing exact values) */
} cw_r2d_post;

/* ── global illumination ────────────────────────────────────────────────
 * A pass with .gi lights its lit layers with the light that bounces round
 * the scene, not only the lights': what glows (cw_r2d_set_emissive, material
 * maps' emission) lights what is near it in its own colour, light that falls
 * on an occluder bounces off it in the occluder's colour, and occluders cast
 * soft shadows from all of it. No light needs to be placed for any of it.
 *
 * How: radiance cascades (Sannikov, 2023) over a distance field of the
 * occluders and the glowing pixels, at a share of the canvas size. Cascade N
 * keeps a probe every 2^(N+1) field pixels with 4^(N+1) directions, each
 * marching an interval four times longer than the one below; merged from the
 * top down they give every pixel the light arriving from all round, with
 * about 5 cascades at 1080p. Each frame's result feeds the next frame's
 * bounce, so light keeps bouncing as long as the scene stands, fading by
 * `bounce` each time.
 *
 * Quality (effects detail): off on Very Low, the field at 1/8 of the canvas
 * on Low, 1/4 on Medium and High (or `resolution`), 1/2 on Ultra.
 * Cost (890M, 1080p, High): about 0.6 ms, 25 full-screen passes. */
typedef struct cw_r2d_gi {
  float intensity;      /* 0 = 1: multiplies the bounced light */
  float bounce;         /* the share of the light on an occluder that it gives back: 0 = 0.5, negative = none */
  float resolution;     /* High and Ultra: the field's share of the canvas (0 = the preset's); smaller is cheaper and softer */
} cw_r2d_gi;

/* Wind: sprites submitted after cw_r2d_set_sway(share) bend their top edge
 * sideways in the vertex stage (grass, reeds, banners, hanging vines,
 * treetops): up to strength x share canvas pixels, in waves `wavelength`
 * layer units long rolling by at `speed` per second, roughened by `gusts`.
 * Driven by the pass's time, so pass a tick-derived time for replays. Costs
 * nothing on the CPU and nothing for sprites that do not sway. */
typedef struct cw_r2d_wind {
  float strength;   /* canvas pixels at share 1; 0 = no wind */
  float speed;      /* waves per second: 0 = 1.5 */
  float wavelength; /* layer units between wave crests: 0 = 300 */
  float gusts;      /* 0..1: a second, faster wave on top */
} cw_r2d_wind;

typedef struct cw_r2d_pass_desc {
  cw_texture target;    /* null = the window (framebuffer 0) */
  bool clear;
  cw_color clear_color;
  /* window passes: draw at virtual_width x virtual_height (nearest-filtered
   * canvas) and scale it to the window by `scale`; 0 = no virtual canvas */
  int virtual_width, virtual_height;
  cw_scale_mode scale;
  cw_color letterbox;   /* bars around a scaled canvas (alpha forced to 1) */
  float time;           /* seconds: animated tiles (pass a tick-derived time for replays) */
  cw_color ambient;     /* lit layers: light where no light reaches; all 0 = black */
  const cw_r2d_post *post; /* HDR canvas and post chain (above); NULL = none. Copied at cw_r2d_begin */
  const cw_r2d_gi *gi;     /* global illumination of the lit layers (above); NULL = none. Copied */
  cw_r2d_wind wind;        /* what sways sprites submitted with cw_r2d_set_sway; zero = still air */
} cw_r2d_pass_desc;

/* Start a pass (desc NULL = window, no clear). Resets layer, z, blend, transform and clips; layer descs persist
 * across passes. Asserts when a pass is open; an invalid .target logs and falls back to the window. */
void cw_r2d_begin(const cw_r2d_pass_desc *desc);
/* Sorts, batches and draws everything submitted since cw_r2d_begin. */
void cw_r2d_end(void);
/* Canvas size of the current pass: the virtual size, the target's size, or
 * for a plain window pass the window's LOGICAL size (cw_window_size), which
 * the pass scales to the drawable (cw_app_desc.render_scale, display
 * density): draw in logical units, it rasterizes at full resolution. */
cw_vec2 cw_r2d_canvas_size(void);
/* Window coordinates (logical, as the platform reports the pointer) ->
 * canvas coordinates for the last window pass (undoes virtual canvas
 * scaling and letterboxing; identity for a plain window pass). Valid after a
 * window pass began; a render-target pass since then does not change the
 * answer. */
cw_vec2 cw_r2d_window_to_canvas(cw_vec2 window_px);
cw_vec2 cw_r2d_canvas_to_window(cw_vec2 canvas_px); /* inverse of cw_r2d_window_to_canvas */
/* Where the scaled canvas lands in the window, in drawable pixels. */
cw_rect cw_r2d_canvas_rect(void);
/* Drawable pixels per canvas unit of the current pass (1 for render targets
 * and virtual canvases; the pixel ratio for a plain window pass). */
float cw_r2d_pixel_scale(void);

/* ── canvas layers ───────────────────────────────────────────────────────
 * CW_R2D_MAX_LAYERS layers, drawn in index order (0 first). Each has its own
 * camera, so a world layer and a HUD layer coexist; settings persist across
 * passes and frames (a scene tree's canvas draw puts back those it used).
 * Layer 0 with no camera is the default: screen space. */
#define CW_R2D_MAX_LAYERS 32

typedef struct cw_r2d_layer_desc {
  bool has_camera;      /* false = screen space (identity) */
  cw_camera2d camera;
  cw_vec2 parallax;     /* camera position factor; (0,0) = (1,1) */
  cw_rect viewport;     /* canvas pixels; w == 0 = the whole canvas. Draws are clipped to it. */
  bool y_sort;          /* within equal z: sort by y (sprite pivot / shape anchor), lower first */
  bool pixel_snap;      /* round transformed vertices to whole canvas pixels */
  bool lit;             /* drawn through the 2D light pass (see lights) */
  /* lit layers: how deep light reaches into occluders (layer units), so solid
   * ground and walls glow at the edge a light falls on and stay dark inside.
   * 0 = all the way: an occluder is lit across its whole face. The run of
   * lit layers takes its first layer's value */
  float occluder_depth;
  /* the canvas as drawn before this layer (the layers under it, lit runs
   * composited) is copied first, for its draws to sample through
   * cw_r2d_screen_texture(): water, glass, frost, a mirror. A pass with such
   * a layer draws offscreen (the HDR canvas with .post, else an 8-bit one)
   * and is copied to its target at the end; layers from post->hud_layer up
   * read nothing. A lit layer here splits the run of lit layers it is in. */
  bool screen_read;
  /* > 0: a distortion layer. Its draws bend the picture instead of drawing
   * colour: each pixel's red pushes the canvas right (0.5 = none, 1 = the
   * full `distortion` canvas pixels, 0 = as far left), green pushes it down,
   * both times its alpha; overlapping draws add up. Heat haze (scroll
   * cw_r2d_noise_texture()), shockwaves (cw_r2d_shockwave), refraction under
   * water. Applied once, to the whole offscreen canvas, at the end of the
   * layers the chain covers (so draw these just under the HUD); lit and
   * tilemaps are ignored here. Needs float targets (WebGPU, and GL hosts
   * with float render targets: cw_pipeline_caps); elsewhere nothing is bent. */
  float distortion;
  /* atmospheric depth: the layer's draws turn toward fog.rgb by fog.a (0..1)
   * in the fragment stage, so far parallax layers fade into the haze (zero =
   * none; unlit layers only: lit layers take it from their lights) */
  cw_color fog;
} cw_r2d_layer_desc;

/* The canvas as it stood before the latest screen_read layer, as a texture
 * of the canvas size (draw it with UVs 0..1 over the canvas, top-left
 * first, as any texture: cw_r2d_quad, cw_r2d_triangles, a sprite). The handle
 * is the same from pass to pass while the canvas size holds; call it any time
 * during submission. Its content is the CURRENT pass's once that layer's
 * draws run. */
cw_texture cw_r2d_screen_texture(void);
/* A 256x256 tileable texture of smooth noise in red and green (0.5 = no
 * push), repeat-wrapped: scroll its UVs in a distortion layer for heat haze or
 * a wobble under water. */
cw_texture cw_r2d_noise_texture(void);
/* A shockwave ring for a distortion layer: pushes the canvas outward across
 * a ring `width` wide centred on `radius` (layer space), strongest mid-ring,
 * by `strength` (0..1 of the layer's distortion). One submission. */
void cw_r2d_shockwave(cw_vec2 center, float radius, float width, float strength);

/* ── GPU particles ──────────────────────────────────────────────────────
 * An emitter of count particles that the GPU draws in one instanced draw
 * with no state anywhere: particle i is a pure function of the time (its own
 * phase and life; each life a fresh spawn point in the extent, direction
 * within spread of angle, speed, spin, from a hash of i, the seed and the
 * life's number), moving in closed form under gravity and linear drag, with
 * an optional wobble. No CPU work per particle, nothing to upload: a million
 * sparks cost their pixels. They sort, blend (CW_BLEND_ADD for fire and
 * sparks), take intensity and emission, and are lit in lit layers like
 * sprites. Deterministic for a given time (pass a tick-derived one).
 * Positions are layer space; the current transform moves the emitter (not
 * the direction). Use the scene's GPUParticles2D (CPU, collisions, sub
 * emitters) for gameplay particles; these are for the volume of a scene's
 * ambience: embers, rain, snow, dust motes, fireflies, fountains. */
typedef struct cw_r2d_particles_desc {
  int count;                  /* particles alive at once */
  uint32_t seed;              /* 24 bits: different seeds, different particles */
  float time;                 /* seconds; 0 = the pass's time */
  cw_vec2 pos;                /* the emitter, layer space */
  cw_vec2 extent;             /* half size of the spawn box */
  float angle, spread;        /* launch direction (radians clockwise from +x) and half-angle */
  float speed_min, speed_max; /* layer units a second (max 0 = min) */
  cw_vec2 gravity;            /* layer units a second squared (+y down) */
  float drag;                 /* 1 / s: speed lost (0 = none) */
  float life_min, life_max;   /* seconds (0 = 1; max 0 = min) */
  float size_start, size_end; /* layer units (start 0 = 8) */
  cw_color color_start;       /* zero = white */
  cw_color color_end;         /* zero = the start colour fading out */
  float spin;                 /* each particle turns at up to this, radians a second, either way */
  float turbulence;           /* layer units of wobble, growing with age */
  cw_texture texture;         /* null = solid squares */
  cw_rect src;                /* texture pixels; w 0 = the whole texture */
} cw_r2d_particles_desc;
void cw_r2d_particles(const cw_r2d_particles_desc *d); /* one emitter into the current layer and z */

/* ── 2D water ────────────────────────────────────────────────────────────
 * A surface that ripples where things splash in it (cw_water2d: springs per
 * column, stepped on the CPU in fixed_update, deterministic), and a body
 * drawn from the screen texture: the world behind it seen through the water
 * and wobbling (refraction), the world above it mirrored on it and fading
 * with depth (reflection), tinted deeper toward the bottom, with caustics
 * (moving filaments of light) and a lit surface line. Draw it into a layer
 * with screen_read and no camera transform (the screen texture is the canvas
 * as drawn); add a distortion layer with cw_r2d_noise_texture over it for
 * per-pixel ripples. All of it is ordinary submissions: a few draws. */
typedef struct cw_water2d cw_water2d;
typedef struct cw_water2d_desc {
  int columns;      /* springs across the surface: 0 = 64 */
  float width;      /* layer units the columns span: 0 = 256 */
  float tension;    /* how fast a column springs back: 0 = 40 */
  float damping;    /* 0 = 2.5 */
  float spread;     /* how much neighbours pull each other (the wave's travel): 0 = 0.25 */
} cw_water2d_desc;
cw_water2d *cw_water2d_new(const cw_water2d_desc *d); /* a still surface */
void cw_water2d_free(cw_water2d *w);                  /* NULL is fine */
void cw_water2d_step(cw_water2d *w, float dt);        /* advance the springs (call with the fixed dt) */
/* push the surface at x (layer units from its left end) up (+) or down (-) at this speed */
void cw_water2d_splash(cw_water2d *w, float x, float speed);
float cw_water2d_height(const cw_water2d *w, float x); /* the surface's height above rest at x (+ up) */
int cw_water2d_columns(const cw_water2d *w, const float **heights); /* the column count; *heights = each column's height */
/* A 256x256 tileable texture of bright caustic filaments (alpha = the light), repeat-wrapped. */
cw_texture cw_r2d_caustics_texture(void);
typedef struct cw_r2d_water_desc {
  cw_rect area;            /* the body, canvas units; its top edge is the surface at rest */
  const cw_water2d *surface; /* NULL: gentle waves only */
  float time;              /* seconds: waves and caustics move with it */
  float wave_height;       /* units of the gentle waves (0 = none) */
  cw_color tint;           /* the deep colour; alpha = how much it hides what is behind (zero = a clear blue-green) */
  float refraction;        /* units the world behind wobbles (0 = none) */
  float reflection;        /* 0..1: the mirrored world at the surface */
  float caustics;          /* 0..1: light filaments on the body */
  float surface_glow;      /* 0..1: the lit surface line */
} cw_r2d_water_desc;
void cw_r2d_water(const cw_r2d_water_desc *d); /* the body and its surface into the current layer */

/* layer 0..CW_R2D_MAX_LAYERS-1 (asserts); NULL resets it; persists until changed */
void cw_r2d_layer_set(int layer, const cw_r2d_layer_desc *desc);
const cw_r2d_layer_desc *cw_r2d_layer_get(int layer); /* pointer into the renderer's table; asserts on a bad index */
/* Update only the camera (has_camera becomes true). */
void cw_r2d_layer_set_camera(int layer, const cw_camera2d *cam);
/* Subsequent submissions go into this layer (default 0). */
void cw_r2d_use_layer(int layer);
/* The layer's world <-> canvas mapping in the current (or last) pass. */
cw_vec2 cw_r2d_layer_screen_to_world(int layer, cw_vec2 canvas_px);
cw_vec2 cw_r2d_layer_world_to_screen(int layer, cw_vec2 world); /* world -> canvas pixels */

/* ── draw state (applies to submissions that follow, reset by begin) ──── */
typedef enum {
  CW_BLEND_ALPHA,    /* premultiplied over */
  CW_BLEND_ADD,      /* additive; batches with ALPHA (no state change) */
  CW_BLEND_MULTIPLY, /* dst * src (own batch) */
  CW_BLEND_OPAQUE,   /* replace (own batch) */
} cw_blend;
void cw_r2d_set_blend(cw_blend b); /* for later submissions until the pass ends (cw_r2d_begin resets to ALPHA) */
/* z within a layer (higher draws later); sprites add their own z. -32768..32767 */
void cw_r2d_set_z(int z);
/* Local transform applied before the layer camera to everything submitted
 * (sprites, shapes, text): what the node layer pushes. NULL = identity. */
void cw_r2d_set_transform(const cw_xform2d *m);
/* HDR intensity of later submissions (sprites, shapes, text, geometry):
 * their colour is multiplied by k (1..16) before blending, so in a pass
 * with .post a glowing thing goes past 1 and blooms. Without .post the
 * result clamps at 1. Reset to 1 by cw_r2d_begin. */
void cw_r2d_set_intensity(float k);
/* Emission of later submissions in lit layers: the share (0..1) of their
 * colour that ignores the 2D lights and ambient (1 = drawn as if fully lit:
 * a lamp, a screen, lava). 0 by default; reset by cw_r2d_begin. */
void cw_r2d_set_emissive(float share);
/* How much later sprites' top edges sway in the pass's wind (0..1, see
 * cw_r2d_wind); 0 by default, reset by cw_r2d_begin. Sprites only. */
void cw_r2d_set_sway(float share);
/* Flash later sprites and shapes toward their colour by amount (0..1),
 * keeping their shape: a white sprite colour at 1 is a white silhouette (a
 * hit flash), red at 0.5 a red tint over the art. Reset by cw_r2d_begin. */
void cw_r2d_set_flash(float amount);
/* Later sprites, shapes, text and tilemaps in lit layers also cast shadows
 * (and block global illumination) wherever their alpha is over half: the
 * ground tiles, a crate, a character, with no occluder polygon. They fill
 * the occluder mask at the canvas size, as polygons do. Off by default; reset
 * by cw_r2d_begin. */
void cw_r2d_set_occluder(bool on);
/* Clip to a canvas-pixel rect (intersected with the enclosing clip). Max
 * depth 32; up to 254 distinct clips per pass. */
void cw_r2d_push_clip(cw_rect canvas_rect);
void cw_r2d_pop_clip(void); /* asserts without a matching push */

/* ── sprites ─────────────────────────────────────────────────────────── */
typedef struct cw_sprite {
  cw_texture texture; /* null = solid colour rectangle */
  cw_rect src;        /* texture pixels (atlas region); w == 0 = whole texture */
  cw_vec2 pos;        /* where the pivot lands (layer space) */
  cw_vec2 size;       /* drawn size; 0 = src size */
  cw_vec2 scale;      /* (0,0) = (1,1) */
  cw_vec2 pivot;      /* 0..1 inside the sprite, (0,0) = top-left, (0.5,0.5) = centre */
  float rotation;     /* radians clockwise, about the pivot */
  cw_color color;     /* modulate; {0,0,0,0} (zero-init) = white */
  bool flip_x, flip_y;
  int z;              /* added to cw_r2d_set_z */
} cw_sprite;

/* A null texture draws a solid rect of the sprite's size and colour. Cost: one quad appended. */
void cw_r2d_sprite(const cw_sprite *s);
/* n sprites at once; big arrays fill vertices in parallel on the job pool.
 * Same result as n cw_r2d_sprite calls. */
void cw_r2d_sprites(const cw_sprite *s, int n);

/* Nine-slice: the corners keep their size, edges stretch one way, the centre
 * both. Margins are texture pixels; `margin_scale` scales them on screen
 * (0 = 1). */
typedef struct cw_nine_slice {
  cw_texture texture;
  cw_rect src;                    /* w == 0 = whole texture */
  float left, top, right, bottom; /* margins, texture pixels */
  cw_rect dst;
  cw_color color;                 /* {0,0,0,0} = white */
  float margin_scale;
  bool hollow;                    /* skip the centre */
} cw_nine_slice;
void cw_r2d_nine_slice(const cw_nine_slice *n); /* margins shrink to fit dst; an invalid texture draws a solid rect */

/* ── shapes (solid colour; batch with sprites) ──────────────────────── */
void cw_r2d_rect(cw_rect r, cw_color c);
void cw_r2d_rect_lines(cw_rect r, float width, cw_color c); /* stroke inside the rect */
/* width in canvas pixels; zero-length lines draw nothing */
void cw_r2d_line(cw_vec2 a, cw_vec2 b, float width, cw_color c);
/* Mitred joins (bevelled past a 4x miter), butt caps; closed joins the ends. */
void cw_r2d_polyline(const cw_vec2 *pts, int n, float width, bool closed, cw_color c);
/* segments <= 0 picks by radius */
void cw_r2d_circle(cw_vec2 center, float radius, int segments, cw_color c);
/* ring of the given width centred on radius */
void cw_r2d_circle_lines(cw_vec2 center, float radius, float width, int segments, cw_color c);
/* Simple polygon (no self-intersections), either winding, concave allowed:
 * ear-clipping triangulation, O(n^2). */
void cw_r2d_polygon(const cw_vec2 *pts, int n, cw_color c);
void cw_r2d_polygon_lines(const cw_vec2 *pts, int n, float width, cw_color c); /* closed cw_r2d_polyline */
/* Batched shapes: one submission each however many segments (HUD panels,
 * gauges, badges cost one call, not one per triangle).
 * Rounded rect: radius clamps to half the shorter side; `segments` per
 * corner (<= 0 picks by radius). _lines strokes `width` inside the edge. */
void cw_r2d_round_rect(cw_rect r, float radius, int segments, cw_color c);
void cw_r2d_round_rect_lines(cw_rect r, float radius, float width, int segments, cw_color c); /* the outline, width inside the edge */
/* Ring segment between radii r0 and r1 (r0 0 = a pie slice) from angle a0 to
 * a1 (radians, clockwise on screen, either direction); `segments` over the
 * sweep (<= 0 picks by radius and sweep). Two trig calls whatever the count. */
void cw_r2d_arc(cw_vec2 center, float r0, float r1, float a0, float a1, int segments, cw_color c);
/* Triangle fan: center joined to each edge pts[i]..pts[i+1], and closed
 * joins the last back to the first. Any outline that is star-shaped about
 * center (stars, torn panels, badges); no triangulation. */
void cw_r2d_fan(cw_vec2 center, const cw_vec2 *pts, int n, bool closed, cw_color c);
/* Triangulate a simple polygon into out_idx (3*(n-2) entries); returns the
 * index count, 0 if degenerate. For tools and tests. */
int cw_triangulate(const cw_vec2 *pts, int n, uint32_t *out_idx);

/* ── arbitrary geometry ─────────────────────────────────────────────────
 * uv is 0..1 over the texture's whole rect (past 1 only on repeat
 * textures). A null texture draws vertex colours. */
typedef struct cw_vertex2d { cw_vec2 pos, uv; cw_color color; } cw_vertex2d;
/* Vertices and indices are copied (caller keeps them); asserts on an index >= nv. Batches like sprites. */
void cw_r2d_triangles(cw_texture tex, const cw_vertex2d *v, int nv, const uint32_t *idx, int ni);
/* One quad: corners in order (any shape), with their UVs. */
void cw_r2d_quad(cw_texture tex, const cw_vec2 pos[4], const cw_vec2 uv[4], cw_color c);

/* A trail: a ribbon along points[0] (the head) .. points[count-1] (the
 * tail), its width and colour going from the head's to the tail's (a fading
 * comet, a sword swipe, a motion smear behind a dash, tyre marks). The
 * texture's u runs along it (0 at the head, 1 at the tail; or one repeat
 * every uv_repeat layer units on a repeat texture), v across it. Mitre-free:
 * each point's edge is square to its neighbours' chord, so keep the points
 * a few pixels apart. One submission, batched like sprites; additive blend
 * (cw_r2d_set_blend) for glowing trails, cw_r2d_set_intensity to bloom. */
typedef struct cw_r2d_trail_desc {
  const cw_vec2 *points;    /* layer space, head first */
  int count;                /* 2 or more */
  float head_width, tail_width;
  cw_color head_color;      /* {0,0,0,0} = white */
  cw_color tail_color;      /* zero = transparent black: the usual fade */
  cw_texture texture;       /* null = the colours alone */
  float uv_repeat;          /* layer units per texture repeat; 0 = u 0..1 over the whole length */
} cw_r2d_trail_desc;
void cw_r2d_trail(const cw_r2d_trail_desc *d); /* one ribbon along d->points (fewer than 2 points draw nothing) */

/* ── text ────────────────────────────────────────────────────────────────
 * Fonts come from TTF/OTF via stb_truetype. Two modes:
 *   SDF:    single-channel signed distance fields (not multi-channel MSDF:
 *           corners round slightly at very large sizes; one channel is what
 *           stb_truetype generates). One bake serves every size; glyphs are
 *           baked on first use into the linear atlas.
 *   BITMAP: coverage bitmaps at one pixel size into the nearest atlas, for
 *           pixel fonts (optionally thresholded to 1-bit); draw at integer
 *           multiples of that size.
 * Layout is left-to-right with kerning (kern table and GPOS pair
 * adjustments), '\n' breaks, greedy word wrap at spaces and between CJK
 * ideographs/kana (with the common kinsoku rules: no break before closing
 * punctuation or small kana; a word longer than the wrap width breaks between
 * characters), and left/centre/right alignment. Measurement runs the same
 * layout code as drawing, so the UI can rely on it to the pixel.
 *
 * Fallback fonts: a codepoint the font lacks is taken from its own fallback
 * chain (cw_font_set_fallbacks), then the global chain
 * (cw_font_set_global_fallbacks; the l10n module sets it per locale), baked
 * at the same em size; no font having it draws the font's .notdef and counts
 * in the stats' glyphs_missing.
 *
 * Complex scripts: text containing right-to-left or complex-shaping
 * codepoints (Hebrew, Arabic, Syriac, Thaana, the Indic scripts, Thai, Lao,
 * Tibetan, Myanmar, Khmer, combining marks) is shaped by kb_text_shape (zlib
 * license, deps/kb): OpenType GSUB/GPOS (joining forms, ligatures, mark
 * placement), bidi runs (no explicit embedding/isolate controls) reordered
 * per line, the same fallback chain. Lines break in logical order and are
 * then reordered, as UAX #9 does. Pure LTR text never takes this path. */
CW_HANDLE_TYPE(cw_font);

typedef enum { CW_FONT_SDF, CW_FONT_BITMAP } cw_font_mode;
typedef struct cw_font_desc {
  cw_font_mode mode;
  float bake_size;     /* SDF: em size glyphs are baked at (0 = 48); BITMAP: the pixel size (0 = 16) */
  int sdf_padding;     /* SDF: distance range in baked pixels (0 = 6) */
  bool threshold;      /* BITMAP: 1-bit glyphs (crisp pixel fonts) */
} cw_font_desc;

/* Loads (and keeps) the font file from the asset bundle. desc NULL = SDF. */
cw_font cw_font_load(const char *asset_path, const cw_font_desc *desc);
/* bytes are copied; null handle (logged) when not a readable TTF/OTF */
cw_font cw_font_from_memory(const void *ttf, size_t len, const cw_font_desc *desc);
void cw_font_destroy(cw_font f); /* fallback chains naming it drop it; null or stale handle is a no-op */

typedef enum { CW_ALIGN_LEFT, CW_ALIGN_CENTER, CW_ALIGN_RIGHT } cw_text_align;
/* Paragraph direction for bidi text (complex path only): AUTO takes the
 * first strong character's direction. */
typedef enum { CW_TEXT_DIR_AUTO, CW_TEXT_DIR_LTR, CW_TEXT_DIR_RTL } cw_text_dir;
typedef struct cw_text_style {
  float size;           /* pixel size of the em (0 = the font's bake size) */
  cw_color color;       /* {0,0,0,0} = white */
  float wrap_width;     /* 0 = no wrapping */
  cw_text_align align;  /* within wrap_width, or within the widest line when not wrapping */
  float line_spacing;   /* multiple of the font's line height (0 = 1) */
  float letter_spacing; /* extra pixels after every glyph */
  int32_t direction;    /* cw_text_dir (complex-script text only) */
  /* Outline and drop shadow, drawn under the text from the same layout (one
   * submission, batched with it). SDF fonts grow and soften the glyphs in
   * the shader: a round outline of any width up to the font's distance range
   * (sdf_padding baked px at bake_size: 6 at 48 = 12.5% of the size by
   * default; a wider one clamps with a warning naming the padding to load the
   * font with). Bitmap fonts stamp the glyphs at every whole-pixel offset
   * within the width (softness ignored). Widths in px at this size. */
  float outline_width;      /* 0 = none */
  cw_color outline_color;   /* {0,0,0,0} = opaque black */
  cw_color shadow_color;    /* alpha 0 = no shadow */
  cw_vec2 shadow_offset;    /* px */
  float shadow_spread;      /* px the shadow grows round the glyphs */
  float shadow_softness;    /* px of blur at the shadow's edge (SDF) */
  /* Art, not reading: a title logo drawn as text, a word on a sign, a
   * decorative background word. Drawn at exactly `size` (the text scale
   * leaves it alone) and left out of the size check (Text size, below). */
  bool fixed_size;
  /* The game draws its own panel, outline or shadow behind this text: the
   * player's Text background setting adds nothing to it. (Text with an
   * outline or a shadow in this style is left alone anyway.) */
  bool own_backdrop;
} cw_text_style;

typedef struct cw_text_metrics {
  float width, height;  /* box of the laid-out text (height = lines * line height) */
  int lines;
  float ascent, descent, line_height; /* at this size; descent is negative */
} cw_text_metrics;

/* Prepare glyphs during init/warmup, on the main rendering thread. These
 * populate the same font/atlas caches as drawing; they submit no draw commands.
 * Repeated calls reuse cached glyphs. Work and atlas memory move to loading;
 * preload only the repertoire needed by the game/current locale.
 * SDF caches serve every display size. Bitmap caches use the font's bake size.
 * Call again after changing fallback fonts/locales. Null/stale fonts are no-ops.
 * Range is inclusive, limited to Unicode scalar values (surrogates skipped).
 * Reversed ranges or first > U+10FFFF are no-ops; last is clamped to U+10FFFF.
 * English UI: cw_font_preload_range(font, 32, 126).
 * Use text for localized strings: it also prepares contextual shaped glyphs;
 * a range alone cannot anticipate all ligatures/contextual forms. NULL text
 * is a no-op. Layout at draw time still honors the requested style. */
void cw_font_preload_range(cw_font f, uint32_t first, uint32_t last);
/* Prepare a UTF-8 string, including fallback and contextual shaped glyphs.
 * Same loading/thread/cache rules as cw_font_preload_range; NULL is a no-op. */
void cw_font_preload_text(cw_font f, const char *utf8);

/* Measure without drawing. O(length). */
cw_text_metrics cw_font_measure(cw_font f, const char *utf8, const cw_text_style *style);
/* Advance of one codepoint (no kerning) at size px. */
float cw_font_advance(cw_font f, uint32_t codepoint, float size);
/* x of the caret before byte offset `byte` on a single unwrapped line (for
 * text fields); byte past the end = full width. */
float cw_font_caret_x(cw_font f, const char *utf8, int byte, float size);
/* Fallback chain of f (copied; replaces the old one; n 0 clears). Fonts in a
 * chain must outlive their use in it (a destroyed one is skipped). */
void cw_font_set_fallbacks(cw_font f, const cw_font *chain, int n);
int cw_font_fallbacks(cw_font f, cw_font *out, int cap); /* returns the chain length */
/* Fallbacks every font tries after its own chain. */
void cw_font_set_global_fallbacks(const cw_font *chain, int n);
bool cw_font_has_glyph(cw_font f, uint32_t codepoint); /* this font alone */
bool cw_font_covers(cw_font f, uint32_t codepoint);    /* this font or one of its fallbacks */
/* The text takes the complex-script path (RTL or shaping codepoints). */
bool cw_text_is_complex(const char *utf8);
/* Draw with the box's top-left at pos (layer space; the first baseline sits
 * at pos.y + ascent). Returns the metrics. */
cw_text_metrics cw_r2d_text(cw_font f, const char *utf8, cw_vec2 pos, const cw_text_style *style);

/* ── text size: the player's scale, the floor, the check ───────────────
 * One scale for every piece of 2D text: cw_r2d_text, cw_font_measure,
 * cw_font_advance, cw_font_caret_x, the prompts (cartwheel/prompt.h) and
 * every UI theme font (cw_ui_set_text_scale is this scale). Measuring scales
 * the same way as drawing, so a layout made from cw_font_measure grows with
 * the text and needs no code. The player picks 100%, 125%, 150% or 200%
 * (the game shell's Settings has the row; cw_text_scale_cycle for a game's
 * own menu).
 *
 * What scales: the em size, letter spacing, outline and shadow widths and
 * the shadow offset. wrap_width is a box and stays. Two limits:
 *   - text the scale would take past CW_TEXT_SCALE_CAP of the canvas height
 *     stops there (it was large already: a banner or a countdown); it never
 *     shrinks below the size the game asked for. The cap is in canvas pixels,
 *     after the 2D transform and the layer camera's zoom, so text on a game's
 *     own stage (a fixed-size virtual canvas scaled to the window by
 *     cw_r2d_set_transform) reaches the same cap as text drawn unscaled;
 *   - a bitmap font scales by whole multiples of its pixel size (no blur).
 * Text drawn into a render target (cw_r2d_begin with .target: a texture, a
 * sign in the scene) is drawn as asked, and so is fixed_size text (art).
 *
 * The floor, as a share of the 2D canvas height (the virtual canvas when the
 * game draws into one), measured on the em size the game asks for at 100%:
 *   CW_TEXT_FLOOR_BODY  body text (HUD values, menus, hints, captions):
 *                       26 px at 1080 lines (2.4%), 17.3 at 720, 19.3 at 800
 *   CW_TEXT_FLOOR_MIN   the smallest text anywhere: 18 px at 1080 lines (1.7%),
 *                       12 at 720, 13.3 at 800
 * The same visual angle covers a television at 3 m and a 5.5" handheld at
 * 30 cm, so one rule fits both.
 *
 * The check: every frame the engine records the smallest text drawn to the
 * window (fixed_size left out), with the transform and the layer camera's
 * zoom applied, as a share of the canvas height. Debug fields:
 *   text.min_share        the smallest this frame (0: no text)
 *   text.min_where        its first characters
 *   text.run_min_share    the smallest since the run began (0: none yet)
 *   text.run_min_where    its first characters
 *   text.scale, text.backdrop  the settings
 * The debug request `text` lists every size drawn this frame (the caller's
 * file and line too, in a debug build of the game); `text scale <k>` and
 * `text backdrop off|outline|panel` set the settings for a test. tests/gate.mjs
 * fails a game's run whose text.run_min_share is under CW_TEXT_FLOOR_MIN
 * (gate option textFloor); tools/check-text.mjs walks a game's screens and
 * holds body text to CW_TEXT_FLOOR_BODY. Render-only: nothing here reaches
 * the simulation. */
#define CW_TEXT_FLOOR_BODY (26.0f / 1080.0f)
#define CW_TEXT_FLOOR_MIN (18.0f / 1080.0f)
#define CW_TEXT_SCALE_CAP 0.08f
/* The player's text scale (clamped 0.5..4; 1 = the sizes the game asks for). */
void cw_text_scale_set(float scale);
float cw_text_scale(void); /* the current scale (1 when unset) */
/* The steps a settings row offers: 100%, 125%, 150%, 200%. cycle goes to the
 * next (200% wraps to 100%), cycle_back to the previous; label is "125%". */
void cw_text_scale_cycle(void);
void cw_text_scale_cycle_back(void); /* the previous step (100% wraps to 200%) */
const char *cw_text_scale_label(void); /* the nearest step: "100%", "125%", "150%" or "200%" */
/* The size text asked for at `size` draws at now in font f (the scale, the
 * cap and a bitmap font's whole multiples applied): for a layout that spaces
 * lines by the size instead of measuring. A null font skips the bitmap rule. */
float cw_text_drawn_size(cw_font f, float size);

/* Text background, the player's contrast setting for text over a 3D scene:
 * an outline round the letters, or a dark rounded panel behind the text's
 * box. The engine adds it to text whose style has no outline and no shadow
 * and is not own_backdrop or fixed_size; the UI's controls and the prompts
 * draw their own. */
typedef enum { CW_TEXT_BACKDROP_OFF, CW_TEXT_BACKDROP_OUTLINE, CW_TEXT_BACKDROP_PANEL } cw_text_backdrop;
void cw_text_backdrop_set(cw_text_backdrop b); /* Off (the default), Outline or Panel */
cw_text_backdrop cw_text_backdrop_get(void); /* the current setting */
void cw_text_backdrop_cycle(void);         /* off, outline, panel, off */
const char *cw_text_backdrop_label(void);  /* "Off", "Outline", "Panel" */
/* Both settings in one byte for a game's save (0: nothing chosen, the
 * defaults), and back: cw_text_scale_restore at init, after the save loaded.
 * The game shell keeps it in its own save. */
uint8_t cw_text_scale_saved(void);
void cw_text_scale_restore(uint8_t saved); /* apply a cw_text_scale_saved byte (0 or a byte from elsewhere: nothing changes) */

/* debug builds of a game: the caller's file and line for the `text` request */
void cw__text_caller(const char *file, int line);
#if !defined(NDEBUG) && !defined(CW_TEXT_NO_CALLER)
#define cw_r2d_text(f, s, p, st) (cw__text_caller(__FILE__, __LINE__), cw_r2d_text(f, s, p, st))
#endif

/* ── tilemaps ────────────────────────────────────────────────────────────
 * Grid maps in chunks of CW_TILE_CHUNK x CW_TILE_CHUNK tiles per layer; each
 * chunk is static geometry in its own buffer, rebuilt only when one of its
 * cells is edited. Drawing culls chunks to the layer's view; each map layer
 * is one draw per visible chunk, ordered with everything else by the canvas
 * layer and z it was submitted at. Animated tiles cycle in the vertex shader
 * from the pass time: no rebuilds. */
#define CW_TILE_CHUNK 32
CW_HANDLE_TYPE(cw_tilemap);

/* Cell value: tile id + 1 in the low 24 bits (0 = empty), transform flags in
 * the high bits (Tiled's convention, so Tiled maps load unchanged). */
#define CW_TILE_FLIP_X 0x80000000u
#define CW_TILE_FLIP_Y 0x40000000u
#define CW_TILE_FLIP_D 0x20000000u /* swap x/y (transpose) */
#define CW_TILE_ROT90 (CW_TILE_FLIP_D | CW_TILE_FLIP_X)  /* clockwise */
#define CW_TILE_ROT180 (CW_TILE_FLIP_X | CW_TILE_FLIP_Y)
#define CW_TILE_ROT270 (CW_TILE_FLIP_D | CW_TILE_FLIP_Y)
#define CW_TILE_ID_MASK 0x00ffffffu
#define CW_TILE(id) ((uint32_t)(id) + 1u)

typedef struct cw_tilemap_desc {
  int width, height;      /* tiles */
  int layers;             /* 1..16 */
  cw_texture tileset;     /* an image texture (not a render target) */
  int tile_w, tile_h;     /* tileset cell size, texture pixels */
  int margin, spacing;    /* tileset border / gap between cells, pixels */
  int columns;            /* tiles per tileset row; 0 = from the texture width */
  float cell_w, cell_h;   /* world size of one tile; 0 = tile_w/tile_h */
} cw_tilemap_desc;

/* Cells start empty. Null handle (logged) unless the tileset is a loaded image (not a target), sizes are
 * positive and layers is 1..16. */
cw_tilemap cw_tilemap_create(const cw_tilemap_desc *desc);
void cw_tilemap_destroy(cw_tilemap m); /* null or stale handle is a no-op */
void cw_tilemap_set(cw_tilemap m, int layer, int x, int y, uint32_t cell); /* out of range ignored */
uint32_t cw_tilemap_get(cw_tilemap m, int layer, int x, int y);           /* 0 out of range */
/* Fill a layer from width*height cells (row-major). */
void cw_tilemap_set_layer(cw_tilemap m, int layer, const uint32_t *cells);
/* Tile `id` animates through ids id..id+frames-1 at fps. Takes effect when
 * the chunks holding it rebuild (immediately for cells set afterwards). */
void cw_tilemap_set_animation(cw_tilemap m, int id, int frames, float fps);
/* Submit one map layer at `origin` (layer space) into the current canvas
 * layer, z and clip. Tint multiplies. */
void cw_r2d_tilemap(cw_tilemap m, int map_layer, cw_vec2 origin, cw_color tint);
/* World position -> cell coordinates (floor). */
void cw_tilemap_world_to_cell(cw_tilemap m, cw_vec2 origin, cw_vec2 world, int *x, int *y);

/* ── 2D lights ───────────────────────────────────────────────────────────
 * Layers marked lit are drawn into an offscreen colour buffer (and, when a
 * texture with a normal map was drawn, a normal buffer), then composited as
 * colour * light. The lights are tiled: the CPU sorts the run's lights into
 * 32-pixel screen tiles, and ONE full-screen pass gives each pixel the
 * ambient colour plus every light of its tile (radial falloff, an optional
 * spot cone, normal maps, specular), reading the pixel's normal and material
 * once and writing it once, however many lights overlap there.
 *
 * Shadows come from a distance field of the occluders: the occluder
 * outlines are filled into a mask and a jump flood turns the mask into the
 * distance to the nearest occluder at every pixel (about 12 full-screen
 * passes at the field's size, made once per lit run whatever the light
 * count). Each lit pixel then marches toward its light through the field
 * (sphere tracing: big steps in open space), which gives hard or soft
 * shadows (cw_light2d.softness) at a cost that does not grow with the
 * number of occluder edges. An occluder's own pixels stay lit (its face),
 * as before. Field size and march steps follow the effects detail
 * (cartwheel/quality.h): Ultra the canvas size and 48 steps, High and
 * Medium half the canvas and 32 / 24 steps, Low and Very Low a quarter and
 * 16 / 12 steps (never under 320 pixels across).
 *
 * The light buffer is RGBA8 storing light/2 (lights overbright up to 2x);
 * in a pass with .post it is RGBA16F holding the light itself, so lights
 * go as bright as asked and bloom.
 *
 * Lights and occluders are submitted into the current layer (its camera
 * places them) and affect the run of consecutive lit layers they are in. */
typedef struct cw_light2d {
  cw_vec2 pos;          /* layer space */
  float radius;         /* layer units; light is 0 at the radius */
  cw_color color;       /* {0,0,0,0} = white */
  float energy;         /* 0 = 1 */
  float falloff;        /* exponent of (1 - d/radius); 0 = 2 */
  float height;         /* above the plane, layer units, for normal maps; 0 = radius/4 (directional: the
                         * sun's elevation 0..1 above the plane, 0 = 0.5) */
  bool shadows;         /* cast shadows from occluders */
  /* shadow penumbra: 0 = hard edges, 1 = very soft. The penumbra widens with
   * the distance from the occluder, as a light of some size makes it */
  float softness;
  float angle;          /* spot and directional lights: where the light points, radians clockwise from +x */
  float cone;           /* spot: half-angle of the beam, radians (0 = all round) */
  float cone_soft;      /* spot: share of the cone's edge that fades (0 = 0.2) */
  /* a sun or moon: parallel light from `angle` over the whole layer (pos,
   * radius and falloff unused; shadows reach across the canvas) */
  bool directional;
  /* light the air scatters toward the eye (0 = none): shafts where the
   * light passes between occluders, a glow round the lamp; added on top
   * of the lit layers, also over empty background */
  float volumetric;
} cw_light2d;
/* current layer must be lit (else warned and ignored); asserts outside a pass; uses the current transform */
void cw_r2d_light(const cw_light2d *l);
/* Occluder outline (layer space). closed joins the ends. */
void cw_r2d_occluder(const cw_vec2 *pts, int n, bool closed);

/* ── stats ───────────────────────────────────────────────────────────────
 * Totals for the current frame (reset by the first cw_r2d_begin of each
 * engine frame), complete once the frame's last pass ended. */
typedef struct cw_r2d_stats {
  uint32_t passes;
  uint32_t draw_calls;     /* every glDraw* the renderer issued */
  uint32_t batches;        /* draws of the sorted stream (sprites/shapes/text) */
  uint32_t vertices;       /* submitted to the stream */
  uint32_t indices;
  uint32_t commands;       /* sprites + shapes + text runs + tilemap layers */
  uint32_t sorted;         /* passes that needed the sort */
  uint32_t tile_chunks;    /* chunks drawn */
  uint32_t chunk_rebuilds;
  uint32_t lights;
  uint32_t atlas_pages;    /* pages across atlas arrays (all time) */
  uint32_t texture_uploads;
  /* complex-script runs laid out this frame, by every measure and draw:
   * shaped now or reused from the shape cache (cached text still counts) */
  uint32_t shaped_runs;
  uint32_t glyphs_missing; /* distinct codepoints no font in the chain had (all time) */
  uint32_t shape_cache_hits; /* complex layouts this frame served by the shape cache (no shaping) */
  /* WebGPU calls the renderer made this frame (0 on GL): passes, pipelines, bind groups,
   * draws, buffer writes. In a cart each is an import call: a weak host holds 60 fps
   * under about 8000 a frame for the whole game, so this is the 2D share of that */
  uint32_t gpu_calls;
  uint32_t gi_cascades;    /* radiance cascades the last lit run with .gi drew (0: none) */
  uint32_t particles;      /* GPU particles drawn (cw_r2d_particles) */
} cw_r2d_stats;
const cw_r2d_stats *cw_r2d_get_stats(void); /* renderer-owned, updated as passes run; never NULL */
/* Register every stats field as a debug field "<prefix>.<name>" (u32). gpu_calls is left out: it
 * is 0 on GL by nature, and a gate compares GL and WebGPU states. */
void cw_r2d_debug_register(const char *prefix);

#ifdef __cplusplus
}
#endif
#endif
